// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_COLOR_FILTER_GLSL_
#define PROPELLER_COLOR_FILTER_GLSL_

#include "blend.glsl"
#include "common.glsl"

/// A color filter, pushed per filtered-composite draw call. Float, and
/// not because the values need the range: the CPU memcpys this struct
/// in, so the layout has to match impeller::Matrix and friends.
#define PROPELLER_PUSH_COLOR_FILTER \
  int filter_kind;                  \
  int filter_reserved;              \
  vec2 filter_padding;              \
  vec4 filter_row0;                 \
  vec4 filter_row1;                 \
  vec4 filter_row2;                 \
  vec4 filter_row3;                 \
  vec4 filter_translate;

struct ColorFilterData {
  int kind;
  vec4 row0;
  vec4 row1;
  vec4 row2;
  vec4 row3;
  vec4 translate;
};

// The sRGB transfer functions. Float: pow() at half precision carries an
// error of a few parts per thousand, which is on the order of an 8-bit
// step once the curve steepens near black.
vec3 SrgbToLinear(vec3 v) {
  return select(v / 12.92, pow((v + 0.055) / 1.055, vec3(2.4)),
                greaterThan(v, vec3(0.04045)));
}

vec3 LinearToSrgb(vec3 v) {
  return select(v * 12.92, 1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055,
                greaterThan(v, vec3(0.0031308)));
}

/// `src` is premultiplied. Matrix and gamma filters are defined on
/// unpremultiplied components, so those round-trip through straight
/// alpha; blend filters are defined premultiplied.
half4 ApplyColorFilter(ColorFilterData f, half4 src) {
  if (f.kind == 1) {  // kBlend
    const half4 constant_premul =
        half4(half3(f.row0.rgb) * half(f.row0.a), half(f.row0.a));
    return BlendPremulFull(constant_premul, src);
  }
  half4 u = src.a > kHalfEpsilon ? half4(half3(src.rgb / src.a), src.a)
                                 : half4(0.0);
  if (f.kind == 2) {  // kMatrix
    const half4 c = u;
    u = half4(dot(half4(f.row0), c) + half(f.translate.x),
              dot(half4(f.row1), c) + half(f.translate.y),
              dot(half4(f.row2), c) + half(f.translate.z),
              dot(half4(f.row3), c) + half(f.translate.w));
    u = clamp(u, half4(0.0), half4(1.0));
  } else if (f.kind == 3) {  // kSrgbToLinear
    u.rgb = half3(SrgbToLinear(vec3(u.rgb)));
  } else if (f.kind == 4) {  // kLinearToSrgb
    u.rgb = half3(LinearToSrgb(vec3(u.rgb)));
  }
  return half4(half3(u.rgb * u.a), u.a);
}

#endif  // PROPELLER_COLOR_FILTER_GLSL_
