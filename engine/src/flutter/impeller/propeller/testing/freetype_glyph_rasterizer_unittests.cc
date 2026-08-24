// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cmath>

#include "flutter/testing/testing.h"
#include "impeller/propeller/freetype_glyph_rasterizer.h"
#ifdef __APPLE__
#include "impeller/propeller/skia_typeface_resolver.h"
#include "impeller/typographer/backends/skia/typeface_skia.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/ports/SkFontMgr_mac_ct.h"
#endif
#include "impeller/propeller/paged_atlas.h"
#include "impeller/propeller/testing/stub_gpu_context.h"
#include "impeller/typographer/text_frame.h"

namespace impeller {
namespace testing {

namespace {

std::shared_ptr<FreeTypeTypeface> LoadRoboto(FreeTypeGlyphRasterizer& raster) {
  auto mapping = flutter::testing::OpenFixtureAsMapping("Roboto-Regular.ttf");
  return raster.CreateTypeface(
      std::shared_ptr<fml::Mapping>(std::move(mapping)));
}

}  // namespace

TEST(FreeTypeGlyphRasterizerTest, RasterizesARealGlyph) {
  auto rasterizer = FreeTypeGlyphRasterizer::Make();
  ASSERT_NE(rasterizer, nullptr);
  auto typeface = LoadRoboto(*rasterizer);
  ASSERT_NE(typeface, nullptr);

  auto index = rasterizer->GetGlyphIndex(*typeface, 'A');
  ASSERT_TRUE(index.has_value());

  Font font(typeface, Font::Metrics{.point_size = 50}, AxisAlignment::kNone);
  RasterizedGlyph glyph = rasterizer->Rasterize(
      font, Glyph(*index, Glyph::Type::kPath), Rational(1), 0);

  ASSERT_GT(glyph.width, 0);
  ASSERT_GT(glyph.height, 0);
  EXPECT_EQ(glyph.coverage.size(),
            static_cast<size_t>(glyph.width) * glyph.height);
  // 'A' at 50pt must produce solid coverage somewhere.
  EXPECT_GT(*std::max_element(glyph.coverage.begin(), glyph.coverage.end()),
            0x80);
  // The bitmap sits above the baseline.
  EXPECT_LT(glyph.bearing.y, 0);
}

TEST(FreeTypeGlyphRasterizerTest, ScaleScalesTheBitmap) {
  auto rasterizer = FreeTypeGlyphRasterizer::Make();
  ASSERT_NE(rasterizer, nullptr);
  auto typeface = LoadRoboto(*rasterizer);
  ASSERT_NE(typeface, nullptr);
  auto index = rasterizer->GetGlyphIndex(*typeface, 'A');
  ASSERT_TRUE(index.has_value());

  Font font(typeface, Font::Metrics{.point_size = 20}, AxisAlignment::kNone);
  const Glyph glyph(*index, Glyph::Type::kPath);
  RasterizedGlyph at_1x = rasterizer->Rasterize(font, glyph, Rational(1), 0);
  RasterizedGlyph at_2x = rasterizer->Rasterize(font, glyph, Rational(2), 0);

  ASSERT_GT(at_1x.width, 0);
  EXPECT_GE(at_2x.width, at_1x.width * 3 / 2);
  EXPECT_GE(at_2x.height, at_1x.height * 3 / 2);
}

TEST(FreeTypeGlyphRasterizerTest, SubpixelPhasesProduceDistinctCoverage) {
  auto rasterizer = FreeTypeGlyphRasterizer::Make();
  ASSERT_NE(rasterizer, nullptr);
  auto typeface = LoadRoboto(*rasterizer);
  ASSERT_NE(typeface, nullptr);
  auto index = rasterizer->GetGlyphIndex(*typeface, 'A');
  ASSERT_TRUE(index.has_value());

  Font font(typeface, Font::Metrics{.point_size = 14}, AxisAlignment::kNone);
  const Glyph glyph(*index, Glyph::Type::kPath);
  RasterizedGlyph phase0 = rasterizer->Rasterize(font, glyph, Rational(1), 0);
  RasterizedGlyph phase2 =
      rasterizer->Rasterize(font, glyph, Rational(1), 0.5f);

  ASSERT_GT(phase0.width, 0);
  // A half-pixel shift must change the rasterization (dimensions, bearing,
  // or coverage bytes).
  EXPECT_TRUE(phase0.width != phase2.width ||
              phase0.bearing != phase2.bearing ||
              phase0.coverage != phase2.coverage);
}

TEST(FreeTypeGlyphRasterizerTest, MissingCodepointHasNoIndex) {
  auto rasterizer = FreeTypeGlyphRasterizer::Make();
  ASSERT_NE(rasterizer, nullptr);
  auto typeface = LoadRoboto(*rasterizer);
  ASSERT_NE(typeface, nullptr);

  // Roboto has no CJK coverage.
  EXPECT_FALSE(rasterizer->GetGlyphIndex(*typeface, 0x4E2D).has_value());
}

// Skia reports a face index per typeface, and it does not always match
// the data it hands over -- a non-collection font with a non-zero index
// used to be dropped outright, taking every glyph in it with it.
TEST(FreeTypeGlyphRasterizerTest, BadFaceIndexFallsBackToFaceZero) {
  auto rasterizer = FreeTypeGlyphRasterizer::Make();
  ASSERT_NE(rasterizer, nullptr);

  auto mapping = flutter::testing::OpenFixtureAsMapping("Roboto-Regular.ttf");
  ASSERT_NE(mapping, nullptr);
  // Roboto is a single face, so index 3 does not exist.
  auto typeface = rasterizer->CreateTypeface(
      std::shared_ptr<fml::Mapping>(std::move(mapping)), /*face_index=*/3);
  ASSERT_NE(typeface, nullptr) << "a bad index dropped the whole font";
  EXPECT_TRUE(typeface->IsValid());

  // And it is the real font, not an empty stand-in.
  auto index = rasterizer->GetGlyphIndex(*typeface, 'A');
  ASSERT_TRUE(index.has_value());
  Font font(typeface, Font::Metrics{.point_size = 32}, AxisAlignment::kNone);
  const RasterizedGlyph glyph = rasterizer->Rasterize(
      font, Glyph(*index, Glyph::Type::kPath), Rational(1), 0);
  EXPECT_GT(glyph.width, 0);
  EXPECT_GT(glyph.height, 0);
}

// Data that is not a font at all still has to fail cleanly rather than
// take the rasterizer down with it.
TEST(FreeTypeGlyphRasterizerTest, GarbageDataFailsWithoutCrashing) {
  auto rasterizer = FreeTypeGlyphRasterizer::Make();
  ASSERT_NE(rasterizer, nullptr);
  std::vector<uint8_t> garbage(512, 0x7F);
  EXPECT_EQ(rasterizer->CreateTypeface(
                std::make_shared<fml::DataMapping>(std::move(garbage))),
            nullptr);
  // Short enough that the tag read has to be bounds checked.
  std::vector<uint8_t> tiny{0x01, 0x02};
  EXPECT_EQ(rasterizer->CreateTypeface(
                std::make_shared<fml::DataMapping>(std::move(tiny))),
            nullptr);
}

#ifdef __APPLE__
// Faces that CoreText serves out of a collection do not serialize
// completely: Skia flattens one face into a synthesized sfnt, and tables
// it cannot copy are simply absent. Songti SC comes out as a structurally
// valid 'true' font with `glyf` but no `loca`, which FreeType rejects
// with error 0x90 -- so the resolver falls back to the file on disk,
// which has everything.
TEST(FreeTypeGlyphRasterizerTest, ResolvesSystemCollectionTypefaces) {
  auto rasterizer = FreeTypeGlyphRasterizer::Make();
  ASSERT_NE(rasterizer, nullptr);
  sk_sp<SkFontMgr> manager = SkFontMgr_New_CoreText(nullptr);
  ASSERT_NE(manager, nullptr);

  int checked = 0;
  for (const char* family :
       {"Songti SC", "STHeiti", "Hiragino Sans", "Apple SD Gothic Neo"}) {
    sk_sp<SkTypeface> sk_typeface =
        manager->matchFamilyStyle(family, SkFontStyle());
    if (!sk_typeface) {
      continue;  // Not installed on this machine.
    }
    auto typeface = std::make_shared<TypefaceSkia>(sk_typeface);
    std::shared_ptr<FreeTypeTypeface> resolved =
        ResolveSkiaTypeface(*rasterizer, *typeface);
    ASSERT_NE(resolved, nullptr) << family << " did not resolve";
    EXPECT_TRUE(resolved->IsValid()) << family;

    // Resolving is not enough: a missing outline table is exactly what
    // stops a glyph from rasterizing, so demand real coverage.
    auto index = rasterizer->GetGlyphIndex(*resolved, 0x4E2D);  // CJK
    if (!index.has_value()) {
      index = rasterizer->GetGlyphIndex(*resolved, 'A');
    }
    ASSERT_TRUE(index.has_value()) << family;
    Font font(resolved, Font::Metrics{.point_size = 32}, AxisAlignment::kNone);
    const RasterizedGlyph glyph = rasterizer->Rasterize(
        font, Glyph(*index, Glyph::Type::kPath), Rational(1), 0);
    EXPECT_GT(glyph.width, 0) << family;
    EXPECT_GT(glyph.height, 0) << family;
    ASSERT_FALSE(glyph.coverage.empty()) << family;
    EXPECT_GT(*std::max_element(glyph.coverage.begin(), glyph.coverage.end()),
              0x40)
        << family;
    checked++;
  }
  if (checked == 0) {
    GTEST_SKIP() << "No system collection typefaces installed.";
  }
}
// Apple's system CJK faces keep their glyphs in `hvgl`/`cidg`, private
// tables only CoreText can read -- there is no `glyf`/`loca` or `CFF` for
// FreeType to work from, at any face index, from any source. So the
// platform draws them instead, and the text appears.
//
// Recognising that from the table list also costs a few bytes, where
// discovering it by loading costs a ~59MB stream read per attempt.
TEST(FreeTypeGlyphRasterizerTest, PrivateOutlineFontsRasterizeViaPlatform) {
  auto rasterizer = FreeTypeGlyphRasterizer::Make();
  ASSERT_NE(rasterizer, nullptr);
  sk_sp<SkFontMgr> manager = SkFontMgr_New_CoreText(nullptr);
  ASSERT_NE(manager, nullptr);
  sk_sp<SkTypeface> sk_typeface =
      manager->matchFamilyStyle("PingFang SC", SkFontStyle());
  if (!sk_typeface) {
    GTEST_SKIP() << "PingFang SC is not installed.";
  }

  // The premise: no outline table FreeType understands.
  const int count = sk_typeface->countTables();
  ASSERT_GT(count, 0);
  std::vector<SkFontTableTag> tags(count);
  const int read = sk_typeface->readTableTags(SkSpan<SkFontTableTag>(tags));
  ASSERT_GT(read, 0);
  tags.resize(read);
  bool has_glyf = false;
  bool has_cff = false;
  for (SkFontTableTag tag : tags) {
    has_glyf |= tag == SkSetFourByteTag('g', 'l', 'y', 'f');
    has_cff |= tag == SkSetFourByteTag('C', 'F', 'F', ' ') ||
               tag == SkSetFourByteTag('C', 'F', 'F', '2');
  }
  ASSERT_FALSE(has_glyf || has_cff)
      << "PingFang gained standard outlines; this test is obsolete";

  auto typeface = std::make_shared<TypefaceSkia>(sk_typeface);
  EXPECT_EQ(ResolveSkiaTypeface(*rasterizer, *typeface), nullptr);

  // FreeType cannot load it -- but the text still has to appear. With the
  // platform rasterizer wired up, a real CJK glyph must come back with
  // real coverage.
  rasterizer->SetForeignTypefaceResolver(&ResolveSkiaTypeface);
  rasterizer->SetForeignGlyphRasterizer(&RasterizeGlyphWithCoreText);

  SkGlyphID glyph_id = 0;
  const SkUnichar cjk = 0x4E2D;  // U+4E2D
  sk_typeface->unicharsToGlyphs(SkSpan<const SkUnichar>(&cjk, 1),
                                SkSpan<SkGlyphID>(&glyph_id, 1));
  ASSERT_NE(glyph_id, 0) << "the font has no glyph for U+4E2D";

  Font font(typeface, Font::Metrics{.point_size = 48}, AxisAlignment::kNone);
  const RasterizedGlyph glyph = rasterizer->Rasterize(
      font, Glyph(glyph_id, Glyph::Type::kPath), Rational(1), 0);
  EXPECT_GT(glyph.width, 0) << "no glyph bitmap: the text would be invisible";
  EXPECT_GT(glyph.height, 0);
  ASSERT_EQ(glyph.coverage.size(),
            static_cast<size_t>(glyph.width) * glyph.height);
  EXPECT_GT(*std::max_element(glyph.coverage.begin(), glyph.coverage.end()),
            0x80)
      << "the bitmap is blank";
  // Above the baseline, like every other glyph.
  EXPECT_LT(glyph.bearing.y, 0);
}

#endif  // __APPLE__

}  // namespace testing
}  // namespace impeller
