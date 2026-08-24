// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>
#include <string>

#include "flutter/display_list/dl_vertices.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/testing/testing.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/geometry/geometry_asserts.h"
#include "impeller/propeller/freetype_glyph_rasterizer.h"
#include "impeller/propeller/gradient_atlas.h"
#include "impeller/propeller/metal/metal_pipelines.h"
#include "impeller/propeller/metal/metal_renderer.h"
#include "impeller/propeller/paged_atlas.h"
#include "impeller/propeller/picture.h"
#include "impeller/propeller/scene.h"
#include "impeller/propeller/text_materializer.h"
#include "impeller/renderer/backend/metal/texture_mtl.h"
#include "impeller/typographer/text_frame.h"

namespace impeller {
namespace testing {

namespace {

struct Pixels {
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> rgba;

  Color Get(int32_t x, int32_t y) const {
    const uint8_t* p = rgba.data() + (y * width + x) * 4;
    return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
  }
};

}  // namespace

// -----------------------------------------------------------------------
// Wide gamut.

namespace {

/// A scene of one picture inside a compositing group, which is what
/// takes a pass of its own and so renders into an offscreen rather than
/// into the frame's target.
PrSceneNode GroupedRect() {
  // Two rects apart, so neither covers the pass: one that did would be
  // folded into the clear colour, and the pass would encode no draw at
  // all -- and no draw is no pipeline to disagree with the target.
  PrPictureBuilder builder;
  builder.DrawRect(Rect::MakeLTRB(10, 10, 30, 30),
                   flutter::DlPaint().setColor(flutter::DlColor::kRed()));
  builder.DrawRect(Rect::MakeLTRB(50, 50, 70, 70),
                   flutter::DlPaint().setColor(flutter::DlColor::kBlue()));

  PrSceneNode scene;
  PrSceneNode group;
  group.opacity = 0.5f;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  group.children.push_back(std::move(leaf));
  scene.children.push_back(std::move(group));
  return scene;
}

}  // namespace

TEST(MetalRendererTest, AGroupCompositesAtItsOpacity) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:100
                                  height:100
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer->GetDevice() newTextureWithDescriptor:descriptor];
  ASSERT_NE(target, nil);

  id<MTLCommandBuffer> command_buffer =
      renderer->Render(GroupedRect(), target, Color(0, 0, 0, 1));
  ASSERT_NE(command_buffer, nil);
  [command_buffer waitUntilCompleted];

  Pixels pixels;
  pixels.width = 100;
  pixels.height = 100;
  pixels.rgba.resize(100 * 100 * 4);
  [target getBytes:pixels.rgba.data()
       bytesPerRow:100 * 4
        fromRegion:MTLRegionMake2D(0, 0, 100, 100)
       mipmapLevel:0];

  // The group's content at half opacity, premultiplied, which is what
  // the composite quad's vertex colour scales the offscreen by. The
  // frame starts transparent: the clear colour the caller asked for is
  // still on the dispatcher's TODO list.
  const Color red = pixels.Get(20, 20);
  EXPECT_NEAR(red.red, 0.5f, 0.02f);
  EXPECT_NEAR(red.green, 0.0f, 0.02f);
  EXPECT_NEAR(red.alpha, 0.5f, 0.02f);
  const Color blue = pixels.Get(60, 60);
  EXPECT_NEAR(blue.blue, 0.5f, 0.02f);
  EXPECT_NEAR(blue.alpha, 0.5f, 0.02f);
  // And nothing outside what the group covers.
  EXPECT_EQ(pixels.Get(90, 90), Color(0, 0, 0, 0));
}

namespace {

/// A five-pointed star drawn as one self-intersecting contour: the
/// middle is wound twice, so the fill rule decides whether it is
/// filled. Concave either way, so it goes through the accumulator.
flutter::DlPath Star(Point centre,
                     Scalar radius,
                     flutter::DlPathFillType fill) {
  flutter::DlPathBuilder path;
  path.SetFillType(fill);
  for (int i = 0; i < 5; i++) {
    // Every second point, which is what crosses the contour over
    // itself.
    const Scalar angle = -kPiOver2 + (i * 2) * (2 * kPi / 5);
    const Point point =
        centre + Point(std::cos(angle), std::sin(angle)) * radius;
    if (i == 0) {
      path.MoveTo(point);
    } else {
      path.LineTo(point);
    }
  }
  path.Close();
  return path.TakePath();
}

/// Render one path and read the pixels back.
std::vector<uint8_t> RenderPath(MetalRenderer& renderer,
                                const flutter::DlPath& path) {
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawPath(path,
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));

  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  scene.children.push_back(std::move(leaf));

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:120
                                  height:120
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer.GetDevice() newTextureWithDescriptor:descriptor];
  [renderer.Render(scene, target, Color(0, 0, 0, 0)) waitUntilCompleted];

  std::vector<uint8_t> rgba(120 * 120 * 4);
  [target getBytes:rgba.data()
       bytesPerRow:120 * 4
        fromRegion:MTLRegionMake2D(0, 0, 120, 120)
       mipmapLevel:0];
  return rgba;
}

}  // namespace

namespace {

/// Render one picture and read the pixels back.
std::vector<uint8_t> RenderPicture(MetalRenderer& renderer,
                                   const std::shared_ptr<PrPicture>& picture) {
  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = picture;
  scene.children.push_back(std::move(leaf));

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:120
                                  height:120
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer.GetDevice() newTextureWithDescriptor:descriptor];
  [renderer.Render(scene, target, Color(0, 0, 0, 0)) waitUntilCompleted];

  std::vector<uint8_t> rgba(120 * 120 * 4);
  [target getBytes:rgba.data()
       bytesPerRow:120 * 4
        fromRegion:MTLRegionMake2D(0, 0, 120, 120)
       mipmapLevel:0];
  return rgba;
}

}  // namespace

TEST(MetalRendererTest, AStrokeIsABandOfItsWidth) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  flutter::DlPathBuilder path;
  path.MoveTo(Point(20, 60));
  path.LineTo(Point(100, 60));
  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kGreen());
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(10);

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawPath(path.TakePath(), paint);
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  // Five either side of the line, and nothing past that.
  EXPECT_GT(green_at(60, 60), 200) << "the middle of the band";
  EXPECT_GT(green_at(60, 57), 200) << "inside it";
  EXPECT_GT(green_at(60, 63), 200) << "inside it";
  EXPECT_LT(green_at(60, 52), 20) << "above the band";
  EXPECT_LT(green_at(60, 68), 20) << "below the band";
  // Butt caps by default: nothing past the ends.
  EXPECT_LT(green_at(15, 60), 20) << "before it starts";
  EXPECT_LT(green_at(105, 60), 20) << "after it ends";
}

