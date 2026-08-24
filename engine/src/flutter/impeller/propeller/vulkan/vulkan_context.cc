// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/vulkan/vulkan_context.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

#include "flutter/fml/build_config.h"
#include "flutter/fml/logging.h"
#include "impeller/propeller/vulkan/vulkan_pipelines.h"

namespace impeller {

namespace {

constexpr const char* kLoaderLibraryName =
#if FML_OS_MACOSX || FML_OS_IOS
    "libvulkan.dylib";
#elif FML_OS_WIN
    "vulkan-1.dll";
#else
    "libvulkan.so";
#endif

/// Canvas attachment formats; index 0 is the target's own.
vk::Format CanvasAttachmentFormat(size_t index) {
  return index == kMLRWindingCanvas ? vk::Format::eR16Sfloat
                                    : vk::Format::eR8G8B8A8Unorm;
}

/// What touches an image while it sits in a layout: the stages that do,
/// every access they make, and the subset that writes. A barrier's
/// source only flushes writes; its destination must see every access.
struct LayoutScope {
  vk::PipelineStageFlags stages;
  vk::AccessFlags access;
  vk::AccessFlags writes;
};

LayoutScope ScopeFor(vk::ImageLayout layout) {
  switch (layout) {
    case vk::ImageLayout::eUndefined:
      return {vk::PipelineStageFlagBits::eTopOfPipe, {}, {}};
    case vk::ImageLayout::eTransferDstOptimal:
      return {vk::PipelineStageFlagBits::eTransfer,
              vk::AccessFlagBits::eTransferWrite,
              vk::AccessFlagBits::eTransferWrite};
    case vk::ImageLayout::eTransferSrcOptimal:
      return {vk::PipelineStageFlagBits::eTransfer,
              vk::AccessFlagBits::eTransferRead,
              {}};
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      // Propeller samples in the fragment stage only; vertex work pulls
      // from storage buffers.
      return {vk::PipelineStageFlagBits::eFragmentShader,
              vk::AccessFlagBits::eShaderRead,
              {}};
    case vk::ImageLayout::eColorAttachmentOptimal:
      return {vk::PipelineStageFlagBits::eColorAttachmentOutput,
              vk::AccessFlagBits::eColorAttachmentRead |
                  vk::AccessFlagBits::eColorAttachmentWrite,
              vk::AccessFlagBits::eColorAttachmentWrite};
    case vk::ImageLayout::ePresentSrcKHR:
    case vk::ImageLayout::eGeneral:
      // Compositor handoff layouts (KHR present, hardware-buffer
      // GENERAL). The stage is the acquire-wait stage, not top-of-pipe:
      // the transition out of the handoff is a write, and it must chain
      // behind the acquire semaphore's wait or it races the compositor's
      // reads of the buffer.
      return {vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}};
    default:
      FML_UNREACHABLE();
  }
}

void Transition(vk::CommandBuffer commands,
                GPUTextureVK& texture,
                vk::ImageLayout to) {
  if (texture.layout == to) {
    return;
  }
  const LayoutScope src = ScopeFor(texture.layout);
  const LayoutScope dst = ScopeFor(to);
  vk::ImageMemoryBarrier barrier;
  barrier.srcAccessMask = src.writes;
  barrier.dstAccessMask = dst.access;
  barrier.oldLayout = texture.layout;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = texture.GetImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  commands.pipelineBarrier(src.stages, dst.stages, {}, nullptr, nullptr,
                           barrier);
  texture.layout = to;
}

}  // namespace

VulkanCore::AllocatorHolder::~AllocatorHolder() {
  if (allocator != nullptr) {
    ::vmaDestroyAllocator(allocator);
  }
}

VulkanCore::~VulkanCore() {
  if (!owns_device) {
    // The members destroyed after this body (pool, render passes, the
    // allocator) still use the adopted device, which the adopter keeps
    // alive; only ownership is handed back.
    (void)device.release();
    (void)instance.release();
  }
}

