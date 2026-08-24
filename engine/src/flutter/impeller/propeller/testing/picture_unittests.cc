// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <functional>

#include "display_list/effects/dl_mask_filter.h"
#include "flutter/display_list/dl_paint.h"
#include "flutter/display_list/dl_vertices.h"
#include "flutter/display_list/effects/dl_color_filter.h"
#include "flutter/display_list/effects/dl_color_filters.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/display_list/geometry/dl_path.h"
#include "flutter/testing/testing.h"
#include "impeller/geometry/geometry_asserts.h"
#include "impeller/geometry/round_rect.h"
#include "impeller/geometry/round_superellipse.h"
#include "impeller/propeller/picture.h"

namespace impeller {
namespace testing {

namespace {

flutter::DlPaint Fill(flutter::DlColor color) {
  return flutter::DlPaint().setColor(color);
}

/// An image of a given size that never uploads anything: recording only
/// needs an identity and a size.
class StubDlImage final : public flutter::DlImage {
 public:
  StubDlImage(int32_t width, int32_t height) : size_(width, height) {}

  Type GetImageType() const override { return Type::kImpeller; }
  bool isTextureBacked() const override { return true; }
  flutter::DlColorSpace GetColorSpace() const override {
    return flutter::DlColorSpace::kSRGB;
  }
  bool isOpaque() const override { return true; }
  bool isUIThreadSafe() const override { return true; }
  flutter::DlISize GetSize() const override { return size_; }
  size_t GetApproximateByteSize() const override {
    return static_cast<size_t>(size_.width) * size_.height * 4;
  }

 private:
  const flutter::DlISize size_;
};

sk_sp<flutter::DlImage> MakeImage(int32_t width, int32_t height) {
  return sk_make_sp<StubDlImage>(width, height);
}

/// The uv corners of a kImageRect draw, as the rect they came from.
Rect UVBounds(const Draw& draw) {
  return Rect::MakePointBounds(std::begin(draw.uv_data.uv),
                               std::end(draw.uv_data.uv))
      .value_or(Rect());
}

/// The transform a draw resolves to: recording only flushes the save
/// stack into the picture's transform table when a draw needs it, so the
/// draw's index is what a test should assert against.
Matrix TransformOf(const PrPicture& picture, const Draw& draw) {
  return picture.GetTransforms()[draw.transform];
}

}  // namespace

TEST(PrPictureTest, EmptyPictureHasBaseTransform) {
  PrPictureBuilder builder(2.0f);

  std::shared_ptr<PrPicture> picture = builder.Build();

  EXPECT_TRUE(picture->GetDraws().empty());
  ASSERT_EQ(picture->GetTransforms().size(), 1u);
  EXPECT_MATRIX_NEAR(picture->GetTransforms()[0],
                     Matrix::MakeScale({2.0f, 2.0f, 1.0f}));
  EXPECT_FALSE(picture->GetBoundsUnion().has_value());
}

TEST(PrPictureTest, Transform2DAffineConcatenates) {
  PrPictureBuilder builder;

  // Row major: the 2x3 affine subset of a 4x4.
  builder.Transform2DAffine(2.0f, 3.0f, 10.0f,  //
                            4.0f, 5.0f, 20.0f);

  EXPECT_MATRIX_NEAR(builder.GetMatrix(),
                     Matrix::MakeRow(2.0f, 3.0f, 0.0f, 10.0f,  //
                                     4.0f, 5.0f, 0.0f, 20.0f,  //
                                     0.0f, 0.0f, 1.0f, 0.0f,   //
                                     0.0f, 0.0f, 0.0f, 1.0f));
}

TEST(PrPictureTest, Transform2DAffineAppliesToCurrentTransform) {
  PrPictureBuilder builder;

  builder.Translate(100.0f, 200.0f);
  builder.Transform2DAffine(2.0f, 0.0f, 10.0f,  //
                            0.0f, 2.0f, 20.0f);

  // Concatenation is on the right: the affine is applied in the local
  // space the translate established.
  EXPECT_POINT_NEAR(builder.GetMatrix() * Point(0, 0), Point(110, 220));
  EXPECT_POINT_NEAR(builder.GetMatrix() * Point(1, 1), Point(112, 222));
}

TEST(PrPictureTest, Transform2DAffineMatchesTranslate) {
  PrPictureBuilder affine;
  PrPictureBuilder translate;

  affine.Transform2DAffine(1.0f, 0.0f, 12.0f,  //
                           0.0f, 1.0f, 34.0f);
  translate.Translate(12.0f, 34.0f);

  EXPECT_MATRIX_NEAR(affine.GetMatrix(), translate.GetMatrix());
}

TEST(PrPictureTest, Transform2DAffineRecordsAgainstDraw) {
  PrPictureBuilder builder;

  builder.Transform2DAffine(2.0f, 0.0f, 10.0f,  //
                            0.0f, 2.0f, 20.0f);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  const Draw& draw = picture->GetDraws()[0];
  EXPECT_EQ(draw.type, Draw::DrawType::kRect);
  EXPECT_MATRIX_NEAR(TransformOf(*picture, draw),
                     Matrix::MakeRow(2.0f, 0.0f, 0.0f, 10.0f,  //
                                     0.0f, 2.0f, 0.0f, 20.0f,  //
                                     0.0f, 0.0f, 1.0f, 0.0f,   //
                                     0.0f, 0.0f, 0.0f, 1.0f));

  // Bounds are stored in the picture's root space.
  ASSERT_EQ(picture->GetBounds().size(), 1u);
  EXPECT_RECT_NEAR(picture->GetBounds()[0], Rect::MakeLTRB(10, 20, 30, 40));
}

TEST(PrPictureTest, TransformFullPerspectiveConcatenates) {
  PrPictureBuilder builder;

  builder.TransformFullPerspective(1.0f, 2.0f, 3.0f, 4.0f,     //
                                   5.0f, 6.0f, 7.0f, 8.0f,     //
                                   9.0f, 10.0f, 11.0f, 12.0f,  //
                                   13.0f, 14.0f, 15.0f, 16.0f);

  EXPECT_MATRIX_NEAR(builder.GetMatrix(),
                     Matrix::MakeRow(1.0f, 2.0f, 3.0f, 4.0f,     //
                                     5.0f, 6.0f, 7.0f, 8.0f,     //
                                     9.0f, 10.0f, 11.0f, 12.0f,  //
                                     13.0f, 14.0f, 15.0f, 16.0f));
}

TEST(PrPictureTest, TransformFullPerspectiveMatches2DAffineSubset) {
  PrPictureBuilder full;
  PrPictureBuilder affine;

  full.TransformFullPerspective(2.0f, 3.0f, 0.0f, 10.0f,  //
                                4.0f, 5.0f, 0.0f, 20.0f,  //
                                0.0f, 0.0f, 1.0f, 0.0f,   //
                                0.0f, 0.0f, 0.0f, 1.0f);
  affine.Transform2DAffine(2.0f, 3.0f, 10.0f,  //
                           4.0f, 5.0f, 20.0f);

  EXPECT_MATRIX_NEAR(full.GetMatrix(), affine.GetMatrix());
}

TEST(PrPictureTest, TransformFullPerspectiveKeepsPerspectiveRow) {
  PrPictureBuilder builder;

  builder.TransformFullPerspective(1.0f, 0.0f, 0.0f, 0.0f,  //
                                   0.0f, 1.0f, 0.0f, 0.0f,  //
                                   0.0f, 0.0f, 1.0f, 0.0f,  //
                                   0.0f, 0.0f, 0.005f, 1.0f);

  EXPECT_FALSE(builder.GetMatrix().IsAffine());
  EXPECT_EQ(builder.GetMatrix().e[2][3], 0.005f);
}

TEST(PrPictureTest, TransformsSurviveSaveRestore) {
  PrPictureBuilder builder;

  builder.Transform2DAffine(2.0f, 0.0f, 0.0f,  //
                            0.0f, 2.0f, 0.0f);
  Matrix outer = builder.GetMatrix();

  builder.Save();
  builder.TransformFullPerspective(1.0f, 0.0f, 0.0f, 5.0f,  //
                                   0.0f, 1.0f, 0.0f, 5.0f,  //
                                   0.0f, 0.0f, 1.0f, 0.0f,  //
                                   0.0f, 0.0f, 0.0f, 1.0f);
  EXPECT_MATRIX_NEAR(builder.GetMatrix(),
                     outer * Matrix::MakeTranslation({5.0f, 5.0f, 0.0f}));
  builder.Restore();

  EXPECT_MATRIX_NEAR(builder.GetMatrix(), outer);
}

TEST(PrPictureTest, TransformResetDropsRecordedTransform) {
  PrPictureBuilder builder(2.0f);

  builder.Transform2DAffine(2.0f, 3.0f, 10.0f,  //
                            4.0f, 5.0f, 20.0f);
  builder.TransformReset();

  EXPECT_MATRIX_NEAR(builder.GetMatrix(), Matrix());
}

TEST(PrPictureTest, EachDistinctTransformIsRecordedOnce) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  Rect rect = Rect::MakeLTRB(0, 0, 10, 10);

  builder.Transform2DAffine(1.0f, 0.0f, 5.0f,  //
                            0.0f, 1.0f, 5.0f);
  builder.DrawRect(rect, paint);
  // No transform op in between: the second draw reuses the entry.
  builder.DrawRect(rect, paint);
  builder.Transform2DAffine(1.0f, 0.0f, 5.0f,  //
                            0.0f, 1.0f, 5.0f);
  builder.DrawRect(rect, paint);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 3u);
  EXPECT_EQ(picture->GetDraws()[0].transform, 1u);
  EXPECT_EQ(picture->GetDraws()[1].transform, 1u);
  EXPECT_EQ(picture->GetDraws()[2].transform, 2u);
  // The identity base plus the two flushed transforms.
  EXPECT_EQ(picture->GetTransforms().size(), 3u);
}

TEST(PrPictureTest, BuildResetsTheRecording) {
  PrPictureBuilder builder(2.0f);

  builder.Transform2DAffine(2.0f, 0.0f, 10.0f,  //
                            0.0f, 2.0f, 20.0f);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Build();

  EXPECT_EQ(builder.GetSaveCount(), 0);
  EXPECT_MATRIX_NEAR(builder.GetMatrix(),
                     Matrix::MakeScale({2.0f, 2.0f, 1.0f}));

  std::shared_ptr<PrPicture> second = builder.Build();
  EXPECT_TRUE(second->GetDraws().empty());
  EXPECT_EQ(second->GetTransforms().size(), 1u);
}

// -----------------------------------------------------------------------
// Clip tracking.

TEST(PrPictureTest, NoClipLeavesCoverageMaximum) {
  PrPictureBuilder builder;

  EXPECT_TRUE(builder.GetDestinationClipCoverage().IsMaximum());
  EXPECT_TRUE(builder.GetLocalClipCoverage().IsMaximum());
  EXPECT_FALSE(builder.QuickReject(Rect::MakeLTRB(-1e9, -1e9, 1e9, 1e9)));
}

TEST(PrPictureTest, ClipRectNarrowsCoverage) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(10, 10, 100, 100));

  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(10, 10, 100, 100));
  EXPECT_RECT_NEAR(builder.GetLocalClipCoverage(),
                   Rect::MakeLTRB(10, 10, 100, 100));
}

