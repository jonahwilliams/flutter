// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_VARYINGS_GLSL_
#define PROPELLER_VARYINGS_GLSL_

// The varying set every stage shares. GLSL declares I/O per stage, so
// the direction comes from the including file.
#ifdef PROPELLER_VERTEX
#define VARYING out
#else
#define VARYING in
#endif

layout(location = 0) VARYING vec2 v_uv;
/// Loop-Blinn implicit coordinate.
layout(location = 1) VARYING vec2 v_implicit;
/// Pre-transform recorded-space position.
layout(location = 2) VARYING vec2 v_local_position;
/// Unpacked from four bytes, so half is exact.
layout(location = 3) VARYING vec4 v_color;
layout(location = 4) flat VARYING uint v_paint_index;

#endif  // PROPELLER_VARYINGS_GLSL_