vk::RenderPass VulkanCore::GetRenderPass(vk::Format format, bool load) {
  const uint64_t key = (static_cast<uint64_t>(format) << 1) | (load ? 1 : 0);
  vk::UniqueRenderPass& cached = render_passes[key];
  if (cached) {
    return cached.get();
  }
  // Attachment 0 is the target; 1 is the RGBA8 virtual canvas; 2 the
  // R16Float winding accumulator. Every attachment doubles as a subpass
  // input (framebuffer fetch), which requires the GENERAL layout while
  // the subpass runs.
  vk::AttachmentDescription attachments[3];
  attachments[0].format = format;
  attachments[0].loadOp =
      load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear;
  attachments[0].storeOp = vk::AttachmentStoreOp::eStore;
  attachments[0].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
  attachments[0].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
  // The caller transitions the target explicitly before the pass (a
  // recycled texture's prior reads and writes are ordered by that
  // barrier, with its real tracked layout), so entry is not a discard.
  attachments[0].initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
  attachments[0].finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  for (size_t i = 1; i < 3; i++) {
    attachments[i].format = CanvasAttachmentFormat(i);
    attachments[i].loadOp = vk::AttachmentLoadOp::eClear;
    attachments[i].storeOp = vk::AttachmentStoreOp::eDontCare;
    attachments[i].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    attachments[i].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    attachments[i].initialLayout = vk::ImageLayout::eUndefined;
    attachments[i].finalLayout = vk::ImageLayout::eGeneral;
  }

  vk::AttachmentReference color_refs[3];
  vk::AttachmentReference input_refs[3];
  for (uint32_t i = 0; i < 3; i++) {
    color_refs[i].attachment = i;
    color_refs[i].layout = vk::ImageLayout::eGeneral;
    input_refs[i].attachment = i;
    input_refs[i].layout = vk::ImageLayout::eGeneral;
  }
  vk::SubpassDescription subpass;
  subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
  subpass.colorAttachmentCount = 3;
  subpass.pColorAttachments = color_refs;
  subpass.inputAttachmentCount = 3;
  subpass.pInputAttachments = input_refs;

  vk::SubpassDependency dependencies[3];
  // A recycled pass texture was sampled last frame and a loaded target
  // was rendered before: order both against this pass's output, and for
  // load make the earlier attachment writes visible to loadOp's read.
  // Prior sampling is a read; it needs ordering, not flushing.
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask =
      vk::PipelineStageFlagBits::eFragmentShader |
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  dependencies[0].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
  dependencies[0].dstStageMask =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  dependencies[0].dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                                  vk::AccessFlagBits::eColorAttachmentWrite;
  // The pass's writes feed later fragment sampling and, for readback and
  // pass-texture copies, transfer reads.
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  dependencies[1].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
  dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader |
                                 vk::PipelineStageFlagBits::eTransfer;
  dependencies[1].dstAccessMask =
      vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead;
  // The self-dependency licenses the per-fetch by-region barrier: a
  // composite reads what earlier draws wrote at its own pixel.
  dependencies[2].srcSubpass = 0;
  dependencies[2].dstSubpass = 0;
  dependencies[2].srcStageMask =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  dependencies[2].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
  dependencies[2].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
  dependencies[2].dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead;
  dependencies[2].dependencyFlags = vk::DependencyFlagBits::eByRegion;

  vk::RenderPassCreateInfo info;
  info.attachmentCount = 3;
  info.pAttachments = attachments;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  info.dependencyCount = 3;
  info.pDependencies = dependencies;
  auto [result, render_pass] = device->createRenderPassUnique(info);
  FML_CHECK(result == vk::Result::eSuccess);
  cached = std::move(render_pass);
  return cached.get();
}

// Texture

namespace {
std::atomic<uint64_t> s_next_texture_id{1};
}  // namespace

GPUTextureVK::GPUTextureVK(std::shared_ptr<VulkanCore> core,
                           const TextureDesc& desc,
                           vk::Image image,
                           VmaAllocation allocation,
                           vk::UniqueImageView view)
    : GPUTexture(desc),
      core_(std::move(core)),
      image_(image),
      allocation_(allocation),
      view_(std::move(view)),
      raw_view_(view_.get()),
      id_(s_next_texture_id++) {}

GPUTextureVK::GPUTextureVK(std::shared_ptr<VulkanCore> core,
                           const TextureDesc& desc,
                           vk::Image image,
                           vk::ImageView view,
                           vk::ImageLayout current_layout,
                           std::shared_ptr<const void> owner)
    : GPUTexture(desc),
      core_(std::move(core)),
      image_(image),
      raw_view_(view),
      owner_(std::move(owner)),
      id_(s_next_texture_id++) {
  layout = current_layout;
}

GPUTextureVK::~GPUTextureVK() {
  view_.reset();
  if (allocation_ != nullptr) {
    ::vmaDestroyImage(core_->vma.allocator, image_, allocation_);
  }
}

// Buffer

GPUBufferVK::GPUBufferVK(std::shared_ptr<VulkanCore> core,
                         vk::Buffer buffer,
                         VmaAllocation allocation,
                         uint8_t* mapping)
    : core_(std::move(core)),
      buffer_(buffer),
      allocation_(allocation),
      mapping_(mapping) {}

GPUBufferVK::~GPUBufferVK() {
  ::vmaDestroyBuffer(core_->vma.allocator, buffer_, allocation_);
}

void GPUBufferVK::Write(const uint8_t* data, uint32_t length, uint32_t offset) {
  std::memcpy(mapping_ + offset, data, length);
}

uint8_t* GPUBufferVK::Contents() {
  return mapping_;
}

// Command buffer

GpuCommandBufferVK::GpuCommandBufferVK(std::shared_ptr<VulkanCore> core,
                                       vk::UniqueCommandBuffer command_buffer)
    : core_(std::move(core)), command_buffer_(std::move(command_buffer)) {}

GpuCommandBufferVK::~GpuCommandBufferVK() = default;

void GpuCommandBufferVK::RetainTexture(const GPUTexture& texture) {
  const std::shared_ptr<const void>& owner =
      static_cast<const GPUTextureVK&>(texture).GetOwner();
  if (owner) {
    retained_.push_back(owner);
  }
}

void GpuCommandBufferVK::PrepareForPresent(const GPUTexture& texture) {
  auto& target =
      const_cast<GPUTextureVK&>(static_cast<const GPUTextureVK&>(texture));
  RetainTexture(target);
  Transition(command_buffer_.get(), target, vk::ImageLayout::eGeneral);
}