TEST(PrPictureTest, ClipRectIsTrackedInRootSpace) {
  PrPictureBuilder builder(2.0f);

  builder.Translate(10, 20);
  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));

  // Root space: the dpr scale and the translate both apply.
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(20, 40, 120, 140));
  // Local space: back where the caller expressed it.
  EXPECT_RECT_NEAR(builder.GetLocalClipCoverage(),
                   Rect::MakeLTRB(0, 0, 50, 50));
}

TEST(PrPictureTest, NestedClipsIntersect) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.ClipRect(Rect::MakeLTRB(50, 50, 200, 200));

  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(50, 50, 100, 100));
}

TEST(PrPictureTest, DisjointClipsLeaveNothingVisible) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 10, 10));
  builder.ClipRect(Rect::MakeLTRB(100, 100, 200, 200));

  EXPECT_TRUE(builder.GetDestinationClipCoverage().IsEmpty());
  EXPECT_TRUE(builder.QuickReject(Rect::MakeLTRB(0, 0, 10, 10)));
}

TEST(PrPictureTest, RestorePopsTheClip) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.Save();
  builder.ClipRect(Rect::MakeLTRB(0, 0, 20, 20));
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(0, 0, 20, 20));
  builder.Restore();

  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(0, 0, 100, 100));
}

TEST(PrPictureTest, NonRectClipsUseConservativeBounds) {
  Rect bounds = Rect::MakeLTRB(0, 0, 100, 100);

  {
    PrPictureBuilder builder;
    builder.ClipOval(bounds);
    // The oval does not fill its bounds; over-admitting is the safe way
    // to be wrong.
    EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(), bounds);
  }
  {
    PrPictureBuilder builder;
    builder.ClipRoundRect(RoundRect::MakeRectXY(bounds, 10, 10));
    EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(), bounds);
  }
  {
    PrPictureBuilder builder;
    builder.ClipRoundSuperellipse(
        RoundSuperellipse::MakeRectXY(bounds, 10, 10));
    EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(), bounds);
  }
  {
    PrPictureBuilder builder;
    builder.ClipPath(flutter::DlPath::MakeRectLTRB(0, 0, 100, 100));
    EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(), bounds);
  }
}

TEST(PrPictureTest, DifferenceClipDoesNotNarrowCoverage) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50),
                   flutter::DlClipOp::kDifference);

  // What a difference clip leaves is not a rectangle, so the conservative
  // answer is the rect it started from.
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(0, 0, 100, 100));
}

TEST(PrPictureTest, ClipDoesNotContributeBounds) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // The clip's scissor, the shape and its resolve, then the pop: the
  // scissor back to where it was and the coverage with it.
  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kScissor);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
  EXPECT_EQ(picture->GetDraws()[2].type, Draw::DrawType::kClipResolveNonZero);
  EXPECT_EQ(picture->GetDraws()[3].type, Draw::DrawType::kScissor);
  EXPECT_EQ(picture->GetDraws()[4].type, Draw::DrawType::kClipReset);
  // A clip bounds content, it never contributes any.
  EXPECT_FALSE(picture->GetBoundsUnion().has_value());
  // bounds_ stays parallel to draws_.
  EXPECT_EQ(picture->GetBounds().size(), picture->GetDraws().size());
}

TEST(PrPictureTest, APictureEndsWithItsClipsPopped) {
  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // Anything can inline this picture's draws, so it must not leave a
  // clip standing for whatever draws next. The pop ends by widening the
  // scissor back, so that is the last of it.
  ASSERT_FALSE(picture->GetDraws().empty());
  EXPECT_EQ(picture->GetDraws().back().type, Draw::DrawType::kScissor);
}

TEST(PrPictureTest, APictureWithNoClipsPopsNothing) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kRect);
}

TEST(PrPictureTest, ARestoreRebuildsTheClipsThatOutliveIt) {
  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.Save();
  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));
  builder.Restore();
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // The outer clip, the inner one, then the restore: the scissor and
  // the coverage back to what the outer clip alone leaves, and that
  // clip applied again, so the draw that follows is under it alone.
  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_EQ(draws.size(), 15u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kScissor);
  EXPECT_EQ(draws[1].type, Draw::DrawType::kRectClip);
  EXPECT_EQ(draws[2].type, Draw::DrawType::kClipResolveNonZero);
  EXPECT_EQ(draws[3].type, Draw::DrawType::kScissor);
  EXPECT_EQ(draws[4].type, Draw::DrawType::kRectClip);
  EXPECT_EQ(draws[5].type, Draw::DrawType::kClipResolveNonZero);
  // The pop holds itself to the region the inner clip masked, puts
  // that back, replays the outer clip into it, and widens again.
  EXPECT_EQ(draws[6].type, Draw::DrawType::kScissor);
  EXPECT_RECT_NEAR(draws[6].rect, Rect::MakeLTRB(0, 0, 50, 50));
  EXPECT_EQ(draws[7].type, Draw::DrawType::kClipReset);
  EXPECT_EQ(draws[8].type, Draw::DrawType::kRectClip);
  EXPECT_RECT_NEAR(draws[8].rect, Rect::MakeLTRB(0, 0, 100, 100));
  EXPECT_EQ(draws[9].type, Draw::DrawType::kClipResolveNonZero);
  EXPECT_EQ(draws[10].type, Draw::DrawType::kScissor);
  EXPECT_RECT_NEAR(draws[10].rect, Rect::MakeLTRB(0, 0, 100, 100));
  EXPECT_EQ(draws[11].type, Draw::DrawType::kRect);
  EXPECT_EQ(draws[12].type, Draw::DrawType::kScissor) << "the picture's own";
  EXPECT_EQ(draws[13].type, Draw::DrawType::kClipReset);
  EXPECT_EQ(draws[14].type, Draw::DrawType::kScissor);
}

// -----------------------------------------------------------------------
// Culling draws.

TEST(PrPictureTest, DrawOutsideClipIsDropped) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawRect(Rect::MakeLTRB(200, 200, 300, 300),
                   Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();

  // Only the clip was recorded, with its scissor and its resolve.
  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
  EXPECT_FALSE(picture->GetBoundsUnion().has_value());
}

TEST(PrPictureTest, DrawIsBoundedByTheClip) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawRect(Rect::MakeLTRB(50, 50, 300, 300),
                   Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 7u);
  const Draw& draw = picture->GetDraws()[3];
  EXPECT_EQ(draw.type, Draw::DrawType::kRect);
  // The draw keeps the rect it asked for...
  EXPECT_RECT_NEAR(draw.rect, Rect::MakeLTRB(50, 50, 300, 300));
  // ...but only covers what the clip admits.
  EXPECT_RECT_NEAR(picture->GetBounds()[3], Rect::MakeLTRB(50, 50, 100, 100));
  EXPECT_RECT_NEAR(picture->GetBoundsUnion().value(),
                   Rect::MakeLTRB(50, 50, 100, 100));
}

TEST(PrPictureTest, ClippedDrawIsCulledInRootSpace) {
  PrPictureBuilder builder(2.0f);

  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));
  builder.Translate(60, 0);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();

  // The translate pushes the draw past the clip: clip only, no draw.
  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
}

TEST(PrPictureTest, ShapeDrawsAreCulled) {
  Rect clip = Rect::MakeLTRB(0, 0, 100, 100);
  Rect outside = Rect::MakeLTRB(200, 200, 300, 300);
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());

  PrPictureBuilder builder;
  builder.ClipRect(clip);
  builder.DrawRect(outside, paint);
  builder.DrawOval(outside, paint);
  builder.DrawCircle(Point(250, 250), 50, paint);
  builder.DrawRoundRect(RoundRect::MakeRectXY(outside, 4, 4), paint);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
}

TEST(PrPictureTest, CulledPointsNeverReachPositionStorage) {
  PrPictureBuilder builder;
  std::array<Point, 3> points = {Point(200, 200), Point(210, 210),
                                 Point(220, 220)};

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawPoints(flutter::DlPointMode::kPoints, points.size(),
                     points.data(), Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();

  // The reject has to happen before the points are copied: appending to
  // the picture is not something a later reject could take back.
  EXPECT_TRUE(picture->GetPositions().empty());
  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
}

TEST(PrPictureTest, VisiblePointsAreBoundedByTheirRadius) {
  PrPictureBuilder builder;
  std::array<Point, 2> points = {Point(20, 20), Point(40, 40)};
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed()).setStrokeWidth(10);

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawPoints(flutter::DlPointMode::kPoints, points.size(),
                     points.data(), paint);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 7u);
  EXPECT_EQ(picture->GetPositions().size(), 2u);
  EXPECT_RECT_NEAR(picture->GetBounds()[3], Rect::MakeLTRB(15, 15, 45, 45));
}

TEST(PrPictureTest, UnboundedDrawCoversTheClip) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(10, 10, 100, 100));
  builder.DrawPaint(Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 7u);
  EXPECT_RECT_NEAR(picture->GetBoundsUnion().value(),
                   Rect::MakeLTRB(10, 10, 100, 100));
}

TEST(PrPictureTest, UnboundedDrawStaysUnboundedWithoutAClip) {
  PrPictureBuilder builder(2.0f);

  builder.DrawColor(flutter::DlColor::kRed(), flutter::DlBlendMode::kSrcOver);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  // Maximum, not saturated to infinity: it is never transformed.
  EXPECT_TRUE(picture->GetBoundsUnion()->IsMaximum());
  EXPECT_TRUE(picture->GetBounds()[0].IsFinite());
}

// -----------------------------------------------------------------------
// Layers.

TEST(PrPictureTest, ClipLimitsTheLayerExtent) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 1000, 1000), paint);
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  // The recording ends its clip with a scissor, a reset and a scissor;
  // the layer is the draw before those.
  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_GE(draws.size(), 4u);
  const Draw& layer = draws[draws.size() - 4];
  ASSERT_EQ(layer.type, Draw::DrawType::kLayer);
  // Without the clip the layer would want a 1000x1000 texture.
  EXPECT_RECT_NEAR(layer.rect, Rect::MakeLTRB(0, 0, 100, 100));
  EXPECT_RECT_NEAR(picture->GetBounds()[draws.size() - 4],
                   Rect::MakeLTRB(0, 0, 100, 100));
}

TEST(PrPictureTest, SaveLayerBoundsLimitTheirContent) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());

  builder.SaveLayer(Rect::MakeLTRB(0, 0, 50, 50), &paint);
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(0, 0, 50, 50));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 1000, 1000), paint);
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  EXPECT_RECT_NEAR(picture->GetDraws().back().rect,
                   Rect::MakeLTRB(0, 0, 50, 50));
  // And the layer bounds are gone again once it is restored.
  EXPECT_TRUE(builder.GetDestinationClipCoverage().IsMaximum());
}

TEST(PrPictureTest, FullyClippedLayerIsDropped) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(500, 500, 600, 600), paint);
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  // No composite, and no orphaned picture left behind by the layer.
  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
  EXPECT_TRUE(picture->GetPictures().empty());
}

TEST(PrPictureTest, LayerContentIsCulledByTheInheritedClip) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10), paint);
  builder.DrawRect(Rect::MakeLTRB(500, 500, 600, 600), paint);
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetPictures().size(), 1u);
  std::shared_ptr<PrPicture> layer = picture->GetPictures()[0];
  // The layer inherits the clip, so only the first draw survives.
  ASSERT_EQ(layer->GetDraws().size(), 1u);
  EXPECT_RECT_NEAR(layer->GetBoundsUnion().value(),
                   Rect::MakeLTRB(0, 0, 10, 10));
}

