// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_CONTEXT_H_
#define FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_CONTEXT_H_

#include <memory>
#include <unordered_map>
#include <vector>

#include "flutter/fml/native_library.h"
#include "impeller/propeller/renderer/gpu_context.h"
#include "impeller/renderer/backend/vulkan/vk.h"
#include "impeller/renderer/backend/vulkan/vma.h"

namespace impeller {

inline vk::Format MLRVkFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::kR8UNorm:
      return vk::Format::eR8Unorm;
    case TextureFormat::kRGBA8UNorm:
      return vk::Format::eR8G8B8A8Unorm;
    case TextureFormat::kBGRA8UNorm:
      return vk::Format::eB8G8R8A8Unorm;
    case TextureFormat::kR16Float:
      return vk::Format::eR16Sfloat;
    case TextureFormat::kBGRA10XR:
      // An Apple format, and nothing here hands one out: a Vulkan
      // swapchain never comes back in it.
      return vk::Format::eUndefined;
  }
  FML_UNREACHABLE();
}

/// The device-lifetime Vulkan objects, shared by every resource created
/// from the context so destruction order never matters. Members destroy
/// in reverse declaration order: allocations before the allocator, the
/// allocator before the device, the device before the instance.
struct VulkanCore {
  ~VulkanCore();

  fml::RefPtr<fml::NativeLibrary> loader_library;
  vk::UniqueInstance instance;
  vk::PhysicalDevice physical_device;
  vk::UniqueDevice device;
  uint32_t queue_family = 0;
  vk::Queue queue;
  vk::UniqueCommandPool command_pool;
  /// False when the instance and device are adopted from an embedder
  /// (impeller's ContextVK on Android): everything created here is still
  /// destroyed, the adopted handles are released instead.
  bool owns_device = true;
  /// Runtime-sized descriptor arrays with update-after-bind: what the
  /// bindless texture table needs beyond a 1.2 core device.
  bool bindless_supported = false;
  /// The device's minStorageBufferOffsetAlignment (64 on Mali, 16 or
  /// less elsewhere), floored at 16.
  size_t buffer_alignment = 16;

  /// The single-attachment pass for `format`. Compatibility ignores
  /// load/store ops, but initialLayout and format do not.
  vk::RenderPass GetRenderPass(vk::Format format, bool load);

  std::unordered_map<uint64_t, vk::UniqueRenderPass> render_passes;

  struct AllocatorHolder {
    VmaAllocator allocator = {};
    ~AllocatorHolder();
  };
  AllocatorHolder vma;
};

class GPUTextureVK final : public GPUTexture {
 public:
  GPUTextureVK(std::shared_ptr<VulkanCore> core,
               const TextureDesc& desc,
               vk::Image image,
               VmaAllocation allocation,
               vk::UniqueImageView view);

  /// Wrap an image somebody else owns (an engine image, a swapchain
  /// target): sampled and rendered like any other, never destroyed.
  /// `owner` keeps the backing alive for as long as the wrapper -- or a
  /// command buffer that referenced it -- lives: dart:ui can dispose an
  /// image the instant its last frame is submitted.
  GPUTextureVK(std::shared_ptr<VulkanCore> core,
               const TextureDesc& desc,
               vk::Image image,
               vk::ImageView view,
               vk::ImageLayout current_layout,
               std::shared_ptr<const void> owner = nullptr);

  ~GPUTextureVK() override;

  [[nodiscard]] vk::Image GetImage() const { return image_; }

  [[nodiscard]] vk::ImageView GetImageView() const { return raw_view_; }

  [[nodiscard]] const std::shared_ptr<const void>& GetOwner() const {
    return owner_;
  }

  /// Never reused, unlike raw Vulkan handles, so cache entries keyed on
  /// it cannot alias a new texture after this one dies.
  [[nodiscard]] uint64_t GetId() const { return id_; }

  /// The framebuffer for rendering into this texture alongside a canvas
  /// set, keyed by the canvases' ids. Shared: every pass that begins
  /// with it retains it in its command buffer, so replacement and
  /// texture death are fence-safe.
  struct CachedFramebuffer {
    uint64_t canvas_id = 0;
    uint64_t winding_id = 0;
    std::shared_ptr<vk::UniqueFramebuffer> framebuffer;
  };
  CachedFramebuffer cached_framebuffer;

  /// The layout the image was last left in. Command recording reads and
  /// writes this as it encodes transitions; nothing records concurrently.
  vk::ImageLayout layout = vk::ImageLayout::eUndefined;

