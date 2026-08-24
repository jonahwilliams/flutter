// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_COLOR_FILTER_H_
#define FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_COLOR_FILTER_H_

#include "blend.h"
#include "common.h"

/// A color filter, uploaded per filtered-composite draw call.
///
/// Float, and not because the values need the range: the CPU memcpys this
/// struct in, so the layout has to match impeller::Matrix and friends.
struct ColorFilterData {
  /// GPUColorFilterKind.
  int kind;
  /// Padding; the blend mode is a specialization constant.
  int reserved;
  /// kMatrix: rows of RGBA coefficients plus the translation column.
  /// kBlend: row0 is the constant colour, UNPREMULTIPLIED.
  float4 row0;
  float4 row1;
  float4 row2;
  float4 row3;
  float4 translate;
};

// The sRGB transfer functions.
static inline float3 SrgbToLinear(float3 v) {
  return select(v / 12.92, pow((v + 0.055) / 1.055, 2.4), v > 0.04045);
}

static inline float3 LinearToSrgb(float3 v) {
  return select(v * 12.92, 1.055 * pow(v, 1.0 / 2.4) - 0.055, v > 0.0031308);
}

/// `src` is premultiplied.
///
/// Matrix and gamma filters are defined on unpremultiplied components, so those
/// round-trip through straight alpha.
/// blend filters are defined premultiplied.
static inline half4 ApplyColorFilter(ColorFilterData f, half4 src) {
  if (f.kind == 1) {  // kBlend
    const half4 constant_premul =
        half4(half3(f.row0.rgb) * half(f.row0.a), half(f.row0.a));
    return BlendPremulFull(constant_premul, src);
  }
  half4 u = src.a > kHalfEpsilon ? half4(src.rgb / src.a, src.a) : half4(0.0h);
  if (f.kind == 2) {  // kMatrix
    const half4 c = u;
    u = half4(dot(half4(f.row0), c) + half(f.translate.x),
              dot(half4(f.row1), c) + half(f.translate.y),
              dot(half4(f.row2), c) + half(f.translate.z),
              dot(half4(f.row3), c) + half(f.translate.w));
    u = clamp(u, 0.0h, 1.0h);
  } else if (f.kind == 3) {  // kSrgbToLinear
    u.rgb = half3(SrgbToLinear(float3(u.rgb)));
  } else if (f.kind == 4) {  // kLinearToSrgb
    u.rgb = half3(LinearToSrgb(float3(u.rgb)));
  }
  return half4(u.rgb * u.a, u.a);
}

#endif  // FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_COLOR_FILTER_H_