TEST(PrPictureTest, AFilteredLayerStillCullsToItsDeclaredBounds) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setImageFilter(
      flutter::DlImageFilter::MakeBlur(5, 5, flutter::DlTileMode::kDecal));

  builder.SaveLayer(Rect::MakeLTRB(0, 0, 50, 50), &paint);

  // The bounds say where the content is, and a filter reads content --
  // so they bound a filtered layer as much as any other. What the filter
  // needs from outside the visible rect is a different question, asked
  // of the inherited clip rather than of these.
  EXPECT_EQ(builder.GetDestinationClipCoverage(), Rect::MakeLTRB(0, 0, 50, 50));
}

TEST(PrPictureTest, AFilteredLayerReadsPastTheClipItInherited) {
  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100),
                   flutter::DlClipOp::kIntersect,
                   /*is_aa=*/true);
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setImageFilter(
      flutter::DlImageFilter::MakeBlur(5, 5, flutter::DlTileMode::kDecal));

  // Declared wider than the clip, so what the layer records is bounded by
  // the clip rather than by these -- and the filter pulls that boundary
  // back out, since content just past it still blurs into view.
  builder.SaveLayer(Rect::MakeLTRB(-200, -200, 200, 200), &paint);

  const Rect cull = builder.GetDestinationClipCoverage();
  EXPECT_LT(cull.GetLeft(), 0);
  EXPECT_LT(cull.GetTop(), 0);
  EXPECT_GT(cull.GetRight(), 100);
  EXPECT_GT(cull.GetBottom(), 100);
}

// -----------------------------------------------------------------------
// Images.

TEST(PrPictureTest, DrawImageCoversTheImageAtItsNaturalSize) {
  PrPictureBuilder builder;

  builder.DrawImage(MakeImage(100, 50), Point(10, 20),
                    flutter::DlImageSampling::kLinear, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  const Draw& draw = picture->GetDraws()[0];
  EXPECT_EQ(draw.type, Draw::DrawType::kImageRect);
  EXPECT_RECT_NEAR(draw.rect, Rect::MakeLTRB(10, 20, 110, 70));
  // The whole image, so the uvs span the whole texture.
  EXPECT_POINT_NEAR(draw.uv_data.uv[0], Point(0, 0));
  EXPECT_POINT_NEAR(draw.uv_data.uv[1], Point(1, 0));
  EXPECT_POINT_NEAR(draw.uv_data.uv[2], Point(0, 1));
  EXPECT_POINT_NEAR(draw.uv_data.uv[3], Point(1, 1));
  ASSERT_EQ(picture->GetImages().size(), 1u);
  EXPECT_EQ(draw.uv_data.image_index, 0u);
}

TEST(PrPictureTest, DrawImageRectNormalizesTheSourceRect) {
  PrPictureBuilder builder;

  builder.DrawImageRect(MakeImage(100, 50), Rect::MakeLTRB(25, 10, 75, 40),
                        Rect::MakeLTRB(0, 0, 200, 200),
                        flutter::DlImageSampling::kNearestNeighbor, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  const Draw& draw = picture->GetDraws()[0];
  EXPECT_RECT_NEAR(draw.rect, Rect::MakeLTRB(0, 0, 200, 200));
  // Normalized at record time: src / image size.
  EXPECT_RECT_NEAR(UVBounds(draw), Rect::MakeLTRB(0.25, 0.2, 0.75, 0.8));
  // Corner order matches Rect::GetPoints, so uv[i] belongs to corner i.
  EXPECT_POINT_NEAR(draw.uv_data.uv[0], Point(0.25, 0.2));
  EXPECT_POINT_NEAR(draw.uv_data.uv[3], Point(0.75, 0.8));
  EXPECT_EQ(draw.uv_data.sampling, flutter::DlImageSampling::kNearestNeighbor);
}

TEST(PrPictureTest, ImagePaintModulatesByAlphaOnly) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed().withAlpha(128))
                               .setBlendMode(flutter::DlBlendMode::kPlus);

  builder.DrawImage(MakeImage(10, 10), Point(0, 0),
                    flutter::DlImageSampling::kLinear, &paint);
  builder.DrawImage(MakeImage(10, 10), Point(0, 0),
                    flutter::DlImageSampling::kLinear, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 2u);
  // The paint's colour does not tint the texels, only its alpha carries.
  EXPECT_EQ(picture->GetDraws()[0].color,
            flutter::DlColor::kWhite().withAlpha(128));
  EXPECT_EQ(picture->GetDraws()[0].blend_mode, flutter::DlBlendMode::kPlus);
  // No paint at all is an opaque src-over blit.
  EXPECT_EQ(picture->GetDraws()[1].color, flutter::DlColor::kWhite());
  EXPECT_EQ(picture->GetDraws()[1].blend_mode, flutter::DlBlendMode::kSrcOver);
}

TEST(PrPictureTest, ImageDrawsAreClipped) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawImageRect(MakeImage(100, 100), Rect::MakeLTRB(0, 0, 100, 100),
                        Rect::MakeLTRB(50, 50, 300, 300),
                        flutter::DlImageSampling::kLinear, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 7u);
  EXPECT_RECT_NEAR(picture->GetBounds()[3], Rect::MakeLTRB(50, 50, 100, 100));
}

TEST(PrPictureTest, CulledImageNeverReachesImageStorage) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawImage(MakeImage(50, 50), Point(500, 500),
                    flutter::DlImageSampling::kLinear, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  // Holding a reference to an image the picture never draws would keep it
  // alive for nothing.
  EXPECT_TRUE(picture->GetImages().empty());
  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
}

TEST(PrPictureTest, DistinctImagesTakeDistinctSlots) {
  PrPictureBuilder builder;
  sk_sp<flutter::DlImage> first = MakeImage(10, 10);
  sk_sp<flutter::DlImage> second = MakeImage(20, 20);

  builder.DrawImage(first, Point(0, 0), flutter::DlImageSampling::kLinear,
                    nullptr);
  builder.DrawImage(second, Point(0, 0), flutter::DlImageSampling::kLinear,
                    nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetImages().size(), 2u);
  EXPECT_EQ(picture->GetDraws()[0].uv_data.image_index, 0u);
  EXPECT_EQ(picture->GetDraws()[1].uv_data.image_index, 1u);
}

TEST(PrPictureTest, DrawImageNineSplitsIntoNinePatches) {
  PrPictureBuilder builder;

  builder.DrawImageNine(MakeImage(30, 30), IRect32::MakeLTRB(10, 10, 20, 20),
                        Rect::MakeLTRB(0, 0, 100, 100),
                        flutter::DlFilterMode::kNearest, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 9u);
  // Nine draws of one image, so one slot.
  EXPECT_EQ(picture->GetImages().size(), 1u);

  // Margins keep their source size, the center stretches over the rest.
  EXPECT_RECT_NEAR(picture->GetDraws()[0].rect, Rect::MakeLTRB(0, 0, 10, 10));
  EXPECT_RECT_NEAR(UVBounds(picture->GetDraws()[0]),
                   Rect::MakeLTRB(0, 0, 1.0 / 3, 1.0 / 3));
  EXPECT_RECT_NEAR(picture->GetDraws()[4].rect, Rect::MakeLTRB(10, 10, 90, 90));
  EXPECT_RECT_NEAR(UVBounds(picture->GetDraws()[4]),
                   Rect::MakeLTRB(1.0 / 3, 1.0 / 3, 2.0 / 3, 2.0 / 3));
  EXPECT_RECT_NEAR(picture->GetDraws()[8].rect,
                   Rect::MakeLTRB(90, 90, 100, 100));
  EXPECT_RECT_NEAR(UVBounds(picture->GetDraws()[8]),
                   Rect::MakeLTRB(2.0 / 3, 2.0 / 3, 1, 1));

  for (const Draw& draw : picture->GetDraws()) {
    EXPECT_EQ(draw.type, Draw::DrawType::kImageRect);
    EXPECT_EQ(draw.uv_data.image_index, 0u);
    EXPECT_EQ(draw.uv_data.sampling,
              flutter::DlImageSampling::kNearestNeighbor);
  }
}

TEST(PrPictureTest, NinePatchesTileTheDestination) {
  PrPictureBuilder builder;

  builder.DrawImageNine(MakeImage(30, 30), IRect32::MakeLTRB(10, 10, 20, 20),
                        Rect::MakeLTRB(5, 7, 105, 107),
                        flutter::DlFilterMode::kLinear, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 9u);
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      const Draw& draw = picture->GetDraws()[row * 3 + col];
      // Each patch continues where the one to its left and above ended.
      if (col > 0) {
        EXPECT_NEAR(draw.rect.GetLeft(),
                    picture->GetDraws()[row * 3 + col - 1].rect.GetRight(),
                    kEhCloseEnough);
      }
      if (row > 0) {
        EXPECT_NEAR(draw.rect.GetTop(),
                    picture->GetDraws()[(row - 1) * 3 + col].rect.GetBottom(),
                    kEhCloseEnough);
      }
    }
  }
  EXPECT_RECT_NEAR(picture->GetBoundsUnion().value(),
                   Rect::MakeLTRB(5, 7, 105, 107));
}

TEST(PrPictureTest, NinePatchMarginsShrinkToFitTheDestination) {
  PrPictureBuilder builder;

  // The margins want 10 + 10 on each axis but the destination is 10.
  builder.DrawImageNine(MakeImage(30, 30), IRect32::MakeLTRB(10, 10, 20, 20),
                        Rect::MakeLTRB(0, 0, 10, 10),
                        flutter::DlFilterMode::kLinear, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  // The center collapses on both axes, leaving the four corners.
  ASSERT_EQ(picture->GetDraws().size(), 4u);
  EXPECT_RECT_NEAR(picture->GetDraws()[0].rect, Rect::MakeLTRB(0, 0, 5, 5));
  EXPECT_RECT_NEAR(picture->GetDraws()[3].rect, Rect::MakeLTRB(5, 5, 10, 10));
  // Shrunk in the destination, but still the same source corners.
  EXPECT_RECT_NEAR(UVBounds(picture->GetDraws()[0]),
                   Rect::MakeLTRB(0, 0, 1.0 / 3, 1.0 / 3));
}

TEST(PrPictureTest, DegenerateNinePatchColumnsAreSkipped) {
  PrPictureBuilder builder;

  // A center covering the whole image leaves no margins at all.
  builder.DrawImageNine(MakeImage(30, 30), IRect32::MakeLTRB(0, 0, 30, 30),
                        Rect::MakeLTRB(0, 0, 100, 100),
                        flutter::DlFilterMode::kLinear, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  EXPECT_RECT_NEAR(picture->GetDraws()[0].rect, Rect::MakeLTRB(0, 0, 100, 100));
  EXPECT_RECT_NEAR(UVBounds(picture->GetDraws()[0]),
                   Rect::MakeLTRB(0, 0, 1, 1));
}

TEST(PrPictureTest, NinePatchesAreCulledIndividually) {
  PrPictureBuilder builder;

  // Only the top left corner patch survives.
  builder.ClipRect(Rect::MakeLTRB(0, 0, 10, 10));
  builder.DrawImageNine(MakeImage(30, 30), IRect32::MakeLTRB(10, 10, 20, 20),
                        Rect::MakeLTRB(0, 0, 100, 100),
                        flutter::DlFilterMode::kLinear, nullptr);

  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 7u);
  EXPECT_EQ(picture->GetDraws()[3].type, Draw::DrawType::kImageRect);
  EXPECT_RECT_NEAR(picture->GetDraws()[3].rect, Rect::MakeLTRB(0, 0, 10, 10));
  EXPECT_EQ(picture->GetImages().size(), 1u);
}

// -----------------------------------------------------------------------
// Layer filters.

TEST(PrPictureTest, UnfilteredLayerNamesNoFilter) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());

  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10), paint);
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  const Draw& layer = picture->GetDraws().back();
  ASSERT_EQ(layer.type, Draw::DrawType::kLayer);
  EXPECT_EQ(layer.layer.image_filter_index, Draw::kNoIndex);
  EXPECT_EQ(layer.layer.color_filter_index, Draw::kNoIndex);
  EXPECT_TRUE(picture->GetImageFilters().empty());
  EXPECT_TRUE(picture->GetColorFilters().empty());
}

