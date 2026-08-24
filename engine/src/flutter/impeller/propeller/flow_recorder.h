// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_FLOW_RECORDER_H_
#define FLUTTER_IMPELLER_PROPELLER_FLOW_RECORDER_H_

#include "flutter/display_list/dl_canvas.h"

namespace flutter {
class Stopwatch;
class Layer;
}  // namespace flutter

namespace impeller {

/// Paint a flow layer tree into `builder`.
void RecordFlowTree(flutter::DlCanvas& builder,
                    const flutter::Layer& root,
                    const flutter::Stopwatch* raster_time = nullptr,
                    const flutter::Stopwatch* ui_time = nullptr);

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_FLOW_RECORDER_H_
