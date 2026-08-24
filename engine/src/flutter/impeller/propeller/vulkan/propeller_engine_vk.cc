// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/vulkan/propeller_engine_vk.h"

#include <cstdlib>

#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/propeller/flow_recorder.h"
#include "impeller/propeller/freetype_glyph_rasterizer.h"
#include "impeller/propeller/skia_typeface_resolver.h"
#include "impeller/propeller/snapshot_hook.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/surface_context_vk.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

PropellerEngineVK* PropellerEngineVK::GetIfEnabled(
    const std::shared_ptr<Context>& context) {
  // On by default on this branch; FLUTTER_PROPELLER=0 opts out.
  const char* enabled = std::getenv("FLUTTER_PROPELLER");
  if ((enabled != nullptr && enabled[0] == '0') || !context) {
    return nullptr;
  }
  static PropellerEngineVK* engine = new PropellerEngineVK(context);
  return engine->valid_ ? engine : nullptr;
}

PropellerEngineVK::PropellerEngineVK(const std::shared_ptr<Context>& context)
    : impeller_context_(context) {
  ContextVK& context_vk = ContextVK::Cast(*context);
  const auto queue_index = context_vk.GetGraphicsQueue()->GetIndex();
  context_ = GPUContextVK::Adopt(
      context_vk.GetInstance(), context_vk.GetPhysicalDevice(),
      context_vk.GetDevice(), queue_index.family,
      context_vk.GetDevice().getQueue(queue_index.family, queue_index.index));
  if (!context_) {
    FML_LOG(ERROR) << "Propeller: could not adopt the Vulkan device.";
    return;
  }
  atlas_ = std::make_shared<PagedAtlas>(context_, TextureFormat::kR8UNorm, 2048,
                                        2048, kMLRMaxAtlasPages);
  glyph_rasterizer_ = FreeTypeGlyphRasterizer::Make();
  if (!glyph_rasterizer_) {
    return;
  }
  // Engine text frames carry Skia-backed typefaces; resolve them into
  // FreeType faces from their font data.
  glyph_rasterizer_->SetForeignTypefaceResolver(&ResolveSkiaTypeface);
  materializer_ = std::make_shared<TextMaterializer>(atlas_, glyph_rasterizer_);
  renderer_ = VulkanRenderer::Make(context_, atlas_, materializer_);
  if (!renderer_) {
    FML_LOG(ERROR) << "Propeller: bindless unsupported; staying disabled.";
    return;
  }
  valid_ = true;
  // The engine is a process singleton, so `this` outlives the handler.
  SetPropellerSnapshotHandler([this](const std::shared_ptr<PrPicture>& picture,
                                     uint32_t width, uint32_t height) {
    return RenderPictureToImage(picture, width, height);
  });
  FML_LOG(IMPORTANT) << "Propeller renderer enabled (Vulkan).";
}

PrSceneNode PropellerEngineVK::RecordTree(const flutter::Layer& root,
                                          uint32_t width,
                                          uint32_t height,
                                          const flutter::Stopwatch* raster_time,
                                          const flutter::Stopwatch* ui_time) {
  const Rect viewport = Rect::MakeXYWH(0, 0, width, height);
  if (builder_ == nullptr) {
    builder_ = std::make_unique<PrSceneBuilder>(viewport);
  }
  PrSceneBuilder& builder = *builder_;
  builder.SetSurfaceBounds(viewport);
  RecordFlowTree(builder, root, raster_time, ui_time);
  return builder.Build();
}

#if FML_OS_ANDROID
bool PropellerEngineVK::RenderLayerTreeToWindow(
    const flutter::Layer* root,
    uint32_t width,
    uint32_t height,
    Scalar device_pixel_ratio,
    const flutter::Stopwatch* raster_time,
    const flutter::Stopwatch* ui_time) {
  TRACE_EVENT0("flutter", "Propeller::RenderLayerTreeToWindow");
  if (!valid_ || root == nullptr || swapchain_ == nullptr) {
    return false;
  }
  if (swapchain_->GetWidth() != width || swapchain_->GetHeight() != height) {
    SetWindow(window_);
    if (swapchain_ == nullptr) {
      return false;
    }
  }
  // The swapchain's size is the truth: it just measured the window,
  // which is where the frame lands.
  const PrSceneNode scene =
      RecordTree(*root, swapchain_->GetWidth(), swapchain_->GetHeight(),
                 raster_time, ui_time);

  std::optional<PropellerSwapchainVK::AcquiredFrame> frame =
      swapchain_->Acquire();
  if (!frame.has_value() ||
      !renderer_->Render(scene, *frame->target, Color::BlackTransparent(),
                         frame->render_ready) ||
      !swapchain_->Present()) {
    FML_LOG(ERROR) << "Propeller: swapchain frame failed.";
    return false;
  }
  return true;
}

