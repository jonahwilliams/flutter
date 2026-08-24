// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct RampVertex {
  packed_float2 position;
  packed_uchar4 color;
};

struct RampVaryings {
  float4 position [[position]];
  half4 color;
};

vertex RampVaryings RampVertexMain(
    uint vid [[vertex_id]],
    const device RampVertex* vertices [[buffer(0)]]) {
  const RampVertex vertex_in = vertices[vid];
  RampVaryings out;
  out.position = float4(float2(vertex_in.position), 0.0, 1.0);
  out.color = half4(float4(uchar4(vertex_in.color)) / 255.0);
  return out;
}

fragment half4 RampFragmentMain(RampVaryings in [[stage_in]]) {
  return half4(in.color.rgb * in.color.a, in.color.a);
}
