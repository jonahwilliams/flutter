// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/metal/metal_pipelines.h"

#include "flutter/fml/logging.h"
#include "impeller/propeller/mtl/propeller_shaders.h"

namespace impeller {

namespace {

/// Human-readable names for GPU captures (Xcode/Instruments show these
/// on pipelines, encoders and buffers, and "Pipeline 37" tells nobody
/// anything).
const char* TechniqueName(size_t technique) {
  return technique == kPathTechnique ? "path" : "color";
}

}  // namespace

GPUProgramResolverMTL::GPUProgramResolverMTL(id<MTLDevice> device)
    : device_(device) {
  NSError* error = nil;
  dispatch_data_t library_data =
      ::dispatch_data_create(impeller_propeller_shaders_data,    // buffer
                             impeller_propeller_shaders_length,  // size
                             dispatch_get_main_queue(),          // queue
                             ^(){
                             }  // destructor
      );
  id<MTLLibrary> library = [device_ newLibraryWithData:library_data
                                                 error:&error];
  if (library == nil) {
    FML_LOG(ERROR) << "Ubershader library load failed: "
                   << error.localizedDescription.UTF8String;
    return;
  }
  library.label = @"Propeller ubershader";
  shader_library_ = library;

  for (size_t technique = 0; technique < kTechniqueCount; technique++) {
    MTLFunctionConstantValues* constants =
        [[MTLFunctionConstantValues alloc] init];
    bool is_path = technique == kPathTechnique;
    [constants setConstantValue:&is_path type:MTLDataTypeBool atIndex:0];
    id<MTLFunction> vertex_function = [library newFunctionWithName:@"VertexMain"
                                                    constantValues:constants
                                                             error:&error];
    id<MTLFunction> fragment_function =
        [library newFunctionWithName:@"FragmentMainSingle"
                      constantValues:constants
                               error:&error];
    if (vertex_function == nil || fragment_function == nil) {
      FML_LOG(ERROR) << "Function specialization failed.";
      return;
    }
    NSString* suffix =
        [NSString stringWithFormat:@" (%s)", TechniqueName(technique)];
    vertex_function.label = [@"VertexMain" stringByAppendingString:suffix];
    fragment_function.label =
        [@"FragmentMainSingle" stringByAppendingString:suffix];
    vertex_functions_[technique] = vertex_function;
    fragment_functions_[technique] = fragment_function;
  }

  {
    MTLRenderPipelineDescriptor* descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = @"Propeller gradient ramp";
    descriptor.vertexFunction = [library newFunctionWithName:@"RampVertexMain"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"RampFragmentMain"];
    if (descriptor.vertexFunction == nil ||
        descriptor.fragmentFunction == nil) {
      FML_LOG(ERROR) << "Ramp function lookup failed.";
      return;
    }
    // One attachment, written straight: a ramp replaces its row.
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
    ramp_ = [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                    error:&error];
    if (ramp_ == nil) {
      FML_LOG(ERROR) << "Ramp pipeline creation failed: "
                     << error.localizedDescription.UTF8String;
      return;
    }
  }

  valid_ = true;
}

bool GPUProgramResolverMTL::SetTargetFormat(MTLPixelFormat format) {
  PipelineSet* set = GetPipelines(format);
  if (set == nullptr) {
    return false;
  }
  current_pipelines_ = set;
  current_format_ = format;
  return true;
}

GPUProgramResolverMTL::PipelineSet* GPUProgramResolverMTL::GetPipelines(
    MTLPixelFormat format) {
  auto found = pipelines_.find(format);
  if (found != pipelines_.end()) {
    return &found->second;
  }

  // Every pipeline declares the whole attachment layout: the target,
  // then the winding accumulator. What differentiates the catalog is
  // per-attachment blend and write-mask state; the functions are
  // shared.
  enum AttachmentMode { kMasked, kDirectWrite, kSrcOver, kAdditive, kMultiply };
  auto make_pipeline = [&](id<MTLFunction> vertex_function,
                           id<MTLFunction> fragment_function,
                           const AttachmentMode(&modes)[kPrAttachmentCount],
                           NSString* label) -> id<MTLRenderPipelineState> {
    MTLRenderPipelineDescriptor* descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = label;
    descriptor.vertexFunction = vertex_function;
    descriptor.fragmentFunction = fragment_function;
    for (size_t i = 0; i < kPrAttachmentCount; i++) {
      MTLRenderPipelineColorAttachmentDescriptor* color =
          descriptor.colorAttachments[i];
      color.pixelFormat = PrAttachmentFormat(i, format);
      if (modes[i] == kMasked) {
        color.writeMask = MTLColorWriteMaskNone;
        continue;
      }
      color.writeMask = MTLColorWriteMaskAll;
      if (modes[i] == kSrcOver) {
        // Src-over, premultiplied.
        color.blendingEnabled = YES;
        color.sourceRGBBlendFactor = MTLBlendFactorOne;
        color.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
      } else if (modes[i] == kMultiply) {
        // Clips intersect, and coverage intersects by multiplying.
        color.blendingEnabled = YES;
        color.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
        color.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorZero;
        color.destinationAlphaBlendFactor = MTLBlendFactorZero;
      } else if (modes[i] == kAdditive) {
        // Signed winding accumulation (float target: no clamping).
        color.blendingEnabled = YES;
        color.sourceRGBBlendFactor = MTLBlendFactorOne;
        color.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color.destinationRGBBlendFactor = MTLBlendFactorOne;
        color.destinationAlphaBlendFactor = MTLBlendFactorOne;
      }
    }
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) {
      FML_LOG(ERROR) << "Pipeline creation failed: "
                     << error.localizedDescription.UTF8String;
    }
    return pipeline;
  };

