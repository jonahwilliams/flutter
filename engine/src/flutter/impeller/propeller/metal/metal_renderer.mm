// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <deque>

#include "flutter/display_list/effects/color_filters/dl_blend_color_filter.h"
#include "flutter/display_list/effects/color_filters/dl_matrix_color_filter.h"
#include "impeller/propeller/gradient_atlas.h"
#include "impeller/propeller/metal/metal_renderer.h"
#include <Metal/Metal.h>

#include "impeller/propeller/dispatcher.h"
#include "impeller/propeller/metal/metal_pipelines.h"
#include "impeller/propeller/renderer/gpu_context.h"
#include "impeller/propeller/renderer/shadow_lut.h"

#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/renderer/backend/metal/texture_mtl.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <vector>

#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"

namespace impeller {

// Texture

GPUTextureMTL::GPUTextureMTL(const TextureDesc& desc, id<MTLTexture> texture)
    : GPUTexture(desc), texture_(texture) {}

GPUTextureMTL::~GPUTextureMTL() = default;

// Buffer

GPUBufferMTL::GPUBufferMTL(id<MTLBuffer> buffer) : buffer_(buffer) {}

GPUBufferMTL::~GPUBufferMTL() = default;

void GPUBufferMTL::Write(const uint8_t* data,
                         uint32_t length,
                         uint32_t offset) {
  FML_DCHECK(offset + length <= buffer_.length);
  std::memcpy(reinterpret_cast<uint8_t*>(buffer_.contents) + offset, data,
              length);
}

uint8_t* GPUBufferMTL::Contents() {
  return static_cast<uint8_t*>(buffer_.contents);
}

void GPUBufferMTL::SetLabel(const char* label) {
  buffer_.label = [NSString stringWithUTF8String:label];
}

// Command buffer

GpuCommandBufferMTL::GpuCommandBufferMTL(id<MTLCommandBuffer> command_buffer)
    : command_buffer_(command_buffer) {}

GpuCommandBufferMTL::~GpuCommandBufferMTL() {
  FML_DCHECK(blit_ == nil) << "EndEncoding was never called.";
}

void GpuCommandBufferMTL::UpdateRegion(const GPUTexture& texture,
                                       uint32_t x,
                                       uint32_t y,
                                       uint32_t width,
                                       uint32_t height,
                                       const GPUBuffer& buffer,
                                       uint32_t offset,
                                       uint32_t row_bytes) {
  if (blit_ == nil) {
    blit_ = [command_buffer_ blitCommandEncoder];
    blit_.label = @"Propeller texture upload";
  }
  [blit_ copyFromBuffer:static_cast<const GPUBufferMTL&>(buffer).GetBuffer()
             sourceOffset:offset
        sourceBytesPerRow:row_bytes
      sourceBytesPerImage:row_bytes * height
               sourceSize:MTLSizeMake(width, height, 1)
                toTexture:static_cast<const GPUTextureMTL&>(texture)
                              .GetTexture()
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(x, y, 0)];
}

void GpuCommandBufferMTL::StartRenderPass(const RenderPassDesc& desc) {
  FML_DCHECK(render_ == nil);
  EndEncoding();
  MTLRenderPassDescriptor* descriptor =
      [MTLRenderPassDescriptor renderPassDescriptor];
  descriptor.colorAttachments[0].texture =
      static_cast<const GPUTextureMTL*>(desc.target)->GetTexture();
  descriptor.colorAttachments[0].loadAction =
      desc.load ? MTLLoadActionLoad : MTLLoadActionClear;
  descriptor.colorAttachments[0].clearColor =
      MTLClearColorMake(desc.clear_color[0], desc.clear_color[1],
                        desc.clear_color[2], desc.clear_color[3]);
  descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
  
  for (size_t i = 0; i < desc.transient_count; i++) {
    descriptor.colorAttachments[i + 1].texture =
        static_cast<const GPUTextureMTL*>(desc.transients[i].texture)
            ->GetTexture();
    descriptor.colorAttachments[i + 1].loadAction = MTLLoadActionClear;
    const double clear = desc.transients[i].clear;
    descriptor.colorAttachments[i + 1].clearColor =
        MTLClearColorMake(clear, clear, clear, clear);
    descriptor.colorAttachments[i + 1].storeAction = MTLStoreActionDontCare;
  }
  render_ = [command_buffer_ renderCommandEncoderWithDescriptor:descriptor];
  render_.label = [NSString stringWithUTF8String:desc.label];

  const TextureDesc& tex_desc = desc.target->GetDesc();
  current_scissor_ = IRect::MakeLTRB(0, 0, tex_desc.width, tex_desc.height);
  pending_scissor_ = std::nullopt;
}

void GpuCommandBufferMTL::EndRenderPass() {
  [render_ endEncoding];
  render_ = nil;
  pending_scissor_ = std::nullopt;
}

void GpuCommandBufferMTL::SetRenderPipeline(const GPUProgram& program) {
  [render_ setRenderPipelineState:static_cast<const GPUProgramMTL&>(program)
                                      .GetNativePipeline()];
}

void GpuCommandBufferMTL::SetScissorRect(int x, int y, int w, int h) {
  IRect candidate = IRect::MakeLTRB(x, y, w, h);
  if (pending_scissor_.has_value()) {
    if (candidate == *pending_scissor_) {
      return;
    } else if (candidate == current_scissor_) {
      pending_scissor_ = std::nullopt;
      return;
    }
  } else {
    if (candidate == current_scissor_) {
      return;
    }
  }
  pending_scissor_ = candidate;
}

void GpuCommandBufferMTL::SetBuffer(GPUShaderStage stage,
                                    const GPUBuffer& buffer,
                                    size_t offset,
                                    int index) {
  const id<MTLBuffer>& handle =
      static_cast<const GPUBufferMTL&>(buffer).GetBuffer();
  if (stage == GPUShaderStage::kVertex) {
    [render_ setVertexBuffer:handle offset:offset atIndex:index];
  } else {
    [render_ setFragmentBuffer:handle offset:offset atIndex:index];
  }
}

void GpuCommandBufferMTL::SetTextureTable(GPUTexture* const* textures,
                                          size_t count) {
  std::vector<MTLResourceID> slots(count);
  for (size_t i = 0; i < count; i++) {
    if (textures[i] == nullptr) {
      continue;
    }
    id<MTLTexture> texture =
        static_cast<const GPUTextureMTL*>(textures[i])->GetTexture();
    slots[i] = texture.gpuResourceID;
    [render_ useResource:texture
                   usage:MTLResourceUsageRead
                  stages:MTLRenderStageFragment];
  }
  [render_ setFragmentBytes:slots.data()
                     length:count * sizeof(MTLResourceID)
                    atIndex:1];
}

void GpuCommandBufferMTL::SetConstantData(GPUShaderStage stage,
                                          const void* data,
                                          size_t bytes,
                                          int index) {
  if (stage == GPUShaderStage::kVertex) {
    [render_ setVertexBytes:data length:bytes atIndex:index];
  } else {
    [render_ setFragmentBytes:data length:bytes atIndex:index];
  }
}

void GpuCommandBufferMTL::DrawTriangles(int start, int count) {
  FlushScissor();
  [render_ drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:start
              vertexCount:count];
}

void GpuCommandBufferMTL::DrawLines(int start, int count) {
  FlushScissor();
  [render_ drawPrimitives:MTLPrimitiveTypeLineStrip
              vertexStart:start
              vertexCount:count];
}

void GpuCommandBufferMTL::DrawTrianglesIndexed(const GPUBuffer& indices,
                                               int start,
                                               int count) {
  FlushScissor();
  const id<MTLBuffer>& handle =
      static_cast<const GPUBufferMTL&>(indices).GetBuffer();
  [render_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:count
                       indexType:MTLIndexTypeUInt16
                     indexBuffer:handle
               indexBufferOffset:start * sizeof(uint16_t)];
}

void GpuCommandBufferMTL::FlushScissor() {
  if (pending_scissor_.has_value()) {
      [render_ setScissorRect:MTLScissorRect{static_cast<NSUInteger>(pending_scissor_->GetLeft()),
                                         static_cast<NSUInteger>(pending_scissor_->GetTop()),
                                         static_cast<NSUInteger>(pending_scissor_->GetRight()),
                                         static_cast<NSUInteger>(pending_scissor_->GetBottom())}];
    current_scissor_ = pending_scissor_.value();
    pending_scissor_ = std::nullopt;
  }
}

void GpuCommandBufferMTL::EndEncoding() {
  if (blit_ == nil) {
    return;
  }
  [blit_ endEncoding];
  blit_ = nil;
}

// Context

namespace {

/// Indexed by TextureFormat.
constexpr MTLPixelFormat kPixelFormats[] = {
    MTLPixelFormatR8Unorm,  //
    MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm,
    MTLPixelFormatR16Float,   MTLPixelFormatBGRA10_XR,
};
static_assert(std::size(kPixelFormats) ==
              static_cast<size_t>(TextureFormat::kBGRA10XR) + 1);

MTLPixelFormat ToMTLPixelFormat(TextureFormat format) {
  FML_DCHECK(static_cast<size_t>(format) < std::size(kPixelFormats));
  return kPixelFormats[static_cast<size_t>(format)];
}

/// A wrapped texture's format as the renderer names it. Nothing for a
/// format it cannot: an offscreen has to be made in the same format as
/// the drawable, and one that cannot be named cannot be made.
std::optional<TextureFormat> FromMTLPixelFormat(MTLPixelFormat format) {
  for (size_t i = 0; i < std::size(kPixelFormats); i++) {
    if (kPixelFormats[i] == format) {
      return static_cast<TextureFormat>(i);
    }
  }
  return std::nullopt;
}

}  // namespace

std::shared_ptr<GPUContextMTL> GPUContextMTL::Make() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    return nullptr;
  }
  return std::make_shared<GPUContextMTL>(device);
}

