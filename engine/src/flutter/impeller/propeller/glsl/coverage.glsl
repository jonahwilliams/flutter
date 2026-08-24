// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_COVERAGE_GLSL_
#define PROPELLER_COVERAGE_GLSL_

// Fragment-stage only: PathCoverage runs on screen-space derivatives.

#include "common.glsl"

// Loop-Blinn analytic coverage as signed distance. Float: the chain rule
// runs on the implicit's screen-space derivatives, and the gradient it
// divides by is routinely near the 1e-4 floor.
float PathCoverage(vec2 implicit) {
  const float f = implicit.x * implicit.x - implicit.y;
  const vec2 px = dFdx(implicit);
  const vec2 py = dFdy(implicit);
  const float fx = 2.0 * implicit.x * px.x - px.y;
  const float fy = 2.0 * implicit.x * py.x - py.y;
  const float gradient = max(sqrt(fx * fx + fy * fy), 1e-4);
  return saturate(0.5 - f / gradient);
}

// The accumulator is R16Float, so this is already half-precision data.
half WindingCoverage(half w) {
  if (fill_rule == 0) {
    return saturateh(abs(w));
  }
  return half(1.0) - abs(fract(w * half(0.5)) * half(2.0) - half(1.0));
}

#endif  // PROPELLER_COVERAGE_GLSL_
