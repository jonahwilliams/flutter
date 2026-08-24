// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

// A scope composite shaded through a colour filter as it resolves.

#include "color_filter.glsl"
#include "common.glsl"
#include "coverage.glsl"
#include "inputs.glsl"
#include "outputs.glsl"
#include "varyings.glsl"

layout(push_constant) uniform Push {
  PROPELLER_PUSH_HEAD
  PROPELLER_PUSH_COLOR_FILTER
} push;

void main() {
  half coverage = half(1.0);
  if (is_path) {
    coverage = half(PathCoverage(v_implicit));
  }
  const ColorFilterData f = ColorFilterData(
      push.filter_kind, push.filter_row0, push.filter_row1,
      push.filter_row2, push.filter_row3, push.filter_translate);
  const half4 src = ApplyColorFilter(f, half4(subpassLoad(in_d1)));
  const half4 result =
      coverage * src * half4(v_color) * half(push.opacity);
  Broadcast(result);
  out_c1 = vec4(0.0);
}