TEST(MetalRendererTest, AStrokesCornerIsSolidAndMitred) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  flutter::DlPathBuilder path;
  path.MoveTo(Point(30, 30));
  path.LineTo(Point(90, 30));
  path.LineTo(Point(90, 90));
  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kGreen());
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(12);
  paint.setStrokeJoin(flutter::DlStrokeJoin::kMiter);

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawPath(path.TakePath(), paint);
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  // Where the two bands abut the join, and each other. Those edges
  // carry no fringe, so the seam is solid: a ramp on either side of it
  // would leave a pale line through the corner.
  EXPECT_GT(green_at(90, 30), 250) << "the seam at the joint";
  EXPECT_GT(green_at(87, 33), 250) << "just inside the corner";
  // And the mitre fills the outer corner, which the bands alone leave
  // as a notch.
  EXPECT_GT(green_at(93, 27), 250) << "the outer corner";
  // The inner corner is where the bands overlap, so it is covered
  // whatever the join does; past it there is nothing.
  EXPECT_LT(green_at(80, 45), 20) << "inside the elbow";
}

TEST(MetalRendererTest, ATranslucentStrokeDoesNotDarkenWhereItOverlaps) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // A right angle: the two bands and the join piece all overlap at the
  // corner, which is where a translucent stroke would darken itself.
  flutter::DlPathBuilder path;
  path.MoveTo(Point(30, 30));
  path.LineTo(Point(90, 30));
  path.LineTo(Point(90, 90));
  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kGreen().withAlphaF(0.5));
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(12);
  paint.setStrokeJoin(flutter::DlStrokeJoin::kMiter);

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawPath(path.TakePath(), paint);
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto alpha_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 3];
  };

  // Half alpha along a straight run...
  const uint8_t along = alpha_at(60, 30);
  EXPECT_NEAR(along, 128, 8);
  // ...and the same at the corner, where three pieces cover the same
  // pixel. Drawing them over each other would make it half again as
  // opaque.
  EXPECT_NEAR(alpha_at(90, 30), along, 8) << "the join darkened itself";
  EXPECT_NEAR(alpha_at(88, 32), along, 8) << "the join darkened itself";
}

TEST(MetalRendererTest, AShadowFallsBelowItsOccluderAndFadesOut) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  const Rect occluder = Rect::MakeLTRB(40, 40, 80, 70);
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawShadow(flutter::DlPath::MakeRect(occluder),
                     flutter::DlColor::kBlack(), /*elevation=*/6,
                     /*transparent_occluder=*/false, /*dpr=*/1);
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto alpha_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 3];
  };

  // Something below the occluder, where the light throws it.
  const uint8_t below = alpha_at(60, 75);
  EXPECT_GT(below, 10) << "the spot half";
  // Less above it than below: the spot is thrown down, the ambient is
  // all that reaches upward.
  EXPECT_LT(alpha_at(60, 35), below);
  // Fading with distance, and gone well before the surface ends.
  EXPECT_LT(alpha_at(60, 90), below);
  EXPECT_LT(alpha_at(60, 115), 10) << "past the band";
  EXPECT_LT(alpha_at(5, 5), 10) << "nowhere near it";
}

TEST(MetalRendererTest, ATransparentOccluderIsShadowedUnderneath) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  const Rect occluder = Rect::MakeLTRB(40, 40, 80, 70);
  auto shadow_of = [&](bool transparent_occluder) {
    PrPictureBuilder builder;
    builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
    builder.DrawShadow(flutter::DlPath::MakeRect(occluder),
                       flutter::DlColor::kBlack(), /*elevation=*/6,
                       transparent_occluder, /*dpr=*/1);
    return RenderPicture(*renderer, builder.Build());
  };
  auto alpha_at = [](const std::vector<uint8_t>& pixels, int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 3];
  };

  // An opaque occluder hides its own interior, so the halves skip it.
  // A transparent one is shadowed through, which is the whole of what
  // the interior modes are for.
  const std::vector<uint8_t> opaque = shadow_of(false);
  const std::vector<uint8_t> transparent = shadow_of(true);
  // Deep enough in to be past both bands' inner rims, where only a fan
  // reaches -- nearer the edge the spot's ring covers it either way.
  EXPECT_GT(alpha_at(transparent, 60, 60), alpha_at(opaque, 60, 60))
      << "inside the occluder";
  EXPECT_LT(alpha_at(opaque, 60, 60), 10) << "which the occluder hides";
  // Outside it the two agree: the same band, drawn the same way.
  EXPECT_NEAR(alpha_at(transparent, 60, 78), alpha_at(opaque, 60, 78), 4);
}

TEST(MetalRendererTest, AnAtlasDrawsEachSpriteOutOfTheOneImage) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // A two by two image: red, green / blue, white. Each sprite cuts out
  // one texel, so where each colour lands says whether the source
  // rects, the transforms and the texture all arrived.
  MTLTextureDescriptor* source_desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:2
                                  height:2
                               mipmapped:NO];
  source_desc.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> source =
      [renderer->GetDevice() newTextureWithDescriptor:source_desc];
  const uint8_t texels[16] = {255, 0,   0,   255,  //
                              0,   255, 0,   255,  //
                              0,   0,   255, 255,  //
                              255, 255, 255, 255};
  [source replaceRegion:MTLRegionMake2D(0, 0, 2, 2)
            mipmapLevel:0
              withBytes:texels
            bytesPerRow:8];

  TextureDescriptor wrapped;
  wrapped.storage_mode = StorageMode::kDevicePrivate;
  wrapped.format = PixelFormat::kR8G8B8A8UNormInt;
  wrapped.size = ISize(2, 2);
  wrapped.usage = TextureUsage::kShaderRead;
  const sk_sp<flutter::DlImage> image =
      DlImageImpeller::Make(TextureMTL::Wrapper(wrapped, source),
                            flutter::DlImage::OwningContext::kRaster);
  ASSERT_NE(image, nullptr);

  // Two sprites, each one texel of the image blown up twenty times.
  const RSTransform transforms[2] = {RSTransform(20, 0, 10, 10),
                                     RSTransform(20, 0, 60, 60)};
  const Rect textures[2] = {Rect::MakeLTRB(0, 0, 1, 1),
                            Rect::MakeLTRB(1, 1, 2, 2)};

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawAtlas(image, transforms, textures, /*colors=*/nullptr, 2,
                    flutter::DlBlendMode::kSrcOver,
                    flutter::DlImageSampling::kNearestNeighbor,
                    /*cull_rect=*/nullptr, nullptr);

  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  scene.children.push_back(std::move(leaf));

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:120
                                  height:120
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer->GetDevice() newTextureWithDescriptor:descriptor];
  [renderer->Render(scene, target, Color(0, 0, 0, 0)) waitUntilCompleted];

  std::vector<uint8_t> rgba(120 * 120 * 4);
  [target getBytes:rgba.data()
       bytesPerRow:120 * 4
        fromRegion:MTLRegionMake2D(0, 0, 120, 120)
       mipmapLevel:0];
  auto pixel_at = [&rgba](int32_t x, int32_t y) {
    const uint8_t* p = rgba.data() + (y * 120 + x) * 4;
    return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
  };

  // The first texel where the first transform put it, the last texel
  // where the second did.
  EXPECT_COLOR_NEAR(pixel_at(20, 20), Color(1, 0, 0, 1));
  EXPECT_COLOR_NEAR(pixel_at(70, 70), Color(1, 1, 1, 1));
  // And nothing between them: the sprites are only as big as they are.
  EXPECT_LT(pixel_at(45, 45).alpha, 0.1f);
}

