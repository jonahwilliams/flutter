// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_SKIA_TYPEFACE_RESOLVER_H_
#define FLUTTER_IMPELLER_PROPELLER_SKIA_TYPEFACE_RESOLVER_H_

#include <memory>

#include "impeller/propeller/text_materializer.h"
#include "impeller/typographer/font.h"
#include "impeller/typographer/glyph.h"

namespace impeller {

class FreeTypeGlyphRasterizer;
class FreeTypeTypeface;
class Typeface;

/// A ForeignTypefaceResolver for engine text: extracts the font data from a
/// Skia-backed typeface (the kind DlTextImpeller frames carry) and loads it
/// into FreeType, cached by SkTypeface id. Glyph ids are font-intrinsic, so
/// the shaped frame's ids remain valid against the FreeType face.
std::shared_ptr<FreeTypeTypeface> ResolveSkiaTypeface(
    FreeTypeGlyphRasterizer& rasterizer,
    const Typeface& typeface);

#ifdef __APPLE__
/// A ForeignGlyphRasterizer for fonts FreeType cannot load: Apple's system
/// CJK faces keep their outlines in private tables (`hvgl`, `cidg`) that
/// only the platform can read, so these go through CoreText straight to a
/// coverage bitmap in the layout the glyph atlas expects.
RasterizedGlyph RasterizeGlyphWithCoreText(const Font& font,
                                           Glyph glyph,
                                           Rational scale,
                                           Scalar subpixel_offset);
#endif  // __APPLE__

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_SKIA_TYPEFACE_RESOLVER_H_
