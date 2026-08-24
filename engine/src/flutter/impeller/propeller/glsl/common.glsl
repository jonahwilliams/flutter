// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_COMMON_GLSL_
#define PROPELLER_COMMON_GLSL_

#include "types.glsl"

// The smallest half that is comfortably normal. Guards divisions whose
// float epsilon (1e-6) is subnormal at half precision and may flush to
// zero. Below one 8-bit alpha step, so it never changes a real colour.
#define kHalfEpsilon half(1.0 / 1024.0)

// Specialization constants, one to one with the Metal function constants.
layout(constant_id = 0) const bool is_path = false;
layout(constant_id = 1) const int fetch_dest = 0;
layout(constant_id = 2) const int fetch_blend = 0;
// 0 = nonzero, 1 = even-odd.
layout(constant_id = 3) const int fill_rule = 0;
// blend color filter mode
layout(constant_id = 4) const int filter_blend = 0;

struct PaintData {
  int gradient_index;
  int texture_index;
  int transform_index;
  uint flags;
};

// Gradient descriptor; layouts match the CPU structs byte for byte.
struct GradientData {
  int ramp_row;
  uint type;       // 0 linear, 1 radial, 2 conic
  uint tile_mode;  // 0 clamp, 1 repeat, 2 mirror, 3 decal
  // First texel in the low 16 bits, texel count in the high 16.
  uint ramp_span;
  vec4 data;
  // Inverse of the gradient's local matrix: row-major 2x2 then (tx, ty).
  vec4 inverse_basis;
  vec4 inverse_translation;
};

// impeller::Matrix, column major. The full 4x4: a canvas transform can
// carry perspective, and dropping it silently misplaces geometry.
struct TransformData {
  vec4 columns[4];
};

struct VertexAttributes {
  vec2 uv;
  uint color;
  uint paint;
};

// The push block: viewport for the vertex stage, the per-run scalars for
// the fragment stage. Filter shaders append their data after it.
// viewport_origin: the pass's size in .xy and where it starts in .zw.
// A pass renders into a texture only as big as what it covers, so the
// vertex stage takes its origin off every position. One subtract per
// vertex, rather than a translation folded into every draw's transform.
#define PROPELLER_PUSH_HEAD \
  vec4 viewport_origin;     \
  float opacity;            \
  int source;               \
  vec2 push_padding;

#endif  // PROPELLER_COMMON_GLSL_
