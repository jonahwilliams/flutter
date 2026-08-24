// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/testing/testing.h"
#include "impeller/geometry/geometry_asserts.h"
#include "impeller/propeller/buffer_arena.h"
#include "impeller/propeller/testing/stub_gpu_context.h"

namespace impeller {
namespace testing {

namespace {

/// StubGpuContext with a tally, so a test can tell a buffer that was
/// reused from one that was allocated again.
class CountingGpuContext final : public GPUContext {
 public:
  std::unique_ptr<GPUTexture> CreateTexture(const TextureDesc& desc,
                                            bool zeroed) override {
    return std::make_unique<StubGpuTexture>(desc);
  }

  std::unique_ptr<GPUBuffer> CreateBuffer(const uint8_t* bytes,
                                          size_t size) override {
    if (fail) {
      return nullptr;
    }
    buffers_created++;
    return std::make_unique<StubGpuBuffer>(size);
  }

  int buffers_created = 0;
  bool fail = false;
};

/// Buffers one set of the arena allocates: one per stream.
constexpr int kStreamsPerSet = 6;

/// Small enough that a test can fill a stream without writing megabytes.
BufferArena::Capacity SmallCapacity() {
  return BufferArena::Capacity{
      .vertices = 8,
      .indices = 12,
      .paints = 2,
      .transforms = 2,
      .gradients = 1,
  };
}

}  // namespace

TEST(BufferArenaTest, StreamsMatchTheShaderLayout) {
  // The sizes the msl and glsl declare. A mismatch here is a mismatch
  // with every shader that reads the stream.
  EXPECT_EQ(sizeof(Point), 8u);        // vec2 / packed_float2 positions
  EXPECT_EQ(sizeof(Attributes), 16u);  // VertexAttributes
  EXPECT_EQ(sizeof(PrPaint), 16u);     // PaintData
  EXPECT_EQ(sizeof(Matrix), 64u);      // TransformData
  EXPECT_EQ(sizeof(GradientData), 64u);
}

TEST(BufferArenaTest, EveryFrameHasASetBeforeAnythingIsReserved) {
  CountingGpuContext context;
  BufferArena arena(context, SmallCapacity());

  BufferBinds binds = arena.GetBinds();

  // A set is always open, so a reservation always has one to fit into.
  EXPECT_NE(binds.positions, nullptr);
  EXPECT_NE(binds.attributes, nullptr);
  EXPECT_NE(binds.indices, nullptr);
  EXPECT_NE(binds.paints, nullptr);
  EXPECT_NE(binds.transforms, nullptr);
  EXPECT_NE(binds.gradients, nullptr);
  EXPECT_EQ(context.buffers_created,
            BufferArena::kFramesInFlight * kStreamsPerSet);
}

TEST(BufferArenaTest, TheFirstReservationUsesTheSetTheSlotCameWith) {
  CountingGpuContext context;
  BufferArena arena(context, SmallCapacity());
  BufferBinds before = arena.GetBinds();

  BufferArena::Result result = arena.ReserveAllocation(4, 6);

  ASSERT_TRUE(result.IsValid());
  // Nothing rolled, so nothing was allocated and the binds still stand.
  EXPECT_FALSE(result.new_buffer);
  EXPECT_EQ(result.vertex_start, 0u);
  EXPECT_EQ(result.index_start, 0u);
  EXPECT_EQ(arena.GetBinds().positions, before.positions);
  EXPECT_EQ(context.buffers_created,
            BufferArena::kFramesInFlight * kStreamsPerSet);
}

TEST(BufferArenaTest, EveryStreamGetsItsOwnBuffer) {
  StubGpuContext context;
  BufferArena arena(context);

  arena.ReserveAllocation(4, 6);
  BufferBinds binds = arena.GetBinds();

  // The shaders bind each stream separately, so nothing is shared.
  std::vector<GPUBuffer*> buffers = {binds.positions,  binds.attributes,
                                     binds.indices,    binds.paints,
                                     binds.transforms, binds.gradients};
  for (size_t i = 0; i < buffers.size(); i++) {
    for (size_t j = i + 1; j < buffers.size(); j++) {
      EXPECT_NE(buffers[i], buffers[j]) << "streams " << i << " and " << j;
    }
  }
}

TEST(BufferArenaTest, ReservationsBumpWithinASet) {
  StubGpuContext context;
  BufferArena arena(context);

  BufferArena::Result first = arena.ReserveAllocation(4, 6);
  BufferArena::Result second = arena.ReserveAllocation(3, 3);

  // No roll, so the second draw can batch with the first.
  EXPECT_FALSE(second.new_buffer);
  EXPECT_EQ(second.vertex_start, 4u);
  EXPECT_EQ(second.index_start, 6u);
  // And its pointers pick up exactly where the first reservation ended.
  EXPECT_EQ(second.position_out, first.position_out + 4);
  EXPECT_EQ(second.attributes_out, first.attributes_out + 4);
  EXPECT_EQ(second.index_out, first.index_out + 6);
  EXPECT_EQ(second.paint_out, first.paint_out + 1);
}

TEST(BufferArenaTest, GeometryLandsInItsOwnStream) {
  StubGpuContext context;
  BufferArena arena(context);

  BufferArena::Result result = arena.ReserveAllocation(2, 3);
  ASSERT_TRUE(result.IsValid());
  result.position_out[0] = Point(1, 2);
  result.position_out[1] = Point(3, 4);
  result.attributes_out[0] =
      Attributes{.uv = Point(5, 6), .color = 0xDEADBEEF, .paint = 7};
  result.index_out[0] = 0;
  result.index_out[1] = 1;
  result.index_out[2] = 0;

  // Read back through the buffers the draw would bind.
  BufferBinds binds = arena.GetBinds();
  auto* positions = reinterpret_cast<const Point*>(binds.positions->Contents());
  auto* attributes =
      reinterpret_cast<const Attributes*>(binds.attributes->Contents());
  auto* indices = reinterpret_cast<const uint16_t*>(binds.indices->Contents());

  EXPECT_POINT_NEAR(positions[0], Point(1, 2));
  EXPECT_POINT_NEAR(positions[1], Point(3, 4));
  EXPECT_POINT_NEAR(attributes[0].uv, Point(5, 6));
  EXPECT_EQ(attributes[0].color, 0xDEADBEEFu);
  EXPECT_EQ(attributes[0].paint, 7u);
  EXPECT_EQ(indices[1], 1u);
}

TEST(BufferArenaTest, EachReservationTakesOneOfEveryPaintStream) {
  StubGpuContext context;
  BufferArena arena(context);

  BufferArena::Result first = arena.ReserveAllocation(4, 6);
  BufferArena::Result second = arena.ReserveAllocation(4, 6);

  ASSERT_TRUE(first.IsValid());
  // Each stream advances by one element of its own type, so no two draws
  // are handed the same slot.
  EXPECT_FALSE(second.new_buffer);
  EXPECT_EQ(second.paint_out, first.paint_out + 1);
  EXPECT_EQ(second.transform_out, first.transform_out + 1);
  EXPECT_EQ(second.gradient_out, first.gradient_out + 1);
}

TEST(BufferArenaTest, ADrawThatDoesNotFitRollsToANewSet) {
  StubGpuContext context;
  BufferArena arena(context, SmallCapacity());

  BufferArena::Result first = arena.ReserveAllocation(6, 6);
  GPUBuffer* first_positions = arena.GetBinds().positions;
  // Only two vertices are left in the set.
  BufferArena::Result second = arena.ReserveAllocation(4, 4);

  EXPECT_TRUE(second.new_buffer);
  // The new set starts empty, so the draw is at its origin.
  EXPECT_EQ(second.vertex_start, 0u);
  EXPECT_EQ(second.index_start, 0u);
  EXPECT_NE(arena.GetBinds().positions, first_positions);
  EXPECT_NE(second.position_out, first.position_out);
}

TEST(BufferArenaTest, AnyFullStreamRollsTheWholeSet) {
  // Whichever stream fills first, everything a draw needs has to stay in
  // one set, so they all move together.
  {
    StubGpuContext context;
    BufferArena arena(context, SmallCapacity());
    arena.ReserveAllocation(1, 10);
    EXPECT_TRUE(arena.ReserveAllocation(1, 10).new_buffer) << "indices";
  }
  {
    StubGpuContext context;
    BufferArena arena(context, SmallCapacity());
    arena.ReserveAllocation(1, 1);
    arena.ReserveAllocation(1, 1);
    // Two paints per set, and each reservation takes one.
    EXPECT_TRUE(arena.ReserveAllocation(1, 1).new_buffer) << "paints";
  }
  {
    StubGpuContext context;
    BufferArena arena(context, SmallCapacity());
    arena.ReserveAllocation(1, 1);
    // One gradient per set, and every reservation takes one.
    EXPECT_TRUE(arena.ReserveAllocation(1, 1).new_buffer) << "gradients";
  }
}

/// End the frame enough times to come back to the slot we are on.
void AroundTheRing(BufferArena& arena) {
  for (int i = 0; i < BufferArena::kFramesInFlight; i++) {
    arena.Reset();
  }
}

TEST(BufferArenaTest, EachFrameOfTheRingWritesItsOwnBuffers) {
  CountingGpuContext context;
  BufferArena arena(context, SmallCapacity());

  arena.ReserveAllocation(1, 1);
  GPUBuffer* first = arena.GetBinds().positions;
  arena.Reset();
  arena.ReserveAllocation(1, 1);
  GPUBuffer* second = arena.GetBinds().positions;
  arena.Reset();
  arena.ReserveAllocation(1, 1);
  GPUBuffer* third = arena.GetBinds().positions;

  // A frame still in flight is reading what it wrote, so the next frame
  // cannot be handed the same buffer.
  EXPECT_NE(first, second);
  EXPECT_NE(second, third);
  EXPECT_NE(first, third);
  EXPECT_EQ(context.buffers_created, 3 * kStreamsPerSet);
}

TEST(BufferArenaTest, TheRingComesBackAroundAndReuses) {
  CountingGpuContext context;
  BufferArena arena(context, SmallCapacity());

  BufferArena::Result first = arena.ReserveAllocation(6, 6);
  GPUBuffer* first_positions = arena.GetBinds().positions;
  ASSERT_EQ(first.vertex_start, 0u);

  BufferArena::Result again;
  for (int i = 0; i < BufferArena::kFramesInFlight; i++) {
    arena.Reset();
    again = arena.ReserveAllocation(1, 1);
  }

  // A full ring later the frame that wrote it has finished, so the same
  // buffer comes back rather than a new one being allocated...
  EXPECT_EQ(arena.GetBinds().positions, first_positions);
  EXPECT_EQ(context.buffers_created,
            BufferArena::kFramesInFlight * kStreamsPerSet);
  // ...with the offsets it was left at a ring ago cleared.
  EXPECT_FALSE(again.new_buffer);
  EXPECT_EQ(again.vertex_start, 0u);
  EXPECT_EQ(again.index_start, 0u);
  EXPECT_EQ(again.paint_out, first.paint_out);
}

TEST(BufferArenaTest, LeftoverSetsAreDroppedAtTheEndOfTheFrame) {
  CountingGpuContext context;
  BufferArena arena(context, SmallCapacity());

  // A frame that spikes to three sets: the capacity is eight vertices.
  arena.ReserveAllocation(8, 8);
  arena.ReserveAllocation(8, 8);
  arena.ReserveAllocation(8, 8);
  ASSERT_EQ(context.buffers_created, 5 * kStreamsPerSet);

  // Back to that slot, needing only one of the three.
  AroundTheRing(arena);
  arena.ReserveAllocation(1, 1);
  EXPECT_EQ(context.buffers_created, 5 * kStreamsPerSet) << "reused, not new";
  // Ending the frame lets the other two go.
  arena.Reset();

  // Spiking again has to allocate the two that were dropped, which is
  // what says they were let go rather than held for the next spike.
  for (int i = 0; i < BufferArena::kFramesInFlight - 1; i++) {
    arena.Reset();
  }
  arena.ReserveAllocation(8, 8);
  arena.ReserveAllocation(8, 8);
  arena.ReserveAllocation(8, 8);
  EXPECT_EQ(context.buffers_created, 7 * kStreamsPerSet);
}

TEST(BufferArenaTest, ASlotIsNeverTrimmedBelowItsFirstSet) {
  CountingGpuContext context;
  BufferArena arena(context, SmallCapacity());

  arena.ReserveAllocation(1, 1);
  GPUBuffer* positions = arena.GetBinds().positions;

  // Back to that slot, recording nothing into it at all.
  AroundTheRing(arena);
  arena.Reset();
  for (int i = 0; i < BufferArena::kFramesInFlight - 1; i++) {
    arena.Reset();
  }
  arena.ReserveAllocation(1, 1);

  // The set the slot was built with stays, so a reservation always has
  // one to fit into.
  EXPECT_EQ(arena.GetBinds().positions, positions);
  EXPECT_EQ(context.buffers_created,
            BufferArena::kFramesInFlight * kStreamsPerSet);
}

TEST(BufferArenaTest, AFailedAllocationIsAnInvalidReservation) {
  CountingGpuContext context;
  BufferArena arena(context, SmallCapacity());
  context.fail = true;

  // Fills the set the slot came with, then needs one that cannot be had.
  arena.ReserveAllocation(8, 8);
  GPUBuffer* positions = arena.GetBinds().positions;
  BufferArena::Result result = arena.ReserveAllocation(8, 8);

  EXPECT_FALSE(result.IsValid());
  // Nothing half built was published: the set that was open still is.
  EXPECT_EQ(arena.GetBinds().positions, positions);
}

TEST(BufferArenaTest, StartsAreInElementsNotBytes) {
  StubGpuContext context;
  BufferArena arena(context);

  arena.ReserveAllocation(4, 6);
  BufferArena::Result second = arena.ReserveAllocation(4, 6);

  // A vertex is eight bytes and an index two, so a byte offset that
  // leaked out here would be off by those factors.
  EXPECT_EQ(second.vertex_start, 4u);
  EXPECT_EQ(second.index_start, 6u);
  // And the pointers agree with what the starts claim.
  BufferBinds binds = arena.GetBinds();
  EXPECT_EQ(second.position_out,
            reinterpret_cast<Point*>(binds.positions->Contents()) + 4);
  EXPECT_EQ(second.index_out,
            reinterpret_cast<uint16_t*>(binds.indices->Contents()) + 6);
}

TEST(BufferArenaTest, VertexStartStaysWithinWhatAnIndexCanName) {
  StubGpuContext context;
  BufferArena arena(context);

  // Fill the default set to its last vertex, one draw short of the top.
  BufferArena::Result first = arena.ReserveAllocation(65535, 0);
  BufferArena::Result second = arena.ReserveAllocation(1, 0);

  ASSERT_TRUE(second.IsValid());
  EXPECT_FALSE(second.new_buffer);
  EXPECT_EQ(second.vertex_start, 65535u);
  EXPECT_EQ(second.position_out, first.position_out + 65535);

  // One more vertex than the set holds, so the next draw starts over.
  EXPECT_TRUE(arena.ReserveAllocation(1, 0).new_buffer);
}

}  // namespace testing
}  // namespace impeller
