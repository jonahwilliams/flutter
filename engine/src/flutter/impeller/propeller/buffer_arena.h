// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_BUFFER_ARENA_H_
#define FLUTTER_IMPELLER_PROPELLER_BUFFER_ARENA_H_

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "display_list/effects/color_sources/dl_linear_gradient_color_source.h"
#include "display_list/effects/color_sources/dl_radial_gradient_color_source.h"
#include "display_list/effects/color_sources/dl_sweep_gradient_color_source.h"
#include "flutter/display_list/effects/dl_color_source.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/rect.h"
#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {

/// Matches VertexAttributes in common.glsl and metal/shaders/common.h.
struct Attributes {
  Point uv;
  uint32_t color = 0;
  uint32_t paint = 0;
};
static_assert(sizeof(Attributes) == 16);

/// The slots every pass's texture table reserves before a draw names
/// one of its own. Keep in sync with the same names in shade.glsl and
/// metal/shaders/shade.h.
static constexpr int32_t kGlyphAtlasTextureSlot = 0;
static constexpr int32_t kGradientTextureSlot = 1;
/// What the first draw-owned texture gets, and how many entries a pass
/// starts its table with.
static constexpr size_t kReservedTextureSlots = 2;

/// What `PrPaint::flags` can carry. Keep in sync with the same names in
/// shade.glsl and metal/shaders/shade.h.
static constexpr uint32_t kPaintFlagTextureIsCoverage = 1;
static constexpr uint32_t kPaintFlagSampleNearest = 2;

/// Matches PaintData in common.glsl and metal/shaders/common.h.
struct PrPaint {
  int32_t gradient_index = -1;
  int32_t texture_index = -1;
  int32_t transform_index = 0;
  uint32_t flags = 0;
};
static_assert(sizeof(PrPaint) == 16);

/// The buffers a run of draws reads from: one per stream the shader set
/// binds, plus the index buffer the draw itself is over.
///
/// Slots, which the arena does not bind but names here so the two
/// catalogs stay together. Vulkan set 0 binding, then the Metal
/// argument index per stage:
///
///   positions    binding 0   vertex 0
///   attributes   binding 1   vertex 1
///   paints       binding 2   vertex 2, fragment 0
///   transforms   binding 4   vertex 4
///   gradients    binding 5   fragment 2
struct BufferBinds {
  GPUBuffer* positions = nullptr;
  GPUBuffer* attributes = nullptr;
  GPUBuffer* indices = nullptr;
  GPUBuffer* paints = nullptr;
  GPUBuffer* transforms = nullptr;
  GPUBuffer* gradients = nullptr;
};

enum class GradientKind : uint32_t {
  kLinear = 0,
  kRadial = 1,
  kConic = 2,
};

struct GradientData {
  int32_t ramp_row = 0;
  uint32_t kind = 0;
  uint32_t tile_mode = 0;
  uint32_t ramp_span = 0;
  Scalar data[4] = {0, 0, 0, 0};
  Scalar inverse_basis[4] = {1, 0, 0, 1};
  Scalar inverse_translation[4] = {0, 0, 0, 0};
};

static_assert(sizeof(GradientData) == 64);

/// Fill a gradient's inverse-matrix fields from its local matrix.
inline void SetGradientMatrix(GradientData& out, const Matrix& local_matrix) {
  if (local_matrix.IsIdentity()) {
    return;
  }
  // The 2D affine part, inverted directly: the local matrix carries the
  // gradient's own space onto the geometry, so evaluating at a point means
  // going the other way. (Only the affine part is honoured -- a gradient
  // matrix with perspective is not expressible here.)
  const Scalar a = local_matrix.m[0];
  const Scalar b = local_matrix.m[4];
  const Scalar c = local_matrix.m[1];
  const Scalar d = local_matrix.m[5];
  const Scalar determinant = a * d - b * c;
  if (std::abs(determinant) < 1e-9f) {
    return;  // Singular: leave the gradient untransformed.
  }
  const Scalar inverse_determinant = 1.0f / determinant;
  const Scalar ia = d * inverse_determinant;
  const Scalar ib = -b * inverse_determinant;
  const Scalar ic = -c * inverse_determinant;
  const Scalar id = a * inverse_determinant;
  out.inverse_basis[0] = ia;
  out.inverse_basis[1] = ib;
  out.inverse_basis[2] = ic;
  out.inverse_basis[3] = id;
  const Scalar tx = local_matrix.m[12];
  const Scalar ty = local_matrix.m[13];
  out.inverse_translation[0] = -(ia * tx + ib * ty);
  out.inverse_translation[1] = -(ic * tx + id * ty);
}

