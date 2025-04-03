// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_PAINTING_CODEC_MANAGER_H_
#define FLUTTER_LIB_UI_PAINTING_CODEC_MANAGER_H_

#include <memory>
#include <mutex>
#include <unordered_map>

#include "flutter/common/task_runners.h"
#include "flutter/lib/ui/painting/image.h"
#include "flutter/lib/ui/painting/multi_frame_codec.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/tonic/dart_persistent_value.h"

namespace flutter {

class CodecManager {
 public:
  explicit CodecManager(
      const TaskRunners& task_runners,
      std::shared_ptr<impeller::Context> impeller_context,
      std::shared_ptr<const fml::SyncSwitch> gpu_is_disabled_sync_switch,
      fml::RefPtr<flutter::SkiaUnrefQueue> skia_unref_queue,
      fml::WeakPtr<GrDirectContext> resource_context);

  ~CodecManager();

  void Shutdown();

  void EnqueueDecode(std::unique_ptr<tonic::DartPersistentValue> callback,
                     const std::shared_ptr<MultiFrameCodec::State>& codec,
                     size_t trace_id);

  void EnqueueComplete(std::unique_ptr<tonic::DartPersistentValue> callback,
                       fml::RefPtr<CanvasImage> image,
                       std::string decode_error,
                       int duration,
                       size_t trace_id);

 private:
  const TaskRunners task_runners_;
  std::shared_ptr<impeller::Context> impeller_context_;
  std::shared_ptr<const fml::SyncSwitch> gpu_is_disabled_sync_switch_;
  fml::RefPtr<flutter::SkiaUnrefQueue> skia_unref_queue_;
  fml::WeakPtr<GrDirectContext> resource_context_;
  std::mutex pending_images_mutex_;

  struct PendingImage {
    std::unique_ptr<tonic::DartPersistentValue> callback;
    std::weak_ptr<MultiFrameCodec::State> codec;
    size_t trace_id;

    PendingImage(std::unique_ptr<tonic::DartPersistentValue> callback,
                 std::weak_ptr<MultiFrameCodec::State> codec,
                 size_t trace_id)
        : callback(std::move(callback)),
          codec(std::move(codec)),
          trace_id(trace_id) {}
  };

  struct PendingCompleteImage {
    std::unique_ptr<tonic::DartPersistentValue> callback;
    fml::RefPtr<CanvasImage> image;
    std::string decode_error;
    int duration;
    size_t trace_id;

    PendingCompleteImage(std::unique_ptr<tonic::DartPersistentValue> callback,
                         fml::RefPtr<CanvasImage> image,
                         std::string decode_error,
                         int duration,
                         size_t trace_id)
        : callback(std::move(callback)),
          image(std::move(image)),
          decode_error(std::move(decode_error)),
          duration(duration),
          trace_id(trace_id) {}
  };

  bool did_shutdown_ = false;
  std::unordered_map<size_t, std::shared_ptr<PendingImage>> pending_;
  std::unordered_map<size_t, std::shared_ptr<PendingCompleteImage>> pending_ui_;

  CodecManager(const CodecManager&) = delete;
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_PAINTING_CODEC_MANAGER_H_