GPUContextMTL::GPUContextMTL(id<MTLDevice> device)
    : device_(device), queue_([device newCommandQueue]) {}

GPUContextMTL::~GPUContextMTL() = default;

std::unique_ptr<GPUTexture> GPUContextMTL::CreateTexture(
    const TextureDesc& desc,
    bool zeroed) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:ToMTLPixelFormat(desc.format)
                                   width:desc.width
                                  height:desc.height
                               mipmapped:NO];
  if (desc.transient) {
    descriptor.storageMode = MTLStorageModeMemoryless;
    descriptor.usage = MTLTextureUsageRenderTarget;
  } else {
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  }
  id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
  if (texture == nil) {
    return nullptr;
  }
  auto result = std::make_unique<GPUTextureMTL>(desc, texture);
  if (!zeroed) {
    return result;
  }
  const size_t row_bytes = desc.width * BytesPerPixel(desc.format);
  id<MTLBuffer> zeros =
      [device_ newBufferWithLength:row_bytes * desc.height
                           options:MTLResourceStorageModeShared];
  if (zeros == nil) {
    return nullptr;
  }
  std::memset(zeros.contents, 0, zeros.length);
  GPUBufferMTL source(zeros);
  id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
  command_buffer.label = @"Propeller texture clear";
  GpuCommandBufferMTL recorder(command_buffer);
  recorder.UpdateRegion(*result, 0, 0, desc.width, desc.height, source, 0,
                        static_cast<uint32_t>(row_bytes));
  recorder.EndEncoding();
  [command_buffer commit];
  return result;
}

