// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/vulkan/vulkan_pipelines.h"

#include <cstring>

#include "flutter/fml/logging.h"
#include "impeller/propeller/mlr.h"
#include "impeller/propeller/vkspv/backdrop_filter.h"
#include "impeller/propeller/vkspv/blur.h"
#include "impeller/propeller/vkspv/composite.h"
#include "impeller/propeller/vkspv/composite_blend.h"
#include "impeller/propeller/vkspv/composite_filter.h"
#include "impeller/propeller/vkspv/fragment.h"
#include "impeller/propeller/vkspv/fragment_fetch_blend.h"
#include "impeller/propeller/vkspv/texture_color_filter.h"
#include "impeller/propeller/vkspv/vertex.h"
#include "impeller/propeller/vkspv/winding.h"
#include "impeller/propeller/vkspv/winding_canvas_composite.h"
#include "impeller/propeller/vkspv/winding_composite.h"

namespace impeller {

namespace {

/// The push block shared by every pipeline: the 32-byte head plus the
/// 96-byte filter blob, exactly the 128-byte device floor.
constexpr uint32_t kPushConstantBytes = 128;

vk::UniqueShaderModule MakeModule(const vk::Device& device,
                                  const unsigned char* data,
                                  size_t length) {
  // The embedded arrays are byte-aligned; SPIR-V words are not.
  std::vector<uint32_t> words((length + 3) / 4);
  std::memcpy(words.data(), data, length);
  vk::ShaderModuleCreateInfo info;
  info.codeSize = length;
  info.pCode = words.data();
  auto [result, module] = device.createShaderModuleUnique(info);
  if (result != vk::Result::eSuccess) {
    FML_LOG(ERROR) << "Shader module creation failed: "
                   << vk::to_string(result);
  }
  return std::move(module);
}

struct BlendFactors {
  vk::BlendFactor src_rgb;
  vk::BlendFactor dst_rgb;
  vk::BlendFactor src_alpha;
  vk::BlendFactor dst_alpha;
};

/// Fixed-function factors for the simple Porter-Duff modes over
/// premultiplied color; the same table as the Metal catalog.
BlendFactors FactorsFor(flutter::DlBlendMode mode) {
  switch (mode) {
    case flutter::DlBlendMode::kClear:
      return {vk::BlendFactor::eZero, vk::BlendFactor::eZero,
              vk::BlendFactor::eZero, vk::BlendFactor::eZero};
    case flutter::DlBlendMode::kSrc:
      return {vk::BlendFactor::eOne, vk::BlendFactor::eZero,
              vk::BlendFactor::eOne, vk::BlendFactor::eZero};
    case flutter::DlBlendMode::kDst:
      return {vk::BlendFactor::eZero, vk::BlendFactor::eOne,
              vk::BlendFactor::eZero, vk::BlendFactor::eOne};
    case flutter::DlBlendMode::kSrcOver:
      return {vk::BlendFactor::eOne, vk::BlendFactor::eOneMinusSrcAlpha,
              vk::BlendFactor::eOne, vk::BlendFactor::eOneMinusSrcAlpha};
    case flutter::DlBlendMode::kDstOver:
      return {vk::BlendFactor::eOneMinusDstAlpha, vk::BlendFactor::eOne,
              vk::BlendFactor::eOneMinusDstAlpha, vk::BlendFactor::eOne};
    case flutter::DlBlendMode::kSrcIn:
      return {vk::BlendFactor::eDstAlpha, vk::BlendFactor::eZero,
              vk::BlendFactor::eDstAlpha, vk::BlendFactor::eZero};
    case flutter::DlBlendMode::kDstIn:
      return {vk::BlendFactor::eZero, vk::BlendFactor::eSrcAlpha,
              vk::BlendFactor::eZero, vk::BlendFactor::eSrcAlpha};
    case flutter::DlBlendMode::kSrcOut:
      return {vk::BlendFactor::eOneMinusDstAlpha, vk::BlendFactor::eZero,
              vk::BlendFactor::eOneMinusDstAlpha, vk::BlendFactor::eZero};
    case flutter::DlBlendMode::kDstOut:
      return {vk::BlendFactor::eZero, vk::BlendFactor::eOneMinusSrcAlpha,
              vk::BlendFactor::eZero, vk::BlendFactor::eOneMinusSrcAlpha};
    case flutter::DlBlendMode::kSrcATop:
      return {vk::BlendFactor::eDstAlpha, vk::BlendFactor::eOneMinusSrcAlpha,
              vk::BlendFactor::eDstAlpha, vk::BlendFactor::eOneMinusSrcAlpha};
    case flutter::DlBlendMode::kDstATop:
      return {vk::BlendFactor::eOneMinusDstAlpha, vk::BlendFactor::eSrcAlpha,
              vk::BlendFactor::eOneMinusDstAlpha, vk::BlendFactor::eSrcAlpha};
    case flutter::DlBlendMode::kXor:
      return {vk::BlendFactor::eOneMinusDstAlpha,
              vk::BlendFactor::eOneMinusSrcAlpha,
              vk::BlendFactor::eOneMinusDstAlpha,
              vk::BlendFactor::eOneMinusSrcAlpha};
    case flutter::DlBlendMode::kPlus:
      return {vk::BlendFactor::eOne, vk::BlendFactor::eOne,
              vk::BlendFactor::eOne, vk::BlendFactor::eOne};
    case flutter::DlBlendMode::kModulate:
      return {vk::BlendFactor::eDstColor, vk::BlendFactor::eZero,
              vk::BlendFactor::eDstAlpha, vk::BlendFactor::eZero};
    case flutter::DlBlendMode::kScreen:
      return {vk::BlendFactor::eOne, vk::BlendFactor::eOneMinusSrcColor,
              vk::BlendFactor::eOne, vk::BlendFactor::eOneMinusSrcAlpha};
    default:
      return {vk::BlendFactor::eOne, vk::BlendFactor::eOneMinusSrcAlpha,
              vk::BlendFactor::eOne, vk::BlendFactor::eOneMinusSrcAlpha};
  }
}

// Modes whose fixed-function dst factor scales with source alpha
// compose correctly with coverage-premultiplied sources.
bool IsCoverageSafeBlend(flutter::DlBlendMode mode) {
  switch (mode) {
    case flutter::DlBlendMode::kSrcOver:
    case flutter::DlBlendMode::kDstOver:
    case flutter::DlBlendMode::kDstOut:
    case flutter::DlBlendMode::kSrcATop:
    case flutter::DlBlendMode::kXor:
    case flutter::DlBlendMode::kPlus:
    case flutter::DlBlendMode::kScreen:
    case flutter::DlBlendMode::kDst:  // Never scheduled.
      return true;
    default:
      return false;
  }
}

}  // namespace

GPUProgramResolverVK::GPUProgramResolverVK(std::shared_ptr<VulkanCore> core)
    : core_(std::move(core)) {
  if (!core_->bindless_supported) {
    FML_LOG(ERROR) << "Device lacks bindless descriptor indexing.";
    return;
  }
  const vk::Device device = core_->device.get();
#define PROPELLER_MODULE(member, symbol)                                \
  member = MakeModule(device, impeller_propeller_vkspv_##symbol##_data, \
                      impeller_propeller_vkspv_##symbol##_length);      \
  if (!member) {                                                        \
    return;                                                             \
  }
  PROPELLER_MODULE(vertex_module_, vertex)
  PROPELLER_MODULE(fragment_module_, fragment)
  PROPELLER_MODULE(fetch_blend_module_, fragment_fetch_blend)
  PROPELLER_MODULE(composite_module_, composite)
  PROPELLER_MODULE(composite_blend_module_, composite_blend)
  PROPELLER_MODULE(composite_filter_module_, composite_filter)
  PROPELLER_MODULE(winding_module_, winding)
  PROPELLER_MODULE(winding_composite_module_, winding_composite)
  PROPELLER_MODULE(winding_canvas_composite_module_, winding_canvas_composite)
  PROPELLER_MODULE(blur_module_, blur)
  PROPELLER_MODULE(backdrop_module_, backdrop_filter)
  PROPELLER_MODULE(texture_filter_module_, texture_color_filter)
#undef PROPELLER_MODULE

  auto make_sampler = [&](vk::Filter filter) {
    vk::SamplerCreateInfo info;
    info.magFilter = filter;
    info.minFilter = filter;
    info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    auto [result, sampler] = device.createSamplerUnique(info);
    FML_CHECK(result == vk::Result::eSuccess);
    return std::move(sampler);
  };
  nearest_sampler_ = make_sampler(vk::Filter::eNearest);
  linear_sampler_ = make_sampler(vk::Filter::eLinear);

  {
    const vk::Sampler nearest = nearest_sampler_.get();
    const vk::Sampler linear = linear_sampler_.get();
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    for (uint32_t binding : {0u, 1u, 2u, 4u, 5u, 6u}) {
      vk::DescriptorSetLayoutBinding buffer_binding;
      buffer_binding.binding = binding;
      buffer_binding.descriptorType = vk::DescriptorType::eStorageBuffer;
      buffer_binding.descriptorCount = 1;
      buffer_binding.stageFlags = vk::ShaderStageFlagBits::eVertex |
                                  vk::ShaderStageFlagBits::eFragment;
      bindings.push_back(buffer_binding);
    }
    vk::DescriptorSetLayoutBinding sampler_binding;
    sampler_binding.descriptorType = vk::DescriptorType::eSampler;
    sampler_binding.descriptorCount = 1;
    sampler_binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
    sampler_binding.binding = 3;
    sampler_binding.pImmutableSamplers = &nearest;
    bindings.push_back(sampler_binding);
    sampler_binding.binding = 7;
    sampler_binding.pImmutableSamplers = &linear;
    bindings.push_back(sampler_binding);

    vk::DescriptorSetLayoutCreateInfo info;
    info.setBindings(bindings);
    auto [result, layout] = device.createDescriptorSetLayoutUnique(info);
    FML_CHECK(result == vk::Result::eSuccess);
    set_layouts_[0] = std::move(layout);
  }
  {
    vk::DescriptorSetLayoutBinding table_binding;
    table_binding.binding = 0;
    table_binding.descriptorType = vk::DescriptorType::eSampledImage;
    table_binding.descriptorCount = kVKTextureTableSlots;
    table_binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
    const vk::DescriptorBindingFlags binding_flags =
        vk::DescriptorBindingFlagBits::ePartiallyBound |
        vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info;
    flags_info.bindingCount = 1;
    flags_info.pBindingFlags = &binding_flags;
    vk::DescriptorSetLayoutCreateInfo info;
    info.pNext = &flags_info;
    info.flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
    info.bindingCount = 1;
    info.pBindings = &table_binding;
    auto [result, layout] = device.createDescriptorSetLayoutUnique(info);
    FML_CHECK(result == vk::Result::eSuccess);
    set_layouts_[1] = std::move(layout);
  }
  {
    // Set 2: the three attachments as subpass inputs (in_d0/in_d1/in_w).
    vk::DescriptorSetLayoutBinding input_bindings[3];
    for (uint32_t i = 0; i < 3; i++) {
      input_bindings[i].binding = i;
      input_bindings[i].descriptorType = vk::DescriptorType::eInputAttachment;
      input_bindings[i].descriptorCount = 1;
      input_bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }
    vk::DescriptorSetLayoutCreateInfo info;
    info.bindingCount = 3;
    info.pBindings = input_bindings;
    auto [result, layout] = device.createDescriptorSetLayoutUnique(info);
    FML_CHECK(result == vk::Result::eSuccess);
    set_layouts_[2] = std::move(layout);
  }

  const vk::DescriptorSetLayout raw_layouts[3] = {
      set_layouts_[0].get(), set_layouts_[1].get(), set_layouts_[2].get()};
  vk::PushConstantRange push_range;
  push_range.stageFlags =
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
  push_range.size = kPushConstantBytes;
  vk::PipelineLayoutCreateInfo layout_info;
  layout_info.setLayoutCount = 3;
  layout_info.pSetLayouts = raw_layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  auto [layout_result, pipeline_layout] =
      device.createPipelineLayoutUnique(layout_info);
  FML_CHECK(layout_result == vk::Result::eSuccess);
  pipeline_layout_ = std::move(pipeline_layout);

  // The precompiled pass-texture catalog; failure here means nothing
  // will ever render. Other target formats specialize on first use.
  valid_ = GetPipelines(vk::Format::eR8G8B8A8Unorm) != nullptr;
}

GPUProgramResolverVK::~GPUProgramResolverVK() = default;

bool GPUProgramResolverVK::SetTargetFormat(TextureFormat format) {
  PipelineSet* set = GetPipelines(MLRVkFormat(format));
  if (set == nullptr) {
    return false;
  }
  current_pipelines_ = set;
  current_format_ = MLRVkFormat(format);
  return true;
}

std::unique_ptr<GPUProgramVK> GPUProgramResolverVK::MakePipeline(
    vk::Format format,
    vk::ShaderModule fragment,
    const SpecValues& specs,
    const AttachmentMode (&modes)[kMLRWindingCanvas + 1],
    flutter::DlBlendMode factors_blend,
    bool fetches,
    const char* label) {
  // Spec ids from common.glsl. Entries for constants a stage does not
  // declare are ignored, so every pipeline supplies the full block.
  const vk::SpecializationMapEntry entries[5] = {
      {0, offsetof(SpecValues, is_path), sizeof(VkBool32)},
      {1, offsetof(SpecValues, fetch_dest), sizeof(int32_t)},
      {2, offsetof(SpecValues, fetch_blend), sizeof(int32_t)},
      {3, offsetof(SpecValues, fill_rule), sizeof(int32_t)},
      {4, offsetof(SpecValues, filter_blend), sizeof(int32_t)},
  };
  vk::SpecializationInfo spec_info;
  spec_info.mapEntryCount = 5;
  spec_info.pMapEntries = entries;
  spec_info.dataSize = sizeof(SpecValues);
  spec_info.pData = &specs;

  vk::PipelineShaderStageCreateInfo stages[2];
  stages[0].stage = vk::ShaderStageFlagBits::eVertex;
  stages[0].module = vertex_module_.get();
  stages[0].pName = "main";
  stages[0].pSpecializationInfo = &spec_info;
  stages[1].stage = vk::ShaderStageFlagBits::eFragment;
  stages[1].module = fragment;
  stages[1].pName = "main";
  stages[1].pSpecializationInfo = &spec_info;

  vk::PipelineVertexInputStateCreateInfo vertex_input;
  vk::PipelineInputAssemblyStateCreateInfo assembly;
  assembly.topology = vk::PrimitiveTopology::eTriangleList;
  vk::PipelineViewportStateCreateInfo viewport;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  vk::PipelineRasterizationStateCreateInfo rasterization;
  rasterization.cullMode = vk::CullModeFlagBits::eNone;
  rasterization.lineWidth = 1.0f;
  vk::PipelineMultisampleStateCreateInfo multisample;

  vk::PipelineColorBlendAttachmentState attachments[kMLRWindingCanvas + 1];
  for (size_t i = 0; i <= kMLRWindingCanvas; i++) {
    vk::PipelineColorBlendAttachmentState& attachment = attachments[i];
    if (modes[i] == AttachmentMode::kMasked) {
      attachment.colorWriteMask = {};
      continue;
    }
    attachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    if (modes[i] == AttachmentMode::kDirectWrite) {
      continue;
    }
    attachment.blendEnable = VK_TRUE;
    if (modes[i] == AttachmentMode::kSrcOver) {
      attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
      attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
      attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
      attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    } else if (modes[i] == AttachmentMode::kAdditive) {
      attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
      attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
      attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
      attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    } else {  // kFactors
      const BlendFactors factors = FactorsFor(factors_blend);
      attachment.srcColorBlendFactor = factors.src_rgb;
      attachment.dstColorBlendFactor = factors.dst_rgb;
      attachment.srcAlphaBlendFactor = factors.src_alpha;
      attachment.dstAlphaBlendFactor = factors.dst_alpha;
    }
  }
  vk::PipelineColorBlendStateCreateInfo blend_state;
  blend_state.attachmentCount = kMLRWindingCanvas + 1;
  blend_state.pAttachments = attachments;

  const vk::DynamicState dynamic_states[2] = {vk::DynamicState::eViewport,
                                              vk::DynamicState::eScissor};
  vk::PipelineDynamicStateCreateInfo dynamic;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  vk::GraphicsPipelineCreateInfo info;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &rasterization;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend_state;
  info.pDynamicState = &dynamic;
  info.layout = pipeline_layout_.get();
  info.renderPass = core_->GetRenderPass(format, false);
  auto [result, pipeline] =
      core_->device->createGraphicsPipelineUnique(nullptr, info);
  if (result != vk::Result::eSuccess) {
    FML_LOG(ERROR) << "Pipeline creation failed (" << label
                   << "): " << vk::to_string(result);
    return nullptr;
  }
  return std::make_unique<GPUProgramVK>(std::move(pipeline), fetches);
}

GPUProgramResolverVK::PipelineSet* GPUProgramResolverVK::GetPipelines(
    vk::Format format) {
  auto found = pipelines_.find(static_cast<uint64_t>(format));
  if (found != pipelines_.end()) {
    return &found->second;
  }
  using Mode = AttachmentMode;
  PipelineSet set;
  for (size_t technique = 0; technique < kTechniqueCount; technique++) {
    SpecValues specs;
    specs.is_path = technique == kPathTechnique ? VK_TRUE : VK_FALSE;
    // Content: one per destination attachment.
    for (size_t dest = 0; dest <= kMLRMaxCanvasDepth; dest++) {
      Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                           Mode::kMasked};
      modes[dest] = Mode::kSrcOver;
      set.content[technique][dest] = MakePipeline(
          format, fragment_module_.get(), specs, modes,
          flutter::DlBlendMode::kSrcOver, /*fetches=*/false, "content");
      if (set.content[technique][dest] == nullptr) {
        return nullptr;
      }
      if (technique == 0) {
        set.blur[dest] =
            MakePipeline(format, blur_module_.get(), specs, modes,
                         flutter::DlBlendMode::kSrcOver, /*fetches=*/false,
                         "blur");
        if (set.blur[dest] == nullptr) {
          return nullptr;
        }
      }
    }
    // Composites: blend into source-1, direct-write-reset source.
    for (size_t source = 1; source <= kMLRMaxCanvasDepth; source++) {
      Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                           Mode::kMasked};
      modes[source - 1] = Mode::kSrcOver;
      modes[source] = Mode::kDirectWrite;
      set.composite[technique][source - 1] = MakePipeline(
          format, composite_module_.get(), specs, modes,
          flutter::DlBlendMode::kSrcOver, /*fetches=*/true, "composite");
      if (set.composite[technique][source - 1] == nullptr) {
        return nullptr;
      }
    }
  }
  // Accumulation is always Loop-Blinn: kWindingAccumulate is the only
  // program that reaches this pipeline.
  {
    SpecValues specs;
    specs.is_path = VK_TRUE;
    Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                         Mode::kAdditive};
    set.winding =
        MakePipeline(format, winding_module_.get(), specs, modes,
                     flutter::DlBlendMode::kSrcOver, /*fetches=*/false,
                     "winding accumulate");
    if (set.winding == nullptr) {
      return nullptr;
    }
  }
  for (size_t rule = 0; rule < 2; rule++) {
    SpecValues specs;
    specs.fill_rule = static_cast<int32_t>(rule);
    for (size_t dest = 0; dest <= kMLRMaxCanvasDepth; dest++) {
      Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                           Mode::kDirectWrite};
      modes[dest] = Mode::kSrcOver;
      set.winding_composite[rule][dest] = MakePipeline(
          format, winding_composite_module_.get(), specs, modes,
          flutter::DlBlendMode::kSrcOver, /*fetches=*/true,
          "winding composite");
      if (set.winding_composite[rule][dest] == nullptr) {
        return nullptr;
      }
    }
    // Blend into source-1, reset source, resolve and clear the
    // accumulator: the union of what the two composites each do.
    for (size_t source = 1; source <= kMLRMaxCanvasDepth; source++) {
      Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                           Mode::kDirectWrite};
      modes[source - 1] = Mode::kSrcOver;
      modes[source] = Mode::kDirectWrite;
      set.winding_canvas_composite[rule][source] = MakePipeline(
          format, winding_canvas_composite_module_.get(), specs, modes,
          flutter::DlBlendMode::kSrcOver, /*fetches=*/true, "concave clip");
      if (set.winding_canvas_composite[rule][source] == nullptr) {
        return nullptr;
      }
    }
  }
  // Resets: direct-write transparent black to one canvas (including the
  // winding accumulator).
  for (size_t canvas = 1; canvas <= kMLRWindingCanvas; canvas++) {
    SpecValues specs;
    Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                         Mode::kMasked};
    modes[canvas] = Mode::kDirectWrite;
    set.reset[canvas - 1] =
        MakePipeline(format, fragment_module_.get(), specs, modes,
                     flutter::DlBlendMode::kSrcOver, /*fetches=*/false,
                     "reset");
    if (set.reset[canvas - 1] == nullptr) {
      return nullptr;
    }
  }
  return &pipelines_.emplace(static_cast<uint64_t>(format), std::move(set))
              .first->second;
}

