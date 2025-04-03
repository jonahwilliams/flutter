
// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/painting/codec_manager.h"

#include <unordered_map>
#include <utility>

#include "flutter/fml/make_copyable.h"
#include "flutter/lib/ui/painting/image.h"
#include "flutter/lib/ui/painting/multi_frame_codec.h"
#include "fml/logging.h"

namespace flutter {

static void InvokeNextFrameCallback(
    const fml::RefPtr<CanvasImage>& image,
    int duration,
    const std::string& decode_error,
    std::unique_ptr<tonic::DartPersistentValue> callback,
    size_t trace_id) {
  std::shared_ptr<tonic::DartState> dart_state = callback->dart_state().lock();
  if (!dart_state) {
    FML_DLOG(ERROR) << "Could not acquire Dart state while attempting to fire "
                       "next frame callback.";
    return;
  }
  tonic::DartState::Scope scope(dart_state);
  tonic::DartInvoke(callback->value(),
                    {tonic::ToDart(image), tonic::ToDart(duration),
                     tonic::ToDart(decode_error)});
}

CodecManager::CodecManager(
    const TaskRunners& task_runners,
    std::shared_ptr<impeller::Context> impeller_context,
    std::shared_ptr<const fml::SyncSwitch> gpu_is_disabled_sync_switch,
    fml::RefPtr<flutter::SkiaUnrefQueue> skia_unref_queue,
    fml::WeakPtr<GrDirectContext> resource_context)
    : task_runners_(task_runners),
      impeller_context_(std::move(impeller_context)),
      gpu_is_disabled_sync_switch_(std::move(gpu_is_disabled_sync_switch)),
      skia_unref_queue_(std::move(skia_unref_queue)),
      resource_context_(std::move(resource_context)) {}

CodecManager::~CodecManager() = default;

void CodecManager::Shutdown() {
  FML_LOG(ERROR) << "CodecManager::Shutdown";
  // Dart persistent values must be deleted on the platform thread. If the
  // Platform and UI threads are merged, then queing a task to delete them will
  // defer the deletion until shell shutdown is complete, which can cause
  // crashes. (See https://github.com/flutter/flutter/issues/166410). Instead,
  // if the threads are merged we can immediately delete.
  std::unordered_map<size_t, std::shared_ptr<PendingImage>> pending_copy;
  std::unordered_map<size_t, std::shared_ptr<PendingCompleteImage>>
      pending_ui_copy;
  {
    std::scoped_lock lock(pending_images_mutex_);
    did_shutdown_ = true;
    std::swap(pending_, pending_copy);
    std::swap(pending_ui_, pending_ui_copy);
  }
  if (!task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread()) {
    task_runners_.GetUITaskRunner()->PostTask(
        [pending_copy = std::move(pending_copy),
         pending_ui_copy = std::move(pending_ui_copy)] {
          for (auto& pair : pending_copy) {
            pair.second->callback->Clear();
          }
          for (auto& pair : pending_ui_copy) {
            pair.second->callback->Clear();
          }
        });
  } else {
    for (auto& pair : pending_copy) {
      pair.second->callback->Clear();
    }
    for (auto& pair : pending_ui_copy) {
      pair.second->callback->Clear();
    }
  }
}

void CodecManager::EnqueueDecode(
    std::unique_ptr<tonic::DartPersistentValue> callback,
    const std::shared_ptr<MultiFrameCodec::State>& codec,
    size_t trace_id) {
  {
    std::scoped_lock lock(pending_images_mutex_);
    if (did_shutdown_) {
      return;
    }
    pending_[trace_id] =
        std::make_shared<PendingImage>(std::move(callback), codec, trace_id);
  }

  task_runners_.GetIOTaskRunner()->PostTask([&, trace_id]() {
    // Manager no longer exists due to shell shutdown.
    CodecManager* manager = this;
    if (!manager) {
      return;
    }
    // Manager still exists but pending images were cleared during shell
    // shutdown sequence.
    std::unordered_map<size_t, std::shared_ptr<PendingImage>>::iterator pending;
    {
      std::scoped_lock lock(manager->pending_images_mutex_);
      pending = manager->pending_.find(trace_id);
      if (pending == manager->pending_.end()) {
        return;
      }
    }

    std::shared_ptr<MultiFrameCodec::State> state =
        pending->second->codec.lock();
    auto data = std::move(pending->second);
    auto callback = std::move(data->callback);
    manager->pending_.erase(pending);

    // Codec was released and GC'd when pending decode.
    if (!state) {
      manager->task_runners_.GetUITaskRunner()->PostTask(fml::MakeCopyable(
          [callback = std::move(callback)]() { callback->Clear(); }));
      return;
    }
#if FML_OS_IOS_SIMULATOR
    // Noop backend.
    if (!resource_context && !impeller_context) {
      manager->task_runners_.GetUITaskRunner()->PostTask(
          fml::MakeCopyable([callback = std::move(callback)]() {
            // must be destroyed on UI thread.
          }));
      return;
    }
#endif  // FML_OS_IOS_SIMULATOR

    state->GetNextFrameAndInvokeCallback(
        *this,
        /*callback=*/std::move(callback),
        /*resource_context=*/manager->resource_context_,
        /*unref_queue=*/manager->skia_unref_queue_,
        /*gpu_disable_sync_switch=*/
        manager->gpu_is_disabled_sync_switch_,
        /*trace_id=*/trace_id,
        /*impeller_context=*/manager->impeller_context_);
  });
}

void CodecManager::EnqueueComplete(
    std::unique_ptr<tonic::DartPersistentValue> callback,
    fml::RefPtr<CanvasImage> image,
    std::string decode_error,
    int duration,
    size_t trace_id) {
  {
    std::scoped_lock lock(pending_images_mutex_);
    if (did_shutdown_) {
      // This should never happen :)
      FML_DCHECK(false);
      return;
    }
    pending_ui_[trace_id] =
        std::make_shared<PendingCompleteImage>(std::move(callback),      //
                                               std::move(image),         //
                                               std::move(decode_error),  //
                                               duration,                 //
                                               trace_id                  //
        );
  }
  task_runners_.GetUITaskRunner()->PostTask([&, trace_id] {
    // Manager no longer exists due to shell shutdown.
    CodecManager* manager = this;
    if (!manager) {
      return;
    }
    // Manager still exists but pending images were cleared during shell
    // shutdown sequence.
    std::unordered_map<size_t, std::shared_ptr<PendingCompleteImage>>::iterator
        pending;
    {
      std::scoped_lock lock(manager->pending_images_mutex_);
      pending = manager->pending_ui_.find(trace_id);
      if (pending == manager->pending_ui_.end()) {
        return;
      }
    }

    auto data = std::move(pending->second);
    manager->pending_ui_.erase(pending);

    InvokeNextFrameCallback(data->image, data->duration, data->decode_error,
                            std::move(data->callback), trace_id);
  });
}

}  // namespace flutter
