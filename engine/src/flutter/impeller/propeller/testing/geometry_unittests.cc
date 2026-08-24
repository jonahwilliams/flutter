// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <array>
#include <string>

#include "flutter/display_list/dl_paint.h"
#include "flutter/display_list/dl_vertices.h"
#include "flutter/display_list/geometry/dl_path.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/testing/testing.h"
#include "impeller/geometry/geometry_asserts.h"
#include "impeller/propeller/geometry.h"
#include "impeller/propeller/testing/stub_gpu_context.h"

namespace impeller {
namespace testing {

namespace {

/// What the frame gives a generator. Nothing here draws text or an
/// image unless it says so.
GeometryContext NoFrame() {
  return GeometryContext{};
}

/// A position no generator would write, so anything left holding it was
/// not touched.
constexpr Point kUntouched = Point(-9999, -9999);

/// Room for one draw, with slack past what it asks for so an overrun
/// shows up as a changed canary rather than as memory corruption.
struct Written {
  Written() { positions.fill(kUntouched); }

  std::array<Point, 512> positions = {};
  std::array<Attributes, 512> attributes = {};
  std::array<uint16_t, 512> indices = {};
  PrPaint paint;
};

/// The sign of each triangle's signed area, which is the facing the
/// winding accumulator reads.
std::vector<int> TriangleFacings(const Written& out, uint32_t index_count) {
  std::vector<int> facings;
  for (uint32_t i = 0; i + 2 < index_count; i += 3) {
    const Point a = out.positions[out.indices[i]];
    const Point b = out.positions[out.indices[i + 1]];
    const Point c = out.positions[out.indices[i + 2]];
    const Scalar area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    facings.push_back(area > 0 ? 1 : (area < 0 ? -1 : 0));
  }
  return facings;
}

/// The Loop-Blinn class the vertex stage will look this vertex's
/// implicit coordinate up by.
uint8_t ImplicitClass(const Attributes& attributes) {
  return static_cast<uint8_t>(attributes.paint >> kPaintFlagsShift);
}

flutter::DlPaint Fill(flutter::DlColor color) {
  return flutter::DlPaint().setColor(color);
}

/// The same shape as a convex path, which is what the walker takes.
std::shared_ptr<PrPicture> RecordConvexPath(const RoundRect& rrect,
                                            flutter::DlColor color) {
  PrPictureBuilder builder;
  builder.DrawPath(flutter::DlPath::MakeRoundRect(rrect), Fill(color));
  return builder.Build();
}

std::shared_ptr<PrPicture> RecordConvexPath(const Rect& bounds,
                                            Scalar radius,
                                            flutter::DlColor color) {
  return RecordConvexPath(RoundRect::MakeRectXY(bounds, radius, radius), color);
}

std::shared_ptr<PrPicture> RecordRoundRect(const Rect& bounds,
                                           Scalar radius,
                                           flutter::DlColor color) {
  PrPictureBuilder builder;
  builder.DrawRoundRect(RoundRect::MakeRectXY(bounds, radius, radius),
                        Fill(color));
  return builder.Build();
}

}  // namespace

TEST(GeometryGeneratorTest, ColoursAreCarriedAsPremultipliedRgba8) {
  // Red in the low byte, alpha in the high one: what the vertex stage
  // unpacks with `c & 0xFF` as red.
  EXPECT_EQ(flutter::DlColor::kRed().premultipliedRGBA(), 0xFF0000FFu);
  EXPECT_EQ(flutter::DlColor::kGreen().premultipliedRGBA(), 0xFF00FF00u);
  EXPECT_EQ(flutter::DlColor::kBlue().premultipliedRGBA(), 0xFFFF0000u);
  EXPECT_EQ(flutter::DlColor::kWhite().premultipliedRGBA(), 0xFFFFFFFFu);
  EXPECT_EQ(flutter::DlColor::kTransparent().premultipliedRGBA(), 0x00000000u);

  // Premultiplied: half alpha halves the colour with it.
  const uint32_t half_red =
      flutter::DlColor::kRed().withAlphaF(0.5).premultipliedRGBA();
  EXPECT_EQ(half_red >> 24, 128u) << "alpha";
  EXPECT_EQ(half_red & 0xFF, 128u) << "red, scaled by the alpha";
  EXPECT_EQ((half_red >> 8) & 0xFFFF, 0u) << "green and blue";
}

TEST(GeometryGeneratorTest, PackPaintSplitsIndexFromFlags) {
  EXPECT_EQ(PackPaint(0), 0u);
  EXPECT_EQ(PackPaint(1234), 1234u);
  EXPECT_EQ(PackPaint(kPaintIndexMask), kPaintIndexMask);
  // The class the vertex stage picks a Loop-Blinn coordinate with rides
  // above the index rather than in it.
  EXPECT_EQ(PackPaint(7, 3), 7u | (3u << 24));
  EXPECT_EQ(PackPaint(7, 3) & kPaintIndexMask, 7u);
}

// -----------------------------------------------------------------------
// Rects.

TEST(GeometryGeneratorTest, RectAsksForAQuad) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  RectGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());

  EXPECT_EQ(vertices, 4u);
  EXPECT_EQ(indices, 6u);
}

TEST(GeometryGeneratorTest, ARectSquareToThePixelGridIsJustAQuad) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // Scale and translation leave a rect's edges on the axes, where the
  // rasterizer's own coverage is exact and a ramp would only blur them.
  RectGeometryGenerator generator;
  auto [vertices, indices] = generator.GetAllocationCount(
      *picture, picture->GetDraws()[0],
      Matrix::MakeScale({3, 2, 1}) * Matrix::MakeTranslation({7, 9, 0}));

  EXPECT_EQ(vertices, 4u);
  EXPECT_EQ(indices, 6u);
}

