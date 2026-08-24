// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/dl_paint.h"
#include "flutter/display_list/dl_vertices.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/flow/layers/performance_overlay_layer.h"  // nogncheck
#include "flutter/flow/stopwatch.h"                         // nogncheck
#include "flutter/testing/testing.h"
#include "impeller/geometry/geometry_asserts.h"
#include "impeller/propeller/flow_recorder.h"
#include "impeller/propeller/scene.h"

namespace impeller {
namespace testing {

namespace {

constexpr Rect kSurface = Rect::MakeLTRB(0, 0, 800, 600);

/// A picture with one rect in it, so a leaf has something to hold.
std::shared_ptr<PrPicture> MakePicture(const Rect& rect) {
  PrPictureBuilder builder;
  builder.DrawRect(rect, flutter::DlPaint().setColor(flutter::DlColor::kRed()));
  return builder.Build();
}

}  // namespace

TEST(PrSceneTest, AnEmptySceneIsAnEmptyRoot) {
  PrSceneBuilder builder(kSurface);

  PrSceneNode root = builder.Build();

  EXPECT_FALSE(root.IsLeaf());
  EXPECT_TRUE(root.children.empty());
  EXPECT_FALSE(root.NeedsComposite());
}

TEST(PrSceneTest, APictureBecomesALeaf) {
  PrSceneBuilder builder(kSurface);
  std::shared_ptr<PrPicture> picture =
      MakePicture(Rect::MakeLTRB(0, 0, 10, 10));

  builder.DrawPicture(picture, 1.0f);
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 1u);
  const PrSceneNode& leaf = root.children[0];
  EXPECT_TRUE(leaf.IsLeaf());
  // The picture itself, not a copy: it is the cacheable thing.
  EXPECT_EQ(leaf.picture->GetId(), picture->GetId());
  EXPECT_TRUE(leaf.children.empty());
  EXPECT_FALSE(leaf.clip.has_value());
  EXPECT_MATRIX_NEAR(leaf.transform, Matrix());
}

TEST(PrSceneTest, ALeafCarriesWhereItLands) {
  PrSceneBuilder builder(kSurface);

  builder.Translate(100, 200);
  builder.Scale(2, 2);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 1u);
  // The picture's own coordinates never move, so this is the whole story
  // of where it goes.
  EXPECT_MATRIX_NEAR(
      root.children[0].transform,
      Matrix::MakeTranslation({100, 200, 0}) * Matrix::MakeScale({2, 2, 1}));
}

TEST(PrSceneTest, PicturesKeepTheirPaintOrder) {
  PrSceneBuilder builder(kSurface);
  std::shared_ptr<PrPicture> first = MakePicture(Rect::MakeLTRB(0, 0, 10, 10));
  std::shared_ptr<PrPicture> second = MakePicture(Rect::MakeLTRB(0, 0, 20, 20));

  builder.DrawPicture(first, 1.0f);
  builder.DrawPicture(second, 1.0f);
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 2u);
  EXPECT_EQ(root.children[0].picture->GetId(), first->GetId());
  EXPECT_EQ(root.children[1].picture->GetId(), second->GetId());
}

TEST(PrSceneTest, ASaveLayerBecomesAGroup) {
  PrSceneBuilder builder(kSurface);
  flutter::DlPaint paint;
  paint.setOpacity(0.5f);

  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  builder.Restore();
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 1u);
  const PrSceneNode& group = root.children[0];
  EXPECT_FALSE(group.IsLeaf());
  EXPECT_TRUE(group.NeedsComposite()) << "an opacity has to resolve somewhere";
  // The paint keeps alpha as a byte, so this is what 0.5 comes back as.
  EXPECT_NEAR(group.opacity, paint.getOpacity(), kEhCloseEnough);
  ASSERT_EQ(group.children.size(), 1u);
  EXPECT_TRUE(group.children[0].IsLeaf());
}

