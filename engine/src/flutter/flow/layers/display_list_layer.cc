// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/display_list_layer.h"

#include <utility>

#include "flutter/display_list/dl_builder.h"
#include "flutter/flow/layers/cacheable_layer.h"
#include "flutter/flow/layers/offscreen_surface.h"
#include "flutter/flow/raster_cache.h"
#include "flutter/flow/raster_cache_util.h"

namespace flutter {

DisplayListLayer::DisplayListLayer(const DlPoint& offset,
                                   sk_sp<DisplayList> display_list,
                                   bool is_complex,
                                   bool will_change)
    : offset_(offset), display_list_(std::move(display_list)) {
  if (display_list_) {
    content_bounds_ = display_list_->GetBounds();
    bounds_ = content_bounds_.Shift(offset_.x, offset_.y);
#if !SLIMPELLER
    display_list_raster_cache_item_ = DisplayListRasterCacheItem::Make(
        display_list_, ToSkPoint(offset_), is_complex, will_change);
#endif  //  !SLIMPELLER
  }
}

DisplayListLayer::DisplayListLayer(const DlPoint& offset,
                                   std::shared_ptr<void> engine_picture,
                                   const DlRect& picture_bounds)
    : offset_(offset), engine_picture_(std::move(engine_picture)) {
  content_bounds_ = picture_bounds;
  bounds_ = content_bounds_.Shift(offset_.x, offset_.y);
}

bool DisplayListLayer::IsReplacing(DiffContext* context,
                                   const Layer* layer) const {
  // Only return true for identical display lists; This way
  // ContainerLayer::DiffChildren can detect when a display list layer
  // got inserted between other display list layers
  auto old_layer = layer->as_display_list_layer();
  return old_layer != nullptr && offset_ == old_layer->offset_ &&
         Compare(context->statistics(), this, old_layer);
}

void DisplayListLayer::Diff(DiffContext* context, const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  if (!context->IsSubtreeDirty()) {
#ifndef NDEBUG
    FML_DCHECK(old_layer);
    auto prev = old_layer->as_display_list_layer();
    DiffContext::Statistics dummy_statistics;
    // IsReplacing has already determined that the display list is same
    FML_DCHECK(prev->offset_ == offset_ &&
               Compare(dummy_statistics, this, prev));
#endif
  }
  context->PushTransform(DlMatrix::MakeTranslation(offset_));
  if (context->has_raster_cache()) {
    context->WillPaintWithIntegralTransform();
  }
  context->AddLayerBounds(content_bounds_);
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

bool DisplayListLayer::Compare(DiffContext::Statistics& statistics,
                               const DisplayListLayer* l1,
                               const DisplayListLayer* l2) {
  if (l1->engine_picture_ != nullptr || l2->engine_picture_ != nullptr) {
    // Engine pictures are immutable and shared: the same object is the
    // same content, and there is nothing cheaper or deeper to compare.
    if (l1->engine_picture_.get() == l2->engine_picture_.get()) {
      statistics.AddSameInstancePicture();
      return true;
    }
    statistics.AddNewPicture();
    return false;
  }
  const auto& dl1 = l1->display_list_;
  const auto& dl2 = l2->display_list_;
  if (dl1.get() == dl2.get()) {
    statistics.AddSameInstancePicture();
    return true;
  }
  const auto op_cnt_1 = dl1->op_count();
  const auto op_cnt_2 = dl2->op_count();
  const auto op_bytes_1 = dl1->bytes();
  const auto op_bytes_2 = dl2->bytes();
  if (op_cnt_1 != op_cnt_2 || op_bytes_1 != op_bytes_2 ||
      dl1->GetBounds() != dl2->GetBounds()) {
    statistics.AddNewPicture();
    return false;
  }

  if (op_bytes_1 > kMaxBytesToCompare) {
    statistics.AddPictureTooComplexToCompare();
    return false;
  }

  statistics.AddDeepComparePicture();

  auto res = dl1->Equals(*dl2);
  if (res) {
    statistics.AddDifferentInstanceButEqualPicture();
  } else {
    statistics.AddNewPicture();
  }
  return res;
}

void DisplayListLayer::Preroll(PrerollContext* context) {
  if (engine_picture_ != nullptr) {
    // A picture reference carries its opacity, so the caller always can.
    context->renderable_state_flags = LayerStateStack::kCallerCanApplyOpacity;
    set_paint_bounds(bounds_);
    return;
  }
  DisplayList* disp_list = display_list();

#if !SLIMPELLER
  AutoCache cache = AutoCache(display_list_raster_cache_item_.get(), context,
                              context->state_stack.matrix());
#endif  //  !SLIMPELLER
  if (disp_list->can_apply_group_opacity()) {
    context->renderable_state_flags = LayerStateStack::kCallerCanApplyOpacity;
  }
  set_paint_bounds(bounds_);
}

void DisplayListLayer::Paint(PaintContext& context) const {
  FML_DCHECK(needs_painting(context));

  auto mutator = context.state_stack.save();
  mutator.translate(offset_.x, offset_.y);

  if (engine_picture_ != nullptr) {
    context.canvas->DrawOpaquePicture(
        engine_picture_, context.state_stack.outstanding_opacity());
    return;
  }
  FML_DCHECK(display_list_);

#if !SLIMPELLER
  if (context.raster_cache) {
    // Always apply the integral transform in the presence of a raster cache
    // whether or not we successfully draw from the cache
    mutator.integralTransform();

    if (display_list_raster_cache_item_) {
      DlPaint paint;
      if (display_list_raster_cache_item_->Draw(
              context, context.state_stack.fill(paint))) {
        TRACE_EVENT_INSTANT0("flutter", "raster cache hit");
        return;
      }
    }
  }
#endif  //  !SLIMPELLER

  DlScalar opacity = context.state_stack.outstanding_opacity();
  context.canvas->DrawDisplayList(display_list_, opacity);
}

}  // namespace flutter
