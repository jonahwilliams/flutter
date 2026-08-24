// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/renderer/shadow_lut.h"

#include <cmath>


namespace impeller {

bool ShadowLUT::Attach(GPUContext& context) {
  uint8_t lut[kShadowLUTWidth];
  auto cdf = [](float x) { return 0.5f * (1.0f + std::erf(x / 1.41421356f)); };
  const float lo = cdf(-2.0f);
  const float hi = cdf(2.0f);
  for (int i = 0; i < kShadowLUTWidth; i++) {
    const float t = static_cast<float>(i) / (kShadowLUTWidth - 1);
    const float coverage = (cdf(4.0f * t - 2.0f) - lo) / (hi - lo);
    lut[i] = static_cast<uint8_t>(std::lround(coverage * 255.0f));
  }
  staging_ = context.CreateBuffer(lut, sizeof(lut));
  if (staging_ == nullptr) {
    return false;
  }
  staging_->SetLabel("Propeller shadow LUT staging");
  texture_ = context.CreateTexture(
      TextureDesc{.format = TextureFormat::kR8UNorm,
                  .width = kShadowLUTWidth,
                  .height = 1},
      /*zeroed=*/false);
  return texture_ != nullptr;
}

void ShadowLUT::RecordUpload(GpuCommandBuffer& command_buffer) {
  command_buffer.UpdateRegion(*texture_, 0, 0, kShadowLUTWidth, 1,
                              *staging_, 0, kShadowLUTWidth);
  uploaded_ = true;
}

}  // namespace impeller