/// What the shader reads for `source`, straight off the DlGradient the
/// picture recorded. False when the source is not a gradient, or has no
/// stops to ramp between.
inline bool WriteGradientData(const flutter::DlColorSource& source,
                              GradientData& out) {
  out = GradientData{};
  if (const auto* linear = source.asLinearGradient()) {
    if (linear->stop_count() == 0) {
      return false;
    }
    SetGradientMatrix(out, linear->matrix());
    out.kind = static_cast<uint32_t>(GradientKind::kLinear);
    out.tile_mode = static_cast<uint32_t>(linear->tile_mode());
    out.data[0] = linear->start_point().x;
    out.data[1] = linear->start_point().y;
    out.data[2] = linear->end_point().x;
    out.data[3] = linear->end_point().y;
    return true;
  }
  if (const auto* radial = source.asRadialGradient()) {
    if (radial->stop_count() == 0) {
      return false;
    }
    SetGradientMatrix(out, radial->matrix());
    out.kind = static_cast<uint32_t>(GradientKind::kRadial);
    out.tile_mode = static_cast<uint32_t>(radial->tile_mode());
    out.data[0] = radial->center().x;
    out.data[1] = radial->center().y;
    out.data[2] = radial->radius();
    return true;
  }
  const auto* sweep = source.asSweepGradient();
  if (sweep == nullptr || sweep->stop_count() == 0) {
    return false;
  }
  SetGradientMatrix(out, sweep->matrix());
  out.kind = static_cast<uint32_t>(GradientKind::kConic);
  out.tile_mode = static_cast<uint32_t>(sweep->tile_mode());
  out.data[0] = sweep->center().x;
  out.data[1] = sweep->center().y;
  // The shader wants a bias and a scale, not angles: t is a turn count
  // offset to the start angle and stretched over the sweep. Folding it
  // here keeps the fragment down to a multiply and an add.
  constexpr Scalar kOneOverTurn = 1.0f / 360.0f;
  const Scalar t0 = sweep->start() * kOneOverTurn;
  const Scalar t1 = sweep->end() * kOneOverTurn;
  const Scalar turns = t1 - t0;
  out.data[2] = -t0;
  out.data[3] = std::abs(turns) < 1e-6f ? 0.0f : 1.0f / turns;
  return true;
}

//------------------------------------------------------------------------------
/// A bump allocator of GPU staging buffers.
class BufferArena {
 public:
  /// Default allocation capacity.
  struct Capacity {
    uint32_t vertices = 65536;
    uint32_t indices = 131072;
    uint32_t paints = 16384;
    uint32_t transforms = 4096;
    uint32_t gradients = 512;
  };

  static constexpr int kFramesInFlight = 3;

  explicit BufferArena(GPUContext& context);

  BufferArena(GPUContext& context, Capacity capacity);

  ~BufferArena();

  struct Result {
    Point* position_out = nullptr;
    Attributes* attributes_out = nullptr;
    uint16_t* index_out = nullptr;
    PrPaint* paint_out = nullptr;
    Matrix* transform_out = nullptr;
    GradientData* gradient_out = nullptr;

    uint16_t vertex_start = 0;
    uint32_t index_start = 0;
    /// The slots taken in the paint-side streams. A vertex names its
    /// paint by index and a paint names its transform by index, so both
    /// have to come back out.
    uint32_t paint_start = 0;
    uint32_t transform_start = 0;
    uint32_t gradient_start = 0;

    bool new_buffer = false;

    bool IsValid() const { return position_out != nullptr; }
  };

  Result ReserveAllocation(uint32_t vertex_count, uint32_t index_count);

  BufferBinds GetBinds() const;

  /// End the frame: move to the next slot of the ring, and drop the sets
  /// this frame did not need.
  void Reset();

  BufferArena(const BufferArena&) = delete;
  BufferArena& operator=(const BufferArena&) = delete;

 private:
  enum BufferType {
    kPosition = 0,
    kAttributes = 1,
    kIndices = 2,
    kPaints = 3,
    kTransforms = 4,
    kGradients = 5,
    kLength = 6
  };

  struct BufferAndOffset {
    std::unique_ptr<GPUBuffer> buffer;
    uint32_t offset = 0;
  };

  using BufferSet = std::array<BufferAndOffset, BufferType::kLength>;

  /// How many bytes of `type` one set holds.
  uint32_t CapacityFor(BufferType type) const;

  /// Append a set of buffers to `slot`.
  ///
  /// Returns whether the allocation succeeded.
  bool AllocateSet(std::vector<BufferSet>& slot);

  static void ClearOffsets(BufferSet& set);

  /// Open a new set of buffers, potentially allocating if needed.
  bool OpenSet();

  GPUContext& context_;
  const Capacity capacity_;
  std::array<uint32_t, BufferType::kLength> buffer_sizes_;

  std::vector<BufferSet> sets_[kFramesInFlight];
  int current_frame_ = 0;
  int current_offset_ = 0;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_BUFFER_ARENA_H_