TEST(MetalRendererTest, AVertexBufferDrawsItsTrianglesAndItsColours) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // Two triangles making a square, each in its own colour, so both the
  // geometry and the per-vertex colours have to arrive for this to
  // come out right.
  const Point positions[6] = {Point(20, 20),  Point(100, 20), Point(20, 100),
                              Point(100, 20), Point(20, 100), Point(100, 100)};
  const flutter::DlColor colors[6] = {
      flutter::DlColor::kRed(),  flutter::DlColor::kRed(),
      flutter::DlColor::kRed(),  flutter::DlColor::kBlue(),
      flutter::DlColor::kBlue(), flutter::DlColor::kBlue()};

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawVertices(
      flutter::DlVertices::Make(flutter::DlVertexMode::kTriangles, 6, positions,
                                /*texture_coordinates=*/nullptr, colors),
      flutter::DlBlendMode::kSrcOver,
      flutter::DlPaint().setColor(flutter::DlColor::kGreen()));

  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  scene.children.push_back(std::move(leaf));

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:120
                                  height:120
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer->GetDevice() newTextureWithDescriptor:descriptor];
  [renderer->Render(scene, target, Color(0, 0, 0, 0)) waitUntilCompleted];

  std::vector<uint8_t> rgba(120 * 120 * 4);
  [target getBytes:rgba.data()
       bytesPerRow:120 * 4
        fromRegion:MTLRegionMake2D(0, 0, 120, 120)
       mipmapLevel:0];
  auto pixel_at = [&rgba](int32_t x, int32_t y) {
    const uint8_t* p = rgba.data() + (y * 120 + x) * 4;
    return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
  };

  // Each triangle in its own vertices' colour, not the paint's green.
  EXPECT_COLOR_NEAR(pixel_at(35, 35), Color(1, 0, 0, 1)) << "the first";
  EXPECT_COLOR_NEAR(pixel_at(85, 85), Color(0, 0, 1, 1)) << "the second";
  // And nothing outside them.
  EXPECT_LT(pixel_at(10, 10).alpha, 0.1f);
  EXPECT_LT(pixel_at(110, 110).alpha, 0.1f);
}

TEST(MetalRendererTest, AnImageDrawSamplesTheImage) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // A two by two image: red, green / blue, white. Sampled nearest, so
  // each quadrant of the destination is exactly one of them.
  MTLTextureDescriptor* source_desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:2
                                  height:2
                               mipmapped:NO];
  source_desc.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> source =
      [renderer->GetDevice() newTextureWithDescriptor:source_desc];
  const uint8_t texels[16] = {255, 0,   0,   255,  //
                              0,   255, 0,   255,  //
                              0,   0,   255, 255,  //
                              255, 255, 255, 255};
  [source replaceRegion:MTLRegionMake2D(0, 0, 2, 2)
            mipmapLevel:0
              withBytes:texels
            bytesPerRow:8];

  TextureDescriptor wrapped;
  wrapped.storage_mode = StorageMode::kDevicePrivate;
  wrapped.format = PixelFormat::kR8G8B8A8UNormInt;
  wrapped.size = ISize(2, 2);
  wrapped.usage = TextureUsage::kShaderRead;
  const sk_sp<flutter::DlImage> image =
      DlImageImpeller::Make(TextureMTL::Wrapper(wrapped, source),
                            flutter::DlImage::OwningContext::kRaster);
  ASSERT_NE(image, nullptr);

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawImageRect(image, Rect::MakeLTRB(0, 0, 2, 2),
                        Rect::MakeLTRB(20, 20, 100, 100),
                        flutter::DlImageSampling::kNearestNeighbor, nullptr);

  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  scene.children.push_back(std::move(leaf));

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:120
                                  height:120
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer->GetDevice() newTextureWithDescriptor:descriptor];
  [renderer->Render(scene, target, Color(0, 0, 0, 0)) waitUntilCompleted];

  std::vector<uint8_t> rgba(120 * 120 * 4);
  [target getBytes:rgba.data()
       bytesPerRow:120 * 4
        fromRegion:MTLRegionMake2D(0, 0, 120, 120)
       mipmapLevel:0];
  auto pixel_at = [&rgba](int32_t x, int32_t y) {
    const uint8_t* p = rgba.data() + (y * 120 + x) * 4;
    return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
  };

  // Each texel where its quadrant of the destination is. A draw that
  // named no texture would be one flat colour instead.
  EXPECT_COLOR_NEAR(pixel_at(40, 40), Color(1, 0, 0, 1));
  EXPECT_COLOR_NEAR(pixel_at(80, 40), Color(0, 1, 0, 1));
  EXPECT_COLOR_NEAR(pixel_at(40, 80), Color(0, 0, 1, 1));
  EXPECT_COLOR_NEAR(pixel_at(80, 80), Color(1, 1, 1, 1));
  // And nothing outside the destination rect.
  EXPECT_LT(pixel_at(10, 10).alpha, 0.1f);
}

TEST(MetalRendererTest, ACircleIsRoundAndActuallyThere) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawCircle(Point(60, 60), 40,
                     flutter::DlPaint().setColor(flutter::DlColor::kGreen()));

  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  scene.children.push_back(std::move(leaf));

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:120
                                  height:120
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer->GetDevice() newTextureWithDescriptor:descriptor];
  ASSERT_NE(target, nil);
  [renderer->Render(scene, target, Color(0, 0, 0, 0)) waitUntilCompleted];

  std::vector<uint8_t> rgba(120 * 120 * 4);
  [target getBytes:rgba.data()
       bytesPerRow:120 * 4
        fromRegion:MTLRegionMake2D(0, 0, 120, 120)
       mipmapLevel:0];
  auto green_at = [&rgba](int32_t x, int32_t y) {
    return rgba[(y * 120 + x) * 4 + 1];
  };

  // A circle is a round rect whose corner radii are half its bounds.
  // Radii of the whole bounds fold every corner onto every other and
  // the mesh comes out a point, which draws nothing at all.
  EXPECT_GT(green_at(60, 60), 200) << "the middle";
  EXPECT_GT(green_at(60, 25), 200) << "just inside the top";
  EXPECT_GT(green_at(25, 60), 200) << "just inside the left";
  // Round, so the corners of the bounds are outside it.
  EXPECT_LT(green_at(26, 26), 20) << "the corner of the bounds";
  EXPECT_LT(green_at(94, 94), 20) << "the corner of the bounds";
  // And nothing past the bounds.
  EXPECT_LT(green_at(60, 15), 20) << "above the circle";
}

