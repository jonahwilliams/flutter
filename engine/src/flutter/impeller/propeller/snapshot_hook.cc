// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/snapshot_hook.h"

namespace impeller {

namespace {
PropellerSnapshotHandler& Handler() {
  static PropellerSnapshotHandler handler;
  return handler;
}
}  // namespace

void SetPropellerSnapshotHandler(PropellerSnapshotHandler handler) {
  Handler() = std::move(handler);
}

sk_sp<flutter::DlImage> PropellerSnapshotPicture(
    const std::shared_ptr<PrPicture>& picture,
    uint32_t width,
    uint32_t height) {
  const PropellerSnapshotHandler& handler = Handler();
  if (!handler) {
    return nullptr;
  }
  return handler(picture, width, height);
}

}  // namespace impeller
