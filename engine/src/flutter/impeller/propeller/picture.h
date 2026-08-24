// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_PICTURE_H_
#define FLUTTER_IMPELLER_PROPELLER_PICTURE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "display_list/dl_paint.h"
#include "display_list/dl_sampling_options.h"
#include "display_list/effects/dl_color_source.h"
#include "flutter/display_list/dl_blend_mode.h"
#include "flutter/display_list/dl_canvas.h"
#include "flutter/display_list/dl_tile_mode.h"
#include "flutter/display_list/effects/dl_color_filter.h"
#include "flutter/display_list/image/dl_image.h"
#include "gradient_atlas.h"
#include "impeller/core/texture.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/path_source.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/rational.h"
#include "impeller/geometry/rect.h"
#include "impeller/geometry/rounding_radii.h"
#include "impeller/geometry/rstransform.h"
#include "impeller/geometry/scalar.h"
#include "impeller/geometry/vector.h"
#include "impeller/propeller/gradient_atlas.h"
#include "impeller/propeller/path_geometry.h"

namespace impeller {

class PrPictureBuilder;

// A draw is a simplified drawing command as a union over a primitive set.
struct Draw {
  enum class DrawType : uint8_t {
    // Rectangle draw.
    kRect,
    // Rounded rectangle or circle.
    kRRect,
    // Circle or square point cloud.
    kPoints,
    // Nested save layer.
    kLayer,
    // Rectangle clip
    kRectClip,
    // Round rectangle or circle clip
    kRRectClip,
    // Text
    kText,
    // ImageRect
    kImageRect,

    kConvexFillPath,

    kConvexClipPath,
    kConcaveClipPath,

    // A quad over a clip shape's bounds, turning what the shape
    // accumulated into coverage in the clip attachment.
    kClipResolveNonZero,
    kClipResolveEvenOdd,

    // Concave filled path and winding rules.
    kConcaveWindingAccumulate,
    kConcaveWindingResolveNonZero,
    kConcaveWindingResolveEvenOdd,

    // DrawVertices
    kDrawVertices,

    // DrawAtlas: sprites cut out of one image.
    kDrawAtlas,

    // The ambient and spot halves of an elevation shadow, both out of
    // the one silhouette.
    kShadow,

    /// A convex shape blurred by a mask filter, as a band of Gaussian
    /// coverage around its silhouette.
    kBlurredFillPath,

    // Clip coverage back to fully visible, over the region a popped
    // clip masked.
    kClipReset,

    // The pass scissor, set to the outer rect of the clips in effect.
    // Draws nothing.
    kScissor,
  };

  /// An index that names no entry.
  static constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();

  DrawType type = DrawType::kRect;
  flutter::DlBlendMode blend_mode;
  Rect rect;
  flutter::DlColor color;
  uint32_t transform;
  uint32_t gradient = -1;

  struct PointData {
    uint32_t offset;
    uint32_t length;
    bool round;
    Scalar radius;
  };

  struct Layer {
    uint32_t picture_index;
    uint32_t image_filter_index;
    uint32_t color_filter_index;
  };

  struct TextData {
    uint32_t text_index;
    Point position;
  };

  struct PathData {
    uint32_t path_index;
  };

  struct UVData {
    // Same order as Rect::GetPoints.
    Point uv[4];
    flutter::DlImageSampling sampling;
    uint32_t image_index;
  };

  struct VerticesData {
    uint32_t vertices_index;
  };

  struct AtlasData {
    uint32_t atlas_index;
  };

  struct BlurData {
    /// The silhouette, in the picture's point storage.
    uint32_t offset;
    uint32_t length;
    /// The blur's standard deviation, as the paint gave it.
    Scalar sigma;
    /// Whether that sigma is measured in the shape's own space, which is
    /// where the mesh is built, or in the device's.
    bool respect_ctm;
  };

  struct ShadowData {
    /// The silhouette, in the picture's point storage.
    uint32_t offset;
    uint32_t length;
    /// How far the occluder is off the surface, which is what decides
    /// how wide each half blurs and how far the spot is thrown.
    Scalar elevation;
    /// Whether the shadow shows through the occluder. When it does not,
    /// the halves skip the interior the occluder covers.
    bool transparent_occluder;
  };

