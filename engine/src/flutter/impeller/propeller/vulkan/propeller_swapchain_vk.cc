// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/vulkan/propeller_swapchain_vk.h"

#if FML_OS_ANDROID

#include <limits>
#include <utility>

#include "flutter/fml/logging.h"
#include "impeller/toolkit/android/native_window.h"
#include "impeller/toolkit/android/surface_transaction.h"
#include "impeller/toolkit/android/surface_transaction_stats.h"

namespace impeller {

std::shared_ptr<PropellerSwapchainVK> PropellerSwapchainVK::Make(
    std::shared_ptr<Context> impeller_context,
    std::shared_ptr<GPUContextVK> context,
    ANativeWindow* window) {
  if (!impeller_context || !context || window == nullptr) {
    return nullptr;
  }
  android::NativeWindow native_window(window);
  if (!native_window.IsValid()) {
    return nullptr;
  }
  auto control = android::SurfaceControl::Create(window, "PropellerSurface");
  if (!control || !control->IsValid()) {
    FML_LOG(ERROR) << "Propeller: could not create a surface control.";
    return nullptr;
  }
  auto swapchain = std::shared_ptr<PropellerSwapchainVK>(
      new PropellerSwapchainVK(std::move(impeller_context), std::move(context),
                               std::move(control), native_window.GetSize()));
  return swapchain->IsValid() ? swapchain : nullptr;
}

PropellerSwapchainVK::PropellerSwapchainVK(
    std::shared_ptr<Context> impeller_context,
    std::shared_ptr<GPUContextVK> context,
    std::unique_ptr<android::SurfaceControl> control,
    const ISize& size)
    : impeller_context_(std::move(impeller_context)),
      context_(std::move(context)),
      surface_control_(std::move(control)) {
  desc_ = android::HardwareBufferDescriptor::MakeForSwapchainImage(size);
  pool_ = std::make_shared<AHBTexturePoolVK>(impeller_context_, desc_);
  if (!pool_->IsValid()) {
    return;
  }
  for (FrameSlot& slot : slots_) {
    auto [result, fence] = context_->GetCore()->device->createFenceUnique({});
    if (result != vk::Result::eSuccess) {
      return;
    }
    slot.fence = std::move(fence);
  }
  is_valid_ = true;
}

PropellerSwapchainVK::~PropellerSwapchainVK() {
  for (FrameSlot& slot : slots_) {
    if (slot.pending) {
      (void)context_->GetCore()->device->waitForFences(
          slot.fence.get(), VK_TRUE, std::numeric_limits<uint64_t>::max());
    }
  }
}

vk::UniqueSemaphore PropellerSwapchainVK::ImportRenderReady(
    const std::shared_ptr<fml::UniqueFD>& fd) const {
  if (!fd || !fd->is_valid()) {
    return {};
  }
  const vk::Device device = context_->GetCore()->device.get();
  auto [result, semaphore] = device.createSemaphoreUnique({});
  if (result != vk::Result::eSuccess) {
    return {};
  }
  vk::ImportSemaphoreFdInfoKHR import_info;
  import_info.semaphore = semaphore.get();
  import_info.fd = fd->get();
  import_info.handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd;
  // Sync fds can only be imported temporarily; the payload restores
  // after the one wait.
  import_info.flags = vk::SemaphoreImportFlagBitsKHR::eTemporary;
  if (device.importSemaphoreFdKHR(import_info) != vk::Result::eSuccess) {
    FML_LOG(ERROR) << "Propeller: could not import the release fence.";
    return {};
  }
  // A successful import owns the fd.
  (void)fd->release();
  return std::move(semaphore);
}

std::optional<PropellerSwapchainVK::AcquiredFrame>
PropellerSwapchainVK::Acquire() {
  slot_index_ = (slot_index_ + 1) % kPendingPresents;
  FrameSlot& slot = slots_[slot_index_];
  const vk::Device device = context_->GetCore()->device.get();
  if (slot.pending) {
    if (device.waitForFences(slot.fence.get(), VK_TRUE,
                             std::numeric_limits<uint64_t>::max()) !=
        vk::Result::eSuccess) {
      return std::nullopt;
    }
    if (device.resetFences(slot.fence.get()) != vk::Result::eSuccess) {
      return std::nullopt;
    }
    slot.pending = false;
  }
  slot.present_commands.reset();
  slot.present_ready.reset();
  slot.render_ready.reset();
  slot.texture.reset();

  AHBTexturePoolVK::PoolEntry entry = pool_->Pop();
  if (!entry.IsValid() || !entry.texture->IsValid()) {
    FML_LOG(ERROR) << "Propeller: could not obtain a swapchain buffer.";
    return std::nullopt;
  }
  slot.render_ready = ImportRenderReady(entry.render_ready_fence);
  slot.texture = entry.texture;

  auto found = targets_.find(entry.texture.get());
  if (found == targets_.end()) {
    // The pool GCs buffers; stale wrappers are only memory. In-flight
    // uses hold their own references.
    if (targets_.size() >= 8) {
      targets_.clear();
    }
    auto wrapper = std::make_unique<GPUTextureVK>(
        context_->GetCore(),
        TextureDesc{.format = TextureFormat::kRGBA8UNorm,
                    .width = static_cast<uint32_t>(desc_.size.width),
                    .height = static_cast<uint32_t>(desc_.size.height)},
        entry.texture->GetImage(), entry.texture->GetImageView(),
        vk::ImageLayout::eUndefined, entry.texture);
    found = targets_
                .emplace(entry.texture.get(),
                         CachedTarget{entry.texture, std::move(wrapper)})
                .first;
  }
  return AcquiredFrame{
      .target = found->second.wrapper.get(),
      .render_ready = slot.render_ready.get(),
  };
}

bool PropellerSwapchainVK::Present() {
  FrameSlot& slot = slots_[slot_index_];
  if (slot.texture == nullptr) {
    return false;
  }
  auto found = targets_.find(slot.texture.get());
  FML_DCHECK(found != targets_.end());

  auto commands = context_->CreateCommandBuffer();
  if (commands == nullptr) {
    return false;
  }
  commands->PrepareForPresent(*found->second.wrapper);

  auto present_ready = std::make_shared<ExternalSemaphoreVK>(impeller_context_);
  if (!present_ready->IsValid()) {
    return false;
  }
  // Queue order places this behind the frame's rendering, so the
  // exported fence fires when the frame is on the buffer.
  if (!context_->Submit(*commands, slot.fence.get(), nullptr,
                        present_ready->GetHandle())) {
    return false;
  }
  slot.pending = true;
  slot.present_commands = std::move(commands);
  slot.present_ready = present_ready;

  // Always a native transaction: a Java-created one is only applied by
  // the Java platform-views flow, which does not run for these frames.
  android::SurfaceTransaction transaction;
  if (!transaction.SetContents(surface_control_.get(),
                               slot.texture->GetBackingStore(),
                               present_ready->CreateFD())) {
    FML_LOG(ERROR) << "Propeller: could not set the surface contents.";
    return false;
  }
  return transaction.Apply(
      [texture = slot.texture,
       weak = weak_from_this()](ASurfaceTransactionStats* stats) {
        if (auto swapchain = weak.lock()) {
          swapchain->OnPresentComplete(std::move(texture), stats);
        }
      });
}

void PropellerSwapchainVK::OnPresentComplete(
    std::shared_ptr<AHBTextureSourceVK> texture,
    ASurfaceTransactionStats* stats) {
  // The compositor now references this buffer; the previous one is
  // reusable once its release fence fires, which the next user of the
  // pool entry waits on.
  auto release_fence =
      android::CreatePreviousReleaseFence(*surface_control_, stats);
  std::lock_guard<std::mutex> lock(displayed_mutex_);
  auto old_texture = std::move(displayed_texture_);
  displayed_texture_ = std::move(texture);
  pool_->Push(std::move(old_texture), std::move(release_fence));
}

}  // namespace impeller

#endif  // FML_OS_ANDROID
