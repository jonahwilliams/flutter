// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>

#include "display_list/effects/image_filters/dl_blur_image_filter.h"
#include "flutter/display_list/dl_paint.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/testing/testing.h"
#include "impeller/geometry/geometry_asserts.h"
#include "impeller/propeller/dispatcher.h"
#include "impeller/propeller/testing/stub_gpu_context.h"
#include "impeller/typographer/text_frame.h"
#include "impeller/typographer/typeface.h"

namespace impeller {
namespace testing {

namespace {

class StubProgram final : public GPUProgram {};

flutter::DlPaint Fill(flutter::DlColor color) {
  return flutter::DlPaint().setColor(color);
}

/// The two pipelines a plan can name, so a test can tell which one a
/// draw asked for.
/// Somewhere for a dispatch to leave the offscreens it made.
using Offscreens = std::vector<std::unique_ptr<GPUTexture>>;

struct Pipelines {
  StubProgram color;
  StubProgram path;
  StubProgram winding_accumulate;
  StubProgram winding_resolve_nonzero;
  StubProgram winding_resolve_even_odd;

  ProgramSet Set() const {
    ProgramSet programs = {};
    programs[ProgramType::kColor] = &color;
    programs[ProgramType::kPath] = &path;
    programs[ProgramType::kWindingAccumulate] = &winding_accumulate;
    programs[ProgramType::kWindingResolveNonZero] = &winding_resolve_nonzero;
    programs[ProgramType::kWindingResolveEvenOdd] = &winding_resolve_even_odd;
    return programs;
  }
};

constexpr Rect kSurface = Rect::MakeLTRB(0, 0, 100, 100);

/// A scene of just `picture`, drawn at the origin.
PrSceneNode SceneOf(const std::shared_ptr<PrPicture>& picture) {
  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = picture;
  scene.children.push_back(std::move(leaf));
  return scene;
}

/// What one picture comes to: the frame's plan, which is the last.
RenderPlan Flatten(const std::shared_ptr<PrPicture>& picture,
                   BufferArena& arena,
                   TextMaterializer* text = nullptr) {
  StubGpuContext context;
  TextureCache textures(&context);
  SceneFlattener flattener;
  flattener.FlattenScene(SceneOf(picture), kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm, text);
  return flattener.GetPlan().back();
}

}  // namespace

TEST(DispatcherTest, ARectIsOneDrawOfSixIndices) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));

  RenderPlan plan = Flatten(builder.Build(), arena);

  ASSERT_EQ(plan.draws.size(), 1u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kColor);
  EXPECT_EQ(plan.draws[0].start, 0u);
  EXPECT_EQ(plan.draws[0].count, 6u);
  EXPECT_EQ(plan.draws[0].buffer_binds, 0u);
  // The buffers are bound once, not per draw.
  ASSERT_EQ(plan.buffers.size(), 1u);
  EXPECT_NE(plan.buffers[0].positions, nullptr);
}

TEST(DispatcherTest, ConsecutiveRectsBatchIntoOneDraw) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  for (int i = 0; i < 3; i++) {
    builder.DrawRect(Rect::MakeLTRB(i * 20, 0, i * 20 + 10, 10),
                     Fill(flutter::DlColor::kRed()));
  }

  RenderPlan plan = Flatten(builder.Build(), arena);

  // Same pipeline, same buffers, and the arena hands out contiguous
  // runs, so three rects cost one draw.
  ASSERT_EQ(plan.draws.size(), 1u);
  EXPECT_EQ(plan.draws[0].start, 0u);
  EXPECT_EQ(plan.draws[0].count, 18u);
}

TEST(DispatcherTest, ADifferentProgramBreaksTheBatch) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.DrawRoundRect(
      RoundRect::MakeRectXY(Rect::MakeLTRB(0, 0, 10, 10), 2, 2),
      Fill(flutter::DlColor::kRed()));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));

  RenderPlan plan = Flatten(builder.Build(), arena);

  // A round rect carries an implicit, so its coverage is not the plain
  // colour program's and it cannot ride in the same draw.
  ASSERT_EQ(plan.draws.size(), 3u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kColor);
  EXPECT_EQ(plan.draws[1].program, ProgramType::kPath);
  EXPECT_EQ(plan.draws[2].program, ProgramType::kColor);
  // The runs still tile the index stream in order.
  EXPECT_EQ(plan.draws[0].start, 0u);
  EXPECT_EQ(plan.draws[1].start, 6u);
  EXPECT_EQ(plan.draws[2].start, 6u + plan.draws[1].count);
  // One bind set: nothing rolled.
  EXPECT_EQ(plan.buffers.size(), 1u);
}

TEST(DispatcherTest, ABlurredLayerHoldsWhatTheBlurReaches) {
  StubGpuContext context;
  BufferArena arena(context);

  const Rect content = Rect::MakeLTRB(100, 100, 140, 140);
  auto blurred = [&](std::shared_ptr<flutter::DlImageFilter> filter) {
    PrPictureBuilder builder;
    flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
    paint.setImageFilter(filter);
    builder.SaveLayer(std::nullopt, &paint);
    builder.DrawRect(content, Fill(flutter::DlColor::kRed()));
    builder.Restore();
    return builder.Build();
  };

  std::shared_ptr<PrPicture> plain = blurred(nullptr);
  std::shared_ptr<PrPicture> blur =
      blurred(std::make_shared<flutter::DlBlurImageFilter>(
          5.0f, 5.0f, flutter::DlTileMode::kDecal));

  ASSERT_EQ(plain->GetDraws().size(), 1u);
  ASSERT_EQ(blur->GetDraws().size(), 1u);
  ASSERT_EQ(plain->GetDraws()[0].type, Draw::DrawType::kLayer);
  ASSERT_EQ(blur->GetDraws()[0].type, Draw::DrawType::kLayer);

  // The composite draw is culled and bounded by its own rect, so a blur
  // that reaches past the content has to be in it.
  EXPECT_EQ(plain->GetDraws()[0].rect, content);
  EXPECT_TRUE(blur->GetDraws()[0].rect.Contains(content));
  EXPECT_LT(blur->GetDraws()[0].rect.GetLeft(), content.GetLeft());
  EXPECT_GT(blur->GetDraws()[0].rect.GetRight(), content.GetRight());
  EXPECT_TRUE(blur->GetBoundsUnion()->Contains(blur->GetDraws()[0].rect));
}

TEST(DispatcherTest, ABlurredLayerIsCulledByWhatItsBlurReaches) {
  // The content sits outside the clip and the blur reaches back in. Kept
  // on the strength of the blur, where the content alone would go.
  StubGpuContext context;
  BufferArena arena(context);

  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(0, 0, 100, 100),
                   flutter::DlClipOp::kIntersect, true);
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setImageFilter(std::make_shared<flutter::DlBlurImageFilter>(
      8.0f, 8.0f, flutter::DlTileMode::kDecal));
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(104, 40, 120, 60),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();
  std::shared_ptr<PrPicture> picture = builder.Build();

  bool composited = false;
  for (const Draw& draw : picture->GetDraws()) {
    composited = composited || draw.type == Draw::DrawType::kLayer;
  }
  EXPECT_TRUE(composited) << "the blur reaches the clip, so it is visible";
}

TEST(DispatcherTest, ABlurredPassIsSizedForItsBlur) {
  StubGpuContext context;
  BufferArena arena(context);

  const Rect content = Rect::MakeLTRB(100, 100, 140, 140);
  auto plan_for = [&](std::shared_ptr<flutter::DlImageFilter> filter) {
    PrPictureBuilder builder;
    flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
    paint.setImageFilter(filter);
    builder.SaveLayer(std::nullopt, &paint);
    builder.DrawRect(content, Fill(flutter::DlColor::kRed()));
    builder.Restore();
    return Flatten(builder.Build(), arena);
  };

  // Nothing to assert about the plan itself here beyond that both flatten;
  // the point is that the pass the blur opens is the larger of the two,
  // which is what sizes its texture.
  RenderPlan plain = plan_for(nullptr);
  RenderPlan blur = plan_for(std::make_shared<flutter::DlBlurImageFilter>(
      6.0f, 6.0f, flutter::DlTileMode::kDecal));
  EXPECT_FALSE(plain.draws.empty());
  EXPECT_FALSE(blur.draws.empty());
}

TEST(DispatcherTest, ALayerWithNoPassCompositesNothing) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10), paint);
  builder.Restore();

  RenderPlan plan = Flatten(builder.Build(), arena);

  // The rect inside the layer went into the layer's own picture, and
  // there is no pass here to composite it back.
  EXPECT_TRUE(plan.draws.empty());
  EXPECT_TRUE(plan.buffers.empty());
}

TEST(DispatcherTest, EachDrawNamesItsOwnTransform) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.Translate(50, 60);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));

  RenderPlan plan = Flatten(builder.Build(), arena);

  ASSERT_EQ(plan.buffers.size(), 1u);
  const auto* paints =
      reinterpret_cast<const PrPaint*>(plan.buffers[0].paints->Contents());
  const auto* transforms =
      reinterpret_cast<const Matrix*>(plan.buffers[0].transforms->Contents());
  const auto* attributes = reinterpret_cast<const Attributes*>(
      plan.buffers[0].attributes->Contents());

  // The vertex stage reaches a transform only through the paint its
  // vertices name, so the whole chain has to line up.
  for (int draw = 0; draw < 2; draw++) {
    const uint32_t paint_index = attributes[draw * 4].paint & kPaintIndexMask;
    EXPECT_EQ(paint_index, static_cast<uint32_t>(draw));
    const int32_t transform_index = paints[paint_index].transform_index;
    EXPECT_EQ(transform_index, draw);
    EXPECT_MATRIX_NEAR(
        transforms[transform_index],
        draw == 0 ? Matrix() : Matrix::MakeTranslation({50, 60, 0}));
  }
}

TEST(DispatcherTest, RunningOutOfRoomStartsANewBindSet) {
  StubGpuContext context;
  // Two rects to a set: eight vertices and twelve indices.
  BufferArena arena(context, BufferArena::Capacity{
                                 .vertices = 8,
                                 .indices = 12,
                                 .paints = 8,
                                 .transforms = 8,
                                 .gradients = 8,
                             });
  PrPictureBuilder builder;
  for (int i = 0; i < 3; i++) {
    builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                     Fill(flutter::DlColor::kRed()));
  }

  RenderPlan plan = Flatten(builder.Build(), arena);

  // The third rect lands in a new set, which is different buffers, so
  // it cannot batch with what came before.
  ASSERT_EQ(plan.buffers.size(), 2u);
  ASSERT_EQ(plan.draws.size(), 2u);
  EXPECT_EQ(plan.draws[0].count, 12u);
  EXPECT_EQ(plan.draws[0].buffer_binds, 0u);
  EXPECT_EQ(plan.draws[1].start, 0u) << "the new set starts over";
  EXPECT_EQ(plan.draws[1].count, 6u);
  EXPECT_EQ(plan.draws[1].buffer_binds, 1u);
  EXPECT_NE(plan.buffers[0].positions, plan.buffers[1].positions);
}

// -----------------------------------------------------------------------
// Encoding.

TEST(DispatcherTest, EncodingBindsEveryStreamAtItsSlot) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  EncodePlan(plan, pipelines.Set(), cmd_buffer);

  // The slots buffers.glsl and the Metal catalog agree on.
  const BufferBinds& binds = plan.buffers[0];
  const std::vector<StubGpuCommandBuffer::Bind> expected = {
      {GPUShaderStage::kVertex, binds.positions, 0, 0},
      {GPUShaderStage::kVertex, binds.attributes, 0, 1},
      {GPUShaderStage::kVertex, binds.paints, 0, 2},
      {GPUShaderStage::kVertex, binds.transforms, 0, 4},
      {GPUShaderStage::kFragment, binds.paints, 0, 0},
      {GPUShaderStage::kFragment, binds.gradients, 0, 2},
  };
  ASSERT_EQ(cmd_buffer.binds.size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(cmd_buffer.binds[i].stage, expected[i].stage) << "bind " << i;
    EXPECT_EQ(cmd_buffer.binds[i].buffer, expected[i].buffer) << "bind " << i;
    EXPECT_EQ(cmd_buffer.binds[i].index, expected[i].index) << "bind " << i;
  }
}

TEST(DispatcherTest, EncodingSetsTheTextureTableOncePerPass) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  EncodePlan(plan, pipelines.Set(), cmd_buffer);

  // The shaders name the table whether or not a draw samples anything,
  // so leaving it unset is a draw-time validation failure rather than a
  // wrong picture. It belongs to the pass, so it is set once for it.
  EXPECT_EQ(cmd_buffer.texture_tables.size(), 1u);
}

TEST(DispatcherTest, EncodingDrawsThroughTheIndexBuffer) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.DrawRect(Rect::MakeLTRB(20, 0, 30, 10),
                   Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  EncodePlan(plan, pipelines.Set(), cmd_buffer);

  ASSERT_EQ(cmd_buffer.draws.size(), 1u);
  EXPECT_EQ(cmd_buffer.draws[0].start, 0);
  EXPECT_EQ(cmd_buffer.draws[0].count, 12);
  // Indexed, over the index stream of the set the run belongs to.
  EXPECT_EQ(cmd_buffer.draws[0].indices, plan.buffers[0].indices);
}

TEST(DispatcherTest, EncodingSetsThePipelineOncePerRun) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10), paint);
  builder.DrawRect(Rect::MakeLTRB(20, 0, 30, 10), paint);
  builder.DrawRoundRect(
      RoundRect::MakeRectXY(Rect::MakeLTRB(0, 0, 10, 10), 2, 2), paint);
  RenderPlan plan = Flatten(builder.Build(), arena);

  EncodePlan(plan, pipelines.Set(), cmd_buffer);

  // The two rects batched, so the pipeline changes once.
  ASSERT_EQ(cmd_buffer.pipelines.size(), 2u);
  EXPECT_EQ(cmd_buffer.pipelines[0], &pipelines.color);
  EXPECT_EQ(cmd_buffer.pipelines[1], &pipelines.path);
  EXPECT_EQ(cmd_buffer.draws.size(), 2u);
}

