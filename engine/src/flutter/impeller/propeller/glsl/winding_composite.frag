// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

// Resolve the winding accumulator into the destination and clear it.

#include "common.glsl"
#include "coverage.glsl"
#include "inputs.glsl"
#include "outputs.glsl"
#include "varyings.glsl"

layout(push_constant) uniform Push { PROPELLER_PUSH_HEAD } push;

void main() {
  const half w = half(subpassLoad(in_w).r);
  Broadcast(WindingCoverage(w) * half4(v_color) * half(push.opacity));
  out_w = vec4(0.0);
}
