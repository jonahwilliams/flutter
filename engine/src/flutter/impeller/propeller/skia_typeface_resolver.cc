// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/skia_typeface_resolver.h"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <mutex>
#include <vector>

#include "flutter/fml/logging.h"
#include "flutter/fml/mapping.h"
#include "impeller/propeller/freetype_glyph_rasterizer.h"
#include "impeller/typographer/backends/skia/typeface_skia.h"
#include "third_party/skia/include/core/SkFontTypes.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include "third_party/skia/include/ports/SkTypeface_mac.h"
#endif

namespace impeller {

namespace {

constexpr SkFontTableTag kTagGlyf = SkSetFourByteTag('g', 'l', 'y', 'f');
constexpr SkFontTableTag kTagLoca = SkSetFourByteTag('l', 'o', 'c', 'a');
constexpr SkFontTableTag kTagCff = SkSetFourByteTag('C', 'F', 'F', ' ');
constexpr SkFontTableTag kTagCff2 = SkSetFourByteTag('C', 'F', 'F', '2');

/// Whether FreeType could draw this font at all.
///
/// Outlines live in `glyf`+`loca` or in `CFF `/`CFF2`. Apple's system CJK
/// faces (PingFang, and the UI font on recent macOS) carry theirs in
/// private tables -- `hvgl` and `cidg` -- that only CoreText can read.
/// Those fonts are not incompletely serialized or wrongly indexed; there
/// is simply nothing FreeType can use.
///
/// Worth checking BEFORE opening the stream: the table list is a handful
/// of bytes, where the stream for one of these is ~59MB that gets read,
/// copied, and thrown away once FreeType reaches the same conclusion.
bool HasOutlinesFreeTypeCanRead(const SkTypeface& typeface) {
  const int count = typeface.countTables();
  if (count <= 0) {
    return true;  // Unknown: let FreeType decide.
  }
  std::vector<SkFontTableTag> tags(count);
  const int read = typeface.readTableTags(SkSpan<SkFontTableTag>(tags));
  if (read <= 0) {
    return true;
  }
  tags.resize(read);
  bool glyf = false;
  bool loca = false;
  for (SkFontTableTag tag : tags) {
    glyf |= tag == kTagGlyf;
    loca |= tag == kTagLoca;
    if (tag == kTagCff || tag == kTagCff2) {
      return true;
    }
  }
  return glyf && loca;
}

}  // namespace

std::shared_ptr<FreeTypeTypeface> ResolveSkiaTypeface(
    FreeTypeGlyphRasterizer& rasterizer,
    const Typeface& typeface) {
  // In-engine, any typeface the FreeType rasterizer did not mint is
  // Skia-backed (the typographer's only other backend).
  const TypefaceSkia& skia_typeface = TypefaceSkia::Cast(typeface);
  const sk_sp<SkTypeface>& sk_typeface = skia_typeface.GetSkiaTypeface();
  if (!sk_typeface) {
    return nullptr;
  }

  // Distinct TypefaceSkia wrappers routinely wrap the same SkTypeface;
  // share one FreeType face per underlying font.
  static std::mutex cache_mutex;
  static std::map<uint32_t, std::shared_ptr<FreeTypeTypeface>> cache;
  const uint32_t unique_id = sk_typeface->uniqueID();
  {
    std::scoped_lock lock(cache_mutex);
    auto found = cache.find(unique_id);
    if (found != cache.end()) {
      return found->second;
    }
  }

  if (!HasOutlinesFreeTypeCanRead(*sk_typeface)) {
    // Not an error, and not worth repeating: these fall through to the
    // platform rasterizer and draw normally. Say it once per family so
    // the reason is on the record without a line per typeface object.
    SkString family;
    sk_typeface->getFamilyName(&family);
    static std::mutex announced_mutex;
    static std::set<std::string> announced;
    {
      std::scoped_lock lock(announced_mutex);
      if (announced.insert(family.c_str()).second) {
        FML_LOG(INFO) << "Propeller: '" << family.c_str()
                      << "' keeps its outlines in tables FreeType cannot "
                         "read; rasterizing it through the platform.";
      }
    }
    std::scoped_lock lock(cache_mutex);
    cache[unique_id] = nullptr;
    return nullptr;
  }

  int ttc_index = 0;
  std::unique_ptr<SkStreamAsset> stream = sk_typeface->openStream(&ttc_index);
  if (!stream || stream->getLength() == 0) {
    FML_LOG(WARNING) << "Propeller: Skia typeface has no accessible data.";
    return nullptr;
  }
  std::vector<uint8_t> bytes(stream->getLength());
  if (stream->read(bytes.data(), bytes.size()) != bytes.size()) {
    return nullptr;
  }

  std::shared_ptr<FreeTypeTypeface> resolved = rasterizer.CreateTypeface(
      std::make_shared<fml::DataMapping>(std::move(bytes)), ttc_index);
  {
    std::scoped_lock lock(cache_mutex);
    cache[unique_id] = resolved;
  }
  return resolved;
}

#ifdef __APPLE__
RasterizedGlyph RasterizeGlyphWithCoreText(const Font& font,
                                           Glyph glyph,
                                           Rational scale,
                                           Scalar subpixel_offset) {
  RasterizedGlyph result;
  const std::shared_ptr<Typeface>& typeface = font.GetTypeface();
  if (!typeface) {
    return result;
  }
  const sk_sp<SkTypeface>& sk_typeface =
      TypefaceSkia::Cast(*typeface).GetSkiaTypeface();
  if (!sk_typeface) {
    return result;
  }
  // Only the platform HANDLE comes from Skia; the rasterization below is
  // CoreText, which is what Skia would be doing on our behalf anyway.
  CTFontRef base = SkTypeface_GetCTFontRef(sk_typeface.get());
  if (base == nullptr) {
    return result;
  }
  const CGFloat size =
      font.GetMetrics().point_size * static_cast<Scalar>(scale);
  if (size <= 0) {
    return result;
  }
  CTFontRef sized =
      CTFontCreateCopyWithAttributes(base, size, nullptr, nullptr);
  if (sized == nullptr) {
    return result;
  }

  const CGGlyph glyph_id = glyph.index;
  const CGRect bounds = CTFontGetBoundingRectsForGlyphs(
      sized, kCTFontOrientationHorizontal, &glyph_id, nullptr, 1);
  if (CGRectIsNull(bounds) || CGRectIsEmpty(bounds)) {
    CFRelease(sized);
    return result;  // Whitespace: a valid empty glyph.
  }
  // CoreText works in a y-UP space with the origin on the baseline. Grow
  // to whole pixels around the subpixel-shifted outline, with a texel of
  // slack so antialiased edges are not clipped.
  const CGRect shifted = CGRectOffset(bounds, subpixel_offset, 0);
  const int32_t left = static_cast<int32_t>(std::floor(CGRectGetMinX(shifted))) - 1;
  const int32_t bottom = static_cast<int32_t>(std::floor(CGRectGetMinY(shifted))) - 1;
  const int32_t right = static_cast<int32_t>(std::ceil(CGRectGetMaxX(shifted))) + 1;
  const int32_t top = static_cast<int32_t>(std::ceil(CGRectGetMaxY(shifted))) + 1;
  const int32_t width = right - left;
  const int32_t height = top - bottom;
  if (width <= 0 || height <= 0) {
    CFRelease(sized);
    return result;
  }

  std::vector<uint8_t> coverage(static_cast<size_t>(width) * height, 0);
  // kCGImageAlphaOnly IS the coverage format: one byte per pixel, and the
  // first row of the buffer is the TOP row, which is the layout the atlas
  // and RasterizedGlyph already use.
  CGContextRef context = CGBitmapContextCreate(
      coverage.data(), width, height, 8, width, nullptr, kCGImageAlphaOnly);
  if (context == nullptr) {
    CFRelease(sized);
    return result;
  }
  CGContextSetShouldAntialias(context, true);
  CGContextSetShouldSmoothFonts(context, false);  // Coverage, not LCD.
  CGContextSetShouldSubpixelPositionFonts(context, true);
  CGContextSetShouldSubpixelQuantizeFonts(context, false);
  // Place the baseline origin so the outline lands inside the bitmap.
  const CGPoint position =
      CGPointMake(subpixel_offset - left, -static_cast<CGFloat>(bottom));
  CTFontDrawGlyphs(sized, &glyph_id, &position, 1, context);
  CGContextRelease(context);
  CFRelease(sized);

  result.width = width;
  result.height = height;
  // y-down offset from the glyph origin to the bitmap's top-left, the
  // same convention FreeType's (bitmap_left, -bitmap_top) produces.
  result.bearing = Point(static_cast<Scalar>(left), static_cast<Scalar>(-top));
  result.coverage = std::move(coverage);
  return result;
}
#endif  // __APPLE__

}  // namespace impeller