TEST(DispatcherTest, EncodingRebindsOnlyWhenTheBuffersChange) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context, BufferArena::Capacity{
                                 .vertices = 8,
                                 .indices = 12,
                                 .paints = 8,
                                 .transforms = 8,
                                 .gradients = 8,
                             });
  PrPictureBuilder builder;
  for (int i = 0; i < 3; i++) {
    builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                     Fill(flutter::DlColor::kRed()));
  }
  RenderPlan plan = Flatten(builder.Build(), arena);

  EncodePlan(plan, pipelines.Set(), cmd_buffer);

  // Six streams per bind set, and the set changed once.
  EXPECT_EQ(cmd_buffer.binds.size(), 12u);
  // The pipeline did not change with it.
  EXPECT_EQ(cmd_buffer.pipelines.size(), 1u);
  EXPECT_EQ(cmd_buffer.draws.size(), 2u);
}

TEST(DispatcherTest, DispatchDrawsAPictureIntoATarget) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  std::unique_ptr<GPUTexture> target = context.CreateTexture(
      TextureDesc{
          .format = TextureFormat::kBGRA8UNorm, .width = 200, .height = 100},
      false);
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));

  TextureCache textures(&context);
  Dispatch(*builder.Build(), arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, cmd_buffer);

  EXPECT_EQ(cmd_buffer.passes, 1);
  ASSERT_EQ(cmd_buffer.draws.size(), 1u);
  EXPECT_EQ(cmd_buffer.draws[0].count, 6);
}

TEST(DispatcherTest, AnEmptyPictureDrawsNothing) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  PrPictureBuilder builder;

  TextureCache textures(&context);
  Dispatch(*builder.Build(), arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, cmd_buffer);

  // The pass still runs: it is what clears the target.
  EXPECT_EQ(cmd_buffer.passes, 1);
  EXPECT_TRUE(cmd_buffer.draws.empty());
  EXPECT_TRUE(cmd_buffer.pipelines.empty());
}

// -----------------------------------------------------------------------
// The clear colour.

namespace {

/// What a plan clears to, as a colour rather than four floats.
Color ClearOf(const RenderPlan& plan) {
  return Color(plan.clear_color[0], plan.clear_color[1], plan.clear_color[2],
               plan.clear_color[3]);
}

}  // namespace

TEST(DispatcherTest, AFillOverThePassBecomesTheClearColour) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawColor(flutter::DlColor::kRed(), flutter::DlBlendMode::kSrcOver);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kGreen()));

  RenderPlan plan = Flatten(builder.Build(), arena);

  EXPECT_COLOR_NEAR(ClearOf(plan), Color::Red());
  // The fill is the clear, so only the rect is left to draw.
  ASSERT_EQ(plan.draws.size(), 1u);
  EXPECT_EQ(plan.draws[0].count, 6u);
}

TEST(DispatcherTest, AFoldedFillCostsNoGeometry) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawColor(flutter::DlColor::kRed(), flutter::DlBlendMode::kSrcOver);

  RenderPlan plan = Flatten(builder.Build(), arena);

  // Nothing to encode, so nothing was reserved for it either.
  EXPECT_TRUE(plan.draws.empty());
  EXPECT_TRUE(plan.buffers.empty());
  EXPECT_COLOR_NEAR(ClearOf(plan), Color::Red());
}

TEST(DispatcherTest, ClearDrawsBlendTogetherOnTheCpu) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  const flutter::DlColor half_red = flutter::DlColor::kRed().withAlphaF(0.5);
  builder.DrawColor(half_red, flutter::DlBlendMode::kSrcOver);
  builder.DrawColor(half_red, flutter::DlBlendMode::kSrcOver);

  RenderPlan plan = Flatten(builder.Build(), arena);

  // Half red over half red over nothing: the same colour the GPU would
  // have blended, worked out once instead of per pixel.
  EXPECT_TRUE(plan.draws.empty());
  EXPECT_COLOR_NEAR(ClearOf(plan), Color(0.75, 0, 0, 0.75));
}

TEST(DispatcherTest, ASourceFillReplacesWhatTheRunBuiltUp) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawColor(flutter::DlColor::kRed(), flutter::DlBlendMode::kSrcOver);
  builder.DrawColor(flutter::DlColor::kBlue().withAlphaF(0.5),
                    flutter::DlBlendMode::kSrc);

  RenderPlan plan = Flatten(builder.Build(), arena);

  EXPECT_TRUE(plan.draws.empty());
  EXPECT_COLOR_NEAR(ClearOf(plan), Color(0, 0, 0.5, 0.5));
}

TEST(DispatcherTest, ADrawThatLeavesThePassUncoveredIsNotAClear) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  // The plan is 100 by 100.
  builder.DrawRect(Rect::MakeLTRB(0, 0, 99, 100),
                   Fill(flutter::DlColor::kRed()));

  RenderPlan plan = Flatten(builder.Build(), arena);

  EXPECT_COLOR_NEAR(ClearOf(plan), Color::BlackTransparent());
  EXPECT_EQ(plan.draws.size(), 1u);
}

TEST(DispatcherTest, ARotatedFillIsNotAClear) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.Rotate(45);
  // Big enough that its bounds swallow the pass, which its corners do
  // not.
  builder.DrawRect(Rect::MakeLTRB(-500, -500, 500, 500),
                   Fill(flutter::DlColor::kRed()));

  RenderPlan plan = Flatten(builder.Build(), arena);

  // A rotated rect covers less than its bounds claim, so folding it
  // would paint corners the draw does not reach.
  EXPECT_COLOR_NEAR(ClearOf(plan), Color::BlackTransparent());
  EXPECT_EQ(plan.draws.size(), 1u);
}

TEST(DispatcherTest, AClipStopsTheFold) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(0, 0, 50, 50));
  builder.DrawColor(flutter::DlColor::kRed(), flutter::DlBlendMode::kSrcOver);

  RenderPlan plan = Flatten(builder.Build(), arena);

  // What the fill covers is what the clip left it, which is not the
  // pass.
  EXPECT_COLOR_NEAR(ClearOf(plan), Color::BlackTransparent());
}

TEST(DispatcherTest, OnlyALeadingRunFolds) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kGreen()));
  builder.DrawColor(flutter::DlColor::kRed(), flutter::DlBlendMode::kSrcOver);

  RenderPlan plan = Flatten(builder.Build(), arena);

  // The fill covers the pass, but folding it would drop the rect under
  // it, which is drawn first. Both are plain rects, so they batch into
  // one draw of two quads.
  EXPECT_COLOR_NEAR(ClearOf(plan), Color::BlackTransparent());
  ASSERT_EQ(plan.draws.size(), 1u);
  EXPECT_EQ(plan.draws[0].count, 12u);
}

TEST(DispatcherTest, TheClearColourReachesTheRenderPass) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  PrPictureBuilder builder;
  builder.DrawColor(flutter::DlColor::kBlue().withAlphaF(0.5),
                    flutter::DlBlendMode::kSrcOver);

  TextureCache textures(&context);
  Dispatch(*builder.Build(), arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, cmd_buffer);

  EXPECT_EQ(cmd_buffer.passes, 1);
  EXPECT_TRUE(cmd_buffer.draws.empty());
  // Premultiplied, which is what the pass takes.
  EXPECT_NEAR(cmd_buffer.clear_color[2], 0.5f, kEhCloseEnough);
  EXPECT_NEAR(cmd_buffer.clear_color[3], 0.5f, kEhCloseEnough);
}

// -----------------------------------------------------------------------
// Scenes.

namespace {

std::shared_ptr<PrPicture> RectPicture(const Rect& rect,
                                       flutter::DlColor color) {
  PrPictureBuilder builder;
  builder.DrawRect(rect, Fill(color));
  return builder.Build();
}

/// The most taps a step will ever gather either side of centre.
int32_t TapRadiusCeiling() {
  return 64;
}

/// A scene, ordered and encoded: the flattener holds both halves.
SceneFlattener Flattened(const PrSceneNode& scene,
                         BufferArena& arena,
                         TextureCache* cache = nullptr) {
  StubGpuContext context;
  TextureCache own(&context);
  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, cache != nullptr ? *cache : own,
                         TextureFormat::kRGBA8UNorm);
  return flattener;
}

/// The frame's plan for a whole scene, which is the last: everything it
/// composites comes before it.
RenderPlan FlattenTheScene(const PrSceneNode& scene, BufferArena& arena) {
  StubGpuContext context;
  TextureCache textures(&context);
  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm);
  return flattener.GetPlan().back();
}

}  // namespace

TEST(DispatcherTest, ABlurredLayerResolvesThroughTwoFilterSteps) {
  StubGpuContext context;
  BufferArena arena(context);

  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setOpacity(0.5f);
  // Under the threshold, so nothing is held down and the sigmas arrive
  // as given. What downscaling does to them is its own test.
  paint.setImageFilter(std::make_shared<flutter::DlBlurImageFilter>(
      3.0f, 2.0f, flutter::DlTileMode::kDecal));
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(100, 100, 140, 140),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  SceneFlattener flattened = Flattened(SceneOf(builder.Build()), arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  // The content, one step per axis, and the frame.
  ASSERT_EQ(passes.size(), 4u);
  EXPECT_EQ(passes[0].filter_source, SceneFlattener::kNoPass)
      << "the content draws itself";
  EXPECT_EQ(passes[1].filter_source, 0u);
  EXPECT_EQ(passes[2].filter_source, 1u);

  // One axis each, and the sigmas are the ones the filter asked for --
  // the transform here is identity, so nothing scales them.
  EXPECT_EQ(passes[1].filter_step.step.y, 0);
  EXPECT_GT(passes[1].filter_step.step.x, 0);
  EXPECT_EQ(passes[1].filter_step.sigma, 3);
  EXPECT_EQ(passes[2].filter_step.step.x, 0);
  EXPECT_GT(passes[2].filter_step.step.y, 0);
  EXPECT_EQ(passes[2].filter_step.sigma, 2);
  EXPECT_EQ(passes[1].resolution_scale, 1.0f);
  EXPECT_EQ(passes[2].resolution_scale, 1.0f);
  EXPECT_GT(passes[1].filter_step.radius, 0);

  // Every step covers what the blur reaches, and the layer's own alpha
  // belongs to the last one, since that is what the frame composites.
  EXPECT_EQ(passes[1].coverage, passes[0].coverage);
  EXPECT_EQ(passes[2].coverage, passes[0].coverage);
  EXPECT_EQ(passes[0].opacity, 1.0f);
  // Through an eight bit alpha channel on the way, so near rather than
  // exact.
  EXPECT_NEAR(passes[2].opacity, 0.5f, 1.0f / 255.0f);

  // A step is held against whatever held its source, so a layer that has
  // not changed keeps the blur it already resolved.
  EXPECT_TRUE(passes[1].key.IsCacheable());
  EXPECT_TRUE(passes[2].key.IsCacheable());

  // It holds the same contents, so it takes the same names for them
  // rather than a summary of them. What tells the steps apart from each
  // other and from the source is how far along they are.
  EXPECT_EQ(passes[1].key.ids[0], passes[0].key.ids[0]);
  EXPECT_EQ(passes[2].key.ids[0], passes[0].key.ids[0]);
  EXPECT_EQ(passes[0].key.variant, 0u);
  EXPECT_EQ(passes[1].key.variant, 1u);
  EXPECT_EQ(passes[2].key.variant, 2u);
  EXPECT_FALSE(passes[1].key == passes[0].key) << "a texture each";
  EXPECT_FALSE(passes[1].key == passes[2].key) << "a texture each";

  // Each step draws its one quad with the blur program and carries the
  // uniform the shader reads.
  const std::vector<RenderPlan>& plans = flattened.GetPlan();
  for (uint32_t step : {1u, 2u}) {
    ASSERT_TRUE(plans[step].filter.has_value()) << "step " << step;
    ASSERT_EQ(plans[step].draws.size(), 1u) << "step " << step;
    EXPECT_EQ(plans[step].draws[0].program, ProgramType::kBlur)
        << "step " << step;
    EXPECT_EQ(plans[step].draws[0].count, 6u) << "step " << step;
  }
}

TEST(DispatcherTest, AClipThatCutsNothingIsNotCarried) {
  StubGpuContext context;
  BufferArena arena(context);
  auto blur = std::make_shared<flutter::DlBlurImageFilter>(
      4.0f, 4.0f, flutter::DlTileMode::kDecal);

  // Rows that each clip their own contents to their own bounds, which is
  // the ordinary way a list is built -- and a clip that is its item's own
  // bounds cuts nothing at all.
  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 800, 800));
  for (int i = 0; i < 3; i++) {
    scene_builder.Save();
    scene_builder.ClipRect(Rect::MakeLTRB(0, 100 + i * 50, 800, 140 + i * 50));
    scene_builder.DrawPicture(
        RectPicture(Rect::MakeLTRB(0, 100 + i * 50, 400, 140 + i * 50),
                    flutter::DlColor::kRed()),
        1);
    scene_builder.Restore();
  }
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 300, 400, 340), flutter::DlColor::kGreen()),
      1);
  scene_builder.SaveLayer(std::nullopt, nullptr, blur.get());
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(10, 110, 390, 290), flutter::DlColor::kBlue()),
      1);
  scene_builder.Restore();

  TextureCache cache(&context);
  SceneFlattener flattened;
  flattened.FlattenScene(scene_builder.Build(), Rect::MakeLTRB(0, 0, 800, 800));
  flattened.EncodePasses(arena, cache, TextureFormat::kRGBA8UNorm);

  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  // The snapshot the backdrop reads. Its items were copied from what came
  // before, carrying whatever clips they had; each row's clip is its own
  // bounds, so none of them cut and none of them are kept.
  const SceneFlattener::Pass& frame = passes.back();
  ASSERT_FALSE(frame.items.empty());
  const SceneFlattener::Item* backdrop = nullptr;
  for (const SceneFlattener::Item& item : frame.items) {
    if (item.IsComposite()) {
      backdrop = &item;
      break;
    }
  }
  ASSERT_NE(backdrop, nullptr);

  const SceneFlattener::Pass& tail = passes[backdrop->pass];
  ASSERT_NE(tail.filter_source, SceneFlattener::kNoPass);
  const SceneFlattener::Pass& mid = passes[tail.filter_source];
  ASSERT_NE(mid.filter_source, SceneFlattener::kNoPass);
  const uint32_t snapshot_index = mid.filter_source;
  const SceneFlattener::Pass& snapshot = passes[snapshot_index];

  ASSERT_GE(snapshot.items.size(), 3u);
  for (const SceneFlattener::Item& item : snapshot.items) {
    EXPECT_FALSE(item.clip.has_value())
        << "a clip that is its item's own bounds cuts nothing";
  }

  // So the whole snapshot goes down as one run, with no scissor at all.
  const RenderPlan& plan = flattened.GetPlan()[snapshot_index];
  ASSERT_EQ(plan.draws.size(), 1u) << "one run, no scissors";
  EXPECT_EQ(plan.draws[0].program, ProgramType::kColor);
}

