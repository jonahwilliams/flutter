// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/freetype_glyph_rasterizer.h"

#include <cmath>
#include <cstring>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "flutter/fml/logging.h"
#include "impeller/typographer/font.h"

namespace impeller {

FreeTypeLibrary::~FreeTypeLibrary() {
  if (library_ != nullptr) {
    FT_Done_FreeType(library_);
  }
}

FreeTypeTypeface::FreeTypeTypeface(std::shared_ptr<FreeTypeLibrary> library,
                                   FT_Face face,
                                   std::shared_ptr<fml::Mapping> mapping)
    : library_(std::move(library)), face_(face), mapping_(std::move(mapping)) {}

FreeTypeTypeface::~FreeTypeTypeface() {
  if (face_ != nullptr) {
    FT_Done_Face(face_);
  }
}

bool FreeTypeTypeface::IsValid() const {
  return face_ != nullptr;
}

std::size_t FreeTypeTypeface::GetHash() const {
  return reinterpret_cast<std::size_t>(face_);
}

bool FreeTypeTypeface::IsEqual(const Typeface& other) const {
  return GetHash() == other.GetHash();
}

std::shared_ptr<FreeTypeGlyphRasterizer> FreeTypeGlyphRasterizer::Make() {
  FT_Library library = nullptr;
  if (FT_Init_FreeType(&library) != 0) {
    FML_LOG(ERROR) << "Failed to initialize FreeType.";
    return nullptr;
  }
  return std::shared_ptr<FreeTypeGlyphRasterizer>(
      new FreeTypeGlyphRasterizer(std::make_shared<FreeTypeLibrary>(library)));
}

FreeTypeGlyphRasterizer::FreeTypeGlyphRasterizer(
    std::shared_ptr<FreeTypeLibrary> library)
    : library_(std::move(library)) {}

FreeTypeGlyphRasterizer::~FreeTypeGlyphRasterizer() = default;

std::shared_ptr<FreeTypeTypeface> FreeTypeGlyphRasterizer::CreateTypeface(
    std::shared_ptr<fml::Mapping> mapping,
    int face_index) {
  if (!mapping || mapping->GetSize() == 0) {
    return nullptr;
  }
  std::scoped_lock lock(mutex_);
  const FT_Byte* data = mapping->GetMapping();
  const auto size = static_cast<FT_Long>(mapping->GetSize());
  FT_Face face = nullptr;
  FT_Error error =
      FT_New_Memory_Face(library_->Get(), data, size, face_index, &face);
  if (error != 0 && face_index != 0) {
    // A non-zero index that FreeType rejects usually means the data is
    // not the collection the index was meant for -- Skia reports an index
    // per typeface, including for fonts it synthesized. The face at 0 is
    // the right one then, and is far better than dropping the font.
    const FT_Error retry =
        FT_New_Memory_Face(library_->Get(), data, size, 0, &face);
    if (retry == 0) {
      FML_LOG(WARNING) << "Propeller: typeface index " << face_index
                       << " rejected (FreeType error " << error
                       << "); loaded face 0 instead.";
      error = 0;
    }
  }
  if (error != 0) {
    // The sfnt tag says what the data actually is: 0x00010000 or 'true'
    // for TrueType, 'OTTO' for CFF, 'ttcf' for a collection, 'wOFF' and
    // 'wOF2' for web fonts (which FreeType may not be built to decode).
    char tag[5] = "????";
    if (mapping->GetSize() >= 4) {
      for (int i = 0; i < 4; i++) {
        const uint8_t byte = data[i];
        tag[i] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
      }
    }
    FML_LOG(ERROR) << "Propeller: failed to load typeface -- FreeType error "
                   << error << ", " << mapping->GetSize() << " bytes, index "
                   << face_index << ", tag '" << tag << "' (0x" << std::hex
                   << (mapping->GetSize() >= 4
                           ? (static_cast<uint32_t>(data[0]) << 24 |
                              static_cast<uint32_t>(data[1]) << 16 |
                              static_cast<uint32_t>(data[2]) << 8 |
                              static_cast<uint32_t>(data[3]))
                           : 0u)
                   << std::dec << ").";
    return nullptr;
  }
  auto typeface = std::shared_ptr<FreeTypeTypeface>(
      new FreeTypeTypeface(library_, face, std::move(mapping)));
  known_typefaces_.insert(typeface.get());
  return typeface;
}

std::optional<uint16_t> FreeTypeGlyphRasterizer::GetGlyphIndex(
    const FreeTypeTypeface& typeface,
    uint32_t codepoint) {
  std::scoped_lock lock(mutex_);
  const FT_UInt index = FT_Get_Char_Index(typeface.GetFace(), codepoint);
  if (index == 0 || index > 0xFFFF) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(index);
}

RasterizedGlyph FreeTypeGlyphRasterizer::Rasterize(const Font& font,
                                                   Glyph glyph,
                                                   Rational scale,
                                                   Scalar subpixel_offset) {
  RasterizedGlyph result;

  const Typeface* raw = font.GetTypeface().get();
  const FreeTypeTypeface* typeface = nullptr;
  std::shared_ptr<FreeTypeTypeface> resolved;
  bool needs_resolve = false;
  {
    std::scoped_lock lock(mutex_);
    if (known_typefaces_.count(raw) != 0) {
      typeface = static_cast<const FreeTypeTypeface*>(raw);
    } else {
      auto found = foreign_.find(raw->GetHash());
      if (found != foreign_.end()) {
        resolved = found->second;
      } else {
        needs_resolve = true;
      }
    }
  }
  if (needs_resolve) {
    // Outside the lock: the resolver calls back into CreateTypeface.
    resolved = resolver_ ? resolver_(*this, *raw) : nullptr;
    std::scoped_lock lock(mutex_);
    foreign_[raw->GetHash()] = resolved;
  }
  if (typeface == nullptr) {
    if (!resolved && foreign_rasterizer_) {
      // Nothing FreeType can load. Let the platform draw it rather than
      // dropping the text.
      return foreign_rasterizer_(font, glyph, scale, subpixel_offset);
    }
    if (!resolved) {
      if (!warned_foreign_typeface_) {
        warned_foreign_typeface_ = true;
        FML_LOG(WARNING) << "Propeller: unresolvable non-FreeType typeface; "
                            "glyphs skipped.";
      }
      return result;
    }
    typeface = resolved.get();
  }
  if (!typeface->IsValid()) {
    return result;
  }
  FT_Face face = typeface->GetFace();
  std::scoped_lock lock(mutex_);

  // Point size scaled by the rounded total scale, in 26.6 fixed point at
  // 72dpi, so one point is one pixel before scaling.
  const Scalar scaled_size =
      font.GetMetrics().point_size * static_cast<Scalar>(scale);
  const auto size_26_6 = static_cast<FT_F26Dot6>(std::round(scaled_size * 64));
  if (size_26_6 <= 0 || FT_Set_Char_Size(face, 0, size_26_6, 72, 72) != 0) {
    return result;
  }

  if (FT_Load_Glyph(face, glyph.index, FT_LOAD_TARGET_LIGHT) != 0) {
    return result;
  }
  const FT_GlyphSlot slot = face->glyph;
  if (slot->format == FT_GLYPH_FORMAT_OUTLINE && subpixel_offset != 0) {
    FT_Outline_Translate(&slot->outline,
                         static_cast<FT_Pos>(std::lround(subpixel_offset * 64)),
                         0);
  }
  if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
    return result;
  }
  const FT_Bitmap& bitmap = slot->bitmap;
  if (bitmap.width == 0 || bitmap.rows == 0) {
    return result;  // Valid empty glyph (e.g. space).
  }
  if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
    // No COLR/bitmap support initially per propeller.md.
    FML_LOG(WARNING) << "Unsupported glyph pixel mode: "
                     << static_cast<int>(bitmap.pixel_mode);
    return result;
  }

  result.width = static_cast<int32_t>(bitmap.width);
  result.height = static_cast<int32_t>(bitmap.rows);
  result.bearing = Point(static_cast<Scalar>(slot->bitmap_left),
                         static_cast<Scalar>(-slot->bitmap_top));
  result.coverage.resize(static_cast<size_t>(result.width) * result.height);
  for (int32_t row = 0; row < result.height; row++) {
    const uint8_t* src = bitmap.buffer + row * bitmap.pitch;
    std::memcpy(result.coverage.data() + row * result.width, src, result.width);
  }
  return result;
}

}  // namespace impeller