TEST(PrPictureTest, LayerRecordsItsImageFilter) {
  PrPictureBuilder builder;
  std::shared_ptr<flutter::DlImageFilter> blur =
      flutter::DlImageFilter::MakeBlur(5, 5, flutter::DlTileMode::kDecal);
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setImageFilter(blur);

  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  const Draw& layer = picture->GetDraws().back();
  ASSERT_EQ(layer.type, Draw::DrawType::kLayer);
  ASSERT_EQ(picture->GetImageFilters().size(), 1u);
  ASSERT_EQ(layer.layer.image_filter_index, 0u);
  EXPECT_EQ(picture->GetImageFilters()[0], blur);
  EXPECT_EQ(layer.layer.color_filter_index, Draw::kNoIndex);
}

TEST(PrPictureTest, LayerRecordsItsColorFilter) {
  PrPictureBuilder builder;
  std::shared_ptr<const flutter::DlColorFilter> tint =
      flutter::DlColorFilter::MakeBlend(flutter::DlColor::kBlue(),
                                        flutter::DlBlendMode::kSrcIn);
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setColorFilter(tint);

  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  const Draw& layer = picture->GetDraws().back();
  ASSERT_EQ(picture->GetColorFilters().size(), 1u);
  ASSERT_EQ(layer.layer.color_filter_index, 0u);
  EXPECT_EQ(picture->GetColorFilters()[0], tint);
  EXPECT_EQ(layer.layer.image_filter_index, Draw::kNoIndex);
}

TEST(PrPictureTest, LayerRecordsBothFilters) {
  PrPictureBuilder builder;
  std::shared_ptr<flutter::DlImageFilter> blur =
      flutter::DlImageFilter::MakeBlur(2, 2, flutter::DlTileMode::kDecal);
  std::shared_ptr<const flutter::DlColorFilter> tint =
      flutter::DlColorFilter::MakeBlend(flutter::DlColor::kBlue(),
                                        flutter::DlBlendMode::kSrcIn);
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setImageFilter(blur);
  paint.setColorFilter(tint);

  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  const Draw& layer = picture->GetDraws().back();
  ASSERT_EQ(picture->GetImageFilters().size(), 1u);
  ASSERT_EQ(picture->GetColorFilters().size(), 1u);
  ASSERT_EQ(layer.layer.image_filter_index, 0u);
  ASSERT_EQ(layer.layer.color_filter_index, 0u);
  EXPECT_EQ(picture->GetImageFilters()[0], blur);
  EXPECT_EQ(picture->GetColorFilters()[0], tint);
}

TEST(PrPictureTest, FiltersBelongToTheCompositingPicture) {
  PrPictureBuilder builder;
  std::shared_ptr<flutter::DlImageFilter> blur =
      flutter::DlImageFilter::MakeBlur(2, 2, flutter::DlTileMode::kDecal);
  flutter::DlPaint filtered = Fill(flutter::DlColor::kRed());
  filtered.setImageFilter(blur);

  builder.SaveLayer(std::nullopt, &filtered);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  // The filter applies to the composite, which the parent records: it is
  // the parent's index space the draw names, not the layer's.
  EXPECT_EQ(picture->GetImageFilters().size(), 1u);
  ASSERT_EQ(picture->GetPictures().size(), 1u);
  EXPECT_TRUE(picture->GetPictures()[0]->GetImageFilters().empty());
}

TEST(PrPictureTest, NestedLayersEachNameTheirOwnFilter) {
  PrPictureBuilder builder;
  std::shared_ptr<flutter::DlImageFilter> outer_blur =
      flutter::DlImageFilter::MakeBlur(2, 2, flutter::DlTileMode::kDecal);
  std::shared_ptr<flutter::DlImageFilter> inner_blur =
      flutter::DlImageFilter::MakeBlur(8, 8, flutter::DlTileMode::kDecal);
  flutter::DlPaint outer = Fill(flutter::DlColor::kRed());
  outer.setImageFilter(outer_blur);
  flutter::DlPaint inner = Fill(flutter::DlColor::kRed());
  inner.setImageFilter(inner_blur);

  builder.SaveLayer(std::nullopt, &outer);
  builder.SaveLayer(std::nullopt, &inner);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  // The outer layer composites into the root...
  const Draw& outer_draw = picture->GetDraws().back();
  ASSERT_EQ(picture->GetImageFilters().size(), 1u);
  ASSERT_EQ(outer_draw.layer.image_filter_index, 0u);
  EXPECT_EQ(picture->GetImageFilters()[outer_draw.layer.image_filter_index],
            outer_blur);

  // ...and the inner one composites into the outer layer's picture.
  std::shared_ptr<PrPicture> outer_layer = picture->GetPictures()[0];
  const Draw& inner_draw = outer_layer->GetDraws().back();
  ASSERT_EQ(outer_layer->GetImageFilters().size(), 1u);
  ASSERT_EQ(inner_draw.layer.image_filter_index, 0u);
  EXPECT_EQ(outer_layer->GetImageFilters()[inner_draw.layer.image_filter_index],
            inner_blur);
}

TEST(PrPictureTest, DroppedLayerRecordsNoFilter) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setImageFilter(
      flutter::DlImageFilter::MakeBlur(2, 2, flutter::DlTileMode::kDecal));
  paint.setColorFilter(flutter::DlColorFilter::MakeBlend(
      flutter::DlColor::kBlue(), flutter::DlBlendMode::kSrcIn));

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(500, 500, 600, 600),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  // Nothing composited, so nothing holds the filters alive either.
  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
  EXPECT_TRUE(picture->GetImageFilters().empty());
  EXPECT_TRUE(picture->GetColorFilters().empty());
}

TEST(PrPictureTest, SaveLayerWithoutAPaintIsUnfiltered) {
  PrPictureBuilder builder;

  builder.SaveLayer(std::nullopt, nullptr);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();

  const Draw& layer = picture->GetDraws().back();
  ASSERT_EQ(layer.type, Draw::DrawType::kLayer);
  EXPECT_EQ(layer.layer.image_filter_index, Draw::kNoIndex);
  EXPECT_EQ(layer.layer.color_filter_index, Draw::kNoIndex);
}

// -----------------------------------------------------------------------
// Drawing a picture.

TEST(PrPictureTest, EveryPictureHasItsOwnId) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> first = builder.Build();
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> second = builder.Build();

  // Re-recording is a different picture, so anything keyed on the id
  // sees the change rather than a stale hit.
  EXPECT_NE(first->GetId(), 0u);
  EXPECT_NE(second->GetId(), 0u);
  EXPECT_NE(first->GetId(), second->GetId());
}

TEST(PrPictureTest, AnOpaquePictureIsInlined) {
  PrPictureBuilder recorder;
  recorder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                    Fill(flutter::DlColor::kRed()));
  recorder.DrawRect(Rect::MakeLTRB(20, 0, 30, 10),
                    Fill(flutter::DlColor::kGreen()));
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  builder.DrawPicture(recorded);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // The picture's draws are this recording's draws now: nothing
  // downstream has to know a picture was involved.
  ASSERT_EQ(picture->GetDraws().size(), 2u);
  EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kRect);
  EXPECT_EQ(picture->GetDraws()[0].color, flutter::DlColor::kRed());
  EXPECT_EQ(picture->GetDraws()[1].color, flutter::DlColor::kGreen());
  EXPECT_TRUE(picture->GetPictures().empty());
}

TEST(PrPictureTest, AnInlinedPictureIsPlacedByTheCurrentTransform) {
  PrPictureBuilder recorder;
  recorder.Translate(5, 5);
  recorder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                    Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  builder.Translate(100, 200);
  builder.DrawPicture(recorded);
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  const Draw& draw = picture->GetDraws()[0];
  // Its own transform composed with where it was drawn, so the geometry
  // it wrote never has to move.
  EXPECT_RECT_NEAR(draw.rect, Rect::MakeLTRB(0, 0, 10, 10));
  EXPECT_MATRIX_NEAR(picture->GetTransforms()[draw.transform],
                     Matrix::MakeTranslation({105, 205, 0}));
  EXPECT_RECT_NEAR(picture->GetBounds()[0], Rect::MakeLTRB(105, 205, 115, 215));
}

TEST(PrPictureTest, InliningRebasesEveryIndexADrawCarries) {
  PrPictureBuilder recorder;
  std::array<Point, 2> points = {Point(1, 1), Point(2, 2)};
  recorder.DrawPoints(flutter::DlPointMode::kPoints, points.size(),
                      points.data(),
                      Fill(flutter::DlColor::kRed()).setStrokeWidth(2));
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  // Something in each stream first, so a rebase that did nothing would
  // land on the wrong entry.
  std::array<Point, 1> first = {Point(50, 50)};
  builder.DrawPoints(flutter::DlPointMode::kPoints, first.size(), first.data(),
                     Fill(flutter::DlColor::kBlue()).setStrokeWidth(2));
  builder.DrawPicture(recorded);
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 2u);
  const Draw& inlined = picture->GetDraws()[1];
  ASSERT_EQ(inlined.type, Draw::DrawType::kPoints);
  // The points were appended after the ones already there.
  EXPECT_EQ(inlined.point_data.offset, 1u);
  EXPECT_EQ(inlined.point_data.length, 2u);
  ASSERT_EQ(picture->GetPositions().size(), 3u);
  EXPECT_POINT_NEAR(picture->GetPositions()[1], Point(1, 1));
  EXPECT_POINT_NEAR(picture->GetPositions()[2], Point(2, 2));
}

TEST(PrPictureTest, ARecordingContinuesAfterInlining) {
  PrPictureBuilder recorder;
  recorder.Translate(5, 5);
  recorder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                    Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  builder.Translate(100, 200);
  // Drawn first, so this recording's transform is already in the table
  // and nothing would flush it again on its own.
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kBlue()));
  builder.DrawPicture(recorded);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kGreen()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 3u);
  // Inlining appended transforms of its own, so the draw after it has to
  // find this recording's transform again rather than the last one the
  // picture brought.
  EXPECT_MATRIX_NEAR(picture->GetTransforms()[picture->GetDraws()[2].transform],
                     Matrix::MakeTranslation({100, 200, 0}));
  EXPECT_RECT_NEAR(picture->GetBounds()[2], Rect::MakeLTRB(100, 200, 110, 210));
}

TEST(PrPictureTest, AnInlinedPictureIsClipped) {
  PrPictureBuilder recorder;
  recorder.DrawRect(Rect::MakeLTRB(0, 0, 100, 100),
                    Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(0, 0, 40, 40));
  builder.DrawPicture(recorded);
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 7u);
  EXPECT_RECT_NEAR(picture->GetBounds()[3], Rect::MakeLTRB(0, 0, 40, 40));
}

