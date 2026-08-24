// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#version 460

#include "outputs.glsl"
#include "shade.glsl"

layout(push_constant) uniform Push { PROPELLER_PUSH_HEAD } push;

void main() {
  Broadcast(ShadeFragment(push.opacity));
}