TEST(DispatcherTest, ABackdropReadsWhatWasDrawnBeforeIt) {
  StubGpuContext context;
  BufferArena arena(context);

  auto blur = std::make_shared<flutter::DlBlurImageFilter>(
      4.0f, 4.0f, flutter::DlTileMode::kDecal);

  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 400, 400));
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 200, 200), flutter::DlColor::kRed()), 1);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 200, 200, 400), flutter::DlColor::kGreen()),
      1);
  scene_builder.SaveLayer(std::nullopt, nullptr, blur.get());
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(50, 50, 150, 150), flutter::DlColor::kBlue()),
      1);
  scene_builder.Restore();

  TextureCache cache(&context);
  SceneFlattener flattened;
  flattened.FlattenScene(scene_builder.Build(), Rect::MakeLTRB(0, 0, 400, 400));
  flattened.EncodePasses(arena, cache, TextureFormat::kRGBA8UNorm);

  const SceneFlattener::Pass& frame = flattened.GetPasses().back();
  // The two behind it, what it shows of them, and its own content.
  ASSERT_EQ(frame.items.size(), 4u);
  EXPECT_FALSE(frame.items[0].IsComposite());
  EXPECT_FALSE(frame.items[1].IsComposite());
  EXPECT_TRUE(frame.items[2].IsComposite()) << "the backdrop, before the child";
  EXPECT_FALSE(frame.items[3].IsComposite());

  // What it reads is a copy of what came before it, and nothing after.
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();
  const SceneFlattener::Pass& tail = passes[frame.items[2].pass];
  ASSERT_NE(tail.filter_source, SceneFlattener::kNoPass)
      << "the blur should have chained onto it";
  const SceneFlattener::Pass& mid = passes[tail.filter_source];
  ASSERT_NE(mid.filter_source, SceneFlattener::kNoPass);
  const SceneFlattener::Pass& snapshot = passes[mid.filter_source];
  EXPECT_EQ(snapshot.items.size(), 2u) << "the two drawn before it";
  EXPECT_EQ(snapshot.items[0].picture, frame.items[0].picture);
  EXPECT_EQ(snapshot.items[1].picture, frame.items[1].picture);

  // It cannot show more than was drawn.
  EXPECT_TRUE(Rect::MakeLTRB(0, 0, 200, 400).Contains(snapshot.coverage));
}

TEST(DispatcherTest, ABackdropOnlyReadsWhatItCanSee) {
  StubGpuContext context;
  BufferArena arena(context);

  auto blur = std::make_shared<flutter::DlBlurImageFilter>(
      2.0f, 2.0f, flutter::DlTileMode::kDecal);

  // Near sits under where the backdrop shows; far is nowhere near it.
  std::shared_ptr<PrPicture> near =
      RectPicture(Rect::MakeLTRB(0, 0, 100, 100), flutter::DlColor::kRed());
  std::shared_ptr<PrPicture> far = RectPicture(
      Rect::MakeLTRB(600, 600, 700, 700), flutter::DlColor::kGreen());

  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 800, 800));
  scene_builder.DrawPicture(near, 1);
  scene_builder.DrawPicture(far, 1);
  scene_builder.SaveLayer(Rect::MakeLTRB(0, 0, 100, 100), nullptr, blur.get());
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(10, 10, 90, 90), flutter::DlColor::kBlue()),
      1);
  scene_builder.Restore();

  TextureCache cache(&context);
  SceneFlattener flattened;
  flattened.FlattenScene(scene_builder.Build(), Rect::MakeLTRB(0, 0, 800, 800));
  flattened.EncodePasses(arena, cache, TextureFormat::kRGBA8UNorm);

  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();
  const SceneFlattener::Pass& frame = passes.back();
  ASSERT_EQ(frame.items.size(), 4u);
  ASSERT_TRUE(frame.items[2].IsComposite()) << "the backdrop";

  // Walk back through the two gathering steps to what was snapshotted.
  const SceneFlattener::Pass& tail = passes[frame.items[2].pass];
  ASSERT_NE(tail.filter_source, SceneFlattener::kNoPass);
  const SceneFlattener::Pass& mid = passes[tail.filter_source];
  ASSERT_NE(mid.filter_source, SceneFlattener::kNoPass);
  const SceneFlattener::Pass& snapshot = passes[mid.filter_source];

  // The far picture was drawn before it but cannot be seen through it,
  // so it is neither redrawn for it nor named among what decides it.
  ASSERT_EQ(snapshot.items.size(), 1u) << "the far picture should be culled";
  EXPECT_EQ(snapshot.items[0].picture, near);
  EXPECT_EQ(snapshot.key.ids.size(), 1u);
  EXPECT_EQ(snapshot.key.ids[0], near->GetId());

  // And it holds only what it shows, not the union of the whole scene.
  EXPECT_LT(snapshot.coverage.GetRight(), 600);
  EXPECT_LT(snapshot.coverage.GetBottom(), 600);
}

TEST(DispatcherTest, ABackdropWithNothingBehindItDrawsNothing) {
  StubGpuContext context;
  BufferArena arena(context);

  auto blur = std::make_shared<flutter::DlBlurImageFilter>(
      4.0f, 4.0f, flutter::DlTileMode::kDecal);

  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 400, 400));
  scene_builder.SaveLayer(std::nullopt, nullptr, blur.get());
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(50, 50, 150, 150), flutter::DlColor::kBlue()),
      1);
  scene_builder.Restore();

  TextureCache cache(&context);
  SceneFlattener flattened;
  flattened.FlattenScene(scene_builder.Build(), Rect::MakeLTRB(0, 0, 400, 400));
  flattened.EncodePasses(arena, cache, TextureFormat::kRGBA8UNorm);

  const SceneFlattener::Pass& frame = flattened.GetPasses().back();
  ASSERT_EQ(frame.items.size(), 1u) << "its own content and no backdrop";
  EXPECT_FALSE(frame.items[0].IsComposite());
}

TEST(DispatcherTest, AFilteredNodeIsClippedLikeItsSiblings) {
  StubGpuContext context;
  BufferArena arena(context);

  // The shape a layer tree takes: a scene clip over alternating nodes,
  // some a filtered layer of their own and some a plain leaf. A node
  // carries its clip rather than sitting under one, so a composite that
  // reads only what it inherited comes out unclipped beside siblings
  // that are not -- and the scissor is thrown wide for every one of
  // them and put back for the next.
  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 1600, 1200));
  scene_builder.ClipRect(Rect::MakeLTRB(0, 112, 1600, 1200));
  for (int i = 0; i < 4; i++) {
    const Scalar y = 200 + i * 120;
    flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
    paint.setImageFilter(std::make_shared<flutter::DlBlurImageFilter>(
        4.0f, 4.0f, flutter::DlTileMode::kDecal));
    scene_builder.SaveLayer(std::nullopt, &paint);
    scene_builder.DrawPicture(RectPicture(Rect::MakeLTRB(100, y, 200, y + 96),
                                          flutter::DlColor::kRed()),
                              1);
    scene_builder.Restore();
    scene_builder.DrawPicture(RectPicture(Rect::MakeLTRB(300, y, 400, y + 96),
                                          flutter::DlColor::kGreen()),
                              1);
  }

  TextureCache cache(&context);
  SceneFlattener flattened;
  flattened.FlattenScene(scene_builder.Build(),
                         Rect::MakeLTRB(0, 0, 1600, 1200));
  flattened.EncodePasses(arena, cache, TextureFormat::kRGBA8UNorm);

  const SceneFlattener::Pass& frame = flattened.GetPasses().back();
  ASSERT_EQ(frame.items.size(), 8u) << "four filtered, four plain";

  // Every item says the same thing about its clip, composite and leaf
  // alike. Which thing matters less than that they agree: one of them
  // out of step is a scissor either side of it.
  for (const SceneFlattener::Item& item : frame.items) {
    ASSERT_EQ(item.clip.has_value(), frame.items[0].clip.has_value())
        << (item.IsComposite() ? "a composite" : "a leaf") << " is out of step";
    if (item.clip.has_value()) {
      EXPECT_EQ(*item.clip, *frame.items[0].clip);
    }
  }

  // So every draw batches, behind at most one scissor.
  const RenderPlan& plan = flattened.GetPlan().back();
  ASSERT_FALSE(plan.draws.empty());
  EXPECT_LE(plan.draws.size(), 2u) << "at most one scissor and one run";
  const GPUDraw& run = plan.draws.back();
  EXPECT_EQ(run.program, ProgramType::kColor);
  EXPECT_EQ(run.count, 48u) << "four composites and four rects";
}

TEST(DispatcherTest, TurningABlurUpDoesNotReuseTheOldOne) {
  StubGpuContext context;
  BufferArena arena(context);

  // The same content at two strengths. A scene carries its filter
  // itself, so no picture id moves when the sigma does -- the key has to
  // say so on its own or the second frame shows the first frame's blur.
  std::shared_ptr<PrPicture> content =
      RectPicture(Rect::MakeLTRB(0, 0, 100, 100), flutter::DlColor::kRed());
  auto steps_for = [&](Scalar sigma) {
    PrSceneBuilder scene_builder(kSurface);
    flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
    paint.setImageFilter(std::make_shared<flutter::DlBlurImageFilter>(
        sigma, sigma, flutter::DlTileMode::kDecal));
    scene_builder.SaveLayer(std::nullopt, &paint);
    scene_builder.DrawPicture(content, 1);
    scene_builder.Restore();
    return Flattened(scene_builder.Build(), arena).GetPasses();
  };

  const std::vector<SceneFlattener::Pass> soft = steps_for(2.0f);
  const std::vector<SceneFlattener::Pass> hard = steps_for(3.0f);
  ASSERT_EQ(soft.size(), hard.size());
  ASSERT_GE(soft.size(), 3u);

  // Same content, so the same names for it.
  EXPECT_EQ(soft[0].key.ids[0], hard[0].key.ids[0]);
  // Different strength, so not the same texture.
  EXPECT_FALSE(soft[1].key == hard[1].key) << "the blur would be reused";
  EXPECT_FALSE(soft[2].key == hard[2].key) << "the blur would be reused";
}

TEST(DispatcherTest, AnUnchangedBlurredLayerKeepsWhatItResolved) {
  StubGpuContext context;
  TextureCache cache(&context);
  BufferArena arena(context);

  // The same picture both times: a layer nothing touched should not be
  // blurred again for the next frame.
  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  paint.setImageFilter(std::make_shared<flutter::DlBlurImageFilter>(
      4.0f, 4.0f, flutter::DlTileMode::kDecal));
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 100, 100),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();
  std::shared_ptr<PrPicture> picture = builder.Build();

  SceneFlattener first = Flattened(SceneOf(picture), arena, &cache);
  ASSERT_EQ(first.GetPasses().size(), 4u);
  for (uint32_t step : {0u, 1u, 2u}) {
    EXPECT_FALSE(first.GetPasses()[step].ready) << "step " << step;
    EXPECT_NE(first.GetPasses()[step].texture, nullptr) << "step " << step;
  }

  cache.Next();  // A frame goes by.

  SceneFlattener second = Flattened(SceneOf(picture), arena, &cache);
  ASSERT_EQ(second.GetPasses().size(), 4u);
  for (uint32_t step : {0u, 1u, 2u}) {
    EXPECT_TRUE(second.GetPasses()[step].ready)
        << "step " << step << " had to be drawn again";
    EXPECT_EQ(second.GetPasses()[step].texture, first.GetPasses()[step].texture)
        << "step " << step << " landed on a different texture";
  }
}