void GpuCommandBufferVK::UpdateRegion(const GPUTexture& texture,
                                      uint32_t x,
                                      uint32_t y,
                                      uint32_t width,
                                      uint32_t height,
                                      const GPUBuffer& buffer,
                                      uint32_t offset,
                                      uint32_t row_bytes) {
  auto& destination =
      const_cast<GPUTextureVK&>(static_cast<const GPUTextureVK&>(texture));
  RetainTexture(destination);
  Transition(command_buffer_.get(), destination,
             vk::ImageLayout::eTransferDstOptimal);

  vk::BufferImageCopy copy;
  copy.bufferOffset = offset;
  // In texels, unlike Metal's bytes-per-row.
  copy.bufferRowLength =
      row_bytes /
      static_cast<uint32_t>(BytesPerPixel(texture.GetDesc().format));
  copy.bufferImageHeight = height;
  copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  copy.imageSubresource.layerCount = 1;
  copy.imageOffset =
      vk::Offset3D{static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
  copy.imageExtent = vk::Extent3D{width, height, 1};
  command_buffer_->copyBufferToImage(
      static_cast<const GPUBufferVK&>(buffer).GetBuffer(),
      destination.GetImage(), vk::ImageLayout::eTransferDstOptimal, copy);

  Transition(command_buffer_.get(), destination,
             vk::ImageLayout::eShaderReadOnlyOptimal);
}

void GpuCommandBufferVK::SetSharedState(vk::PipelineLayout layout,
                                        vk::DescriptorSetLayout buffers_layout,
                                        vk::DescriptorSetLayout inputs_layout,
                                        vk::DescriptorSet table,
                                        vk::Buffer filler) {
  shared_layout_ = layout;
  buffers_set_layout_ = buffers_layout;
  inputs_set_layout_ = inputs_layout;
  table_set_ = table;
  filler_buffer_ = filler;
}

vk::DescriptorSet GpuCommandBufferVK::AllocateSet(
    vk::DescriptorSetLayout layout) {
  auto add_pool = [&]() {
    // One buffers set and one inputs set per pass; the immutable
    // samplers still count against the pool.
    vk::DescriptorPoolSize sizes[3];
    sizes[0].type = vk::DescriptorType::eStorageBuffer;
    sizes[0].descriptorCount = 64 * 6;
    sizes[1].type = vk::DescriptorType::eSampler;
    sizes[1].descriptorCount = 64 * 2;
    sizes[2].type = vk::DescriptorType::eInputAttachment;
    sizes[2].descriptorCount = 64 * 3;
    vk::DescriptorPoolCreateInfo pool_info;
    pool_info.maxSets = 128;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = sizes;
    auto [result, pool] = core_->device->createDescriptorPoolUnique(pool_info);
    FML_CHECK(result == vk::Result::eSuccess);
    descriptor_pools_.push_back(std::move(pool));
  };
  if (descriptor_pools_.empty()) {
    add_pool();
  }
  vk::DescriptorSetAllocateInfo allocate_info;
  allocate_info.descriptorPool = descriptor_pools_.back().get();
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &layout;
  auto [result, sets] = core_->device->allocateDescriptorSets(allocate_info);
  if (result == vk::Result::eErrorOutOfPoolMemory ||
      result == vk::Result::eErrorFragmentedPool) {
    // A frame deeper than one pool; chain another and carry on.
    add_pool();
    allocate_info.descriptorPool = descriptor_pools_.back().get();
    std::tie(result, sets) =
        core_->device->allocateDescriptorSets(allocate_info);
  }
  FML_CHECK(result == vk::Result::eSuccess) << vk::to_string(result);
  return sets[0];
}

void GpuCommandBufferVK::StartRenderPass(const RenderPassDesc& desc) {
  auto* target =
      static_cast<GPUTextureVK*>(const_cast<GPUTexture*>(desc.target));
  RetainTexture(*target);
  // Mirror impeller's discipline: every render target gets an explicit
  // transition from its tracked layout, so a recycled texture's prior
  // sampling and a borrowed swapchain image's presentation are ordered
  // per resource rather than left to the pass's implicit transition.
  Transition(command_buffer_.get(), *target,
             vk::ImageLayout::eColorAttachmentOptimal);
  const TextureDesc& target_desc = target->GetDesc();
  const vk::RenderPass render_pass =
      core_->GetRenderPass(MLRVkFormat(target_desc.format), desc.load);

  FML_DCHECK(desc.transient_count == 2)
      << "canvas mode: every pass carries the canvas and the accumulator";
  const auto* canvas = static_cast<const GPUTextureVK*>(desc.transients[0]);
  const auto* winding = static_cast<const GPUTextureVK*>(desc.transients[1]);
  const vk::ImageView views[3] = {
      target->GetImageView(),
      canvas->GetImageView(),
      winding->GetImageView(),
  };
  // Load and clear render passes are compatible (compatibility ignores
  // load/store ops), so one framebuffer serves both.
  GPUTextureVK::CachedFramebuffer& cached = target->cached_framebuffer;
  if (cached.framebuffer == nullptr || cached.canvas_id != canvas->GetId() ||
      cached.winding_id != winding->GetId()) {
    vk::FramebufferCreateInfo framebuffer_info;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 3;
    framebuffer_info.pAttachments = views;
    framebuffer_info.width = target_desc.width;
    framebuffer_info.height = target_desc.height;
    framebuffer_info.layers = 1;
    auto [framebuffer_result, framebuffer] =
        core_->device->createFramebufferUnique(framebuffer_info);
    FML_CHECK(framebuffer_result == vk::Result::eSuccess);
    cached = {canvas->GetId(), winding->GetId(),
              std::make_shared<vk::UniqueFramebuffer>(std::move(framebuffer))};
  }
  retained_.push_back(cached.framebuffer);

  vk::ClearValue clears[3];
  clears[0].color =
      vk::ClearColorValue{desc.clear_color[0], desc.clear_color[1],
                          desc.clear_color[2], desc.clear_color[3]};
  clears[1].color = vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f};
  clears[2].color = vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f};
  vk::RenderPassBeginInfo begin_info;
  begin_info.renderPass = render_pass;
  begin_info.framebuffer = target->cached_framebuffer.framebuffer->get();
  begin_info.renderArea =
      vk::Rect2D{{0, 0}, {target_desc.width, target_desc.height}};
  begin_info.clearValueCount = 3;
  begin_info.pClearValues = clears;
  command_buffer_->beginRenderPass(begin_info, vk::SubpassContents::eInline);
  pass_target_ = target;

  // The pass's attachments as subpass inputs, set 2. A pass that only
  // clears (no shared state) binds nothing.
  if (inputs_set_layout_) {
    const vk::DescriptorSet inputs_set = AllocateSet(inputs_set_layout_);
    vk::DescriptorImageInfo input_infos[3];
    vk::WriteDescriptorSet input_writes[3];
    for (uint32_t i = 0; i < 3; i++) {
      input_infos[i].imageView = views[i];
      input_infos[i].imageLayout = vk::ImageLayout::eGeneral;
      input_writes[i].dstSet = inputs_set;
      input_writes[i].dstBinding = i;
      input_writes[i].descriptorCount = 1;
      input_writes[i].descriptorType = vk::DescriptorType::eInputAttachment;
      input_writes[i].pImageInfo = &input_infos[i];
    }
    core_->device->updateDescriptorSets(3, input_writes, 0, nullptr);
    command_buffer_->bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                        shared_layout_, 2, inputs_set, nullptr);
  }

  // Negative height flips NDC to the Metal orientation the vertex shader
  // emits.
  vk::Viewport viewport;
  viewport.y = static_cast<float>(target_desc.height);
  viewport.width = static_cast<float>(target_desc.width);
  viewport.height = -static_cast<float>(target_desc.height);
  viewport.maxDepth = 1.0f;
  command_buffer_->setViewport(0, viewport);
  command_buffer_->setScissor(
      0, vk::Rect2D{{0, 0}, {target_desc.width, target_desc.height}});
  if (table_set_) {
    command_buffer_->bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                        shared_layout_, 1, table_set_, nullptr);
  }
}