TEST(MetalRendererTest, AConcaveFillObeysItsFillRule) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  auto green_at = [](const std::vector<uint8_t>& rgba, int32_t x, int32_t y) {
    return rgba[(y * 120 + x) * 4 + 1];
  };
  const Point centre(60, 60);

  ASSERT_FALSE(Star(centre, 50, flutter::DlPathFillType::kNonZero).IsConvex())
      << "a convex star would never reach the accumulator";
  const std::vector<uint8_t> nonzero = RenderPath(
      *renderer, Star(centre, 50, flutter::DlPathFillType::kNonZero));
  const std::vector<uint8_t> even_odd =
      RenderPath(*renderer, Star(centre, 50, flutter::DlPathFillType::kOdd));

  // A point of the star is inside under either rule.
  const int32_t point_y = static_cast<int32_t>(centre.y - 40);
  EXPECT_GT(green_at(nonzero, 60, point_y), 200) << "a point of the star";
  EXPECT_GT(green_at(even_odd, 60, point_y), 200) << "a point of the star";

  // The middle is wound twice: filled under nonzero, a hole under
  // even-odd. Which is the whole of what the accumulator is for.
  EXPECT_GT(green_at(nonzero, 60, 60), 200) << "the middle, nonzero";
  EXPECT_LT(green_at(even_odd, 60, 60), 20) << "the middle, even-odd";

  // And nothing of either reaches the corner.
  EXPECT_LT(green_at(nonzero, 5, 5), 20);
  EXPECT_LT(green_at(even_odd, 5, 5), 20);
}

TEST(MetalRendererTest, ARoundRectCornerCoversTheArcAndNothingBeyondIt) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // Recorded in logical units under a device pixel ratio, which is what
  // makes the difference: anything the mesh measures in local units
  // comes out three times too big here.
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.Scale(3, 3);
  builder.DrawRoundRect(
      RoundRect::MakeRectXY(Rect::MakeLTRB(1, 1, 39, 39), 8, 8),
      flutter::DlPaint().setColor(flutter::DlColor::kGreen()));

  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = builder.Build();
  scene.children.push_back(std::move(leaf));

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:120
                                  height:120
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer->GetDevice() newTextureWithDescriptor:descriptor];
  ASSERT_NE(target, nil);
  [renderer->Render(scene, target, Color(0, 0, 0, 0)) waitUntilCompleted];

  std::vector<uint8_t> rgba(120 * 120 * 4);
  [target getBytes:rgba.data()
       bytesPerRow:120 * 4
        fromRegion:MTLRegionMake2D(0, 0, 120, 120)
       mipmapLevel:0];
  auto green_at = [&rgba](int32_t x, int32_t y) {
    return rgba[(y * 120 + x) * 4 + 1];
  };

  // The corner's circle, in device pixels.
  const Point centre(27, 27);
  const Scalar radius = 24;
  const Point out = Point(-1, -1).Normalize();
  for (Scalar along = 0; along <= radius - 4; along += 4) {
    const Point inside = centre + out * along;
    EXPECT_GT(green_at(inside.x, inside.y), 200)
        << "inside the arc at " << inside;
  }
  for (Scalar past = 2; past <= 10; past += 2) {
    const Point outside = centre + out * (radius + past);
    EXPECT_LT(green_at(outside.x, outside.y), 20)
        << "past the arc at " << outside;
  }

  // Where the tangent at the arc's midpoint runs out to. Nothing of the
  // shape reaches here, and a fringe that follows the tangent instead
  // of the curve is what would put something here.
  // Where the tangent at the arc's midpoint runs out to, a good two
  // pixels clear of the arc. A fringe that follows the tangent instead
  // of the curve paints half coverage all along here.
  EXPECT_LT(green_at(16, 3), 20) << "along the tangent, above the arc";
  EXPECT_LT(green_at(15, 4), 20) << "along the tangent, above the arc";
  EXPECT_LT(green_at(3, 16), 20) << "along the tangent, left of the arc";
  EXPECT_LT(green_at(4, 15), 20) << "along the tangent, left of the arc";
}

TEST(MetalRendererTest, AWideGamutTargetRendersItsPassesInItsOwnFormat) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }
  // The extended range formats are Apple GPU only, which is also the
  // only place a drawable arrives in one.
  if (![renderer->GetDevice() supportsFamily:MTLGPUFamilyApple1]) {
    GTEST_SKIP() << "No Apple GPU, so no wide gamut drawable.";
  }

  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA10_XR
                                   width:100
                                  height:100
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModePrivate;
  id<MTLTexture> target =
      [renderer->GetDevice() newTextureWithDescriptor:descriptor];
  ASSERT_NE(target, nil);

  // The pipelines are built for the target's format, so the group's
  // offscreen has to be in it too. Metal validation aborts on a
  // mismatch, so a regression here fails hard rather than quietly.
  id<MTLCommandBuffer> command_buffer =
      renderer->Render(GroupedRect(), target, Color(0, 0, 0, 0));
  ASSERT_NE(command_buffer, nil);
  [command_buffer waitUntilCompleted];
  EXPECT_EQ(command_buffer.error, nil);
  EXPECT_EQ(command_buffer.status, MTLCommandBufferStatusCompleted);
}

}  // namespace testing
}  // namespace impeller

namespace impeller {
namespace testing {

TEST(MetalRendererTest, AStrokedShapeIsABorderAndNotAFill) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kGreen());
  paint.setDrawStyle(flutter::DlDrawStyle::kStroke);
  paint.setStrokeWidth(8);

  // A shape asked for as a border has one, and has nothing inside it.
  {
    PrPictureBuilder builder;
    builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
    builder.DrawRect(Rect::MakeLTRB(20, 20, 100, 100), paint);
    const std::vector<uint8_t> pixels =
        RenderPicture(*renderer, builder.Build());
    auto green_at = [&pixels](int32_t x, int32_t y) {
      return pixels[(y * 120 + x) * 4 + 1];
    };
    EXPECT_GT(green_at(60, 20), 200) << "the top edge";
    EXPECT_GT(green_at(20, 60), 200) << "the left edge";
    EXPECT_GT(green_at(100, 60), 200) << "the right edge";
    EXPECT_LT(green_at(60, 60), 20) << "the middle, which is not filled";
    EXPECT_LT(green_at(60, 10), 20) << "outside it";
  }

