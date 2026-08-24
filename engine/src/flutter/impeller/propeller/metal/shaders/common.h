// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_COMMON_H_
#define FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_COMMON_H_

#include <metal_stdlib>
using namespace metal;

// The smallest half that is comfortably normal. Guards divisions whose
// float epsilon (1e-6) is subnormal at half precision and may flush to
// zero. Below one 8-bit alpha step, so it never changes a real colour.
constant half kHalfEpsilon = 1.0h / 1024.0h;

struct PaintData {
  int gradient_index;
  int texture_index;
  int transform_index;
  uint flags;
};

// Gradient descriptor.
struct GradientData {
  int ramp_row;
  uint type;       // 0 linear, 1 radial, 2 conic
  uint tile_mode;  // 0 clamp, 1 repeat, 2 mirror, 3 decal
  // First texel in the low 16 bits, texel count in the high 16.
  uint ramp_span;
  float4 data;
  // Inverse of the gradient's local matrix: row-major 2x2 then (tx, ty).
  // A gradient's parameters live in its OWN space, and the local matrix is
  // what carries that space onto the geometry -- so evaluating at a point
  // means mapping the point back the other way.
  float4 inverse_basis;
  float4 inverse_translation;
};

// The residual affine: a 2x2 by column, then translate. The frame flatten
// composes the instance transform into these on the CPU, so the shader
// applies exactly one transform per vertex.
/// impeller::Matrix, column major. The full 4x4: a canvas transform can
/// carry perspective, and dropping it silently misplaces geometry.
struct TransformData {
  float4 columns[4];
};

struct VertexAttributes {
  float2 uv;
  uint color;
  uint paint;
};

struct Varyings {
  float4 position [[position]];
  float2 uv;
  /// Loop-Blinn implicit coordinate.
  float2 implicit;
  /// Pre-transform recorded-space position.
  float2 local_position;
  /// Unpacked from four bytes, so half is exact.
  half4 color;
  uint paint_index [[flat]];
};

struct TextureRef {
  texture2d<half> tex;
};

constant bool is_path [[function_constant(0)]];
constant int fetch_dest [[function_constant(1)]];
constant int fetch_blend [[function_constant(2)]];
// 0 = nonzero, 1 = even-odd.
constant int fill_rule [[function_constant(3)]];
/// blend color filter mode
constant int filter_blend [[function_constant(4)]];

// Loop-Blinn analytic coverage as signed distance.
static inline float PathCoverage(float2 implicit) {
  const float f = implicit.x * implicit.x - implicit.y;
  const float2 px = dfdx(implicit);
  const float2 py = dfdy(implicit);
  const float fx = 2.0 * implicit.x * px.x - px.y;
  const float fy = 2.0 * implicit.x * py.x - py.y;
  const float gradient = sqrt(fx * fx + fy * fy);
  return saturate(0.5 - f / gradient);
}

static inline half WindingCoverage(half w) {
  if (fill_rule == 0) {
    return saturate(abs(w));
  }
  return 1.0h - abs(fract(w * 0.5h) * 2.0h - 1.0h);
}

// A pass has two attachments and no more: what it renders, and the
// scalar R16Float winding accumulator. Both are written every time and
// the colour write mask on the pipeline picks which one lands.
struct FragmentOut {
  half4 c0 [[color(0)]];
  half w [[color(1)]];
};

static inline FragmentOut Broadcast(half4 value) {
  FragmentOut out;
  out.c0 = value;
  out.w = value.a;  // Only the winding reset path writes this attachment.
  return out;
}

#endif  // FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_COMMON_H_
