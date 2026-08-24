// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_METAL_METAL_RENDERER_H_
#define FLUTTER_IMPELLER_PROPELLER_METAL_METAL_RENDERER_H_

#import <Metal/Metal.h>

#include <memory>
#include <unordered_map>

#include "impeller/geometry/color.h"
#include "impeller/propeller/dispatcher.h"
#include "impeller/propeller/paged_atlas.h"
#include "impeller/propeller/renderer/gpu_context.h"
#include "impeller/propeller/text_materializer.h"

namespace impeller {

class GPUTextureMTL final : public GPUTexture {
 public:
  GPUTextureMTL(const TextureDesc& desc, id<MTLTexture> texture);

  ~GPUTextureMTL() override;

  [[nodiscard]] id<MTLTexture> GetTexture() const { return texture_; }

  GPUTextureMTL(const GPUTextureMTL&) = delete;
  GPUTextureMTL(GPUTextureMTL&&) = delete;
  GPUTextureMTL& operator=(const GPUTextureMTL&) = delete;
  GPUTextureMTL& operator=(GPUTextureMTL&&) = delete;

 private:
  const id<MTLTexture> texture_;
};

class GPUBufferMTL final : public GPUBuffer {
 public:
  explicit GPUBufferMTL(id<MTLBuffer> buffer);

  ~GPUBufferMTL() override;

  void Write(const uint8_t* data, uint32_t length, uint32_t offset) override;

  // |GPUBuffer|
  uint8_t* Contents() override;

  // |GPUBuffer|
  void SetLabel(const char* label) override;

  [[nodiscard]] const id<MTLBuffer>& GetBuffer() const { return buffer_; }

  GPUBufferMTL(const GPUBufferMTL&) = delete;
  GPUBufferMTL(GPUBufferMTL&&) = delete;
  GPUBufferMTL& operator=(const GPUBufferMTL&) = delete;
  GPUBufferMTL& operator=(GPUBufferMTL&&) = delete;

 private:
  const id<MTLBuffer> buffer_;
};

class GpuCommandBufferMTL final : public GpuCommandBuffer {
 public:
  explicit GpuCommandBufferMTL(id<MTLCommandBuffer> command_buffer);

  ~GpuCommandBufferMTL() override;

  // |GpuCommandBuffer|
  void UpdateRegion(const GPUTexture& texture,
                    uint32_t x,
                    uint32_t y,
                    uint32_t width,
                    uint32_t height,
                    const GPUBuffer& buffer,
                    uint32_t offset,
                    uint32_t row_bytes) override;

  /// End the blit encoder any UpdateRegion opened. Nothing else can be
  /// encoded onto the command buffer until this runs.
  void EndEncoding();

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

  void DrawLines(int start, int count) override;

  // |GpuCommandBuffer|
  void DrawTrianglesIndexed(const GPUBuffer& indices,
                            int start,
                            int count) override;

  /// The open render encoder, for backend-only calls (bindless residency)
  /// between StartRenderPass and EndRenderPass.
  [[nodiscard]] id<MTLRenderCommandEncoder> GetRenderEncoder() const {
    return render_;
  }

  GpuCommandBufferMTL(const GpuCommandBufferMTL&) = delete;
  GpuCommandBufferMTL& operator=(const GpuCommandBufferMTL&) = delete;

 private:
  void FlushScissor();

  const id<MTLCommandBuffer> command_buffer_;
  id<MTLBlitCommandEncoder> blit_ = nil;
  id<MTLRenderCommandEncoder> render_ = nil;
  IRect current_scissor_ = {};
  std::optional<IRect> pending_scissor_ = {};
};

class GPUContextMTL final : public GPUContext {
 public:
  /// Returns nullptr when no Metal device is available.
  static std::shared_ptr<GPUContextMTL> Make();

  explicit GPUContextMTL(id<MTLDevice> device);

  ~GPUContextMTL() override;

  [[nodiscard]] id<MTLDevice> GetDevice() const { return device_; }

  [[nodiscard]] id<MTLCommandQueue> GetCommandQueue() const { return queue_; }

  // |GPUContext|
  std::unique_ptr<GPUTexture> CreateTexture(const TextureDesc& desc,
                                            bool zeroed) override;

  // |GPUContext|
  std::unique_ptr<GPUBuffer> CreateBuffer(const uint8_t* bytes,
                                          size_t size) override;

  // |GPUContext|
  GPUTexture* GetDLImageTexture(const sk_sp<flutter::DlImage>& image) override;

 private:
  const id<MTLDevice> device_;
  struct WrappedImage {
    sk_sp<flutter::DlImage> image;
    std::unique_ptr<GPUTexture> texture;
  };
  std::unordered_map<const flutter::DlImage*, WrappedImage> wrapped_images_;
  const id<MTLCommandQueue> queue_;
};

//------------------------------------------------------------------------------
/// Draws a propeller scene with Metal, implementing the propeller.md
/// shader design directly.
class MetalRenderer {
 public:
  /// Returns nullptr when no Metal device is available.
  /// The gradient atlas is the renderer's own: it allocates out of the
  /// same context and nothing outside needs to hold it.
  static std::unique_ptr<MetalRenderer> Make(
      std::shared_ptr<GPUContextMTL> context,
      std::shared_ptr<PagedAtlas> atlas,
      std::shared_ptr<TextMaterializer> materializer = nullptr);

  ~MetalRenderer();

  MetalRenderer(const MetalRenderer&) = delete;
  MetalRenderer& operator=(const MetalRenderer&) = delete;

  id<MTLDevice> GetDevice() const;

  /// Encode a propeller scene into `target` and commit.
  id<MTLCommandBuffer> Render(const PrSceneNode& scene,
                              id<MTLTexture> target,
                              Color clear_color);

  /// The same, for a lone picture: a scene of just that picture.
  id<MTLCommandBuffer> Render(const PrPicture& picture,
                              id<MTLTexture> target,
                              Color clear_color);

 private:
  struct Impl;

  explicit MetalRenderer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_METAL_METAL_RENDERER_H_
