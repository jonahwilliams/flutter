// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/gradient_atlas.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "flutter/display_list/effects/dl_color_sources.h"
#include "flutter/fml/logging.h"
#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {

namespace {

/// Matches RampVertex in metal/shaders/ramp.metal.
struct RampVertex {
  float x = 0;
  float y = 0;
  uint32_t color;
};
static_assert(sizeof(RampVertex) == 12);

const flutter::DlGradientColorSourceBase* AsGradient(
    const flutter::DlColorSource& source) {
  if (const auto* linear = source.asLinearGradient()) {
    return linear;
  }
  if (const auto* radial = source.asRadialGradient()) {
    return radial;
  }
  if (const auto* sweep = source.asSweepGradient()) {
    return sweep;
  }
  return nullptr;
}

/// Where the stop at `t` lands across the row, in clip space.
float StopX(Scalar t) {
  if (t <= 0.0f) {
    return -1.0f;
  }
  if (t >= 1.0f) {
    return 1.0f;
  }
  const float texel = 0.5f + t * (GradientAtlas::kWidth - 1);
  return 2.0f * texel / GradientAtlas::kWidth - 1.0f;
}

}  // namespace

GradientAtlas::GradientAtlas(GPUContext* context) : context_(context) {
  free_rows_.set();
  texture_ = context_->CreateTexture(
      TextureDesc{
          .format = TextureFormat::kRGBA8UNorm,
          .width = kWidth,
          .height = kMaxRows,
      },
      /*zeroed=*/false);
  FML_DCHECK(!!texture_);
}

GradientAtlas::~GradientAtlas() = default;

std::optional<GradientAtlas::RampLocation> GradientAtlas::RegisterGradient(
    const std::shared_ptr<const flutter::DlColorSource>& source) {
  if (!source) {
    return std::nullopt;
  }
  auto it = cache_.find(source.get());
  if (it != cache_.end()) {
    it->second.used_this_frame = true;
    return it->second.location;
  }

  size_t row = kMaxRows;
  for (size_t candidate = 0; candidate < kMaxRows; candidate++) {
    if (free_rows_[candidate]) {
      row = candidate;
      break;
    }
  }
  if (row == kMaxRows) {
    // Atlas is full.
    return std::nullopt;
  }
  free_rows_[row] = false;

  const RampLocation location{
      .row = static_cast<uint32_t>(row),
      .first_texel = 0,
      .texel_count = kWidth,
  };
  SourceEntry entry{
      .source = source,
      .location = location,
      .used_this_frame = true,
  };
  cache_.emplace(source.get(), entry);
  pending_.push_back(entry);
  return location;
}

void GradientAtlas::RecordUploads(GpuCommandBuffer& command_buffer,
                                  const GPUProgram& ramp) {
  if (pending_.empty()) {
    return;
  }

  struct Run {
    uint32_t start = 0;
    uint32_t count = 0;
  };

  // One vertex per stop and one draw per pending.
  uint32_t vertex_count = 0;
  uint32_t draw_count = 0;
  for (const SourceEntry& source : pending_) {
    const flutter::DlGradientColorSourceBase* gradient =
        AsGradient(*source.source);
    FML_DCHECK(!!gradient);
    draw_count += 1;
    vertex_count += gradient->stop_count();
  }

  if (vertex_count * sizeof(RampVertex) > buffer_capacities_[next_buffer_]) {
    // No enough space, allocate a new buffer. For later, we need to
    // hold this old buffer and destroy it later for vulkan.
    auto size = buffer_capacities_[next_buffer_] =
        vertex_count * sizeof(RampVertex);
    buffers_[next_buffer_] = context_->CreateBuffer(nullptr, size);
  }
  RampVertex* vertex_out =
      reinterpret_cast<RampVertex*>(buffers_[next_buffer_]->Contents());
  uint32_t v_i = 0;

  std::vector<Run> ramps;
  ramps.reserve(draw_count);
  for (const SourceEntry& source : pending_) {
    const flutter::DlGradientColorSourceBase* gradient =
        AsGradient(*source.source);
    FML_DCHECK(!!gradient);

    const uint32_t row = source.location->row;
    const float center =
        1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / kMaxRows;

    const int stop_count = gradient->stop_count();
    const flutter::DlColor* colors = gradient->colors();
    const float* stops = gradient->stops();

    ramps.push_back(Run{
        .start = static_cast<uint32_t>(v_i),
        .count = static_cast<uint32_t>(stop_count),
    });
    for (int stop = 0; stop < stop_count; stop++) {
      vertex_out[v_i++] =
          RampVertex{.x = StopX(stops[stop]),
                     .y = center,
                     .color = colors[stop].unpremultipliedRGBA()};
    }
  }

  // Clear initial render pass, then subsequent updates load to
  // preserve.
  RenderPassDesc desc;
  desc.target = texture_.get();
  desc.load = cleared_;
  desc.label = "Propeller gradient ramps";
  cleared_ = true;

  command_buffer.StartRenderPass(desc);
  command_buffer.SetRenderPipeline(ramp);
  command_buffer.SetBuffer(GPUShaderStage::kVertex, *buffers_[next_buffer_], 0,
                           0);
  for (const Run& ramp_run : ramps) {
    command_buffer.DrawLines(static_cast<int>(ramp_run.start),
                             static_cast<int>(ramp_run.count));
  }
  command_buffer.EndRenderPass();
  next_buffer_ = (next_buffer_ + 1) % kStagingBuffers;
  pending_.clear();
}

GPUTexture* GradientAtlas::GetTexture() const {
  return texture_.get();
}

void GradientAtlas::EvictUnused() {
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->second.used_this_frame) {
      it->second.used_this_frame = false;
      ++it;
      continue;
    }
    if (it->second.location.has_value()) {
      free_rows_[it->second.location->row] = true;
    }
    it = cache_.erase(it);
  }
}

}  // namespace impeller
