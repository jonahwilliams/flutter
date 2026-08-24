// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/painting/picture_recorder.h"

#include "flutter/lib/ui/painting/canvas.h"
#include "flutter/lib/ui/painting/picture.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_args.h"
#include "third_party/tonic/dart_binding_macros.h"
#include "third_party/tonic/dart_library_natives.h"

namespace flutter {

IMPLEMENT_WRAPPERTYPEINFO(ui, PictureRecorder);

void PictureRecorder::Create(Dart_Handle wrapper) {
  UIDartState::ThrowIfUIOperationsProhibited();
  auto res = fml::MakeRefCounted<PictureRecorder>();
  res->AssociateWithDartWrapper(wrapper);
}

PictureRecorder::PictureRecorder() {}

PictureRecorder::~PictureRecorder() {}

std::shared_ptr<impeller::PrPictureBuilder> PictureRecorder::BeginRecording(
    DlRect bounds) {
  // The propeller recorder: pictures come out already in the scheduler's
  // form, and nothing converts later. Recording runs on the UI thread,
  // which is why gradients stay unresolved until flatten.
  builder_ = std::make_shared<impeller::PrPictureBuilder>(1.0f);
  builder_->SetSurfaceBounds(bounds);
  return builder_;
}

void PictureRecorder::endRecording(Dart_Handle dart_picture) {
  if (!canvas_) {
    return;
  }

  std::shared_ptr<impeller::PrPicture> picture = builder_->Build();
  builder_ = nullptr;

  Picture::CreateAndAssociateWithDartWrapper(dart_picture, std::move(picture));

  canvas_->Invalidate();
  canvas_ = nullptr;
  ClearDartWrapper();
}

}  // namespace flutter