void GpuCommandBufferVK::EndRenderPass() {
  command_buffer_->endRenderPass();
  pass_target_->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
  pass_target_ = nullptr;
}

void GpuCommandBufferVK::SetRenderPipeline(const GPUProgram& program) {
  const auto& program_vk = static_cast<const GPUProgramVK&>(program);
  // if (program_vk.Fetches()) {
  //   // The fetch reads what earlier draws wrote at this pixel; the
  //   // render pass's self-dependency licenses this by-region barrier.
  //   // Rasterization-order hardware can skip it -- a later optimization.
  //   vk::MemoryBarrier barrier;
  //   barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
  //   barrier.dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead;
  //   command_buffer_->pipelineBarrier(
  //       vk::PipelineStageFlagBits::eColorAttachmentOutput,
  //       vk::PipelineStageFlagBits::eFragmentShader,
  //       vk::DependencyFlagBits::eByRegion, barrier, nullptr, nullptr);
  // }
  command_buffer_->bindPipeline(vk::PipelineBindPoint::eGraphics,
                                program_vk.GetPipeline());
}

void GpuCommandBufferVK::SetScissorRect(int x, int y, int w, int h) {
  command_buffer_->setScissor(
      0,
      vk::Rect2D{{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}});
}

namespace {

/// Metal binding index -> set-0 GLSL binding, per stage. -1 drops the
/// call (the fragment "texture table" buffer, which is set 1 here).
int32_t ToSetZeroBinding(GPUShaderStage stage, int index) {
  if (stage == GPUShaderStage::kVertex) {
    return index;  // positions 0, attributes 1, paints 2, transforms 4.
  }
  switch (index) {
    case 0:
      return 2;  // Paints.
    case 1:
      return -1;  // The texture table.
    case 2:
      return 5;  // Gradients.
    case 4:
      return 6;  // Clip rects.
  }
  FML_UNREACHABLE();
}

/// Metal SetConstantData index -> byte offset in the shared push block.
uint32_t ToPushOffset(GPUShaderStage stage, int index) {
  if (stage == GPUShaderStage::kVertex) {
    FML_DCHECK(index == 3);
    return 0;  // viewport_page.
  }
  switch (index) {
    case 3:
      return 16;  // opacity.
    case 5:
      return 20;  // source.
    case 6:
      return 32;  // The filter blob.
  }
  FML_UNREACHABLE();
}

}  // namespace

void GpuCommandBufferVK::SetBuffer(GPUShaderStage stage,
                                   const GPUBuffer& buffer,
                                   size_t offset,
                                   int index) {
  const int32_t binding = ToSetZeroBinding(stage, index);
  if (binding < 0) {
    return;
  }
  const vk::Buffer handle = static_cast<const GPUBufferVK&>(buffer).GetBuffer();
  for (PendingBinding& pending : pending_bindings_) {
    if (pending.binding == static_cast<uint32_t>(binding)) {
      if (pending.buffer != handle || pending.offset != offset) {
        pending.buffer = handle;
        pending.offset = offset;
        bindings_dirty_ = true;
      }
      return;
    }
  }
  pending_bindings_.push_back({static_cast<uint32_t>(binding), handle, offset});
  bindings_dirty_ = true;
}

void GpuCommandBufferVK::SetTextureTable(GPUTexture* const* textures,
                                         size_t count) {
  // The table is a descriptor array here rather than a buffer, so the
  // slots are written into the set the shared state bound.
  std::vector<vk::DescriptorImageInfo> infos;
  std::vector<vk::WriteDescriptorSet> writes;
  infos.reserve(count);
  writes.reserve(count);
  for (size_t i = 0; i < count; i++) {
    if (textures[i] == nullptr) {
      continue;
    }
    vk::DescriptorImageInfo info;
    info.imageView = static_cast<GPUTextureVK*>(textures[i])->GetImageView();
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    infos.push_back(info);

    vk::WriteDescriptorSet write;
    write.dstSet = table_set_;
    write.dstBinding = 0;
    write.dstArrayElement = static_cast<uint32_t>(i);
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eSampledImage;
    writes.push_back(write);
  }
  for (size_t i = 0; i < writes.size(); i++) {
    writes[i].pImageInfo = &infos[i];
  }
  if (!writes.empty()) {
    core_->device->updateDescriptorSets(writes, nullptr);
  }
}