TEST(GeometryGeneratorTest, EveryTriangleOfARectFacesTheSameWay) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // A clip accumulates its mesh into the winding buffer, which signs
  // coverage by facing: halves that disagree cancel along the diagonal
  // they share instead of summing to one.
  RectGeometryGenerator generator;
  Written quad;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     quad.positions.data(), quad.attributes.data(),
                     quad.indices.data(), &quad.paint, 0, 0, NoFrame());
  const std::vector<int> halves = TriangleFacings(quad, 6);
  ASSERT_EQ(halves.size(), 2u);
  EXPECT_EQ(halves[0], halves[1]);
  EXPECT_NE(halves[0], 0) << "neither half is degenerate";

  const Matrix turned = Matrix::MakeRotationZ(Radians(0.3f));
  Written feathered;
  generator.Generate(*picture, picture->GetDraws()[0], turned,
                     feathered.positions.data(), feathered.attributes.data(),
                     feathered.indices.data(), &feathered.paint, 0, 0,
                     NoFrame());
  const std::vector<int> ring = TriangleFacings(feathered, 30);
  ASSERT_EQ(ring.size(), 10u);
  for (size_t i = 0; i < ring.size(); i++) {
    EXPECT_EQ(ring[i], ring[0]) << "triangle " << i;
  }
}

TEST(GeometryGeneratorTest, ATurnedRectIsFeatheredByADevicePixel) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  const Matrix turned = Matrix::MakeRotationZ(Radians(0.3f));
  Written out;
  RectGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], turned);
  EXPECT_EQ(vertices, 8u);
  EXPECT_EQ(indices, 30u);

  generator.Generate(*picture, picture->GetDraws()[0], turned,
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  // Half a pixel in and half a pixel out, so the ramp between them is a
  // pixel wide and crosses half coverage on the rect itself.
  EXPECT_POINT_NEAR(out.positions[0], Point(0.5, 0.5));
  EXPECT_POINT_NEAR(out.positions[2], Point(9.5, 9.5));
  EXPECT_POINT_NEAR(out.positions[4], Point(-0.5, -0.5));
  EXPECT_POINT_NEAR(out.positions[6], Point(10.5, 10.5));

  // The inner ring is the draw's colour and the outer one is nothing.
  // Premultiplied, so fading to zero fades the whole vertex.
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(out.attributes[i].color,
              flutter::DlColor::kRed().premultipliedRGBA())
        << "inner " << i;
    EXPECT_EQ(out.attributes[i + 4].color, 0u) << "outer " << i;
  }
}

TEST(GeometryGeneratorTest, AFeatherIsADevicePixelAtAnyScale) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // Drawn at four times the size, a device pixel is a quarter of a unit
  // of what was recorded.
  const Matrix turned =
      Matrix::MakeRotationZ(Radians(0.3f)) * Matrix::MakeScale({4, 4, 1});
  Written out;
  RectGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[0], turned,
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  EXPECT_POINT_NEAR(out.positions[0], Point(0.125, 0.125));
  EXPECT_POINT_NEAR(out.positions[4], Point(-0.125, -0.125));
}

TEST(GeometryGeneratorTest, AFeatherNeverCrossesItsOwnRect) {
  PrPictureBuilder builder;
  // Thinner than the ramp it would be given.
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 0.4f),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  Written out;
  RectGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[0],
                     Matrix::MakeRotationZ(Radians(0.3f)), out.positions.data(),
                     out.attributes.data(), out.indices.data(), &out.paint, 0,
                     0, NoFrame());

  // The inner ring collapses onto the middle rather than turning inside
  // out.
  EXPECT_NEAR(out.positions[0].y, 0.2f, 1e-4f);
  EXPECT_NEAR(out.positions[2].y, 0.2f, 1e-4f);
}

TEST(GeometryGeneratorTest, RectWritesItsCorners) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(10, 20, 30, 50),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  Written out;
  RectGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  EXPECT_POINT_NEAR(out.positions[0], Point(10, 20));
  EXPECT_POINT_NEAR(out.positions[1], Point(10, 50));
  EXPECT_POINT_NEAR(out.positions[2], Point(30, 20));
  EXPECT_POINT_NEAR(out.positions[3], Point(30, 50));
  // Two triangles over those four corners, wound the same way.
  const std::array<uint16_t, 6> expected = {0, 1, 2, 1, 3, 2};
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(out.indices[i], expected[i]) << "index " << i;
  }
}

TEST(GeometryGeneratorTest, RectCarriesTheDrawsColourOnEveryVertex) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kGreen()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  Written out;
  RectGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 9, NoFrame());

  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(out.attributes[i].color,
              flutter::DlColor::kGreen().premultipliedRGBA());
    // Every vertex names the paint slot it was reserved at.
    EXPECT_EQ(out.attributes[i].paint, PackPaint(9)) << "vertex " << i;
    EXPECT_POINT_NEAR(out.attributes[i].uv, Point(0, 0));
  }
}

TEST(GeometryGeneratorTest, IndicesAreRelativeToTheAllocation) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  Written out;
  RectGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 100, 0, NoFrame());

  // The vertices landed 100 into the buffer, so the indices have to name
  // them there.
  const std::array<uint16_t, 6> expected = {100, 101, 102, 101, 103, 102};
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(out.indices[i], expected[i]) << "index " << i;
  }
}

TEST(GeometryGeneratorTest, PositionsAreLocalNotTransformed) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(10, 20, 30, 50),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  Written out;
  RectGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[0],
                     Matrix::MakeTranslation({1000, 1000, 0}),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  // The vertex stage maps positions through the transform the paint
  // names, so the matrix must not move them here as well.
  EXPECT_POINT_NEAR(out.positions[0], Point(10, 20));
  EXPECT_POINT_NEAR(out.positions[3], Point(30, 50));
}

// -----------------------------------------------------------------------
// Points.

TEST(GeometryGeneratorTest, PointsAskForAQuadEach) {
  PrPictureBuilder builder;
  std::array<Point, 3> points = {Point(10, 10), Point(20, 20), Point(30, 30)};
  builder.DrawPoints(flutter::DlPointMode::kPoints, points.size(),
                     points.data(),
                     Fill(flutter::DlColor::kRed()).setStrokeWidth(4));
  std::shared_ptr<PrPicture> picture = builder.Build();

  DrawPointsGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());

  EXPECT_EQ(vertices, 12u);
  EXPECT_EQ(indices, 18u);
}

