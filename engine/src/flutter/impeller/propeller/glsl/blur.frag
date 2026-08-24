// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

/// Separable gaussian: one axis per pass, pushed per filtered composite.

#include "buffers.glsl"
#include "outputs.glsl"
#include "varyings.glsl"

layout(push_constant) uniform Push {
  PROPELLER_PUSH_HEAD
  // Per-tap UV offset along the blur axis.
  vec2 filter_step;
  float filter_sigma;
  int filter_radius;
} push;

void main() {
  const PaintData paint = paints[v_paint_index];
  // The taps arrive as half, but the running sum stays float: a wide blur
  // accumulates dozens of weighted samples, and half's mantissa runs out
  // well before the tail weights stop contributing.
  vec4 sum = vec4(0.0);
  float weight_sum = 0.0;
  const float denominator = 2.0 * push.filter_sigma * push.filter_sigma;
  for (int i = -push.filter_radius; i <= push.filter_radius; i++) {
    const float weight = exp(-float(i * i) / denominator);
    const vec2 uv = v_uv + push.filter_step * float(i);
    sum += textureLod(sampler2D(textures[nonuniformEXT(paint.texture_index)],
                                linear_sampler),
                      uv, 0.0) *
           weight;
    weight_sum += weight;
  }
  const half4 color = half4(sum / weight_sum);
  Broadcast(color * half4(v_color) * half(push.opacity));
}