const GPUProgram* GPUProgramResolverVK::GetContentPipeline(
    size_t technique,
    size_t dest,
    flutter::DlBlendMode blend) {
  PipelineSet& set = *current_pipelines_;
  if (blend == flutter::DlBlendMode::kSrcOver) {
    return set.content[technique][dest].get();
  }
  const uint32_t key = (static_cast<uint32_t>(blend) << 8) |
                       (static_cast<uint32_t>(technique) << 4) |
                       static_cast<uint32_t>(dest);
  auto& cached = set.blend_variants[key];
  if (cached == nullptr) {
    using Mode = AttachmentMode;
    Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                         Mode::kMasked};
    SpecValues specs;
    specs.is_path = technique == kPathTechnique ? VK_TRUE : VK_FALSE;
    // Coverage-safe modes use fixed-function factors over the standard
    // fragment function; the rest use the fetch-blend function with
    // blending disabled.
    if (IsCoverageSafeBlend(blend)) {
      modes[dest] = Mode::kFactors;
      cached =
          MakePipeline(current_format_, fragment_module_.get(), specs, modes,
                       blend, /*fetches=*/false, "content blend");
    } else {
      modes[dest] = Mode::kDirectWrite;
      specs.fetch_dest = static_cast<int32_t>(dest);
      specs.fetch_blend = static_cast<int32_t>(blend);
      cached = MakePipeline(current_format_, fetch_blend_module_.get(), specs,
                            modes, blend, /*fetches=*/true, "fetch blend");
    }
  }
  return cached.get();
}