  {
    PrPictureBuilder builder;
    builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
    builder.DrawCircle(Point(60, 60), 40, paint);
    const std::vector<uint8_t> pixels =
        RenderPicture(*renderer, builder.Build());
    auto green_at = [&pixels](int32_t x, int32_t y) {
      return pixels[(y * 120 + x) * 4 + 1];
    };
    EXPECT_GT(green_at(60, 20), 200) << "the top of the ring";
    EXPECT_GT(green_at(20, 60), 200) << "the left of the ring";
    EXPECT_LT(green_at(60, 60), 20) << "the middle, which is not filled";
    // The corner the circle does not reach, which a bounds fill would.
    EXPECT_LT(green_at(26, 26), 20) << "outside the ring";
  }
}

}  // namespace testing
}  // namespace impeller

namespace impeller {
namespace testing {

TEST(MetalRendererTest, ALineDrawsWhereItWasAskedFor) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  flutter::DlPaint paint;
  paint.setColor(flutter::DlColor::kGreen());
  paint.setStrokeWidth(10);

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawLine(Point(20, 20), Point(100, 100), paint);
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  // Along the diagonal, and half a width either side of it.
  EXPECT_GT(green_at(60, 60), 200) << "the middle of the line";
  EXPECT_GT(green_at(63, 57), 200) << "inside it";
  EXPECT_GT(green_at(57, 63), 200) << "inside it";
  // Past half a width, measured square to the line rather than to the
  // axes: a rectangle swept along the diagonal, not an axis-aligned one.
  EXPECT_LT(green_at(72, 48), 20) << "off the line";
  EXPECT_LT(green_at(48, 72), 20) << "off the line";
  // Butt ends: nothing past either one.
  EXPECT_LT(green_at(12, 12), 20) << "before it starts";
  EXPECT_LT(green_at(108, 108), 20) << "after it ends";
}

}  // namespace testing
}  // namespace impeller

namespace impeller {
namespace testing {

TEST(MetalRendererTest, ATurnedRectHasARampedEdge) {
  auto context = GPUContextMTL::Make();
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  auto renderer = MetalRenderer::Make(context, atlas);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // Square to the pixel grid, a rect's own edges are exact.
  PrPictureBuilder square;
  square.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  square.DrawRect(Rect::MakeLTRB(20, 20.5f, 100, 100),
                  flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  const std::vector<uint8_t> unturned =
      RenderPicture(*renderer, square.Build());

  // Turned by 45 degrees about the middle, they are not, so the draw
  // carries a pixel of ramp around it.
  PrPictureBuilder turned;
  turned.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  turned.Translate(60, 60);
  turned.Rotate(45);
  turned.Translate(-60, -60);
  turned.DrawRect(Rect::MakeLTRB(20, 20, 100, 100),
                  flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  const std::vector<uint8_t> rotated = RenderPicture(*renderer, turned.Build());

  auto alpha_at = [](const std::vector<uint8_t>& pixels, int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 3];
  };
  // The middle is covered either way.
  EXPECT_GT(alpha_at(unturned, 60, 60), 250);
  EXPECT_GT(alpha_at(rotated, 60, 60), 250);

  // The turned square's corner points up the middle of the top edge, so
  // somewhere along the diagonal there is a partly covered pixel. An
  // unramped quad has none: every pixel is in or out.
  int partial_on_the_diagonal = 0;
  for (int32_t i = 0; i < 40; i++) {
    const uint8_t alpha = alpha_at(rotated, 60 - i, 60 - i);
    if (alpha > 20 && alpha < 235) {
      partial_on_the_diagonal++;
    }
  }
  EXPECT_GT(partial_on_the_diagonal, 0) << "the edge is not ramped";
}

}  // namespace testing
}  // namespace impeller

namespace impeller {
namespace testing {

TEST(MetalRendererTest, AGradientRampRasterizesAcrossItsRow) {
  auto context = GPUContextMTL::Make();
  if (!context) {
    GTEST_SKIP() << "No Metal device available.";
  }
  GPUProgramResolverMTL resolver(context->GetDevice());
  if (!resolver.IsValid()) {
    GTEST_SKIP() << "No Metal device available.";
  }
  const GPUProgram* ramp = resolver.Resolve(ProgramType::kGradientRamp);
  ASSERT_NE(ramp, nullptr);

  GradientAtlas atlas(context.get());
  const std::array<flutter::DlColor, 2> colors = {flutter::DlColor::kRed(),
                                                  flutter::DlColor::kBlue()};
  const std::array<float, 2> stops = {0, 1};
  std::shared_ptr<flutter::DlColorSource> source =
      flutter::DlColorSource::MakeLinear(
          flutter::DlPoint(0, 0), flutter::DlPoint(64, 0), 2, colors.data(),
          stops.data(), flutter::DlTileMode::kClamp);
  std::optional<GradientAtlas::RampLocation> location =
      atlas.RegisterGradient(source);
  ASSERT_TRUE(location.has_value());
  EXPECT_EQ(location->texel_count,
            static_cast<uint32_t>(GradientAtlas::kWidth));

  // The same source again is the same ramp, and does not take a row.
  EXPECT_EQ(atlas.RegisterGradient(source), location);

  id<MTLCommandBuffer> command_buffer =
      [context->GetCommandQueue() commandBuffer];
  {
    GpuCommandBufferMTL recorder(command_buffer);
    atlas.RecordUploads(recorder, *ramp);
    recorder.EndEncoding();
  }

  // The atlas is private, so read it back through a shared copy.
  id<MTLTexture> ramps =
      static_cast<GPUTextureMTL*>(atlas.GetTexture())->GetTexture();
  MTLTextureDescriptor* readback = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:GradientAtlas::kWidth
                                  height:1
                               mipmapped:NO];
  readback.storageMode = MTLStorageModeShared;
  id<MTLTexture> row = [context->GetDevice() newTextureWithDescriptor:readback];
  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  [blit copyFromTexture:ramps
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, location->row, 0)
             sourceSize:MTLSizeMake(GradientAtlas::kWidth, 1, 1)
              toTexture:row
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  std::vector<uint8_t> texels(GradientAtlas::kWidth * 4);
  [row getBytes:texels.data()
      bytesPerRow:GradientAtlas::kWidth * 4
       fromRegion:MTLRegionMake2D(0, 0, GradientAtlas::kWidth, 1)
      mipmapLevel:0];
  auto at = [&texels](int32_t x) {
    const uint8_t* p = texels.data() + x * 4;
    return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
  };

