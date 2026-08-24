// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_RENDERER_GPU_CONTEXT_H_
#define FLUTTER_IMPELLER_PROPELLER_RENDERER_GPU_CONTEXT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "flutter/display_list/image/dl_image.h"

namespace impeller {

enum class TextureFormat {
  /// Single channel coverage: glyph and path masks.
  kR8UNorm,
  kRGBA8UNorm,
  /// Swapchain order on Android and Windows; identical layout otherwise.
  kBGRA8UNorm,
  /// Signed, unclamped scalar: winding accumulation.
  kR16Float,
  /// A wide gamut drawable, 16 bits a channel carrying a 10 bit extended
  /// range. Apple GPUs only, which is where such a drawable comes from.
  kBGRA10XR,
};

constexpr size_t BytesPerPixel(TextureFormat format) {
  switch (format) {
    case TextureFormat::kR8UNorm:
      return 1;
    case TextureFormat::kRGBA8UNorm:
    case TextureFormat::kBGRA8UNorm:
      return 4;
    case TextureFormat::kR16Float:
      return 2;
    case TextureFormat::kBGRA10XR:
      return 8;
  }
}

struct TextureDesc {
  TextureFormat format = TextureFormat::kRGBA8UNorm;
  uint32_t width = 0;
  uint32_t height = 0;
  bool transient = false;
};

class GPUTexture {
 public:
  virtual ~GPUTexture() = 0;

  const TextureDesc& GetDesc() const { return desc_; }

  GPUTexture(const GPUTexture&) = delete;
  GPUTexture(GPUTexture&&) = delete;
  GPUTexture& operator=(const GPUTexture&) = delete;
  GPUTexture& operator=(GPUTexture&&) = delete;

 protected:
  explicit GPUTexture(const TextureDesc& desc) : desc_(desc) {}

 private:
  const TextureDesc desc_;
};

inline GPUTexture::~GPUTexture() = default;

class GPUBuffer {
 public:
  virtual ~GPUBuffer() = 0;

  virtual void Write(const uint8_t* data, uint32_t length, uint32_t offset) = 0;

  /// The buffer's host-visible mapping, valid for its whole lifetime.
  virtual uint8_t* Contents() = 0;

  /// Name shown in GPU captures; backends without labels ignore it.
  virtual void SetLabel(const char* label) {}

  GPUBuffer(const GPUBuffer&) = delete;
  GPUBuffer(GPUBuffer&&) = delete;
  GPUBuffer& operator=(const GPUBuffer&) = delete;
  GPUBuffer& operator=(GPUBuffer&&) = delete;

 protected:
  GPUBuffer() = default;
};

inline GPUBuffer::~GPUBuffer() = default;

struct GPUBufferView {
  GPUBuffer* buffer;
  uint32_t offset;
  uint32_t length;
};

class GPUProgram {
 public:
  virtual ~GPUProgram() = default;
};

/// The pipelines a frame can draw with. Every draw type maps to one of
/// these, and a backend builds exactly this many.
enum ProgramType {
  /// Nothing bound yet, what a draw type nothing draws yet maps to, and
  /// what a scissor draw carries, since it binds no pipeline.
  kInvalid = 0,
  /// Flat coverage: the geometry is the shape.
  kColor = 1,
  /// Loop-Blinn coverage: the fragment stage reads the implicit the
  /// vertex stage carried in the paint word.
  kPath = 2,
  /// Signed Loop-Blinn coverage summed into the winding accumulator,
  /// with the colour attachment masked off.
  kWindingAccumulate = 3,
  /// The same, for a mesh that carries its edge ramp in its vertex
  /// colour rather than in an implicit.
  kWindingAccumulateFlat = 10,
  /// What the accumulator came to, under the fill rule, written in the
  /// draw's colour -- and zeroed as it goes.
  kWindingResolveNonZero = 4,
  kWindingResolveEvenOdd = 5,
  /// Gradient ramps into the ramp atlas: vertex position and colour,
  /// and nothing else. Renders to one attachment of its own rather
  /// than to a frame's target.
  kGradientRamp = 6,
  /// What a clip shape accumulated, under the fill rule, multiplied
  /// into the pass's clip attachment -- and zeroed as it goes.
  kClipResolveNonZero = 7,
  kClipResolveEvenOdd = 8,
  /// Back to fully visible, where a clip was popped.
  kClipReset = 9,
  /// One axis of a separable Gaussian, sampling what the pass before it
  /// resolved into. Two of these in a row are a blur.
  kBlur = 11,
  kProgramLength = 12,
};

using ProgramSet = std::array<const GPUProgram*, ProgramType::kProgramLength>;

/// Where a backend's pipelines come from. Resolution is by program
/// alone: a frame's draws differ in what they bind, not in how they are
/// compiled.
class GPUProgramResolver {
 public:
  virtual ~GPUProgramResolver() = default;