TEST(PrSceneTest, AGroupThatCompositesNothingIsDropped) {
  PrSceneBuilder builder(kSurface);
  flutter::DlPaint paint;
  paint.setOpacity(0.5f);

  builder.SaveLayer(std::nullopt, &paint);
  builder.Restore();
  PrSceneNode root = builder.Build();

  EXPECT_TRUE(root.children.empty());
}

TEST(PrSceneTest, APlainSaveIsNotAGroup) {
  PrSceneBuilder builder(kSurface);

  builder.Save();
  builder.Translate(10, 10);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  builder.Restore();
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode root = builder.Build();

  // Nothing to composite, so both pictures are the root's own children.
  ASSERT_EQ(root.children.size(), 2u);
  EXPECT_MATRIX_NEAR(root.children[0].transform,
                     Matrix::MakeTranslation({10, 10, 0}));
  EXPECT_MATRIX_NEAR(root.children[1].transform, Matrix());
}

TEST(PrSceneTest, GroupsNest) {
  PrSceneBuilder builder(kSurface);
  flutter::DlPaint outer;
  outer.setOpacity(0.5f);
  flutter::DlPaint inner;
  inner.setOpacity(0.25f);

  builder.SaveLayer(std::nullopt, &outer);
  builder.SaveLayer(std::nullopt, &inner);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  builder.Restore();
  builder.Restore();
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 1u);
  ASSERT_EQ(root.children[0].children.size(), 1u);
  EXPECT_NEAR(root.children[0].opacity, outer.getOpacity(), kEhCloseEnough);
  EXPECT_NEAR(root.children[0].children[0].opacity, inner.getOpacity(),
              kEhCloseEnough);
  EXPECT_TRUE(root.children[0].children[0].children[0].IsLeaf());
}

TEST(PrSceneTest, AGroupCarriesItsFilters) {
  PrSceneBuilder builder(kSurface);
  std::shared_ptr<flutter::DlImageFilter> blur =
      flutter::DlImageFilter::MakeBlur(5, 5, flutter::DlTileMode::kDecal);
  flutter::DlPaint paint;
  paint.setImageFilter(blur);

  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  builder.Restore();
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 1u);
  EXPECT_EQ(root.children[0].image_filter, blur);
  EXPECT_TRUE(root.children[0].NeedsComposite());
}

TEST(PrSceneTest, AClipRidesTheNodesUnderIt) {
  PrSceneBuilder builder(kSurface);

  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));
  builder.Translate(10, 10);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode root = builder.Build();

  // An axis aligned rect is a scissor, so it opens no group: the leaf
  // carries the clip itself.
  ASSERT_EQ(root.children.size(), 1u);
  const PrSceneNode& leaf = root.children[0];
  EXPECT_TRUE(leaf.IsLeaf());
  EXPECT_FALSE(leaf.NeedsComposite());
  // In the same space the transform maps into, so the two read together
  // without anything being inverted.
  ASSERT_TRUE(leaf.clip.has_value());
  EXPECT_RECT_NEAR(*leaf.clip, Rect::MakeLTRB(0, 0, 50, 50));
  EXPECT_MATRIX_NEAR(leaf.transform, Matrix::MakeTranslation({10, 10, 0}));
}

TEST(PrSceneTest, ATurnedRectClipNeedsAGroup) {
  PrSceneBuilder builder(kSurface);

  // Turned, the rect is no longer a rect in device space, so a scissor
  // cannot express it and it takes a pass of its own.
  builder.Rotate(30);
  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 1u);
  EXPECT_NE(root.children[0].clip_shape, nullptr);
  EXPECT_TRUE(root.children[0].NeedsComposite());
}

TEST(PrSceneTest, AClipThatAdmitsTheSurfaceIsNoClip) {
  PrSceneBuilder builder(kSurface);

  builder.ClipRect(Rect::MakeLTRB(-100, -100, 5000, 5000));
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 1u);
  EXPECT_FALSE(root.children[0].clip.has_value());
}

TEST(PrSceneTest, ARestoredClipStopsApplying) {
  PrSceneBuilder builder(kSurface);

  builder.Save();
  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  builder.Restore();
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode root = builder.Build();

  ASSERT_EQ(root.children.size(), 2u);
  EXPECT_TRUE(root.children[0].clip.has_value());
  EXPECT_FALSE(root.children[1].clip.has_value());
}