TEST(GeometryGeneratorTest, EachPointIsAQuadAroundItsCentre) {
  PrPictureBuilder builder;
  std::array<Point, 2> points = {Point(10, 10), Point(100, 200)};
  builder.DrawPoints(flutter::DlPointMode::kPoints, points.size(),
                     points.data(),
                     Fill(flutter::DlColor::kRed()).setStrokeWidth(4));
  std::shared_ptr<PrPicture> picture = builder.Build();

  Written out;
  DrawPointsGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  // A stroke width of four is a radius of two about each centre.
  EXPECT_POINT_NEAR(out.positions[0], Point(8, 8));
  EXPECT_POINT_NEAR(out.positions[1], Point(8, 12));
  EXPECT_POINT_NEAR(out.positions[2], Point(12, 8));
  EXPECT_POINT_NEAR(out.positions[3], Point(12, 12));
  EXPECT_POINT_NEAR(out.positions[4], Point(98, 198));
  EXPECT_POINT_NEAR(out.positions[7], Point(102, 202));
}

TEST(GeometryGeneratorTest, EachPointsIndicesNameItsOwnQuad) {
  PrPictureBuilder builder;
  std::array<Point, 2> points = {Point(10, 10), Point(100, 200)};
  builder.DrawPoints(flutter::DlPointMode::kPoints, points.size(),
                     points.data(),
                     Fill(flutter::DlColor::kRed()).setStrokeWidth(4));
  std::shared_ptr<PrPicture> picture = builder.Build();

  Written out;
  DrawPointsGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 50, 0, NoFrame());

  // The first point's quad, then the second's four vertices along.
  const std::array<uint16_t, 12> expected = {50, 51, 52, 51, 53, 52,
                                             54, 55, 56, 55, 57, 56};
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(out.indices[i], expected[i]) << "index " << i;
  }
}

TEST(GeometryGeneratorTest, RoundPointsCarryTheirCornerInUv) {
  std::array<Point, 1> points = {Point(10, 10)};
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed()).setStrokeWidth(4);

  {
    PrPictureBuilder builder;
    paint.setStrokeCap(flutter::DlStrokeCap::kRound);
    builder.DrawPoints(flutter::DlPointMode::kPoints, points.size(),
                       points.data(), paint);
    std::shared_ptr<PrPicture> picture = builder.Build();

    Written out;
    DrawPointsGeometryGenerator generator;
    generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                       out.positions.data(), out.attributes.data(),
                       out.indices.data(), &out.paint, 0, 0, NoFrame());

    // What the corner covers is its distance from the centre.
    EXPECT_POINT_NEAR(out.attributes[0].uv, Point(-1, -1));
    EXPECT_POINT_NEAR(out.attributes[3].uv, Point(1, 1));
  }
  {
    PrPictureBuilder builder;
    paint.setStrokeCap(flutter::DlStrokeCap::kButt);
    builder.DrawPoints(flutter::DlPointMode::kPoints, points.size(),
                       points.data(), paint);
    std::shared_ptr<PrPicture> picture = builder.Build();

    Written out;
    DrawPointsGeometryGenerator generator;
    generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                       out.positions.data(), out.attributes.data(),
                       out.indices.data(), &out.paint, 0, 0, NoFrame());

    // A square point covers all of its quad, so there is nothing to read.
    EXPECT_POINT_NEAR(out.attributes[0].uv, Point(0, 0));
    EXPECT_POINT_NEAR(out.attributes[3].uv, Point(0, 0));
  }
}

TEST(GeometryGeneratorTest, PointsReadTheirCentresFromThePicture) {
  PrPictureBuilder builder;
  // A first draw so the second one's points do not start at offset zero.
  std::array<Point, 1> first = {Point(1, 1)};
  builder.DrawPoints(flutter::DlPointMode::kPoints, first.size(), first.data(),
                     Fill(flutter::DlColor::kRed()).setStrokeWidth(2));
  std::array<Point, 1> second = {Point(70, 80)};
  builder.DrawPoints(flutter::DlPointMode::kPoints, second.size(),
                     second.data(),
                     Fill(flutter::DlColor::kRed()).setStrokeWidth(2));
  std::shared_ptr<PrPicture> picture = builder.Build();

  Written out;
  DrawPointsGeometryGenerator generator;
  generator.Generate(*picture, picture->GetDraws()[1], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  // The draw names a run of the picture's point storage, so it has to
  // read from the offset it recorded and not from the start.
  ASSERT_EQ(picture->GetDraws()[1].point_data.offset, 1u);
  EXPECT_POINT_NEAR(out.positions[0], Point(69, 79));
  EXPECT_POINT_NEAR(out.positions[3], Point(71, 81));
}

// -----------------------------------------------------------------------
// Round rects.

TEST(GeometryGeneratorTest, ConvexPathWritesExactlyWhatItAskedFor) {
  std::shared_ptr<PrPicture> picture = RecordConvexPath(
      Rect::MakeLTRB(0, 0, 100, 60), 20, flutter::DlColor::kRed());

  ConvexPathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());

  ASSERT_GT(vertices, 0u);
  // A triangle soup, so there is one index per vertex and both are whole
  // triangles.
  EXPECT_EQ(vertices, indices);
  EXPECT_EQ(vertices % 3, 0u);
  ASSERT_LT(vertices, 512u) << "the test buffer is too small";

  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  // The count and the write are one walk apart, so a drift between them
  // is a buffer overrun in the dispatcher.
  EXPECT_NE(out.positions[vertices - 1], kUntouched) << "wrote fewer";
  EXPECT_EQ(out.positions[vertices], kUntouched) << "wrote more";
}

TEST(GeometryGeneratorTest, ConvexPathCountDoesNotDependOnTheMatrix) {
  std::shared_ptr<PrPicture> picture = RecordConvexPath(
      Rect::MakeLTRB(0, 0, 100, 60), 20, flutter::DlColor::kRed());
  const Draw& draw = picture->GetDraws()[0];

  ConvexPathGeometryGenerator generator;
  auto [small, unused_a] =
      generator.GetAllocationCount(*picture, draw, Matrix());
  auto [large, unused_b] = generator.GetAllocationCount(
      *picture, draw, Matrix::MakeScale({100.0f, 100.0f, 1.0f}));

  // The corners chop to a fixed number of quadratics by design, so the
  // size a draw comes out at does not change what it costs.
  EXPECT_EQ(small, large);
}

TEST(GeometryGeneratorTest, ConvexPathIndicesRunStraightFromTheAllocation) {
  std::shared_ptr<PrPicture> picture = RecordConvexPath(
      Rect::MakeLTRB(0, 0, 100, 60), 20, flutter::DlColor::kRed());

  ConvexPathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 40, 0, NoFrame());

  for (uint32_t i = 0; i < indices; i++) {
    ASSERT_EQ(out.indices[i], 40 + i) << "index " << i;
  }
}