  virtual const GPUProgram* Resolve(ProgramType type) = 0;
};

enum class GPUShaderStage {
  kVertex,
  kFragment,
};

struct RenderPassDesc {
  GPUTexture* target = nullptr;
  float clear_color[4] = {0, 0, 0, 0};
  bool load = false;
  struct Transient {
    GPUTexture* texture = nullptr;
    float clear = 0;
  };
  const Transient* transients = nullptr;
  size_t transient_count = 0;
  /// Shown in GPU captures.
  const char* label = "";
};

class GpuCommandBuffer {
 public:
  virtual ~GpuCommandBuffer() = default;

  // Update the region of the given texture from the given buffer.
  //
  // [row_bytes] is the stride of the source image within the buffer, so a
  // sub-rect can be copied without first being packed down.
  virtual void UpdateRegion(const GPUTexture&,
                            uint32_t x,
                            uint32_t y,
                            uint32_t width,
                            uint32_t height,
                            const GPUBuffer& buffer,
                            uint32_t offset,
                            uint32_t row_bytes) = 0;

  virtual void StartRenderPass(const RenderPassDesc& desc) = 0;

  virtual void EndRenderPass() = 0;

  virtual void SetRenderPipeline(const GPUProgram& program) = 0;

  virtual void SetScissorRect(int x, int y, int w, int h) = 0;

  /// Bind a buffer range to a shader-stage slot.
  virtual void SetBuffer(GPUShaderStage stage,
                         const GPUBuffer& buffer,
                         size_t offset,
                         int index) = 0;

  /// Bind the textures a pass samples, as the table the fragment stage
  /// indexes. A paint names one by its position here, so the table is
  /// whatever the pass needed, in the order it needed it.
  ///
  /// Per pass: what a pass samples is decided when it is encoded, and
  /// nothing carries between passes.
  virtual void SetTextureTable(GPUTexture* const* textures, size_t count) = 0;

  /// Bind a small amount of data to a shader-stage slot without a buffer.
  virtual void SetConstantData(GPUShaderStage stage,
                               const void* data,
                               size_t bytes,
                               int index) = 0;

  virtual void DrawTriangles(int start, int count) = 0;

  /// Draw `count` vertices as a connected line strip. A gradient ramp
  /// is a row of texels running through its stops, so it is one vertex
  /// per stop and nothing else.
  virtual void DrawLines(int start, int count) = 0;

  /// Draw through an index buffer of 16 bit indices, `start` of them in,
  /// where an index names a vertex of the streams currently bound.
  virtual void DrawTrianglesIndexed(const GPUBuffer& indices,
                                    int start,
                                    int count) = 0;
};

class GPUContext {
 public:
  virtual ~GPUContext() = default;

  // Create a new texture.
  //
  // Contents are undefined unless [zeroed], which costs a full clear of the
  // texture before it is returned.
  virtual std::unique_ptr<GPUTexture> CreateTexture(const TextureDesc& desc,
                                                    bool zeroed) = 0;

  // Create a new buffer from the given sized data.
  //
  // If [nullptr] is provided, a zeroed buffer is created.
  virtual std::unique_ptr<GPUBuffer> CreateBuffer(const uint8_t* bytes,
                                                  size_t size) = 0;

  /// Lookup the GPUTexture for a given DlImage.
  virtual GPUTexture* GetDLImageTexture(const sk_sp<flutter::DlImage>& image) {
    return nullptr;
  }

  /// The offset granularity for binding a buffer to a shader stage.
  [[nodiscard]] virtual size_t GetBufferAlignment() const { return 16; }

  GPUContext(const GPUContext&) = delete;
  GPUContext(GPUContext&&) = delete;
  GPUContext& operator=(const GPUContext&) = delete;
  GPUContext& operator=(GPUContext&&) = delete;

 protected:
  GPUContext() = default;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_RENDERER_GPU_CONTEXT_H_