TEST(PrPictureTest, AFadedPictureIsComposited) {
  PrPictureBuilder recorder;
  recorder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                    Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  builder.DrawPicture(recorded, 0.5f);
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  const Draw& draw = picture->GetDraws()[0];
  // An opacity applies to the picture as one thing, which is what a
  // composite is for: per draw it would show through the overlaps.
  EXPECT_EQ(draw.type, Draw::DrawType::kLayer);
  EXPECT_NEAR(draw.color.getAlphaF(), 0.5f, kEhCloseEnough);
}

TEST(PrPictureTest, ADrawnPictureIsPlacedByTheCurrentTransform) {
  PrPictureBuilder recorder;
  recorder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                    Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  builder.Translate(100, 200);
  builder.DrawPicture(recorded);
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  // The rect it holds is the picture's own, and the coverage is where
  // that lands here.
  EXPECT_RECT_NEAR(picture->GetDraws()[0].rect, Rect::MakeLTRB(0, 0, 10, 10));
  EXPECT_RECT_NEAR(picture->GetBounds()[0], Rect::MakeLTRB(100, 200, 110, 210));
}

TEST(PrPictureTest, ADrawnPictureIsCulledByTheClip) {
  PrPictureBuilder recorder;
  recorder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                    Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));
  builder.Translate(500, 500);
  builder.DrawPicture(recorded);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // Only the clip: a reference the clip rejects holds nothing alive.
  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
  EXPECT_TRUE(picture->GetPictures().empty());
}

TEST(PrPictureTest, AnEmptyPictureIsNotDrawnAtAll) {
  PrPictureBuilder recorder;
  std::shared_ptr<PrPicture> recorded = recorder.Build();

  PrPictureBuilder builder;
  builder.DrawPicture(recorded);
  builder.DrawPicture(nullptr);
  std::shared_ptr<PrPicture> picture = builder.Build();

  EXPECT_TRUE(picture->GetDraws().empty());
  EXPECT_TRUE(picture->GetPictures().empty());
}

TEST(PrPictureTest, AnOvalsCornersAreHalfItsBounds) {
  const Rect bounds = Rect::MakeLTRB(10, 20, 110, 80);
  PrPictureBuilder builder;
  builder.DrawOval(bounds, Fill(flutter::DlColor::kRed()));
  builder.DrawCircle(Point(50, 50), 40, Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // An oval is the round rect whose corner arcs meet: radii of half the
  // bounds each way. Radii of the whole bounds put every corner on top
  // of every other and the shape collapses.
  ASSERT_EQ(picture->GetDraws().size(), 2u);
  EXPECT_EQ(picture->GetDraws()[0].radii.top_left, Size(50, 30));
  EXPECT_EQ(picture->GetDraws()[0].radii.bottom_right, Size(50, 30));
  EXPECT_EQ(picture->GetDraws()[1].radii.top_left, Size(40, 40));
}

TEST(PrPictureTest, AStrokedShapeIsTheFillOfItsOutline) {
  const Rect square = Rect::MakeLTRB(20, 20, 80, 80);
  flutter::DlPaint stroke = Fill(flutter::DlColor::kRed())
                                .setDrawStyle(flutter::DlDrawStyle::kStroke);
  stroke.setStrokeWidth(6);

  // Every shape has a path, and a stroked one is that path's outline
  // filled: the same two draws a stroked path records, and no draw type
  // of its own.
  struct Shape {
    const char* name;
    std::function<void(PrPictureBuilder&)> draw;
  };
  const Shape shapes[] = {
      {"rect", [&](PrPictureBuilder& b) { b.DrawRect(square, stroke); }},
      {"oval", [&](PrPictureBuilder& b) { b.DrawOval(square, stroke); }},
      {"circle",
       [&](PrPictureBuilder& b) { b.DrawCircle(Point(50, 50), 30, stroke); }},
      {"round rect",
       [&](PrPictureBuilder& b) {
         b.DrawRoundRect(RoundRect::MakeRectXY(square, 10, 10), stroke);
       }},
  };

  for (const Shape& shape : shapes) {
    PrPictureBuilder builder;
    shape.draw(builder);
    std::shared_ptr<PrPicture> picture = builder.Build();

    ASSERT_EQ(picture->GetDraws().size(), 2u) << shape.name;
    EXPECT_EQ(picture->GetDraws()[0].type,
              Draw::DrawType::kConcaveWindingAccumulate)
        << shape.name;
    EXPECT_EQ(picture->GetDraws()[1].type,
              Draw::DrawType::kConcaveWindingResolveNonZero)
        << shape.name;

    // The outline reaches half a width outside the shape it borders.
    ASSERT_EQ(picture->GetPaths().size(), 1u) << shape.name;
    const Rect outline = picture->GetPaths()[0].GetBounds();
    EXPECT_NEAR(outline.GetLeft(), 20 - 3, 0.01f) << shape.name;
    EXPECT_NEAR(outline.GetTop(), 20 - 3, 0.01f) << shape.name;
    EXPECT_NEAR(outline.GetRight(), 80 + 3, 0.01f) << shape.name;
    EXPECT_NEAR(outline.GetBottom(), 80 + 3, 0.01f) << shape.name;
  }
}

TEST(PrPictureTest, ALineIsTheRectangleItSweeps) {
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setStrokeWidth(10);

  PrPictureBuilder builder;
  builder.DrawLine(Point(20, 50), Point(80, 50), paint);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // A quad is convex, so it is one draw rather than the accumulate and
  // resolve a stroked outline needs.
  ASSERT_EQ(picture->GetDraws().size(), 1u);
  EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kConvexFillPath);
  ASSERT_EQ(picture->GetPaths().size(), 1u);
  // Half a width either side, and nothing past the ends: the caps this
  // does not draw are the only thing that would reach further.
  EXPECT_RECT_NEAR(picture->GetPaths()[0].GetBounds(),
                   Rect::MakeLTRB(20, 45, 80, 55));
}

TEST(PrPictureTest, ADiagonalLineSweepsAcrossItsOwnDirection) {
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setStrokeWidth(2 * kSqrt2);

  PrPictureBuilder builder;
  builder.DrawLine(Point(0, 0), Point(50, 50), paint);
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetPaths().size(), 1u);
  // The rectangle is square to the line and not to the axes, so a
  // diagonal of this width reaches one unit out along each axis.
  EXPECT_RECT_NEAR(picture->GetPaths()[0].GetBounds(),
                   Rect::MakeLTRB(-1, -1, 51, 51));
}

TEST(PrPictureTest, ALineWithNothingToSweepIsNotRecorded) {
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setStrokeWidth(10);

  PrPictureBuilder builder;
  // No width to sweep, and no length to sweep it along.
  paint.setStrokeWidth(0);
  builder.DrawLine(Point(20, 50), Point(80, 50), paint);
  paint.setStrokeWidth(10);
  builder.DrawLine(Point(20, 50), Point(20, 50), paint);

  EXPECT_TRUE(builder.Build()->GetDraws().empty());
}

TEST(PrPictureTest, AStrokedShapeWithNoWidthIsNotRecordedYet) {
  flutter::DlPaint stroke = Fill(flutter::DlColor::kRed())
                                .setDrawStyle(flutter::DlDrawStyle::kStroke);
  stroke.setStrokeWidth(0);

  // TODO: hairlines, which are a device pixel wide whatever the scale.
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(20, 20, 80, 80), stroke);
  builder.DrawOval(Rect::MakeLTRB(20, 20, 80, 80), stroke);
  builder.DrawCircle(Point(50, 50), 30, stroke);
  builder.DrawRoundRect(
      RoundRect::MakeRectXY(Rect::MakeLTRB(20, 20, 80, 80), 10, 10), stroke);

  EXPECT_TRUE(builder.Build()->GetDraws().empty());
}

TEST(PrPictureTest, AVertexBufferCoversWhereItsVerticesAre) {
  const Point positions[3] = {Point(10, 20), Point(40, 20), Point(10, 60)};
  PrPictureBuilder builder;
  builder.DrawVertices(
      flutter::DlVertices::Make(flutter::DlVertexMode::kTriangles, 3, positions,
                                /*texture_coordinates=*/nullptr,
                                /*colors=*/nullptr),
      flutter::DlBlendMode::kSrcOver, Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // Where the vertices are, and nowhere else. A draw that claimed to
  // cover everything would put the picture's bounds at the maximum
  // rect, which is not a thing any transform survives.
  ASSERT_EQ(picture->GetDraws().size(), 1u);
  const Rect bounds = Rect::MakeLTRB(10, 20, 40, 60);
  EXPECT_RECT_NEAR(picture->GetBounds()[0], bounds);
  ASSERT_TRUE(picture->GetBoundsUnion().has_value());
  EXPECT_RECT_NEAR(picture->GetBoundsUnion().value(), bounds);
  EXPECT_FALSE(picture->GetBoundsUnion()->IsMaximum());
}

TEST(PrPictureTest, AnAtlasCoversWhereItsSpritesLand) {
  const sk_sp<flutter::DlImage> image = MakeImage(100, 50);
  // One unrotated at (10, 10), one turned a quarter at (60, 20): the
  // second's quad is not its source rect, so bounds taken from the
  // rects rather than the corners would come out wrong.
  const RSTransform transforms[2] = {RSTransform(1, 0, 10, 10),
                                     RSTransform(0, 1, 60, 20)};
  const Rect textures[2] = {Rect::MakeLTRB(0, 0, 20, 10),
                            Rect::MakeLTRB(0, 0, 30, 5)};

  PrPictureBuilder builder;
  builder.DrawAtlas(image, transforms, textures, /*colors=*/nullptr, 2,
                    flutter::DlBlendMode::kSrcOver,
                    flutter::DlImageSampling::kLinear,
                    /*cull_rect=*/nullptr, nullptr);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // The first spans (10,10)-(30,20); the second, turned, spans
  // (55,20)-(60,50).
  ASSERT_EQ(picture->GetDraws().size(), 1u);
  EXPECT_RECT_NEAR(picture->GetDraws()[0].rect, Rect::MakeLTRB(10, 10, 60, 50));
  ASSERT_TRUE(picture->GetBoundsUnion().has_value());
  EXPECT_RECT_NEAR(picture->GetBoundsUnion().value(),
                   Rect::MakeLTRB(10, 10, 60, 50));
}

TEST(PrPictureTest, AnAtlasKeepsEverySpriteItWasGiven) {
  const sk_sp<flutter::DlImage> image = MakeImage(100, 50);
  const RSTransform transforms[2] = {RSTransform(1, 0, 0, 0),
                                     RSTransform(1, 0, 30, 0)};
  const Rect textures[2] = {Rect::MakeLTRB(0, 0, 20, 10),
                            Rect::MakeLTRB(20, 0, 40, 10)};
  const flutter::DlColor colors[2] = {flutter::DlColor::kRed(),
                                      flutter::DlColor::kBlue()};

  PrPictureBuilder builder;
  builder.DrawAtlas(image, transforms, textures, colors, 2,
                    flutter::DlBlendMode::kSrcOver,
                    flutter::DlImageSampling::kNearestNeighbor,
                    /*cull_rect=*/nullptr, nullptr);
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetAtlases().size(), 1u);
  const PrAtlas& atlas = picture->GetAtlases()[0];
  EXPECT_EQ(atlas.image, image);
  EXPECT_EQ(atlas.sampling, flutter::DlImageSampling::kNearestNeighbor);
  ASSERT_EQ(atlas.transforms.size(), 2u);
  ASSERT_EQ(atlas.textures.size(), 2u);
  ASSERT_EQ(atlas.colors.size(), 2u);
  EXPECT_RECT_NEAR(atlas.textures[1], textures[1]);
  EXPECT_EQ(atlas.colors[1], colors[1]);
}

