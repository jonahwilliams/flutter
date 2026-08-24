// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// GPU runtime smoke test through the REAL VulkanRenderer on SwiftShader:
// the same picture pipeline as the Metal tests, full canvas mode.

#include <cmath>

#include "flutter/display_list/geometry/dl_path_builder.h"

#include "flutter/testing/test_swiftshader_utils.h"
#include "flutter/testing/testing.h"
#include "impeller/propeller/vulkan/vulkan_renderer.h"

namespace impeller {
namespace testing {

namespace {

float HalfToFloat(uint16_t half) {
  const uint32_t sign = (half >> 15) & 1;
  const uint32_t exponent = (half >> 10) & 0x1f;
  const uint32_t mantissa = half & 0x3ff;
  if (exponent == 0) {
    return (sign ? -1.0f : 1.0f) * std::ldexp(mantissa, -24);
  }
  if (exponent == 31) {
    return sign ? -INFINITY : INFINITY;
  }
  return (sign ? -1.0f : 1.0f) *
         std::ldexp(1024.0f + mantissa, static_cast<int>(exponent) - 25);
}

struct Harness {
  std::shared_ptr<GPUContextVK> context;
  std::unique_ptr<VulkanRenderer> renderer;

  Color Get(const std::vector<uint8_t>& pixels,
            uint32_t width,
            int32_t x,
            int32_t y) const {
    const uint8_t* p = pixels.data() + (y * width + x) * 4;
    return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
  }
};

Harness MakeHarness() {
  flutter::testing::SetupSwiftshaderOnce(true);
  Harness harness;
  harness.context = GPUContextVK::Make();
  if (!harness.context) {
    return harness;
  }
  auto atlas = std::make_shared<PagedAtlas>(
      harness.context.get(), TextureFormat::kR8UNorm, 512, 512, 4);
  harness.renderer = VulkanRenderer::Make(harness.context, atlas);
  return harness;
}

}  // namespace

namespace {

flutter::DlPath Star(Scalar cx,
                     Scalar cy,
                     Scalar r,
                     flutter::DlPathFillType fill) {
  flutter::DlPathBuilder path;
  path.SetFillType(fill);
  for (int i = 0; i < 5; i++) {
    const Scalar angle = -1.5708f + i * 4.0f * 3.14159265f / 5.0f;
    const Point p(cx + r * std::cos(angle), cy + r * std::sin(angle));
    if (i == 0) {
      path.MoveTo(p);
    } else {
      path.LineTo(p);
    }
  }
  path.Close();
  return path.TakePath();
}

}  // namespace

TEST(VulkanRendererTest, FpTargetClearsToExactValues) {
  Harness harness = MakeHarness();
  if (!harness.context) {
    GTEST_SKIP() << "No Vulkan driver.";
  }

  // The accumulator's ground truth: an R16Float target holds what the
  // pass cleared it to, and readback returns halfs, not normalized
  // bytes.
  auto target = harness.context->CreateTexture(
      TextureDesc{.format = TextureFormat::kR16Float,
                  .width = 8,
                  .height = 8},
      /*zeroed=*/false);
  ASSERT_NE(target, nullptr);
  auto canvas = harness.context->CreateTransientTexture(
      TextureFormat::kRGBA8UNorm, 8, 8);
  auto winding =
      harness.context->CreateTransientTexture(TextureFormat::kR16Float, 8, 8);
  ASSERT_NE(canvas, nullptr);
  ASSERT_NE(winding, nullptr);
  GPUTexture* const transients[2] = {canvas.get(), winding.get()};
  auto commands = harness.context->CreateCommandBuffer();
  ASSERT_NE(commands, nullptr);
  RenderPassDesc desc;
  desc.target = target.get();
  desc.clear_color[0] = -2.5f;
  desc.transients = transients;
  desc.transient_count = 2;
  desc.label = "fp clear probe";
  commands->StartRenderPass(desc);
  commands->EndRenderPass();
  ASSERT_TRUE(harness.context->SubmitAndWait(*commands));

  const std::vector<uint8_t> bytes =
      harness.context->ReadbackTexture(*target);
  ASSERT_EQ(bytes.size(), 8u * 8 * 2);
  const auto* halves = reinterpret_cast<const uint16_t*>(bytes.data());
  // Negative and out of unorm range: float semantics or nothing.
  EXPECT_NEAR(HalfToFloat(halves[0]), -2.5f, 0.01f);
  EXPECT_NEAR(HalfToFloat(halves[63]), -2.5f, 0.01f);
}

}  // namespace testing
}  // namespace impeller