  union {
    RoundingRadii radii;
    PointData point_data;
    Layer layer;
    TextData text_data;
    UVData uv_data;
    PathData path_data;
    VerticesData vertices_data;
    AtlasData atlas_data;
    ShadowData shadow_data;
    BlurData blur_data;
  };
};

static_assert(sizeof(Draw) == 88);

//------------------------------------------------------------------------------
/// Data for drawAtlas
struct PrAtlas {
  sk_sp<flutter::DlImage> image;
  std::vector<RSTransform> transforms;
  std::vector<Rect> textures;
  std::vector<flutter::DlColor> colors;
  flutter::DlImageSampling sampling = flutter::DlImageSampling::kLinear;
};

class PrPicture {
 public:
  PrPicture();

  ~PrPicture();

  uint64_t GetId() const { return id_; }

  const std::vector<std::shared_ptr<PrPicture>> GetPictures() const {
    return pictures_;
  }

  const std::vector<Draw>& GetDraws() const { return draws_; }

  const std::vector<Matrix>& GetTransforms() const { return transforms_; }

  const std::vector<Rect>& GetBounds() const { return bounds_; }

  const std::optional<Rect>& GetBoundsUnion() const { return bounds_union_; }

  const std::vector<Point>& GetPositions() const { return positions_; }

  const std::vector<std::shared_ptr<impeller::TextFrame>>& GetTextFrames()
      const {
    return text_frames_;
  }

  const std::vector<flutter::DlPath>& GetPaths() const { return paths_; }

  const std::vector<std::shared_ptr<const flutter::DlColorSource>>&
  GetColorSources() const {
    return color_sources_;
  }

  const std::vector<sk_sp<flutter::DlImage>>& GetImages() const {
    return image_sources_;
  }

  const std::vector<std::shared_ptr<flutter::DlImageFilter>>& GetImageFilters()
      const {
    return image_filters_;
  }

  const std::vector<std::shared_ptr<const flutter::DlColorFilter>>&
  GetColorFilters() const {
    return color_filters_;
  }

  const std::vector<std::shared_ptr<flutter::DlVertices>>& GetVertices() const {
    return vertices_;
  }

  const std::vector<PrAtlas>& GetAtlases() const { return atlases_; }

 private:
  friend class PrPictureBuilder;

  uint32_t GetCurrentTransform() { return transforms_.size() - 1; }

  const uint64_t id_;

  // Minimal draw metadata
  std::vector<Draw> draws_;
  std::vector<Matrix> transforms_;

  // Conservative bounding rect and union.
  std::vector<Rect> bounds_;
  std::optional<Rect> bounds_union_;

  // Point storage for vertex, atlas, point data.
  std::vector<Point> positions_;
  std::vector<Point> texture_coordinates_;
  std::vector<Color> colors_;

