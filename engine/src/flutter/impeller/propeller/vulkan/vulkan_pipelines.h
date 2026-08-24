// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_PIPELINES_H_
#define FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_PIPELINES_H_

#include <memory>
#include <unordered_map>

#include "impeller/propeller/render_loop.h"
#include "impeller/propeller/vulkan/vulkan_context.h"

namespace impeller {

/// Bindless texture-table capacity, sized to cover the pass and image
/// slot ranges in mlr.h.
constexpr uint32_t kVKTextureTableSlots = 4096;

class GPUProgramVK final : public GPUProgram {
 public:
  GPUProgramVK(vk::UniquePipeline pipeline, bool fetches)
      : pipeline_(std::move(pipeline)), fetches_(fetches) {}

  ~GPUProgramVK() override = default;

  [[nodiscard]] vk::Pipeline GetPipeline() const { return pipeline_.get(); }

  /// Reads attachments through subpass inputs: earlier writes to this
  /// pixel must be ordered ahead of it (a by-region barrier, or nothing
  /// on rasterization-order hardware).
  [[nodiscard]] bool Fetches() const { return fetches_; }

 private:
  vk::UniquePipeline pipeline_;
  bool fetches_ = false;
};

/// The canvas-mode pipeline catalog over the SPIR-V ubershaders: the
/// same programs the Metal catalog carries, with the virtual canvases as
/// real transient attachments read through subpass inputs.
class GPUProgramResolverVK final : public GPUProgramResolver {
 public:
  explicit GPUProgramResolverVK(std::shared_ptr<VulkanCore> core);

  ~GPUProgramResolverVK();

  [[nodiscard]] bool IsValid() const { return valid_; }

  /// Point Resolve at the catalog for `format`, building the
  /// precompiled set on first use. False when pipeline creation fails.
  bool SetTargetFormat(TextureFormat format);

  // |GPUProgramResolver|
  const GPUProgram* Resolve(const ProgramKey& key) override;

  [[nodiscard]] vk::PipelineLayout GetPipelineLayout() const {
    return pipeline_layout_.get();
  }

  [[nodiscard]] vk::DescriptorSetLayout GetBuffersSetLayout() const {
    return set_layouts_[0].get();
  }

  [[nodiscard]] vk::DescriptorSetLayout GetTableSetLayout() const {
    return set_layouts_[1].get();
  }

  [[nodiscard]] vk::DescriptorSetLayout GetInputsSetLayout() const {
    return set_layouts_[2].get();
  }

  GPUProgramResolverVK(const GPUProgramResolverVK&) = delete;
  GPUProgramResolverVK& operator=(const GPUProgramResolverVK&) = delete;

 private:
  /// Every fragment shader's view of one attachment for one pipeline.
  enum class AttachmentMode {
    kMasked,
    kDirectWrite,
    kSrcOver,
    kAdditive,
    /// Porter-Duff factors from the run's blend mode.
    kFactors,
  };

  struct SpecValues {
    VkBool32 is_path = VK_FALSE;
    int32_t fetch_dest = 0;
    int32_t fetch_blend = 0;
    int32_t fill_rule = 0;
    int32_t filter_blend = 0;
  };

  /// Pipelines are specialized per render-target pixel format on first
  /// use (offscreen RGBA8, swapchain BGRA8, ...).
  struct PipelineSet {
    std::unique_ptr<GPUProgramVK> content[kTechniqueCount]
                                         [kMLRMaxCanvasDepth + 1];
    std::unordered_map<uint32_t, std::unique_ptr<GPUProgramVK>>
        blend_variants;
    std::unique_ptr<GPUProgramVK> blur[kMLRMaxCanvasDepth + 1];
    std::unordered_map<uint32_t, std::unique_ptr<GPUProgramVK>>
        color_filter_variants;
    std::unordered_map<uint32_t, std::unique_ptr<GPUProgramVK>>
        composite_blend_variants;
    std::unique_ptr<GPUProgramVK> composite[kTechniqueCount]
                                           [kMLRMaxCanvasDepth];
    std::unique_ptr<GPUProgramVK> reset[kMLRWindingCanvas];
    std::unique_ptr<GPUProgramVK> winding;
    std::unique_ptr<GPUProgramVK> winding_composite[2]
                                                   [kMLRMaxCanvasDepth + 1];
    std::unique_ptr<GPUProgramVK> winding_canvas_composite
        [2][kMLRMaxCanvasDepth + 1];
  };

  /// A color-filter pipeline, specialized per blend mode.
  enum ColorFilterTier {
    kCanvasFetch = 0,
    kTextureSample = 1,
    kBackdrop = 2,
  };

  PipelineSet* GetPipelines(vk::Format format);

  const GPUProgram* GetContentPipeline(size_t technique,
                                       size_t dest,
                                       flutter::DlBlendMode blend);

  const GPUProgram* GetCompositeBlendPipeline(size_t technique,
                                              size_t source,
                                              flutter::DlBlendMode blend);

  const GPUProgram* GetColorFilterPipeline(ColorFilterTier tier,
                                           size_t technique,
                                           size_t index,
                                           flutter::DlBlendMode blend);

  std::unique_ptr<GPUProgramVK> MakePipeline(
      vk::Format format,
      vk::ShaderModule fragment,
      const SpecValues& specs,
      const AttachmentMode (&modes)[kMLRWindingCanvas + 1],
      flutter::DlBlendMode factors_blend,
      bool fetches,
      const char* label);

  std::shared_ptr<VulkanCore> core_;
  bool valid_ = false;

  vk::UniqueShaderModule vertex_module_;
  vk::UniqueShaderModule fragment_module_;
  vk::UniqueShaderModule fetch_blend_module_;
  vk::UniqueShaderModule composite_module_;
  vk::UniqueShaderModule composite_blend_module_;
  vk::UniqueShaderModule composite_filter_module_;
  vk::UniqueShaderModule winding_module_;
  vk::UniqueShaderModule winding_composite_module_;
  vk::UniqueShaderModule winding_canvas_composite_module_;
  vk::UniqueShaderModule blur_module_;
  vk::UniqueShaderModule backdrop_module_;
  vk::UniqueShaderModule texture_filter_module_;
  vk::UniqueSampler nearest_sampler_;
  vk::UniqueSampler linear_sampler_;
  vk::UniqueDescriptorSetLayout set_layouts_[3];
  vk::UniquePipelineLayout pipeline_layout_;

  std::unordered_map<uint64_t, PipelineSet> pipelines_;
  /// The catalog EncodeRuns is currently resolving against.
  PipelineSet* current_pipelines_ = nullptr;
  vk::Format current_format_ = vk::Format::eR8G8B8A8Unorm;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_VULKAN_VULKAN_PIPELINES_H_