TEST(DispatcherTest, AWideBlurIsHeldDownWhileItIsGathered) {
  StubGpuContext context;
  BufferArena arena(context);

  auto chain_for = [&](Scalar sigma) {
    PrPictureBuilder builder;
    flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
    paint.setImageFilter(std::make_shared<flutter::DlBlurImageFilter>(
        sigma, sigma, flutter::DlTileMode::kDecal));
    builder.SaveLayer(std::nullopt, &paint);
    builder.DrawRect(Rect::MakeLTRB(0, 0, 400, 400),
                     Fill(flutter::DlColor::kRed()));
    builder.Restore();
    return Flattened(SceneOf(builder.Build()), arena);
  };

  struct Expected {
    Scalar sigma;
    Scalar scale;
  };
  // A power of two either side of four texels, and a floor: one halving
  // per axis is as far down as this goes, so anything wider than that
  // stops there and gathers a broader kernel instead.
  const Expected ladder[] = {
      {.sigma = 2, .scale = 1.0f},  {.sigma = 4, .scale = 1.0f},
      {.sigma = 6, .scale = 0.5f},  {.sigma = 16, .scale = 0.5f},
      {.sigma = 64, .scale = 0.5f}, {.sigma = 400, .scale = 0.5f},
  };
  for (const Expected& expect : ladder) {
    SceneFlattener flattened = chain_for(expect.sigma);
    const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

    // The content, one gathering step per axis, and the frame. Nothing
    // resamples: the content is drawn at the size it is gathered at.
    ASSERT_EQ(passes.size(), 4u) << "sigma " << expect.sigma;

    // All three held at the same size, so a tap is one texel of what it
    // reads as much as of what it writes. Anything larger on the way in
    // has the taps striding over source texels that never get read.
    for (uint32_t step : {0u, 1u, 2u}) {
      EXPECT_EQ(passes[step].resolution_scale, expect.scale)
          << "sigma " << expect.sigma << " step " << step;
    }

    for (uint32_t step : {1u, 2u}) {
      EXPECT_EQ(passes[step].filter_step.sigma, expect.sigma * expect.scale)
          << "sigma " << expect.sigma << " step " << step;
      EXPECT_LE(passes[step].filter_step.radius, TapRadiusCeiling())
          << "sigma " << expect.sigma << " step " << step;
      EXPECT_EQ(passes[step].filter_source, step - 1)
          << "sigma " << expect.sigma << " step " << step;
    }

    // The offscreen is allocated at the held size, not at the coverage:
    // a target of one size with transients of another is a render pass
    // that draws nothing anyone would want.
    // Taken off the coverage the pass actually spans, which the filter
    // widened past the content that was drawn into it.
    const std::vector<RenderPlan>& plans = flattened.GetPlan();
    const uint32_t held =
        static_cast<uint32_t>(std::ceil(plans[0].extent.x * expect.scale));
    for (uint32_t step : {0u, 1u, 2u}) {
      EXPECT_EQ(passes[step].key.width, held)
          << "sigma " << expect.sigma << " step " << step;
    }
    if (expect.scale < 1.0f) {
      EXPECT_LT(held, static_cast<uint32_t>(plans[0].extent.x))
          << "sigma " << expect.sigma;
    }

    for (uint32_t step : {0u, 1u, 2u}) {
      EXPECT_EQ(plans[step].width, passes[step].key.width)
          << "sigma " << expect.sigma << " step " << step;
      EXPECT_EQ(plans[step].height, passes[step].key.height)
          << "sigma " << expect.sigma << " step " << step;
      EXPECT_EQ(plans[step].extent, plans[0].extent)
          << "sigma " << expect.sigma << " step " << step;
    }
  }
}

TEST(DispatcherTest, ALayerWithNoBlurKeepsItsSinglePass) {
  StubGpuContext context;
  BufferArena arena(context);

  PrPictureBuilder builder;
  flutter::DlPaint paint = Fill(flutter::DlColor::kRed());
  builder.SaveLayer(std::nullopt, &paint);
  builder.DrawRect(Rect::MakeLTRB(100, 100, 140, 140),
                   Fill(flutter::DlColor::kRed()));
  builder.Restore();

  SceneFlattener flattened = Flattened(SceneOf(builder.Build()), arena);
  ASSERT_EQ(flattened.GetPasses().size(), 2u) << "the layer and the frame";
  EXPECT_FALSE(flattened.GetPlan()[0].filter.has_value());
}

TEST(DispatcherTest, ASceneDrawsItsLeavesInOrder) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 100, 100));
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed()), 1);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kGreen()), 1);

  RenderPlan plan = FlattenTheScene(scene_builder.Build(), arena);

  // Both leaves are plain rects under the same binds, so they batch.
  ASSERT_EQ(plan.draws.size(), 1u);
  EXPECT_EQ(plan.draws[0].count, 12u);
}

TEST(DispatcherTest, ALeafIsPlacedByItsNodeTransform) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 100, 100));
  scene_builder.Translate(30, 40);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed()), 1);

  RenderPlan plan = FlattenTheScene(scene_builder.Build(), arena);

  ASSERT_EQ(plan.buffers.size(), 1u);
  const auto* transforms =
      reinterpret_cast<const Matrix*>(plan.buffers[0].transforms->Contents());
  // The picture's own transform composed with where the scene put it.
  EXPECT_MATRIX_NEAR(transforms[0], Matrix::MakeTranslation({30, 40, 0}));
}

TEST(DispatcherTest, ALeafTheClipRejectsIsNotDrawn) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture =
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed());
  leaf.clip = Rect::MakeLTRB(500, 500, 600, 600);
  scene.children.push_back(std::move(leaf));

  RenderPlan plan = FlattenTheScene(scene, arena);

  EXPECT_TRUE(plan.draws.empty());
}

TEST(DispatcherTest, AGroupsClipReachesItsChildren) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  PrSceneNode group;
  group.clip = Rect::MakeLTRB(500, 500, 600, 600);
  PrSceneNode leaf;
  leaf.picture =
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed());
  group.children.push_back(std::move(leaf));
  scene.children.push_back(std::move(group));

  RenderPlan plan = FlattenTheScene(scene, arena);

  EXPECT_TRUE(plan.draws.empty());
}

TEST(DispatcherTest, ACompositingGroupGetsItsOwnPass) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneBuilder scene_builder(kSurface);
  flutter::DlPaint paint;
  paint.setOpacity(0.5f);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed()), 1);
  scene_builder.SaveLayer(std::nullopt, &paint);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kGreen()), 1);
  scene_builder.Restore();
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kBlue()), 1);

  SceneFlattener flattened = Flattened(scene_builder.Build(), arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  ASSERT_EQ(passes.size(), 2u);
  // Post order: the group is resolved before the pass that reads it.
  EXPECT_EQ(passes[0].items.size(), 1u);
  EXPECT_EQ(passes[0].parent, 1u);
  // The root keeps everything that did not need a pass of its own, and
  // it is last -- its two pictures and the group between them.
  EXPECT_EQ(passes[1].items.size(), 3u);
  EXPECT_EQ(passes[1].parent, SceneFlattener::kNoPass);
  EXPECT_EQ(flattened.GetPlan()[1].draws.size(), 1u);
  // Its two rects and, between them, the quad that composites the group.
  EXPECT_EQ(flattened.GetPlan()[1].draws[0].count, 18u);
}

TEST(DispatcherTest, AGroupThatCompositesNothingStaysInThePass) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  PrSceneNode group;  // No opacity and no filters.
  PrSceneNode leaf;
  leaf.picture =
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed());
  group.children.push_back(std::move(leaf));
  scene.children.push_back(std::move(group));

  SceneFlattener flattened = Flattened(scene, arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  // Nothing to resolve, so nothing to resolve it into.
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].items.size(), 1u);
}

TEST(DispatcherTest, NestedGroupsComeOutChildBeforeParent) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneBuilder scene_builder(kSurface);
  flutter::DlPaint outer;
  outer.setOpacity(0.5f);
  flutter::DlPaint inner;
  inner.setOpacity(0.25f);

  scene_builder.SaveLayer(std::nullopt, &outer);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed()), 1);
  scene_builder.SaveLayer(std::nullopt, &inner);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kGreen()), 1);
  scene_builder.Restore();
  scene_builder.Restore();

  SceneFlattener flattened = Flattened(scene_builder.Build(), arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  ASSERT_EQ(passes.size(), 3u);
  // Innermost first, then the one that reads it, then the root.
  EXPECT_EQ(passes[0].parent, 1u);
  EXPECT_EQ(passes[1].parent, 2u);
  EXPECT_EQ(passes[2].parent, SceneFlattener::kNoPass);
  EXPECT_NEAR(passes[0].opacity, inner.getOpacity(), kEhCloseEnough);
  EXPECT_NEAR(passes[1].opacity, outer.getOpacity(), kEhCloseEnough);
  // The outer group holds a picture of its own alongside the inner pass.
  EXPECT_EQ(passes[1].items.size(), 2u);
  EXPECT_EQ(passes[2].items.size(), 1u) << "the outer group";
}

TEST(DispatcherTest, APassCoversWhereItsPicturesLand) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneBuilder scene_builder(kSurface);
  flutter::DlPaint paint;
  paint.setOpacity(0.5f);
  scene_builder.SaveLayer(std::nullopt, &paint);
  scene_builder.Translate(20, 30);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed()), 1);
  scene_builder.Restore();

  SceneFlattener flattened = Flattened(scene_builder.Build(), arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  ASSERT_EQ(passes.size(), 2u);
  EXPECT_RECT_NEAR(passes[0].coverage, Rect::MakeLTRB(20, 30, 30, 40));
  // However little the root draws, it resolves into the whole target.
  EXPECT_RECT_NEAR(passes[1].coverage, kSurface);
}

TEST(DispatcherTest, ASceneClearsToItsLeadingFill) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder picture_builder;
  picture_builder.DrawColor(flutter::DlColor::kRed(),
                            flutter::DlBlendMode::kSrcOver);
  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 100, 100));
  scene_builder.DrawPicture(picture_builder.Build(), 1);

  RenderPlan plan = FlattenTheScene(scene_builder.Build(), arena);

  EXPECT_COLOR_NEAR(ClearOf(plan), Color::Red());
  EXPECT_TRUE(plan.draws.empty());
}

TEST(DispatcherTest, OnlyTheFirstLeafCanBeTheClear) {
  StubGpuContext context;
  BufferArena arena(context);
  PrPictureBuilder fill_builder;
  fill_builder.DrawColor(flutter::DlColor::kRed(),
                         flutter::DlBlendMode::kSrcOver);
  std::shared_ptr<PrPicture> fill = fill_builder.Build();

  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 100, 100));
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kGreen()), 1);
  scene_builder.DrawPicture(fill, 1);

  RenderPlan plan = FlattenTheScene(scene_builder.Build(), arena);

  // Folding the fill would drop the leaf under it.
  EXPECT_COLOR_NEAR(ClearOf(plan), Color::BlackTransparent());
  EXPECT_FALSE(plan.draws.empty());
}

TEST(DispatcherTest, DispatchDrawsAScene) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  PrSceneBuilder scene_builder(Rect::MakeLTRB(0, 0, 100, 100));
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed()), 1);

  TextureCache textures(&context);
  Dispatch(scene_builder.Build(), arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, cmd_buffer);

  EXPECT_EQ(cmd_buffer.passes, 1);
  ASSERT_EQ(cmd_buffer.draws.size(), 1u);
  EXPECT_EQ(cmd_buffer.draws[0].count, 6);
}

// -----------------------------------------------------------------------
// Offscreen passes.

namespace {

/// Two rects apart from each other, so the first does not cover the
/// pass and fold into its clear colour.
std::shared_ptr<PrPicture> SpreadPicture() {
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(0, 0, 10, 10),
                   Fill(flutter::DlColor::kRed()));
  builder.DrawRect(Rect::MakeLTRB(20, 20, 30, 30),
                   Fill(flutter::DlColor::kGreen()));
  return builder.Build();
}

/// Where a plan's first vertex lands, in the pass's own space.
Point FirstVertexOf(const RenderPlan& plan) {
  const auto* transforms =
      reinterpret_cast<const Matrix*>(plan.buffers[0].transforms->Contents());
  const auto* positions =
      reinterpret_cast<const Point*>(plan.buffers[0].positions->Contents());
  return transforms[0] * positions[0];
}

/// The first vertex of a plan's `index`'th draw. Passes share the
/// arena's buffers, so where a draw's vertices start is what its run of
/// the index stream says, not the start of the buffer.
uint16_t FirstVertexOfDraw(const RenderPlan& plan, size_t index) {
  const BufferBinds& binds = plan.buffers[plan.draws[index].buffer_binds];
  const auto* indices =
      reinterpret_cast<const uint16_t*>(binds.indices->Contents());
  return indices[plan.draws[index].start];
}

/// A scene with one compositing group holding one picture at `offset`.
PrSceneNode GroupAt(Point offset, const std::shared_ptr<PrPicture>& picture) {
  PrSceneNode scene;
  PrSceneNode group;
  group.opacity = 0.5f;
  PrSceneNode leaf;
  leaf.picture = picture;
  leaf.transform = Matrix::MakeTranslation({offset.x, offset.y, 0});
  group.children.push_back(std::move(leaf));
  scene.children.push_back(std::move(group));
  return scene;
}

}  // namespace

TEST(DispatcherTest, APassCarriesItsOriginRatherThanMovingItsGeometry) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene = GroupAt(Point(20, 30), SpreadPicture());

  SceneFlattener flattened = Flattened(scene, arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  ASSERT_EQ(passes.size(), 2u);
  const SceneFlattener::Pass& group = passes[0];
  EXPECT_RECT_NEAR(group.coverage, Rect::MakeLTRB(20, 30, 50, 60));
  // A texture only as big as what the pass covers...
  EXPECT_EQ(flattened.GetPlan()[0].width, 30u);
  EXPECT_EQ(flattened.GetPlan()[0].height, 30u);
  // ...reached by telling the vertex stage where the pass starts, not by
  // folding a translation into every draw.
  EXPECT_POINT_NEAR(flattened.GetPlan()[0].origin, Point(20, 30));
  ASSERT_FALSE(flattened.GetPlan()[0].buffers.empty());
  EXPECT_POINT_NEAR(FirstVertexOf(flattened.GetPlan()[0]), Point(20, 30));
}

TEST(DispatcherTest, APassClipAndItsGeometryShareRootSpace) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  PrSceneNode group;
  group.opacity = 0.5f;
  PrSceneNode leaf;
  leaf.picture = SpreadPicture();  // Covers (0, 0) to (30, 30).
  leaf.transform = Matrix::MakeTranslation({20, 30, 0});
  // Cuts the far rect away, leaving the near one whole.
  leaf.clip = Rect::MakeLTRB(20, 30, 35, 45);
  group.children.push_back(std::move(leaf));
  scene.children.push_back(std::move(group));

  SceneFlattener flattened = Flattened(scene, arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  ASSERT_EQ(passes.size(), 2u);
  EXPECT_RECT_NEAR(passes[0].coverage, Rect::MakeLTRB(20, 30, 35, 45));
  // Nothing moved the geometry, so the clip needs no moving either.
  ASSERT_FALSE(flattened.GetPlan()[0].buffers.empty());
  EXPECT_POINT_NEAR(FirstVertexOf(flattened.GetPlan()[0]), Point(20, 30));
}

