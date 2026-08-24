// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "color_filter.h"
#include "common.h"

/// A blur, uploaded per filtered pass-composite draw call.
struct ImageFilterData {
  /// Per-tap UV offset along the blur axis.
  float2 step;
  float sigma;
  int radius;
};

/// separable gaussian
fragment FragmentOut FragmentMainBlur(
    Varyings in [[stage_in]],
    const device PaintData* paints [[buffer(0)]],
    const device TextureRef* textures [[buffer(1)]],
    constant ImageFilterData& filter [[buffer(6)]]) {
  const PaintData paint = paints[in.paint_index];
  constexpr sampler blur_sampler(coord::normalized, filter::linear,
                                 address::clamp_to_edge);
  float4 sum = float4(0.0);
  float weight_sum = 0.0;
  const float denominator = 2.0 * filter.sigma * filter.sigma;
  for (int i = -filter.radius; i <= filter.radius; i++) {
    const float weight = exp(-float(i * i) / denominator);
    const float2 uv = in.uv + filter.step * float(i);
    sum += float4(textures[paint.texture_index].tex.sample(blur_sampler, uv)) *
           weight;
    weight_sum += weight;
  }
  const half4 color = half4(sum / weight_sum);
  return Broadcast(color * in.color);
}

/// A framebuffer-local backdrop filter. It reads the very attachment it
/// is about to write: the destination arrives through programmable
/// blending, gets filtered, and is composited straight back over itself.
fragment FragmentOut FragmentMainBackdrop(
    Varyings in [[stage_in]],
    constant ColorFilterData& filter [[buffer(6)]],
    half4 d0 [[color(0)]]) {
  // One fetchable attachment now, so fetch_dest has nothing to choose.
  const half4 dst = d0;
  const half4 filtered =
      ApplyColorFilter(filter, dst) * in.color;
  return Broadcast(filtered + dst * (1.0h - filtered.a));
}

/// A color filter applied to a sampled texel rather than a canvas fetch:
/// the pass composite for a flow-level ColorFilterLayer.
fragment FragmentOut FragmentMainColorFilter(
    Varyings in [[stage_in]],
    const device PaintData* paints [[buffer(0)]],
    const device TextureRef* textures [[buffer(1)]],
    constant ColorFilterData& filter [[buffer(6)]]) {
  const PaintData paint = paints[in.paint_index];
  constexpr sampler texture_sampler(coord::normalized, filter::linear,
                                    address::clamp_to_edge);
  const half4 texel =
      textures[paint.texture_index].tex.sample(texture_sampler, in.uv);
  return Broadcast(ApplyColorFilter(filter, texel) * in.color);
}
