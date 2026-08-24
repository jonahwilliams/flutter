// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_SCENE_H_
#define FLUTTER_IMPELLER_PROPELLER_SCENE_H_

#include <memory>
#include <optional>
#include <vector>

#include "flutter/display_list/dl_canvas.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/rect.h"
#include "impeller/propeller/picture.h"

namespace impeller {

//------------------------------------------------------------------------------
/// One node of the scene a layer tree paints.
struct PrSceneNode {
  Matrix transform;
  std::optional<Rect> clip;

  std::shared_ptr<PrPicture> picture;
  std::vector<PrSceneNode> children;

  Scalar opacity = 1.0f;
  std::shared_ptr<flutter::DlImageFilter> image_filter;
  std::shared_ptr<const flutter::DlColorFilter> color_filter;

  /// What this group shows through itself: everything drawn into the
  /// same target before it, put through this filter and composited
  /// underneath the group's own content.
  ///
  /// Only a scene node ever carries one. A backdrop reaches the builder
  /// through the layer state stack, whose delegate is the scene, so a
  /// picture never sees one.
  std::shared_ptr<flutter::DlImageFilter> backdrop_filter;
  /// How the filtered backdrop goes down over what it read.
  BlendMode backdrop_blend = BlendMode::kSrcOver;
  /// Names a backdrop that several groups share, so the one they read
  /// is resolved once rather than once each.
  std::optional<int64_t> backdrop_id;
  /// Where the group shows it, in root space. A backdrop reads only what
  /// lands here, so this is what decides both how much has to be
  /// resolved and which of what came before decides it.
  std::optional<Rect> backdrop_bounds;

  /// The shape this group clips its content to, recorded as a picture
  /// of nothing but clip draws. Its own pass applies it, and the pass
  /// boundary is the restore.
  std::shared_ptr<PrPicture> clip_shape;

  bool IsLeaf() const { return picture != nullptr; }

  /// Whether the group has to resolve into a texture of its own rather
  /// than letting its children draw straight into its parent.
  bool NeedsComposite() const {
    return opacity < 1.0f || image_filter != nullptr ||
           color_filter != nullptr || clip_shape != nullptr;
  }
};

//------------------------------------------------------------------------------
/// Paints a flow layer tree into a scene.
///
/// A DlCanvas so the layer tree paints straight in, but only the part of
/// the interface a layer tree reaches for does anything: transforms,
/// clips, save layers, and drawing a picture. A layer tree draws no
/// geometry of its own.
class PrSceneBuilder final : public flutter::DlCanvas {
 public:
  explicit PrSceneBuilder(const Rect& surface_bounds);

  ~PrSceneBuilder() override;

  /// What the scene is being painted onto. Everything outside it is
  /// culled, so a resize has to say so before the next frame.
  void SetSurfaceBounds(const Rect& bounds);

  /// The scene as painted so far, and a fresh root to paint the next one
  /// into.
  PrSceneNode Build();

