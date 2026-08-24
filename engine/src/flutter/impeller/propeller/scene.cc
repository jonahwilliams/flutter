// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/scene.h"

#include "flutter/display_list/dl_paint.h"

namespace impeller {

PrSceneBuilder::PrSceneBuilder(const Rect& surface_bounds)
    : surface_bounds_(surface_bounds) {
  stack_.push_back(State{.clip = surface_bounds});
  open_.emplace_back();
}

PrSceneBuilder::~PrSceneBuilder() = default;

void PrSceneBuilder::SetSurfaceBounds(const Rect& bounds) {
  surface_bounds_ = bounds;
  stack_.back().clip = bounds;
}

PrSceneNode PrSceneBuilder::Build() {
  // Whatever is still open closes into the root: an unbalanced save is
  // the layer tree's problem, not something to lose content over.
  while (open_.size() > 1) {
    CloseGroup();
  }
  PrSceneNode root = std::move(open_.back());

  // One frame deep: a shape this one did not ask for is dropped, and
  // what it did ask for is offered to the next.
  std::erase_if(clip_cache_,
                [](const ClipCacheEntry& entry) { return !entry.used; });
  for (ClipCacheEntry& entry : clip_cache_) {
    entry.used = false;
  }

  open_.clear();
  open_.emplace_back();
  stack_.clear();
  stack_.push_back(State{.clip = surface_bounds_});

  return root;
}

flutter::DlISize PrSceneBuilder::GetBaseLayerDimensions() const {
  return flutter::DlISize(static_cast<int32_t>(surface_bounds_.GetWidth()),
                          static_cast<int32_t>(surface_bounds_.GetHeight()));
}

SkImageInfo PrSceneBuilder::GetImageInfo() const {
  return SkImageInfo::MakeUnknown(0, 0);
}

void PrSceneBuilder::Save() {
  stack_.push_back(State{
      .transform = stack_.back().transform,
      .clip = stack_.back().clip,
      .group = false,
  });
}

void PrSceneBuilder::SaveLayer(const std::optional<flutter::DlRect>& bounds,
                               const flutter::DlPaint* paint,
                               const flutter::DlImageFilter* backdrop,
                               std::optional<int64_t> backdrop_id) {
  const flutter::DlPaint attributes = paint ? *paint : flutter::DlPaint();

  stack_.push_back(State{
      .transform = stack_.back().transform,
      .clip = stack_.back().clip,
      .group = true,
  });
  // A filter reads pixels the layer bounds do not show, so a filtered
  // layer keeps the clip it inherited.
  if (bounds.has_value() && backdrop == nullptr &&
      attributes.getImageFilter() == nullptr) {
    IntersectClip(bounds.value());
  }

  PrSceneNode group;
  group.transform = stack_.back().transform;
  group.clip = NodeClip();
  group.opacity = attributes.getOpacity();
  group.image_filter = attributes.getImageFilter();
  group.color_filter = attributes.getColorFilter();
  open_.push_back(std::move(group));
}

void PrSceneBuilder::Restore() {
  if (stack_.size() <= 1) {
    return;
  }
  const State state = stack_.back();
  stack_.pop_back();
  // The clips taken under this save are inside whatever it opened, so
  // they close first.
  for (uint32_t i = 0; i < state.clip_groups; i++) {
    CloseGroup();
  }
  if (state.group) {
    CloseGroup();
  }
}

void PrSceneBuilder::CloseGroup() {
  PrSceneNode done = std::move(open_.back());
  open_.pop_back();
  if (done.children.empty()) {
    // A group that composited nothing is nothing.
    return;
  }
  open_.back().children.push_back(std::move(done));
}

bool PrSceneBuilder::ClipKey::operator==(const ClipKey& other) const {
  if (kind != other.kind) {
    return false;
  }
  switch (kind) {
    case Kind::kRect:
    case Kind::kOval:
      return rect == other.rect;
    case Kind::kRoundRect:
      return round_rect == other.round_rect;
    case Kind::kRoundSuperellipse:
      return round_superellipse == other.round_superellipse;
    case Kind::kPath:
      return path == other.path;
  }
}

std::shared_ptr<PrPicture> PrSceneBuilder::ClipShapeFor(const ClipKey& key) {
  for (ClipCacheEntry& entry : clip_cache_) {
    if (entry.key == key) {
      entry.used = true;
      return entry.shape;
    }
  }

  PrPictureBuilder shape;
  switch (key.kind) {
    case ClipKey::Kind::kRect:
      shape.ClipRect(key.rect);
      break;
    case ClipKey::Kind::kOval:
      shape.ClipOval(key.rect);
      break;
    case ClipKey::Kind::kRoundRect:
      shape.ClipRoundRect(key.round_rect);
      break;
    case ClipKey::Kind::kRoundSuperellipse:
      shape.ClipRoundSuperellipse(key.round_superellipse);
      break;
    case ClipKey::Kind::kPath:
      shape.ClipPath(key.path);
      break;
  }
  clip_cache_.push_back(ClipCacheEntry{
      .key = key,
      .shape = shape.BuildStandingClips(),
      .used = true,
  });
  return clip_cache_.back().shape;
}

void PrSceneBuilder::OpenClipGroup(std::shared_ptr<PrPicture> shape) {
  PrSceneNode group;
  group.transform = stack_.back().transform;
  group.clip = NodeClip();
  group.clip_shape = std::move(shape);
  open_.push_back(std::move(group));
  stack_.back().clip_groups++;
}

int PrSceneBuilder::GetSaveCount() const {
  return stack_.size();
}

void PrSceneBuilder::RestoreToCount(int restore_count) {
  while (GetSaveCount() > restore_count && stack_.size() > 1) {
    Restore();
  }
}

void PrSceneBuilder::Translate(Scalar tx, Scalar ty) {
  Transform(Matrix::MakeTranslation({tx, ty, 0}));
}

void PrSceneBuilder::Scale(Scalar sx, Scalar sy) {
  Transform(Matrix::MakeScale({sx, sy, 1}));
}

void PrSceneBuilder::Rotate(Scalar degrees) {
  Transform(Matrix::MakeRotationZ(Radians(Degrees(degrees))));
}

void PrSceneBuilder::Skew(Scalar sx, Scalar sy) {
  Transform(Matrix::MakeSkew(sx, sy));
}

void PrSceneBuilder::Transform2DAffine(Scalar mxx,
                                       Scalar mxy,
                                       Scalar mxt,
                                       Scalar myx,
                                       Scalar myy,
                                       Scalar myt) {
  Transform(Matrix(mxx, myx, 0.0f, 0.0f,    //
                   mxy, myy, 0.0f, 0.0f,    //
                   0.0f, 0.0f, 1.0f, 0.0f,  //
                   mxt, myt, 0.0f, 1.0f));
}

void PrSceneBuilder::TransformFullPerspective(Scalar mxx,
                                              Scalar mxy,
                                              Scalar mxz,
                                              Scalar mxt,
                                              Scalar myx,
                                              Scalar myy,
                                              Scalar myz,
                                              Scalar myt,
                                              Scalar mzx,
                                              Scalar mzy,
                                              Scalar mzz,
                                              Scalar mzt,
                                              Scalar mwx,
                                              Scalar mwy,
                                              Scalar mwz,
                                              Scalar mwt) {
  Transform(Matrix(mxx, myx, mzx, mwx,  //
                   mxy, myy, mzy, mwy,  //
                   mxz, myz, mzz, mwz,  //
                   mxt, myt, mzt, mwt));
}

void PrSceneBuilder::TransformReset() {
  stack_.back().transform = Matrix();
}

void PrSceneBuilder::Transform(const Matrix& matrix) {
  stack_.back().transform = stack_.back().transform * matrix;
}

void PrSceneBuilder::SetTransform(const Matrix& matrix) {
  stack_.back().transform = matrix;
}

Matrix PrSceneBuilder::GetMatrix() const {
  return stack_.back().transform;
}

void PrSceneBuilder::IntersectClip(const Rect& local_bounds) {
  Rect& clip = stack_.back().clip;
  clip = clip.IntersectionOrEmpty(
      local_bounds.TransformBounds(stack_.back().transform));
}

std::optional<Rect> PrSceneBuilder::NodeClip() const {
  const Rect& clip = stack_.back().clip;
  if (clip.Contains(surface_bounds_)) {
    return std::nullopt;
  }
  return clip;
}

void PrSceneBuilder::ClipRect(const Rect& rect,
                              flutter::DlClipOp clip_op,
                              bool is_aa) {
  // TODO: support difference clips. Until then what a difference clip
  // leaves has to keep admitting everything.
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }
  IntersectClip(rect);
  if (!NodeClip().has_value()) {
    return;
  }
  if (stack_.back().transform.IsAligned2D()) {
    // Use a scissor clip instead of a group.
    return;
  }
  OpenClipGroup(ClipShapeFor(ClipKey{
      .kind = ClipKey::Kind::kRect,
      .rect = rect,
  }));
}

