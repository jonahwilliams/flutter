// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_SNAPSHOT_HOOK_H_
#define FLUTTER_IMPELLER_PROPELLER_SNAPSHOT_HOOK_H_

#include <functional>
#include <memory>

#include "flutter/display_list/image/dl_image.h"

namespace impeller {

class PrPicture;

/// The raster half of Picture.toImageSync: renders `picture` into a
/// fresh width x height texture and wraps it as an engine image.
using PropellerSnapshotHandler = std::function<sk_sp<flutter::DlImage>(
    const std::shared_ptr<PrPicture>& picture,
    uint32_t width,
    uint32_t height)>;

/// Backend engines register themselves when they come up. Raster thread
/// only, like the snapshots themselves.
void SetPropellerSnapshotHandler(PropellerSnapshotHandler handler);

/// Nullptr before any engine has registered, i.e. before the first
/// frame; the snapshot image then simply stays empty.
sk_sp<flutter::DlImage> PropellerSnapshotPicture(
    const std::shared_ptr<PrPicture>& picture,
    uint32_t width,
    uint32_t height);

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_SNAPSHOT_HOOK_H_