const GPUProgram* GPUProgramResolverVK::GetCompositeBlendPipeline(
    size_t technique,
    size_t source,
    flutter::DlBlendMode blend) {
  PipelineSet& set = *current_pipelines_;
  const uint32_t key = (static_cast<uint32_t>(blend) << 8) |
                       (static_cast<uint32_t>(source) << 4) |
                       static_cast<uint32_t>(technique);
  auto& cached = set.composite_blend_variants[key];
  if (cached == nullptr) {
    using Mode = AttachmentMode;
    // Both the destination and the source canvas are direct writes: the
    // blend already accounts for the destination, and blending it again
    // through fixed function would count it twice.
    Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                         Mode::kMasked};
    modes[source - 1] = Mode::kDirectWrite;
    modes[source] = Mode::kDirectWrite;
    SpecValues specs;
    specs.is_path = technique == kPathTechnique ? VK_TRUE : VK_FALSE;
    specs.fetch_blend = static_cast<int32_t>(blend);
    cached = MakePipeline(current_format_, composite_blend_module_.get(),
                          specs, modes, blend, /*fetches=*/true,
                          "composite blend");
  }
  return cached.get();
}

const GPUProgram* GPUProgramResolverVK::GetColorFilterPipeline(
    ColorFilterTier tier,
    size_t technique,
    size_t index,
    flutter::DlBlendMode blend) {
  PipelineSet& set = *current_pipelines_;
  const bool texture_source = tier == kTextureSample;
  const uint32_t key = (static_cast<uint32_t>(blend) << 12) |
                       (static_cast<uint32_t>(tier) << 8) |
                       (static_cast<uint32_t>(technique) << 4) |
                       static_cast<uint32_t>(index);
  auto& cached = set.color_filter_variants[key];
  if (cached == nullptr) {
    using Mode = AttachmentMode;
    // The canvas-fetch tier writes one level DOWN from the canvas it
    // reads; the other two write the attachment named by `index`.
    const size_t destination = tier == kCanvasFetch ? index - 1 : index;
    Mode modes[kMLRWindingCanvas + 1] = {Mode::kMasked, Mode::kMasked,
                                         Mode::kMasked};
    if (tier == kBackdrop) {
      // Blending OFF: the destination is already in hand through the
      // fetch, and blending it again would count it twice.
      modes[destination] = Mode::kDirectWrite;
    } else {
      modes[destination] = Mode::kSrcOver;
    }
    if (tier == kCanvasFetch) {
      // The fetch tier clears its source canvas as it resolves.
      modes[index] = Mode::kDirectWrite;
    }
    SpecValues specs;
    specs.is_path = !texture_source && technique == kPathTechnique
                        ? VK_TRUE
                        : VK_FALSE;
    specs.filter_blend = static_cast<int32_t>(blend);
    if (tier == kBackdrop) {
      // The backdrop reads the attachment it writes; fetch_dest selects
      // it.
      specs.fetch_dest = static_cast<int32_t>(index);
    }
    const vk::ShaderModule fragment =
        tier == kBackdrop  ? backdrop_module_.get()
        : texture_source   ? texture_filter_module_.get()
                           : composite_filter_module_.get();
    cached = MakePipeline(current_format_, fragment, specs, modes, blend,
                          /*fetches=*/tier != kTextureSample, "color filter");
  }
  return cached.get();
}