  PipelineSet set;
  for (size_t technique = 0; technique < kTechniqueCount; technique++) {
    AttachmentMode modes[kPrAttachmentCount] = {};
    modes[0] = kSrcOver;
    set.content[technique] = make_pipeline(
        vertex_functions_[technique], fragment_functions_[technique], modes,
        [NSString stringWithFormat:@"Propeller %s", TechniqueName(technique)]);
    if (set.content[technique] == nil) {
      return nullptr;
    }
  }

  for (size_t technique = 0; technique < kTechniqueCount; technique++) {
    AttachmentMode modes[kPrAttachmentCount] = {};
    modes[kPrAccumulatorAttachment] = kAdditive;
    MTLFunctionConstantValues* constants =
        [[MTLFunctionConstantValues alloc] init];
    bool is_path = technique == kPathTechnique;
    [constants setConstantValue:&is_path type:MTLDataTypeBool atIndex:0];
    NSError* winding_error = nil;
    id<MTLFunction> winding_function =
        [shader_library_ newFunctionWithName:@"WindingMain"
                              constantValues:constants
                                       error:&winding_error];
    FML_DCHECK(winding_function != nil);
    winding_function.label = [NSString
        stringWithFormat:@"WindingMain (%s)", TechniqueName(technique)];
    set.winding[technique] = make_pipeline(
        vertex_functions_[technique], winding_function, modes,
        [NSString stringWithFormat:@"Propeller winding accumulate %s",
                                   TechniqueName(technique)]);
    if (set.winding[technique] == nil) {
      return nullptr;
    }
  }

