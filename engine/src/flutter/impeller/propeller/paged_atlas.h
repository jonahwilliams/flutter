// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_PAGED_ATLAS_H_
#define FLUTTER_IMPELLER_PROPELLER_PAGED_ATLAS_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "impeller/geometry/scalar.h"
#include "impeller/propeller/renderer/gpu_context.h"
#include "impeller/typographer/rectangle_packer.h"

namespace impeller {

//------------------------------------------------------------------------------
/// Fixed-size-page texture atlas used by the glyph atlas.
///
/// Pages are allocated on demand up to `max_pages`.
class PagedAtlas {
 public:
  struct Placement {
    size_t page = 0;
    /// Interior rect in texels; the 1px guard border is excluded.
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
  };

  /// `context` allocates the page textures and must outlive the atlas.
  PagedAtlas(GPUContext* context,
             TextureFormat format,
             int32_t page_width,
             int32_t page_height,
             size_t max_pages);

  ~PagedAtlas();

  PagedAtlas(const PagedAtlas&) = delete;
  PagedAtlas& operator=(const PagedAtlas&) = delete;

  /// Incremented whenever entries move (compaction). All references into the
  /// atlas must be considered invalid once this changes.
  uint64_t GetGeneration() const;

  /// Incremented on every mutation, including in-place additions that do not
  /// bump the generation.
  uint64_t GetRevision() const;

  size_t GetPageCount() const;

  /// The texture backing `page`, or nullptr when there is no such page.
  ///
  /// Valid until the next Compact().
  GPUTexture* GetPageTexture(size_t page) const;

  int32_t GetPageWidth() const { return page_width_; }

  int32_t GetPageHeight() const { return page_height_; }

  /// Reserve space for a width x height entry.
  ///
  /// The returned placement is the interior rect. Returns nullopt when
  /// all pages are full. This placement manages internal padding to make
  /// bilinear filtering safe.
  std::optional<Placement> PlaceEntry(int32_t width, int32_t height);

  /// Copy tightly packed `data` into the placement's interior rect.
  void WriteEntry(const Placement& placement, const uint8_t* data);

  /// Whether anything has been written since the last RecordUploads().
  bool HasPendingUploads() const { return !dirty_.empty(); }

  /// Record the uploads for everything written since the last call.
  void RecordUploads(GpuCommandBuffer& command_buffer);

  /// The staging buffer holding the page's texels, or nullptr when there is
  /// no such page.
  GPUBuffer* GetPageBuffer(size_t page) const;

  Scalar GetUtilization(size_t page) const;

  struct RetiredPage {
    std::unique_ptr<GPUBuffer> staging;
    std::unique_ptr<GPUTexture> texture;
  };

  /// Drop all entries and bump the generation, returning the dropped pages.
  std::vector<RetiredPage> Compact();

 private:
  struct DirtyRegion {
    size_t page = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
  };

  struct Page {
    std::shared_ptr<RectanglePacker> packer;
    std::unique_ptr<GPUBuffer> staging;
    std::unique_ptr<GPUTexture> texture;
  };

  GPUContext* context_;
  const TextureFormat format_;
  const int32_t page_width_;
  const int32_t page_height_;
  const size_t max_pages_;

  bool AppendPage();

  std::vector<Page> pages_;
  std::vector<DirtyRegion> dirty_;
  uint64_t generation_ = 0;
  uint64_t revision_ = 0;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_PAGED_ATLAS_H_
