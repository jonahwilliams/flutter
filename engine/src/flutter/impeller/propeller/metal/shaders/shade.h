// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_SHADE_H_
#define FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_SHADE_H_

#include "common.h"

// The pass table's reserved slots; see kGradientTextureSlot in
// buffer_arena.h.
constant int kGradientTextureSlot = 1;

constant float k1Over2Pi = 0.1591549430918;

constant uint kPaintFlagTextureIsCoverage = 1u;
constant uint kPaintFlagSampleNearest = 2u;

struct ShadeResult {
  /// The intended source color, premultiplied.
  half4 source;
  half coverage;
};

/// Sample the gradient texture at the given t.
static inline half4 SampleGradientRamp(GradientData gradient,
                                       texture2d<half> ramp,
                                       float raw_t) {
  float t;
  if (gradient.tile_mode == 1u) {
    t = fract(raw_t);
  } else if (gradient.tile_mode == 2u) {
    const float m = fract(raw_t * 0.5) * 2.0;
    t = 1.0 - abs(m - 1.0);
  } else {
    t = saturate(raw_t);
  }
  constexpr sampler gradient_sampler(coord::normalized, filter::linear,
                                     address::clamp_to_edge);
  const float v = (float(gradient.ramp_row) + 0.5) / float(ramp.get_height());
  const uint first_texel = gradient.ramp_span & 0xFFFFu;
  const uint texel_count = gradient.ramp_span >> 16;
  const float u = (float(first_texel) + 0.5 + t * float(texel_count - 1)) /
                  float(ramp.get_width());
  half4 color = ramp.sample(gradient_sampler, float2(u, v), level(0));
  if (gradient.tile_mode == 3u && (raw_t < 0.0 || raw_t > 1.0)) {
    color = half4(0.0h);
  }
  return color;
}

constexpr sampler nearest_sampler(coord::normalized,
                                  filter::nearest,
                                  address::clamp_to_edge);
constexpr sampler linear_sampler(coord::normalized,
                                 filter::linear,
                                 address::clamp_to_edge);

static inline ShadeResult ShadeParts(Varyings in,
                                     const device PaintData* paints,
                                     const device TextureRef* textures,
                                     const device GradientData* gradients,
                                     half clip_coverage) {
  half coverage = 1.0h;
  half4 color = in.color;
  const PaintData paint = paints[in.paint_index];
  if (is_path) {
    coverage = half(PathCoverage(in.implicit));
  }
  if (paint.texture_index >= 0) {
    const bool nearest = (paint.flags & kPaintFlagSampleNearest) != 0u;
    const texture2d<half> tex = textures[paint.texture_index].tex;
    if ((paint.flags & kPaintFlagTextureIsCoverage) != 0u) {
      coverage *= (nearest ? tex.sample(nearest_sampler, in.uv, level(0))
                           : tex.sample(linear_sampler, in.uv, level(0)))
                      .r;
    } else {
      color *= nearest ? tex.sample(nearest_sampler, in.uv)
                       : tex.sample(linear_sampler, in.uv);
    }
  }

  if (paint.gradient_index >= 0) {
    const GradientData gradient = gradients[paint.gradient_index];
    // Everything up to the ramp lookup is geometry in the gradient's own
    // space, so it stays float.
    const float2 local = in.local_position;
    const float2 p = float2(dot(gradient.inverse_basis.xy, local),
                            dot(gradient.inverse_basis.zw, local)) +
                     gradient.inverse_translation.xy;
    float t = 0.0;
    if (gradient.type == 0u) {
      // Linear: data = (start, end).
      const float2 start = gradient.data.xy;
      const float2 direction = gradient.data.zw - start;
      t = dot(p - start, direction) / max(dot(direction, direction), 1e-6);
    } else if (gradient.type == 1u) {
      // Radial: data = (center, radius).
      t = length(p - gradient.data.xy) / max(gradient.data.z, 1e-6);
    } else {
      // Conic: data = (center, bias, scale)
      const float2 d = p - gradient.data.xy + 0.001;
      t = (atan2(-d.y, -d.x) * k1Over2Pi + 0.5 + gradient.data.z) *
          gradient.data.w;
    }

    const texture2d<half> ramp = textures[kGradientTextureSlot].tex;
    color *= SampleGradientRamp(gradient, ramp, t);
  }
  ShadeResult result;
  result.source = color;
  result.coverage = coverage * clip_coverage;
  return result;
}

static inline half4 ShadeFragment(Varyings in,
                                  const device PaintData* paints,
                                  const device TextureRef* textures,
                                  const device GradientData* gradients,
                                  half clip_coverage) {
  const ShadeResult parts =
      ShadeParts(in, paints, textures, gradients, clip_coverage);
  return parts.source * parts.coverage;
}

#endif  // FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_SHADE_H_