  for (size_t rule = 0; rule < 2; rule++) {
    MTLFunctionConstantValues* constants =
        [[MTLFunctionConstantValues alloc] init];
    bool is_path = false;
    int rule_value = static_cast<int>(rule);
    [constants setConstantValue:&is_path type:MTLDataTypeBool atIndex:0];
    [constants setConstantValue:&rule_value type:MTLDataTypeInt atIndex:3];
    NSError* composite_error = nil;
    id<MTLFunction> composite_function =
        [shader_library_ newFunctionWithName:@"WindingCompositeMain"
                              constantValues:constants
                                       error:&composite_error];
    FML_DCHECK(composite_function != nil);
    composite_function.label =
        [NSString stringWithFormat:@"WindingCompositeMain (%s)",
                                   rule == 0 ? "nonzero" : "evenOdd"];
    AttachmentMode modes[kPrAttachmentCount] = {};
    modes[0] = kSrcOver;
    modes[kPrAccumulatorAttachment] = kDirectWrite;  // Resolve and clear.
    set.winding_resolve[rule] = make_pipeline(
        vertex_functions_[kColorTechnique], composite_function, modes,
        [NSString stringWithFormat:@"Propeller winding resolve %s",
                                   rule == 0 ? "nonzero" : "evenOdd"]);
    if (set.winding_resolve[rule] == nil) {
      return nullptr;
    }
  }
  for (size_t rule = 0; rule < 2; rule++) {
    MTLFunctionConstantValues* constants =
        [[MTLFunctionConstantValues alloc] init];
    int rule_value = static_cast<int>(rule);
    [constants setConstantValue:&rule_value type:MTLDataTypeInt atIndex:3];
    NSError* resolve_error = nil;
    id<MTLFunction> resolve_function =
        [shader_library_ newFunctionWithName:@"ClipResolveMain"
                              constantValues:constants
                                       error:&resolve_error];
    FML_DCHECK(resolve_function != nil);
    resolve_function.label =
        [NSString stringWithFormat:@"ClipResolveMain (%s)",
                                   rule == 0 ? "nonzero" : "evenOdd"];
    AttachmentMode modes[kPrAttachmentCount] = {};
    modes[kPrAccumulatorAttachment] = kDirectWrite;  // Resolve and clear.
    modes[kPrClipAttachment] = kMultiply;            // Clips intersect.
    set.clip_resolve[rule] = make_pipeline(
        vertex_functions_[kColorTechnique], resolve_function, modes,
        [NSString stringWithFormat:@"Propeller clip resolve %s",
                                   rule == 0 ? "nonzero" : "evenOdd"]);
    if (set.clip_resolve[rule] == nil) {
      return nullptr;
    }
  }

  {
    id<MTLFunction> clip_function =
        [shader_library_ newFunctionWithName:@"ClipMain"];
    FML_DCHECK(clip_function != nil);
    clip_function.label = @"ClipMain";
    AttachmentMode modes[kPrAttachmentCount] = {};
    modes[kPrClipAttachment] = kDirectWrite;
    set.clip_reset =
        make_pipeline(vertex_functions_[kColorTechnique], clip_function, modes,
                      @"Propeller clip reset");
    if (set.clip_reset == nil) {
      return nullptr;
    }
  }

  return &pipelines_.emplace(format, set).first->second;
}

const GPUProgram* GPUProgramResolverMTL::Wrap(
    id<MTLRenderPipelineState> pipeline) {
  if (pipeline == nil) {
    return nullptr;
  }
  auto& wrapper = program_wrappers_[(__bridge void*)pipeline];
  if (wrapper == nullptr) {
    wrapper = std::make_unique<GPUProgramMTL>(pipeline);
  }
  return wrapper.get();
}

const GPUProgram* GPUProgramResolverMTL::Resolve(ProgramType type) {
  if (type == ProgramType::kGradientRamp) {
    return Wrap(ramp_);
  }
  PipelineSet& set = *current_pipelines_;
  switch (type) {
    case ProgramType::kColor:
      return Wrap(set.content[kColorTechnique]);
    case ProgramType::kPath:
      return Wrap(set.content[kPathTechnique]);
    case ProgramType::kWindingAccumulate:
      return Wrap(set.winding[kPathTechnique]);
    case ProgramType::kWindingAccumulateFlat:
      return Wrap(set.winding[kColorTechnique]);
    case ProgramType::kWindingResolveNonZero:
      return Wrap(set.winding_resolve[0]);
    case ProgramType::kWindingResolveEvenOdd:
      return Wrap(set.winding_resolve[1]);
    case ProgramType::kClipResolveNonZero:
      return Wrap(set.clip_resolve[0]);
    case ProgramType::kClipResolveEvenOdd:
      return Wrap(set.clip_resolve[1]);
    case ProgramType::kClipReset:
      return Wrap(set.clip_reset);
    case ProgramType::kInvalid:
    case ProgramType::kGradientRamp:
    case ProgramType::kProgramLength:
      break;
  }
  return nullptr;
}

}  // namespace impeller