TEST(PrPictureTest, AnAtlasWithNoColoursKeepsNone) {
  const sk_sp<flutter::DlImage> image = MakeImage(100, 50);
  const RSTransform transform(1, 0, 0, 0);
  const Rect texture = Rect::MakeLTRB(0, 0, 20, 10);

  PrPictureBuilder builder;
  builder.DrawAtlas(image, &transform, &texture, /*colors=*/nullptr, 1,
                    flutter::DlBlendMode::kSrcOver,
                    flutter::DlImageSampling::kLinear,
                    /*cull_rect=*/nullptr, nullptr);

  // Empty rather than a colour each: what a sprite with none is drawn
  // in is the draw's own colour.
  std::shared_ptr<PrPicture> picture = builder.Build();
  ASSERT_EQ(picture->GetAtlases().size(), 1u);
  EXPECT_TRUE(picture->GetAtlases()[0].colors.empty());
  EXPECT_EQ(picture->GetAtlases()[0].transforms.size(), 1u);
}

TEST(PrPictureTest, AVertexBufferOutsideTheClipIsDropped) {
  const Point positions[3] = {Point(200, 200), Point(240, 200),
                              Point(200, 260)};
  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawVertices(
      flutter::DlVertices::Make(flutter::DlVertexMode::kTriangles, 3, positions,
                                /*texture_coordinates=*/nullptr,
                                /*colors=*/nullptr),
      flutter::DlBlendMode::kSrcOver, Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();
  ASSERT_EQ(picture->GetDraws().size(), 6u) << "only the clip";
  EXPECT_TRUE(picture->GetVertices().empty());
}

TEST(PrPictureTest, AShadowIsOneDrawOverItsSilhouette) {
  PrPictureBuilder builder;
  const Rect occluder = Rect::MakeLTRB(20, 20, 60, 50);
  builder.DrawShadow(flutter::DlPath::MakeRect(occluder),
                     flutter::DlColor::kBlack(), /*elevation=*/4,
                     /*transparent_occluder=*/false, /*dpr=*/1);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // One draw for both halves, over the silhouette the recording kept.
  ASSERT_EQ(picture->GetDraws().size(), 1u);
  const Draw& draw = picture->GetDraws()[0];
  EXPECT_EQ(draw.type, Draw::DrawType::kShadow);
  EXPECT_EQ(draw.shadow_data.length, 4u) << "a rect's four corners";
  EXPECT_EQ(draw.shadow_data.elevation, 4);
  EXPECT_FALSE(draw.shadow_data.transparent_occluder);
  ASSERT_EQ(picture->GetPositions().size(), 4u);

  // It covers what both halves reach: wider than the occluder all
  // round, and further below it, since the spot is thrown down by the
  // elevation and blurred wider than the ambient.
  EXPECT_LT(draw.rect.GetLeft(), occluder.GetLeft());
  EXPECT_GT(draw.rect.GetRight(), occluder.GetRight());
  EXPECT_LT(draw.rect.GetTop(), occluder.GetTop());
  EXPECT_GT(draw.rect.GetBottom() - occluder.GetBottom(),
            occluder.GetTop() - draw.rect.GetTop())
      << "the spot is thrown downward";
}

TEST(PrPictureTest, AShadowWithNoElevationOrANonConvexPathIsNotRecorded) {
  PrPictureBuilder builder;
  builder.DrawShadow(flutter::DlPath::MakeRect(Rect::MakeLTRB(0, 0, 10, 10)),
                     flutter::DlColor::kBlack(), /*elevation=*/0,
                     /*transparent_occluder=*/false, /*dpr=*/1);
  // TODO: concave occluders.
  const Point points[4] = {Point(0, 0), Point(50, 40), Point(100, 0),
                           Point(50, 100)};
  builder.DrawShadow(flutter::DlPath::MakePoly(points, 4, /*close=*/true),
                     flutter::DlColor::kBlack(), /*elevation=*/4,
                     /*transparent_occluder=*/false, /*dpr=*/1);

  EXPECT_TRUE(builder.Build()->GetDraws().empty());
}

// -----------------------------------------------------------------------
// Paths.

namespace {

/// A convex path that is not a shape with a draw of its own.
flutter::DlPath Triangle(Point a, Point b, Point c) {
  const Point points[3] = {a, b, c};
  return flutter::DlPath::MakePoly(points, 3, /*close=*/true);
}

}  // namespace

TEST(PrPictureTest, AConvexFilledPathIsRecordedWithItsPath) {
  PrPictureBuilder builder;

  const flutter::DlPath path =
      Triangle(Point(10, 10), Point(90, 10), Point(50, 70));
  builder.DrawPath(path, Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  const Draw& draw = picture->GetDraws()[0];
  EXPECT_EQ(draw.type, Draw::DrawType::kConvexFillPath);
  // The bounds are what the draw is culled by; the shape is the path.
  EXPECT_RECT_NEAR(draw.rect, path.GetBounds());
  ASSERT_EQ(picture->GetPaths().size(), 1u);
  EXPECT_EQ(picture->GetPaths()[draw.path_data.path_index].GetBounds(),
            path.GetBounds());
  EXPECT_RECT_NEAR(picture->GetBoundsUnion().value(), path.GetBounds());
}

TEST(PrPictureTest, AStrokedPathKeepsItsOutlineForTheNextFrame) {
  const flutter::DlPath path =
      Triangle(Point(10, 10), Point(90, 10), Point(50, 70));
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(8);

  PrPictureBuilder first;
  first.DrawPath(path, paint);
  std::shared_ptr<PrPicture> one = first.Build();

  // Recording the stroke left the outline on the path, so a second
  // recording of the same stroke is the same outline rather than
  // another trip through the stroker.
  PrPictureBuilder second;
  second.DrawPath(path, paint);
  std::shared_ptr<PrPicture> two = second.Build();

  ASSERT_EQ(one->GetPaths().size(), 1u);
  ASSERT_EQ(two->GetPaths().size(), 1u);
  EXPECT_EQ(&one->GetPaths()[0].GetSkPath(), &two->GetPaths()[0].GetSkPath());
}

TEST(PrPictureTest, AStrokeOfADifferentWidthIsStrokedAgain) {
  const flutter::DlPath path =
      Triangle(Point(10, 10), Point(90, 10), Point(50, 70));
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);

  paint.setStrokeWidth(8);
  PrPictureBuilder thin;
  thin.DrawPath(path, paint);
  const Rect thin_bounds = thin.Build()->GetPaths()[0].GetBounds();

  paint.setStrokeWidth(24);
  PrPictureBuilder thick;
  thick.DrawPath(path, paint);
  const Rect thick_bounds = thick.Build()->GetPaths()[0].GetBounds();

  // The one slot holds the stroke it was last asked for, and a wider
  // one reaches further.
  EXPECT_LT(thick_bounds.GetLeft(), thin_bounds.GetLeft());
  EXPECT_GT(thick_bounds.GetRight(), thin_bounds.GetRight());
}

TEST(PrPictureTest, AStrokeIsTheFillOfItsOutline) {
  PrPictureBuilder builder;
  const flutter::DlPath path =
      Triangle(Point(10, 10), Point(90, 10), Point(50, 70));
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(8);
  paint.setStrokeJoin(flutter::DlStrokeJoin::kBevel);
  paint.setStrokeCap(flutter::DlStrokeCap::kSquare);
  builder.DrawPath(path, paint);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // A ring, so a concave fill: nothing about this draw says stroke, and
  // the width, the joins and the caps are all spent by the time it is
  // recorded.
  ASSERT_EQ(picture->GetDraws().size(), 2u);
  EXPECT_EQ(picture->GetDraws()[0].type,
            Draw::DrawType::kConcaveWindingAccumulate);
  EXPECT_EQ(picture->GetDraws()[1].type,
            Draw::DrawType::kConcaveWindingResolveNonZero);
  ASSERT_EQ(picture->GetPaths().size(), 1u);

  // Half the width all round, of the shape rather than of the stroke.
  const Rect outline = picture->GetPaths()[0].GetBounds();
  EXPECT_NEAR(outline.GetLeft(), path.GetBounds().GetLeft() - 4, 1);
  EXPECT_NEAR(outline.GetRight(), path.GetBounds().GetRight() + 4, 1);
}

TEST(PrPictureTest, ATranslucentStrokeNeedsNoDrawOfItsOwn) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed().withAlphaF(0.5));
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(8);
  builder.DrawPath(Triangle(Point(10, 10), Point(90, 10), Point(50, 70)),
                   paint);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // The outline is one shape rather than a heap of overlapping pieces,
  // so there is nothing for a translucent stroke to darken itself with
  // and no second way to record one.
  ASSERT_EQ(picture->GetDraws().size(), 2u);
  EXPECT_EQ(picture->GetDraws()[0].type,
            Draw::DrawType::kConcaveWindingAccumulate);
  EXPECT_EQ(picture->GetDraws()[1].type,
            Draw::DrawType::kConcaveWindingResolveNonZero);
  // Recorded against the same coverage, so a clip drops both or
  // neither: a resolve without its accumulation draws what the last
  // path left behind.
  EXPECT_RECT_NEAR(picture->GetBounds()[0], picture->GetBounds()[1]);
}

TEST(PrPictureTest, AStrokeWithNoWidthIsNotRecordedYet) {
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(0);

  // TODO: hairlines, which are a device pixel wide whatever the scale.
  builder.DrawPath(Triangle(Point(10, 10), Point(90, 10), Point(50, 70)),
                   paint);
  EXPECT_TRUE(builder.Build()->GetDraws().empty());
}

TEST(PrPictureTest, AConcaveFillIsAnAccumulationAndAResolve) {
  PrPictureBuilder builder;

  const Point points[4] = {Point(0, 0), Point(50, 40), Point(100, 0),
                           Point(50, 100)};
  const flutter::DlPath path =
      flutter::DlPath::MakePoly(points, 4, /*close=*/true);
  ASSERT_FALSE(path.IsConvex());
  builder.DrawPath(path, Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 2u);
  EXPECT_EQ(picture->GetDraws()[0].type,
            Draw::DrawType::kConcaveWindingAccumulate);
  EXPECT_EQ(picture->GetDraws()[1].type,
            Draw::DrawType::kConcaveWindingResolveNonZero);
  // One path, named by both.
  ASSERT_EQ(picture->GetPaths().size(), 1u);
  EXPECT_EQ(picture->GetDraws()[0].path_data.path_index, 0u);
  // And one coverage, so a clip that drops one drops the other: half a
  // concave fill is worse than none.
  EXPECT_RECT_NEAR(picture->GetBounds()[0], picture->GetBounds()[1]);
}

TEST(PrPictureTest, AConcaveFillReachesPastItsPathByTheFringe) {
  const Point points[4] = {Point(0, 0), Point(50, 40), Point(100, 0),
                           Point(50, 100)};
  const flutter::DlPath path =
      flutter::DlPath::MakePoly(points, 4, /*close=*/true);

  for (Scalar scale : {1.0f, 4.0f}) {
    PrPictureBuilder builder;
    builder.Scale(scale, scale);
    builder.DrawPath(path, Fill(flutter::DlColor::kRed()));
    std::shared_ptr<PrPicture> picture = builder.Build();

    // Resolving is what zeroes the accumulator, so both draws have to
    // cover what the accumulation's fringe reached -- two device
    // pixels, in the units the draw is recorded in.
    ASSERT_EQ(picture->GetDraws().size(), 2u);
    const Rect expected = path.GetBounds().Expand(2.0f / scale);
    EXPECT_RECT_NEAR(picture->GetDraws()[0].rect, expected)
        << "at scale " << scale;
    EXPECT_RECT_NEAR(picture->GetDraws()[1].rect, expected)
        << "at scale " << scale;
  }
}

