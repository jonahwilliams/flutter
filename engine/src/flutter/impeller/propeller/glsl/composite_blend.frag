// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

/// A scope composite that lands on its parent with an ADVANCED blend --
/// a saveLayer whose paint carries one. The canvas supplies the source,
/// the destination attachment is fetched, and the blend is computed here
/// rather than by fixed-function state, which only knows src-over.

#include "blend.glsl"
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
  const half4 src =
      half4(subpassLoad(in_d1)) * half4(v_color) * half(push.opacity);
  const half4 dst = half4(subpassLoad(in_d0));
  const half4 result = mix(dst, BlendPremult(src, dst), coverage);
  Broadcast(result);
  out_c1 = vec4(0.0);
}
