// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/paged_atlas.h"

#include <utility>

#include "flutter/fml/logging.h"

namespace impeller {

PagedAtlas::PagedAtlas(GPUContext* context,
                       TextureFormat format,
                       int32_t page_width,
                       int32_t page_height,
                       size_t max_pages)
    : context_(context),
      format_(format),
      page_width_(page_width),
      page_height_(page_height),
      max_pages_(max_pages) {}

PagedAtlas::~PagedAtlas() = default;

uint64_t PagedAtlas::GetGeneration() const {
  return generation_;
}

size_t PagedAtlas::GetPageCount() const {
  return pages_.size();
}

uint64_t PagedAtlas::GetRevision() const {
  return revision_;
}

GPUTexture* PagedAtlas::GetPageTexture(size_t page) const {
  if (page >= pages_.size()) {
    return nullptr;
  }
  return pages_[page].texture.get();
}

std::optional<PagedAtlas::Placement> PagedAtlas::PlaceEntry(int32_t width,
                                                            int32_t height) {
  // 1px guard border on each side.
  const int32_t padded_width = width + 2;
  const int32_t padded_height = height + 2;
  FML_DCHECK(width > 0 && height > 0);
  // Bigger than a page: no amount of eviction makes room.
  if (padded_width > page_width_ || padded_height > page_height_) {
    return std::nullopt;
  }

  for (size_t i = 0; i < pages_.size(); i++) {
    IPoint16 location;
    if (pages_[i].packer->AddRect(padded_width, padded_height, &location)) {
      return Placement{
          .page = i,
          .x = location.x() + 1,
          .y = location.y() + 1,
          .width = width,
          .height = height,
      };
    }
  }

  if (pages_.size() >= max_pages_ || !AppendPage()) {
    return std::nullopt;
  }

  IPoint16 location;
  if (!pages_.back().packer->AddRect(padded_width, padded_height, &location)) {
    return std::nullopt;
  }
  return Placement{
      .page = pages_.size() - 1,
      .x = location.x() + 1,
      .y = location.y() + 1,
      .width = width,
      .height = height,
  };
}

bool PagedAtlas::AppendPage() {
  Page page;
  page.texture = context_->CreateTexture(
      TextureDesc{
          .format = format_,
          .width = static_cast<uint32_t>(page_width_),
          .height = static_cast<uint32_t>(page_height_),
      },
      /*zeroed=*/true);
  page.staging = context_->CreateBuffer(
      nullptr,
      static_cast<size_t>(page_width_) * page_height_ * BytesPerPixel(format_));
  if (!page.texture || !page.staging) {
    return false;
  }
  page.packer = RectanglePacker::Factory(page_width_, page_height_);
  pages_.emplace_back(std::move(page));
  revision_++;
  return true;
}

void PagedAtlas::WriteEntry(const Placement& placement, const uint8_t* data) {
  FML_DCHECK(placement.page < pages_.size() &&
             placement.x + placement.width <= page_width_ &&
             placement.y + placement.height <= page_height_);
  const size_t bytes_per_pixel = BytesPerPixel(format_);
  const size_t row_bytes = placement.width * bytes_per_pixel;
  const size_t page_row_bytes = page_width_ * bytes_per_pixel;
  GPUBuffer& staging = *pages_[placement.page].staging;
  for (int32_t row = 0; row < placement.height; row++) {
    staging.Write(data + row * row_bytes, static_cast<uint32_t>(row_bytes),
                  static_cast<uint32_t>((placement.y + row) * page_row_bytes +
                                        placement.x * bytes_per_pixel));
  }
  dirty_.emplace_back(placement.page, placement.x, placement.y, placement.width,
                      placement.height);
  revision_++;
}

void PagedAtlas::RecordUploads(GpuCommandBuffer& command_buffer) {
  const size_t bytes_per_pixel = BytesPerPixel(format_);
  const size_t page_row_bytes = page_width_ * bytes_per_pixel;
  for (const DirtyRegion& region : dirty_) {
    const Page& page = pages_[region.page];
    command_buffer.UpdateRegion(
        *page.texture, region.x, region.y, region.width, region.height,
        *page.staging,
        static_cast<uint32_t>(region.y * page_row_bytes +
                              region.x * bytes_per_pixel),
        static_cast<uint32_t>(page_row_bytes));
  }
  dirty_.clear();
}

GPUBuffer* PagedAtlas::GetPageBuffer(size_t page) const {
  if (page >= pages_.size()) {
    return nullptr;
  }
  return pages_[page].staging.get();
}

Scalar PagedAtlas::GetUtilization(size_t page) const {
  if (page >= pages_.size()) {
    return 0;
  }
  return pages_[page].packer->PercentFull();
}

std::vector<PagedAtlas::RetiredPage> PagedAtlas::Compact() {
  std::vector<RetiredPage> retired;
  retired.reserve(pages_.size());
  for (Page& page : pages_) {
    retired.push_back({std::move(page.staging), std::move(page.texture)});
  }
  pages_.clear();
  dirty_.clear();
  generation_++;
  revision_++;
  return retired;
}

}  // namespace impeller