GPUTexture* GPUContextMTL::GetDLImageTexture(
    const sk_sp<flutter::DlImage>& image) {
  if (!image) {
    return nullptr;
  }
  auto found = wrapped_images_.find(image.get());
  if (found != wrapped_images_.end()) {
    return found->second.texture.get();
  }

  const DlImageImpeller* impeller_image = image->asImpellerImage();
  if (impeller_image == nullptr) {
    return nullptr;
  }
  const std::shared_ptr<Texture>& texture =
      impeller_image->GetImpellerTexture(nullptr);
  if (!texture) {
    return nullptr;
  }
  id<MTLTexture> mtl = TextureMTL::Cast(*texture).GetMTLTexture();
  if (mtl == nil) {
    return nullptr;
  }

  const TextureDesc desc{
      .format = FromMTLPixelFormat(mtl.pixelFormat)
                    .value_or(TextureFormat::kRGBA8UNorm),
      .width = static_cast<uint32_t>(mtl.width),
      .height = static_cast<uint32_t>(mtl.height),
  };
  WrappedImage wrapped{
      .image = image,
      .texture = std::make_unique<GPUTextureMTL>(desc, mtl),
  };
  GPUTexture* result = wrapped.texture.get();
  wrapped_images_[image.get()] = std::move(wrapped);
  return result;
}