TEST(PrPictureTest, APathOutsideTheClipIsDropped) {
  PrPictureBuilder builder;

  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100));
  builder.DrawPath(Triangle(Point(200, 200), Point(300, 200), Point(250, 300)),
                   Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();
  ASSERT_EQ(picture->GetDraws().size(), 6u) << "only the clip";
  EXPECT_TRUE(picture->GetPaths().empty());
}

TEST(PrPictureTest, APathClipThatIsAShapeIsThatShapesClip) {
  const Rect bounds = Rect::MakeLTRB(10, 20, 110, 80);
  PrPictureBuilder builder;

  builder.ClipPath(flutter::DlPath::MakeRect(bounds));
  builder.ClipPath(flutter::DlPath::MakeOval(bounds));
  builder.ClipPath(flutter::DlPath::MakeRoundRectXY(bounds, 10, 10));
  // Each of them narrows what follows, as its own clip method does.
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(), bounds);
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 12u);
  EXPECT_EQ(picture->GetDraws()[1].type, Draw::DrawType::kRectClip);
  EXPECT_EQ(picture->GetDraws()[4].type, Draw::DrawType::kRRectClip);
  EXPECT_EQ(picture->GetDraws()[7].type, Draw::DrawType::kRRectClip);
  // None of them needed the path.
  EXPECT_TRUE(picture->GetPaths().empty());
}

TEST(PrPictureTest, AConvexPathClipIsRecordedAndNarrowsTheCullRect) {
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 100, 100));

  const flutter::DlPath path =
      Triangle(Point(10, 10), Point(90, 10), Point(50, 70));
  builder.ClipPath(path);
  // The path itself is enforced by the recorded clip; the cull rect
  // narrows to its bounds, which is also what the scissor gets.
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(), path.GetBounds());
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetDraws().size(), 6u);
  EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kScissor);
  EXPECT_RECT_NEAR(picture->GetDraws()[0].rect, path.GetBounds());
  const Draw& draw = picture->GetDraws()[1];
  EXPECT_EQ(draw.type, Draw::DrawType::kConvexClipPath);
  ASSERT_EQ(picture->GetPaths().size(), 1u);
  EXPECT_EQ(picture->GetPaths()[draw.path_data.path_index].GetBounds(),
            path.GetBounds());
  // A clip bounds content, it never contributes any.
  EXPECT_FALSE(picture->GetBoundsUnion().has_value());
}

TEST(PrPictureTest, AnInlinedPicturesPathsComeWithIt) {
  PrPictureBuilder inner;
  inner.ClipPath(Triangle(Point(0, 0), Point(80, 0), Point(40, 60)));
  inner.DrawPath(Triangle(Point(10, 10), Point(90, 10), Point(50, 70)),
                 Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = inner.Build();

  PrPictureBuilder outer;
  outer.DrawPath(Triangle(Point(0, 0), Point(10, 0), Point(5, 10)),
                 Fill(flutter::DlColor::kBlue()));
  outer.DrawPicture(picture, 1.0f);
  std::shared_ptr<PrPicture> inlined = outer.Build();

  // The outer path, then the inner clip and draw rebased past it.
  ASSERT_EQ(inlined->GetPaths().size(), 3u);
  ASSERT_EQ(inlined->GetDraws().size(), 9u);
  for (const Draw& draw : inlined->GetDraws()) {
    ASSERT_LT(draw.path_data.path_index, inlined->GetPaths().size());
  }
  EXPECT_EQ(inlined->GetDraws()[2].path_data.path_index, 1u);
  EXPECT_EQ(inlined->GetDraws()[4].path_data.path_index, 2u);
}

TEST(PrPictureTest, AGradientDrawNamesItsSource) {
  const std::array<flutter::DlColor, 2> colors = {flutter::DlColor::kRed(),
                                                  flutter::DlColor::kBlue()};
  const std::array<float, 2> stops = {0, 1};
  flutter::DlPaint paint = Fill(flutter::DlColor::kBlack());
  paint.setColorSource(flutter::DlColorSource::MakeLinear(
      flutter::DlPoint(0, 0), flutter::DlPoint(64, 0), 2, colors.data(),
      stops.data(), flutter::DlTileMode::kClamp));

  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 64, 64), paint);
  // A clip carries no colour, so it names no gradient.
  builder.ClipRect(Rect::MakeLTRB(0, 0, 32, 32));
  builder.DrawCircle(Point(16, 16), 8, paint);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // The two draws share a source, and a clip between them does not
  // interrupt that: only the source itself decides.
  ASSERT_EQ(picture->GetColorSources().size(), 1u);
  ASSERT_EQ(picture->GetDraws().size(), 8u);
  EXPECT_EQ(picture->GetDraws()[0].gradient, 0u);
  EXPECT_EQ(picture->GetDraws()[2].gradient, Draw::kNoIndex) << "the clip";
  EXPECT_EQ(picture->GetDraws()[4].gradient, 0u);
}

TEST(PrPictureTest, EveryGradientKindTakesASlot) {
  const std::array<flutter::DlColor, 2> colors = {flutter::DlColor::kRed(),
                                                  flutter::DlColor::kBlue()};
  const std::array<float, 2> stops = {0, 1};
  const std::shared_ptr<flutter::DlColorSource> sources[] = {
      flutter::DlColorSource::MakeLinear(
          flutter::DlPoint(0, 0), flutter::DlPoint(64, 0), 2, colors.data(),
          stops.data(), flutter::DlTileMode::kClamp),
      flutter::DlColorSource::MakeRadial(flutter::DlPoint(32, 32), 32, 2,
                                         colors.data(), stops.data(),
                                         flutter::DlTileMode::kClamp),
      flutter::DlColorSource::MakeSweep(flutter::DlPoint(32, 32), 0, 360, 2,
                                        colors.data(), stops.data(),
                                        flutter::DlTileMode::kClamp),
  };

  PrPictureBuilder builder;
  for (const std::shared_ptr<flutter::DlColorSource>& source : sources) {
    flutter::DlPaint paint = Fill(flutter::DlColor::kBlack());
    paint.setColorSource(source);
    builder.DrawRect(Rect::MakeLTRB(0, 0, 64, 64), paint);
  }
  std::shared_ptr<PrPicture> picture = builder.Build();

  ASSERT_EQ(picture->GetColorSources().size(), 3u);
  for (uint32_t i = 0; i < 3; i++) {
    EXPECT_EQ(picture->GetDraws()[i].gradient, i);
  }
}

TEST(PrPictureTest, ADrawWithNoGradientNamesNone) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 64, 64),
                   Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  EXPECT_TRUE(picture->GetColorSources().empty());
  EXPECT_EQ(picture->GetDraws()[0].gradient, Draw::kNoIndex);
}

TEST(PrPictureTest, AnInlinedPicturesGradientsComeWithIt) {
  const std::array<flutter::DlColor, 2> colors = {flutter::DlColor::kRed(),
                                                  flutter::DlColor::kBlue()};
  const std::array<float, 2> stops = {0, 1};
  flutter::DlPaint paint = Fill(flutter::DlColor::kBlack());
  paint.setColorSource(flutter::DlColorSource::MakeLinear(
      flutter::DlPoint(0, 0), flutter::DlPoint(64, 0), 2, colors.data(),
      stops.data(), flutter::DlTileMode::kClamp));

  PrPictureBuilder inner;
  inner.DrawRect(Rect::MakeLTRB(0, 0, 64, 64), paint);
  std::shared_ptr<PrPicture> picture = inner.Build();

  PrPictureBuilder outer;
  outer.DrawRect(Rect::MakeLTRB(0, 0, 8, 8), paint);
  outer.DrawPicture(picture, 1.0f);
  std::shared_ptr<PrPicture> inlined = outer.Build();

  // The outer draw's source, then the inner one's rebased past it.
  ASSERT_EQ(inlined->GetColorSources().size(), 2u);
  ASSERT_EQ(inlined->GetDraws().size(), 2u);
  EXPECT_EQ(inlined->GetDraws()[0].gradient, 0u);
  EXPECT_EQ(inlined->GetDraws()[1].gradient, 1u);
}

TEST(PrPictureTest, RestoreToCountComesBackToTheCountNotThatManyTimes) {
  PrPictureBuilder builder;

  const int count = builder.GetSaveCount();
  builder.Save();
  builder.Save();
  builder.Save();
  EXPECT_EQ(builder.GetSaveCount(), count + 3);

  builder.RestoreToCount(count);
  EXPECT_EQ(builder.GetSaveCount(), count);

  // Already there, so it does nothing rather than unwinding that many.
  builder.Save();
  const int deeper = builder.GetSaveCount();
  builder.RestoreToCount(deeper);
  EXPECT_EQ(builder.GetSaveCount(), deeper);
}

TEST(PrPictureTest, RestoreToCountDropsTheClipsTheSavesHeld) {
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 100, 100));

  // What vector_graphics does around an svg: note the count, save, clip,
  // draw, and come back to the count it noted.
  const int count = builder.GetSaveCount();
  builder.Save();
  builder.Translate(10, 10);
  builder.Save();
  builder.ClipRect(Rect::MakeLTRB(0, 0, 20, 20));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 20, 20),
                   Fill(flutter::DlColor::kRed()));
  builder.RestoreToCount(count);

  // The clip the saves held is gone, so what follows is bounded by the
  // surface alone, and the translate went with it.
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(0, 0, 100, 100));
  EXPECT_MATRIX_NEAR(builder.GetMatrix(), Matrix());
}

namespace {

flutter::DlPaint Blurred(
    flutter::DlColor color,
    Scalar sigma,
    flutter::DlBlurStyle style = flutter::DlBlurStyle::kNormal) {
  flutter::DlPaint paint = Fill(color);
  paint.setMaskFilter(flutter::DlBlurMaskFilter::Make(style, sigma));
  return paint;
}

}  // namespace

TEST(PrPictureTest, AConvexPathWithANormalBlurBecomesABand) {
  PrPictureBuilder builder;
  builder.DrawPath(flutter::DlPath::MakeOval(Rect::MakeLTRB(0, 0, 40, 40)),
                   Blurred(flutter::DlColor::kRed(), 4));
  std::shared_ptr<PrPicture> picture = builder.Build();

  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kBlurredFillPath);
  EXPECT_EQ(draws[0].blur_data.sigma, 4);
  EXPECT_TRUE(draws[0].blur_data.respect_ctm);
  // The silhouette went into the picture's point storage.
  EXPECT_EQ(draws[0].blur_data.length, picture->GetPositions().size());
  EXPECT_GE(draws[0].blur_data.length, 3u);

  // The band reaches two sigma out, and the draw has to cover it.
  EXPECT_TRUE(draws[0].rect.Contains(Rect::MakeLTRB(-8, -8, 48, 48)));
}

