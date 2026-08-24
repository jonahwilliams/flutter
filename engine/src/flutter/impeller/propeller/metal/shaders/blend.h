// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_BLEND_H_
#define FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_BLEND_H_

#include "common.h"

// Blending is entirely colour arithmetic on 8-bit sourced operands, so it
// runs in half throughout. The only care needed is the guard epsilons:
// float's 1e-6 is subnormal as a half, so they use kHalfEpsilon.

// Advanced separable blend functions (unpremultiplied, per channel).
static inline half3 BlendChannel3(int mode, half3 s, half3 d) {
  // clang-format off
  switch (mode) {
    case 15: {  // overlay = hardLight(d, s)
      const half3 lo = 2.0h * s * d;
      const half3 hi = 1.0h - 2.0h * (1.0h - s) * (1.0h - d);
      return select(hi, lo, d <= 0.5h);
    }
    case 16: return min(s, d);                      // darken
    case 17: return max(s, d);                      // lighten
    case 18:                                        // colorDodge
      return select(min(half3(1.0h), d / max(1.0h - s, kHalfEpsilon)),
                    half3(0.0h), d <= 0.0h);
    case 19:                                        // colorBurn
      return select(1.0h - min(half3(1.0h), (1.0h - d) / max(s, kHalfEpsilon)),
                    half3(1.0h), d >= 1.0h);
    case 20: {  // hardLight
      const half3 lo = 2.0h * s * d;
      const half3 hi = 1.0h - 2.0h * (1.0h - s) * (1.0h - d);
      return select(hi, lo, s <= 0.5h);
    }
    case 21: {  // softLight (W3C)
      const half3 dd =
          select(sqrt(d), ((16.0h * d - 12.0h) * d + 4.0h) * d, d <= 0.25h);
      const half3 lo = d - (1.0h - 2.0h * s) * d * (1.0h - d);
      const half3 hi = d + (2.0h * s - 1.0h) * (dd - d);
      return select(hi, lo, s <= 0.5h);
    }
    case 22: return abs(s - d);                     // difference
    case 23: return s + d - 2.0h * s * d;           // exclusion
    case 24: return s * d;                          // multiply
  }
  // clang-format on
  return s;
}

// Non-separable HSL helpers (W3C).
static inline half Luminosity3(half3 c) {
  return dot(c, half3(0.3h, 0.59h, 0.11h));
}
static inline half3 ClipColor3(half3 c) {
  const half lum = Luminosity3(c);
  const half lo = min(c.r, min(c.g, c.b));
  const half hi = max(c.r, max(c.g, c.b));
  if (lo < 0.0h) {
    c = lum + (c - lum) * lum / max(lum - lo, kHalfEpsilon);
  }
  if (hi > 1.0h) {
    c = lum + (c - lum) * (1.0h - lum) / max(hi - lum, kHalfEpsilon);
  }
  return c;
}
static inline half3 SetLuminosity3(half3 c, half lum) {
  return ClipColor3(c + (lum - Luminosity3(c)));
}
static inline half Saturation3(half3 c) {
  return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}
static inline half3 SetSaturation3(half3 c, half sat) {
  const half lo = min(c.r, min(c.g, c.b));
  const half hi = max(c.r, max(c.g, c.b));
  if (hi <= lo) {
    return half3(0.0h);
  }
  return (c - lo) * sat / (hi - lo);
}

// The full premultiplied blend for the specialized mode: simple
// Porter-Duff for the coverage-unsafe coefficient modes, W3C compositing
// for the advanced ones. The switch folds to ONE case under the
// fetch_blend function constant.
static inline half4 BlendPremult(half4 sp, half4 dp) {
  // clang-format off
  switch (fetch_blend) {
    case 0: return half4(0.0h);                      // clear
    case 1: return sp;                               // src
    case 5: return sp * dp.a;                        // srcIn
    case 6: return dp * sp.a;                        // dstIn
    case 7: return sp * (1.0h - dp.a);               // srcOut
    case 10: return sp * (1.0h - dp.a) + dp * sp.a;  // dstATop
    case 13: return sp * dp;                         // modulate
  }
  // clang-format on
  // Advanced: unpremultiply, blend, recompose (W3C section 10).
  const half3 s = sp.a > kHalfEpsilon ? sp.rgb / sp.a : half3(0.0h);
  const half3 d = dp.a > kHalfEpsilon ? dp.rgb / dp.a : half3(0.0h);
  half3 blended;
  switch (fetch_blend) {
    case 25:  // hue
      blended =
          SetLuminosity3(SetSaturation3(s, Saturation3(d)), Luminosity3(d));
      break;
    case 26:  // saturation
      blended =
          SetLuminosity3(SetSaturation3(d, Saturation3(s)), Luminosity3(d));
      break;
    case 27:  // color
      blended = SetLuminosity3(s, Luminosity3(d));
      break;
    case 28:  // luminosity
      blended = SetLuminosity3(d, Luminosity3(s));
      break;
    default:
      blended = BlendChannel3(fetch_blend, s, d);
      break;
  }
  const half3 rgb =
      sp.rgb * (1.0h - dp.a) + dp.rgb * (1.0h - sp.a) + sp.a * dp.a * blended;
  const half alpha = sp.a + dp.a * (1.0h - sp.a);
  return half4(rgb, alpha);
}

static inline half4 BlendPremulFull(half4 sp, half4 dp) {
  // clang-format off
  switch (filter_blend) {
    case 0: return half4(0.0h);                             // clear
    case 1: return sp;                                      // src
    case 2: return dp;                                      // dst
    case 3: return sp + dp * (1.0h - sp.a);                 // srcOver
    case 4: return dp + sp * (1.0h - dp.a);                 // dstOver
    case 5: return sp * dp.a;                               // srcIn
    case 6: return dp * sp.a;                               // dstIn
    case 7: return sp * (1.0h - dp.a);                      // srcOut
    case 8: return dp * (1.0h - sp.a);                      // dstOut
    case 9: return sp * dp.a + dp * (1.0h - sp.a);          // srcATop
    case 10: return dp * sp.a + sp * (1.0h - dp.a);         // dstATop
    case 11: return sp * (1.0h - dp.a) + dp * (1.0h - sp.a);  // xor
    case 12: return min(sp + dp, half4(1.0h));              // plus
    case 13: return sp * dp;                                // modulate
  }
  // clang-format on
  // Advanced: unpremultiply, blend per W3C section 10, recompose.
  const half3 s = sp.a > kHalfEpsilon ? sp.rgb / sp.a : half3(0.0h);
  const half3 d = dp.a > kHalfEpsilon ? dp.rgb / dp.a : half3(0.0h);
  half3 blended;
  switch (filter_blend) {
    case 14:
      blended = s + d - s * d;
      break;  // screen
    case 25:
      blended =
          SetLuminosity3(SetSaturation3(s, Saturation3(d)), Luminosity3(d));
      break;
    case 26:
      blended =
          SetLuminosity3(SetSaturation3(d, Saturation3(s)), Luminosity3(d));
      break;
    case 27:
      blended = SetLuminosity3(s, Luminosity3(d));
      break;
    case 28:
      blended = SetLuminosity3(d, Luminosity3(s));
      break;
    default:
      blended = BlendChannel3(filter_blend, s, d);
      break;
  }
  const half3 rgb =
      sp.rgb * (1.0h - dp.a) + dp.rgb * (1.0h - sp.a) + sp.a * dp.a * blended;
  return half4(rgb, sp.a + dp.a * (1.0h - sp.a));
}

#endif  // FLUTTER_IMPELLER_PROPELLER_METAL_SHADERS_BLEND_H_
