// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/testing/testing.h"
#include "impeller/propeller/buffer_arena.h"
#include "impeller/propeller/paged_atlas.h"
#include "impeller/propeller/testing/stub_gpu_context.h"
#include "impeller/propeller/text_materializer.h"
#include "impeller/typographer/text_frame.h"

namespace impeller {
namespace testing {

namespace {

class FakeTypeface final : public Typeface {
 public:
  // |Typeface|
  bool IsValid() const override { return true; }

  // |Comparable<Typeface>|
  std::size_t GetHash() const override { return 42; }

  // |Comparable<Typeface>|
  bool IsEqual(const Typeface& other) const override {
    return other.GetHash() == GetHash();
  }
};

/// Every non-zero glyph index rasterizes as an 8x8 solid coverage block.
class FakeRasterizer final : public GlyphRasterizer {
 public:
  // |GlyphRasterizer|
  RasterizedGlyph Rasterize(const Font& font,
                            Glyph glyph,
                            Rational scale,
                            Scalar subpixel_offset) override {
    rasterize_count++;
    offsets_seen.push_back(subpixel_offset);
    scales_seen.push_back(static_cast<Scalar>(scale));
    if (glyph.index == 0) {
      return {};  // Whitespace-like: no coverage.
    }
    RasterizedGlyph result;
    result.width = 8;
    result.height = 8;
    result.bearing = Point(0, -8);
    result.coverage = std::vector<uint8_t>(64, 0xFF);
    return result;
  }

  int rasterize_count = 0;
  std::vector<Scalar> offsets_seen;
  std::vector<Scalar> scales_seen;
};

std::shared_ptr<TextFrame> MakeFrame(uint16_t glyph_count,
                                     Scalar spacing = 10.0f) {
  Font font(std::make_shared<FakeTypeface>(), Font::Metrics{},
            AxisAlignment::kNone);
  TextRun run(font);
  for (uint16_t i = 0; i < glyph_count; i++) {
    run.AddGlyph(Glyph(i + 1, Glyph::Type::kPath), Point(i * spacing, 0));
  }
  std::vector<TextRun> runs = {run};
  return std::make_shared<TextFrame>(
      runs, Rect::MakeLTRB(0, -10, glyph_count * spacing, 0),
      /*has_color=*/false);
}

}  // namespace

TEST(PagedAtlasTest, PlacesEntriesAndGrowsPages) {
  // With the 1px guard border a 32x32 entry occupies 34x34, so only one fits
  // per 64x64 page.
  StubGpuContext context;
  PagedAtlas atlas(&context, TextureFormat::kR8UNorm, 64, 64, 3);
  auto a = atlas.PlaceEntry(32, 32);
  auto b = atlas.PlaceEntry(32, 32);
  auto c = atlas.PlaceEntry(32, 32);
  ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
  EXPECT_EQ(a->page, 0u);
  EXPECT_EQ(b->page, 1u);
  EXPECT_EQ(c->page, 2u);
  EXPECT_EQ(atlas.GetPageCount(), 3u);

  // All pages full and max_pages reached.
  EXPECT_FALSE(atlas.PlaceEntry(32, 32).has_value());
  // Never fits at all.
  EXPECT_FALSE(atlas.PlaceEntry(64, 64).has_value());
}

TEST(PagedAtlasTest, WriteEntryBlitsIntoPageAndBumpsRevision) {
  StubGpuContext context;
  PagedAtlas atlas(&context, TextureFormat::kR8UNorm, 64, 64, 1);
  auto placement = atlas.PlaceEntry(2, 2);
  ASSERT_TRUE(placement.has_value());

  const uint64_t revision = atlas.GetRevision();
  const uint8_t data[4] = {1, 2, 3, 4};
  atlas.WriteEntry(*placement, data);
  EXPECT_GT(atlas.GetRevision(), revision);

  const std::vector<uint8_t>& page =
      static_cast<StubGpuBuffer*>(atlas.GetPageBuffer(0))->GetData();
  ASSERT_EQ(page.size(), 64u * 64u);
  EXPECT_EQ(page[placement->y * 64 + placement->x], 1);
  EXPECT_EQ(page[placement->y * 64 + placement->x + 1], 2);
  EXPECT_EQ(page[(placement->y + 1) * 64 + placement->x], 3);
  EXPECT_EQ(page[(placement->y + 1) * 64 + placement->x + 1], 4);

  // The write is staged, not uploaded, until a command buffer records it.
  StubGpuCommandBuffer command_buffer;
  atlas.RecordUploads(command_buffer);
  ASSERT_EQ(command_buffer.updates.size(), 1u);
  EXPECT_EQ(command_buffer.updates[0].width,
            static_cast<uint32_t>(placement->width));
  EXPECT_EQ(command_buffer.updates[0].row_bytes, 64u);

  // Nothing left pending.
  StubGpuCommandBuffer second;
  atlas.RecordUploads(second);
  EXPECT_TRUE(second.updates.empty());
}

TEST(PagedAtlasTest, CompactClearsPagesAndBumpsGeneration) {
  StubGpuContext context;
  PagedAtlas atlas(&context, TextureFormat::kR8UNorm, 64, 64, 2);
  ASSERT_TRUE(atlas.PlaceEntry(8, 8).has_value());
  EXPECT_EQ(atlas.GetGeneration(), 0u);
  EXPECT_EQ(atlas.GetPageCount(), 1u);

  atlas.Compact();

  EXPECT_EQ(atlas.GetGeneration(), 1u);
  EXPECT_EQ(atlas.GetPageCount(), 0u);
}

class TextMaterializerTest : public ::testing::Test {
 public:
  void SetUp() override {
    atlas_ = std::make_shared<PagedAtlas>(&context_, TextureFormat::kR8UNorm,
                                          256, 256, 4);
    rasterizer_ = std::make_shared<FakeRasterizer>();
    materializer_ = std::make_unique<TextMaterializer>(atlas_, rasterizer_);
  }