  // Red at one end, blue at the other, and the interpolation between:
  // the vertex stage draws the ramp, so the ends are whole texels and
  // the middle is halfway.
  const Color left = at(0);
  EXPECT_GT(left.red, 0.98f);
  EXPECT_LT(left.blue, 0.02f);
  const Color right = at(GradientAtlas::kWidth - 1);
  EXPECT_GT(right.blue, 0.98f);
  EXPECT_LT(right.red, 0.02f);
  const Color middle = at(GradientAtlas::kWidth / 2);
  EXPECT_NEAR(middle.red, 0.5f, 0.02f);
  EXPECT_NEAR(middle.blue, 0.5f, 0.02f);
  EXPECT_GT(middle.alpha, 0.98f);
}

TEST(MetalRendererTest, AStopLandsWhereItsOffsetSaysAndNotHalfway) {
  auto context = GPUContextMTL::Make();
  if (!context) {
    GTEST_SKIP() << "No Metal device available.";
  }
  GPUProgramResolverMTL resolver(context->GetDevice());
  if (!resolver.IsValid()) {
    GTEST_SKIP() << "No Metal device available.";
  }
  const GPUProgram* ramp = resolver.Resolve(ProgramType::kGradientRamp);
  ASSERT_NE(ramp, nullptr);

  // Three stops, the middle one a quarter of the way along: the strip
  // has a vertex apiece, so the green belongs at a quarter and the two
  // segments either side of it ramp at different rates.
  GradientAtlas atlas(context.get());
  const std::array<flutter::DlColor, 3> colors = {flutter::DlColor::kRed(),
                                                  flutter::DlColor::kGreen(),
                                                  flutter::DlColor::kBlue()};
  const std::array<float, 3> stops = {0, 0.25f, 1};
  std::shared_ptr<flutter::DlColorSource> source =
      flutter::DlColorSource::MakeLinear(
          flutter::DlPoint(0, 0), flutter::DlPoint(64, 0), 3, colors.data(),
          stops.data(), flutter::DlTileMode::kClamp);
  std::optional<GradientAtlas::RampLocation> location =
      atlas.RegisterGradient(source);
  ASSERT_TRUE(location.has_value());

  id<MTLCommandBuffer> command_buffer =
      [context->GetCommandQueue() commandBuffer];
  {
    GpuCommandBufferMTL recorder(command_buffer);
    atlas.RecordUploads(recorder, *ramp);
    recorder.EndEncoding();
  }
  id<MTLTexture> ramps =
      static_cast<GPUTextureMTL*>(atlas.GetTexture())->GetTexture();
  MTLTextureDescriptor* readback = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:GradientAtlas::kWidth
                                  height:1
                               mipmapped:NO];
  readback.storageMode = MTLStorageModeShared;
  id<MTLTexture> row = [context->GetDevice() newTextureWithDescriptor:readback];
  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  [blit copyFromTexture:ramps
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, location->row, 0)
             sourceSize:MTLSizeMake(GradientAtlas::kWidth, 1, 1)
              toTexture:row
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  std::vector<uint8_t> texels(GradientAtlas::kWidth * 4);
  [row getBytes:texels.data()
      bytesPerRow:GradientAtlas::kWidth * 4
       fromRegion:MTLRegionMake2D(0, 0, GradientAtlas::kWidth, 1)
      mipmapLevel:0];
  auto at = [&texels](int32_t x) {
    const uint8_t* p = texels.data() + x * 4;
    return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
  };

  const Color quarter = at(GradientAtlas::kWidth / 4);
  EXPECT_GT(quarter.green, 0.97f) << "the middle stop's own colour";
  EXPECT_LT(quarter.red, 0.03f);
  EXPECT_LT(quarter.blue, 0.03f);

  // An eighth in is halfway along the first segment, and five eighths
  // is halfway along the second: red-green, then green-blue.
  const Color eighth = at(GradientAtlas::kWidth / 8);
  EXPECT_NEAR(eighth.red, 0.5f, 0.03f);
  EXPECT_NEAR(eighth.green, 0.5f, 0.03f);
  const Color late = at(GradientAtlas::kWidth * 5 / 8);
  EXPECT_NEAR(late.green, 0.5f, 0.03f);
  EXPECT_NEAR(late.blue, 0.5f, 0.03f);
}

}  // namespace testing
}  // namespace impeller

namespace impeller {
namespace testing {

namespace {
/// A renderer, or nothing when the machine has no Metal device.
std::unique_ptr<MetalRenderer> MakeClipRenderer(
    std::shared_ptr<GPUContextMTL>& context) {
  context = GPUContextMTL::Make();
  if (!context) {
    return nullptr;
  }
  auto atlas = std::make_shared<PagedAtlas>(
      context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  return MetalRenderer::Make(context, atlas);
}
}  // namespace

TEST(MetalRendererTest, AClipMasksWhatFallsOutsideIt) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.ClipRect(Rect::MakeLTRB(40, 40, 80, 80));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 120, 120),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  EXPECT_GT(green_at(60, 60), 200) << "inside the clip";
  EXPECT_LT(green_at(20, 60), 20) << "left of it";
  EXPECT_LT(green_at(100, 60), 20) << "right of it";
  EXPECT_LT(green_at(60, 20), 20) << "above it";
  EXPECT_LT(green_at(60, 100), 20) << "below it";
}

TEST(MetalRendererTest, ClipsIntersect) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.ClipRect(Rect::MakeLTRB(20, 20, 80, 80));
  builder.ClipRect(Rect::MakeLTRB(40, 40, 100, 100));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 120, 120),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  // Only where the two overlap.
  EXPECT_GT(green_at(60, 60), 200) << "in both";
  EXPECT_LT(green_at(30, 30), 20) << "in the first alone";
  EXPECT_LT(green_at(90, 90), 20) << "in the second alone";
}

TEST(MetalRendererTest, ARestoredClipStopsMasking) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.ClipRect(Rect::MakeLTRB(20, 20, 100, 100));
  builder.Save();
  builder.ClipRect(Rect::MakeLTRB(40, 40, 60, 60));
  builder.Restore();
  builder.DrawRect(Rect::MakeLTRB(0, 0, 120, 120),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  // The inner clip was popped, so the draw is under the outer one only.
  EXPECT_GT(green_at(50, 50), 200) << "inside both";
  EXPECT_GT(green_at(90, 90), 200) << "outside the popped clip";
  EXPECT_GT(green_at(30, 30), 200) << "outside the popped clip";
  EXPECT_LT(green_at(10, 10), 20) << "still outside the outer clip";
}

