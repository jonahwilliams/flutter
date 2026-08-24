// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_OUTPUTS_GLSL_
#define PROPELLER_OUTPUTS_GLSL_

#include "types.glsl"

// Every pass outputs every attachment and the correct destination is
// selected via color write mask on the pipeline. Location 2 is the
// scalar R16Float winding accumulator, not a canvas.
layout(location = 0) out vec4 out_c0;
layout(location = 1) out vec4 out_c1;
layout(location = 2) out vec4 out_w;

void Broadcast(half4 value) {
  out_c0 = vec4(value);
  out_c1 = vec4(value);
  out_w = vec4(float(value.a));  // Only the winding reset path writes this.
}

#endif  // PROPELLER_OUTPUTS_GLSL_
