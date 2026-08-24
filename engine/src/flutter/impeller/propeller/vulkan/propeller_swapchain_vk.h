// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_VULKAN_PROPELLER_SWAPCHAIN_VK_H_
#define FLUTTER_IMPELLER_PROPELLER_VULKAN_PROPELLER_SWAPCHAIN_VK_H_

#include "flutter/fml/build_config.h"

#if FML_OS_ANDROID

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "impeller/propeller/vulkan/vulkan_context.h"
#include "impeller/renderer/backend/vulkan/android/ahb_texture_source_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/ahb/ahb_texture_pool_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/ahb/external_semaphore_vk.h"
#include "impeller/toolkit/android/surface_control.h"
#include "impeller/toolkit/android/surface_transaction.h"

namespace impeller {

class Context;

//------------------------------------------------------------------------------
/// Propeller's own presentation path: hardware-buffer images composited
/// through a SurfaceControl child layer of the engine's window. Every
/// sync object -- the buffer-release fence in, the present-ready fence
/// out, the pacing ring -- is created and tracked here, so nothing in
/// the impeller backend participates. The discipline is the impeller
/// AHB swapchain's, minus its final-command-buffer indirection: the
/// present submission is a propeller command buffer.
class PropellerSwapchainVK
    : public std::enable_shared_from_this<PropellerSwapchainVK> {
 public:
  /// `impeller_context` allocates the hardware-buffer textures and
  /// external semaphores; `context` (adopted from the same device)
  /// records and submits. Returns nullptr when the surface control
  /// cannot be created.
  ///
  /// Presents always use natively created transactions: Java-created
  /// ones are only ever applied by the Java platform-views flow, which
  /// does not run for propeller frames.
  static std::shared_ptr<PropellerSwapchainVK> Make(
      std::shared_ptr<Context> impeller_context,
      std::shared_ptr<GPUContextVK> context,
      ANativeWindow* window);

  ~PropellerSwapchainVK();

  PropellerSwapchainVK(const PropellerSwapchainVK&) = delete;
  PropellerSwapchainVK& operator=(const PropellerSwapchainVK&) = delete;

  [[nodiscard]] uint32_t GetWidth() const { return desc_.size.width; }
  [[nodiscard]] uint32_t GetHeight() const { return desc_.size.height; }

  struct AcquiredFrame {
    /// Borrowed wrapper; its owner keep-alive is the hardware-buffer
    /// texture source, so command buffers that reference it hold the
    /// backing until their fences.
    GPUTextureVK* target = nullptr;
    /// The compositor's release of this buffer. The frame's first
    /// submission touching the image waits it at the color-attachment
    /// stage. Null when no wait is needed.
    vk::Semaphore render_ready;
  };

  /// Blocks on the pacing fence from two presents ago, like the
  /// impeller ring.
  std::optional<AcquiredFrame> Acquire();

  /// Transition the acquired image for the compositor, submit the
  /// present-ready signal ordered behind the frame's rendering, and
  /// hand the buffer to the surface control.
  bool Present();

 private:
  static constexpr size_t kPendingPresents = 2;

  /// One pending present's objects, reclaimed when its fence proves the
  /// present submission retired.
  struct FrameSlot {
    vk::UniqueFence fence;
    bool pending = false;
    /// Imported from the buffer-release sync fd, fresh per acquire.
    vk::UniqueSemaphore render_ready;
    std::shared_ptr<ExternalSemaphoreVK> present_ready;
    std::unique_ptr<GpuCommandBufferVK> present_commands;
    std::shared_ptr<AHBTextureSourceVK> texture;
  };

  /// Persistent per hardware buffer, so the framebuffer cached on the
  /// wrapper survives across acquires. The held source pins the key
  /// pointer against reuse.
  struct CachedTarget {
    std::shared_ptr<AHBTextureSourceVK> source;
    std::unique_ptr<GPUTextureVK> wrapper;
  };

  PropellerSwapchainVK(std::shared_ptr<Context> impeller_context,
                       std::shared_ptr<GPUContextVK> context,
                       std::unique_ptr<android::SurfaceControl> control,
                       const ISize& size);

  [[nodiscard]] bool IsValid() const { return is_valid_; }

  vk::UniqueSemaphore ImportRenderReady(
      const std::shared_ptr<fml::UniqueFD>& fd) const;

  void OnPresentComplete(std::shared_ptr<AHBTextureSourceVK> texture,
                         ASurfaceTransactionStats* stats);

  std::shared_ptr<Context> impeller_context_;
  std::shared_ptr<GPUContextVK> context_;
  std::shared_ptr<android::SurfaceControl> surface_control_;
  android::HardwareBufferDescriptor desc_;
  std::shared_ptr<AHBTexturePoolVK> pool_;
  FrameSlot slots_[kPendingPresents];
  size_t slot_index_ = 0;
  std::unordered_map<const AHBTextureSourceVK*, CachedTarget> targets_;
  /// The transaction-complete callback fires on a system thread.
  std::mutex displayed_mutex_;
  std::shared_ptr<AHBTextureSourceVK> displayed_texture_;
  bool is_valid_ = false;
};

}  // namespace impeller

#endif  // FML_OS_ANDROID

#endif  // FLUTTER_IMPELLER_PROPELLER_VULKAN_PROPELLER_SWAPCHAIN_VK_H_
