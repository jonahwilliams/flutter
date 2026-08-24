// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/painting/picture.h"

#include "impeller/propeller/snapshot_hook.h"

#include <memory>
#include <utility>

#include "flutter/fml/make_copyable.h"
#include "flutter/lib/ui/painting/canvas.h"
#include "flutter/lib/ui/painting/display_list_deferred_image_gpu_skia.h"
#include "flutter/lib/ui/ui_dart_state.h"
#if IMPELLER_SUPPORTS_RENDERING
#include "flutter/impeller/display_list/dl_image_impeller.h"  // nogncheck
#include "flutter/lib/ui/painting/display_list_deferred_image_gpu_impeller.h"  // nogncheck
#endif  // IMPELLER_SUPPORTS_RENDERING
#include "flutter/lib/ui/painting/display_list_image_gpu.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_args.h"
#include "third_party/tonic/dart_binding_macros.h"
#include "third_party/tonic/dart_library_natives.h"
#include "third_party/tonic/dart_persistent_value.h"
#include "third_party/tonic/logging/dart_invoke.h"

namespace flutter {

IMPLEMENT_WRAPPERTYPEINFO(ui, Picture);

void Picture::CreateAndAssociateWithDartWrapper(
    Dart_Handle dart_handle,
    std::shared_ptr<impeller::PrPicture> picture) {
  auto canvas_picture = fml::MakeRefCounted<Picture>(std::move(picture));
  canvas_picture->AssociateWithDartWrapper(dart_handle);
}

// TODO: PrPicture has no id or retained flag yet, so nothing downstream
// can recognise the same picture across frames the way MLR's pass
// recycling did.
Picture::Picture(std::shared_ptr<impeller::PrPicture> picture)
    : picture_(std::move(picture)) {}

Picture::~Picture() = default;

Dart_Handle Picture::toImage(uint32_t width,
                             uint32_t height,
                             Dart_Handle raw_image_callback) {
  // Propeller pictures have no display list to rasterize through this
  // path yet.
  return tonic::ToDart("Picture.toImage is not supported with Propeller");
}

namespace {

/// Picture.toImageSync hands its image back immediately; the texture
/// lands once the raster thread has rendered the picture, which is
/// before any frame recorded after this call gets flattened. Draws that
/// resolve it earlier see no texture and skip.
class DlDeferredPropellerImage final : public DlImage {
 public:
  explicit DlDeferredPropellerImage(DlISize size) : size_(size) {}

  void Fulfill(sk_sp<DlImage> image) {
    std::scoped_lock lock(mutex_);
    image_ = std::move(image);
  }

  // |DlImage|
  Type GetImageType() const override { return Type::kImpeller; }

  // |DlImage|
  const impeller::DlImageImpeller* asImpellerImage() const override {
    std::scoped_lock lock(mutex_);
    return image_ ? image_->asImpellerImage() : nullptr;
  }

  // |DlImage|
  bool isTextureBacked() const override { return true; }

  // |DlImage|
  DlColorSpace GetColorSpace() const override { return DlColorSpace::kSRGB; }

  // |DlImage|
  bool isOpaque() const override { return false; }

  // |DlImage|
  bool isUIThreadSafe() const override { return true; }

  // |DlImage|
  DlISize GetSize() const override { return size_; }

  // |DlImage|
  size_t GetApproximateByteSize() const override {
    return sizeof(*this) + static_cast<size_t>(size_.width) * size_.height * 4;
  }

  // |DlImage|
  OwningContext owning_context() const override {
    return OwningContext::kRaster;
  }

