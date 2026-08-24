// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_PROPELLER_SHELL_HOOK_H_
#define FLUTTER_IMPELLER_PROPELLER_PROPELLER_SHELL_HOOK_H_

namespace flutter {
class Layer;
class Stopwatch;
}  // namespace flutter

namespace impeller {

class RenderTarget;

/// Renders a flow layer tree into the render target's color attachment via
/// Propeller.
///
/// Returns false if Propeller is not available.
bool PropellerRenderFlowTree(const flutter::Layer* root,
                             RenderTarget& target,
                             float device_pixel_ratio,
                             const flutter::Stopwatch* raster_time = nullptr,
                             const flutter::Stopwatch* ui_time = nullptr);

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_PROPELLER_SHELL_HOOK_H_
