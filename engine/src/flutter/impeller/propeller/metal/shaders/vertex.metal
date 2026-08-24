// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common.h"

// The canonical Loop-Blinn implicit coordinates, selected by bits 0-2 of
// the paint word's high byte. Only these five are ever written there:
// see kPathImplicit* in geometry.h, which nothing else packs.
constant float2 kPathImplicitCoords[5] = {
    float2(0.0, 0.0),    // curve p0 / linear edge endpoints
    float2(0.5, 0.0),    // curve control
    float2(1.0, 1.0),    // curve p1
    float2(0.0, 1.0),    // saturated interior / linear centroid
    float2(0.0, -1.0),   // outer AA fringe vertex (1px past a line edge)
};

// viewport_origin: the pass's size in .xy, where it starts in .zw.
vertex Varyings VertexMain(uint vid [[vertex_id]],
                           const device packed_float2* positions [[buffer(0)]],
                           const device VertexAttributes* attributes [[buffer(1)]],
                           const device PaintData* paints [[buffer(2)]],
                           constant float4& viewport_origin [[buffer(3)]],
                           const device TransformData* transforms [[buffer(4)]]) {
  const VertexAttributes attr = attributes[vid];
  const uint paint_index = attr.paint & 0xFFFFFFu;
  const PaintData paint = paints[paint_index];
  const TransformData transform = transforms[paint.transform_index];

  const float2 local_position = float2(positions[vid]);
  const float4 homogeneous = transform.columns[0] * local_position.x +
                             transform.columns[1] * local_position.y +
                             transform.columns[3];
  // Perspective divide; w is 1 for the affine case, so this is a no-op
  // there rather than a separate path.
  const float2 p = homogeneous.xy / (homogeneous.w == 0 ? 1 : homogeneous.w);

  Varyings out;
  const float2 in_pass = p - viewport_origin.zw;
  out.position = float4(in_pass.x / viewport_origin.x * 2.0 - 1.0,
                        1.0 - in_pass.y / viewport_origin.y * 2.0, 0.0, 1.0);
  out.uv = attr.uv;
  out.implicit = kPathImplicitCoords[(attr.paint >> 24) & 7u];
  out.local_position = local_position;
  const uint c = attr.color;
  // Four bytes, so every value is exact as a half.
  out.color = half4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu,
                    (c >> 24) & 0xFFu) /
              255.0h;
  out.paint_index = paint_index;
  return out;
}