  // |DlCanvas|
  flutter::DlISize GetBaseLayerDimensions() const override;
  // |DlCanvas|
  SkImageInfo GetImageInfo() const override;
  // |DlCanvas|
  void Save() override;
  // |DlCanvas|
  void SaveLayer(const std::optional<flutter::DlRect>& bounds,
                 const flutter::DlPaint* paint = nullptr,
                 const flutter::DlImageFilter* backdrop = nullptr,
                 std::optional<int64_t> backdrop_id = std::nullopt) override;
  // |DlCanvas|
  void Restore() override;
  // |DlCanvas|
  int GetSaveCount() const override;
  // |DlCanvas|
  void RestoreToCount(int restore_count) override;
  // |DlCanvas|
  void Translate(Scalar tx, Scalar ty) override;
  // |DlCanvas|
  void Scale(Scalar sx, Scalar sy) override;
  // |DlCanvas|
  void Rotate(Scalar degrees) override;
  // |DlCanvas|
  void Skew(Scalar sx, Scalar sy) override;
  // |DlCanvas|
  void Transform2DAffine(Scalar mxx,
                         Scalar mxy,
                         Scalar mxt,
                         Scalar myx,
                         Scalar myy,
                         Scalar myt) override;
  // |DlCanvas|
  void TransformFullPerspective(Scalar mxx,
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
                                Scalar mwt) override;
  // |DlCanvas|
  void TransformReset() override;
  // |DlCanvas|
  void Transform(const Matrix& matrix) override;
  // |DlCanvas|
  void SetTransform(const Matrix& matrix) override;
  // |DlCanvas|
  Matrix GetMatrix() const override;
  // |DlCanvas|
  void ClipRect(const Rect& rect,
                flutter::DlClipOp clip_op = flutter::DlClipOp::kIntersect,
                bool is_aa = false) override;
  // |DlCanvas|
  void ClipOval(const Rect& bounds,
                flutter::DlClipOp clip_op = flutter::DlClipOp::kIntersect,
                bool is_aa = false) override;
  // |DlCanvas|
  void ClipRoundRect(const RoundRect& rrect,
                     flutter::DlClipOp clip_op = flutter::DlClipOp::kIntersect,
                     bool is_aa = false) override;
  // |DlCanvas|
  void ClipRoundSuperellipse(
      const RoundSuperellipse& rse,
      flutter::DlClipOp clip_op = flutter::DlClipOp::kIntersect,
      bool is_aa = false) override;
  // |DlCanvas|
  void ClipPath(const flutter::DlPath& path,
                flutter::DlClipOp clip_op = flutter::DlClipOp::kIntersect,
                bool is_aa = false) override;
  // |DlCanvas|
  Rect GetDestinationClipCoverage() const override;
  // |DlCanvas|
  Rect GetLocalClipCoverage() const override;
  // |DlCanvas|
  bool QuickReject(const Rect& bounds) const override;

  /// Add a picture to the scene at the current transform and clip.
  void DrawPicture(const std::shared_ptr<PrPicture>& picture, Scalar opacity);

  // |DlCanvas|
  void DrawOpaquePicture(const std::shared_ptr<void>& picture,
                         flutter::DlScalar opacity) override;

  // A layer tree draws no geometry of its own, so the rest of the canvas
  // is here to satisfy the interface and does nothing.