TEST(MetalRendererTest, ARoundRectClipRampsItsEdge) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.ClipRoundRect(
      RoundRect::MakeRectXY(Rect::MakeLTRB(20, 20, 100, 100), 30, 30));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 120, 120),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto alpha_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 3];
  };

  EXPECT_GT(alpha_at(60, 60), 250) << "the middle";
  EXPECT_LT(alpha_at(22, 22), 20) << "the corner the arc cuts off";
  // Somewhere along the corner arc a pixel is partly covered: the clip
  // is coverage, not a stencil. The diagonal crosses the arc at about
  // (29, 29), where the radius meets it at 45 degrees.
  int partial = 0;
  for (int32_t i = 0; i < 30; i++) {
    const uint8_t alpha = alpha_at(20 + i, 20 + i);
    if (alpha > 20 && alpha < 235) {
      partial++;
    }
  }
  EXPECT_GT(partial, 0) << "the arc is not ramped";
}

TEST(MetalRendererTest, AConvexPathClipMasksToTheShapeNotItsBounds) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  const Point points[3] = {Point(60, 20), Point(100, 100), Point(20, 100)};
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.ClipPath(flutter::DlPath::MakePoly(points, 3, /*close=*/true));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 120, 120),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  EXPECT_GT(green_at(60, 80), 200) << "inside the triangle";
  // The corners of the bounding box the triangle does not reach into.
  EXPECT_LT(green_at(25, 30), 20) << "above and left of the hypotenuse";
  EXPECT_LT(green_at(95, 30), 20) << "above and right of it";
}

TEST(MetalRendererTest, AConcavePathClipMasksItsNotch) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // A rectangle with a notch cut into the top, so the shape is concave
  // and the notch is interior to its bounds.
  const Point points[8] = {
      Point(20, 20), Point(40, 20),  Point(40, 70),   Point(80, 70),
      Point(80, 20), Point(100, 20), Point(100, 100), Point(20, 100),
  };
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.ClipPath(flutter::DlPath::MakePoly(points, 8, /*close=*/true));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 120, 120),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  EXPECT_GT(green_at(30, 40), 200) << "the left arm";
  EXPECT_GT(green_at(90, 40), 200) << "the right arm";
  EXPECT_GT(green_at(60, 90), 200) << "below the notch";
  EXPECT_LT(green_at(60, 40), 20) << "the notch itself";
  EXPECT_LT(green_at(10, 60), 20) << "outside the shape";
}

namespace {

/// Render a scene, rather than the one picture the others draw.
std::vector<uint8_t> RenderScene(MetalRenderer& renderer,
                                 const PrSceneNode& scene) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:120
                                  height:120
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> target =
      [renderer.GetDevice() newTextureWithDescriptor:descriptor];
  [renderer.Render(scene, target, Color(0, 0, 0, 0)) waitUntilCompleted];

  std::vector<uint8_t> rgba(120 * 120 * 4);
  [target getBytes:rgba.data()
       bytesPerRow:120 * 4
        fromRegion:MTLRegionMake2D(0, 0, 120, 120)
       mipmapLevel:0];
  return rgba;
}

/// A picture of one green rect over the whole surface.
std::shared_ptr<PrPicture> GreenSurface() {
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawRect(Rect::MakeLTRB(0, 0, 120, 120),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  return builder.Build();
}

}  // namespace

TEST(MetalRendererTest, ASceneClipMasksWhatFallsOutsideIt) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrSceneBuilder scene(Rect::MakeLTRB(0, 0, 120, 120));
  scene.ClipRect(Rect::MakeLTRB(40, 40, 80, 80));
  scene.DrawPicture(GreenSurface(), 1.0f);

  const std::vector<uint8_t> pixels = RenderScene(*renderer, scene.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  EXPECT_GT(green_at(60, 60), 200) << "inside the clip";
  EXPECT_LT(green_at(20, 60), 20) << "left of it";
  EXPECT_LT(green_at(100, 60), 20) << "right of it";
  EXPECT_LT(green_at(60, 20), 20) << "above it";
  EXPECT_LT(green_at(60, 100), 20) << "below it";
}

TEST(MetalRendererTest, ASceneRoundRectClipMasksItsCorners) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrSceneBuilder scene(Rect::MakeLTRB(0, 0, 120, 120));
  scene.ClipRoundRect(
      RoundRect::MakeRectXY(Rect::MakeLTRB(20, 20, 100, 100), 30, 30));
  scene.DrawPicture(GreenSurface(), 1.0f);

  const std::vector<uint8_t> pixels = RenderScene(*renderer, scene.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  EXPECT_GT(green_at(60, 60), 200) << "the middle";
  EXPECT_LT(green_at(22, 22), 20) << "the corner the arc cuts off";
}

TEST(MetalRendererTest, AMatrixImageFilterMovesWhatTheLayerResolvedInto) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrPictureBuilder inner;
  inner.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  inner.DrawRect(Rect::MakeLTRB(10, 10, 30, 30),
                 flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  std::shared_ptr<PrPicture> content = inner.Build();

  PrSceneBuilder scene(Rect::MakeLTRB(0, 0, 120, 120));
  flutter::DlPaint paint;
  paint.setImageFilter(flutter::DlImageFilter::MakeMatrix(
      Matrix::MakeTranslation({60, 60, 0}), flutter::DlImageSampling::kLinear));
  scene.SaveLayer(std::nullopt, &paint);
  scene.DrawPicture(content, 1.0f);
  scene.Restore();

  const std::vector<uint8_t> pixels = RenderScene(*renderer, scene.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  EXPECT_LT(green_at(20, 20), 20) << "where it was recorded";
  EXPECT_GT(green_at(80, 80), 200) << "where the filter puts it";
}

TEST(MetalRendererTest, AMatrixImageFilterScalesUnderATransform) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrPictureBuilder inner;
  inner.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  inner.DrawRect(Rect::MakeLTRB(0, 0, 40, 40),
                 flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
  std::shared_ptr<PrPicture> content = inner.Build();

  // The filter's matrix is in the space of the saveLayer, so under a
  // translate it still doubles about that space's origin, not the
  // surface's.
  PrSceneBuilder scene(Rect::MakeLTRB(0, 0, 120, 120));
  scene.Translate(20, 20);
  flutter::DlPaint paint;
  paint.setImageFilter(flutter::DlImageFilter::MakeMatrix(
      Matrix::MakeScale({2, 2, 1}), flutter::DlImageSampling::kLinear));
  scene.SaveLayer(std::nullopt, &paint);
  scene.DrawPicture(content, 1.0f);
  scene.Restore();

  const std::vector<uint8_t> pixels = RenderScene(*renderer, scene.Build());
  auto green_at = [&pixels](int32_t x, int32_t y) {
    return pixels[(y * 120 + x) * 4 + 1];
  };

  // Recorded at 0..40, placed at 20..60, doubled about (20, 20) to
  // 20..100.
  EXPECT_GT(green_at(90, 90), 200) << "inside what the scale reaches";
  EXPECT_LT(green_at(110, 110), 20) << "past it";
}

TEST(MetalRendererTest, ScratchDrawAfterInlinedClippedPicture) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // What an SVG looks like: a picture that clips to its viewBox, draws,
  // and restores.
  PrPictureBuilder svg;
  svg.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  svg.Save();
  svg.ClipRect(Rect::MakeLTRB(40, 10, 80, 50));
  svg.DrawRect(Rect::MakeLTRB(40, 10, 80, 50),
               flutter::DlPaint().setColor(flutter::DlColor::kRed()));
  svg.Restore();

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawPicture(svg.Build());
  // Where the title would be, below the logo.
  builder.DrawRect(Rect::MakeLTRB(20, 70, 100, 100),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));

  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto at = [&pixels](int32_t x, int32_t y, int c) {
    return pixels[(y * 120 + x) * 4 + c];
  };

  EXPECT_GT(at(60, 30, 0), 200) << "the svg";
  EXPECT_GT(at(60, 85, 1), 200) << "what is drawn after it";
}