TEST(GeometryGeneratorTest, ConvexPathCarriesItsColourAndPaintThroughout) {
  std::shared_ptr<PrPicture> picture = RecordConvexPath(
      Rect::MakeLTRB(0, 0, 100, 60), 20, flutter::DlColor::kGreen());

  ConvexPathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 5, NoFrame());

  for (uint32_t i = 0; i < vertices; i++) {
    ASSERT_EQ(out.attributes[i].color,
              flutter::DlColor::kGreen().premultipliedRGBA())
        << "vertex " << i;
    // The implicit class rides above the index, so the index the vertex
    // names has to survive it.
    ASSERT_EQ(out.attributes[i].paint & kPaintIndexMask, 5u) << "vertex " << i;
  }
}

TEST(GeometryGeneratorTest, ConvexPathCurvesAreLoopBlinnCurves) {
  std::shared_ptr<PrPicture> picture = RecordConvexPath(
      Rect::MakeLTRB(0, 0, 100, 60), 20, flutter::DlColor::kRed());

  ConvexPathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  std::array<int, 8> classes = {};
  for (uint32_t i = 0; i < vertices; i++) {
    classes[ImplicitClass(out.attributes[i])]++;
  }

  // The corners are quadratics, so the fragment stage gets the three
  // canonical coordinates...
  EXPECT_GT(classes[kPathImplicitCurveP0], 0);
  EXPECT_GT(classes[kPathImplicitCurveControl], 0);
  EXPECT_GT(classes[kPathImplicitCurveP1], 0);
  // ...over a saturated interior, with a fringe outside.
  EXPECT_GT(classes[kPathImplicitInterior], 0);
  EXPECT_GT(classes[kPathImplicitFringeOuter], 0);
  // A control and its endpoints come as whole triangles.
  EXPECT_EQ(classes[kPathImplicitCurveControl], classes[kPathImplicitCurveP1]);
}

TEST(GeometryGeneratorTest, AConvexPathOfOnlyLinesHasNoCurves) {
  std::shared_ptr<PrPicture> picture = RecordConvexPath(
      Rect::MakeLTRB(0, 0, 100, 60), 0, flutter::DlColor::kRed());

  ConvexPathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  for (uint32_t i = 0; i < vertices; i++) {
    const uint8_t implicit = ImplicitClass(out.attributes[i]);
    // With no radius there is no curve to be on the outside of.
    ASSERT_NE(implicit, kPathImplicitCurveControl) << "vertex " << i;
    ASSERT_NE(implicit, kPathImplicitCurveP1) << "vertex " << i;
  }
}

TEST(GeometryGeneratorTest, AConvexPathStaysWithinItsBoundsAndFringe) {
  const Rect bounds = Rect::MakeLTRB(10, 20, 110, 80);
  std::shared_ptr<PrPicture> picture =
      RecordConvexPath(bounds, 20, flutter::DlColor::kRed());

  ConvexPathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  // Everything the mesh covers is the shape plus the one unit of fringe
  // the coverage ramp needs.
  const Rect limit = bounds.Expand(1);
  for (uint32_t i = 0; i < vertices; i++) {
    ASSERT_TRUE(limit.ContainsInclusive(out.positions[i]))
        << "vertex " << i << " at " << out.positions[i];
  }
}

/// How far past `bounds` the mesh reaches, which is its AA fringe.
Scalar FringeUnder(const Matrix& matrix, const Rect& bounds) {
  std::shared_ptr<PrPicture> picture =
      RecordConvexPath(bounds, 20, flutter::DlColor::kRed());

  ConvexPathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], matrix);
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], matrix,
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  Scalar reach = 0;
  for (uint32_t i = 0; i < vertices; i++) {
    reach = std::max(reach, bounds.GetLeft() - out.positions[i].x);
    reach = std::max(reach, out.positions[i].x - bounds.GetRight());
    reach = std::max(reach, bounds.GetTop() - out.positions[i].y);
    reach = std::max(reach, out.positions[i].y - bounds.GetBottom());
  }
  return reach;
}

TEST(GeometryGeneratorTest, TheFringeIsOneDevicePixelWide) {
  const Rect bounds = Rect::MakeLTRB(10, 20, 110, 80);

  // Positions are local and the vertex stage scales them, so a fringe
  // written in local units would come out four pixels wide at 4x. It is
  // the device pixel that has to stay one wide.
  EXPECT_NEAR(FringeUnder(Matrix(), bounds), 1.0f, kEhCloseEnough);
  EXPECT_NEAR(FringeUnder(Matrix::MakeScale({4.0f, 4.0f, 1.0f}), bounds), 0.25f,
              kEhCloseEnough);
  EXPECT_NEAR(FringeUnder(Matrix::MakeScale({0.5f, 0.5f, 1.0f}), bounds), 2.0f,
              kEhCloseEnough);
  // The larger basis wins: the ramp has to cover the axis that is
  // magnified least in local terms.
  EXPECT_NEAR(FringeUnder(Matrix::MakeScale({4.0f, 1.0f, 1.0f}), bounds), 0.25f,
              kEhCloseEnough);
}

TEST(GeometryGeneratorTest, ConvexPathPositionsAreLocalNotTransformed) {
  const Rect bounds = Rect::MakeLTRB(10, 20, 110, 80);
  std::shared_ptr<PrPicture> picture =
      RecordConvexPath(bounds, 20, flutter::DlColor::kRed());

  ConvexPathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0],
                     Matrix::MakeTranslation({1000, 1000, 0}),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  const Rect limit = bounds.Expand(1);
  for (uint32_t i = 0; i < vertices; i++) {
    ASSERT_TRUE(limit.ContainsInclusive(out.positions[i])) << "vertex " << i;
  }
}

// -----------------------------------------------------------------------
// Round rects off the static table.

