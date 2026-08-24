// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_RENDERER_FRAME_ARENA_H_
#define FLUTTER_IMPELLER_PROPELLER_RENDERER_FRAME_ARENA_H_

#include <memory>
#include <string>
#include <vector>

#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {

//------------------------------------------------------------------------------
/// A growable host-visible buffer the frame flatten appends into. Arenas
/// live in a ring of frames-in-flight slots; Reset() reuses the buffer,
/// so a settled frame allocates nothing.
class FrameArena {
 public:
  /// Shown in GPU captures; applied to each buffer the arena allocates.
  std::string label = "Propeller arena";

  /// Growth allocates fresh, so reservations already handed out stay in
  /// the retired buffer until the frame completes and Reset() runs.
  void Reset() {
    used_ = 0;
    retired_.clear();
  }

  /// Reserve `bytes` and return the byte offset of the reservation. The
  /// offset lands on the context's buffer-binding alignment.
  size_t Reserve(GPUContext& context, size_t bytes) {
    const size_t alignment = context.GetBufferAlignment();
    bytes = (bytes + alignment - 1) & ~(alignment - 1);
    if (buffer_ == nullptr || used_ + bytes > capacity_) {
      capacity_ = std::max({capacity_ * 2, bytes, static_cast<size_t>(16384)});
      if (buffer_ != nullptr) {
        retired_.push_back(std::move(buffer_));
      }
      buffer_ = context.CreateBuffer(nullptr, capacity_);
      FML_CHECK(buffer_ != nullptr) << "Arena allocation failed: " << capacity_;
      buffer_->SetLabel(label.c_str());
      used_ = 0;
    }
    const size_t offset = used_;
    used_ += bytes;
    return offset;
  }

  void* At(size_t offset) { return buffer_->Contents() + offset; }

  GPUBuffer* buffer() const { return buffer_.get(); }

 private:
  std::unique_ptr<GPUBuffer> buffer_;
  /// Buffers grown past this frame; reservations into them stay valid
  /// until Reset().
  std::vector<std::unique_ptr<GPUBuffer>> retired_;
  size_t capacity_ = 0;
  size_t used_ = 0;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_RENDERER_FRAME_ARENA_H_
