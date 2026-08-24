// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_GRADIENT_ATLAS_H_
#define FLUTTER_IMPELLER_PROPELLER_GRADIENT_ATLAS_H_

#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "flutter/display_list/effects/dl_color_sources.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/scalar.h"
#include "impeller/propeller/paged_atlas.h"
#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {

//------------------------------------------------------------------------------
/// Gradient ramp rows: a premultiplied RGBA8 texture, bilinearly sampled,
/// one row per gradient.
///
/// Ramps never move, so there is no generation.
class GradientAtlas {
 public:
  static constexpr int32_t kWidth = 512;
  static constexpr int32_t kMaxRows = 1024;

  /// Where a ramp lives. The gradient descriptor carries this to the
  /// shader, which maps t onto [first texel centre, last texel centre].
  struct RampLocation {
    uint32_t row = 0;
    uint32_t first_texel = 0;
    uint32_t texel_count = 0;

    /// first_texel in the low 16 bits, texel_count in the high 16 --
    /// the form the shader reads.
    uint32_t Pack() const {
      return (first_texel & 0xFFFFu) | (texel_count << 16);
    }

    bool operator==(const RampLocation& o) const {
      return row == o.row && first_texel == o.first_texel &&
             texel_count == o.texel_count;
    }
  };

  explicit GradientAtlas(GPUContext* context);

  ~GradientAtlas();

  GradientAtlas(const GradientAtlas&) = delete;
  GradientAtlas& operator=(const GradientAtlas&) = delete;

  /// Register the gradient in the atlas, returning the ramp location as long as
  /// there is room.
  std::optional<RampLocation> RegisterGradient(
      const std::shared_ptr<const flutter::DlColorSource>& source);

  /// Drop every ramp no retained picture referenced since the last call,
  /// and clear the marks for the next frame.
  void EvictUnused();

  /// Rasterize every ramp registered since the last call, into the rows
  /// they were given. `ramp` is the position-and-colour program: a ramp
  /// is stop colours interpolated across a row, which is all a vertex
  /// stage has to do.
  void RecordUploads(GpuCommandBuffer& command_buffer, const GPUProgram& ramp);

  GPUTexture* GetTexture() const;

 private:
  struct SourceEntry {
    std::shared_ptr<const flutter::DlColorSource> source;
    std::optional<RampLocation> location;
    bool used_this_frame = true;
  };

  std::unordered_map<const flutter::DlColorSource*, SourceEntry> cache_;
  std::bitset<kMaxRows> free_rows_;
  std::vector<SourceEntry> pending_;

  GPUContext* context_;
  std::unique_ptr<GPUTexture> texture_;
  static constexpr size_t kStagingBuffers = 3;
  std::unique_ptr<GPUBuffer> buffers_[kStagingBuffers];
  uint32_t buffer_capacities_[kStagingBuffers] = {};
  size_t next_buffer_ = 0;
  /// Whether the texture has been cleared yet.
  bool cleared_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_GRADIENT_ATLAS_H_