namespace {

/// One triangle of a mesh: where its corners are and what coverage each
/// carries.
struct Triangle {
  std::array<Point, 3> positions;
  std::array<uint8_t, 3> classes;
};

bool IsSameTriangle(const Triangle& a, const Triangle& b) {
  for (int corner = 0; corner < 3; corner++) {
    if (a.positions[corner].GetDistance(b.positions[corner]) > 1e-3f) {
      return false;
    }
  }
  return true;
}

bool IsDegenerate(const Triangle& triangle) {
  const Point a = triangle.positions[1] - triangle.positions[0];
  const Point b = triangle.positions[2] - triangle.positions[0];
  return std::abs(a.Cross(b)) < 1e-3f;
}

/// The same ring of triangles, starting where `first` does. A path
/// begins its contour wherever the path stored it, so two meshes of one
/// shape can be the same ring from a different starting edge.
std::vector<Triangle> StartingAt(const std::vector<Triangle>& triangles,
                                 const Triangle& first) {
  size_t offset = 0;
  while (offset < triangles.size() &&
         !IsSameTriangle(triangles[offset], first)) {
    offset++;
  }
  EXPECT_LT(offset, triangles.size()) << "nothing in the ring starts it";
  if (offset >= triangles.size()) {
    return triangles;
  }
  std::vector<Triangle> rotated(triangles.begin() + offset, triangles.end());
  rotated.insert(rotated.end(), triangles.begin(), triangles.begin() + offset);
  return rotated;
}

/// A generator's mesh, with the triangles that cover nothing dropped:
/// what a shape's degeneracies skip and what the table keeps are the
/// same nothing, so this is what the two are comparable over.
std::vector<Triangle> CoveringTriangles(GeometryGenerator& generator,
                                        const PrPicture& picture,
                                        const Matrix& matrix) {
  const Draw& draw = picture.GetDraws()[0];
  auto [vertices, indices] =
      generator.GetAllocationCount(picture, draw, matrix);
  EXPECT_LT(vertices, 512u) << "the test buffer is too small";

  Written out;
  generator.Generate(picture, draw, matrix, out.positions.data(),
                     out.attributes.data(), out.indices.data(), &out.paint, 0,
                     0, NoFrame());

  std::vector<Triangle> triangles;
  for (uint32_t i = 0; i + 2 < vertices; i += 3) {
    Triangle triangle;
    for (int corner = 0; corner < 3; corner++) {
      triangle.positions[corner] = out.positions[i + corner];
      triangle.classes[corner] = ImplicitClass(out.attributes[i + corner]);
    }
    if (!IsDegenerate(triangle)) {
      triangles.push_back(triangle);
    }
  }
  return triangles;
}

}  // namespace

TEST(GeometryGeneratorTest, RoundRectIsTheMeshWalkingTheContourGives) {
  std::shared_ptr<PrPicture> picture = RecordRoundRect(
      Rect::MakeLTRB(10, 20, 110, 80), 20, flutter::DlColor::kRed());

  RRectGeometryGenerator table;
  ConvexPathGeometryGenerator walk;
  const std::vector<Triangle> from_table =
      CoveringTriangles(table, *picture, Matrix());
  const std::vector<Triangle> from_walk =
      CoveringTriangles(walk,
                        *RecordConvexPath(Rect::MakeLTRB(10, 20, 110, 80), 20,
                                          flutter::DlColor::kRed()),
                        Matrix());

  // The table is the same mesh, so what it draws has to be triangle for
  // triangle what walking the contour draws.
  ASSERT_EQ(from_table.size(), from_walk.size());
  const std::vector<Triangle> aligned = StartingAt(from_walk, from_table[0]);
  for (size_t i = 0; i < aligned.size(); i++) {
    for (int corner = 0; corner < 3; corner++) {
      EXPECT_POINT_NEAR(from_table[i].positions[corner],
                        aligned[i].positions[corner])
          << "triangle " << i << " corner " << corner;
      EXPECT_EQ(from_table[i].classes[corner], aligned[i].classes[corner])
          << "triangle " << i << " corner " << corner;
    }
  }
}

TEST(GeometryGeneratorTest, ACornerWithNoRadiusLosesItsWedgeOfFringe) {
  const Rect bounds = Rect::MakeLTRB(10, 20, 110, 80);
  const RoundRect rrect =
      RoundRect::MakeRectRadii(bounds, RoundingRadii{
                                           .top_left = Size(20, 20),
                                           .top_right = Size(20, 20),
                                           .bottom_left = Size(20, 20),
                                           .bottom_right = Size(),
                                       });
  PrPictureBuilder builder;
  builder.DrawRoundRect(rrect, Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  RRectGeometryGenerator table;
  ConvexPathGeometryGenerator walk;
  const std::vector<Triangle> from_table =
      CoveringTriangles(table, *picture, Matrix());
  const std::vector<Triangle> from_walk = CoveringTriangles(
      walk, *RecordConvexPath(rrect, flutter::DlColor::kRed()), Matrix());

  // A sharp corner turns its whole 90 degrees at a point, and what
  // covers that turn is the wedge between the two edges' normals. The
  // table's wedges collapse with the arc they belong to, so the sharp
  // corner is the one place the two meshes differ -- and the one
  // triangle it costs is the pixel of fringe outside that corner.
  ASSERT_EQ(from_table.size() + 1, from_walk.size());

  size_t table_index = 0;
  int dropped = 0;
  for (const Triangle& walked : StartingAt(from_walk, from_table[0])) {
    if (table_index < from_table.size() &&
        IsSameTriangle(walked, from_table[table_index])) {
      table_index++;
      continue;
    }
    dropped++;
    for (const Point& position : walked.positions) {
      EXPECT_LT(position.GetDistance(bounds.GetRightBottom()), 2.0f)
          << "the wedge belongs to the sharp corner";
    }
  }
  EXPECT_EQ(dropped, 1);
  EXPECT_EQ(table_index, from_table.size());
}

TEST(GeometryGeneratorTest, ARoundRectWithNoRoundingIsRecordedAsARect) {
  PrPictureBuilder builder;
  builder.DrawRoundRect(
      RoundRect::MakeRectXY(Rect::MakeLTRB(10, 20, 110, 80), 0, 0),
      Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // Two orders of magnitude of geometry to say a rect is a rect.
  ASSERT_EQ(picture->GetDraws().size(), 1u);
  EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kRect);
}

TEST(GeometryGeneratorTest, ARoundRectAlwaysCostsTheSame) {
  RRectGeometryGenerator generator;
  const Rect bounds = Rect::MakeLTRB(0, 0, 100, 60);

  std::pair<uint32_t, uint32_t> first;
  for (Scalar radius : {0.0f, 1.0f, 20.0f, 30.0f}) {
    std::shared_ptr<PrPicture> picture =
        RecordRoundRect(bounds, radius, flutter::DlColor::kRed());
    const auto counts = generator.GetAllocationCount(
        *picture, picture->GetDraws()[0],
        Matrix::MakeScale({radius + 1, 1.0f, 1.0f}));
    if (radius == 0.0f) {
      first = counts;
      EXPECT_GT(first.first, 0u);
    }
    // The table is the same table whatever the shape, which is what
    // makes the count a lookup rather than a walk.
    EXPECT_EQ(counts, first) << "radius " << radius;
  }
}

TEST(GeometryGeneratorTest, ARoundRectWritesExactlyWhatItAskedFor) {
  std::shared_ptr<PrPicture> picture = RecordRoundRect(
      Rect::MakeLTRB(0, 0, 100, 60), 20, flutter::DlColor::kGreen());

  RRectGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  EXPECT_EQ(vertices, indices);
  ASSERT_LT(vertices, 512u) << "the test buffer is too small";

  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 7, 5, NoFrame());

  EXPECT_NE(out.positions[vertices - 1], kUntouched) << "wrote fewer";
  EXPECT_EQ(out.positions[vertices], kUntouched) << "wrote more";

  for (uint32_t i = 0; i < vertices; i++) {
    ASSERT_EQ(out.attributes[i].color,
              flutter::DlColor::kGreen().premultipliedRGBA())
        << "vertex " << i;
    // The implicit class rides above the index, so the index the vertex
    // names has to survive it.
    ASSERT_EQ(out.attributes[i].paint & kPaintIndexMask, 5u) << "vertex " << i;
    // A triangle soup, indexed straight off the allocation.
    ASSERT_EQ(out.indices[i], 7 + i) << "vertex " << i;
  }
}

