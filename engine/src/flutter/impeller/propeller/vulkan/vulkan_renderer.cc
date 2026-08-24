// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/vulkan/vulkan_renderer.h"
#include "impeller/propeller/gradient_atlas.h"

#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/propeller/dispatcher.h"
#include "impeller/propeller/renderer/shadow_lut.h"
#include "impeller/propeller/vulkan/vulkan_pipelines.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"

namespace impeller {

// struct VulkanRenderer::Impl final : public FrameImageResolver {
//   std::shared_ptr<GPUContextVK> context;
//   std::shared_ptr<PagedAtlas> atlas;
//   std::shared_ptr<GradientAtlas> gradients;
//   std::shared_ptr<TextMaterializer> materializer;
//   std::unique_ptr<GPUProgramResolverVK> programs;
//   ShadowLUT shadow_lut;
//   uint64_t synced_atlas_revision = 0;
//   /// EncodeRuns wants the table as a buffer, which it is on Metal; here
//   /// it is descriptor set 1 and this placeholder is never bound.
//   std::unique_ptr<GPUBuffer> table_placeholder;

//   vk::UniqueDescriptorPool table_pool;
//   /// The propeller path's geometry buffers, which carry a frames in
//   /// flight ring of their own.
//   std::unique_ptr<BufferArena> arena;
//   /// Every texture a propeller frame renders into, held past the frame
//   /// so an unchanged layer can be drawn from what it resolved into last
//   /// time.
//   std::unique_ptr<TextureCache> textures;

//   static constexpr size_t kFramesInFlight = 3;
//   /// Everything one frame's execution reads stays in its slot until the
//   /// fence proves the GPU is done with it.
//   struct FrameSlot {
//     FrameArenas arenas;
//     vk::UniqueFence fence;
//     bool submitted = false;
//     std::unique_ptr<GpuCommandBufferVK> commands;
//     /// Wrappers around engine images sampled this frame. Each carries
//     /// its engine texture as owner, held until the fence proves the
//     /// frame done: dart:ui can dispose an image (a transition snapshot,
//     /// a decoded image) the instant its last frame is submitted.
//     std::vector<std::unique_ptr<GPUTextureVK>> images;
//     /// Resources evicted while this frame was recorded. Destruction
//     /// waits for the fence: this frame finishing proves every earlier
//     /// frame finished too, the queue being in order.
//     std::vector<std::unique_ptr<GPUTexture>> retired_textures;
//     std::vector<std::unique_ptr<GPUBuffer>> retired_buffers;
//     vk::DescriptorSet table_set;
//     /// Signalled by the slot's submission when the frame hands its
//     /// completion to a present. Fresh per frame: a dropped present
//     /// leaves the old one signalled, and a binary semaphore must never
//     /// be signalled twice. Retired ones drain behind the fence.
//     vk::UniqueSemaphore done;
//     std::vector<vk::UniqueSemaphore> retired_semaphores;
//   };
//   FrameSlot frame_ring[kFramesInFlight];
//   /// The slot RenderFrame is currently writing.
//   FrameSlot* frame = nullptr;

//   /// Render-pass texture recycling, keyed by RenderPassPlan::cache_key.
//   struct PassTexture {
//     std::unique_ptr<GPUTexture> texture;
//     uint64_t last_used_frame = 0;
//   };
//   std::unordered_map<uint64_t, PassTexture> pass_cache;
//   std::deque<std::unique_ptr<GPUTexture>> texture_pool;

//   /// The transient canvas + accumulator per target size, reused across
//   /// frames like Metal's memoryless canvases.
//   struct CanvasSet {
//     std::unique_ptr<GPUTexture> canvas;
//     std::unique_ptr<GPUTexture> winding;
//     uint64_t last_used_frame = 0;
//   };
//   std::unordered_map<uint64_t, CanvasSet> canvas_cache;

//   CanvasSet* GetCanvases(uint32_t width, uint32_t height) {
//     const uint64_t key = (static_cast<uint64_t>(width) << 32) | height;
//     CanvasSet& set = canvas_cache[key];
//     set.last_used_frame = frame_number;
//     if (set.canvas == nullptr) {
//       set.canvas =
//       context->CreateTransientTexture(TextureFormat::kRGBA8UNorm,
//                                                    width, height);
//       set.winding = context->CreateTransientTexture(TextureFormat::kR16Float,
//                                                     width, height);
//       if (set.canvas == nullptr || set.winding == nullptr) {
//         canvas_cache.erase(key);
//         return nullptr;
//       }
//     }
//     return &set;
//   }

//   /// Hand a resource that may still be in flight to the current frame's
//   /// slot; the fence decides when it actually dies.
//   void Retire(std::unique_ptr<GPUTexture> texture) {
//     frame->retired_textures.push_back(std::move(texture));
//   }
//   void Retire(std::unique_ptr<GPUBuffer> buffer) {
//     frame->retired_buffers.push_back(std::move(buffer));
//   }
//   static constexpr size_t kMaxPooledTextures = 32;
//   uint64_t frame_number = 0;
//   uint32_t last_frame_encoded_passes = 0;

//   /// Registered engine images, wrapped without ownership so the bindless
//   /// slot baked into cached pictures stays valid for the frame.
//   std::unordered_map<void*, int32_t> frame_image_slots;
//   int32_t next_frame_image_slot = 0;
//   bool warned_image_slots_full = false;

//   /// The bindless slot for `image` this frame, assigning one if it has
//   /// not been seen yet. -1 when the frame has used every slot.
//   // |FrameImageResolver|
//   int32_t Slot(const sk_sp<flutter::DlImage>& image) override {
//     if (!image) {
//       return -1;
//     }
//     const DlImageImpeller* impeller_image = image->asImpellerImage();
//     if (impeller_image == nullptr) {
//       return -1;  // Another backend's image: nothing here can draw it.
//     }
//     const std::shared_ptr<Texture> texture =
//         impeller_image->GetImpellerTexture(nullptr);
//     if (!texture) {
//       return -1;
//     }
//     // Only meaningful on an adopted device, where the engine's images
//     // live where propeller renders.
//     TextureVK& source = TextureVK::Cast(*texture);
//     void* key = static_cast<VkImage>(source.GetImage());
//     auto found = frame_image_slots.find(key);
//     if (found != frame_image_slots.end()) {
//       return found->second;
//     }
//     if (next_frame_image_slot >= kMLRImageTextureSlots) {
//       if (!warned_image_slots_full) {
//         warned_image_slots_full = true;
//         FML_LOG(WARNING) << "Propeller: more than " << kMLRImageTextureSlots
//                          << " images in one frame; the rest are skipped.";
//       }
//       return -1;
//     }
//     const int32_t slot = kMLRImageTextureSlotBase + next_frame_image_slot++;
//     frame_image_slots[key] = slot;
//     frame->images.push_back(std::make_unique<GPUTextureVK>(
//         context->GetCore(),
//         TextureDesc{.format = TextureFormat::kRGBA8UNorm,
//                     .width = static_cast<uint32_t>(texture->GetSize().width),
//                     .height =
//                     static_cast<uint32_t>(texture->GetSize().height)},
//         source.GetImage(), source.GetImageView(), source.GetLayout(),
//         texture));
//     return slot;
//   }

//   bool SetUp() {
//     programs = std::make_unique<GPUProgramResolverVK>(context->GetCore());
//     if (!programs->IsValid()) {
//       return false;
//     }
//     table_placeholder = context->CreateBuffer(nullptr, 16);
//     if (table_placeholder == nullptr) {
//       return false;
//     }
//     if (!shadow_lut.Attach(*context)) {
//       return false;
//     }
//     if (gradients) {
//       gradients->AttachBacking(context);
//     }

//     // A table per ring slot: a frame in flight samples its own set
//     // while the next frame's writes land in another.
//     vk::DescriptorPoolSize size;
//     size.type = vk::DescriptorType::eSampledImage;
//     size.descriptorCount = kVKTextureTableSlots * kFramesInFlight;
//     vk::DescriptorPoolCreateInfo pool_info;
//     pool_info.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
//     pool_info.maxSets = kFramesInFlight;
//     pool_info.poolSizeCount = 1;
//     pool_info.pPoolSizes = &size;
//     auto [pool_result, pool] =
//         context->GetCore()->device->createDescriptorPoolUnique(pool_info);
//     if (pool_result != vk::Result::eSuccess) {
//       return false;
//     }
//     table_pool = std::move(pool);

//     for (FrameSlot& slot : frame_ring) {
//       const vk::DescriptorSetLayout layout = programs->GetTableSetLayout();
//       vk::DescriptorSetAllocateInfo allocate_info;
//       allocate_info.descriptorPool = table_pool.get();
//       allocate_info.descriptorSetCount = 1;
//       allocate_info.pSetLayouts = &layout;
//       auto [set_result, sets] =
//           context->GetCore()->device->allocateDescriptorSets(allocate_info);
//       if (set_result != vk::Result::eSuccess) {
//         return false;
//       }
//       slot.table_set = sets[0];
//       auto [fence_result, fence] =
//           context->GetCore()->device->createFenceUnique({});
//       if (fence_result != vk::Result::eSuccess) {
//         return false;
//       }
//       slot.fence = std::move(fence);
//     }
//     return true;
//   }

//   std::unique_ptr<GPUTexture> TakePooledTexture(uint32_t width,
//                                                 uint32_t height) {
//     for (auto it = texture_pool.begin(); it != texture_pool.end(); ++it) {
//       if ((*it)->GetDesc().width == width &&
//           (*it)->GetDesc().height == height) {
//         std::unique_ptr<GPUTexture> texture = std::move(*it);
//         texture_pool.erase(it);
//         return texture;
//       }
//     }
//     return nullptr;
//   }

//   /// Record atlas uploads; page rebinding happens with the frame's
//   /// table writes, which bind every page each time.
//   void SyncAtlasPages(GpuCommandBufferVK& commands) {
//     const uint64_t revision = atlas->GetRevision();
//     if (revision == synced_atlas_revision) {
//       return;
//     }
//     synced_atlas_revision = revision;
//     if (atlas->HasPendingUploads()) {
//       TRACE_EVENT0("flutter", "Propeller::UploadAtlasPages");
//       atlas->RecordUploads(commands);
//     }
//   }

//   void SyncGradients(GpuCommandBufferVK& commands) {
//     if (!gradients) {
//       return;
//     }
//     gradients->EvictUnused();
//     if (!gradients->HasPendingUploads()) {
//       return;
//     }
//     TRACE_EVENT0("flutter", "Propeller::UploadGradientRamps");
//     gradients->RecordUploads(commands);
//   }

//   /// Point every live table slot at its texture. Update-after-bind, so
//   /// this may run after the bind is recorded, as long as it lands
//   /// before submit.
//   void WriteTable(vk::DescriptorSet table_set,
//                   const std::vector<GPUTexture*>& pass_targets,
//                   const FramePlan& plan) {
//     std::vector<vk::DescriptorImageInfo> infos;
//     std::vector<uint32_t> slots;
//     auto add = [&](int32_t slot, GPUTexture* texture) {
//       if (texture == nullptr) {
//         return;
//       }
//       vk::DescriptorImageInfo info;
//       info.imageView = static_cast<GPUTextureVK*>(texture)->GetImageView();
//       info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
//       infos.push_back(info);
//       slots.push_back(static_cast<uint32_t>(slot));
//     };
//     for (size_t i = 0; i < atlas->GetPageCount(); i++) {
//       add(static_cast<int32_t>(i), atlas->GetPageTexture(i));
//     }
//     if (gradients) {
//       add(kMLRGradientTextureSlot, gradients->GetTexture());
//     }
//     add(kMLRShadowLUTTextureSlot, shadow_lut.GetTexture());
//     for (size_t i = 0; i < frame->images.size(); i++) {
//       add(kMLRImageTextureSlotBase + static_cast<int32_t>(i),
//           frame->images[i].get());
//     }
//     for (size_t i = 1; i < pass_targets.size(); i++) {
//       const int32_t slot =
//           plan.passes[i].bindless_slot >= 0
//               ? plan.passes[i].bindless_slot
//               : kMLRPassTextureSlotBase + static_cast<int32_t>(i);
//       add(slot, pass_targets[i]);
//     }
//     std::vector<vk::WriteDescriptorSet> writes(infos.size());
//     for (size_t i = 0; i < infos.size(); i++) {
//       writes[i].dstSet = table_set;
//       writes[i].dstBinding = 0;
//       writes[i].dstArrayElement = slots[i];
//       writes[i].descriptorCount = 1;
//       writes[i].descriptorType = vk::DescriptorType::eSampledImage;
//       writes[i].pImageInfo = &infos[i];
//     }
//     context->GetCore()->device->updateDescriptorSets(writes, nullptr);
//   }
// };

// std::unique_ptr<VulkanRenderer> VulkanRenderer::Make(
//     std::shared_ptr<GPUContextVK> context,
//     std::shared_ptr<PagedAtlas> atlas,
//     std::shared_ptr<TextMaterializer> materializer) {
//   if (!context) {
//     return nullptr;
//   }
//   auto impl = std::make_unique<Impl>();
//   impl->context = std::move(context);
//   impl->atlas = std::move(atlas);
//   impl->gradients = std::make_shared<GradientAtlas>(impl->context.get());
//   impl->materializer = std::move(materializer);
//   if (!impl->SetUp()) {
//     return nullptr;
//   }
//   return std::unique_ptr<VulkanRenderer>(new
//   VulkanRenderer(std::move(impl)));
// }

// VulkanRenderer::VulkanRenderer(std::unique_ptr<Impl> impl)
//     : impl_(std::move(impl)) {}

// VulkanRenderer::~VulkanRenderer() {
//   // Everything owned here -- pass textures, pipelines, retired slots --
//   // may still be referenced by in-flight frames.
//   (void)impl_->context->GetCore()->device->waitIdle();
// }

// bool VulkanRenderer::Render(const PrPicture& picture,
//                             GPUTexture& target,
//                             Color clear_color,
//                             vk::Semaphore wait_acquire,
//                             vk::Semaphore* render_done) {
//   PrSceneNode scene;
//   PrSceneNode leaf;
//   // Aliased, not owned: the scene does not outlive this call.
//   leaf.picture = std::shared_ptr<PrPicture>(std::shared_ptr<void>(),
//                                             const_cast<PrPicture*>(&picture));
//   scene.children.push_back(std::move(leaf));
//   return Render(scene, target, clear_color, wait_acquire, render_done);
// }

// bool VulkanRenderer::Render(const PrSceneNode& scene,
//                             GPUTexture& target,
//                             Color clear_color,
//                             vk::Semaphore wait_acquire,
//                             vk::Semaphore* render_done) {
//   TRACE_EVENT0("flutter", "Propeller::RenderScene");
//   Impl& impl = *impl_;
//   impl.frame_number++;
//   impl.context->ReapRetired();
//   impl.frame_image_slots.clear();
//   impl.next_frame_image_slot = 0;

//   // Take the slot's resources back from the GPU before reusing them.
//   Impl::FrameSlot& slot =
//       impl.frame_ring[impl.frame_number % Impl::kFramesInFlight];
//   impl.frame = &slot;
//   const vk::Device device = impl.context->GetCore()->device.get();
//   if (slot.submitted) {
//     if (device.waitForFences(slot.fence.get(), VK_TRUE,
//                              std::numeric_limits<uint64_t>::max()) !=
//         vk::Result::eSuccess) {
//       return false;
//     }
//     if (device.resetFences(slot.fence.get()) != vk::Result::eSuccess) {
//       return false;
//     }
//     slot.submitted = false;
//   }
//   slot.commands.reset();
//   slot.images.clear();
//   slot.retired_textures.clear();
//   slot.retired_buffers.clear();
//   slot.retired_semaphores.clear();

//   if (!impl.programs->SetTargetFormat(target.GetDesc().format)) {
//     return false;
//   }
//   // The two the dispatcher can name, which are the base content
//   // pipelines the resolver already builds.
//   ProgramSet programs = {};
//   programs[ProgramType::kColor] = impl.programs->Resolve(
//       ProgramKey{.kind = MLRDrawKind::kContent, .technique =
//       kColorTechnique});
//   programs[ProgramType::kPath] = impl.programs->Resolve(
//       ProgramKey{.kind = MLRDrawKind::kContent, .technique =
//       kPathTechnique});
//   if (programs[ProgramType::kColor] == nullptr ||
//       programs[ProgramType::kPath] == nullptr) {
//     return false;
//   }

//   slot.commands = impl.context->CreateCommandBuffer();
//   if (slot.commands == nullptr) {
//     return false;
//   }
//   GpuCommandBufferVK& commands = *slot.commands;
//   impl.SyncAtlasPages(commands);
//   impl.SyncGradients(commands);
//   if (impl.shadow_lut.HasPendingUpload()) {
//     impl.shadow_lut.RecordUpload(commands);
//   }
//   // No passes, so no pass textures in the table; the atlas, gradient and
//   // shadow entries still have to be written or their descriptors are
//   // invalid even unread.
//   impl.WriteTable(slot.table_set, {}, FramePlan{});
//   commands.SetSharedState(
//       impl.programs->GetPipelineLayout(),
//       impl.programs->GetBuffersSetLayout(),
//       impl.programs->GetInputsSetLayout(), slot.table_set,
//       static_cast<GPUBufferVK*>(impl.table_placeholder.get())->GetBuffer());

//   if (impl.arena == nullptr) {
//     impl.arena = std::make_unique<BufferArena>(*impl.context);
//   }
//   if (impl.textures == nullptr) {
//     impl.textures = std::make_unique<TextureCache>(impl.context.get());
//   }
//   // The frame boundary: what last frame stopped using is kept one frame
//   // longer, in case this one wants a texture that size.
//   //
//   // TODO: what the cache drops is destroyed there and then, which a
//   // frame still in flight may be reading. It has to go through Retire.
//   impl.textures->Next();
//   Dispatch(scene, *impl.arena, programs, target, *impl.textures,
//            impl.materializer.get(), commands);
//   // The arena's ring is what keeps this frame's buffers alive while it is
//   // in flight, so the rewind happens once the frame is encoded.
//   impl.arena->Reset();

//   vk::Semaphore signal;
//   if (render_done != nullptr) {
//     if (slot.done) {
//       slot.retired_semaphores.push_back(std::move(slot.done));
//     }
//     auto [semaphore_result, semaphore] =
//         impl.context->GetCore()->device->createSemaphoreUnique({});
//     if (semaphore_result != vk::Result::eSuccess) {
//       return false;
//     }
//     slot.done = std::move(semaphore);
//     signal = slot.done.get();
//     ContextVK::SetDebugName(
//         device, signal, "PropellerDoneF" +
//         std::to_string(impl.frame_number));
//   }
//   if (!impl.context->Submit(commands, slot.fence.get(), wait_acquire,
//   signal)) {
//     return false;
//   }
//   slot.submitted = true;
//   if (render_done != nullptr) {
//     *render_done = signal;
//   }
//   return true;
// }

// uint32_t VulkanRenderer::GetLastFrameEncodedPassCount() const {
//   return impl_->last_frame_encoded_passes;
// }

}  // namespace impeller
