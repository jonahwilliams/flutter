// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

/// A framebuffer-local backdrop filter. It reads the very attachment it
/// is about to write: the destination arrives through the subpass input,
/// gets filtered, and is composited straight back over itself.

#include "color_filter.glsl"
#include "inputs.glsl"
#include "outputs.glsl"
#include "varyings.glsl"

layout(push_constant) uniform Push {
  PROPELLER_PUSH_HEAD
  PROPELLER_PUSH_COLOR_FILTER
} push;

void main() {
  const half4 dst = fetch_dest == 0 ? half4(subpassLoad(in_d0))
                                    : half4(subpassLoad(in_d1));
  const ColorFilterData f = ColorFilterData(
      push.filter_kind, push.filter_row0, push.filter_row1,
      push.filter_row2, push.filter_row3, push.filter_translate);
  const half4 filtered =
      ApplyColorFilter(f, dst) * half4(v_color) * half(push.opacity);
  Broadcast(filtered + dst * (half(1.0) - filtered.a));
}
