// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

// Single-attachment variant for devices (or the A/B toggle) without
// virtual canvases: no extra outputs, no canvas attachments in the pass.

#include "shade.glsl"

layout(push_constant) uniform Push { PROPELLER_PUSH_HEAD } push;

layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(ShadeFragment(push.opacity));
}