TEST(GeometryGeneratorTest, ARoundRectsFringeIsADevicePixelWhateverTheScale) {
  const Rect bounds = Rect::MakeLTRB(0, 0, 100, 60);
  std::shared_ptr<PrPicture> picture =
      RecordRoundRect(bounds, 20, flutter::DlColor::kRed());

  RRectGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());

  for (Scalar scale : {1.0f, 4.0f}) {
    Written out;
    generator.Generate(*picture, picture->GetDraws()[0],
                       Matrix::MakeScale({scale, scale, 1.0f}),
                       out.positions.data(), out.attributes.data(),
                       out.indices.data(), &out.paint, 0, 0, NoFrame());

    // The fringe is a pixel of the device, so in the draw's own
    // coordinates it is a pixel divided by the scale it lands under.
    const Scalar fringe = 1.0f / scale;
    Scalar reach = 0.0f;
    for (uint32_t i = 0; i < vertices; i++) {
      reach = std::max(reach, bounds.GetLeft() - out.positions[i].x);
      reach = std::max(reach, out.positions[i].x - bounds.GetRight());
    }
    EXPECT_NEAR(reach, fringe, 1e-4f) << "scale " << scale;
  }
}

// -----------------------------------------------------------------------
// Concave fills.

namespace {

/// A chevron: concave, and every edge straight, so the whole mesh is
/// fans and fringe.
std::shared_ptr<PrPicture> RecordChevron(flutter::DlColor color) {
  const Point points[4] = {Point(10, 10), Point(50, 40), Point(90, 10),
                           Point(50, 90)};
  PrPictureBuilder builder;
  builder.DrawPath(flutter::DlPath::MakePoly(points, 4, /*close=*/true),
                   Fill(color));
  return builder.Build();
}

Scalar SignedArea(const std::array<Point, 3>& triangle) {
  return (triangle[1] - triangle[0]).Cross(triangle[2] - triangle[0]);
}

}  // namespace

TEST(GeometryGeneratorTest, AConcaveFringeFacesTheWayItsFanDoes) {
  std::shared_ptr<PrPicture> picture = RecordChevron(flutter::DlColor::kRed());
  ASSERT_EQ(picture->GetDraws()[0].type,
            Draw::DrawType::kConcaveWindingAccumulate);

  ConcavePathGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  ASSERT_LT(vertices, 512u) << "the test buffer is too small";
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  // The accumulator reads winding from which way a triangle faces, so
  // the fringe has to face the way its fan does -- its natural winding
  // is the other one, and a fringe that subtracts eats the edge it is
  // supposed to soften.
  Scalar fans = 0;
  Scalar fringe = 0;
  for (uint32_t i = 0; i + 2 < vertices; i += 3) {
    const std::array<Point, 3> triangle = {
        out.positions[i], out.positions[i + 1], out.positions[i + 2]};
    bool outer = false;
    bool interior = false;
    for (int corner = 0; corner < 3; corner++) {
      const uint8_t klass = ImplicitClass(out.attributes[i + corner]);
      outer = outer || klass == kPathImplicitFringeOuter;
      interior = interior || klass == kPathImplicitInterior;
    }
    if (outer) {
      fringe += SignedArea(triangle);
    } else if (interior) {
      fans += SignedArea(triangle);
    }
  }
  ASSERT_NE(fans, 0.0f);
  ASSERT_NE(fringe, 0.0f);
  EXPECT_EQ(fans < 0, fringe < 0) << "the fringe winds against its fans";
}

// -----------------------------------------------------------------------
// Vertex buffers.

namespace {

std::shared_ptr<PrPicture> RecordVertices(
    const std::shared_ptr<flutter::DlVertices>& vertices,
    flutter::DlColor color) {
  PrPictureBuilder builder;
  builder.DrawVertices(vertices, flutter::DlBlendMode::kSrcOver, Fill(color));
  return builder.Build();
}

const Point kTriangle[3] = {Point(0, 0), Point(30, 0), Point(0, 40)};

}  // namespace

TEST(GeometryGeneratorTest, VerticesComeAcrossAsTheyWereGiven) {
  std::shared_ptr<PrPicture> picture = RecordVertices(
      flutter::DlVertices::Make(flutter::DlVertexMode::kTriangles, 3, kTriangle,
                                /*texture_coordinates=*/nullptr,
                                /*colors=*/nullptr),
      flutter::DlColor::kGreen());

  VerticesGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  EXPECT_EQ(vertices, 3u);
  EXPECT_EQ(indices, 3u) << "no indices, so every vertex once";

  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 7, 5, NoFrame());

  for (int i = 0; i < 3; i++) {
    EXPECT_POINT_NEAR(out.positions[i], kTriangle[i]);
    // Nothing carries a colour of its own, so the paint's is used.
    EXPECT_EQ(out.attributes[i].color,
              flutter::DlColor::kGreen().premultipliedRGBA());
    EXPECT_EQ(out.attributes[i].paint & kPaintIndexMask, 5u);
    EXPECT_EQ(out.indices[i], 7 + i) << "relative to the allocation";
  }
}

