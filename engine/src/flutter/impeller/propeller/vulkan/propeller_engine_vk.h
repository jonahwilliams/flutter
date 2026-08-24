// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_VULKAN_PROPELLER_ENGINE_VK_H_
#define FLUTTER_IMPELLER_PROPELLER_VULKAN_PROPELLER_ENGINE_VK_H_

#include <memory>
#include <unordered_map>

#include "flutter/display_list/image/dl_image.h"
#include "flutter/flow/layers/layer.h"  // nogncheck
#include "flutter/fml/build_config.h"
#include "impeller/propeller/scene.h"
#include "impeller/propeller/text_materializer.h"
#include "impeller/propeller/vulkan/vulkan_renderer.h"

#if FML_OS_ANDROID
#include "impeller/propeller/vulkan/propeller_swapchain_vk.h"
#endif  // FML_OS_ANDROID

namespace flutter {
class Stopwatch;
}  // namespace flutter

namespace impeller {

class Context;
class FreeTypeGlyphRasterizer;
class RenderTarget;
class SurfaceContextVK;
class TextureSourceVK;

//------------------------------------------------------------------------------
/// The Propeller renderer over an embedder's Vulkan device: the Android
/// integration point. Adopts `context`'s device so the engine's images
/// and render targets are directly usable.
class PropellerEngineVK {
 public:
  /// The engine, created against `context` on first use. nullptr when
  /// opted out (FLUTTER_PROPELLER=0) or the device cannot do bindless.
  /// One engine per process; every call must pass the same context.
  static PropellerEngineVK* GetIfEnabled(
      const std::shared_ptr<Context>& context);

  /// Render one frame of a flow layer tree into the target's color
  /// attachment.
  ///
  /// `swapchain_context` is the surface the target was acquired from,
  /// when it is a swapchain image: the frame's submission then waits the
  /// acquire semaphore and hands the present one it signals. Null for
  /// offscreen targets.
  bool RenderLayerTree(const flutter::Layer* root,
                       RenderTarget& target,
                       Scalar device_pixel_ratio,
                       const flutter::Stopwatch* raster_time = nullptr,
                       const flutter::Stopwatch* ui_time = nullptr,
                       const SurfaceContextVK* swapchain_context = nullptr);

  /// Render `picture` into a fresh texture and wrap it as an engine
  /// image: the raster half of Picture.toImageSync.
  sk_sp<flutter::DlImage> RenderPictureToImage(
      const std::shared_ptr<PrPicture>& picture,
      uint32_t width,
      uint32_t height);

#if FML_OS_ANDROID
  /// Adopt the engine window: propeller then presents through its own
  /// SurfaceControl layer instead of into impeller's swapchain image,
  /// and no impeller synchronization participates. Null tears the layer
  /// down.
  void SetWindow(ANativeWindow* window);

  /// Whether RenderLayerTreeToWindow can present: a window was adopted
  /// and its swapchain stood up. The shell then skips the engine
  /// swapchain entirely -- no acquire, no present underneath.
  [[nodiscard]] bool CanPresentToWindow() const {
    return window_ != nullptr && swapchain_ != nullptr;
  }

  /// Render one frame of a flow layer tree into propeller's own
  /// swapchain and present it. `width`/`height` are the frame size the
  /// shell wants; a mismatch with the swapchain means the window
  /// resized, and the swapchain follows before rendering.
  bool RenderLayerTreeToWindow(const flutter::Layer* root,
                               uint32_t width,
                               uint32_t height,
                               Scalar device_pixel_ratio,
                               const flutter::Stopwatch* raster_time = nullptr,
                               const flutter::Stopwatch* ui_time = nullptr);
#endif  // FML_OS_ANDROID

 private:
  explicit PropellerEngineVK(const std::shared_ptr<Context>& context);

  PrSceneNode RecordTree(const flutter::Layer& root,
                         uint32_t width,
                         uint32_t height,
                         const flutter::Stopwatch* raster_time,
                         const flutter::Stopwatch* ui_time);

  bool valid_ = false;
  std::weak_ptr<Context> impeller_context_;
  std::shared_ptr<GPUContextVK> context_;
  std::shared_ptr<PagedAtlas> atlas_;
  std::shared_ptr<FreeTypeGlyphRasterizer> glyph_rasterizer_;
  std::shared_ptr<TextMaterializer> materializer_;
  std::unique_ptr<VulkanRenderer> renderer_;
  std::unique_ptr<PrSceneBuilder> builder_;

  /// Root-target wrappers per swapchain image, persistent so the
  /// framebuffer cached on the wrapper survives across frames. The
  /// texture source is the stable identity across acquires (impeller
  /// wraps it in a fresh TextureVK per frame); holding it pins the key
  /// pointer against reuse.
  struct CachedTarget {
    std::shared_ptr<const TextureSourceVK> source;
    std::unique_ptr<GPUTextureVK> wrapper;
  };
  std::unordered_map<const TextureSourceVK*, CachedTarget> target_wrappers_;

#if FML_OS_ANDROID
  ANativeWindow* window_ = nullptr;
  std::shared_ptr<PropellerSwapchainVK> swapchain_;
#endif  // FML_OS_ANDROID
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_VULKAN_PROPELLER_ENGINE_VK_H_