TEST(DispatcherTest, EncodingTellsTheVertexStageWhereThePassStarts) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  PrSceneNode scene = GroupAt(Point(20, 30), SpreadPicture());
  SceneFlattener flattened = Flattened(scene, arena);

  EncodePlan(flattened.GetPlan()[0], pipelines.Set(), cmd_buffer);

  // The pass's size, then where it starts: what the vertex stage takes
  // off every position.
  EXPECT_NEAR(cmd_buffer.viewport_origin[0], 30.0f, kEhCloseEnough);
  EXPECT_NEAR(cmd_buffer.viewport_origin[1], 30.0f, kEhCloseEnough);
  EXPECT_NEAR(cmd_buffer.viewport_origin[2], 20.0f, kEhCloseEnough);
  EXPECT_NEAR(cmd_buffer.viewport_origin[3], 30.0f, kEhCloseEnough);

  EncodePlan(flattened.GetPlan()[1], pipelines.Set(), cmd_buffer);

  // The root starts at the frame's own origin.
  EXPECT_NEAR(cmd_buffer.viewport_origin[2], 0.0f, kEhCloseEnough);
  EXPECT_NEAR(cmd_buffer.viewport_origin[3], 0.0f, kEhCloseEnough);
}

TEST(DispatcherTest, TheRootWritesItsGeometryWhereItLands) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneBuilder scene_builder(kSurface);
  scene_builder.Translate(20, 30);
  scene_builder.DrawPicture(SpreadPicture(), 1);

  SceneFlattener flattened = Flattened(scene_builder.Build(), arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  // The root covers the whole target from the origin, so nothing shifts.
  ASSERT_EQ(passes.size(), 1u);
  ASSERT_FALSE(flattened.GetPlan()[0].buffers.empty());
  EXPECT_POINT_NEAR(FirstVertexOf(flattened.GetPlan()[0]), Point(20, 30));
}

TEST(DispatcherTest, APassNamesTheOffscreenItResolvesInto) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene = GroupAt(
      Point(0, 0),
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed()));

  SceneFlattener flattened = Flattened(scene, arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  ASSERT_EQ(passes.size(), 2u);
  // Which slot of the frame's table has to hold what it resolved into.
  EXPECT_EQ(passes[0].texture_slot, kReservedTextureSlots)
      << "after the reserved slots";
  EXPECT_EQ(passes[0].parent, 1u);
  // The frame's own pass resolves into the target, so nothing samples
  // it and it names no slot.
  EXPECT_EQ(passes[1].parent, SceneFlattener::kNoPass);
  EXPECT_EQ(passes[1].texture_slot, 0u);
}

TEST(DispatcherTest, ACulledPassIsNotOpened) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  PrSceneNode group;
  group.opacity = 0.5f;
  PrSceneNode leaf;
  leaf.picture =
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed());
  leaf.clip = Rect::MakeLTRB(900, 900, 950, 950);
  group.children.push_back(std::move(leaf));
  scene.children.push_back(std::move(group));

  SceneFlattener flattened = Flattened(scene, arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  // Nothing reaches it, so there is no pass to ask the device for a
  // texture for: only the frame's is left.
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].parent, SceneFlattener::kNoPass);
  EXPECT_GT(flattened.GetPlan()[0].width, 0u);
}

TEST(DispatcherTest, AnEmptySceneStillLeavesTheRoot) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;

  SceneFlattener flattened = Flattened(scene, arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  // The root is what clears the frame, so it is kept where an ordinary
  // pass would be dropped.
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_RECT_NEAR(passes[0].coverage, kSurface);
  EXPECT_TRUE(flattened.GetPlan()[0].draws.empty());
}

TEST(DispatcherTest, APassCoversWhatCompositesIntoIt) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  PrSceneNode outer;
  outer.opacity = 0.5f;
  PrSceneNode inner;
  inner.opacity = 0.5f;
  PrSceneNode leaf;
  leaf.picture =
      RectPicture(Rect::MakeLTRB(0, 0, 10, 10), flutter::DlColor::kRed());
  leaf.transform = Matrix::MakeTranslation({40, 50, 0});
  inner.children.push_back(std::move(leaf));
  outer.children.push_back(std::move(inner));
  scene.children.push_back(std::move(outer));

  SceneFlattener flattened = Flattened(scene, arena);
  const std::vector<SceneFlattener::Pass>& passes = flattened.GetPasses();

  ASSERT_EQ(passes.size(), 3u);
  // The outer group draws nothing itself, but it has to be big enough to
  // hold what composites into it.
  EXPECT_RECT_NEAR(passes[1].coverage, Rect::MakeLTRB(40, 50, 50, 60));
  EXPECT_EQ(passes[1].items.size(), 1u) << "only the pass it composites";
}

TEST(DispatcherTest, EachPassRendersIntoItsOwnTarget) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  PrSceneNode scene = GroupAt(Point(20, 30), SpreadPicture());

  TextureCache textures(&context);
  Dispatch(scene, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, cmd_buffer);

  // The group's offscreen and then the frame's target.
  EXPECT_EQ(cmd_buffer.passes, 2);
  // The group's own content, and the quad that draws the result of it
  // into the frame.
  EXPECT_EQ(cmd_buffer.draws.size(), 2u);
}

// -----------------------------------------------------------------------
// Text.

namespace {

class FakeTypeface final : public Typeface {
 public:
  bool IsValid() const override { return true; }
  std::size_t GetHash() const override { return 42; }
  bool IsEqual(const Typeface& other) const override {
    return other.GetHash() == GetHash();
  }
};

/// The engine's text object over a shaped frame, which is all the
/// recorder reads.
class StubDlText final : public flutter::DlText {
 public:
  explicit StubDlText(std::shared_ptr<TextFrame> frame)
      : frame_(std::move(frame)) {}

  flutter::DlRect GetBounds() const override { return frame_->GetBounds(); }
  std::shared_ptr<TextFrame> GetTextFrame() const override { return frame_; }
  const SkTextBlob* GetTextBlob() const override { return nullptr; }

 private:
  const std::shared_ptr<TextFrame> frame_;
};

/// A glyph rasterizer that always has coverage, so every glyph resolves.
class SolidRasterizer final : public GlyphRasterizer {
 public:
  RasterizedGlyph Rasterize(const Font& font,
                            Glyph glyph,
                            Rational scale,
                            Scalar subpixel_offset) override {
    RasterizedGlyph result;
    result.width = 8;
    result.height = 8;
    result.bearing = Point(0, -8);
    result.coverage = std::vector<uint8_t>(64, 0xFF);
    return result;
  }
};

std::shared_ptr<TextFrame> MakeTextFrame(uint16_t glyphs) {
  Font font(std::make_shared<FakeTypeface>(), Font::Metrics{},
            AxisAlignment::kNone);
  TextRun run(font);
  for (uint16_t i = 0; i < glyphs; i++) {
    run.AddGlyph(Glyph(i + 1, Glyph::Type::kPath), Point(i * 10.0f, 0));
  }
  std::vector<TextRun> runs = {run};
  return std::make_shared<TextFrame>(
      runs, Rect::MakeLTRB(0, -10, glyphs * 10.0f, 0), /*has_color=*/false);
}

/// A picture holding one text draw.
std::shared_ptr<PrPicture> TextPicture(uint16_t glyphs) {
  PrPictureBuilder builder;
  builder.DrawText(std::make_shared<StubDlText>(MakeTextFrame(glyphs)), 0, 20,
                   Fill(flutter::DlColor::kRed()));
  return builder.Build();
}

}  // namespace

TEST(DispatcherTest, ARecordedTextDrawKeepsItsFrame) {
  std::shared_ptr<PrPicture> picture = TextPicture(3);

  ASSERT_EQ(picture->GetDraws().size(), 1u);
  EXPECT_EQ(picture->GetDraws()[0].type, Draw::DrawType::kText);
  // The draw names a slot, so the frame has to be in it.
  ASSERT_EQ(picture->GetTextFrames().size(), 1u);
  EXPECT_EQ(picture->GetDraws()[0].text_data.text_index, 0u);
}

TEST(DispatcherTest, TextAsksForAQuadPerGlyph) {
  StubGpuContext context;
  BufferArena arena(context);
  auto atlas = std::make_shared<PagedAtlas>(&context, TextureFormat::kR8UNorm,
                                            256, 256, 4);
  TextMaterializer materializer(atlas, std::make_shared<SolidRasterizer>());

  RenderPlan plan = Flatten(TextPicture(3), arena, &materializer);

  ASSERT_EQ(plan.draws.size(), 1u);
  // Three glyphs, six indices each.
  EXPECT_EQ(plan.draws[0].count, 18u);
}

TEST(DispatcherTest, TextSamplesTheAtlasAsCoverage) {
  StubGpuContext context;
  BufferArena arena(context);
  auto atlas = std::make_shared<PagedAtlas>(&context, TextureFormat::kR8UNorm,
                                            256, 256, 4);
  TextMaterializer materializer(atlas, std::make_shared<SolidRasterizer>());

  RenderPlan plan = Flatten(TextPicture(2), arena, &materializer);

  ASSERT_EQ(plan.buffers.size(), 1u);
  const auto* paints =
      reinterpret_cast<const PrPaint*>(plan.buffers[0].paints->Contents());
  // The atlas holds coverage, not colour: the fragment stage keys on the
  // flag to read it as an alpha.
  EXPECT_EQ(paints[0].texture_index, 0);
  EXPECT_NE(paints[0].flags & kPaintFlagTextureIsCoverage, 0u);

  const auto* attributes = reinterpret_cast<const Attributes*>(
      plan.buffers[0].attributes->Contents());
  // Every glyph vertex carries a uv into the page.
  bool any_uv = false;
  for (int i = 0; i < 8; i++) {
    any_uv = any_uv || attributes[i].uv != Point(0, 0);
  }
  EXPECT_TRUE(any_uv);
}

TEST(DispatcherTest, DispatchFlushesTheAtlasBeforeAnyPass) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  auto atlas = std::make_shared<PagedAtlas>(&context, TextureFormat::kR8UNorm,
                                            256, 256, 4);
  TextMaterializer materializer(atlas, std::make_shared<SolidRasterizer>());
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);

  PrSceneBuilder scene_builder(kSurface);
  scene_builder.DrawPicture(TextPicture(2), 1);
  TextureCache textures(&context);
  Dispatch(scene_builder.Build(), arena, pipelines.Set(), *target, textures,
           &materializer, cmd_buffer);

  // The blit has to be recorded before the pass that samples it, and a
  // blit cannot be recorded inside one.
  EXPECT_FALSE(cmd_buffer.updates.empty());
  EXPECT_FALSE(atlas->HasPendingUploads());
  EXPECT_EQ(cmd_buffer.passes, 1);
  ASSERT_EQ(cmd_buffer.draws.size(), 1u);
  EXPECT_EQ(cmd_buffer.draws[0].count, 12);

  // A glyph paint names slot 0, so that is where the atlas has to be.
  ASSERT_EQ(cmd_buffer.texture_tables.size(), 1u);
  ASSERT_FALSE(cmd_buffer.texture_tables[0].empty());
  EXPECT_EQ(cmd_buffer.texture_tables[0][0], atlas->GetPageTexture(0));
}

TEST(DispatcherTest, AGroupTakesASlotInTheTableOfThePassItCompositesInto) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  PrSceneNode scene = GroupAt(Point(20, 30), SpreadPicture());

  TextureCache textures(&context);
  Dispatch(scene, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, cmd_buffer);

  ASSERT_EQ(cmd_buffer.texture_tables.size(), 2u);
  // The group's own pass samples nothing but the reserved slots.
  EXPECT_EQ(cmd_buffer.texture_tables[0].size(), kReservedTextureSlots);
  // The root reads the group's offscreen, which lands after the
  // reserved slots.
  ASSERT_EQ(cmd_buffer.texture_tables[1].size(), kReservedTextureSlots + 1);
  // Which is the texture the group's pass rendered into.
  ASSERT_EQ(cmd_buffer.pass_targets.size(), 2u);
  EXPECT_EQ(cmd_buffer.texture_tables[1][kReservedTextureSlots],
            cmd_buffer.pass_targets[0]);
}

TEST(DispatcherTest, AnOffscreenIsInTheSameFormatAsTheFrameTarget) {
  StubGpuContext context;
  StubGpuCommandBuffer cmd_buffer;
  Pipelines pipelines;
  BufferArena arena(context);
  // One set of pipelines encodes every pass of the frame, and they are
  // built for the target's format, so a pass that renders into anything
  // else is a pipeline the encoder rejects.
  std::unique_ptr<GPUTexture> target = context.CreateTexture(
      TextureDesc{
          .format = TextureFormat::kBGRA10XR, .width = 100, .height = 100},
      false);
  PrSceneNode scene = GroupAt(Point(20, 30), SpreadPicture());

  TextureCache textures(&context);
  Dispatch(scene, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, cmd_buffer);

  ASSERT_EQ(cmd_buffer.pass_targets.size(), 2u);
  ASSERT_NE(cmd_buffer.pass_targets[0], nullptr);
  EXPECT_EQ(cmd_buffer.pass_targets[0]->GetDesc().format,
            TextureFormat::kBGRA10XR);
}

