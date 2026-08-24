// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_RENDERER_SHADOW_LUT_H_
#define FLUTTER_IMPELLER_PROPELLER_RENDERER_SHADOW_LUT_H_

#include <memory>

#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {

/// Texels across the penumbra band the LUT covers.
static constexpr int kShadowLUTWidth = 256;

/// The 1xkShadowLUTWidth Gaussian coverage LUT for mesh shadows: a
/// normalized Gaussian CDF across the penumbra band [-2sigma, +2sigma]
/// (0 at the outer edge, 0.5 at the silhouette, 1 inside). Shadow meshes
/// sample it as a coverage texture.
class ShadowLUT {
 public:
  /// Create the texture and stage its texels. False when the device
  /// refuses the allocation.
  bool Attach(GPUContext& context);

  [[nodiscard]] bool HasPendingUpload() const { return !uploaded_; }

  /// Copy the staged texels into the texture. Once. The staging buffer
  /// stays alive: the recorded copy reads it at execution, which is
  /// after this returns, and it is 256 bytes.
  void RecordUpload(GpuCommandBuffer& command_buffer);

  [[nodiscard]] GPUTexture* GetTexture() const { return texture_.get(); }

 private:
  std::unique_ptr<GPUTexture> texture_;
  std::unique_ptr<GPUBuffer> staging_;
  bool uploaded_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_RENDERER_SHADOW_LUT_H_
