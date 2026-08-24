// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/flow_recorder.h"

#include "flutter/flow/layers/layer.h"
#include "flutter/flow/layers/layer_state_stack.h"
#include "flutter/flow/stopwatch.h"

namespace impeller {

void RecordFlowTree(flutter::DlCanvas& builder,
                    const flutter::Layer& root,
                    const flutter::Stopwatch* raster_time,
                    const flutter::Stopwatch* ui_time) {
  flutter::LayerStateStack state_stack;
  state_stack.set_delegate(&builder);

  static const flutter::FixedRefreshRateStopwatch kNoTiming;
  flutter::PaintContext context = {
      .state_stack = state_stack,
      .canvas = &builder,
      .gr_context = nullptr,
      .dst_color_space = nullptr,
      .view_embedder = nullptr,
      .raster_time = raster_time ? *raster_time : kNoTiming,
      .ui_time = ui_time ? *ui_time : kNoTiming,
      .texture_registry = nullptr,
      .raster_cache = nullptr,
      .impeller_enabled = true,
      .aiks_context = nullptr,
      // The scene this paints into takes pictures, not primitives.
      .propeller_enabled = true,
  };
  root.Paint(context);
}

}  // namespace impeller
