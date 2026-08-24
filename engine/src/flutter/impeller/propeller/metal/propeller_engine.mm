// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/metal/propeller_engine.h"

#include <cstdlib>

#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/propeller/flow_recorder.h"
#include "impeller/propeller/freetype_glyph_rasterizer.h"
#include "impeller/propeller/propeller_shell_hook.h"
#include "impeller/propeller/skia_typeface_resolver.h"
#include "impeller/propeller/snapshot_hook.h"
#include "impeller/renderer/backend/metal/texture_mtl.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

PropellerEngine* PropellerEngine::GetIfEnabled() {
  // On by default on this branch; FLUTTER_PROPELLER=0 opts out.
  const char* enabled = std::getenv("FLUTTER_PROPELLER");
  if (enabled != nullptr && enabled[0] == '0') {
    return nullptr;
  }
  static PropellerEngine* engine = new PropellerEngine();
  return engine->valid_ ? engine : nullptr;
}

PropellerEngine::PropellerEngine() {
  context_ = GPUContextMTL::Make();
  if (!context_) {
    FML_LOG(ERROR) << "Propeller: no Metal device; staying disabled.";
    return;
  }
  atlas_ = std::make_shared<PagedAtlas>(context_.get(), TextureFormat::kR8UNorm, 4096,
                                        4096, 4);
  glyph_rasterizer_ = FreeTypeGlyphRasterizer::Make();
  if (!glyph_rasterizer_) {
    return;
  }
  glyph_rasterizer_->SetForeignTypefaceResolver(&ResolveSkiaTypeface);
  glyph_rasterizer_->SetForeignGlyphRasterizer(&RasterizeGlyphWithCoreText);
  materializer_ = std::make_shared<TextMaterializer>(atlas_, glyph_rasterizer_);
  renderer_ = MetalRenderer::Make(context_, atlas_, materializer_);
  if (!renderer_) {
    FML_LOG(ERROR) << "Propeller: no Metal 3 device; staying disabled.";
    return;
  }
  valid_ = true;
  // The engine is a process singleton, so `this` outlives the handler.
  SetPropellerSnapshotHandler([this](const std::shared_ptr<PrPicture>& picture,
                                     uint32_t width, uint32_t height) {
    return RenderPictureToImage(picture, width, height);
  });
  FML_LOG(IMPORTANT) << "Propeller renderer enabled.";
}

sk_sp<flutter::DlImage> PropellerEngine::RenderPictureToImage(
    const std::shared_ptr<PrPicture>& picture,
    uint32_t width,
    uint32_t height) {
  TRACE_EVENT0("flutter", "Propeller::RenderPictureToImage");
  if (!valid_ || picture == nullptr || width == 0 || height == 0) {
    return nullptr;
  }
  MTLTextureDescriptor* mtl_desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:width
                                  height:height
                               mipmapped:NO];
  mtl_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  mtl_desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> target =
      [context_->GetDevice() newTextureWithDescriptor:mtl_desc];
  if (target == nil) {
    return nullptr;
  }
  if (!renderer_->Render(*picture, target, Color::BlackTransparent())) {
    return nullptr;
  }
  // Wrapped so the rest of the engine sees an ordinary impeller-backed
  // image.
  TextureDescriptor desc;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.format = PixelFormat::kR8G8B8A8UNormInt;
  desc.size = ISize(width, height);
  desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  const std::shared_ptr<Texture> texture = TextureMTL::Wrapper(desc, target);
  if (!texture) {
    return nullptr;
  }
  return DlImageImpeller::Make(texture,
                               flutter::DlImage::OwningContext::kRaster);
}

bool PropellerEngine::RenderLayerTree(const flutter::Layer* root,
                                      id<MTLTexture> target,
                                      Scalar device_pixel_ratio,
                                      const flutter::Stopwatch* raster_time,
                                      const flutter::Stopwatch* ui_time) {
  TRACE_EVENT0("flutter", "Propeller::RenderLayerTree");
  if (!valid_ || target == nil) {
    return false;
  }
  const Rect viewport = Rect::MakeXYWH(0, 0, static_cast<Scalar>(target.width),
                                       static_cast<Scalar>(target.height));
  if (builder_ == nullptr) {
    builder_ = std::make_unique<PrSceneBuilder>(viewport);
  }
  PrSceneBuilder& builder = *builder_;
  builder.SetSurfaceBounds(viewport);
  RecordFlowTree(builder, *root, raster_time, ui_time);

  const PrSceneNode scene = builder.Build();
  id<MTLCommandBuffer> command_buffer =
      renderer_->Render(scene, target, Color::BlackTransparent());
  if (command_buffer == nil) {
    return false;
  }
  [command_buffer waitUntilScheduled];
  return true;
}

bool PropellerRenderFlowTree(const flutter::Layer* root,
                             RenderTarget& target,
                             float device_pixel_ratio,
                             const flutter::Stopwatch* raster_time,
                             const flutter::Stopwatch* ui_time) {
  PropellerEngine* engine = PropellerEngine::GetIfEnabled();
  if (engine == nullptr) {
    return false;
  }
  // The texture the compositor presents: the resolve texture when the
  // target is MSAA, the color attachment otherwise.
  std::shared_ptr<Texture> texture = target.GetRenderTargetTexture();
  if (!texture) {
    return false;
  }
  id<MTLTexture> mtl_texture = TextureMTL::Cast(*texture).GetMTLTexture();
  return engine->RenderLayerTree(root, mtl_texture, device_pixel_ratio,
                                 raster_time, ui_time);
}

}  // namespace impeller