TEST(GeometryGeneratorTest, VerticesKeepTheirOwnColoursAndCoordinates) {
  const Point uvs[3] = {Point(0, 0), Point(1, 0), Point(0, 1)};
  const flutter::DlColor colors[3] = {flutter::DlColor::kRed(),
                                      flutter::DlColor::kGreen(),
                                      flutter::DlColor::kBlue()};
  std::shared_ptr<PrPicture> picture = RecordVertices(
      flutter::DlVertices::Make(flutter::DlVertexMode::kTriangles, 3, kTriangle,
                                uvs, colors),
      flutter::DlColor::kWhite());

  VerticesGeometryGenerator generator;
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, NoFrame());

  for (int i = 0; i < 3; i++) {
    EXPECT_POINT_NEAR(out.attributes[i].uv, uvs[i]);
    EXPECT_EQ(out.attributes[i].color, colors[i].premultipliedRGBA())
        << "vertex " << i;
  }
}

TEST(GeometryGeneratorTest, IndexedVerticesAreRebasedOntoTheAllocation) {
  const uint16_t source[3] = {2, 0, 1};
  std::shared_ptr<PrPicture> picture = RecordVertices(
      flutter::DlVertices::Make(flutter::DlVertexMode::kTriangles, 3, kTriangle,
                                /*texture_coordinates=*/nullptr,
                                /*colors=*/nullptr, 3, source),
      flutter::DlColor::kGreen());

  VerticesGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  EXPECT_EQ(vertices, 3u);
  EXPECT_EQ(indices, 3u);

  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 12, 0, NoFrame());

  // The buffer's own order, but naming where the vertices actually
  // landed: an index is into the arena, not into the vertices.
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(out.indices[i], 12 + source[i]);
  }
}

// -----------------------------------------------------------------------
// Atlases.

namespace {

/// An image of a given size. What is in it is the device's business;
/// what these tests read is which slot names it and what samples it.
class TestImage final : public flutter::DlImage {
 public:
  TestImage(int32_t width, int32_t height) : size_(width, height) {}

  Type GetImageType() const override { return Type::kImpeller; }
  bool isTextureBacked() const override { return true; }
  flutter::DlColorSpace GetColorSpace() const override {
    return flutter::DlColorSpace::kSRGB;
  }
  bool isOpaque() const override { return true; }
  bool isUIThreadSafe() const override { return true; }
  flutter::DlISize GetSize() const override { return size_; }
  size_t GetApproximateByteSize() const override { return 4; }

 private:
  const flutter::DlISize size_;
};

sk_sp<flutter::DlImage> MakeTestImage(int32_t width, int32_t height) {
  return sk_make_sp<TestImage>(width, height);
}

}  // namespace

TEST(GeometryGeneratorTest, AnAtlasIsAQuadPerSpriteOutOfOneImage) {
  StubGpuContext context;
  const sk_sp<flutter::DlImage> image = MakeTestImage(100, 50);
  const RSTransform transforms[2] = {
      RSTransform(1, 0, 10, 20),  // Unrotated, at (10, 20).
      RSTransform(0, 1, 60, 20),  // A quarter turn, at (60, 20).
  };
  const Rect textures[2] = {Rect::MakeLTRB(0, 0, 25, 25),
                            Rect::MakeLTRB(50, 0, 100, 50)};

  PrPictureBuilder builder;
  builder.DrawAtlas(image, transforms, textures, /*colors=*/nullptr, 2,
                    flutter::DlBlendMode::kSrcOver,
                    flutter::DlImageSampling::kNearestNeighbor,
                    /*cull_rect=*/nullptr, nullptr);
  std::shared_ptr<PrPicture> picture = builder.Build();
  ASSERT_EQ(picture->GetDraws().size(), 1u);

  AtlasGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  EXPECT_EQ(vertices, 8u) << "a quad each";
  EXPECT_EQ(indices, 12u);

  std::vector<GPUTexture*> textures_bound;
  const GeometryContext frame{.context = &context, .textures = &textures_bound};
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, frame);

  // One image, so one slot however many sprites come out of it.
  ASSERT_EQ(textures_bound.size(), 1u);
  EXPECT_EQ(textures_bound[0], context.GetDLImageTexture(image));
  EXPECT_EQ(out.paint.texture_index, 0);
  EXPECT_NE(out.paint.flags & kPaintFlagSampleNearest, 0u);

  // The first sprite: unrotated, so its quad is its source rect at the
  // transform's origin.
  EXPECT_POINT_NEAR(out.positions[0], Point(10, 20));
  EXPECT_POINT_NEAR(out.positions[3], Point(35, 45));
  // Cutting out the top left quarter of a 100 by 50 image.
  EXPECT_POINT_NEAR(out.attributes[0].uv, Point(0, 0));
  EXPECT_POINT_NEAR(out.attributes[3].uv, Point(0.25, 0.5));

  // The second is turned a quarter, so its quad is not its source rect.
  EXPECT_POINT_NEAR(out.positions[4], Point(60, 20));
  EXPECT_POINT_NEAR(out.positions[7], Point(10, 70));
  EXPECT_POINT_NEAR(out.attributes[4].uv, Point(0.5, 0));
  EXPECT_POINT_NEAR(out.attributes[7].uv, Point(1, 1));

  // Two quads, indexed off the allocation.
  EXPECT_EQ(out.indices[0], 0);
  EXPECT_EQ(out.indices[6], 4);
}