void GpuCommandBufferVK::SetConstantData(GPUShaderStage stage,
                                         const void* data,
                                         size_t bytes,
                                         int index) {
  command_buffer_->pushConstants(
      shared_layout_,
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
      ToPushOffset(stage, index), bytes, data);
}

void GpuCommandBufferVK::FlushBindings() {
  if (!bindings_dirty_) {
    return;
  }
  bindings_dirty_ = false;
  const vk::DescriptorSet set = AllocateSet(buffers_set_layout_);

  // Every storage binding in the layout, the filler behind absent ones.
  constexpr uint32_t kStorageBindings[6] = {0, 1, 2, 4, 5, 6};
  std::vector<vk::DescriptorBufferInfo> infos(std::size(kStorageBindings));
  std::vector<vk::WriteDescriptorSet> writes(std::size(kStorageBindings));
  for (size_t i = 0; i < std::size(kStorageBindings); i++) {
    infos[i].buffer = filler_buffer_;
    infos[i].offset = 0;
    infos[i].range = VK_WHOLE_SIZE;
    for (const PendingBinding& pending : pending_bindings_) {
      if (pending.binding == kStorageBindings[i]) {
        infos[i].buffer = pending.buffer;
        infos[i].offset = pending.offset;
        break;
      }
    }
    writes[i].dstSet = set;
    writes[i].dstBinding = kStorageBindings[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[i].pBufferInfo = &infos[i];
  }
  core_->device->updateDescriptorSets(writes, nullptr);
  command_buffer_->bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      shared_layout_, 0, set, nullptr);
}

void GpuCommandBufferVK::DrawTriangles(int start, int count) {
  FlushBindings();
  command_buffer_->draw(count, 1, start, 0);
}

void GpuCommandBufferVK::DrawTrianglesIndexed(const GPUBuffer& indices,
                                              int start,
                                              int count) {
  FlushBindings();
  command_buffer_->bindIndexBuffer(
      static_cast<const GPUBufferVK&>(indices).GetBuffer(), 0,
      vk::IndexType::eUint16);
  command_buffer_->drawIndexed(count, 1, start, 0, 0);
}

// Context

namespace {

/// The parts shared by Make and Adopt: the allocator and command pool.
std::shared_ptr<GPUContextVK> FinishSetup(std::shared_ptr<VulkanCore> core) {
  core->buffer_alignment =
      std::max<size_t>(16, core->physical_device.getProperties()
                               .limits.minStorageBufferOffsetAlignment);
  VmaVulkanFunctions proc_table = {};
#define BIND_VMA_PROC(x) proc_table.x = VULKAN_HPP_DEFAULT_DISPATCHER.x;
#define BIND_VMA_PROC_KHR(x)                                \
  proc_table.x##KHR = VULKAN_HPP_DEFAULT_DISPATCHER.x       \
                          ? VULKAN_HPP_DEFAULT_DISPATCHER.x \
                          : VULKAN_HPP_DEFAULT_DISPATCHER.x##KHR;
  BIND_VMA_PROC(vkGetInstanceProcAddr);
  BIND_VMA_PROC(vkGetDeviceProcAddr);
  BIND_VMA_PROC(vkGetPhysicalDeviceProperties);
  BIND_VMA_PROC(vkGetPhysicalDeviceMemoryProperties);
  BIND_VMA_PROC(vkAllocateMemory);
  BIND_VMA_PROC(vkFreeMemory);
  BIND_VMA_PROC(vkMapMemory);
  BIND_VMA_PROC(vkUnmapMemory);
  BIND_VMA_PROC(vkFlushMappedMemoryRanges);
  BIND_VMA_PROC(vkInvalidateMappedMemoryRanges);
  BIND_VMA_PROC(vkBindBufferMemory);
  BIND_VMA_PROC(vkBindImageMemory);
  BIND_VMA_PROC(vkGetBufferMemoryRequirements);
  BIND_VMA_PROC(vkGetImageMemoryRequirements);
  BIND_VMA_PROC(vkCreateBuffer);
  BIND_VMA_PROC(vkDestroyBuffer);
  BIND_VMA_PROC(vkCreateImage);
  BIND_VMA_PROC(vkDestroyImage);
  BIND_VMA_PROC(vkCmdCopyBuffer);
  BIND_VMA_PROC_KHR(vkGetBufferMemoryRequirements2);
  BIND_VMA_PROC_KHR(vkGetImageMemoryRequirements2);
  BIND_VMA_PROC_KHR(vkBindBufferMemory2);
  BIND_VMA_PROC_KHR(vkBindImageMemory2);
  BIND_VMA_PROC_KHR(vkGetPhysicalDeviceMemoryProperties2);
#undef BIND_VMA_PROC_KHR
#undef BIND_VMA_PROC

  VmaAllocatorCreateInfo allocator_info = {};
  allocator_info.vulkanApiVersion = VK_API_VERSION_1_2;
  allocator_info.physicalDevice = core->physical_device;
  allocator_info.device = core->device.get();
  allocator_info.instance = core->instance.get();
  allocator_info.pVulkanFunctions = &proc_table;
  if (vk::Result{::vmaCreateAllocator(&allocator_info, &core->vma.allocator)} !=
      vk::Result::eSuccess) {
    FML_LOG(ERROR) << "Could not create the Vulkan memory allocator.";
    return nullptr;
  }

  vk::CommandPoolCreateInfo pool_info;
  pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient |
                    vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  pool_info.queueFamilyIndex = core->queue_family;
  auto [pool_result, pool] = core->device->createCommandPoolUnique(pool_info);
  if (pool_result != vk::Result::eSuccess) {
    return nullptr;
  }
  core->command_pool = std::move(pool);

  return std::make_shared<GPUContextVK>(std::move(core));
}

}  // namespace