void PrSceneBuilder::ClipOval(const Rect& bounds,
                              flutter::DlClipOp clip_op,
                              bool is_aa) {
  // The node's clip is the bounds, which over-admits at the corners;
  // the shape itself is what the group's pass applies.
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }
  IntersectClip(bounds);
  OpenClipGroup(ClipShapeFor(ClipKey{
      .kind = ClipKey::Kind::kOval,
      .rect = bounds,
  }));
}

void PrSceneBuilder::ClipRoundRect(const RoundRect& rrect,
                                   flutter::DlClipOp clip_op,
                                   bool is_aa) {
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }
  IntersectClip(rrect.GetBounds());
  OpenClipGroup(ClipShapeFor(ClipKey{
      .kind = ClipKey::Kind::kRoundRect,
      .round_rect = rrect,
  }));
}

void PrSceneBuilder::ClipRoundSuperellipse(const RoundSuperellipse& rse,
                                           flutter::DlClipOp clip_op,
                                           bool is_aa) {
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }
  IntersectClip(rse.GetBounds());
  OpenClipGroup(ClipShapeFor(ClipKey{
      .kind = ClipKey::Kind::kRoundSuperellipse,
      .round_superellipse = rse,
  }));
}

void PrSceneBuilder::ClipPath(const flutter::DlPath& path,
                              flutter::DlClipOp clip_op,
                              bool is_aa) {
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }
  IntersectClip(path.GetBounds());
  OpenClipGroup(ClipShapeFor(ClipKey{
      .kind = ClipKey::Kind::kPath,
      .path = path,
  }));
}