 private:
  mutable std::mutex mutex_;
  sk_sp<DlImage> image_;
  const DlISize size_;
};

}  // namespace

void Picture::toImageSync(uint32_t width,
                          uint32_t height,
                          int32_t target_format,
                          Dart_Handle raw_image_handle) {
  // target_format is ignored: propeller snapshots are RGBA8, the
  // kDontCare answer.
  auto* dart_state = UIDartState::Current();
  if (!dart_state || !picture_ || width == 0 || height == 0) {
    return;
  }
  auto deferred = sk_make_sp<DlDeferredPropellerImage>(
      DlISize(static_cast<int32_t>(width), static_cast<int32_t>(height)));
  auto image = CanvasImage::Create();
  image->set_image(deferred);
  image->AssociateWithDartWrapper(raw_image_handle);
  dart_state->GetTaskRunners().GetRasterTaskRunner()->PostTask(
      [picture = picture_, deferred, width, height] {
        if (sk_sp<DlImage> rendered =
                impeller::PropellerSnapshotPicture(picture, width, height)) {
          deferred->Fulfill(std::move(rendered));
        }
      });
}

static sk_sp<DlImage> CreateDeferredImage(
    bool impeller,
    sk_sp<DisplayList> display_list,
    uint32_t width,
    uint32_t height,
    SnapshotPixelFormat target_format,
    fml::TaskRunnerAffineWeakPtr<SnapshotDelegate> snapshot_delegate,
    fml::RefPtr<fml::TaskRunner> raster_task_runner,
    const fml::RefPtr<SkiaUnrefQueue>& unref_queue) {
#if IMPELLER_SUPPORTS_RENDERING
  if (impeller) {
    return DlDeferredImageGPUImpeller::Make(
        std::move(display_list), DlISize(width, height), target_format,
        std::move(snapshot_delegate), std::move(raster_task_runner));
  }
#endif  // IMPELLER_SUPPORTS_RENDERING

#if SLIMPELLER
  FML_LOG(FATAL) << "Impeller opt-out unavailable.";
  return nullptr;
#else   // SLIMPELLER
  const SkImageInfo image_info = SkImageInfo::Make(
      width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  return DlDeferredImageGPUSkia::Make(image_info, std::move(display_list),
                                      std::move(snapshot_delegate),
                                      raster_task_runner, unref_queue);
#endif  //  !SLIMPELLER
}

// static
void Picture::RasterizeToImageSync(sk_sp<DisplayList> display_list,
                                   uint32_t width,
                                   uint32_t height,
                                   SnapshotPixelFormat target_format,
                                   Dart_Handle raw_image_handle) {
  auto* dart_state = UIDartState::Current();
  if (!dart_state) {
    return;
  }
  auto unref_queue = dart_state->GetSkiaUnrefQueue();
  auto snapshot_delegate = dart_state->GetSnapshotDelegate();
  auto raster_task_runner = dart_state->GetTaskRunners().GetRasterTaskRunner();

  auto image = CanvasImage::Create();
  auto dl_image = CreateDeferredImage(
      dart_state->IsImpellerEnabled(), std::move(display_list), width, height,
      target_format, std::move(snapshot_delegate),
      std::move(raster_task_runner), unref_queue);
  image->set_image(dl_image);
  image->AssociateWithDartWrapper(raw_image_handle);
}

void Picture::dispose() {
  picture_.reset();
  ClearDartWrapper();
}

size_t Picture::GetAllocationSize() const {
  if (picture_) {
    // An estimate from the dominant spans; the GC only wants pressure.
    return sizeof(Picture) + sizeof(impeller::PrPicture) +
           picture_->GetDraws().size() * sizeof(impeller::Draw) +
           picture_->GetPositions().size() * sizeof(impeller::Point);
  }
  return sizeof(Picture);
}

#if IMPELLER_SUPPORTS_RENDERING
static sk_sp<DlImage> MakeImpellerImage(
    const std::shared_ptr<impeller::Texture>& texture) {
  if (texture) {
    return impeller::DlImageImpeller::Make(texture,
                                           DlImage::OwningContext::kRaster);
  }
  return nullptr;
}
#endif  // IMPELLER_SUPPORTS_RENDERING

Dart_Handle Picture::RasterizeToImage(const sk_sp<DisplayList>& display_list,
                                      uint32_t width,
                                      uint32_t height,
                                      Dart_Handle raw_image_callback) {
  return DoRasterizeToImage(display_list, nullptr, width, height,
                            raw_image_callback);
}

Dart_Handle Picture::RasterizeLayerTreeToImage(
    std::unique_ptr<LayerTree> layer_tree,
    Dart_Handle raw_image_callback) {
  FML_DCHECK(layer_tree != nullptr);
  auto frame_size = layer_tree->frame_size();
  return DoRasterizeToImage(nullptr, std::move(layer_tree), frame_size.width,
                            frame_size.height, raw_image_callback);
}

Dart_Handle Picture::DoRasterizeToImage(const sk_sp<DisplayList>& display_list,
                                        std::unique_ptr<LayerTree> layer_tree,
                                        uint32_t width,
                                        uint32_t height,
                                        Dart_Handle raw_image_callback) {
  // Either display_list or layer_tree should be provided.
  FML_DCHECK((display_list == nullptr) != (layer_tree == nullptr));

  if (Dart_IsNull(raw_image_callback) || !Dart_IsClosure(raw_image_callback)) {
    return tonic::ToDart("Image callback was invalid");
  }

  if (width == 0 || height == 0) {
    return tonic::ToDart("Image dimensions for scene were invalid.");
  }

  auto* dart_state = UIDartState::Current();
  auto image_callback = std::make_unique<tonic::DartPersistentValue>(
      dart_state, raw_image_callback);
  auto unref_queue = dart_state->GetSkiaUnrefQueue();
  auto ui_task_runner = dart_state->GetTaskRunners().GetUITaskRunner();
  auto raster_task_runner = dart_state->GetTaskRunners().GetRasterTaskRunner();
  auto snapshot_delegate = dart_state->GetSnapshotDelegate();
#if IMPELLER_SUPPORTS_RENDERING
  auto is_impeller_enabled = dart_state->IsImpellerEnabled();
#else
  auto is_impeller_enabled = false;
#endif  // IMPELLER_SUPPORTS_RENDERING

  // We can't create an image on this task runner because we don't have a
  // graphics context. Even if we did, it would be slow anyway. Also, this
  // thread owns the sole reference to the layer tree. So we do it in the
  // raster thread.

  auto ui_task =
      // The static leak checker gets confused by the use of fml::MakeCopyable.
      // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
      fml::MakeCopyable([image_callback = std::move(image_callback),
                         unref_queue](sk_sp<DlImage> image) mutable {
        auto dart_state = image_callback->dart_state().lock();
        if (!dart_state) {
          // The root isolate could have died in the meantime.
          return;
        }
        tonic::DartState::Scope scope(dart_state);

        if (!image) {
          tonic::DartInvoke(image_callback->Get(), {Dart_Null()});
          return;
        }

        if (!image->isUIThreadSafe()) {
          // All images with impeller textures should already be safe.
          FML_DCHECK(image->GetImageType() == DlImage::Type::kSkia);
          auto skia_image = image->asSkiaImage();
          image =
              DlImageGPU::Make({skia_image ? skia_image->skia_image() : nullptr,
                                std::move(unref_queue)});
        }

        auto dart_image = CanvasImage::Create();
        dart_image->set_image(image);
        auto* raw_dart_image = tonic::ToDart(dart_image);

        // All done!
        tonic::DartInvoke(image_callback->Get(), {raw_dart_image});

        // image_callback is associated with the Dart isolate and must be
        // deleted on the UI thread.
        image_callback.reset();
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
      });

  // Kick things off on the raster rask runner.
  fml::TaskRunner::RunNowOrPostTask(
      raster_task_runner,
      fml::MakeCopyable([ui_task_runner, snapshot_delegate, display_list, width,
                         height, ui_task, is_impeller_enabled,
                         layer_tree = std::move(layer_tree)]() mutable {
        auto picture_bounds = DlISize(width, height);
        sk_sp<DisplayList> snapshot_display_list = display_list;
        if (layer_tree) {
          FML_DCHECK(picture_bounds == layer_tree->frame_size());
          snapshot_display_list =
              layer_tree->Flatten(DlRect::MakeWH(width, height),
                                  snapshot_delegate->GetTextureRegistry(),
                                  snapshot_delegate->GetGrContext());
        }
        if (is_impeller_enabled) {
#if IMPELLER_SUPPORTS_RENDERING
          snapshot_delegate->MakeImpellerSnapshot(
              snapshot_display_list, picture_bounds,
              [ui_task_runner,
               ui_task](const std::shared_ptr<impeller::Texture>& texture) {
                fml::TaskRunner::RunNowOrPostTask(
                    ui_task_runner, [ui_task, texture]() {
                      ui_task(MakeImpellerImage(texture));
                    });
              },
              SnapshotPixelFormat::kDontCare);
#endif  // IMPELLER_SUPPORTS_RENDERING
        } else {
          snapshot_delegate->MakeSkiaSnapshot(
              snapshot_display_list, picture_bounds,
              [ui_task_runner, ui_task](const sk_sp<SkImage>& sk_image) {
                fml::TaskRunner::RunNowOrPostTask(
                    ui_task_runner, [ui_task, sk_image]() {
                      sk_sp<DlImage> image;
                      if (sk_image) {
                        image = DlImageSkia::Make(sk_image);
                      }
                      ui_task(std::move(image));
                    });
              },
              SnapshotPixelFormat::kDontCare);
        }
      }));

  return Dart_Null();
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
}

}  // namespace flutter
