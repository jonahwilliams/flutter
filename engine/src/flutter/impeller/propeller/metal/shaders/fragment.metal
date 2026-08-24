// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "blend.h"
#include "common.h"
#include "shade.h"

// Fetch-blend content: exact `mix(dst, blend(src, dst), coverage)`,
// blending disabled (masks route the write). Destination and mode are
// function constants: each pipeline keeps one live fetch input and one
// compiled blend function.
fragment FragmentOut FragmentMainFetchBlend(
    Varyings in [[stage_in]],
    const device PaintData* paints [[buffer(0)]],
    const device TextureRef* textures [[buffer(1)]],
    const device GradientData* gradients [[buffer(2)]],
    half4 d0 [[color(0)]],
    half clip [[color(2)]]) {
  const ShadeResult parts = ShadeParts(in, paints, textures, gradients, clip);
  // One fetchable attachment now, so fetch_dest has nothing to choose.
  const half4 dst = d0;
  const half4 result =
      mix(dst, BlendPremult(parts.source, dst), parts.coverage);
  return Broadcast(result);
}

fragment FragmentOut FragmentMain(
    Varyings in [[stage_in]],
    const device PaintData* paints [[buffer(0)]],
    const device TextureRef* textures [[buffer(1)]],
    const device GradientData* gradients [[buffer(2)]],
    half clip [[color(2)]]) {
  return Broadcast(ShadeFragment(in, paints, textures, gradients, clip));
}

// Single-attachment variant for devices (or the A/B toggle) without
// virtual canvases: no extra outputs, no canvas attachments in the pass.
fragment half4 FragmentMainSingle(
    Varyings in [[stage_in]],
    const device PaintData* paints [[buffer(0)]],
    const device TextureRef* textures [[buffer(1)]],
    const device GradientData* gradients [[buffer(2)]],
    half clip [[color(2)]]) {
  return ShadeFragment(in, paints, textures, gradients, clip);
}