Rect PrSceneBuilder::GetDestinationClipCoverage() const {
  return stack_.back().clip;
}

Rect PrSceneBuilder::GetLocalClipCoverage() const {
  const State& state = stack_.back();
  if (state.clip.IsMaximum()) {
    return Rect::MakeMaximum();
  }
  return state.clip.TransformBounds(state.transform.Invert());
}

bool PrSceneBuilder::QuickReject(const Rect& bounds) const {
  const State& state = stack_.back();
  return !bounds.TransformBounds(state.transform)
              .IntersectsWithRect(state.clip);
}

void PrSceneBuilder::DrawPicture(const std::shared_ptr<PrPicture>& picture,
                                 Scalar opacity) {
  if (!picture || !picture->GetBoundsUnion().has_value()) {
    return;
  }
  const State& state = stack_.back();
  if (!picture->GetBoundsUnion()
           ->TransformBounds(state.transform)
           .IntersectsWithRect(state.clip)) {
    return;
  }

  PrSceneNode leaf;
  leaf.picture = picture;
  leaf.transform = state.transform;
  leaf.clip = NodeClip();
  // A leaf draws straight into whatever holds it, so an opacity on it
  // needs a group of its own to apply to.
  if (opacity < 1.0f) {
    PrSceneNode group;
    group.transform = state.transform;
    group.clip = leaf.clip;
    group.opacity = opacity;
    leaf.clip = std::nullopt;
    group.children.push_back(std::move(leaf));
    open_.back().children.push_back(std::move(group));
    return;
  }
  open_.back().children.push_back(std::move(leaf));
}

void PrSceneBuilder::DrawOpaquePicture(const std::shared_ptr<void>& picture,
                                       flutter::DlScalar opacity) {
  DrawPicture(std::static_pointer_cast<PrPicture>(picture), opacity);
}

}  // namespace impeller
