// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <numeric>

#include "flutter/testing/test_swiftshader_utils.h"
#include "flutter/testing/testing.h"
#include "impeller/propeller/vulkan/vulkan_context.h"

namespace impeller {
namespace testing {

namespace {

std::shared_ptr<GPUContextVK> MakeContext() {
  flutter::testing::SetupSwiftshaderOnce(true);
  return GPUContextVK::Make();
}

}  // namespace

TEST(VulkanContextTest, UploadsAndReadsBackTexels) {
  auto context = MakeContext();
  if (!context) {
    GTEST_SKIP() << "No Vulkan driver.";
  }

  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 4;
  std::vector<uint8_t> texels(kWidth * kHeight * 4);
  std::iota(texels.begin(), texels.end(), 0);
  auto staging = context->CreateBuffer(texels.data(), texels.size());
  ASSERT_NE(staging, nullptr);

  auto texture = context->CreateTexture(
      TextureDesc{.format = TextureFormat::kRGBA8UNorm,
                  .width = kWidth,
                  .height = kHeight},
      /*zeroed=*/false);
  ASSERT_NE(texture, nullptr);

  auto commands = context->CreateCommandBuffer();
  ASSERT_NE(commands, nullptr);
  commands->UpdateRegion(*texture, 0, 0, kWidth, kHeight, *staging, 0,
                         kWidth * 4);
  ASSERT_TRUE(context->SubmitAndWait(*commands));

  EXPECT_EQ(context->ReadbackTexture(*texture), texels);
}

TEST(VulkanContextTest, RenderPassClearsTheTarget) {
  auto context = MakeContext();
  if (!context) {
    GTEST_SKIP() << "No Vulkan driver.";
  }
  auto target = context->CreateTexture(
      TextureDesc{.format = TextureFormat::kRGBA8UNorm,
                  .width = 8,
                  .height = 8},
      /*zeroed=*/false);
  ASSERT_NE(target, nullptr);
  auto canvas =
      context->CreateTransientTexture(TextureFormat::kRGBA8UNorm, 8, 8);
  auto winding =
      context->CreateTransientTexture(TextureFormat::kR16Float, 8, 8);
  ASSERT_NE(canvas, nullptr);
  ASSERT_NE(winding, nullptr);
  GPUTexture* const transients[2] = {canvas.get(), winding.get()};
  auto commands = context->CreateCommandBuffer();
  ASSERT_NE(commands, nullptr);
  RenderPassDesc desc;
  desc.target = target.get();
  desc.clear_color[0] = 1.0f;
  desc.clear_color[3] = 1.0f;
  desc.transients = transients;
  desc.transient_count = 2;
  desc.label = "clear probe";
  commands->StartRenderPass(desc);
  commands->EndRenderPass();
  ASSERT_TRUE(context->SubmitAndWait(*commands));
  const std::vector<uint8_t> pixels = context->ReadbackTexture(*target);
  ASSERT_EQ(pixels.size(), 8u * 8 * 4);
  EXPECT_EQ(pixels[0], 255);
  EXPECT_EQ(pixels[1], 0);
  EXPECT_EQ(pixels[3], 255);
}

TEST(VulkanContextTest, SubRectUploadLandsAtItsOffset) {
  auto context = MakeContext();
  if (!context) {
    GTEST_SKIP() << "No Vulkan driver.";
  }

  auto texture = context->CreateTexture(
      TextureDesc{.format = TextureFormat::kR8UNorm, .width = 8, .height = 8},
      /*zeroed=*/true);
  ASSERT_NE(texture, nullptr);

  // A 2x2 patch inside a 4-texel-wide source image: exercises the stride,
  // which Vulkan counts in texels where Metal counts bytes.
  const uint8_t patch[8] = {1, 2, 0, 0, 3, 4, 0, 0};
  auto staging = context->CreateBuffer(patch, sizeof(patch));
  auto commands = context->CreateCommandBuffer();
  ASSERT_NE(commands, nullptr);
  commands->UpdateRegion(*texture, 5, 6, 2, 2, *staging, 0, 4);
  ASSERT_TRUE(context->SubmitAndWait(*commands));

  const std::vector<uint8_t> pixels = context->ReadbackTexture(*texture);
  ASSERT_EQ(pixels.size(), 64u);
  EXPECT_EQ(pixels[6 * 8 + 5], 1);
  EXPECT_EQ(pixels[6 * 8 + 6], 2);
  EXPECT_EQ(pixels[7 * 8 + 5], 3);
  EXPECT_EQ(pixels[7 * 8 + 6], 4);
  // The zeroed clear held everywhere else.
  EXPECT_EQ(pixels[6 * 8 + 4], 0);
  EXPECT_EQ(pixels[5 * 8 + 5], 0);
  EXPECT_EQ(std::accumulate(pixels.begin(), pixels.end(), 0), 1 + 2 + 3 + 4);
}

}  // namespace testing
}  // namespace impeller
