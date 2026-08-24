// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_TYPES_GLSL_
#define PROPELLER_TYPES_GLSL_

// Precision policy: colour and coverage are half, geometry is float.
// `half` compiles as real float16 when PROPELLER_HALF is defined (devices
// with shaderFloat16) and as float otherwise, so one source serves both.
// Every half literal is written through the constructor -- half(0.5) --
// which is valid in either mode.

#ifdef PROPELLER_HALF
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#define half float16_t
#define half2 f16vec2
#define half3 f16vec3
#define half4 f16vec4
#else
#define half float
#define half2 vec2
#define half3 vec3
#define half4 vec4
#endif

#define saturate(x) clamp(x, 0.0, 1.0)
#define saturateh(x) clamp(x, half(0.0), half(1.0))

// MSL select(a, b, cond) is cond ? b : a per component, which is GLSL's
// mix with a boolean vector.
#define select(a, b, cond) mix(a, b, cond)

#endif  // PROPELLER_TYPES_GLSL_
