// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/buffer_arena.h"

#include <iterator>
#include <limits>
#include <sstream>

#include "flutter/fml/logging.h"

namespace impeller {

namespace {

/// What each stream is an array of, in BufferType order.
struct StreamInfo {
  uint32_t stride;
  const char* label;
};

constexpr StreamInfo kStreams[] = {
    {sizeof(Point), "propeller.positions"},
    {sizeof(Attributes), "propeller.attributes"},
    {sizeof(uint16_t), "propeller.indices"},
    {sizeof(PrPaint), "propeller.paints"},
    {sizeof(Matrix), "propeller.transforms"},
    {sizeof(GradientData), "propeller.gradients"},
};

/// A reservation scales only these three streams, and all three have a
/// power of two stride.
constexpr uint32_t kPositionShift = 3;
constexpr uint32_t kAttributesShift = 4;
constexpr uint32_t kIndexShift = 1;
static_assert((1u << kPositionShift) == sizeof(Point));
static_assert((1u << kAttributesShift) == sizeof(Attributes));
static_assert((1u << kIndexShift) == sizeof(uint16_t));

}  // namespace

BufferArena::BufferArena(GPUContext& context)
    : BufferArena(context, Capacity{}) {}

BufferArena::BufferArena(GPUContext& context, Capacity capacity)
    : context_(context), capacity_(capacity) {
  static_assert(std::size(kStreams) == BufferType::kLength);
  FML_DCHECK(capacity.vertices <=
             std::numeric_limits<uint16_t>::max() + uint32_t{1});

  // Positions and attributes are indexed together, one of each per
  // vertex; the rest are a stream apiece.
  const uint32_t elements[BufferType::kLength] = {
      capacity.vertices, capacity.vertices,   capacity.indices,
      capacity.paints,   capacity.transforms, capacity.gradients,
  };
  for (int i = 0; i < BufferType::kLength; i++) {
    buffer_sizes_[i] = elements[i] * kStreams[i].stride;
  }

  // Every slot starts with a set, so there is always one being filled.
  for (std::vector<BufferSet>& slot : sets_) {
    FML_CHECK(AllocateSet(slot)) << "Buffer arena allocation failed";
  }
}

BufferArena::~BufferArena() = default;

uint32_t BufferArena::CapacityFor(BufferType type) const {
  return buffer_sizes_[type];
}

bool BufferArena::AllocateSet(std::vector<BufferSet>& slot) {
  BufferSet set;
  for (int i = 0; i < BufferType::kLength; i++) {
    set[i].buffer =
        context_.CreateBuffer(nullptr, CapacityFor(static_cast<BufferType>(i)));
    if (set[i].buffer == nullptr) {
      // TODO: log fatal message.
      return false;
    }
    set[i].buffer->SetLabel(kStreams[i].label);
  }
  slot.push_back(std::move(set));
  return true;
}

void BufferArena::ClearOffsets(BufferSet& set) {
  for (BufferAndOffset& stream : set) {
    stream.offset = 0;
  }
}

bool BufferArena::OpenSet() {
  std::vector<BufferSet>& slot = sets_[current_frame_];
  const size_t next = current_offset_ + 1;
  if (next == slot.size() && !AllocateSet(slot)) {
    return false;
  }
  current_offset_ = next;
  // Whatever the set holds was written a full ring of frames ago.
  ClearOffsets(slot[current_offset_]);
  return true;
}

BufferArena::Result BufferArena::ReserveAllocation(uint32_t vertex_count,
                                                   uint32_t index_count) {
  // One of each paint-side stream: a draw carries one paint, and that
  // paint names one transform and one gradient.
  const uint32_t wanted[BufferType::kLength] = {
      vertex_count << kPositionShift,
      vertex_count << kAttributesShift,
      index_count << kIndexShift,
      kStreams[BufferType::kPaints].stride,
      kStreams[BufferType::kTransforms].stride,
      kStreams[BufferType::kGradients].stride};

  std::vector<BufferSet>& slot = sets_[current_frame_];
  bool new_buffer = false;
  int overflowed = BufferType::kLength;
  const BufferSet& open = slot[current_offset_];
  for (int i = 0; i < BufferType::kLength; i++) {
    if (open[i].offset + wanted[i] > CapacityFor(static_cast<BufferType>(i))) {
      new_buffer = true;
      overflowed = i;
      break;
    }
  }
  if (new_buffer) {
    // One stream ran out and the whole set rolls over with it, so what
    // the other five had left is wasted until the ring comes back
    // round. Naming the stream that overflowed alongside what the rest
    // gave up is what says whether the capacities are in proportion.
    std::ostringstream leftovers;
    for (int i = 0; i < BufferType::kLength; i++) {
      const uint32_t capacity = CapacityFor(static_cast<BufferType>(i));
      leftovers << (i == 0 ? "" : ", ") << kStreams[i].label << " "
                << (capacity - open[i].offset) << "/" << capacity << " B free"
                << (i == overflowed ? " (full)" : "");
    }
    FML_LOG(IMPORTANT) << "propeller: buffer set " << current_offset_
                       << " rolled over, wanted " << wanted[overflowed]
                       << " B of " << kStreams[overflowed].label << ": "
                       << leftovers.str();
  }
  if (new_buffer && !OpenSet()) {
    return Result{};
  }

  BufferSet& set = slot[current_offset_];
  auto at = [&set](BufferType type) {
    return set[type].buffer->Contents() + set[type].offset;
  };

  Result result{
      .position_out = reinterpret_cast<Point*>(at(BufferType::kPosition)),
      .attributes_out =
          reinterpret_cast<Attributes*>(at(BufferType::kAttributes)),
      .index_out = reinterpret_cast<uint16_t*>(at(BufferType::kIndices)),
      .paint_out = reinterpret_cast<PrPaint*>(at(BufferType::kPaints)),
      .transform_out = reinterpret_cast<Matrix*>(at(BufferType::kTransforms)),
      .gradient_out =
          reinterpret_cast<GradientData*>(at(BufferType::kGradients)),
      .vertex_start = static_cast<uint16_t>(set[BufferType::kPosition].offset /
                                            sizeof(Point)),
      .index_start = static_cast<uint32_t>(set[BufferType::kIndices].offset /
                                           sizeof(uint16_t)),
      .paint_start = static_cast<uint32_t>(set[BufferType::kPaints].offset /
                                           sizeof(PrPaint)),
      .transform_start = static_cast<uint32_t>(
          set[BufferType::kTransforms].offset / sizeof(Matrix)),
      .gradient_start = static_cast<uint32_t>(
          set[BufferType::kGradients].offset / sizeof(GradientData)),
      .new_buffer = new_buffer,
  };

  for (int i = 0; i < BufferType::kLength; i++) {
    set[i].offset += wanted[i];
  }

  return result;
}

BufferBinds BufferArena::GetBinds() const {
  const BufferSet& set = sets_[current_frame_][current_offset_];
  return BufferBinds{
      .positions = set[BufferType::kPosition].buffer.get(),
      .attributes = set[BufferType::kAttributes].buffer.get(),
      .indices = set[BufferType::kIndices].buffer.get(),
      .paints = set[BufferType::kPaints].buffer.get(),
      .transforms = set[BufferType::kTransforms].buffer.get(),
      .gradients = set[BufferType::kGradients].buffer.get(),
  };
}

void BufferArena::Reset() {
  sets_[current_frame_].resize(current_offset_ + 1);
  current_frame_ = (current_frame_ + 1) % kFramesInFlight;
  current_offset_ = 0;
  ClearOffsets(sets_[current_frame_][0]);
}

}  // namespace impeller