 private:
  std::shared_ptr<VulkanCore> core_;
  vk::Image image_;
  VmaAllocation allocation_ = {};
  vk::UniqueImageView view_;
  vk::ImageView raw_view_;
  std::shared_ptr<const void> owner_;
  uint64_t id_;
};

class GPUBufferVK final : public GPUBuffer {
 public:
  GPUBufferVK(std::shared_ptr<VulkanCore> core,
              vk::Buffer buffer,
              VmaAllocation allocation,
              uint8_t* mapping);

  ~GPUBufferVK() override;

  // |GPUBuffer|
  void Write(const uint8_t* data, uint32_t length, uint32_t offset) override;

  // |GPUBuffer|
  uint8_t* Contents() override;

  [[nodiscard]] vk::Buffer GetBuffer() const { return buffer_; }

 private:
  std::shared_ptr<VulkanCore> core_;
  vk::Buffer buffer_;
  VmaAllocation allocation_;
  uint8_t* mapping_ = nullptr;
};

class GpuCommandBufferVK final : public GpuCommandBuffer {
 public:
  GpuCommandBufferVK(std::shared_ptr<VulkanCore> core,
                     vk::UniqueCommandBuffer command_buffer);

  ~GpuCommandBufferVK() override;

  // |GpuCommandBuffer|
  void UpdateRegion(const GPUTexture& texture,
                    uint32_t x,
                    uint32_t y,
                    uint32_t width,
                    uint32_t height,
                    const GPUBuffer& buffer,
                    uint32_t offset,
                    uint32_t row_bytes) override;

  /// Release the image to the compositor: GENERAL layout (what impeller
  /// leaves swapchain images in), external reads ordered by the
  /// present-ready fence rather than access masks.
  void PrepareForPresent(const GPUTexture& texture);

  /// Backend wiring for EncodeRuns, set once before encoding: the shared
  /// pipeline layout that push constants and descriptors target, and the
  /// frame's texture-table descriptor set, bound at set 1 as each pass
  /// starts. The neutral SetBuffer(kFragment, ..., 1) call is ignored;
  /// the table is a descriptor array here, not a buffer.
  /// `filler` backs any set-0 binding no arena supplied: the layout
  /// declares them all and the shaders reference them statically, and an
  /// unwritten storage-buffer descriptor is invalid even when no
  /// invocation dynamically reads it.
  void SetSharedState(vk::PipelineLayout layout,
                      vk::DescriptorSetLayout buffers_layout,
                      vk::DescriptorSetLayout inputs_layout,
                      vk::DescriptorSet table,
                      vk::Buffer filler);

  // |GpuCommandBuffer|
  void StartRenderPass(const RenderPassDesc& desc) override;

  // |GpuCommandBuffer|
  void EndRenderPass() override;

  // |GpuCommandBuffer|
  void SetRenderPipeline(const GPUProgram& program) override;

  // |GpuCommandBuffer|
  void SetScissorRect(int x, int y, int w, int h) override;

  // |GpuCommandBuffer|
  void SetBuffer(GPUShaderStage stage,
                 const GPUBuffer& buffer,
                 size_t offset,
                 int index) override;

  // |GpuCommandBuffer|
  void SetTextureTable(GPUTexture* const* textures, size_t count) override;

  // |GpuCommandBuffer|
  void SetConstantData(GPUShaderStage stage,
                       const void* data,
                       size_t bytes,
                       int index) override;

  // |GpuCommandBuffer|
  void DrawTriangles(int start, int count) override;

  // |GpuCommandBuffer|
  void DrawTrianglesIndexed(const GPUBuffer& indices,
                            int start,
                            int count) override;

  [[nodiscard]] vk::CommandBuffer GetCommandBuffer() const {
    return command_buffer_.get();
  }

 private:
  /// Allocate, write and bind the set-0 descriptors SetBuffer gathered.
  void FlushBindings();

  /// A descriptor set from the chained per-command-buffer pools.
  vk::DescriptorSet AllocateSet(vk::DescriptorSetLayout layout);

  /// Hold a borrowed texture's owner as long as this command buffer:
  /// the impeller tracked-objects discipline, minus the threads.
  void RetainTexture(const GPUTexture& texture);

  std::shared_ptr<VulkanCore> core_;
  vk::UniqueCommandBuffer command_buffer_;

