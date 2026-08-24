// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_RENDERER_STUB_GPU_CONTEXT_H_
#define FLUTTER_IMPELLER_PROPELLER_RENDERER_STUB_GPU_CONTEXT_H_

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {
namespace testing {

/// A GPUContext for tests with no device: buffers are plain memory and
/// textures hold nothing, so anything written can be read back but nothing
/// can be sampled.
class StubGpuTexture final : public GPUTexture {
 public:
  explicit StubGpuTexture(const TextureDesc& desc) : GPUTexture(desc) {}
};

class StubGpuBuffer final : public GPUBuffer {
 public:
  explicit StubGpuBuffer(size_t size) : data_(size, 0) {}

  void Write(const uint8_t* data, uint32_t length, uint32_t offset) override {
    if (offset + length > data_.size()) {
      return;
    }
    std::memcpy(data_.data() + offset, data, length);
  }

  uint8_t* Contents() override { return data_.data(); }

  const std::vector<uint8_t>& GetData() const { return data_; }

 private:
  std::vector<uint8_t> data_;
};

class StubGpuContext final : public GPUContext {
 public:
  std::unique_ptr<GPUTexture> CreateTexture(const TextureDesc& desc,
                                            bool zeroed) override {
    return std::make_unique<StubGpuTexture>(desc);
  }

  /// Any image resolves, to one texture per image: what the tests care
  /// about is which slot a draw names, not what is in it.
  GPUTexture* GetDLImageTexture(const sk_sp<flutter::DlImage>& image) override {
    if (!image) {
      return nullptr;
    }
    std::unique_ptr<GPUTexture>& texture = images[image.get()];
    if (texture == nullptr) {
      texture = std::make_unique<StubGpuTexture>(TextureDesc{
          .format = TextureFormat::kRGBA8UNorm,
          .width = static_cast<uint32_t>(image->GetSize().width),
          .height = static_cast<uint32_t>(image->GetSize().height),
      });
    }
    return texture.get();
  }

  std::unordered_map<const flutter::DlImage*, std::unique_ptr<GPUTexture>>
      images;

  std::unique_ptr<GPUBuffer> CreateBuffer(const uint8_t* bytes,
                                          size_t size) override {
    auto buffer = std::make_unique<StubGpuBuffer>(size);
    if (bytes != nullptr) {
      buffer->Write(bytes, size, 0);
    }
    return buffer;
  }
};

/// Records the regions an upload would copy.
class StubGpuCommandBuffer final : public GpuCommandBuffer {
 public:
  struct Update {
    const GPUTexture* texture;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t offset;
    uint32_t row_bytes;
  };

  void UpdateRegion(const GPUTexture& texture,
                    uint32_t x,
                    uint32_t y,
                    uint32_t width,
                    uint32_t height,
                    const GPUBuffer& buffer,
                    uint32_t offset,
                    uint32_t row_bytes) override {
    updates.push_back(Update{&texture, x, y, width, height, offset, row_bytes});
  }
  void StartRenderPass(const RenderPassDesc& desc) override {
    passes++;
    pass_targets.push_back(desc.target);
    for (int i = 0; i < 4; i++) {
      clear_color[i] = desc.clear_color[i];
    }
  }
  void EndRenderPass() override {}
  void SetRenderPipeline(const GPUProgram& program) override {
    pipelines.push_back(&program);
  }
  void SetScissorRect(int x, int y, int w, int h) override {}
  void SetBuffer(GPUShaderStage stage,
                 const GPUBuffer& buffer,
                 size_t offset,
                 int index) override {
    binds.push_back(Bind{stage, &buffer, offset, index});
  }
  void SetTextureTable(GPUTexture* const* textures, size_t count) override {
    texture_tables.emplace_back(textures, textures + count);
  }
  void SetConstantData(GPUShaderStage stage,
                       const void* data,
                       size_t bytes,
                       int index) override {
    if (stage == GPUShaderStage::kFragment && index == 3 &&
        bytes == sizeof(float)) {
      std::memcpy(&last_opacity, data, sizeof(float));
    }
    if (stage == GPUShaderStage::kVertex && index == 3 &&
        bytes == sizeof(viewport_origin)) {
      std::memcpy(viewport_origin, data, sizeof(viewport_origin));
    }
  }
  void DrawTriangles(int start, int count) override {
    draws.push_back(Draw{start, count, last_opacity});
  }
  void DrawLines(int start, int count) override {
    lines.push_back(Draw{start, count, last_opacity});
  }
  void DrawTrianglesIndexed(const GPUBuffer& indices,
                            int start,
                            int count) override {
    draws.push_back(Draw{start, count, last_opacity, &indices});
  }

  /// One recorded draw, with the opacity constant in effect when it was
  /// issued.
  struct Draw {
    int start = 0;
    int count = 0;
    float opacity = 1;
    /// The index buffer the draw read, or null if it was not indexed.
    const GPUBuffer* indices = nullptr;
  };

  /// One recorded buffer binding.
  struct Bind {
    GPUShaderStage stage;
    const GPUBuffer* buffer;
    size_t offset;
    int index;
  };

  std::vector<Update> updates;
  std::vector<Draw> draws;
  /// Line draws, which is how a gradient ramp is rasterized.
  std::vector<Draw> lines;
  std::vector<const GPUProgram*> pipelines;
  std::vector<Bind> binds;
  /// One entry per SetTextureTable call: what that pass could sample.
  std::vector<std::vector<GPUTexture*>> texture_tables;
  int passes = 0;
  /// What each pass rendered into, in the order they were started.
  std::vector<GPUTexture*> pass_targets;
  float clear_color[4] = {0, 0, 0, 0};
  float last_opacity = 1;
  /// The pass size in [0..1] and where it starts in [2..3].
  float viewport_origin[4] = {0, 0, 0, 0};
};

}  // namespace testing
}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_RENDERER_STUB_GPU_CONTEXT_H_
