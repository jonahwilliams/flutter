// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_RENDERER_H_
#define FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_RENDERER_H_

#include <memory>

#include "impeller/geometry/color.h"
#include "impeller/propeller/dispatcher.h"
#include "impeller/propeller/paged_atlas.h"
#include "impeller/propeller/text_materializer.h"
#include "impeller/propeller/vulkan/vulkan_context.h"

namespace impeller {

//------------------------------------------------------------------------------
class VulkanRenderer {
 public:
  /// Returns nullptr when the device cannot do bindless.
  ///
  /// `context` supplies the device; the atlas must allocate its pages
  /// from the same one. The gradient atlas is the renderer's own, for
  /// the same reason.
  static std::unique_ptr<VulkanRenderer> Make(
      std::shared_ptr<GPUContextVK> context,
      std::shared_ptr<PagedAtlas> atlas,
      std::shared_ptr<TextMaterializer> materializer = nullptr);

  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  /// Encode a propeller scene into `target` and submit.
  bool Render(const PrSceneNode& scene,
              GPUTexture& target,
              Color clear_color,
              vk::Semaphore wait_acquire = nullptr,
              vk::Semaphore* render_done = nullptr);

  /// The same, for a lone picture: a scene of just that picture.
  bool Render(const PrPicture& picture,
              GPUTexture& target,
              Color clear_color,
              vk::Semaphore wait_acquire = nullptr,
              vk::Semaphore* render_done = nullptr);

  /// Passes actually encoded by the last RenderFrame (root included);
  /// recycled passes don't count.
  uint32_t GetLastFrameEncodedPassCount() const;

 private:
  struct Impl;

  explicit VulkanRenderer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_RENDERER_H_