std::unique_ptr<GPUBuffer> GPUContextMTL::CreateBuffer(const uint8_t* bytes,
                                                       size_t size) {
  id<MTLBuffer> buffer =
      bytes == nullptr
          ? [device_ newBufferWithLength:size
                                 options:MTLResourceStorageModeShared]
          : [device_ newBufferWithBytes:bytes
                                 length:size
                                options:MTLResourceStorageModeShared];
  if (buffer == nil) {
    return nullptr;
  }
  if (bytes == nullptr) {
    std::memset(buffer.contents, 0, size);
  }
  return std::make_unique<GPUBufferMTL>(buffer);
}


struct MetalRenderer::Impl final {
  std::shared_ptr<GPUContextMTL> context;
  std::shared_ptr<PagedAtlas> atlas;
  std::shared_ptr<GradientAtlas> gradients;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  std::unique_ptr<GPUProgramResolverMTL> programs;
  static constexpr size_t kFramesInFlight = 3;

  std::unique_ptr<BufferArena> arena;
  std::unique_ptr<TextureCache> textures;
  uint64_t synced_atlas_revision = 0;
  dispatch_semaphore_t frames_in_flight = nil;

  ShadowLUT shadow_lut;
  /// The LUT's texture, cached for residency and the table.
  id<MTLTexture> shadow_lut_texture = nil;
  id<MTLTexture> gradient_ramps = nil;
  std::shared_ptr<TextMaterializer> materializer;

  uint64_t frame_number = 0;
  uint32_t last_frame_encoded_passes = 0;

  static id<MTLBuffer> Mtl(GPUBuffer* buffer) {
    return static_cast<GPUBufferMTL*>(buffer)->GetBuffer();
  }

  bool SetUp() {
    device = context->GetDevice();
    if (device == nil) {
      return false;
    }
    if (![device supportsFamily:MTLGPUFamilyMetal3]) {
      FML_LOG(ERROR) << "Metal 3 (bindless argument tables) unsupported.";
      return false;
    }
    queue = context->GetCommandQueue();
    frames_in_flight = dispatch_semaphore_create(kFramesInFlight);
    if (!shadow_lut.Attach(*context)) {
      return false;
    }
    shadow_lut_texture =
        static_cast<GPUTextureMTL*>(shadow_lut.GetTexture())->GetTexture();
    shadow_lut_texture.label = @"Propeller shadow gaussian LUT";
    programs = std::make_unique<GPUProgramResolverMTL>(device);


    return programs->IsValid();
  }
};

