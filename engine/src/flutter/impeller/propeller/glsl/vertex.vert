// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

#define PROPELLER_VERTEX

#include "buffers.glsl"
#include "common.glsl"
#include "varyings.glsl"

layout(push_constant) uniform Push { PROPELLER_PUSH_HEAD } push;

// The canonical Loop-Blinn implicit coordinates, selected by bits 0-2 of
// the paint word's high byte. Only these five are ever written there:
// see kPathImplicit* in geometry.h, which nothing else packs.
const vec2 kPathImplicitCoords[5] = vec2[5](
    vec2(0.0, 0.0),    // curve p0 / linear edge endpoints
    vec2(0.5, 0.0),    // curve control
    vec2(1.0, 1.0),    // curve p1
    vec2(0.0, 1.0),    // saturated interior / linear centroid
    vec2(0.0, -1.0));  // outer AA fringe vertex (1px past a line edge)

void main() {
  const VertexAttributes attr = attributes[gl_VertexIndex];
  const uint paint_index = attr.paint & 0xFFFFFFu;
  const PaintData paint = paints[paint_index];
  const TransformData transform = transforms[paint.transform_index];

  const vec2 local_position = positions[gl_VertexIndex];
  const vec4 homogeneous = transform.columns[0] * local_position.x +
                           transform.columns[1] * local_position.y +
                           transform.columns[3];
  // Perspective divide; w is 1 for the affine case, so this is a no-op
  // there rather than a separate path.
  const vec2 p = homogeneous.xy / (homogeneous.w == 0.0 ? 1.0 : homogeneous.w);

  const vec2 in_pass = p - push.viewport_origin.zw;
  gl_Position = vec4(in_pass.x / push.viewport_origin.x * 2.0 - 1.0,
                     1.0 - in_pass.y / push.viewport_origin.y * 2.0, 0.0, 1.0);
  v_uv = attr.uv;
  v_implicit = kPathImplicitCoords[(attr.paint >> 24) & 7u];
  v_local_position = local_position;
  const uint c = attr.color;
  // Four bytes, so every value is exact as a half.
  v_color = vec4(float(c & 0xFFu), float((c >> 8) & 0xFFu),
                 float((c >> 16) & 0xFFu), float((c >> 24) & 0xFFu)) /
            255.0;
  v_paint_index = paint_index;
}