  /// Declared first, so it outlives the atlas that points at it.
  StubGpuContext context_;
  std::shared_ptr<PagedAtlas> atlas_;
  std::shared_ptr<FakeRasterizer> rasterizer_;
  std::unique_ptr<TextMaterializer> materializer_;
};

// Two entries sharing texels is invisible until the second upload lands
// on the first, and then one glyph draws with another's pixels -- the
// same letter coming out right in one place and wrong in another.
TEST(PagedAtlasTest, PlacementsNeverOverlap) {
  auto context = std::make_shared<StubGpuContext>();
  PagedAtlas atlas(context.get(), TextureFormat::kR8UNorm, 128, 128, 4);

  struct Placed {
    PagedAtlas::Placement placement;
    int32_t width = 0;
    int32_t height = 0;
  };
  std::vector<Placed> placed;
  // Sizes that do not divide the page evenly, so the packer has to leave
  // awkward gaps rather than a clean grid.
  const int32_t sizes[] = {3, 7, 11, 5, 13, 9, 17, 6};
  for (int i = 0; i < 200; i++) {
    const int32_t w = sizes[i % 8];
    const int32_t h = sizes[(i + 3) % 8];
    std::optional<PagedAtlas::Placement> placement = atlas.PlaceEntry(w, h);
    if (!placement.has_value()) {
      break;  // Full: everything placed so far still has to be disjoint.
    }
    placed.push_back({*placement, w, h});
  }
  ASSERT_GT(placed.size(), 20u) << "too few placements to prove anything";

  for (size_t a = 0; a < placed.size(); a++) {
    const PagedAtlas::Placement& first = placed[a].placement;
    EXPECT_EQ(first.width, placed[a].width);
    EXPECT_EQ(first.height, placed[a].height);
    for (size_t b = a + 1; b < placed.size(); b++) {
      const PagedAtlas::Placement& second = placed[b].placement;
      if (first.page != second.page) {
        continue;
      }
      const bool apart = first.x + first.width <= second.x ||
                         second.x + second.width <= first.x ||
                         first.y + first.height <= second.y ||
                         second.y + second.height <= first.y;
      ASSERT_TRUE(apart) << "entries " << a << " and " << b
                         << " share texels on page " << first.page << ": ("
                         << first.x << "," << first.y << " " << first.width
                         << "x" << first.height << ") and (" << second.x << ","
                         << second.y << " " << second.width << "x"
                         << second.height << ")";
    }
  }
}

// -----------------------------------------------------------------------
// Resolving for the propeller path.

/// One glyph as it landed in the streams, read back off them.
struct ResolvedQuad {
  Rect position;
  Rect uv;
  uint32_t color;
  uint32_t paint;
};

/// Resolve into streams sized for the frame and read the quads back:
/// four vertices each, in Rect::GetPoints order.
std::vector<ResolvedQuad> Resolve(TextMaterializer& materializer,
                                  const TextFrame& frame,
                                  Point origin,
                                  const Matrix& composed,
                                  uint32_t color = 0,
                                  uint32_t paint = 0) {
  size_t glyphs = 0;
  for (const TextRun& run : frame.GetRuns()) {
    glyphs += run.GetGlyphCount();
  }
  // Slack past what the frame needs, so writing too much is caught
  // rather than landing on the last quad.
  std::vector<Point> positions(glyphs * 4 + 4);
  std::vector<Attributes> attributes(glyphs * 4 + 4);
  const uint32_t written = materializer.ResolveGlyphs(
      frame, origin, composed, color, paint, positions.data(),
      attributes.data());
  EXPECT_EQ(written, glyphs);
  EXPECT_EQ(positions.back(), Point(0, 0)) << "wrote past the frame";

  std::vector<ResolvedQuad> quads;
  for (uint32_t i = 0; i < written; i++) {
    const Point* p = positions.data() + i * 4;
    const Attributes* a = attributes.data() + i * 4;
    quads.push_back(ResolvedQuad{
        .position = Rect::MakeLTRB(p[0].x, p[0].y, p[3].x, p[3].y),
        .uv = Rect::MakeLTRB(a[0].uv.x, a[0].uv.y, a[3].uv.x, a[3].uv.y),
        .color = a[0].color,
        .paint = a[0].paint,
    });
  }
  return quads;
}

TEST_F(TextMaterializerTest, ResolveGlyphsGivesOneQuadPerGlyph) {
  const std::vector<ResolvedQuad> quads =
      Resolve(*materializer_, *MakeFrame(3), Point(0, 20), Matrix());

  ASSERT_EQ(quads.size(), 3u);
  for (const ResolvedQuad& quad : quads) {
    EXPECT_FALSE(quad.position.IsEmpty());
    // Normalized into the page, so a uv outside the unit square would be
    // sampling something else entirely.
    EXPECT_GE(quad.uv.GetLeft(), 0.0f);
    EXPECT_LE(quad.uv.GetRight(), 1.0f);
  }
  // Laid out along the run, in order.
  EXPECT_LT(quads[0].position.GetLeft(), quads[1].position.GetLeft());
  EXPECT_LT(quads[1].position.GetLeft(), quads[2].position.GetLeft());
}

TEST_F(TextMaterializerTest, ResolvingQueuesTheAtlasUploadItNeeds) {
  EXPECT_FALSE(atlas_->HasPendingUploads());

  Resolve(*materializer_, *MakeFrame(2), Point(0, 0), Matrix());

  // Rasterized and placed, waiting on the blit the dispatch records.
  EXPECT_EQ(rasterizer_->rasterize_count, 2);
  EXPECT_TRUE(atlas_->HasPendingUploads());

  StubGpuCommandBuffer cmd_buffer;
  materializer_->FlushAtlas(cmd_buffer);
  EXPECT_FALSE(atlas_->HasPendingUploads());
  EXPECT_FALSE(cmd_buffer.updates.empty());
}

TEST_F(TextMaterializerTest, ResolvingAGlyphTwiceRasterizesItOnce) {
  const std::vector<ResolvedQuad> first =
      Resolve(*materializer_, *MakeFrame(3), Point(0, 0), Matrix());
  const int after_first = rasterizer_->rasterize_count;

  const std::vector<ResolvedQuad> second =
      Resolve(*materializer_, *MakeFrame(3), Point(0, 0), Matrix());

  // The second frame is all cache hits, which is what the atlas is for.
  EXPECT_EQ(rasterizer_->rasterize_count, after_first);
  ASSERT_EQ(second.size(), first.size());
  EXPECT_EQ(second[0].uv, first[0].uv);
}

TEST_F(TextMaterializerTest, AGlyphWithNoCoverageResolvesEmpty) {
  // Glyph index 0 is the whitespace-like one the fake gives no coverage.
  Font font(std::make_shared<FakeTypeface>(), Font::Metrics{},
            AxisAlignment::kNone);
  TextRun run(font);
  run.AddGlyph(Glyph(0, Glyph::Type::kPath), Point(0, 0));
  run.AddGlyph(Glyph(1, Glyph::Type::kPath), Point(10, 0));
  std::vector<TextRun> runs = {run};
  TextFrame frame(runs, Rect::MakeLTRB(0, -10, 20, 0), /*has_color=*/false);

  const std::vector<ResolvedQuad> quads =
      Resolve(*materializer_, frame, Point(0, 0), Matrix());

  // Still one quad per glyph, so a caller can walk them together.
  ASSERT_EQ(quads.size(), 2u);
  EXPECT_TRUE(quads[0].position.IsEmpty());
  EXPECT_FALSE(quads[1].position.IsEmpty());
}

TEST_F(TextMaterializerTest, EveryGlyphVertexCarriesTheDrawsColorAndPaint) {
  const std::vector<ResolvedQuad> quads = Resolve(
      *materializer_, *MakeFrame(2), Point(0, 0), Matrix(), 0xFF00FF00, 7);

  ASSERT_EQ(quads.size(), 2u);
  for (const ResolvedQuad& quad : quads) {
    EXPECT_EQ(quad.color, 0xFF00FF00u);
    EXPECT_EQ(quad.paint, 7u);
  }
}

}  // namespace testing
}  // namespace impeller