  vk::PipelineLayout shared_layout_;
  vk::DescriptorSet table_set_;
  vk::DescriptorSetLayout buffers_set_layout_;
  vk::DescriptorSetLayout inputs_set_layout_;
  vk::Buffer filler_buffer_;
  /// Chained: a pass allocates one set, and a deep frame (backdrop
  /// redispatch multiplies passes) outgrows any fixed count.
  std::vector<vk::UniqueDescriptorPool> descriptor_pools_;
  /// Everything the recording referenced that outlives no one else --
  /// borrowed textures' owners, cached framebuffers -- alive until the
  /// command buffer retires.
  std::vector<std::shared_ptr<const void>> retained_;
  GPUTextureVK* pass_target_ = nullptr;
  /// Set-0 bindings gathered from SetBuffer, flushed at the first draw.
  struct PendingBinding {
    uint32_t binding;
    vk::Buffer buffer;
    vk::DeviceSize offset;
  };
  std::vector<PendingBinding> pending_bindings_;
  bool bindings_dirty_ = false;
};

//------------------------------------------------------------------------------
/// Owns the Vulkan device and allocator behind the neutral GPUContext
/// surface. Single-threaded, like its Metal counterpart.
class GPUContextVK final : public GPUContext {
 public:
  /// Load the Vulkan loader, create the instance, pick the first device
  /// with a graphics queue and stand up the allocator. Returns nullptr
  /// when any of that is unavailable.
  static std::shared_ptr<GPUContextVK> Make();

  /// Share an embedder's device instead of creating one: propeller's
  /// resources then live where the engine's images and render targets
  /// do. The vulkan-hpp dispatcher must already be initialized for these
  /// handles, the caller keeps them alive past the returned context, it
  /// must have enabled the bindless descriptor-indexing features to
  /// render, and it must not submit on `queue` from another thread while
  /// this context does.
  static std::shared_ptr<GPUContextVK> Adopt(vk::Instance instance,
                                             vk::PhysicalDevice physical,
                                             vk::Device device,
                                             uint32_t queue_family,
                                             vk::Queue queue);

  explicit GPUContextVK(std::shared_ptr<VulkanCore> core);

  ~GPUContextVK() override;

  // |GPUContext|
  std::unique_ptr<GPUTexture> CreateTexture(const TextureDesc& desc,
                                            bool zeroed) override;

  // |GPUContext|
  std::unique_ptr<GPUBuffer> CreateBuffer(const uint8_t* bytes,
                                          size_t size) override;

  /// A per-pass scratch attachment (virtual canvas, winding
  /// accumulator): color + subpass input only, lazily allocated where
  /// the device offers it (tile memory on mobile GPUs).
  std::unique_ptr<GPUTexture> CreateTransientTexture(TextureFormat format,
                                                     uint32_t width,
                                                     uint32_t height);

  // |GPUContext|
  size_t GetBufferAlignment() const override {
    return core_->buffer_alignment;
  }

  /// Begin recording a one-shot command buffer.
  std::unique_ptr<GpuCommandBufferVK> CreateCommandBuffer();

  /// End and submit `command_buffer`, signalling `fence` when it
  /// retires. The caller keeps the command buffer alive until then.
  /// `wait` gates the color-attachment stage (the swapchain acquire
  /// handshake); `signal` fires when the submission completes.
  bool Submit(GpuCommandBufferVK& command_buffer,
              vk::Fence fence,
              vk::Semaphore wait = nullptr,
              vk::Semaphore signal = nullptr);

  /// End, submit and wait for `command_buffer`'s own fence -- scoped to
  /// this submission, not the whole queue.
  bool SubmitAndWait(GpuCommandBufferVK& command_buffer);

  /// End and submit `command_buffer`, taking ownership: it is parked
  /// with a fence and freed by ReapRetired only once the fence proves
  /// the GPU is done with it -- the impeller FenceWaiter discipline,
  /// polled at frame boundaries instead of on a thread.
  bool SubmitAndRetire(std::unique_ptr<GpuCommandBufferVK> command_buffer);

  /// Free retired submissions whose fences have signalled. Cheap;
  /// callers run it once per frame.
  void ReapRetired();

  /// Copy the whole of `texture` into host memory, tightly packed. For
  /// tests and debugging; stalls the queue.
  std::vector<uint8_t> ReadbackTexture(GPUTexture& texture);

  [[nodiscard]] const std::shared_ptr<VulkanCore>& GetCore() const {
    return core_;
  }

 private:
  struct RetiredSubmission {
    vk::UniqueFence fence;
    std::unique_ptr<GpuCommandBufferVK> commands;
  };

  std::shared_ptr<VulkanCore> core_;
  std::vector<RetiredSubmission> retired_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_CONTEXT_H_
