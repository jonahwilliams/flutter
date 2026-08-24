// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

// Winding accumulation for concave fills: signed coverage into the
// accumulator, every other attachment masked off. The sign is the
// triangle's FACING, so an outward bulge adds and an inward dent
// subtracts with no CPU orientation test.

#include "common.glsl"
#include "coverage.glsl"
#include "varyings.glsl"

layout(location = 2) out vec4 out_w;

void main() {
  half coverage = half(1.0);
  if (is_path) {
    coverage = half(PathCoverage(v_implicit));
  }
  out_w = vec4(float(gl_FrontFacing ? coverage : -coverage));
}
