// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_FREETYPE_GLYPH_RASTERIZER_H_
#define FLUTTER_IMPELLER_PROPELLER_FREETYPE_GLYPH_RASTERIZER_H_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include "flutter/fml/mapping.h"
#include "impeller/propeller/text_materializer.h"
#include "impeller/typographer/typeface.h"

// Keep FreeType out of the public header.
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace impeller {

class FreeTypeGlyphRasterizer;

//------------------------------------------------------------------------------
/// Owns the FT_Library.
class FreeTypeLibrary {
 public:
  explicit FreeTypeLibrary(FT_Library library) : library_(library) {}

  ~FreeTypeLibrary();

  FreeTypeLibrary(const FreeTypeLibrary&) = delete;
  FreeTypeLibrary& operator=(const FreeTypeLibrary&) = delete;

  FT_Library Get() const { return library_; }

 private:
  FT_Library library_ = nullptr;
};

//------------------------------------------------------------------------------
/// A typeface backed by an in-memory font file loaded through FreeType.
class FreeTypeTypeface final : public Typeface {
 public:
  ~FreeTypeTypeface() override;

  // |Typeface|
  bool IsValid() const override;

  // |Comparable<Typeface>|
  std::size_t GetHash() const override;

  // |Comparable<Typeface>|
  bool IsEqual(const Typeface& other) const override;

  FT_Face GetFace() const { return face_; }

 private:
  friend class FreeTypeGlyphRasterizer;

  FreeTypeTypeface(std::shared_ptr<FreeTypeLibrary> library,
                   FT_Face face,
                   std::shared_ptr<fml::Mapping> mapping);

  /// Keeps the library alive for at least as long as the face.
  std::shared_ptr<FreeTypeLibrary> library_;
  FT_Face face_ = nullptr;
  std::shared_ptr<fml::Mapping> mapping_;
};

//------------------------------------------------------------------------------
/// GlyphRasterization via freetype.
class FreeTypeGlyphRasterizer final : public GlyphRasterizer {
 public:
  /// Returns nullptr if FreeType fails to initialize.
  static std::shared_ptr<FreeTypeGlyphRasterizer> Make();

  ~FreeTypeGlyphRasterizer() override;

  /// Load a typeface from an in-memory font file. Returns nullptr on failure.
  std::shared_ptr<FreeTypeTypeface> CreateTypeface(
      std::shared_ptr<fml::Mapping> mapping,
      int face_index = 0);

  /// Resolves a foreign typefaceinto a FreeType-backed one.
  using ForeignTypefaceResolver =
      std::function<std::shared_ptr<FreeTypeTypeface>(
          FreeTypeGlyphRasterizer& rasterizer,
          const Typeface& typeface)>;

  void SetForeignTypefaceResolver(ForeignTypefaceResolver resolver) {
    resolver_ = std::move(resolver);
  }

  /// Rasterizes a glyph for a typeface FreeType cannot load at all.
  ///
  /// Apple's system CJK faces keep their outlines in private tables, so
  /// there is no font data to hand FreeType.
  using ForeignGlyphRasterizer =
      std::function<RasterizedGlyph(const Font& font,
                                    Glyph glyph,
                                    Rational scale,
                                    Scalar subpixel_offset)>;

  void SetForeignGlyphRasterizer(ForeignGlyphRasterizer rasterizer) {
    foreign_rasterizer_ = std::move(rasterizer);
  }

  /// Map a unicode codepoint to a glyph index, since the prototype has no
  /// shaper. Returns nullopt when the face has no glyph for the codepoint.
  std::optional<uint16_t> GetGlyphIndex(const FreeTypeTypeface& typeface,
                                        uint32_t codepoint);

  // |GlyphRasterizer|
  RasterizedGlyph Rasterize(const Font& font,
                            Glyph glyph,
                            Rational scale,
                            Scalar subpixel_offset) override;

 private:
  explicit FreeTypeGlyphRasterizer(std::shared_ptr<FreeTypeLibrary> library);

  /// FreeType faces are not thread safe; one big lock for the prototype.
  std::mutex mutex_;
  std::shared_ptr<FreeTypeLibrary> library_;
  std::set<const Typeface*> known_typefaces_;
  std::unordered_map<std::size_t, std::shared_ptr<FreeTypeTypeface>> foreign_;
  ForeignTypefaceResolver resolver_;
  ForeignGlyphRasterizer foreign_rasterizer_;
  bool warned_foreign_typeface_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_FREETYPE_GLYPH_RASTERIZER_H_
