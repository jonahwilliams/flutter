// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_GPU_GPU_SURFACE_VULKAN_IMPELLER_H_
#define FLUTTER_SHELL_GPU_GPU_SURFACE_VULKAN_IMPELLER_H_

#include "flutter/common/graphics/gl_context_switch.h"
#include "flutter/flow/surface.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/memory/weak_ptr.h"
#include "flutter/impeller/display_list/aiks_context.h"
#include "flutter/impeller/renderer/context.h"
#include "flutter/shell/gpu/gpu_surface_vulkan_delegate.h"
#include "impeller/renderer/backend/vulkan/swapchain/swapchain_transients_vk.h"

namespace flutter {

namespace testing {
FML_TEST_CLASS(GPUSurfaceVulkanImpeller,
               RecreatesTransientsWhenFrameSizeChanges);
}  // namespace testing

class GPUSurfaceVulkanImpeller final : public Surface {
 public:
  explicit GPUSurfaceVulkanImpeller(GPUSurfaceVulkanDelegate* delegate,
                                    std::shared_ptr<impeller::Context> context);

  // |Surface|
  ~GPUSurfaceVulkanImpeller() override;

  // |Surface|
  bool IsValid() override;

  // |Surface|
  void SetPropellerLayerTree(flutter::LayerTree* layer_tree,
                             float device_pixel_ratio,
                             const Stopwatch* raster_time,
                             const Stopwatch* ui_time) override {
    propeller_layer_tree_ = layer_tree;
    propeller_device_pixel_ratio_ = device_pixel_ratio;
    propeller_raster_time_ = raster_time;
    propeller_ui_time_ = ui_time;
  }

 private:
  FML_FRIEND_TEST(testing::GPUSurfaceVulkanImpeller,
                  RecreatesTransientsWhenFrameSizeChanges);

  GPUSurfaceVulkanDelegate* delegate_;
  std::shared_ptr<impeller::Context> impeller_context_;
  std::shared_ptr<impeller::AiksContext> aiks_context_;
  std::shared_ptr<impeller::SwapchainTransientsVK> transients_;
  /// The size of the textures in [transients_]
  impeller::ISize transients_size_ = {};
  bool is_valid_ = false;
  // Propeller prototype: stashed per frame by the rasterizer; consumed by
  // the encode callback.
  flutter::LayerTree* propeller_layer_tree_ = nullptr;
  float propeller_device_pixel_ratio_ = 1.0f;
  const Stopwatch* propeller_raster_time_ = nullptr;
  const Stopwatch* propeller_ui_time_ = nullptr;

  // |Surface|
  std::unique_ptr<SurfaceFrame> AcquireFrame(const DlISize& size) override;

  // |Surface|
  DlMatrix GetRootTransformation() const override;

  // |Surface|
  GrDirectContext* GetContext() override;

  // |Surface|
  std::unique_ptr<GLContextResult> MakeRenderContextCurrent() override;

  // |Surface|
  bool EnableRasterCache() const override;

  // |Surface|
  std::shared_ptr<impeller::AiksContext> GetAiksContext() const override;

  FML_DISALLOW_COPY_AND_ASSIGN(GPUSurfaceVulkanImpeller);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_GPU_GPU_SURFACE_VULKAN_IMPELLER_H_
