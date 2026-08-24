// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_BLEND_GLSL_
#define PROPELLER_BLEND_GLSL_

#include "common.glsl"

// Blending is entirely colour arithmetic on 8-bit sourced operands, so it
// runs in half throughout. The only care needed is the guard epsilons:
// float's 1e-6 is subnormal as a half, so they use kHalfEpsilon.

// Advanced separable blend functions (unpremultiplied, per channel).
half3 BlendChannel3(int mode, half3 s, half3 d) {
  switch (mode) {
    case 15: {  // overlay = hardLight(d, s)
      const half3 lo = half(2.0) * s * d;
      const half3 hi =
          half3(1.0) - half(2.0) * (half3(1.0) - s) * (half3(1.0) - d);
      return select(hi, lo, lessThanEqual(d, half3(0.5)));
    }
    case 16: return min(s, d);                      // darken
    case 17: return max(s, d);                      // lighten
    case 18:                                        // colorDodge
      return select(min(half3(1.0), d / max(half3(1.0) - s, half3(kHalfEpsilon))),
                    half3(0.0), lessThanEqual(d, half3(0.0)));
    case 19:                                        // colorBurn
      return select(
          half3(1.0) -
              min(half3(1.0), (half3(1.0) - d) / max(s, half3(kHalfEpsilon))),
          half3(1.0), greaterThanEqual(d, half3(1.0)));
    case 20: {  // hardLight
      const half3 lo = half(2.0) * s * d;
      const half3 hi =
          half3(1.0) - half(2.0) * (half3(1.0) - s) * (half3(1.0) - d);
      return select(hi, lo, lessThanEqual(s, half3(0.5)));
    }
    case 21: {  // softLight (W3C)
      const half3 dd = select(
          sqrt(d), ((half(16.0) * d - half(12.0)) * d + half(4.0)) * d,
          lessThanEqual(d, half3(0.25)));
      const half3 lo = d - (half3(1.0) - half(2.0) * s) * d * (half3(1.0) - d);
      const half3 hi = d + (half(2.0) * s - half3(1.0)) * (dd - d);
      return select(hi, lo, lessThanEqual(s, half3(0.5)));
    }
    case 22: return abs(s - d);                     // difference
    case 23: return s + d - half(2.0) * s * d;      // exclusion
    case 24: return s * d;                          // multiply
  }
  return s;
}

// Non-separable HSL helpers (W3C).
half Luminosity3(half3 c) {
  return dot(c, half3(half(0.3), half(0.59), half(0.11)));
}
half3 ClipColor3(half3 c) {
  const half lum = Luminosity3(c);
  const half lo = min(c.r, min(c.g, c.b));
  const half hi = max(c.r, max(c.g, c.b));
  if (lo < half(0.0)) {
    c = lum + (c - lum) * lum / max(lum - lo, kHalfEpsilon);
  }
  if (hi > half(1.0)) {
    c = lum + (c - lum) * (half(1.0) - lum) / max(hi - lum, kHalfEpsilon);
  }
  return c;
}
half3 SetLuminosity3(half3 c, half lum) {
  return ClipColor3(c + (lum - Luminosity3(c)));
}
half Saturation3(half3 c) {
  return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}
half3 SetSaturation3(half3 c, half sat) {
  const half lo = min(c.r, min(c.g, c.b));
  const half hi = max(c.r, max(c.g, c.b));
  if (hi <= lo) {
    return half3(0.0);
  }
  return (c - lo) * sat / (hi - lo);
}

// The full premultiplied blend for the specialized mode: simple
// Porter-Duff for the coverage-unsafe coefficient modes, W3C compositing
// for the advanced ones. The switch folds to ONE case under the
// fetch_blend specialization constant.
half4 BlendPremult(half4 sp, half4 dp) {
  switch (fetch_blend) {
    case 0: return half4(0.0);                              // clear
    case 1: return sp;                                      // src
    case 5: return sp * dp.a;                               // srcIn
    case 6: return dp * sp.a;                               // dstIn
    case 7: return sp * (half(1.0) - dp.a);                 // srcOut
    case 10: return sp * (half(1.0) - dp.a) + dp * sp.a;    // dstATop
    case 13: return sp * dp;                                // modulate
  }
  // Advanced: unpremultiply, blend, recompose (W3C section 10).
  const half3 s =
      sp.a > kHalfEpsilon ? half3(sp.rgb / sp.a) : half3(0.0);
  const half3 d =
      dp.a > kHalfEpsilon ? half3(dp.rgb / dp.a) : half3(0.0);
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
  const half3 rgb = sp.rgb * (half(1.0) - dp.a) + dp.rgb * (half(1.0) - sp.a) +
                    sp.a * dp.a * blended;
  const half alpha = sp.a + dp.a * (half(1.0) - sp.a);
  return half4(rgb, alpha);
}

half4 BlendPremulFull(half4 sp, half4 dp) {
  switch (filter_blend) {
    case 0: return half4(0.0);                                    // clear
    case 1: return sp;                                            // src
    case 2: return dp;                                            // dst
    case 3: return sp + dp * (half(1.0) - sp.a);                  // srcOver
    case 4: return dp + sp * (half(1.0) - dp.a);                  // dstOver
    case 5: return sp * dp.a;                                     // srcIn
    case 6: return dp * sp.a;                                     // dstIn
    case 7: return sp * (half(1.0) - dp.a);                       // srcOut
    case 8: return dp * (half(1.0) - sp.a);                       // dstOut
    case 9: return sp * dp.a + dp * (half(1.0) - sp.a);           // srcATop
    case 10: return dp * sp.a + sp * (half(1.0) - dp.a);          // dstATop
    case 11: return sp * (half(1.0) - dp.a) + dp * (half(1.0) - sp.a);  // xor
    case 12: return min(sp + dp, half4(1.0));                     // plus
    case 13: return sp * dp;                                      // modulate
  }
  // Advanced: unpremultiply, blend per W3C section 10, recompose.
  const half3 s =
      sp.a > kHalfEpsilon ? half3(sp.rgb / sp.a) : half3(0.0);
  const half3 d =
      dp.a > kHalfEpsilon ? half3(dp.rgb / dp.a) : half3(0.0);
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
  const half3 rgb = sp.rgb * (half(1.0) - dp.a) + dp.rgb * (half(1.0) - sp.a) +
                    sp.a * dp.a * blended;
  return half4(rgb, sp.a + dp.a * (half(1.0) - sp.a));
}

#endif  // PROPELLER_BLEND_GLSL_
