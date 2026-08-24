// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/text_materializer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "flutter/fml/logging.h"
#include "impeller/propeller/buffer_arena.h"
#include "impeller/typographer/text_frame.h"
#include "impeller/typographer/text_run.h"

namespace impeller {

GlyphRasterizer::~GlyphRasterizer() = default;

TextMaterializer::TextMaterializer(std::shared_ptr<PagedAtlas> atlas,
                                   std::shared_ptr<GlyphRasterizer> rasterizer)
    : atlas_(std::move(atlas)), rasterizer_(std::move(rasterizer)) {}

TextMaterializer::~TextMaterializer() = default;

const TextMaterializer::AtlasEntry* TextMaterializer::ResolveGlyph(
    const Font& font,
    Glyph glyph,
    Rational scale,
    uint8_t phase) {
  // Prototype: the font hash stands in for font identity in the key.
  const GlyphKey key = {
      .font_hash = font.GetHash(),
      .glyph_index = glyph.index,
      .glyph_type = static_cast<uint8_t>(glyph.type),
      .phase = phase,
      .scale = scale,
  };
  auto found = entries_.find(key);
  if (found != entries_.end()) {
    found->second.last_used_frame = frame_number_;
    return &found->second;
  }

  // One bitmap at the exact quarter-pixel offset it will display at. Only
  // phases actually used rasterize; each caches independently.
  RasterizedGlyph rasterized =
      rasterizer_->Rasterize(font, glyph, scale, phase * 0.25f);

  AtlasEntry entry;
  entry.bearing = rasterized.bearing;
  if (rasterized.width <= 0 || rasterized.height <= 0) {
    // Empty glyph (e.g. whitespace); cached as a degenerate entry.
    entry.last_used_frame = frame_number_;
    return &entries_.emplace(key, entry).first->second;
  }

  std::optional<PagedAtlas::Placement> placement =
      atlas_->PlaceEntry(rasterized.width, rasterized.height);
  if (!placement.has_value()) {
    // Atlas full. Do not cache, so a later compaction can retry.
    FML_LOG(WARNING) << "Propeller glyph atlas full; dropping glyph.";
    return nullptr;
  }
  atlas_->WriteEntry(*placement, rasterized.coverage.data());
  entry.placement = *placement;
  entry.last_used_frame = frame_number_;
  return &entries_.emplace(key, entry).first->second;
}

Scalar TextMaterializer::GetLiveFraction(uint64_t stale_after) const {
  // Area, not entry count: one big glyph is worth many small ones, and it
  // is area the packer ran out of.
  Scalar placed = 0;
  Scalar live = 0;
  const uint64_t cutoff =
      frame_number_ > stale_after ? frame_number_ - stale_after : 0;
  auto measure = [&](const PagedAtlas::Placement& placement,
                     uint64_t last_used) {
    const Scalar area = static_cast<Scalar>(placement.width) *
                        static_cast<Scalar>(placement.height);
    placed += area;
    if (last_used >= cutoff) {
      live += area;
    }
  };
  for (const auto& [key, entry] : entries_) {
    measure(entry.placement, entry.last_used_frame);
  }
  return placed > 0 ? live / placed : 1.0f;
}

std::vector<PagedAtlas::RetiredPage> TextMaterializer::CompactIfStale(
    uint64_t stale_after,
    Scalar threshold) {
  if (atlas_->GetPageCount() <= 1) {
    return {};  // Nothing to win: one page cannot be consolidated.
  }
  if (GetLiveFraction(stale_after) >= threshold) {
    return {};
  }
  std::vector<PagedAtlas::RetiredPage> retired = atlas_->Compact();
  entries_.clear();
  entries_generation_ = atlas_->GetGeneration();
  return retired;
}

uint32_t TextMaterializer::ResolveGlyphs(const TextFrame& frame,
                                         Point origin,
                                         const Matrix& composed,
                                         uint32_t color,
                                         uint32_t paint,
                                         Point* position_out,
                                         Attributes* attributes_out) {
  const Rational scale_key =
      TextFrame::RoundScaledFontSize(composed.GetMaxBasisLengthXY());
  const uint64_t generation = atlas_->GetGeneration();
  if (entries_generation_ != generation) {
    entries_.clear();
    entries_generation_ = generation;
  }

  const Scalar scale = static_cast<Scalar>(scale_key);
  const Scalar page_width = atlas_->GetPageWidth();
  const Scalar page_height = atlas_->GetPageHeight();
  const bool axis_aligned = composed.IsAligned2D();
  const Scalar inverse = 1.0f / std::max(scale, 1e-6f);
  Vector2 translation = composed.GetTranslation();

  uint32_t glyphs = 0;
  for (const TextRun& run : frame.GetRuns()) {
    const Font& font = run.GetFont();
    for (const TextRun::GlyphPosition& glyph_position :
         run.GetGlyphPositions()) {
      const Point scaled = (origin + glyph_position.position) * scale;
      Point local_origin;
      uint8_t phase = 0;
      if (axis_aligned) {
        const Scalar device_x = scaled.x + translation.x;
        const Scalar device_y = scaled.y + translation.y;
        Scalar snapped_x = std::floor(device_x);
        int quarter =
            static_cast<int>(std::lround((device_x - snapped_x) * 4.0f));
        if (quarter == 4) {
          snapped_x += 1;
          quarter = 0;
        }
        phase = static_cast<uint8_t>(quarter);
        local_origin = Point(snapped_x - translation.x,
                             std::floor(device_y + 0.5f) - translation.y);
      } else {
        local_origin = scaled;
      }

      const AtlasEntry* entry =
          ResolveGlyph(font, glyph_position.glyph, scale_key, phase);

      // Degenerate for whitespace and for a glyph the atlas is too full
      // to take, which keeps quads and glyphs one to one.
      Rect position;
      Rect uv;
      if (entry != nullptr && entry->placement.width > 0) {
        const Point placed = (local_origin + entry->bearing) * inverse;
        position = Rect::MakeXYWH(
            placed.x, placed.y,
            static_cast<Scalar>(entry->placement.width) * inverse,
            static_cast<Scalar>(entry->placement.height) * inverse);
        uv = Rect::MakeLTRB(
            entry->placement.x / page_width, entry->placement.y / page_height,
            (entry->placement.x + entry->placement.width) / page_width,
            (entry->placement.y + entry->placement.height) / page_height);
      }

      const std::array<Point, 4> corners = position.GetPoints();
      const std::array<Point, 4> uvs = uv.GetPoints();
      for (int corner = 0; corner < 4; corner++) {
        position_out[corner] = corners[corner];
        attributes_out[corner] = Attributes{
            .uv = uvs[corner],
            .color = color,
            .paint = paint,
        };
      }
      position_out += 4;
      attributes_out += 4;
      glyphs++;
    }
  }
  return glyphs;
}

void TextMaterializer::FlushAtlas(GpuCommandBuffer& cmd_buffer) {
  if (atlas_->HasPendingUploads()) {
    atlas_->RecordUploads(cmd_buffer);
  }
}

}  // namespace impeller