TEST(MetalRendererTest, ScratchDrawAfterFilteredInlinedPicture) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  PrPictureBuilder svg;
  svg.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  svg.Save();
  svg.ClipRect(Rect::MakeLTRB(40, 10, 80, 50));
  svg.DrawRect(Rect::MakeLTRB(40, 10, 80, 50),
               flutter::DlPaint().setColor(flutter::DlColor::kRed()));
  svg.Restore();
  std::shared_ptr<PrPicture> svg_picture = svg.Build();

  // What SvgPicture(colorFilter:) does: a save layer around the picture.
  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  flutter::DlPaint layer_paint;
  layer_paint.setColorFilter(flutter::DlColorFilter::MakeBlend(
      flutter::DlColor::kWhite(), flutter::DlBlendMode::kSrcIn));
  builder.SaveLayer(Rect::MakeLTRB(40, 10, 80, 50), &layer_paint);
  builder.DrawPicture(svg_picture);
  builder.Restore();
  builder.DrawRect(Rect::MakeLTRB(20, 70, 100, 100),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));

  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto at = [&pixels](int32_t x, int32_t y, int c) {
    return pixels[(y * 120 + x) * 4 + c];
  };

  EXPECT_GT(at(60, 30, 0), 200) << "the svg, through the layer";
  EXPECT_GT(at(60, 85, 1), 200) << "what is drawn after it";
}

TEST(MetalRendererTest, ScratchDrawAfterUnbalancedClipPicture) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // vector_graphics clips to the viewBox with no save of its own, so
  // the picture ends with the clip still standing.
  PrPictureBuilder svg;
  svg.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  svg.ClipRect(Rect::MakeLTRB(40, 10, 80, 50));
  svg.DrawRect(Rect::MakeLTRB(40, 10, 80, 50),
               flutter::DlPaint().setColor(flutter::DlColor::kRed()));

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawPicture(svg.Build());
  builder.DrawRect(Rect::MakeLTRB(20, 70, 100, 100),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));

  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto at = [&pixels](int32_t x, int32_t y, int c) {
    return pixels[(y * 120 + x) * 4 + c];
  };

  EXPECT_GT(at(60, 30, 0), 200) << "the svg";
  EXPECT_GT(at(60, 85, 1), 200) << "what is drawn after it";
}

TEST(MetalRendererTest, ScratchDrawAfterConcaveUnderUnbalancedClip) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // The compass: a four pointed star, concave, under a viewBox clip the
  // picture never restores.
  const Point star[8] = {
      Point(60, 10), Point(64, 26), Point(80, 30), Point(64, 34),
      Point(60, 50), Point(56, 34), Point(40, 30), Point(56, 26),
  };
  PrPictureBuilder svg;
  svg.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  svg.ClipRect(Rect::MakeLTRB(40, 10, 80, 50));
  svg.DrawPath(flutter::DlPath::MakePoly(star, 8, /*close=*/true),
               flutter::DlPaint().setColor(flutter::DlColor::kRed()));

  PrPictureBuilder builder;
  builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
  builder.DrawPicture(svg.Build());
  builder.DrawRect(Rect::MakeLTRB(20, 70, 100, 100),
                   flutter::DlPaint().setColor(flutter::DlColor::kGreen()));

  const std::vector<uint8_t> pixels = RenderPicture(*renderer, builder.Build());
  auto at = [&pixels](int32_t x, int32_t y, int c) {
    return pixels[(y * 120 + x) * 4 + c];
  };

  EXPECT_GT(at(60, 30, 0), 200) << "the star";
  EXPECT_GT(at(60, 85, 1), 200) << "what is drawn after it";
}

TEST(MetalRendererTest, ARestoreDoesNotMultiplyAClipIntoItself) {
  std::shared_ptr<GPUContextMTL> context;
  auto renderer = MakeClipRenderer(context);
  if (!renderer) {
    GTEST_SKIP() << "No Metal device available.";
  }

  // Coverage is multiplied in, so a clip replayed over a region that
  // was not put back first is squared: an edge at 25 reads 2, then 0.
  auto edge_alpha = [&](int pops) {
    PrPictureBuilder builder;
    builder.SetSurfaceBounds(Rect::MakeLTRB(0, 0, 120, 120));
    builder.ClipRoundRect(
        RoundRect::MakeRectXY(Rect::MakeLTRB(20, 20, 100, 100), 30, 30));
    for (int i = 0; i < pops; i++) {
      // Smaller than the round rect, so the region it puts back is not
      // the region the round rect masked.
      builder.Save();
      builder.ClipRect(Rect::MakeLTRB(50, 50, 70, 70));
      builder.Restore();
    }
    builder.DrawRect(Rect::MakeLTRB(0, 0, 120, 120),
                     flutter::DlPaint().setColor(flutter::DlColor::kGreen()));
    const std::vector<uint8_t> pixels =
        RenderPicture(*renderer, builder.Build());
    // The partly covered pixel where the diagonal crosses the arc.
    uint8_t partial = 0;
    for (int32_t i = 0; i < 30; i++) {
      const uint8_t alpha = pixels[((20 + i) * 120 + (20 + i)) * 4 + 3];
      if (alpha > 0 && alpha < 255) {
        partial = alpha;
      }
    }
    return partial;
  };

  const uint8_t alone = edge_alpha(0);
  ASSERT_GT(alone, 0) << "the arc is ramped to begin with";
  EXPECT_EQ(edge_alpha(1), alone) << "one clip pushed and popped over it";
  EXPECT_EQ(edge_alpha(2), alone) << "two";
}

}  // namespace testing
}  // namespace impeller