std::unique_ptr<MetalRenderer> MetalRenderer::Make(
    std::shared_ptr<GPUContextMTL> context,
    std::shared_ptr<PagedAtlas> atlas,
    std::shared_ptr<TextMaterializer> materializer) {
  if (!context) {
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  impl->context = std::move(context);
  impl->atlas = std::move(atlas);
  impl->gradients = std::make_shared<GradientAtlas>(impl->context.get());
  impl->materializer = std::move(materializer);
  if (!impl->SetUp()) {
    return nullptr;
  }
  return std::unique_ptr<MetalRenderer>(new MetalRenderer(std::move(impl)));
}

MetalRenderer::MetalRenderer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MetalRenderer::~MetalRenderer() = default;

id<MTLDevice> MetalRenderer::GetDevice() const {
  return impl_->device;
}

id<MTLCommandBuffer> MetalRenderer::Render(const PrPicture& picture,
                                           id<MTLTexture> target,
                                           Color clear_color) {
  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = std::shared_ptr<PrPicture>(std::shared_ptr<void>(),
                                            const_cast<PrPicture*>(&picture));
  scene.children.push_back(std::move(leaf));
  return Render(scene, target, clear_color);
}

id<MTLCommandBuffer> MetalRenderer::Render(const PrSceneNode& scene,
                                           id<MTLTexture> target,
                                           Color clear_color) {
  if (target == nil) {
    return nil;
  }

  @autoreleasepool {
    TRACE_EVENT0("flutter", "Propeller::RenderScene");
    Impl& impl = *impl_;
    {
      TRACE_EVENT0("flutter", "Propeller::WaitForFrameSlot");
      dispatch_semaphore_wait(impl.frames_in_flight, DISPATCH_TIME_FOREVER);
    }
    impl.frame_number++;

    if (!impl.programs->SetTargetFormat(target.pixelFormat)) {
      dispatch_semaphore_signal(impl.frames_in_flight);
      return nil;
    }
    // The two the dispatcher can name, which are the base content
    // pipelines the resolver already builds.
    ProgramSet programs = {};
    for (size_t program = ProgramType::kColor;
         program < ProgramType::kProgramLength; program++) {
      programs[program] =
          impl.programs->Resolve(static_cast<ProgramType>(program));
    }
    for (size_t program = ProgramType::kColor;
         program < ProgramType::kProgramLength; program++) {
      if (programs[program] == nullptr) {
        dispatch_semaphore_signal(impl.frames_in_flight);
        return nil;
      }
    }
    if (impl.arena == nullptr) {
      impl.arena = std::make_unique<BufferArena>(*impl.context);
    }
    if (impl.textures == nullptr) {
      impl.textures = std::make_unique<TextureCache>(impl.context.get());
    }

    impl.textures->Next();
    const std::optional<TextureFormat> format =
        FromMTLPixelFormat(target.pixelFormat);
    if (!format.has_value()) {
      FML_LOG(ERROR) << "Propeller cannot render to pixel format "
                     << target.pixelFormat << ".";
      dispatch_semaphore_signal(impl.frames_in_flight);
      return nil;
    }
    const TextureDesc desc{
        .format = format.value(),
        .width = static_cast<uint32_t>(target.width),
        .height = static_cast<uint32_t>(target.height),
    };
    GPUTextureMTL wrapped(desc, target);

    id<MTLCommandBuffer> command_buffer = [impl.queue commandBuffer];
    command_buffer.label =
        [NSString stringWithFormat:@"Propeller frame %llu", impl.frame_number];
    {
      GpuCommandBufferMTL gpu_command_buffer(command_buffer);
      if (impl.shadow_lut.HasPendingUpload()) {
        impl.shadow_lut.RecordUpload(gpu_command_buffer);
      }
      Dispatch(scene, *impl.arena, programs, wrapped, *impl.textures,
               impl.materializer.get(), gpu_command_buffer,
               impl.shadow_lut.GetTexture(), impl.gradients.get());
      gpu_command_buffer.EndEncoding();
    }
    // Rows no draw asked for this frame go back to the atlas.
    impl.gradients->EvictUnused();
    impl.arena->Reset();

    dispatch_semaphore_t semaphore = impl.frames_in_flight;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
      dispatch_semaphore_signal(semaphore);
    }];
    [command_buffer commit];
    return command_buffer;
  }
}


}  // namespace impeller