const GPUProgram* GPUProgramResolverVK::Resolve(const ProgramKey& key) {
  PipelineSet& set = *current_pipelines_;
  switch (key.kind) {
    case MLRDrawKind::kWindingAccumulate:
      return set.winding.get();
    case MLRDrawKind::kBlur:
      return set.blur[key.canvas].get();
    case MLRDrawKind::kColorFilter: {
      const ColorFilterTier tier = key.backdrop            ? kBackdrop
                                   : key.canvas_source < 0 ? kTextureSample
                                                           : kCanvasFetch;
      return GetColorFilterPipeline(
          tier, key.technique,
          tier == kCanvasFetch ? key.canvas_source : key.canvas, key.blend);
    }
    case MLRDrawKind::kWindingResolve:
      return set
          .winding_composite[static_cast<size_t>(key.fill_rule)][key.canvas]
          .get();
    case MLRDrawKind::kWindingMask:
      return set
          .winding_canvas_composite[static_cast<size_t>(key.fill_rule)]
                                   [key.canvas_source]
          .get();
    case MLRDrawKind::kComposite:
      if (key.blend != flutter::DlBlendMode::kSrcOver) {
        return GetCompositeBlendPipeline(
            key.technique, static_cast<size_t>(key.canvas_source), key.blend);
      }
      return set.composite[key.technique][key.canvas_source - 1].get();
    case MLRDrawKind::kReset:
      return set.reset[key.canvas - 1].get();
    case MLRDrawKind::kContent:
      return GetContentPipeline(key.technique, key.canvas, key.blend);
  }
  return nullptr;
}

}  // namespace impeller
