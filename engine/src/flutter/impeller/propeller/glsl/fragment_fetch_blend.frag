// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

// Fetch-blend content: exact `mix(dst, blend(src, dst), coverage)`,
// blending disabled (masks route the write). Destination and mode are
// specialization constants: each pipeline keeps one live fetch input and
// one folded blend case.

#include "blend.glsl"
#include "inputs.glsl"
#include "outputs.glsl"
#include "shade.glsl"

layout(push_constant) uniform Push { PROPELLER_PUSH_HEAD } push;

void main() {
  const ShadeResult parts = ShadeParts(push.opacity);
  const half4 dst = fetch_dest == 0 ? half4(subpassLoad(in_d0))
                                    : half4(subpassLoad(in_d1));
  const half4 result =
      mix(dst, BlendPremult(parts.source, dst), parts.coverage);
  Broadcast(result);
}
