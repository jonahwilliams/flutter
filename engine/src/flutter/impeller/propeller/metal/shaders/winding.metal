// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common.h"

// Winding accumulation: signed coverage into the accumulator, the
// colour attachment masked off. The sign is the triangle's FACING, so an
// outward bulge adds and an inward dent subtracts with no CPU
// orientation test. A flat mesh has no implicit and carries its edge
// ramp in the vertex colour's alpha instead.
struct WindingOut {
  half w [[color(1)]];
};

fragment WindingOut WindingMain(Varyings in [[stage_in]],
                                bool front_facing [[front_facing]]) {
  half coverage = is_path ? half(PathCoverage(in.implicit)) : in.color.a;
  WindingOut out;
  out.w = front_facing ? coverage : -coverage;
  return out;
}

fragment FragmentOut WindingCompositeMain(
    Varyings in [[stage_in]],
    half w [[color(1)]],
    half clip [[color(2)]]) {
  FragmentOut out = Broadcast(WindingCoverage(w) * clip * in.color);
  out.w = 0.0h;
  return out;
}

// A clip shape's turn at the accumulator: what it summed becomes
// coverage in the clip attachment, and the accumulator is zeroed for
// whatever accumulates next.
struct ClipResolveOut {
  half w [[color(1)]];
  half clip [[color(2)]];
};

fragment ClipResolveOut ClipResolveMain(half w [[color(1)]]) {
  ClipResolveOut out;
  out.w = 0.0h;
  out.clip = WindingCoverage(w);
  return out;
}