std::shared_ptr<GPUContextVK> GPUContextVK::Make() {
  auto core = std::make_shared<VulkanCore>();

  core->loader_library = fml::NativeLibrary::Create(kLoaderLibraryName);
  if (!core->loader_library) {
    FML_LOG(ERROR) << "Could not open " << kLoaderLibraryName << ".";
    return nullptr;
  }
  auto instance_proc =
      core->loader_library->ResolveFunction<PFN_vkGetInstanceProcAddr>(
          "vkGetInstanceProcAddr");
  if (!instance_proc.has_value()) {
    FML_LOG(ERROR) << "Loader has no vkGetInstanceProcAddr.";
    return nullptr;
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(instance_proc.value());

  vk::ApplicationInfo application_info;
  application_info.pApplicationName = "propeller";
  application_info.apiVersion = VK_API_VERSION_1_2;

  vk::InstanceCreateInfo instance_info;
  instance_info.pApplicationInfo = &application_info;
  // MoltenVK reports itself as a portability driver and the loader hides
  // it unless enumeration is asked for; drivers without the extension
  // reject it.
  std::vector<const char*> instance_extensions;
  auto [extensions_result, available_extensions] =
      vk::enumerateInstanceExtensionProperties();
  if (extensions_result == vk::Result::eSuccess) {
    for (const auto& extension : available_extensions) {
      if (std::strcmp(extension.extensionName,
                      VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
        instance_extensions.push_back(
            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instance_info.flags |=
            vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
      }
    }
  }
  instance_info.setPEnabledExtensionNames(instance_extensions);

  auto [instance_result, instance] = vk::createInstanceUnique(instance_info);
  if (instance_result != vk::Result::eSuccess) {
    FML_LOG(ERROR) << "Could not create Vulkan instance: "
                   << vk::to_string(instance_result);
    return nullptr;
  }
  core->instance = std::move(instance);
  VULKAN_HPP_DEFAULT_DISPATCHER.init(core->instance.get());

  auto [devices_result, devices] = core->instance->enumeratePhysicalDevices();
  if (devices_result != vk::Result::eSuccess || devices.empty()) {
    FML_LOG(ERROR) << "No Vulkan devices.";
    return nullptr;
  }
  bool found = false;
  for (const vk::PhysicalDevice& candidate : devices) {
    const auto families = candidate.getQueueFamilyProperties();
    for (uint32_t i = 0; i < families.size(); i++) {
      if (families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
        core->physical_device = candidate;
        core->queue_family = i;
        found = true;
        break;
      }
    }
    if (found) {
      break;
    }
  }
  if (!found) {
    FML_LOG(ERROR) << "No Vulkan device has a graphics queue.";
    return nullptr;
  }

  const float priority = 1.0f;
  vk::DeviceQueueCreateInfo queue_info;
  queue_info.queueFamilyIndex = core->queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  // The bindless texture table: a runtime-sized, partially bound sampled
  // image array indexed nonuniformly, updated after bind. All optional
  // 1.2 features; a device without them gets no propeller.
  vk::PhysicalDeviceVulkan12Features available12;
  vk::PhysicalDeviceFeatures2 available;
  available.pNext = &available12;
  core->physical_device.getFeatures2(&available);
  core->bindless_supported =
      available12.runtimeDescriptorArray &&
      available12.descriptorBindingPartiallyBound &&
      available12.shaderSampledImageArrayNonUniformIndexing &&
      available12.descriptorBindingSampledImageUpdateAfterBind &&
      available.features.independentBlend;

  vk::PhysicalDeviceVulkan12Features enabled12;
  vk::PhysicalDeviceFeatures2 enabled;
  enabled.pNext = &enabled12;
  if (core->bindless_supported) {
    enabled12.runtimeDescriptorArray = true;
    enabled12.descriptorBindingPartiallyBound = true;
    enabled12.shaderSampledImageArrayNonUniformIndexing = true;
    enabled12.descriptorBindingSampledImageUpdateAfterBind = true;
    // The canvas pipelines blend each attachment differently.
    enabled.features.independentBlend = true;
  }

  vk::DeviceCreateInfo device_info;
  device_info.pNext = &enabled;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  auto [device_result, device] =
      core->physical_device.createDeviceUnique(device_info);
  if (device_result != vk::Result::eSuccess) {
    FML_LOG(ERROR) << "Could not create Vulkan device: "
                   << vk::to_string(device_result);
    return nullptr;
  }
  core->device = std::move(device);
  VULKAN_HPP_DEFAULT_DISPATCHER.init(core->device.get());
  core->queue = core->device->getQueue(core->queue_family, 0);

  return FinishSetup(std::move(core));
}

std::shared_ptr<GPUContextVK> GPUContextVK::Adopt(vk::Instance instance,
                                                  vk::PhysicalDevice physical,
                                                  vk::Device device,
                                                  uint32_t queue_family,
                                                  vk::Queue queue) {
  auto core = std::make_shared<VulkanCore>();
  core->owns_device = false;
  core->instance = vk::UniqueInstance(instance);
  core->physical_device = physical;
  core->device = vk::UniqueDevice(device);
  core->queue_family = queue_family;
  core->queue = queue;
  // Physical support is what can be queried after the fact; the adopter
  // is responsible for having enabled these on the device. The
  // descriptor-indexing struct rather than Vulkan12Features: adopted
  // instances (impeller's) are 1.1 with the EXT extension.
  vk::PhysicalDeviceDescriptorIndexingFeatures indexing;
  vk::PhysicalDeviceFeatures2 available;
  available.pNext = &indexing;
  physical.getFeatures2(&available);
  core->bindless_supported =
      indexing.runtimeDescriptorArray &&
      indexing.descriptorBindingPartiallyBound &&
      indexing.shaderSampledImageArrayNonUniformIndexing &&
      indexing.descriptorBindingSampledImageUpdateAfterBind &&
      available.features.independentBlend;
  return FinishSetup(std::move(core));
}

GPUContextVK::GPUContextVK(std::shared_ptr<VulkanCore> core)
    : core_(std::move(core)) {}

GPUContextVK::~GPUContextVK() {
  // Retired submissions may still be executing; their fences are the
  // proof of completion, exactly as with the frame ring.
  for (RetiredSubmission& retired : retired_) {
    (void)core_->device->waitForFences(retired.fence.get(), VK_TRUE,
                                       std::numeric_limits<uint64_t>::max());
  }
}

std::unique_ptr<GPUTexture> GPUContextVK::CreateTexture(const TextureDesc& desc,
                                                        bool zeroed) {
  vk::ImageCreateInfo image_info;
  image_info.imageType = vk::ImageType::e2D;
  image_info.format = MLRVkFormat(desc.format);
  image_info.extent = vk::Extent3D{desc.width, desc.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  // Input attachment: every render target is also fetchable as in_d0.
  image_info.usage = vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eInputAttachment |
                     vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eTransferSrc;
  const auto image_info_native =
      static_cast<vk::ImageCreateInfo::NativeType>(image_info);

  VmaAllocationCreateInfo allocation_info = {};
  allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
  allocation_info.preferredFlags = static_cast<VkMemoryPropertyFlags>(
      vk::MemoryPropertyFlagBits::eDeviceLocal);

  VkImage image = {};
  VmaAllocation allocation = {};
  if (vk::Result{::vmaCreateImage(core_->vma.allocator, &image_info_native,
                                  &allocation_info, &image, &allocation,
                                  nullptr)} != vk::Result::eSuccess) {
    return nullptr;
  }

  vk::ImageViewCreateInfo view_info;
  view_info.image = image;
  view_info.viewType = vk::ImageViewType::e2D;
  view_info.format = image_info.format;
  view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  auto [view_result, view] = core_->device->createImageViewUnique(view_info);
  if (view_result != vk::Result::eSuccess) {
    ::vmaDestroyImage(core_->vma.allocator, image, allocation);
    return nullptr;
  }

  auto texture = std::make_unique<GPUTextureVK>(core_, desc, image, allocation,
                                                std::move(view));
  // if (zeroed) {
  //   auto commands = CreateCommandBuffer();
  //   if (commands == nullptr) {
  //     return nullptr;
  //   }
  //   Transition(commands->GetCommandBuffer(), *texture,
  //              vk::ImageLayout::eTransferDstOptimal);
  //   vk::ImageSubresourceRange range;
  //   range.aspectMask = vk::ImageAspectFlagBits::eColor;
  //   range.levelCount = 1;
  //   range.layerCount = 1;
  //   commands->GetCommandBuffer().clearColorImage(
  //       texture->GetImage(), vk::ImageLayout::eTransferDstOptimal,
  //       vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f}, range);
  //   Transition(commands->GetCommandBuffer(), *texture,
  //              vk::ImageLayout::eShaderReadOnlyOptimal);
  //   // Later work on this queue is ordered after the clear; nothing needs
  //   // to block for it, only the command buffer's lifetime does.
  //   if (!SubmitAndRetire(std::move(commands))) {
  //     return nullptr;
  //   }
  // }
  return texture;
}

std::unique_ptr<GPUTexture> GPUContextVK::CreateTransientTexture(
    TextureFormat format,
    uint32_t width,
    uint32_t height) {
  vk::ImageCreateInfo image_info;
  image_info.imageType = vk::ImageType::e2D;
  image_info.format = MLRVkFormat(format);
  image_info.extent = vk::Extent3D{width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.usage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eInputAttachment |
                     vk::ImageUsageFlagBits::eTransientAttachment;
  const auto image_info_native =
      static_cast<vk::ImageCreateInfo::NativeType>(image_info);

  VmaAllocationCreateInfo allocation_info = {};
  allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
  allocation_info.preferredFlags = static_cast<VkMemoryPropertyFlags>(
      vk::MemoryPropertyFlagBits::eDeviceLocal |
      vk::MemoryPropertyFlagBits::eLazilyAllocated);

  VkImage image = {};
  VmaAllocation allocation = {};
  if (vk::Result{::vmaCreateImage(core_->vma.allocator, &image_info_native,
                                  &allocation_info, &image, &allocation,
                                  nullptr)} != vk::Result::eSuccess) {
    return nullptr;
  }

  vk::ImageViewCreateInfo view_info;
  view_info.image = image;
  view_info.viewType = vk::ImageViewType::e2D;
  view_info.format = image_info.format;
  view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  auto [view_result, view] = core_->device->createImageViewUnique(view_info);
  if (view_result != vk::Result::eSuccess) {
    ::vmaDestroyImage(core_->vma.allocator, image, allocation);
    return nullptr;
  }
  return std::make_unique<GPUTextureVK>(
      core_, TextureDesc{.format = format, .width = width, .height = height},
      image, allocation, std::move(view));
}

std::unique_ptr<GPUBuffer> GPUContextVK::CreateBuffer(const uint8_t* bytes,
                                                      size_t size) {
  vk::BufferCreateInfo buffer_info;
  buffer_info.size = size;
  buffer_info.usage = vk::BufferUsageFlagBits::eStorageBuffer |
                      vk::BufferUsageFlagBits::eUniformBuffer |
                      vk::BufferUsageFlagBits::eTransferSrc |
                      vk::BufferUsageFlagBits::eTransferDst;
  const auto buffer_info_native =
      static_cast<vk::BufferCreateInfo::NativeType>(buffer_info);

  VmaAllocationCreateInfo allocation_info = {};
  allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
  allocation_info.preferredFlags = static_cast<VkMemoryPropertyFlags>(
      vk::MemoryPropertyFlagBits::eHostVisible |
      vk::MemoryPropertyFlagBits::eHostCoherent);
  allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;

  VkBuffer buffer = {};
  VmaAllocation allocation = {};
  VmaAllocationInfo result_info = {};
  if (vk::Result{::vmaCreateBuffer(core_->vma.allocator, &buffer_info_native,
                                   &allocation_info, &buffer, &allocation,
                                   &result_info)} != vk::Result::eSuccess) {
    return nullptr;
  }
  auto* mapping = static_cast<uint8_t*>(result_info.pMappedData);
  if (bytes != nullptr) {
    std::memcpy(mapping, bytes, size);
  }
  return std::make_unique<GPUBufferVK>(core_, buffer, allocation, mapping);
}

std::unique_ptr<GpuCommandBufferVK> GPUContextVK::CreateCommandBuffer() {
  vk::CommandBufferAllocateInfo allocate_info;
  allocate_info.commandPool = core_->command_pool.get();
  allocate_info.level = vk::CommandBufferLevel::ePrimary;
  allocate_info.commandBufferCount = 1;
  auto [result, buffers] =
      core_->device->allocateCommandBuffersUnique(allocate_info);
  if (result != vk::Result::eSuccess) {
    return nullptr;
  }
  vk::CommandBufferBeginInfo begin_info;
  begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  if (buffers[0]->begin(begin_info) != vk::Result::eSuccess) {
    return nullptr;
  }
  return std::make_unique<GpuCommandBufferVK>(core_, std::move(buffers[0]));
}

bool GPUContextVK::Submit(GpuCommandBufferVK& command_buffer,
                          vk::Fence fence,
                          vk::Semaphore wait,
                          vk::Semaphore signal) {
  if (const vk::Result result = command_buffer.GetCommandBuffer().end();
      result != vk::Result::eSuccess) {
    FML_LOG(ERROR) << "Propeller command buffer end failed: "
                   << vk::to_string(result);
    return false;
  }
  vk::SubmitInfo submit_info;
  const vk::CommandBuffer handle = command_buffer.GetCommandBuffer();
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &handle;
  const vk::PipelineStageFlags wait_stage =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  if (wait) {
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &wait;
    submit_info.pWaitDstStageMask = &wait_stage;
  }
  if (signal) {
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &signal;
  }
  if (const vk::Result result = core_->queue.submit(submit_info, fence);
      result != vk::Result::eSuccess) {
    FML_LOG(ERROR) << "Propeller queue submit failed: "
                   << vk::to_string(result);
    return false;
  }
  return true;
}

bool GPUContextVK::SubmitAndWait(GpuCommandBufferVK& command_buffer) {
  auto [fence_result, fence] = core_->device->createFenceUnique({});
  if (fence_result != vk::Result::eSuccess) {
    return false;
  }
  if (!Submit(command_buffer, fence.get())) {
    return false;
  }
  return core_->device->waitForFences(fence.get(), VK_TRUE,
                                      std::numeric_limits<uint64_t>::max()) ==
         vk::Result::eSuccess;
}

bool GPUContextVK::SubmitAndRetire(
    std::unique_ptr<GpuCommandBufferVK> command_buffer) {
  ReapRetired();
  auto [fence_result, fence] = core_->device->createFenceUnique({});
  if (fence_result != vk::Result::eSuccess) {
    return false;
  }
  if (!Submit(*command_buffer, fence.get())) {
    return false;
  }
  retired_.push_back(
      RetiredSubmission{std::move(fence), std::move(command_buffer)});
  return true;
}

void GPUContextVK::ReapRetired() {
  auto reaped = std::remove_if(
      retired_.begin(), retired_.end(), [&](const RetiredSubmission& retired) {
        return core_->device->getFenceStatus(retired.fence.get()) ==
               vk::Result::eSuccess;
      });
  retired_.erase(reaped, retired_.end());
}

std::vector<uint8_t> GPUContextVK::ReadbackTexture(GPUTexture& texture) {
  auto& source = static_cast<GPUTextureVK&>(texture);
  const TextureDesc& desc = texture.GetDesc();
  const size_t size = static_cast<size_t>(desc.width) * desc.height *
                      BytesPerPixel(desc.format);
  std::unique_ptr<GPUBuffer> staging = CreateBuffer(nullptr, size);
  auto commands = CreateCommandBuffer();
  if (staging == nullptr || commands == nullptr) {
    return {};
  }
  const vk::ImageLayout restored = source.layout;
  Transition(commands->GetCommandBuffer(), source,
             vk::ImageLayout::eTransferSrcOptimal);
  vk::BufferImageCopy copy;
  copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = vk::Extent3D{desc.width, desc.height, 1};
  commands->GetCommandBuffer().copyImageToBuffer(
      source.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
      static_cast<GPUBufferVK*>(staging.get())->GetBuffer(), copy);
  Transition(commands->GetCommandBuffer(), source, restored);
  if (!SubmitAndWait(*commands)) {
    return {};
  }
  std::vector<uint8_t> pixels(size);
  std::memcpy(pixels.data(), staging->Contents(), size);
  return pixels;
}

}  // namespace impeller