TEST(GeometryGeneratorTest, AnAtlasSpriteIsTintedByItsOwnColour) {
  StubGpuContext context;
  const sk_sp<flutter::DlImage> image = MakeTestImage(100, 50);
  const RSTransform transforms[2] = {RSTransform(1, 0, 0, 0),
                                     RSTransform(1, 0, 30, 0)};
  const Rect textures[2] = {Rect::MakeLTRB(0, 0, 25, 25),
                            Rect::MakeLTRB(0, 0, 25, 25)};
  const flutter::DlColor colors[2] = {flutter::DlColor::kRed(),
                                      flutter::DlColor::kBlue()};

  PrPictureBuilder builder;
  builder.DrawAtlas(image, transforms, textures, colors, 2,
                    flutter::DlBlendMode::kSrcOver,
                    flutter::DlImageSampling::kLinear,
                    /*cull_rect=*/nullptr, nullptr);
  std::shared_ptr<PrPicture> picture = builder.Build();

  AtlasGeometryGenerator generator;
  std::vector<GPUTexture*> textures_bound;
  const GeometryContext frame{.context = &context, .textures = &textures_bound};
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, frame);

  for (int corner = 0; corner < 4; corner++) {
    EXPECT_EQ(out.attributes[corner].color,
              flutter::DlColor::kRed().premultipliedRGBA());
    EXPECT_EQ(out.attributes[4 + corner].color,
              flutter::DlColor::kBlue().premultipliedRGBA());
  }
}

// -----------------------------------------------------------------------
// Shadows.

namespace {

/// A shadow over a rect occluder, recorded the way the canvas does.
std::shared_ptr<PrPicture> RecordShadow(const Rect& occluder,
                                        Scalar elevation,
                                        bool transparent_occluder) {
  PrPictureBuilder builder;
  builder.DrawShadow(flutter::DlPath::MakeRect(occluder),
                     flutter::DlColor::kBlack(), elevation,
                     transparent_occluder, /*dpr=*/1);
  return builder.Build();
}

/// The alpha every vertex of the mesh carries, in the order the two
/// halves are drawn.
std::vector<uint8_t> HalfAlphas(const Written& out, uint32_t vertices) {
  std::vector<uint8_t> alphas;
  for (uint32_t i = 0; i < vertices; i++) {
    const uint8_t alpha = out.attributes[i].color >> 24;
    if (alphas.empty() || alphas.back() != alpha) {
      alphas.push_back(alpha);
    }
  }
  return alphas;
}

}  // namespace

TEST(GeometryGeneratorTest, AShadowIsAnAmbientAndASpotMesh) {
  StubGpuContext context;
  StubGpuTexture lut(TextureDesc{.width = 256, .height = 1});
  std::shared_ptr<PrPicture> picture =
      RecordShadow(Rect::MakeLTRB(20, 20, 60, 50), 4,
                   /*transparent_occluder=*/true);

  ShadowGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  // A fan and a ring for each half of a four-sided silhouette.
  EXPECT_EQ(vertices, 4u * 9u * 2u);
  EXPECT_EQ(indices, vertices);
  ASSERT_LT(vertices, 512u) << "the test buffer is too small";

  std::vector<GPUTexture*> textures;
  const GeometryContext frame{
      .context = &context, .shadow_lut = &lut, .textures = &textures};
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, frame);

  // Read off the ramp as coverage, the way a glyph is read off the
  // atlas.
  ASSERT_EQ(textures.size(), 1u);
  EXPECT_EQ(textures[0], &lut);
  EXPECT_EQ(out.paint.texture_index, 0);
  EXPECT_NE(out.paint.flags & kPaintFlagTextureIsCoverage, 0u);

  // Two halves, the ambient faint and the spot dark. Both are cut from
  // the same colour, so an elevation reads as one shadow.
  const std::vector<uint8_t> alphas = HalfAlphas(out, vertices);
  ASSERT_EQ(alphas.size(), 2u);
  EXPECT_EQ(alphas[0], static_cast<uint8_t>(std::round(255 * 0.039f)));
  EXPECT_EQ(alphas[1], static_cast<uint8_t>(std::round(255 * 0.25f)));
}

TEST(GeometryGeneratorTest, AnOpaqueOccluderSkipsWhatItHides) {
  StubGpuContext context;
  StubGpuTexture lut(TextureDesc{.width = 256, .height = 1});
  const Rect occluder = Rect::MakeLTRB(20, 20, 60, 50);

  ShadowGeometryGenerator generator;
  const uint32_t transparent =
      generator
          .GetAllocationCount(*RecordShadow(occluder, 4, true),
                              RecordShadow(occluder, 4, true)->GetDraws()[0],
                              Matrix())
          .first;
  std::shared_ptr<PrPicture> opaque = RecordShadow(occluder, 4, false);
  const uint32_t hidden =
      generator.GetAllocationCount(*opaque, opaque->GetDraws()[0], Matrix())
          .first;

  // Both interiors are under the occluder, so neither fan is drawn:
  // the ambient rings from the silhouette outward and the spot is ring
  // only, leaving two rings and nothing else.
  EXPECT_EQ(transparent, 4u * 9u * 2u);
  EXPECT_EQ(hidden, 4u * 6u * 2u);
}

TEST(GeometryGeneratorTest, AShadowsMeshFitsWhatTheRecordingCovered) {
  StubGpuContext context;
  StubGpuTexture lut(TextureDesc{.width = 256, .height = 1});
  const Rect occluder = Rect::MakeLTRB(20, 20, 60, 50);
  std::shared_ptr<PrPicture> picture =
      RecordShadow(occluder, 4, /*transparent_occluder=*/true);

  ShadowGeometryGenerator generator;
  auto [vertices, indices] =
      generator.GetAllocationCount(*picture, picture->GetDraws()[0], Matrix());
  std::vector<GPUTexture*> textures;
  const GeometryContext frame{
      .context = &context, .shadow_lut = &lut, .textures = &textures};
  Written out;
  generator.Generate(*picture, picture->GetDraws()[0], Matrix(),
                     out.positions.data(), out.attributes.data(),
                     out.indices.data(), &out.paint, 0, 0, frame);

  // Everything the mesh reaches is inside what the draw said it
  // covers, or the pass it lands in would be sized too small for it.
  // The recording approximates that at three sigma, which is one and a
  // half blur radii -- past what a right angle's mitre reaches, at
  // sqrt(2) of one.
  const Rect covered = picture->GetDraws()[0].rect;
  for (uint32_t i = 0; i < vertices; i++) {
    ASSERT_TRUE(covered.ContainsInclusive(out.positions[i]))
        << "vertex " << i << " at " << out.positions[i];
  }
}

}  // namespace testing
}  // namespace impeller
