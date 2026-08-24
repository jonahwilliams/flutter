// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

// A scope composite: the canvas sampled through the draw's own geometry
// coverage, then the canvas reset to transparent for its next scope.

#include "common.glsl"
#include "coverage.glsl"
#include "inputs.glsl"
#include "outputs.glsl"
#include "varyings.glsl"

layout(push_constant) uniform Push { PROPELLER_PUSH_HEAD } push;

void main() {
  half coverage = half(1.0);
  if (is_path) {
    coverage = half(PathCoverage(v_implicit));
  }
  const half4 s1 = half4(subpassLoad(in_d1));
  const half4 result =
      coverage * s1 * half4(v_color) * half(push.opacity);
  Broadcast(result);
  out_c1 = vec4(0.0);
}
