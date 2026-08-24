// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

/// A scope composite MASKED BY THE WINDING ACCUMULATOR: a concave clip.
/// The canvas supplies the colour and the accumulated winding supplies
/// the coverage, so an arbitrary path clips a whole subtree without ever
/// tessellating its interior. Resolves and clears the accumulator, and
/// resets the canvas, exactly as the two composites it is made of do.

#include "common.glsl"
#include "coverage.glsl"
#include "inputs.glsl"
#include "outputs.glsl"
#include "varyings.glsl"

layout(push_constant) uniform Push { PROPELLER_PUSH_HEAD } push;

void main() {
  const half w = half(subpassLoad(in_w).r);
  const half4 s1 = half4(subpassLoad(in_d1));
  Broadcast(WindingCoverage(w) * s1 * half4(v_color) * half(push.opacity));
  out_w = vec4(0.0);
  out_c1 = vec4(0.0);
}