  // |DlCanvas|
  void DrawPaint(const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawColor(flutter::DlColor color, flutter::DlBlendMode mode) override {}
  // |DlCanvas|
  void DrawLine(const Point& p0,
                const Point& p1,
                const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawDashedLine(const Point& p0,
                      const Point& p1,
                      Scalar on_length,
                      Scalar off_length,
                      const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawRect(const Rect& rect, const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawOval(const Rect& bounds, const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawCircle(const Point& center,
                  Scalar radius,
                  const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawRoundRect(const RoundRect& rrect,
                     const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawDiffRoundRect(const RoundRect& outer,
                         const RoundRect& inner,
                         const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawRoundSuperellipse(const RoundSuperellipse& rse,
                             const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawPath(const flutter::DlPath& path,
                const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawArc(const Rect& bounds,
               Scalar start,
               Scalar sweep,
               bool use_center,
               const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawPoints(flutter::DlPointMode mode,
                  uint32_t count,
                  const Point pts[],
                  const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawVertices(const std::shared_ptr<flutter::DlVertices>& vertices,
                    flutter::DlBlendMode mode,
                    const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawImage(const sk_sp<flutter::DlImage>& image,
                 const Point& point,
                 flutter::DlImageSampling sampling,
                 const flutter::DlPaint* paint = nullptr) override {}
  // |DlCanvas|
  void DrawImageRect(const sk_sp<flutter::DlImage>& image,
                     const Rect& src,
                     const Rect& dst,
                     flutter::DlImageSampling sampling,
                     const flutter::DlPaint* paint = nullptr,
                     flutter::DlSrcRectConstraint constraint =
                         flutter::DlSrcRectConstraint::kFast) override {}
  using flutter::DlCanvas::DrawImageRect;
  // |DlCanvas|
  void DrawImageNine(const sk_sp<flutter::DlImage>& image,
                     const IRect32& center,
                     const Rect& dst,
                     flutter::DlFilterMode filter,
                     const flutter::DlPaint* paint = nullptr) override {}
  // |DlCanvas|
  void DrawAtlas(const sk_sp<flutter::DlImage>& atlas,
                 const RSTransform xform[],
                 const Rect tex[],
                 const flutter::DlColor colors[],
                 int count,
                 flutter::DlBlendMode mode,
                 flutter::DlImageSampling sampling,
                 const Rect* cull_rect,
                 const flutter::DlPaint* paint = nullptr) override {}
  // |DlCanvas|
  void DrawDisplayList(const sk_sp<flutter::DisplayList> display_list,
                       Scalar opacity = 1.0f) override {}
  // |DlCanvas|
  void DrawText(const std::shared_ptr<flutter::DlText>& text,
                Scalar x,
                Scalar y,
                const flutter::DlPaint& paint) override {}
  // |DlCanvas|
  void DrawShadow(const flutter::DlPath& path,
                  const flutter::DlColor color,
                  const Scalar elevation,
                  bool transparent_occluder,
                  Scalar dpr) override {}
  // |DlCanvas|
  void Flush() override {}

  PrSceneBuilder(const PrSceneBuilder&) = delete;
  PrSceneBuilder& operator=(const PrSceneBuilder&) = delete;

 private:
  struct State {
    Matrix transform;
    /// In root space, like the nodes'.
    Rect clip;
    /// Whether this save opened a group that its restore has to close.
    bool group = false;
    /// How many groups the clips taken since this save opened. A clip
    /// runs to the enclosing restore rather than to one of its own, so
    /// that restore closes all of them.
    uint32_t clip_groups = 0;
  };

  /// Narrow the current clip by a shape whose conservative extent in
  /// local space is `local_bounds`.
  void IntersectClip(const Rect& local_bounds);

  /// A clip shape, as the cache compares them. Two frames that ask for
  /// the same shape get the same picture, and a pass is keyed by the
  /// ids of the pictures it holds.
  struct ClipKey {
    enum class Kind : uint8_t {
      kRect,
      kOval,
      kRoundRect,
      kRoundSuperellipse,
      kPath,
    };

    Kind kind = Kind::kRect;
    Rect rect;
    RoundRect round_rect;
    RoundSuperellipse round_superellipse;
    flutter::DlPath path;

    bool operator==(const ClipKey& other) const;
  };

  struct ClipCacheEntry {
    ClipKey key;
    std::shared_ptr<PrPicture> shape;
    /// Whether the frame being recorded asked for it.
    bool used = false;
  };

  /// The picture that records `key`, recording it if the last frame did
  /// not already.
  std::shared_ptr<PrPicture> ClipShapeFor(const ClipKey& key);

  /// Open a group that clips its content to `shape`, which runs until
  /// the enclosing restore closes it.
  void OpenClipGroup(std::shared_ptr<PrPicture> shape);

  /// Close the innermost open group into the one that holds it.
  void CloseGroup();

  /// The clip as a node records it: nothing when it admits the whole
  /// surface, since that is no clip at all.
  std::optional<Rect> NodeClip() const;

  Rect surface_bounds_;
  /// One frame deep: what a frame does not ask for is dropped when it
  /// ends, so a clip that comes and goes is recorded again.
  std::vector<ClipCacheEntry> clip_cache_;
  std::vector<State> stack_;
  /// The groups being filled, outermost first. The last is the one
  /// content is added to; the first is the scene's root.
  std::vector<PrSceneNode> open_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_SCENE_H_