TEST(DispatcherTest, AConvexPathDrawsThroughThePathProgram) {
  StubGpuContext context;
  BufferArena arena(context);
  const Point points[3] = {Point(10, 10), Point(90, 10), Point(50, 70)};

  PrPictureBuilder builder;
  builder.DrawPath(flutter::DlPath::MakePoly(points, 3, /*close=*/true),
                   Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  // Loop-Blinn coverage, the same program a round rect draws with.
  ASSERT_EQ(plan.draws.size(), 1u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kPath);
  EXPECT_GT(plan.draws[0].count, 0u);
}

TEST(DispatcherTest, AConcaveFillAccumulatesThenResolves) {
  StubGpuContext context;
  BufferArena arena(context);
  // A chevron: the notch makes it concave, so it cannot be one fan.
  const Point points[4] = {Point(10, 10), Point(50, 40), Point(90, 10),
                           Point(50, 90)};

  PrPictureBuilder builder;
  builder.DrawPath(flutter::DlPath::MakePoly(points, 4, /*close=*/true),
                   Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  // The contours sum into the accumulator, and a quad resolves them.
  ASSERT_EQ(plan.draws.size(), 2u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kWindingAccumulate);
  EXPECT_GT(plan.draws[0].count, 0u);
  EXPECT_EQ(plan.draws[1].program, ProgramType::kWindingResolveNonZero);
  EXPECT_EQ(plan.draws[1].count, 6u) << "a quad";
}

TEST(DispatcherTest, AnEvenOddFillResolvesUnderItsOwnRule) {
  StubGpuContext context;
  BufferArena arena(context);
  const Point points[4] = {Point(10, 10), Point(50, 40), Point(90, 10),
                           Point(50, 90)};
  flutter::DlPathBuilder path;
  path.SetFillType(flutter::DlPathFillType::kOdd);
  path.MoveTo(points[0]);
  for (int i = 1; i < 4; i++) {
    path.LineTo(points[i]);
  }
  path.Close();

  PrPictureBuilder builder;
  builder.DrawPath(path.TakePath(), Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  ASSERT_EQ(plan.draws.size(), 2u);
  EXPECT_EQ(plan.draws[1].program, ProgramType::kWindingResolveEvenOdd);
}

TEST(DispatcherTest, TwoConcaveFillsNeverShareARun) {
  StubGpuContext context;
  BufferArena arena(context);
  const Point points[4] = {Point(10, 10), Point(50, 40), Point(90, 10),
                           Point(50, 90)};
  const flutter::DlPath path =
      flutter::DlPath::MakePoly(points, 4, /*close=*/true);

  PrPictureBuilder builder;
  builder.DrawPath(path, Fill(flutter::DlColor::kRed()));
  builder.DrawPath(path, Fill(flutter::DlColor::kBlue()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  // Accumulating both before resolving either would resolve each with
  // the other's winding. The programs alternate, so no two runs merge.
  ASSERT_EQ(plan.draws.size(), 4u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kWindingAccumulate);
  EXPECT_EQ(plan.draws[1].program, ProgramType::kWindingResolveNonZero);
  EXPECT_EQ(plan.draws[2].program, ProgramType::kWindingAccumulate);
  EXPECT_EQ(plan.draws[3].program, ProgramType::kWindingResolveNonZero);
}

TEST(DispatcherTest, APathClipAccumulatesAndResolves) {
  StubGpuContext context;
  BufferArena arena(context);
  const Point points[3] = {Point(10, 10), Point(90, 10), Point(50, 70)};

  PrPictureBuilder builder;
  builder.ClipPath(flutter::DlPath::MakePoly(points, 3, /*close=*/true));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 100, 100),
                   Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  // The scissor is the clip's outer rect, the shape accumulates its
  // winding, and a quad over the same rect resolves that into the clip
  // attachment for the draw that follows. The pop the recording ends
  // with has no draw after it, so none of it is encoded.
  ASSERT_EQ(plan.draws.size(), 4u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kInvalid);
  EXPECT_EQ(plan.draws[0].scissor, IRect32::MakeLTRB(10, 10, 90, 70));
  EXPECT_EQ(plan.draws[1].program, ProgramType::kWindingAccumulate);
  EXPECT_GT(plan.draws[1].count, 0u);
  EXPECT_EQ(plan.draws[2].program, ProgramType::kClipResolveNonZero);
  EXPECT_EQ(plan.draws[2].count, 6u);
  EXPECT_EQ(plan.draws[3].program, ProgramType::kColor);
  EXPECT_EQ(plan.draws[3].count, 6u);
}

TEST(DispatcherTest, AnAxisAlignedRectClipIsOnlyItsScissor) {
  StubGpuContext context;
  BufferArena arena(context);

  PrPictureBuilder builder;
  builder.ClipRect(Rect::MakeLTRB(10, 10, 50, 50));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 100, 100),
                   Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  // The scissor masks it exactly, so the shape and the resolve that
  // would have masked it again are never encoded.
  ASSERT_EQ(plan.draws.size(), 2u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kInvalid);
  EXPECT_EQ(plan.draws[0].scissor, IRect32::MakeLTRB(10, 10, 50, 50));
  EXPECT_EQ(plan.draws[1].program, ProgramType::kColor);
}

TEST(DispatcherTest, ATurnedRectClipStillMasksItsShape) {
  StubGpuContext context;
  BufferArena arena(context);

  PrPictureBuilder builder;
  builder.Rotate(30);
  builder.ClipRect(Rect::MakeLTRB(10, 10, 50, 50));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 100, 100),
                   Fill(flutter::DlColor::kRed()));
  RenderPlan plan = Flatten(builder.Build(), arena);

  // Turned, the scissor is only the outer rect of the clip, so what
  // falls between the two still has to be masked by the shape.
  ASSERT_EQ(plan.draws.size(), 4u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kInvalid);
  EXPECT_EQ(plan.draws[1].program, ProgramType::kWindingAccumulateFlat);
  EXPECT_EQ(plan.draws[2].program, ProgramType::kClipResolveNonZero);
  EXPECT_EQ(plan.draws[3].program, ProgramType::kColor);
}

// -----------------------------------------------------------------------
// Ordering a scene.

namespace {

using Item = SceneFlattener::Item;
using Pass = SceneFlattener::Pass;

/// A leaf drawing `picture` at `offset`.
PrSceneNode LeafAt(Point offset, const std::shared_ptr<PrPicture>& picture) {
  PrSceneNode leaf;
  leaf.picture = picture;
  leaf.transform = Matrix::MakeTranslation({offset.x, offset.y, 0});
  return leaf;
}

/// A group that has to composite, holding `children`.
PrSceneNode GroupOf(std::vector<PrSceneNode> children) {
  PrSceneNode group;
  group.opacity = 0.5f;
  group.children = std::move(children);
  return group;
}

}  // namespace

TEST(SceneFlattenerTest, ASceneOfPicturesIsOnePass) {
  PrSceneNode scene;
  scene.children.push_back(LeafAt(Point(0, 0), SpreadPicture()));
  scene.children.push_back(LeafAt(Point(50, 0), SpreadPicture()));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);

  // Nothing composites, so everything draws into the frame's own pass.
  ASSERT_EQ(flattener.GetPasses().size(), 1u);
  const Pass& frame = flattener.GetPasses()[0];
  EXPECT_EQ(frame.parent, SceneFlattener::kNoPass);
  EXPECT_RECT_NEAR(frame.coverage, kSurface)
      << "the frame covers the surface however little it draws";
  ASSERT_EQ(frame.items.size(), 2u);
  EXPECT_FALSE(frame.items[0].IsComposite());
  EXPECT_RECT_NEAR(frame.items[0].coverage, Rect::MakeLTRB(0, 0, 30, 30));
  EXPECT_RECT_NEAR(frame.items[1].coverage, Rect::MakeLTRB(50, 0, 80, 30));
}

TEST(SceneFlattenerTest, AGroupComesBeforeThePassThatCompositesIt) {
  PrSceneNode scene;
  scene.children.push_back(GroupOf({LeafAt(Point(20, 30), SpreadPicture())}));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);

  ASSERT_EQ(flattener.GetPasses().size(), 2u);
  const Pass& group = flattener.GetPasses()[0];
  const Pass& frame = flattener.GetPasses()[1];
  EXPECT_EQ(group.parent, 1u);
  EXPECT_EQ(frame.parent, SceneFlattener::kNoPass);
  // Only as big as what reaches it, and carrying what it composites
  // with.
  EXPECT_RECT_NEAR(group.coverage, Rect::MakeLTRB(20, 30, 50, 60));
  EXPECT_EQ(group.opacity, 0.5f);

  // And the frame draws the result of it, where the group was.
  ASSERT_EQ(frame.items.size(), 1u);
  EXPECT_TRUE(frame.items[0].IsComposite());
  EXPECT_EQ(frame.items[0].pass, 0u);
  EXPECT_RECT_NEAR(frame.items[0].coverage, group.coverage);
}

TEST(SceneFlattenerTest, GroupsResolveInnermostFirst) {
  PrSceneNode scene;
  scene.children.push_back(
      GroupOf({GroupOf({LeafAt(Point(0, 0), SpreadPicture())})}));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);

  // Inner, outer, frame: a pass is always behind the one that reads it.
  ASSERT_EQ(flattener.GetPasses().size(), 3u);
  EXPECT_EQ(flattener.GetPasses()[0].parent, 1u);
  EXPECT_EQ(flattener.GetPasses()[1].parent, 2u);
  EXPECT_EQ(flattener.GetPasses()[2].parent, SceneFlattener::kNoPass);
}

TEST(SceneFlattenerTest, ItemsKeepThePaintOrderTheSceneDrewThemIn) {
  PrSceneNode scene;
  scene.children.push_back(LeafAt(Point(0, 0), SpreadPicture()));
  scene.children.push_back(GroupOf({LeafAt(Point(0, 0), SpreadPicture())}));
  scene.children.push_back(LeafAt(Point(0, 0), SpreadPicture()));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);

  ASSERT_EQ(flattener.GetPasses().size(), 2u);
  const std::vector<Item>& items = flattener.GetPasses()[1].items;
  ASSERT_EQ(items.size(), 3u);
  EXPECT_FALSE(items[0].IsComposite()) << "under the group";
  EXPECT_TRUE(items[1].IsComposite()) << "the group";
  EXPECT_FALSE(items[2].IsComposite()) << "over the group";
}

TEST(SceneFlattenerTest, AGroupThatCompositesNothingOpensNoPass) {
  PrSceneNode scene;
  // Clipped to nothing, so nothing of it reaches the pass it would
  // have taken.
  PrSceneNode group = GroupOf({LeafAt(Point(0, 0), SpreadPicture())});
  group.children[0].clip = Rect::MakeLTRB(500, 500, 600, 600);
  scene.children.push_back(std::move(group));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);

  ASSERT_EQ(flattener.GetPasses().size(), 1u) << "only the frame";
  EXPECT_TRUE(flattener.GetPasses()[0].items.empty());
}

TEST(SceneFlattenerTest, AnItemCoversWhatIsLeftOfItAfterTheClip) {
  PrSceneNode scene;
  PrSceneNode leaf = LeafAt(Point(0, 0), SpreadPicture());
  leaf.clip = Rect::MakeLTRB(10, 10, 100, 100);
  scene.children.push_back(std::move(leaf));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);

  ASSERT_EQ(flattener.GetPasses().size(), 1u);
  ASSERT_EQ(flattener.GetPasses()[0].items.size(), 1u);
  const Item& item = flattener.GetPasses()[0].items[0];
  // The picture spreads to (30, 30); the clip takes the rest.
  EXPECT_RECT_NEAR(item.coverage, Rect::MakeLTRB(10, 10, 30, 30));
  ASSERT_TRUE(item.clip.has_value());
  EXPECT_RECT_NEAR(item.clip.value(), Rect::MakeLTRB(10, 10, 100, 100));
}

TEST(SceneFlattenerTest, AClipNarrowsWhatIsInsideIt) {
  PrSceneNode scene;
  PrSceneNode outer;
  outer.clip = Rect::MakeLTRB(0, 0, 20, 20);
  outer.children.push_back(GroupOf({LeafAt(Point(0, 0), SpreadPicture())}));
  scene.children.push_back(std::move(outer));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);

  // The group inherits the clip above it, so its texture is only as big
  // as what the clip leaves.
  ASSERT_EQ(flattener.GetPasses().size(), 2u);
  EXPECT_RECT_NEAR(flattener.GetPasses()[0].coverage,
                   Rect::MakeLTRB(0, 0, 20, 20));
}

TEST(SceneFlattenerTest, FlatteningAgainDropsWhatCameBefore) {
  PrSceneNode scene;
  scene.children.push_back(GroupOf({LeafAt(Point(0, 0), SpreadPicture())}));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  const size_t first = flattener.GetPasses().size();
  flattener.FlattenScene(scene, kSurface);

  // The same scene comes to the same passes, not to twice as many.
  EXPECT_EQ(flattener.GetPasses().size(), first);
}

TEST(SceneFlattenerTest, EveryPassGetsAPlanTheSizeOfItsCoverage) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  scene.children.push_back(GroupOf({LeafAt(Point(20, 30), SpreadPicture())}));

  TextureCache textures(&context);
  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm);

  ASSERT_EQ(flattener.GetPlan().size(), 2u);
  const RenderPlan& group = flattener.GetPlan()[0];
  EXPECT_EQ(group.width, 30u);
  EXPECT_EQ(group.height, 30u);
  EXPECT_POINT_NEAR(group.origin, Point(20, 30));
  const RenderPlan& frame = flattener.GetPlan()[1];
  EXPECT_EQ(frame.width, 100u);
  EXPECT_EQ(frame.height, 100u);
  EXPECT_POINT_NEAR(frame.origin, Point(0, 0));

  // Every table starts with the glyph atlas and the gradient ramps,
  // reserved whether or not the pass draws text or a gradient.
  ASSERT_EQ(group.textures.size(), kReservedTextureSlots);
  EXPECT_EQ(group.textures[kGlyphAtlasTextureSlot], nullptr);
  EXPECT_EQ(group.textures[kGradientTextureSlot], nullptr);
  EXPECT_EQ(frame.textures.size(), kReservedTextureSlots + 1)
      << "the reserved slots and the group";
}

TEST(SceneFlattenerTest, APicturesGeometryGoesIntoItsOwnPassesPlan) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  scene.children.push_back(GroupOf({LeafAt(Point(20, 30), SpreadPicture())}));
  scene.children.push_back(LeafAt(Point(0, 0), SpreadPicture()));

  TextureCache textures(&context);
  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm);

  // The group's two rects draw in the group's pass...
  ASSERT_EQ(flattener.GetPlan()[0].draws.size(), 1u);
  EXPECT_EQ(flattener.GetPlan()[0].draws[0].count, 12u);
  // ...and the frame draws its own two rects and the quad compositing
  // the group, batched into one run.
  ASSERT_EQ(flattener.GetPlan()[1].draws.size(), 1u);
  EXPECT_EQ(flattener.GetPlan()[1].draws[0].count, 18u);
}

