// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_SHADE_GLSL_
#define PROPELLER_SHADE_GLSL_

#include "buffers.glsl"
#include "common.glsl"
#include "coverage.glsl"
#include "varyings.glsl"

#define k1Over2Pi 0.1591549430918

#define kPaintFlagTextureIsCoverage 1u
#define kPaintFlagSampleNearest 2u

struct ShadeResult {
  /// The intended source color (premultiplied, opacity applied).
  half4 source;
  /// Geometric + texture + clip coverage: how much of the pixel the
  /// source occupies. Fixed-function paths pre-multiply source by this;
  /// fetch-blend paths lerp with it instead (required for blend modes
  /// whose dst factor does not scale with source alpha).
  half coverage;
};

/// One tap of a gradient ramp: tile the interpolant, then read the row.
///
/// The interpolant and the texel address stay float -- the ramp atlas is
/// thousands of texels wide, so a half `u` would land on the wrong stop --
/// and only the sampled colour comes back as half.
half4 SampleGradientRamp(GradientData gradient, float raw_t) {
  float t;
  if (gradient.tile_mode == 1u) {
    t = fract(raw_t);
  } else if (gradient.tile_mode == 2u) {
    const float m = fract(raw_t * 0.5) * 2.0;
    t = 1.0 - abs(m - 1.0);
  } else {
    t = saturate(raw_t);
  }
  const vec2 ramp_size = vec2(textureSize(
      sampler2D(textures[kGradientTextureSlot], linear_sampler), 0));
  const float v = (float(gradient.ramp_row) + 0.5) / ramp_size.y;
  const uint first_texel = gradient.ramp_span & 0xFFFFu;
  const uint texel_count = gradient.ramp_span >> 16;
  const float u =
      (float(first_texel) + 0.5 + t * float(texel_count - 1u)) / ramp_size.x;
  half4 color = half4(
      textureLod(sampler2D(textures[kGradientTextureSlot], linear_sampler),
                 vec2(u, v), 0.0));
  if (gradient.tile_mode == 3u && (raw_t < 0.0 || raw_t > 1.0)) {
    color = half4(0.0);
  }
  return color;
}

ShadeResult ShadeParts(float opacity) {
  half coverage = half(1.0);
  half4 color = half4(v_color);
  const PaintData paint = paints[v_paint_index];

  half clip_coverage = half(1.0);
  if (is_path) {
    coverage = half(PathCoverage(v_implicit));
  }
  if (paint.texture_index >= 0) {
    // The paint decides the texture's semantics. Which filter to sample
    // with is the paint's to say: glyphs landing on whole device pixels
    // want nearest, the same glyphs under a rotation want bilinear, and
    // an image asks for whichever it was drawn with.
    const bool nearest = (paint.flags & kPaintFlagSampleNearest) != 0u;
    if ((paint.flags & kPaintFlagTextureIsCoverage) != 0u) {
      // Coverage: typically glyphs.
      const vec4 texel =
          nearest ? textureLod(
                        sampler2D(textures[nonuniformEXT(paint.texture_index)],
                                  nearest_sampler),
                        v_uv, 0.0)
                  : textureLod(
                        sampler2D(textures[nonuniformEXT(paint.texture_index)],
                                  linear_sampler),
                        v_uv, 0.0);
      coverage *= half(texel.r);
    } else {
      // Color.
      const vec4 texel =
          nearest
              ? texture(sampler2D(textures[nonuniformEXT(paint.texture_index)],
                                  nearest_sampler),
                        v_uv)
              : texture(sampler2D(textures[nonuniformEXT(paint.texture_index)],
                                  linear_sampler),
                        v_uv);
      color *= half4(texel);
    }
  }

  if (paint.gradient_index >= 0) {
    const GradientData gradient = gradients[paint.gradient_index];
    // Everything up to the ramp lookup is geometry in the gradient's own
    // space, so it stays float.
    const vec2 local = v_local_position;
    const vec2 p = vec2(dot(gradient.inverse_basis.xy, local),
                        dot(gradient.inverse_basis.zw, local)) +
                   gradient.inverse_translation.xy;
    float t = 0.0;
    if (gradient.type == 0u) {
      // Linear: data = (start, end).
      const vec2 start = gradient.data.xy;
      const vec2 direction = gradient.data.zw - start;
      t = dot(p - start, direction) / max(dot(direction, direction), 1e-6);
    } else if (gradient.type == 1u) {
      // Radial: data = (center, radius).
      t = length(p - gradient.data.xy) / max(gradient.data.z, 1e-6);
    } else {
      // Conic: data = (center, bias, scale)
      const vec2 d = p - gradient.data.xy + 0.001;
      t = (atan(-d.y, -d.x) * k1Over2Pi + 0.5 + gradient.data.z) *
          gradient.data.w;
    }
    color *= SampleGradientRamp(gradient, t);
  }
  ShadeResult result;
  result.source = color * half(opacity);
  result.coverage = coverage * clip_coverage;
  return result;
}

half4 ShadeFragment(float opacity) {
  const ShadeResult parts = ShadeParts(opacity);
  return parts.source * parts.coverage;
}

#endif  // PROPELLER_SHADE_GLSL_
