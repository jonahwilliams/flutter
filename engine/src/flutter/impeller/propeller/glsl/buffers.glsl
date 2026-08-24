// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_BUFFERS_GLSL_
#define PROPELLER_BUFFERS_GLSL_

#extension GL_EXT_nonuniform_qualifier : require

#include "common.glsl"

// The frame's buffers, bound once per pass. Binding numbers mirror the
// Metal argument indices so the two catalogs stay one mapping apart.
layout(set = 0, binding = 0, std430) readonly buffer Positions {
  vec2 positions[];
};
layout(set = 0, binding = 1, std430) readonly buffer Attributes {
  VertexAttributes attributes[];
};
layout(set = 0, binding = 2, std430) readonly buffer Paints {
  PaintData paints[];
};
layout(set = 0, binding = 4, std430) readonly buffer Transforms {
  TransformData transforms[];
};
layout(set = 0, binding = 5, std430) readonly buffer Gradients {
  GradientData gradients[];
};

// The bindless table: one descriptor-indexed texture array with the two
// samplers every paint chooses between.
// The runtime-sized array sits alone in its set: Metal translation puts
// it at the end of a device argument buffer, so nothing may follow it.
layout(set = 1, binding = 0) uniform texture2D textures[];
// Bindings stay dense: 3 fills the hole left by Metal's push-data slot,
// which some implementations (SwiftShader) require.
layout(set = 0, binding = 3) uniform sampler nearest_sampler;
layout(set = 0, binding = 7) uniform sampler linear_sampler;

// The pass table's reserved slots; see kGradientTextureSlot in
// buffer_arena.h.
#define kGradientTextureSlot 1

#endif  // PROPELLER_BUFFERS_GLSL_