TEST(PrSceneTest, APictureTheClipRejectsIsNotAdded) {
  PrSceneBuilder builder(kSurface);

  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));
  builder.Translate(500, 500);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode root = builder.Build();

  EXPECT_TRUE(root.children.empty());
}

TEST(PrSceneTest, AFadedPictureGetsAGroupToFadeIn) {
  PrSceneBuilder builder(kSurface);

  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 0.5f);
  PrSceneNode root = builder.Build();

  // A leaf draws straight into what holds it, so the opacity needs
  // something of its own to apply to.
  ASSERT_EQ(root.children.size(), 1u);
  const PrSceneNode& group = root.children[0];
  EXPECT_FALSE(group.IsLeaf());
  EXPECT_NEAR(group.opacity, 0.5f, kEhCloseEnough);
  ASSERT_EQ(group.children.size(), 1u);
  EXPECT_TRUE(group.children[0].IsLeaf());
}

TEST(PrSceneTest, BuildLeavesAFreshSceneBehind) {
  PrSceneBuilder builder(kSurface);
  builder.Translate(10, 10);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode first = builder.Build();
  ASSERT_EQ(first.children.size(), 1u);

  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode second = builder.Build();

  ASSERT_EQ(second.children.size(), 1u);
  // The transform the last scene was left at does not carry over.
  EXPECT_MATRIX_NEAR(second.children[0].transform, Matrix());
}

TEST(PrSceneTest, AnUnbalancedSaveStillClosesIntoTheRoot) {
  PrSceneBuilder builder(kSurface);
  flutter::DlPaint paint;
  paint.setOpacity(0.5f);

  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawPicture(MakePicture(Rect::MakeLTRB(0, 0, 10, 10)), 1.0f);
  PrSceneNode root = builder.Build();

  // Losing the content would be worse than tolerating the layer tree's
  // mistake.
  ASSERT_EQ(root.children.size(), 1u);
  EXPECT_EQ(root.children[0].children.size(), 1u);
}

TEST(PrSceneTest, TheCanvasReportsWhereItIs) {
  PrSceneBuilder builder(kSurface);

  builder.Translate(10, 20);
  EXPECT_MATRIX_NEAR(builder.GetMatrix(), Matrix::MakeTranslation({10, 20, 0}));
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(), kSurface);

  builder.ClipRect(Rect::MakeLTRB(0, 0, 30, 30));
  EXPECT_RECT_NEAR(builder.GetDestinationClipCoverage(),
                   Rect::MakeLTRB(10, 20, 40, 50));
  EXPECT_TRUE(builder.QuickReject(Rect::MakeLTRB(100, 100, 200, 200)));
  EXPECT_FALSE(builder.QuickReject(Rect::MakeLTRB(0, 0, 10, 10)));
}

TEST(PrSceneBuilderTest, ThePerformanceOverlayIsRecordedAsAPicture) {
  PrSceneBuilder builder(Rect::MakeLTRB(0, 0, 200, 100));

  // A layer that draws geometry of its own rather than handing over a
  // picture: the scene takes pictures, so it records one.
  auto overlay = std::make_shared<flutter::PerformanceOverlayLayer>(
      flutter::kVisualizeRasterizerStatistics);
  overlay->set_paint_bounds(flutter::DlRect::MakeLTRB(0, 0, 200, 100));

  flutter::FixedRefreshRateStopwatch raster_time;
  flutter::FixedRefreshRateStopwatch ui_time;
  RecordFlowTree(builder, *overlay, &raster_time, &ui_time);

  PrSceneNode scene = builder.Build();
  ASSERT_EQ(scene.children.size(), 1u);
  ASSERT_TRUE(scene.children[0].IsLeaf());
  const std::shared_ptr<PrPicture>& picture = scene.children[0].picture;
  EXPECT_FALSE(picture->GetDraws().empty()) << "the graph draws nothing";
  EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kDrawVertices);
}

}  // namespace testing
}  // namespace impeller