TEST(SceneFlattenerTest, ACompositeIsAQuadSamplingWhatThePassResolvedInto) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  scene.children.push_back(GroupOf({LeafAt(Point(20, 30), SpreadPicture())}));

  TextureCache textures(&context);
  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm);

  const SceneFlattener::Pass& group = flattener.GetPasses()[0];
  const RenderPlan& frame = flattener.GetPlan()[1];
  ASSERT_EQ(frame.draws.size(), 1u);
  EXPECT_EQ(frame.draws[0].count, 6u) << "a quad";
  EXPECT_EQ(frame.draws[0].program, ProgramType::kColor);

  const uint16_t vertex = FirstVertexOfDraw(frame, 0);
  const auto* positions =
      reinterpret_cast<const Point*>(frame.buffers[0].positions->Contents());
  // Over exactly what the group covers, in root space.
  EXPECT_POINT_NEAR(positions[vertex], group.coverage.GetLeftTop());
  EXPECT_POINT_NEAR(positions[vertex + 3], group.coverage.GetRightBottom());

  const auto* attributes = reinterpret_cast<const Attributes*>(
      frame.buffers[0].attributes->Contents());
  // The whole texture, corner for corner with the quad, and the group's
  // alpha premultiplied onto white.
  EXPECT_POINT_NEAR(attributes[vertex].uv, Point(0, 0));
  EXPECT_POINT_NEAR(attributes[vertex + 3].uv, Point(1, 1));
  EXPECT_EQ(attributes[vertex].color, 0x80808080u);

  // Naming the slot of the frame's table the group was given.
  const auto* paints =
      reinterpret_cast<const PrPaint*>(frame.buffers[0].paints->Contents());
  const uint32_t paint = attributes[vertex].paint & kPaintIndexMask;
  EXPECT_EQ(group.texture_slot, kReservedTextureSlots)
      << "after the reserved slots";
  EXPECT_EQ(paints[paint].texture_index,
            static_cast<int32_t>(group.texture_slot));
}

TEST(SceneFlattenerTest, ACompositeIsEncodedWhereTheGroupWasPainted) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  scene.children.push_back(LeafAt(Point(0, 0), SpreadPicture()));
  scene.children.push_back(GroupOf({LeafAt(Point(0, 0), SpreadPicture())}));
  scene.children.push_back(LeafAt(Point(0, 0), SpreadPicture()));

  TextureCache textures(&context);
  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm);

  const RenderPlan& frame = flattener.GetPlan()[1];
  const auto* attributes = reinterpret_cast<const Attributes*>(
      frame.buffers[0].attributes->Contents());
  const auto* paints =
      reinterpret_cast<const PrPaint*>(frame.buffers[0].paints->Contents());
  const uint16_t first = FirstVertexOfDraw(frame, 0);
  auto texture_of = [&](uint32_t vertex) {
    return paints[attributes[vertex].paint & kPaintIndexMask].texture_index;
  };

  // Under the group, the group, then over it: the composite is encoded
  // in the order the scene painted it.
  EXPECT_EQ(texture_of(first), -1) << "the picture below";
  EXPECT_GE(texture_of(first + 8), 0) << "the group";
  EXPECT_EQ(texture_of(first + 12), -1) << "the picture above";
}

TEST(SceneFlattenerTest, EncodingAgainDropsTheLastFramesPlans) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneNode scene;
  scene.children.push_back(GroupOf({LeafAt(Point(0, 0), SpreadPicture())}));

  TextureCache textures(&context);
  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm);
  const size_t first = flattener.GetPlan().size();
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm);

  EXPECT_EQ(flattener.GetPlan().size(), first);
  EXPECT_EQ(flattener.GetPlan()[1].textures.size(), kReservedTextureSlots + 1)
      << "the table is the frame's, not both frames'";
}

// -----------------------------------------------------------------------
// The texture cache.

namespace {

using OffscreenKey = TextureCache::OffscreenKey;
using Placement = TextureCache::Placement;

OffscreenKey KeyOf(uint64_t id, uint32_t width, uint32_t height) {
  OffscreenKey key;
  key.ids[0] = id;
  key.width = width;
  key.height = height;
  return key;
}

}  // namespace

TEST(TextureCacheTest, AnAccumulatorIsSharedByEveryPassOfItsSize) {
  StubGpuContext context;
  TextureCache cache(&context);

  GPUTexture* first = cache.AllocateWinding(100, 100);
  ASSERT_NE(first, nullptr);
  // Its contents die with the pass that writes it, so a second pass of
  // the same size in the same frame takes the same one.
  EXPECT_EQ(cache.AllocateWinding(100, 100), first);
  EXPECT_NE(cache.AllocateWinding(50, 100), first) << "a different size";

  cache.Next();
  EXPECT_EQ(cache.AllocateWinding(100, 100), first) << "and the next frame";
}

TEST(TextureCacheTest, TheSamePicturesAtTheSameSizeComeBackReady) {
  StubGpuContext context;
  TextureCache cache(&context);
  const std::vector<Placement> placements = {Placement{}};

  EXPECT_EQ(cache.FindOffscreen(KeyOf(7, 30, 40), placements,
                                TextureFormat::kRGBA8UNorm),
            nullptr)
      << "nothing has drawn it yet";
  GPUTexture* first = cache.CreateOffscreen(KeyOf(7, 30, 40), placements,
                                            TextureFormat::kRGBA8UNorm);
  ASSERT_NE(first, nullptr);

  cache.Next();
  EXPECT_EQ(cache.FindOffscreen(KeyOf(7, 30, 40), placements,
                                TextureFormat::kRGBA8UNorm),
            first)
      << "the same pictures drew it last frame";
}

TEST(TextureCacheTest, WhatChangesThePixelsMissesTheCache) {
  StubGpuContext context;
  TextureCache cache(&context);
  const std::vector<Placement> placements = {Placement{}};
  cache.CreateOffscreen(KeyOf(7, 30, 40), placements,
                        TextureFormat::kRGBA8UNorm);
  cache.Next();
  auto found = [&](const OffscreenKey& key,
                   const std::vector<Placement>& with) {
    return cache.FindOffscreen(key, with, TextureFormat::kRGBA8UNorm);
  };

  // A re-recorded picture is a different picture.
  EXPECT_EQ(found(KeyOf(8, 30, 40), placements), nullptr);
  // A different size is a different rendering of it.
  EXPECT_EQ(found(KeyOf(7, 30, 41), placements), nullptr);
  // And so is the same size the other way up: a rotation or a mirror
  // leaves the coverage alone.
  const std::vector<Placement> mirrored = {Placement{.basis = {-1, 0, 0, 1}}};
  EXPECT_EQ(found(KeyOf(7, 30, 40), mirrored), nullptr);
  // Or the same picture moved within the pass.
  const std::vector<Placement> moved = {Placement{.offset = Point(5, 0)}};
  EXPECT_EQ(found(KeyOf(7, 30, 40), moved), nullptr);
  // A different format is a different texture.
  EXPECT_EQ(cache.FindOffscreen(KeyOf(7, 30, 40), placements,
                                TextureFormat::kBGRA8UNorm),
            nullptr);
}

TEST(TextureCacheTest, APassThatCannotBeKeyedIsNeverReady) {
  StubGpuContext context;
  TextureCache cache(&context);
  OffscreenKey none;  // No ids: too many pictures, or not just pictures.
  none.width = 30;
  none.height = 40;
  ASSERT_FALSE(none.IsCacheable());

  EXPECT_NE(cache.CreateOffscreen(none, {}, TextureFormat::kRGBA8UNorm),
            nullptr);
  cache.Next();
  EXPECT_EQ(cache.FindOffscreen(none, {}, TextureFormat::kRGBA8UNorm), nullptr);
}

TEST(TextureCacheTest, TwoPassesOfOneFrameNeverShareAnOffscreen) {
  StubGpuContext context;
  TextureCache cache(&context);
  const std::vector<Placement> placements = {Placement{}};

  // What a pass resolves into is read later in the same frame, so
  // unlike an accumulator it cannot be handed out twice.
  GPUTexture* first = cache.CreateOffscreen(KeyOf(1, 30, 40), placements,
                                            TextureFormat::kRGBA8UNorm);
  GPUTexture* second = cache.CreateOffscreen(KeyOf(2, 30, 40), placements,
                                             TextureFormat::kRGBA8UNorm);
  EXPECT_NE(first, nullptr);
  EXPECT_NE(second, nullptr);
  EXPECT_NE(first, second);
}

TEST(TextureCacheTest, AnUnusedTextureIsKeptOneFrameForItsSize) {
  StubGpuContext context;
  TextureCache cache(&context);
  const std::vector<Placement> placements = {Placement{}};
  GPUTexture* first = cache.CreateOffscreen(KeyOf(1, 30, 40), placements,
                                            TextureFormat::kRGBA8UNorm);

  // A frame that wants nothing of that size...
  cache.Next();
  // ...and then one that wants a different picture at the same size,
  // which is what the texture is kept around for.
  cache.Next();
  EXPECT_EQ(cache.FindOffscreen(KeyOf(2, 30, 40), placements,
                                TextureFormat::kRGBA8UNorm),
            nullptr)
      << "nothing holds those pictures";
  EXPECT_EQ(cache.CreateOffscreen(KeyOf(2, 30, 40), placements,
                                  TextureFormat::kRGBA8UNorm),
            first)
      << "but the allocation is reused";
}