  // Dynamic.
  std::vector<std::shared_ptr<PrPicture>> pictures_;
  std::vector<sk_sp<flutter::DlImage>> image_sources_;
  std::vector<std::shared_ptr<const flutter::DlColorSource>> color_sources_;
  std::vector<std::shared_ptr<flutter::DlImageFilter>> image_filters_;
  std::vector<std::shared_ptr<const flutter::DlColorFilter>> color_filters_;
  std::vector<std::shared_ptr<impeller::TextFrame>> text_frames_;
  std::vector<std::shared_ptr<flutter::DlVertices>> vertices_;
  std::vector<PrAtlas> atlases_;
  std::vector<flutter::DlPath> paths_;
};

//------------------------------------------------------------------------------
/// Records a Propeller Picture
class PrPictureBuilder final : public flutter::DlCanvas {
 public:
  /// What the picture is being recorded onto. Everything outside it is
  /// culled, and it is what GetBaseLayerDimensions reports.
  void SetSurfaceBounds(const Rect& bounds);

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
  // |DlCanvas|
  void DrawPaint(const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawColor(
      flutter::DlColor color,
      flutter::DlBlendMode mode = flutter::DlBlendMode::kSrcOver) override;
  // |DlCanvas|
  void DrawLine(const Point& p0,
                const Point& p1,
                const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawDashedLine(const Point& p0,
                      const Point& p1,
                      Scalar on_length,
                      Scalar off_length,
                      const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawRect(const Rect& rect, const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawOval(const Rect& bounds, const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawCircle(const Point& center,
                  Scalar radius,
                  const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawRoundRect(const RoundRect& rrect,
                     const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawDiffRoundRect(const RoundRect& outer,
                         const RoundRect& inner,
                         const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawRoundSuperellipse(const RoundSuperellipse& rse,
                             const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawPath(const flutter::DlPath& path,
                const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawArc(const Rect& bounds,
               Scalar start,
               Scalar sweep,
               bool use_center,
               const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawPoints(flutter::DlPointMode mode,
                  uint32_t count,
                  const Point pts[],
                  const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawVertices(const std::shared_ptr<flutter::DlVertices>& vertices,
                    flutter::DlBlendMode mode,
                    const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawImage(const sk_sp<flutter::DlImage>& image,
                 const Point& point,
                 flutter::DlImageSampling sampling,
                 const flutter::DlPaint* paint = nullptr) override;
  // |DlCanvas|
  void DrawImageRect(const sk_sp<flutter::DlImage>& image,
                     const Rect& src,
                     const Rect& dst,
                     flutter::DlImageSampling sampling,
                     const flutter::DlPaint* paint = nullptr,
                     flutter::DlSrcRectConstraint constraint =
                         flutter::DlSrcRectConstraint::kFast) override;
  using flutter::DlCanvas::DrawImageRect;
  // |DlCanvas|
  void DrawImageNine(const sk_sp<flutter::DlImage>& image,
                     const IRect32& center,
                     const Rect& dst,
                     flutter::DlFilterMode filter,
                     const flutter::DlPaint* paint = nullptr) override;
  // |DlCanvas|
  void DrawAtlas(const sk_sp<flutter::DlImage>& atlas,
                 const RSTransform xform[],
                 const Rect tex[],
                 const flutter::DlColor colors[],
                 int count,
                 flutter::DlBlendMode mode,
                 flutter::DlImageSampling sampling,
                 const Rect* cull_rect,
                 const flutter::DlPaint* paint = nullptr) override;
  // |DlCanvas|
  void DrawDisplayList(const sk_sp<flutter::DisplayList> display_list,
                       Scalar opacity = 1.0f) override;

  /// Reference an already-built picture.
  void DrawPicture(const std::shared_ptr<PrPicture>& picture,
                   Scalar opacity = 1);

  // |DlCanvas|
  void DrawOpaquePicture(const std::shared_ptr<void>& picture,
                         flutter::DlScalar opacity) override;
  // |DlCanvas|
  void DrawText(const std::shared_ptr<flutter::DlText>& text,
                Scalar x,
                Scalar y,
                const flutter::DlPaint& paint) override;
  // |DlCanvas|
  void DrawShadow(const flutter::DlPath& path,
                  const flutter::DlColor color,
                  const Scalar elevation,
                  bool transparent_occluder,
                  Scalar dpr) override;
  // |DlCanvas|
  void Flush() override {}

  std::shared_ptr<PrPicture> Build();

  /// Build a picture whose clips are left standing rather than popped:
  /// what it recorded is a prologue for a pass, and the pass boundary
  /// is the restore.
  std::shared_ptr<PrPicture> BuildStandingClips();

  explicit PrPictureBuilder(Scalar dpr = 1.0f);

  ~PrPictureBuilder();

  PrPictureBuilder(const PrPictureBuilder&) = delete;
  PrPictureBuilder& operator=(const PrPictureBuilder&) = delete;

 private:
  void Reset();

  void FlushTransform();

  Rect ComputeCoverage(const Rect& local_bounds) const;

  /// Record a filled path, as one draw if it is convex and as an
  /// accumulate and a resolve if it is not.
  void FillPath(const flutter::DlPath& path, const flutter::DlPaint& paint);

  /// Record `path` as a band of Gaussian coverage when the paint carries
  /// a blur this can draw, and say whether it did. A paint with no mask
  /// filter, a style other than normal, or a shape with no single
  /// silhouette to ring falls through to the ordinary fill.
  bool FillBlurredPath(const flutter::DlPath& path,
                       const flutter::DlPaint& paint);

  /// Flatten the path's contour into `shadow_silhouette_`, returning
  /// what it bounds, or nothing when it has too few points to bound
  /// anything.
  std::optional<Rect> FlattenSilhouette(const flutter::DlPath& path);

  /// Record a stroked shape, whichever draw asked for it. A stroke is
  /// the fill of its own outline, so this is where it becomes one.
  void StrokePath(const flutter::DlPath& path, const flutter::DlPaint& paint);

  void IntersectCullRect(const Rect& local_bounds);

  void AppendDraw(Draw draw, Rect bounds);

  /// Record a clip: the shape, which accumulates its winding, and the
  /// resolve that turns it into coverage.
  void AppendClip(Draw shape, Draw::DrawType resolve, Rect clip);

  /// Rebuild the clip coverage without the clips pushed since `depth`.
  void PopClips(size_t depth);

  /// Record the current cull rect as the pass scissor. Every clip that
  /// narrows the cull rect follows itself with one of these, and popping
  /// clips emits one for the rect that came back.
  void RecordScissor();

  /// The same, for a rect of its own: a pop bounds the clips it replays
  /// to the region it just put back.
  void RecordScissor(Rect local_bounds);

  struct ClipEntry {
    Draw shape;
    Draw resolve;
    Rect coverage;
  };
  /// The clips in effect for the picture being recorded, innermost
  /// last. A restore rebuilds the coverage out of these rather than
  /// trying to undo one.
  std::vector<ClipEntry> clip_stack_;

  /// Push a draw and what it covers, both already resolved.
  void RecordDraw(const Draw& draw, Rect coverage);

  /// The same, for a clip: a clip bounds the content that follows it and
  /// never contributes any of its own.
  void RecordClip(const Draw& draw, Rect coverage);

  /// Copy `source`'s draws into the recording, placed by the current
  /// transform. Every index a draw carries is rebased onto the streams
  /// they were appended to.
  void AppendPicture(const PrPicture& source);

  flutter::DlColor ComputeImagePaintColor(const flutter::DlPaint* paint);

  flutter::DlColor ComputeDrawPaintColor(const flutter::DlPaint* paint);

  uint32_t AddImage(const sk_sp<flutter::DlImage>& image);

  uint32_t AddImageFilter(
      const std::shared_ptr<flutter::DlImageFilter>& filter);

  uint32_t AddColorFilter(
      const std::shared_ptr<const flutter::DlColorFilter>& filter);

  uint32_t GetGradientIndex(
      const std::shared_ptr<const flutter::DlColorSource>& source);

  const float dpr_;
  Rect surface_bounds_ = Rect::MakeMaximum();

  struct CanvasStackEntry {
    Matrix transform;
    bool composite = false;
    Scalar distributed_opacity = 1.0f;

    /// How many clips were in effect when this was pushed: what a
    /// restore has to get back to.
    size_t clip_depth = 0;

    // Rectangular culling rect, in root canvas space.
    Rect cull_rect = Rect::MakeMaximum();

    // Only valid on compositing saveLayers, these are applied to
    // the resulting draw.
    std::shared_ptr<flutter::DlImageFilter> image_filter;
    std::shared_ptr<const flutter::DlColorFilter> color_filter;
  };

  bool transform_dirty_;

  /// Where a shadow's silhouette is worked out. A shadow is recorded
  /// afresh every frame and none of this outlives the call, so the room
  /// for it is kept rather than asked for again.
  ConvexContour shadow_contour_;
  std::vector<Point> shadow_silhouette_;
  std::vector<CanvasStackEntry> save_stack_;
  std::vector<std::shared_ptr<PrPicture>> pictures_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_PICTURE_H_