TEST(PrPictureTest, ABlurredShapeTakesTheBandToo) {
  // A rect has no contour of its own, so it grows one rather than
  // silently dropping the filter.
  for (int shape = 0; shape < 4; shape++) {
    PrPictureBuilder builder;
    const flutter::DlPaint paint = Blurred(flutter::DlColor::kRed(), 3);
    const Rect bounds = Rect::MakeLTRB(0, 0, 40, 40);
    switch (shape) {
      case 0:
        builder.DrawRect(bounds, paint);
        break;
      case 1:
        builder.DrawOval(bounds, paint);
        break;
      case 2:
        builder.DrawCircle(Point(20, 20), 20, paint);
        break;
      default:
        builder.DrawRoundRect(RoundRect::MakeRectRadius(bounds, 8), paint);
        break;
    }
    std::shared_ptr<PrPicture> picture = builder.Build();
    ASSERT_EQ(picture->GetDraws().size(), 1u) << "shape " << shape;
    EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kBlurredFillPath)
        << "shape " << shape;
  }
}

TEST(PrPictureTest, AnUnblurredShapeKeepsItsOwnMesh) {
  // The fast paths are only stepped around when there is a blur to draw.
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 40, 40),
                   Fill(flutter::DlColor::kRed()));
  builder.DrawRoundRect(
      RoundRect::MakeRectRadius(Rect::MakeLTRB(0, 60, 40, 90), 8),
      Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_EQ(draws.size(), 2u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kRect);
  EXPECT_EQ(draws[1].type, Draw::DrawType::kRRect);
}

TEST(PrPictureTest, AStyleThisCannotDrawFallsThroughToAPlainFill) {
  // Solid, outer and inner ramp across the band differently, and a
  // concave shape has no one silhouette. Both fill unblurred rather than
  // drawing nothing.
  for (flutter::DlBlurStyle style :
       {flutter::DlBlurStyle::kSolid, flutter::DlBlurStyle::kOuter,
        flutter::DlBlurStyle::kInner}) {
    PrPictureBuilder builder;
    builder.DrawPath(flutter::DlPath::MakeOval(Rect::MakeLTRB(0, 0, 40, 40)),
                     Blurred(flutter::DlColor::kRed(), 4, style));
    std::shared_ptr<PrPicture> picture = builder.Build();
    ASSERT_FALSE(picture->GetDraws().empty());
    EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kConvexFillPath);
  }

  const Point points[4] = {Point(10, 10), Point(50, 40), Point(90, 10),
                           Point(50, 90)};
  PrPictureBuilder concave;
  concave.DrawPath(flutter::DlPath::MakePoly(points, 4, /*close=*/true),
                   Blurred(flutter::DlColor::kRed(), 4));
  std::shared_ptr<PrPicture> picture = concave.Build();
  ASSERT_FALSE(picture->GetDraws().empty());
  EXPECT_EQ(picture->GetDraws()[0].type,
            Draw::DrawType::kConcaveWindingAccumulate);
}

TEST(PrPictureTest, ADiffRoundRectIsAnEvenOddRing) {
  PrPictureBuilder builder;
  builder.DrawDiffRoundRect(
      RoundRect::MakeRectRadius(Rect::MakeLTRB(0, 0, 100, 60), 12),
      RoundRect::MakeRectRadius(Rect::MakeLTRB(10, 10, 90, 50), 6),
      Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // Two contours, so never convex, and the fill rule has to be the one
  // that cancels the inner shape rather than filling it.
  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_EQ(draws.size(), 2u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kConcaveWindingAccumulate);
  EXPECT_EQ(draws[1].type, Draw::DrawType::kConcaveWindingResolveEvenOdd);
  // A concave fill carries the outset its resolve reads back over, so
  // the draw covers the shape rather than matching it exactly.
  EXPECT_TRUE(draws[0].rect.Contains(Rect::MakeLTRB(0, 0, 100, 60)));
}

TEST(PrPictureTest, ADiffRoundRectWithNothingTakenOutIsJustTheOuter) {
  PrPictureBuilder builder;
  builder.DrawDiffRoundRect(
      RoundRect::MakeRectRadius(Rect::MakeLTRB(0, 0, 100, 60), 12), RoundRect(),
      Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  // Falls back to the shape's own mesh rather than a two contour path.
  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kRRect);
}

TEST(PrPictureTest, ADiffRoundRectWithNoOuterDrawsNothing) {
  PrPictureBuilder builder;
  builder.DrawDiffRoundRect(
      RoundRect(), RoundRect::MakeRectRadius(Rect::MakeLTRB(10, 10, 90, 50), 6),
      Fill(flutter::DlColor::kRed()));
  EXPECT_TRUE(builder.Build()->GetDraws().empty());
}

TEST(PrPictureTest, APieArcIsOneConvexFill) {
  // Under half a turn with a centre, so the wedge is convex and the
  // whole thing is one draw.
  PrPictureBuilder builder;
  builder.DrawArc(Rect::MakeLTRB(0, 0, 100, 100), 0, 90, /*use_center=*/true,
                  Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kConvexFillPath);
  // The wedge reaches the centre and the rim, not the far corners.
  EXPECT_TRUE(Rect::MakeLTRB(0, 0, 100, 100).Contains(draws[0].rect));
}

TEST(PrPictureTest, AnArcPastHalfATurnIsConcave) {
  // A wedge of more than 180 degrees folds past its own centre, so it
  // has to go through the winding accumulator.
  PrPictureBuilder builder;
  builder.DrawArc(Rect::MakeLTRB(0, 0, 100, 100), 0, 270, /*use_center=*/true,
                  Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_EQ(draws.size(), 2u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kConcaveWindingAccumulate);
  EXPECT_EQ(draws[1].type, Draw::DrawType::kConcaveWindingResolveNonZero);
}

TEST(PrPictureTest, AnArcWithNoCentreIsClosedByItsChord) {
  // Without a centre the contour is the rim alone, and filling it seals
  // the ends with a chord rather than reaching the middle of the oval.
  const Rect bounds = Rect::MakeLTRB(0, 0, 100, 100);
  PrPictureBuilder builder;
  builder.DrawArc(bounds, 0, 90, /*use_center=*/false,
                  Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  const std::vector<Draw>& draws = picture->GetDraws();
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kConvexFillPath);
  // The quarter from 0 to 90 degrees sweeps the lower right, so the
  // segment stays out of the upper left where the centre sits.
  EXPECT_GE(draws[0].rect.GetLeft(), bounds.GetCenter().x - 1);
  EXPECT_GE(draws[0].rect.GetTop(), bounds.GetCenter().y - 1);
}

TEST(PrPictureTest, AStrokedArcTakesItsOutline) {
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(4);

  PrPictureBuilder builder;
  builder.DrawArc(Rect::MakeLTRB(0, 0, 100, 100), 0, 90, /*use_center=*/false,
                  paint);
  std::shared_ptr<PrPicture> picture = builder.Build();

  // An open contour stroked is a closed ring around it, which fills.
  ASSERT_FALSE(picture->GetDraws().empty());
  ASSERT_FALSE(picture->GetPaths().empty());
}

TEST(PrPictureTest, AnArcThatSweepsNothingDrawsNothing) {
  PrPictureBuilder builder;
  builder.DrawArc(Rect::MakeLTRB(0, 0, 100, 100), 0, 0, /*use_center=*/false,
                  Fill(flutter::DlColor::kRed()));
  builder.DrawArc(Rect(), 0, 90, /*use_center=*/true,
                  Fill(flutter::DlColor::kRed()));
  std::shared_ptr<PrPicture> picture = builder.Build();

  EXPECT_TRUE(picture->GetDraws().empty());
}

TEST(PrPictureTest, DrawReorderingBatchesNonOverlappingDraws) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.DrawRoundRect(
      RoundRect::MakeRectRadius(Rect::MakeLTRB(20, 20, 30, 30), 5.0f),
      Fill(flutter::DlColor::kRed()));
  builder.DrawRect(Rect::MakeLTRB(40, 40, 50, 50),
                   Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();
  const std::vector<Draw>& draws = picture->GetDraws();

  ASSERT_EQ(draws.size(), 3u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kRect);
  EXPECT_EQ(draws[1].type, Draw::DrawType::kRect);
  EXPECT_EQ(draws[2].type, Draw::DrawType::kRRect);
}

TEST(PrPictureTest, DrawReorderingDoesNotBatchOverlappingDraws) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 25, 25),
                   Fill(flutter::DlColor::kRed()));
  builder.DrawRoundRect(
      RoundRect::MakeRectRadius(Rect::MakeLTRB(20, 20, 40, 40), 5.0f),
      Fill(flutter::DlColor::kRed()));
  builder.DrawRect(Rect::MakeLTRB(30, 30, 50, 50),
                   Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();
  const std::vector<Draw>& draws = picture->GetDraws();

  ASSERT_EQ(draws.size(), 3u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kRect);
  EXPECT_EQ(draws[1].type, Draw::DrawType::kRRect);
  EXPECT_EQ(draws[2].type, Draw::DrawType::kRect);
}

TEST(PrPictureTest, DrawReorderingStopsAtClips) {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Save();
  // Clip is 100, 100, 200, 200
  builder.ClipRect(Rect::MakeLTRB(100, 100, 200, 200));
  // RRect inside clip
  builder.DrawRoundRect(
      RoundRect::MakeRectRadius(Rect::MakeLTRB(100, 100, 110, 110), 5.0f),
      Fill(flutter::DlColor::kRed()));
  // Rect inside clip, but disjoint from RRect (doesn't overlap RRect)
  builder.DrawRect(Rect::MakeLTRB(150, 150, 160, 160),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  std::shared_ptr<PrPicture> picture = builder.Build();
  const std::vector<Draw>& draws = picture->GetDraws();

  // Draws should be:
  // 0: kRect (0, 0, 10, 10)
  // 1: kScissor
  // 2: kRectClip
  // 3: kClipResolveNonZero
  // 4: kRRect (100, 100, 110, 110)
  // 5: kRect (150, 150, 160, 160)
  // 6: kClipReset
  // 7: kScissor

  ASSERT_EQ(draws.size(), 9u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kRect);
  EXPECT_EQ(draws[1].type, Draw::DrawType::kScissor);
  EXPECT_EQ(draws[2].type, Draw::DrawType::kRectClip);
  EXPECT_EQ(draws[3].type, Draw::DrawType::kClipResolveNonZero);
  EXPECT_EQ(draws[4].type, Draw::DrawType::kRRect);
  EXPECT_EQ(draws[5].type, Draw::DrawType::kRect);
}

TEST(PrPictureTest, DrawReorderingMergesThroughAccumulate) {
  PrPictureBuilder builder;
  flutter::DlPoint points1[4] = {{0, 0}, {10, 10}, {0, 10}, {10, 0}};
  flutter::DlPath path1 = flutter::DlPath::MakePoly(points1, 4, true);

  // Make path2 disjoint from path1
  flutter::DlPoint points2[4] = {{20, 20}, {30, 30}, {20, 30}, {30, 20}};
  flutter::DlPath path2 = flutter::DlPath::MakePoly(points2, 4, true);

  builder.DrawPath(path1, Fill(flutter::DlColor::kRed()));
  builder.DrawPath(path2, Fill(flutter::DlColor::kRed()));

  std::shared_ptr<PrPicture> picture = builder.Build();
  const std::vector<Draw>& draws = picture->GetDraws();

  ASSERT_EQ(draws.size(), 4u);
  EXPECT_EQ(draws[0].type, Draw::DrawType::kConcaveWindingAccumulate);
  EXPECT_EQ(draws[1].type, Draw::DrawType::kConcaveWindingAccumulate);
  EXPECT_EQ(draws[2].type, Draw::DrawType::kConcaveWindingResolveNonZero);
  EXPECT_EQ(draws[3].type, Draw::DrawType::kConcaveWindingResolveNonZero);
}

}  // namespace testing
}  // namespace impeller
