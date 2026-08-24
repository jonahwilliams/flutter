// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common.h"

// Clip coverage accumulator.
struct ClipOut {
  half clip [[color(2)]];
};

// Back to fully visible, over the region a popped clip masked.
fragment ClipOut ClipMain() {
  ClipOut out;
  out.clip = 1.0h;
  return out;
}
