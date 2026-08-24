// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

/// A color filter applied to a sampled texel rather than a canvas fetch:
/// the pass composite for a flow-level ColorFilterLayer.

#include "buffers.glsl"
#include "color_filter.glsl"
#include "outputs.glsl"
#include "varyings.glsl"

layout(push_constant) uniform Push {
  PROPELLER_PUSH_HEAD
  PROPELLER_PUSH_COLOR_FILTER
} push;

void main() {
  const PaintData paint = paints[v_paint_index];
  const half4 texel = half4(
      texture(sampler2D(textures[nonuniformEXT(paint.texture_index)],
                        linear_sampler),
              v_uv));
  const ColorFilterData f = ColorFilterData(
      push.filter_kind, push.filter_row0, push.filter_row1,
      push.filter_row2, push.filter_row3, push.filter_translate);
  Broadcast(ApplyColorFilter(f, texel) * half4(v_color) * half(push.opacity));
}