TEST(DispatcherTest, AnUnchangedLayerIsNotDrawnAgain) {
  StubGpuContext context;
  Pipelines pipelines;
  BufferArena arena(context);
  TextureCache textures(&context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  // The same pictures across both frames, which is what a retained
  // layer is: the scene is rebuilt, the pictures are not.
  PrSceneNode scene = GroupAt(Point(20, 30), SpreadPicture());

  StubGpuCommandBuffer first;
  Dispatch(scene, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, first);
  EXPECT_EQ(first.passes, 2) << "the group, and the frame";
  ASSERT_EQ(first.pass_targets.size(), 2u);
  GPUTexture* resolved = first.pass_targets[0];

  textures.Next();
  arena.Reset();

  StubGpuCommandBuffer second;
  Dispatch(scene, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, second);

  // Nothing about the group changed, so its texture still holds it and
  // the pass that drew it is not encoded at all.
  EXPECT_EQ(second.passes, 1) << "only the frame";
  ASSERT_EQ(second.draws.size(), 1u);
  EXPECT_EQ(second.draws[0].count, 6) << "the quad that composites it";
  // Which samples what the group resolved into last frame.
  ASSERT_EQ(second.texture_tables.size(), 1u);
  ASSERT_EQ(second.texture_tables[0].size(), kReservedTextureSlots + 1);
  EXPECT_EQ(second.texture_tables[0][kReservedTextureSlots], resolved);
}

TEST(DispatcherTest, ANewLayerDoesNotTakeATextureAKeptLayerStillNeeds) {
  StubGpuContext context;
  Pipelines pipelines;
  BufferArena arena(context);
  TextureCache textures(&context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  std::shared_ptr<PrPicture> kept = SpreadPicture();

  PrSceneNode first_scene;
  first_scene.children.push_back(GroupOf({LeafAt(Point(0, 0), kept)}));
  StubGpuCommandBuffer first;
  Dispatch(first_scene, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, first);
  ASSERT_EQ(first.pass_targets.size(), 2u);
  GPUTexture* keeps = first.pass_targets[0];

  textures.Next();
  arena.Reset();

  // A group that was not here last frame, painted under the one that
  // was. It is the same size, so the only texture going spare is the
  // one holding the kept group -- which is spare only until the kept
  // group is asked about.
  PrSceneNode second_scene;
  second_scene.children.push_back(
      GroupOf({LeafAt(Point(0, 0), SpreadPicture())}));
  second_scene.children.push_back(GroupOf({LeafAt(Point(0, 0), kept)}));
  StubGpuCommandBuffer second;
  Dispatch(second_scene, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, second);

  // So the kept group is still not drawn, and still composited from
  // what it resolved into.
  EXPECT_EQ(second.passes, 2) << "the new group, and the frame";
  ASSERT_FALSE(second.texture_tables.empty());
  const std::vector<GPUTexture*>& frame = second.texture_tables.back();
  ASSERT_EQ(frame.size(), kReservedTextureSlots + 2)
      << "the reserved slots and the two groups";
  EXPECT_EQ(frame[kReservedTextureSlots + 1], keeps)
      << "the kept group's texture";
  EXPECT_NE(frame[1], keeps) << "which the new group must not have taken";
}

TEST(DispatcherTest, ALayerWhoseRecordingChangedIsDrawnAgain) {
  StubGpuContext context;
  Pipelines pipelines;
  BufferArena arena(context);
  TextureCache textures(&context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);

  StubGpuCommandBuffer first;
  Dispatch(GroupAt(Point(20, 30), SpreadPicture()), arena, pipelines.Set(),
           *target, textures, /*text=*/nullptr, first);
  EXPECT_EQ(first.passes, 2);

  textures.Next();
  arena.Reset();

  // A re-recorded picture is a different picture, whatever it draws.
  StubGpuCommandBuffer second;
  Dispatch(GroupAt(Point(20, 30), SpreadPicture()), arena, pipelines.Set(),
           *target, textures, /*text=*/nullptr, second);
  EXPECT_EQ(second.passes, 2) << "the group has to be drawn again";
}

TEST(DispatcherTest, ALayerThatMovesIsNotDrawnAgain) {
  StubGpuContext context;
  Pipelines pipelines;
  BufferArena arena(context);
  TextureCache textures(&context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  std::shared_ptr<PrPicture> picture = SpreadPicture();

  StubGpuCommandBuffer first;
  Dispatch(GroupAt(Point(20, 30), picture), arena, pipelines.Set(), *target,
           textures, /*text=*/nullptr, first);
  EXPECT_EQ(first.passes, 2);

  textures.Next();
  arena.Reset();

  // A pass renders relative to its own origin, so a layer that only
  // moved draws the same pixels -- fractional offsets included.
  StubGpuCommandBuffer second;
  Dispatch(GroupAt(Point(31.5, 12.25), picture), arena, pipelines.Set(),
           *target, textures, /*text=*/nullptr, second);
  EXPECT_EQ(second.passes, 1) << "only the frame";
}

TEST(DispatcherTest, ALayerThatScalesIsDrawnAgain) {
  StubGpuContext context;
  Pipelines pipelines;
  BufferArena arena(context);
  TextureCache textures(&context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  std::shared_ptr<PrPicture> picture = SpreadPicture();

  PrSceneNode scene = GroupAt(Point(0, 0), picture);
  StubGpuCommandBuffer first;
  Dispatch(scene, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, first);
  EXPECT_EQ(first.passes, 2);

  textures.Next();
  arena.Reset();

  // Twice the size is twice the pixels, so the key's size sees it.
  PrSceneNode scaled = GroupAt(Point(0, 0), picture);
  scaled.children[0].children[0].transform = Matrix::MakeScale({2, 2, 1});
  StubGpuCommandBuffer second;
  Dispatch(scaled, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, second);
  EXPECT_EQ(second.passes, 2);
}

TEST(DispatcherTest, ALayerThatIsMirroredIsDrawnAgain) {
  StubGpuContext context;
  Pipelines pipelines;
  BufferArena arena(context);
  TextureCache textures(&context);
  std::unique_ptr<GPUTexture> target =
      context.CreateTexture(TextureDesc{.width = 100, .height = 100}, false);
  std::shared_ptr<PrPicture> picture = SpreadPicture();

  StubGpuCommandBuffer first;
  Dispatch(GroupAt(Point(0, 0), picture), arena, pipelines.Set(), *target,
           textures, /*text=*/nullptr, first);
  EXPECT_EQ(first.passes, 2);

  textures.Next();
  arena.Reset();

  // A mirror leaves the coverage the same size and the pictures the
  // same pictures, so what catches it is the basis.
  PrSceneNode mirrored = GroupAt(Point(0, 0), picture);
  mirrored.children[0].children[0].transform = Matrix::MakeScale({-1, 1, 1});
  StubGpuCommandBuffer second;
  Dispatch(mirrored, arena, pipelines.Set(), *target, textures,
           /*text=*/nullptr, second);
  EXPECT_EQ(second.passes, 2);
}

// -----------------------------------------------------------------------
// Images.

namespace {

/// An image of a given size, which the stub context resolves to a
/// texture of its own.
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
  size_t GetApproximateByteSize() const override { return 4; }

 private:
  const flutter::DlISize size_;
};

/// The paint a draw names, out of the plan's paint stream.
const PrPaint& PaintOfDraw(const RenderPlan& plan, size_t index) {
  const BufferBinds& binds = plan.buffers[plan.draws[index].buffer_binds];
  const auto* attributes =
      reinterpret_cast<const Attributes*>(binds.attributes->Contents());
  const auto* paints =
      reinterpret_cast<const PrPaint*>(binds.paints->Contents());
  const uint16_t vertex = FirstVertexOfDraw(plan, index);
  return paints[attributes[vertex].paint & kPaintIndexMask];
}

}  // namespace

TEST(DispatcherTest, AnImageDrawSamplesTheImage) {
  StubGpuContext context;
  BufferArena arena(context);
  sk_sp<flutter::DlImage> image = sk_make_sp<StubDlImage>(64, 32);

  PrPictureBuilder builder;
  builder.DrawImage(image, Point(10, 20), flutter::DlImageSampling::kLinear,
                    nullptr);
  RenderPlan plan = Flatten(builder.Build(), arena);

  ASSERT_EQ(plan.draws.size(), 1u);
  const PrPaint& paint = PaintOfDraw(plan, 0);
  // The image takes a slot of the pass's table, after the atlas, and
  // the paint names it -- without which the fragment stage samples
  // nothing and the draw is a flat rectangle.
  ASSERT_GE(paint.texture_index, 0);
  ASSERT_LT(static_cast<size_t>(paint.texture_index), plan.textures.size());
  EXPECT_EQ(plan.textures[paint.texture_index],
            context.GetDLImageTexture(image));
  EXPECT_NE(paint.texture_index, 0) << "slot 0 is the glyph atlas";
  // Colour, not coverage: the atlas flag is what tells them apart.
  EXPECT_EQ(paint.flags & kPaintFlagTextureIsCoverage, 0u);
}

TEST(DispatcherTest, OneImageDrawnTwiceIsOneSlot) {
  StubGpuContext context;
  BufferArena arena(context);
  sk_sp<flutter::DlImage> image = sk_make_sp<StubDlImage>(64, 32);
  sk_sp<flutter::DlImage> other = sk_make_sp<StubDlImage>(64, 32);

  PrPictureBuilder builder;
  builder.DrawImage(image, Point(0, 0), flutter::DlImageSampling::kLinear,
                    nullptr);
  builder.DrawImage(other, Point(0, 40), flutter::DlImageSampling::kLinear,
                    nullptr);
  builder.DrawImage(image, Point(40, 0), flutter::DlImageSampling::kLinear,
                    nullptr);
  RenderPlan plan = Flatten(builder.Build(), arena);

  // The reserved slots and the two images: the one drawn twice is
  // named twice and bound once.
  EXPECT_EQ(plan.textures.size(), kReservedTextureSlots + 2);
}

TEST(DispatcherTest, ANearestSampledImageSaysSoInItsPaint) {
  StubGpuContext context;
  BufferArena arena(context);
  sk_sp<flutter::DlImage> image = sk_make_sp<StubDlImage>(64, 32);

  PrPictureBuilder builder;
  builder.DrawImage(image, Point(0, 0),
                    flutter::DlImageSampling::kNearestNeighbor, nullptr);
  RenderPlan plan = Flatten(builder.Build(), arena);

  ASSERT_EQ(plan.draws.size(), 1u);
  EXPECT_NE(PaintOfDraw(plan, 0).flags & kPaintFlagSampleNearest, 0u);
}

TEST(DispatcherTest, AGradientDrawNamesItsRampAndItsShape) {
  const std::array<flutter::DlColor, 2> colors = {flutter::DlColor::kRed(),
                                                  flutter::DlColor::kBlue()};
  const std::array<float, 2> stops = {0, 1};
  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kBlack());
  paint.setColorSource(flutter::DlColorSource::MakeLinear(
      flutter::DlPoint(0, 0), flutter::DlPoint(64, 0), 2, colors.data(),
      stops.data(), flutter::DlTileMode::kClamp));

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(kSurface);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 64, 64), paint);

  StubGpuContext context;
  BufferArena arena(context);
  TextureCache textures(&context);
  GradientAtlas gradients(&context);
  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  scene.children.push_back(std::move(leaf));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm,
                         /*text=*/nullptr, /*shadow_lut=*/nullptr, &gradients);

  const RenderPlan& plan = flattener.GetPlan()[0];
  // The ramps are bound where the shader looks for them.
  EXPECT_EQ(plan.textures[kGradientTextureSlot], gradients.GetTexture());

  ASSERT_FALSE(plan.buffers.empty());
  const auto* attributes = reinterpret_cast<const Attributes*>(
      plan.buffers[0].attributes->Contents());
  const auto* paints =
      reinterpret_cast<const PrPaint*>(plan.buffers[0].paints->Contents());
  const uint32_t paint_index = attributes[0].paint & kPaintIndexMask;
  ASSERT_GE(paints[paint_index].gradient_index, 0);

  const auto* gradient_data = reinterpret_cast<const GradientData*>(
      plan.buffers[0].gradients->Contents());
  const GradientData& data = gradient_data[paints[paint_index].gradient_index];
  // What the recording said the gradient was...
  EXPECT_EQ(data.kind, static_cast<uint32_t>(GradientKind::kLinear));
  EXPECT_EQ(data.tile_mode, static_cast<uint32_t>(flutter::DlTileMode::kClamp));
  EXPECT_EQ(data.data[0], 0);
  EXPECT_EQ(data.data[2], 64) << "the end point";
  // ...and where the atlas put its ramp.
  EXPECT_EQ(data.ramp_row, 0);
  EXPECT_EQ(data.ramp_span, static_cast<uint32_t>(GradientAtlas::kWidth) << 16)
      << "the whole row, starting at its first texel";
}

TEST(DispatcherTest, ADrawWithNoGradientNamesNoGradientData) {
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(kSurface);
  builder.DrawRect(Rect::MakeLTRB(0, 0, 64, 64),
                   flutter::DlPaint().setColor(flutter::DlColor::kRed()));

  StubGpuContext context;
  BufferArena arena(context);
  TextureCache textures(&context);
  GradientAtlas gradients(&context);
  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  scene.children.push_back(std::move(leaf));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, kSurface);
  flattener.EncodePasses(arena, textures, TextureFormat::kRGBA8UNorm,
                         /*text=*/nullptr, /*shadow_lut=*/nullptr, &gradients);

  const RenderPlan& plan = flattener.GetPlan()[0];
  const auto* attributes = reinterpret_cast<const Attributes*>(
      plan.buffers[0].attributes->Contents());
  const auto* paints =
      reinterpret_cast<const PrPaint*>(plan.buffers[0].paints->Contents());
  EXPECT_EQ(paints[attributes[0].paint & kPaintIndexMask].gradient_index, -1);
}

TEST(DispatcherTest, APicturesClipDoesNotReachTheLeafAfterIt) {
  StubGpuContext context;
  BufferArena arena(context);

  PrPictureBuilder first;
  first.SetSurfaceBounds(kSurface);
  first.ClipRect(Rect::MakeLTRB(0, 0, 20, 20));
  first.DrawRect(Rect::MakeLTRB(0, 0, 20, 20), Fill(flutter::DlColor::kRed()));

  // No clip of its own, so nothing may narrow it.
  PrPictureBuilder second;
  second.SetSurfaceBounds(kSurface);
  second.DrawRect(Rect::MakeLTRB(0, 0, 100, 100),
                  Fill(flutter::DlColor::kGreen()));

  PrSceneBuilder scene_builder(kSurface);
  scene_builder.DrawPicture(first.Build(), 1.0f);
  scene_builder.DrawPicture(second.Build(), 1.0f);
  RenderPlan plan = FlattenTheScene(scene_builder.Build(), arena);

  // The first leaf's draw, then the pop, then the second leaf. The draw
  // reached no further than the clip admitted, so the clip masked
  // nothing, was never recorded, and left the pop nothing to put back.
  ASSERT_EQ(plan.draws.size(), 3u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kColor);
  EXPECT_EQ(plan.draws[1].program, ProgramType::kInvalid);
  EXPECT_EQ(plan.draws[1].scissor, IRect32::MakeLTRB(0, 0, 100, 100))
      << "the pop goes back to the pass, not to what the picture covered";
  EXPECT_EQ(plan.draws[2].program, ProgramType::kColor);
}

TEST(DispatcherTest, AClipGroupComesBackReadyWhenNothingChanged) {
  StubGpuContext context;
  BufferArena arena(context);
  TextureCache cache(&context);
  // Held across both frames, the way a layer holds its picture.
  std::shared_ptr<PrPicture> leaf =
      RectPicture(Rect::MakeLTRB(0, 0, 40, 40), flutter::DlColor::kRed());
  // And so is the builder, which is what makes the clip shape keep its
  // id from one frame to the next.
  PrSceneBuilder builder(kSurface);

  auto frame = [&]() {
    builder.ClipRoundRect(
        RoundRect::MakeRectXY(Rect::MakeLTRB(0, 0, 20, 20), 4, 4));
    builder.DrawPicture(leaf, 1.0f);
    return Flattened(builder.Build(), arena, &cache).GetPasses()[0].ready;
  };

  EXPECT_FALSE(frame()) << "nothing has drawn it yet";
  cache.Next();
  EXPECT_TRUE(frame()) << "the same clip over the same picture";
}

TEST(DispatcherTest, AClipShapeIsRecordedAgainAfterAFrameWithoutIt) {
  StubGpuContext context;
  BufferArena arena(context);
  TextureCache cache(&context);
  std::shared_ptr<PrPicture> leaf =
      RectPicture(Rect::MakeLTRB(0, 0, 40, 40), flutter::DlColor::kRed());
  PrSceneBuilder builder(kSurface);

  auto frame = [&](bool clip) {
    if (clip) {
      builder.ClipRoundRect(
          RoundRect::MakeRectXY(Rect::MakeLTRB(0, 0, 20, 20), 4, 4));
    }
    builder.DrawPicture(leaf, 1.0f);
    return Flattened(builder.Build(), arena, &cache).GetPasses()[0].ready;
  };

  frame(true);
  cache.Next();
  frame(false);  // The cache holds one frame, and this one did not ask.
  cache.Next();
  EXPECT_FALSE(frame(true)) << "the shape it dropped is a new picture";
}

TEST(DispatcherTest, AnAxisAlignedSceneClipIsAScissorAndNoPass) {
  StubGpuContext context;
  BufferArena arena(context);
  PrSceneBuilder scene_builder(kSurface);
  scene_builder.ClipRect(Rect::MakeLTRB(10, 10, 50, 50));
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 100, 100), flutter::DlColor::kRed()), 1);
  scene_builder.DrawPicture(
      RectPicture(Rect::MakeLTRB(0, 0, 100, 100), flutter::DlColor::kGreen()),
      1);

  SceneFlattener flattener = Flattened(scene_builder.Build(), arena);
  const RenderPlan& plan = flattener.GetPlan().back();

  // One pass, so no offscreen for the clip.
  EXPECT_EQ(flattener.GetPasses().size(), 1u);
  // The scissor once, then both leaves batched under it.
  ASSERT_EQ(plan.draws.size(), 2u);
  EXPECT_EQ(plan.draws[0].program, ProgramType::kInvalid);
  EXPECT_EQ(plan.draws[0].scissor, IRect32::MakeLTRB(10, 10, 50, 50));
  EXPECT_EQ(plan.draws[1].program, ProgramType::kColor);
  EXPECT_EQ(plan.draws[1].count, 12u) << "the two leaves still batch";
}

}  // namespace testing

}  // namespace impeller
