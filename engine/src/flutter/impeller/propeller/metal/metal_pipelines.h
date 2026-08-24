// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_METAL_METAL_PIPELINES_H_
#define FLUTTER_IMPELLER_PROPELLER_METAL_METAL_PIPELINES_H_

#import <Metal/Metal.h>

#include <memory>
#include <unordered_map>

#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {

/// The attachments every pass carries after the target it renders.
static constexpr size_t kPrAccumulatorAttachment = 1;
static constexpr size_t kPrClipAttachment = 2;
static constexpr size_t kPrAttachmentCount = 3;

/// Attachment `index`'s pixel format: the target it renders, the
/// scalar winding accumulator, then the pass's clip coverage.
inline MTLPixelFormat PrAttachmentFormat(size_t index, MTLPixelFormat target) {
  switch (index) {
    case kPrAccumulatorAttachment:
      return MTLPixelFormatR16Float;
    case kPrClipAttachment:
      return MTLPixelFormatR8Unorm;
    default:
      return target;
  }
}

/// The two shader specializations: flat coverage, and the Loop-Blinn
/// implicit the path programs read.
static constexpr size_t kColorTechnique = 0;
static constexpr size_t kPathTechnique = 1;
static constexpr size_t kTechniqueCount = 2;

class GPUProgramMTL final : public GPUProgram {
 public:
  explicit GPUProgramMTL(id<MTLRenderPipelineState> pipeline)
      : pipeline_(pipeline) {}

  ~GPUProgramMTL() override = default;

  id<MTLRenderPipelineState> GetNativePipeline() const { return pipeline_; }

 private:
  id<MTLRenderPipelineState> pipeline_;
};

/// The ubershader library and its render pipeline catalog: one set per
/// target pixel format, built on first use.
class GPUProgramResolverMTL final : public GPUProgramResolver {
 public:
  /// Loads the library and its base specializations; a resolver that is
  /// not IsValid() afterwards can never resolve anything.
  explicit GPUProgramResolverMTL(id<MTLDevice> device);

  [[nodiscard]] bool IsValid() const { return valid_; }

  /// Point Resolve at the catalog for `format`, building it on first
  /// use. False when pipeline creation fails.
  bool SetTargetFormat(MTLPixelFormat format);

  // |GPUProgramResolver|
  const GPUProgram* Resolve(ProgramType type) override;

  GPUProgramResolverMTL(const GPUProgramResolverMTL&) = delete;
  GPUProgramResolverMTL& operator=(const GPUProgramResolverMTL&) = delete;

 private:
  struct PipelineSet {
    /// Content, per technique: src-over into the target, accumulator
    /// masked off.
    id<MTLRenderPipelineState> content[kTechniqueCount] = {};
    /// Per technique: signed coverage summed into the accumulator,
    /// target masked off.
    id<MTLRenderPipelineState> winding[kTechniqueCount] = {};
    /// Per fill rule: the accumulator resolved into the target, and
    /// zeroed on the way through.
    id<MTLRenderPipelineState> winding_resolve[2] = {};
    /// Per fill rule: what a clip shape accumulated, multiplied into
    /// the clip attachment, and the attachment written back to fully
    /// visible.
    id<MTLRenderPipelineState> clip_resolve[2] = {};
    id<MTLRenderPipelineState> clip_reset = nil;
    /// One axis of a separable Gaussian over a sampled texture,
    /// src-over into the target like any other content.
    id<MTLRenderPipelineState> blur = nil;
  };

  /// The ramp pipeline, which is not per target format: it only ever
  /// draws into the RGBA8 gradient atlas, and only ever from one
  /// attachment's worth of state.
  id<MTLRenderPipelineState> ramp_ = nil;

  PipelineSet* GetPipelines(MTLPixelFormat format);

  const GPUProgram* Wrap(id<MTLRenderPipelineState> pipeline);

  id<MTLDevice> device_ = nil;
  bool valid_ = false;
  id<MTLLibrary> shader_library_ = nil;
  id<MTLFunction> vertex_functions_[kTechniqueCount] = {nil, nil};
  id<MTLFunction> fragment_functions_[kTechniqueCount] = {nil, nil};

  /// Pipelines are specialized per render-target pixel format on first
  /// use (offscreen RGBA8, drawable BGRA8, ...).
  std::unordered_map<uint64_t, PipelineSet> pipelines_;
  /// The catalog Resolve is currently answering from.
  PipelineSet* current_pipelines_ = nullptr;
  MTLPixelFormat current_format_ = MTLPixelFormatRGBA8Unorm;
  /// Stable GPUProgram wrappers over the raw pipeline states.
  std::unordered_map<void*, std::unique_ptr<GPUProgramMTL>> program_wrappers_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_METAL_METAL_PIPELINES_H_
