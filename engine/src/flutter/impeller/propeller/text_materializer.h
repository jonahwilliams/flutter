// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_TEXT_MATERIALIZER_H_
#define FLUTTER_IMPELLER_PROPELLER_TEXT_MATERIALIZER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "impeller/geometry/point.h"
#include "impeller/geometry/rational.h"
#include "impeller/propeller/paged_atlas.h"
#include "impeller/typographer/font.h"
#include "impeller/typographer/glyph.h"
#include "impeller/typographer/text_frame.h"

namespace impeller {

struct Attributes;

/// A glyph's coverage bitmap rasterized at a specific (rounded) scale.
struct RasterizedGlyph {
  /// Bitmap dimensions in device pixels. 0x0 is valid (e.g. whitespace).
  int32_t width = 0;
  int32_t height = 0;
  /// Offset from the glyph origin to the bitmap's top-left, in device pixels.
  Point bearing;
  /// width * height coverage bytes (R8).
  std::vector<uint8_t> coverage;
};

/// Produces coverage bitmaps.
class GlyphRasterizer {
 public:
  virtual ~GlyphRasterizer();

  /// Rasterize at the given rounded scale, with the outline shifted right by
  /// `subpixel_offset` device pixels (0, 0.25, 0.5, or 0.75).
  virtual RasterizedGlyph Rasterize(const Font& font,
                                    Glyph glyph,
                                    Rational scale,
                                    Scalar subpixel_offset) = 0;
};

//------------------------------------------------------------------------------
/// Materializes text recipes: resolves glyphs against the paged atlas,
/// rasterizing and uploading missing ones, and emits glyph quads.
class TextMaterializer {
 public:
  TextMaterializer(std::shared_ptr<PagedAtlas> atlas,
                   std::shared_ptr<GlyphRasterizer> rasterizer);

  ~TextMaterializer();

  TextMaterializer(const TextMaterializer&) = delete;
  TextMaterializer& operator=(const TextMaterializer&) = delete;

  /// Resolve `frame` at `origin` under `composed`.
  uint32_t ResolveGlyphs(const TextFrame& frame,
                         Point origin,
                         const Matrix& composed,
                         uint32_t color,
                         uint32_t paint,
                         Point* position_out,
                         Attributes* attributes_out);

  /// The atlas page glyph coverage is read from.
  /// TODO: one page, which is what a glyph paint's texture slot names.
  GPUTexture* GetAtlasTexture() const { return atlas_->GetPageTexture(0); }

  /// Upload what resolving queued.
  void FlushAtlas(GpuCommandBuffer& cmd_buffer);

  /// Start of a frame's materialization. Entries resolved from here on are
  /// stamped with `frame_number`, which is what CompactIfIdle measures.
  void BeginFrame(uint64_t frame_number) { frame_number_ = frame_number; }

  /// The fraction of placed atlas area whose entries were resolved within
  /// the last `stale_after` frames.
  Scalar GetLiveFraction(uint64_t stale_after) const;

  /// Drop every entry not resolved within `stale_after` frames and compact
  /// the atlas, if the live fraction has fallen below `threshold`.
  std::vector<PagedAtlas::RetiredPage> CompactIfStale(uint64_t stale_after,
                                                      Scalar threshold);

 private:
  struct GlyphKey {
    std::size_t font_hash = 0;
    uint16_t glyph_index = 0;
    uint8_t glyph_type = 0;
    /// Quarter-pixel X phase (0-3) the bitmap was rasterized at.
    uint8_t phase = 0;
    Rational scale = Rational(0);

    bool operator==(const GlyphKey& o) const {
      return font_hash == o.font_hash && glyph_index == o.glyph_index &&
             glyph_type == o.glyph_type && phase == o.phase && scale == o.scale;
    }
  };

  struct GlyphKeyHash {
    std::size_t operator()(const GlyphKey& key) const {
      return key.font_hash ^ (static_cast<std::size_t>(key.glyph_index) << 1) ^
             (static_cast<std::size_t>(key.glyph_type) << 17) ^
             (static_cast<std::size_t>(key.phase) << 19) ^ key.scale.GetHash();
    }
  };

  struct AtlasEntry {
    /// One bitmap, rasterized at the key's quarter-pixel phase.
    PagedAtlas::Placement placement;
    /// Bearing of that bitmap (device px, y-down, integer).
    Point bearing;
    /// Frame this entry was last resolved for. What is NOT here has gone
    /// unused, which is what makes compaction worth doing.
    uint64_t last_used_frame = 0;
  };

  /// Look up or rasterize-and-place a glyph at a quarter-pixel phase.
  /// Returns nullptr only when the atlas is full.
  const AtlasEntry* ResolveGlyph(const Font& font,
                                 Glyph glyph,
                                 Rational scale,
                                 uint8_t phase);

  std::shared_ptr<PagedAtlas> atlas_;
  std::shared_ptr<GlyphRasterizer> rasterizer_;
  /// Resolved glyphs for the current atlas generation. Cleared wholesale when
  /// the generation changes: compaction honestly invalidates everything.
  std::unordered_map<GlyphKey, AtlasEntry, GlyphKeyHash> entries_;
  uint64_t entries_generation_ = 0;
  uint64_t frame_number_ = 0;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_TEXT_MATERIALIZER_H_