void PropellerEngineVK::SetWindow(ANativeWindow* window) {
  window_ = window;
  swapchain_.reset();
  if (!valid_ || window == nullptr) {
    return;
  }
  FML_LOG(ERROR) << "Propeller: creating swapchain.";
  swapchain_ =
      PropellerSwapchainVK::Make(impeller_context_.lock(), context_, window_);
  if (swapchain_ == nullptr) {
    FML_LOG(ERROR) << "Propeller: falling back to the engine swapchain.";
  }
}
#endif  // FML_OS_ANDROID

sk_sp<flutter::DlImage> PropellerEngineVK::RenderPictureToImage(
    const std::shared_ptr<PrPicture>& picture,
    uint32_t width,
    uint32_t height) {
  TRACE_EVENT0("flutter", "Propeller::RenderPictureToImage");
  const std::shared_ptr<Context> impeller_context = impeller_context_.lock();
  if (!valid_ || !impeller_context || picture == nullptr || width == 0 ||
      height == 0) {
    return nullptr;
  }
  // Allocated through impeller so the result is an ordinary engine
  // image; propeller renders into it borrowed, like a swapchain target.
  TextureDescriptor desc;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.format = PixelFormat::kR8G8B8A8UNormInt;
  desc.size = ISize(width, height);
  desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  const std::shared_ptr<Texture> texture =
      impeller_context->GetResourceAllocator()->CreateTexture(desc);
  if (!texture) {
    return nullptr;
  }
  TextureVK& target_vk = TextureVK::Cast(*texture);
  // The frame's command buffer holds `texture` until its fence: the
  // returned image can be disposed before the render completes.
  GPUTextureVK wrapper(context_->GetCore(),
                       TextureDesc{.format = TextureFormat::kRGBA8UNorm,
                                   .width = width,
                                   .height = height},
                       target_vk.GetImage(), target_vk.GetImageView(),
                       target_vk.GetLayout(), texture);
  if (!renderer_->Render(*picture, wrapper, Color::BlackTransparent())) {
    return nullptr;
  }
  target_vk.SetLayoutWithoutEncoding(wrapper.layout);
  return DlImageImpeller::Make(texture,
                               flutter::DlImage::OwningContext::kRaster);
}

bool PropellerEngineVK::RenderLayerTree(
    const flutter::Layer* root,
    RenderTarget& target,
    Scalar device_pixel_ratio,
    const flutter::Stopwatch* raster_time,
    const flutter::Stopwatch* ui_time,
    const SurfaceContextVK* swapchain_context) {
  TRACE_EVENT0("flutter", "Propeller::RenderLayerTree");
  if (!valid_ || root == nullptr) {
    return false;
  }
  // The texture the compositor presents: the resolve texture when the
  // target is MSAA, the color attachment otherwise.
  const std::shared_ptr<Texture> texture = target.GetRenderTargetTexture();
  if (!texture) {
    return false;
  }
  const auto width = static_cast<uint32_t>(texture->GetSize().width);
  const auto height = static_cast<uint32_t>(texture->GetSize().height);
  const PrSceneNode scene =
      RecordTree(*root, width, height, raster_time, ui_time);

  TextureVK& target_vk = TextureVK::Cast(*texture);
  const std::shared_ptr<const TextureSourceVK> source =
      target_vk.GetTextureSource();
  auto found = target_wrappers_.find(source.get());
  if (found == target_wrappers_.end()) {
    // Swapchain recreation retires images wholesale; dropping the map
    // is safe, in-flight frames hold their own references.
    if (target_wrappers_.size() >= 8) {
      target_wrappers_.clear();
    }
    auto cached = std::make_unique<GPUTextureVK>(
        context_->GetCore(),
        TextureDesc{.format = TextureFormat::kRGBA8UNorm,
                    .width = width,
                    .height = height},
        target_vk.GetImage(), target_vk.GetImageView(), target_vk.GetLayout(),
        source);
    found = target_wrappers_
                .emplace(source.get(), CachedTarget{source, std::move(cached)})
                .first;
  }
  GPUTextureVK& wrapper = *found->second.wrapper;
  // Impeller transitions the image between propeller frames (the present
  // barrier); re-sync the tracked layout every frame.
  wrapper.layout = target_vk.GetLayout();

  // The first submission touching an acquired swapchain image must wait
  // the acquire semaphore, and the present must wait this frame instead.
  const vk::Semaphore acquire =
      swapchain_context ? swapchain_context->TakeFrameRenderSemaphore()
                        : vk::Semaphore{};
  vk::Semaphore done;
  if (!renderer_->Render(scene, wrapper, Color::BlackTransparent(), acquire,
                         acquire ? &done : nullptr)) {
    FML_LOG(ERROR) << "Propeller frame render failed.";
    return false;
  }
  if (acquire && done) {
    swapchain_context->SetFrameRenderDone(done);
  }
  // The frame left the target shader-readable; keep impeller's layout
  // tracking coherent for its own present path.
  target_vk.SetLayoutWithoutEncoding(wrapper.layout);
  return true;
}

}  // namespace impeller
