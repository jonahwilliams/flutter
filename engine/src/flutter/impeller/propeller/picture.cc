// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/picture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

#include "display_list/dl_color.h"
#include "display_list/dl_paint.h"
#include "display_list/dl_types.h"
#include "display_list/effects/color_filters/dl_blend_color_filter.h"
#include "display_list/effects/dl_color_source.h"
#include "display_list/effects/dl_mask_filter.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/display_list/skia/dl_sk_conversions.h"
#include "flutter/fml/logging.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/rounding_radii.h"
#include "third_party/skia/include/core/SkPathUtils.h"

namespace impeller {

namespace {

static constexpr flutter::DlImageSampling kFilterToSampling[] = {
    flutter::DlImageSampling::kNearestNeighbor,
    flutter::DlImageSampling::kLinear};

/// The corners of `src` within an image of `image_bounds`.
Draw::UVData MakeUVData(const Rect& src,
                        const Rect& image_bounds,
                        flutter::DlImageSampling sampling,
                        uint32_t image_index) {
  Scalar width = image_bounds.GetWidth();
  Scalar height = image_bounds.GetHeight();
  Rect uv = Rect::MakeLTRB(width > 0 ? src.GetLeft() / width : 0,
                           height > 0 ? src.GetTop() / height : 0,
                           width > 0 ? src.GetRight() / width : 1,
                           height > 0 ? src.GetBottom() / height : 1);
  std::array<Point, 4> corners = uv.GetPoints();
  return Draw::UVData{
      .uv = {corners[0], corners[1], corners[2], corners[3]},
      .sampling = sampling,
      .image_index = image_index,
  };
}

/// Half of one minus root two over two: how far in from a side the
/// largest rect inside an ellipse starts.
constexpr Scalar kOvalInset = 0.14645f;

/// A rect wholly inside `rrect`, which is its bounds pulled in by the
/// widest radius on each side. Conservative: the true interior is wider
/// than this through the middle of each edge.
Rect InteriorOf(const RoundRect& rrect) {
  const RoundingRadii& radii = rrect.GetRadii();
  const Rect bounds = rrect.GetBounds();
  const Scalar left = std::max(radii.top_left.width, radii.bottom_left.width);
  const Scalar right =
      std::max(radii.top_right.width, radii.bottom_right.width);
  const Scalar top = std::max(radii.top_left.height, radii.top_right.height);
  const Scalar bottom =
      std::max(radii.bottom_left.height, radii.bottom_right.height);
  return Rect::MakeLTRB(bounds.GetLeft() + left, bounds.GetTop() + top,
                        bounds.GetRight() - right, bounds.GetBottom() - bottom);
}

bool IsStateful(Draw::DrawType type) {
  static constexpr bool kStatefulTable[] = {
      false,  // kRect
      false,  // kRRect
      false,  // kPoints
      false,  // kLayer
      true,   // kRectClip
      true,   // kRRectClip
      false,  // kText
      false,  // kImageRect
      false,  // kConvexFillPath
      true,   // kConvexClipPath
      true,   // kConcaveClipPath
      true,   // kClipResolveNonZero
      true,   // kClipResolveEvenOdd
      false,  // kConcaveWindingAccumulate
      false,  // kConcaveWindingResolveNonZero
      false,  // kConcaveWindingResolveEvenOdd
      false,  // kDrawVertices
      false,  // kDrawAtlas
      false,  // kShadow
      false,  // kBlurredFillPath
      true,   // kClipReset
      true,   // kScissor
  };
  return kStatefulTable[static_cast<size_t>(type)];
}

uint32_t GetMergeType(Draw::DrawType type) {
  static constexpr uint32_t kMergeTypeTable[] = {
      1,   // kRect
      2,   // kRRect
      1,   // kPoints
      1,   // kLayer
      3,   // kRectClip
      4,   // kRRectClip
      1,   // kText
      1,   // kImageRect
      2,   // kConvexFillPath
      4,   // kConvexClipPath
      4,   // kConcaveClipPath
      5,   // kClipResolveNonZero
      6,   // kClipResolveEvenOdd
      4,   // kConcaveWindingAccumulate
      7,   // kConcaveWindingResolveNonZero
      8,   // kConcaveWindingResolveEvenOdd
      1,   // kDrawVertices
      1,   // kDrawAtlas
      1,   // kShadow
      1,   // kBlurredFillPath
      9,   // kClipReset
      10,  // kScissor
  };
  return kMergeTypeTable[static_cast<size_t>(type)];
}

constexpr uint32_t kColorMaskType = 1;
constexpr uint32_t kCoverageMaskType = 2;

uint32_t GetDrawLayerMask(Draw::DrawType type) {
  static constexpr uint32_t kLayerTable[] = {
      kColorMaskType,     // kRect (Color)
      kColorMaskType,     // kRRect (Color)
      kColorMaskType,     // kPoints (Color)
      kColorMaskType,     // kLayer (Color)
      kCoverageMaskType,  // kRectClip (Stencil)
      kCoverageMaskType,  // kRRectClip (Stencil)
      kColorMaskType,     // kText (Color)
      kColorMaskType,     // kImageRect (Color)
      kColorMaskType,     // kConvexFillPath (Color)
      kCoverageMaskType,  // kConvexClipPath (Stencil)
      kCoverageMaskType,  // kConcaveClipPath (Stencil)
      kCoverageMaskType,  // kClipResolveNonZero (Stencil)
      kCoverageMaskType,  // kClipResolveEvenOdd (Stencil)
      kCoverageMaskType,  // kConcaveWindingAccumulate (Stencil)
      // A winding resolve reads the accumulator and zeroes it on the way
      // out, so it owns the coverage layer as well as the colour one.
      // Tagged colour alone it looks reorderable against an accumulate,
      // and the accumulate that follows it gets hoisted back to join the
      // one before -- leaving both shapes summed into the accumulator
      // and each resolved with the other's winding.
      kColorMaskType | kCoverageMaskType,  // kConcaveWindingResolveNonZero
      kColorMaskType | kCoverageMaskType,  // kConcaveWindingResolveEvenOdd
      kColorMaskType,                      // kDrawVertices (Color)
      kColorMaskType,                      // kDrawAtlas (Color)
      kColorMaskType,                      // kShadow (Color)
      kColorMaskType,                      // kBlurredFillPath (Color)
      kCoverageMaskType,                   // kClipReset (Stencil)
      kCoverageMaskType,                   // kScissor (Stencil)
  };
  return kLayerTable[static_cast<size_t>(type)];
}

/// The slot `value` occupies in `list`, appending it unless the entry
/// before it is already the same object.
template <typename Container, typename Value>
uint32_t AddUnique(Container& list, const Value& value) {
  if (list.empty() || list.back() != value) {
    list.push_back(value);
  }
  return list.size() - 1;
}

}  // namespace

namespace {

uint64_t NextPictureId() {
  static std::atomic<uint64_t> next{1};
  return next++;
}

}  // namespace

PrPicture::PrPicture() : id_(NextPictureId()) {}

PrPicture::~PrPicture() = default;

PrPictureBuilder::PrPictureBuilder(Scalar dpr) : dpr_(dpr) {
  Reset();
}

PrPictureBuilder::~PrPictureBuilder() = default;

Rect ExpandForFilterInput(const Rect& coverage,
                          const flutter::DlImageFilter* filter,
                          const Matrix& ctm) {
  if (filter == nullptr || coverage.IsEmpty() || coverage.IsMaximum()) {
    return coverage;
  }
  flutter::DlIRect in;
  if (filter->get_input_device_bounds(flutter::DlIRect::RoundOut(coverage), ctm,
                                      in) == nullptr) {
    return coverage;
  }
  return Rect::MakeLTRB(in.GetLeft(), in.GetTop(), in.GetRight(),
                        in.GetBottom());
}

Rect ExpandForFilter(const Rect& coverage,
                     const flutter::DlImageFilter* filter,
                     const Matrix& ctm) {
  if (filter == nullptr || coverage.IsEmpty() || coverage.IsMaximum()) {
    return coverage;
  }
  flutter::DlIRect out;
  if (filter->map_device_bounds(flutter::DlIRect::RoundOut(coverage), ctm,
                                out) == nullptr) {
    return coverage;
  }
  return Rect::MakeLTRB(out.GetLeft(), out.GetTop(), out.GetRight(),
                        out.GetBottom());
}

void PrPictureBuilder::Reset() {
  Matrix base = Matrix::MakeScale({dpr_, dpr_, 1});

  pictures_.clear();
  pictures_.push_back(std::make_shared<PrPicture>());
  pictures_.back()->transforms_.push_back(base);

  // The base entry the save stack is never popped below: GetSaveCount
  // counts the entries above it.
  save_stack_.clear();
  save_stack_.push_back(CanvasStackEntry{.transform = base});

  transform_dirty_ = false;
}

void PrPictureBuilder::SetSurfaceBounds(const Rect& bounds) {
  surface_bounds_ = bounds;
  // Nothing outside the surface can be seen, so it bounds the recording
  // the way the outermost clip would.
  save_stack_.back().cull_rect = bounds;
}

flutter::DlISize PrPictureBuilder::GetBaseLayerDimensions() const {
  return flutter::DlISize(static_cast<int32_t>(surface_bounds_.GetWidth()),
                          static_cast<int32_t>(surface_bounds_.GetHeight()));
}

SkImageInfo PrPictureBuilder::GetImageInfo() const {
  return SkImageInfo::MakeUnknown(0, 0);
}

void PrPictureBuilder::Save() {
  Matrix current_transform = save_stack_.back().transform;
  Rect cull_rect = save_stack_.back().cull_rect;

  save_stack_.push_back(CanvasStackEntry{
      .transform = current_transform,
      .composite = false,
      .distributed_opacity = 1.0f,
      .clip_depth = clip_stack_.size(),
      .cull_rect = cull_rect,  //
  });
}

void PrPictureBuilder::SaveLayer(const std::optional<flutter::DlRect>& bounds,
                                 const flutter::DlPaint* paint,
                                 const flutter::DlImageFilter* backdrop,
                                 std::optional<int64_t> backdrop_id) {
  Matrix current_transform = save_stack_.back().transform;
  Rect cull_rect = save_stack_.back().cull_rect;
  const flutter::DlPaint attributes = paint ? *paint : flutter::DlPaint();

  // Expand cull rect based on filter inputs, as these may sample outside of the
  // exact culling rect.
  cull_rect = ExpandForFilterInput(cull_rect, attributes.getImageFilterPtr(),
                                   current_transform);

  pictures_.push_back(std::make_shared<PrPicture>());
  pictures_.back()->transforms_.push_back(current_transform);
  transform_dirty_ = false;
  save_stack_.push_back(CanvasStackEntry{
      .transform = current_transform,
      .composite = true,
      .distributed_opacity = attributes.getOpacity(),
      .clip_depth = clip_stack_.size(),
      .cull_rect = cull_rect,
      .image_filter = attributes.getImageFilter(),
      .color_filter = attributes.getColorFilter(),  //
  });

  // The layer declares where its content is, so anything it draws
  // outside that rect is content nothing will show: it bounds the layer
  // the way a clip does.
  if (bounds.has_value() && backdrop == nullptr) {
    IntersectCullRect(bounds.value());
  }
}

void PrPictureBuilder::Restore() {
  Matrix old_transform = save_stack_.back().transform;

  if (save_stack_.back().composite) {
    std::shared_ptr<PrPicture> layer = pictures_.back();
    CanvasStackEntry stack = save_stack_.back();
    clip_stack_.erase(clip_stack_.begin() + stack.clip_depth,
                      clip_stack_.end());
    pictures_.pop_back();
    save_stack_.pop_back();
    if (old_transform != save_stack_.back().transform) {
      transform_dirty_ = true;
    }

    // The layer's bounds are already in the picture root coordinates, and
    // already limited by the clips that were in effect while it recorded.
    Rect coverage = layer->bounds_union_.value_or(Rect());
    if (coverage.IsEmpty()) {
      // Nothing survived the clip, so there is no layer to composite and
      // no reason to keep the picture that recorded it.
      return;
    }
    // What the filter shows, rather than what the content drew. The draw
    // below is culled against this and the picture above unions it in,
    // so a layer whose blur reaches into view has to say so here or be
    // dropped for content that does not.
    coverage =
        ExpandForFilter(coverage, stack.image_filter.get(), stack.transform);

    // The draw's rect is in the coordinates local to it, so it goes back
    // through the transform it will be drawn under. The maximum rect is
    // never transformed: it saturates to infinity, and transforming an
    // infinite rect lands on NaN, which reads as empty.
    Matrix inverse = save_stack_.back().transform.Invert();
    Rect local_bounds = coverage.IsMaximum()
                            ? Rect::MakeMaximum()
                            : coverage.TransformBounds(inverse);

    uint32_t picture_index = pictures_.back()->pictures_.size();
    pictures_.back()->pictures_.push_back(layer);

    // The filters composite the layer into this picture, so they belong
    // to it and not to the one that recorded the content.
    uint32_t image_filter_index = stack.image_filter
                                      ? AddImageFilter(stack.image_filter)
                                      : Draw::kNoIndex;
    uint32_t color_filter_index = stack.color_filter
                                      ? AddColorFilter(stack.color_filter)
                                      : Draw::kNoIndex;

    FlushTransform();
    AppendDraw(
        Draw{
            .type = Draw::DrawType::kLayer,
            .blend_mode = BlendMode::kSrcOver,
            .rect = local_bounds,
            .color = flutter::DlColor::kWhite().withAlphaF(
                stack.distributed_opacity),
            .layer =
                Draw::Layer{
                    .picture_index = picture_index,
                    .image_filter_index = image_filter_index,
                    .color_filter_index = color_filter_index,  //
                },
        },
        coverage, local_bounds);
    return;
  }

  const size_t depth = save_stack_.back().clip_depth;
  save_stack_.pop_back();
  if (old_transform != save_stack_.back().transform) {
    transform_dirty_ = true;
  }
  PopClips(depth);
}

void PrPictureBuilder::PopClips(size_t depth) {
  if (clip_stack_.size() <= depth) {
    return;
  }
  // TODO: claudlish
  // What the popped clips could have masked, which is what the reset
  // has to reach and never more than the cull rect they narrowed.
  Rect restored;
  bool any_live = false;
  for (auto it = clip_stack_.begin() + depth; it != clip_stack_.end(); it++) {
    if (!it->live) {
      continue;  // Never drawn, so there is nothing of it to undo.
    }
    any_live = true;
    restored = restored.Union(it->coverage);
  }
  clip_stack_.erase(clip_stack_.begin() + depth, clip_stack_.end());
  if (!any_live) {
    // Nothing was masked, so nothing has to be put back. The cull rect
    // still widened, which the scissor follows.
    RecordScissor();
    return;
  }

  // Coverage multiplies, so a clip replayed over a region that was not
  // put back is multiplied into itself. The scissor holds the reset and
  // the replay to the region the popped clips masked, which is all of
  // what they masked and none of what the survivors did.
  const Rect local =
      restored.TransformBounds(save_stack_.back().transform.Invert());
  RecordScissor(local);

  FlushTransform();
  Draw reset{
      .type = Draw::DrawType::kClipReset,
      .blend_mode = BlendMode::kSrcOver,
      .rect = local,
      .color = flutter::DlColor(),  //
  };
  reset.transform = pictures_.back()->GetCurrentTransform();
  // Maximum coverage: the clips below narrow again, and a pass that
  // skipped the reset would multiply them in a second time.
  RecordClip(reset, Rect::MakeMaximum());
  for (const ClipEntry& clip : clip_stack_) {
    if (!clip.live) {
      continue;  // The reset did not take away what was never applied.
    }
    RecordClip(clip.shape, clip.coverage);
    RecordClip(clip.resolve, clip.coverage);
  }

  // Back to what the clips that survived admit.
  RecordScissor();
}

int PrPictureBuilder::GetSaveCount() const {
  return save_stack_.size() - 1;
}

void PrPictureBuilder::RestoreToCount(int restore_count) {
  // Restore until the count is what was asked for, which is not the
  // same as restoring that many times: a caller records the count it
  // wants to come back to, not how deep it went.
  while (GetSaveCount() > restore_count && save_stack_.size() > 1) {
    Restore();
  }
}

void PrPictureBuilder::Translate(Scalar tx, Scalar ty) {
  save_stack_.back().transform =
      save_stack_.back().transform * Matrix::MakeTranslation({tx, ty, 0});
  transform_dirty_ = true;
}

void PrPictureBuilder::Scale(Scalar sx, Scalar sy) {
  save_stack_.back().transform =
      save_stack_.back().transform * Matrix::MakeScale({sx, sy, 1});
  transform_dirty_ = true;
}

void PrPictureBuilder::Rotate(Scalar degrees) {
  save_stack_.back().transform =
      save_stack_.back().transform *
      Matrix::MakeRotationZ(Radians(Degrees(degrees)));
  transform_dirty_ = true;
}

void PrPictureBuilder::Skew(Scalar sx, Scalar sy) {
  save_stack_.back().transform =
      save_stack_.back().transform * Matrix::MakeSkew(sx, sy);
  transform_dirty_ = true;
}

void PrPictureBuilder::Transform2DAffine(Scalar mxx,
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

void PrPictureBuilder::TransformFullPerspective(Scalar mxx,
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

void PrPictureBuilder::TransformReset() {
  save_stack_.back().transform = Matrix();
  transform_dirty_ = true;
}

void PrPictureBuilder::Transform(const Matrix& matrix) {
  save_stack_.back().transform = save_stack_.back().transform * matrix;
  transform_dirty_ = true;
}

void PrPictureBuilder::SetTransform(const Matrix& matrix) {
  save_stack_.back().transform = matrix;
  transform_dirty_ = true;
}

Matrix PrPictureBuilder::GetMatrix() const {
  return save_stack_.back().transform;
}

void PrPictureBuilder::ClipRect(const Rect& rect,
                                flutter::DlClipOp clip_op,
                                bool is_aa) {
  // TODO: support difference clips. Until the recording can express
  // one, nothing is recorded and the cull rect keeps admitting
  // everything the clip might leave.
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }
  IntersectCullRect(rect);
  FlushTransform();
  AppendClip(
      Draw{
          .type = Draw::DrawType::kRectClip,
          .blend_mode = BlendMode::kSrcOver,
          .rect = rect,
          .color = flutter::DlColor(),  //
      },
      Draw::DrawType::kClipResolveNonZero, save_stack_.back().cull_rect,
      /*interior=*/rect);
}

void PrPictureBuilder::ClipOval(const Rect& bounds,
                                flutter::DlClipOp clip_op,
                                bool is_aa) {
  // TODO: support difference clips.
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }
  IntersectCullRect(bounds);
  FlushTransform();
  AppendClip(
      Draw{
          .type = Draw::DrawType::kRRectClip,
          .blend_mode = BlendMode::kSrcOver,
          .rect = bounds,
          .color = flutter::DlColor(),                             //
          .radii = RoundingRadii::MakeRadii(bounds.GetSize() / 2)  //
      },
      Draw::DrawType::kClipResolveNonZero, save_stack_.back().cull_rect,
      /*interior=*/
      bounds.Expand(-bounds.GetSize().width * kOvalInset,
                    -bounds.GetSize().height * kOvalInset));
}

void PrPictureBuilder::ClipRoundRect(const RoundRect& rrect,
                                     flutter::DlClipOp clip_op,
                                     bool is_aa) {
  // TODO: support difference clips.
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }
  IntersectCullRect(rrect.GetBounds());
  FlushTransform();
  AppendClip(
      Draw{
          .type = Draw::DrawType::kRRectClip,
          .blend_mode = BlendMode::kSrcOver,
          .rect = rrect.GetBounds(),
          .color = flutter::DlColor(),  //
          .radii = rrect.GetRadii()     //
      },
      Draw::DrawType::kClipResolveNonZero, save_stack_.back().cull_rect,
      /*interior=*/InteriorOf(rrect));
}

void PrPictureBuilder::ClipRoundSuperellipse(const RoundSuperellipse& rse,
                                             flutter::DlClipOp clip_op,
                                             bool is_aa) {
  if (clip_op == flutter::DlClipOp::kIntersect) {
    IntersectCullRect(rse.GetBounds());
    RecordScissor();
  }
}

void PrPictureBuilder::ClipPath(const flutter::DlPath& path,
                                flutter::DlClipOp clip_op,
                                bool is_aa) {
  // A path that is really one of the shapes with a clip of its own is
  // that clip: cheaper to record and cheaper to apply.
  Rect rect;
  if (path.IsRect(&rect)) {
    ClipRect(rect, clip_op, is_aa);
    return;
  }
  Rect oval;
  if (path.IsOval(&oval)) {
    ClipOval(oval, clip_op, is_aa);
    return;
  }
  RoundRect rrect;
  if (path.IsRoundRect(&rrect)) {
    ClipRoundRect(rrect, clip_op, is_aa);
    return;
  }

  // TODO: support difference clips.
  if (clip_op != flutter::DlClipOp::kIntersect) {
    return;
  }

  IntersectCullRect(path.GetBounds());

  FlushTransform();
  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  const auto index = static_cast<uint32_t>(picture->paths_.size());
  picture->paths_.push_back(path);
  AppendClip(
      Draw{
          .type = path.IsConvex() ? Draw::DrawType::kConvexClipPath
                                  : Draw::DrawType::kConcaveClipPath,
          .blend_mode = BlendMode::kSrcOver,
          .rect = path.GetBounds(),
          .color = flutter::DlColor(),        //
          .path_data = {.path_index = index}  //
      },
      path.GetFillType() == flutter::DlPathFillType::kOdd
          ? Draw::DrawType::kClipResolveEvenOdd
          : Draw::DrawType::kClipResolveNonZero,
      save_stack_.back().cull_rect);
}

Rect PrPictureBuilder::GetDestinationClipCoverage() const {
  return save_stack_.back().cull_rect;
}

Rect PrPictureBuilder::GetLocalClipCoverage() const {
  const CanvasStackEntry& state = save_stack_.back();
  if (state.cull_rect.IsMaximum()) {
    return Rect::MakeMaximum();
  }
  return state.cull_rect.TransformBounds(state.transform.Invert());
}

bool PrPictureBuilder::QuickReject(const Rect& bounds) const {
  return ComputeCoverage(bounds).IsEmpty();
}

void PrPictureBuilder::DrawPaint(const flutter::DlPaint& paint) {
  // An unbounded draw covers exactly what the clip admits.
  Rect coverage = ComputeCoverage(Rect::MakeMaximum());
  if (coverage.IsEmpty()) {
    return;
  }

  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kRect,
          .blend_mode = paint.getBlendMode(),
          .rect = GetLocalClipCoverage(),
          .color = ComputeDrawPaintColor(&paint),
          .gradient = GetGradientIndex(paint.getColorSource()),  //
      },
      coverage, GetLocalClipCoverage());
}

void PrPictureBuilder::DrawColor(flutter::DlColor color,
                                 flutter::DlBlendMode mode) {
  Rect coverage = ComputeCoverage(Rect::MakeMaximum());
  if (coverage.IsEmpty()) {
    return;
  }

  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kRect,
          .blend_mode = mode,
          .rect = GetLocalClipCoverage(),
          .color = color,  //
      },
      coverage, GetLocalClipCoverage());
}

void PrPictureBuilder::DrawLine(const Point& p0,
                                const Point& p1,
                                const flutter::DlPaint& paint) {
  const Scalar width = paint.getStrokeWidth();
  if (width <= 0) {
    // TODO(jonahwilliams): hairlines, which are a device pixel wide
    // whatever the scale.
    return;
  }
  const Point along = p1 - p0;
  const Scalar length = along.GetLength();
  if (length < 1e-6f) {
    // A line of no length, which has nothing but the caps this does not
    // draw.
    return;
  }

  // A line is its own band: the rectangle half a width either side of
  // it. End caps are ignored, so there is no outline to stroke and no
  // ring to fill -- the quad is convex and goes down as one draw.
  const Point normal = Point(along.y, -along.x) / length * (width * 0.5f);
  const Point corners[4] = {p0 + normal, p1 + normal, p1 - normal, p0 - normal};
  FillPath(flutter::DlPath::MakePoly(corners, 4, /*close=*/true), paint);
}

void PrPictureBuilder::DrawDashedLine(const Point& p0,
                                      const Point& p1,
                                      Scalar on_length,
                                      Scalar off_length,
                                      const flutter::DlPaint& paint) {
  // TODO: Generate geometry in logical coordinate space. insert as a point
  // mesh of rectangles as a variant of drawPoints.
}

namespace {

/// Whether the paint carries a blur this draws as a band. A shape with
/// one has to become a contour first: the band rings a silhouette, and
/// only a contour has one.
bool WantsBlurBand(const flutter::DlPaint& paint) {
  if (paint.getDrawStyle() != flutter::DlDrawStyle::kFill) {
    return false;
  }
  const flutter::DlMaskFilter* filter = paint.getMaskFilterPtr();
  if (filter == nullptr) {
    return false;
  }
  const flutter::DlBlurMaskFilter* blur = filter->asBlur();
  return blur != nullptr && blur->style() == flutter::DlBlurStyle::kNormal &&
         blur->sigma() > 0;
}

}  // namespace

void PrPictureBuilder::DrawRect(const Rect& rect,
                                const flutter::DlPaint& paint) {
  if (WantsBlurBand(paint)) {
    DrawPath(flutter::DlPath::MakeRect(rect), paint);
    return;
  }
  if (paint.getDrawStyle() != flutter::DlDrawStyle::kFill) {
    StrokePath(flutter::DlPath::MakeRect(rect), paint);
    return;
  }
  Rect coverage = ComputeCoverage(rect);
  if (coverage.IsEmpty()) {
    return;
  }

  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kRect,
          .blend_mode = paint.getBlendMode(),
          .rect = rect,
          .color = ComputeDrawPaintColor(&paint),               //
          .gradient = GetGradientIndex(paint.getColorSource())  //
      },
      coverage, rect);
}

void PrPictureBuilder::DrawOval(const Rect& bounds,
                                const flutter::DlPaint& paint) {
  if (WantsBlurBand(paint)) {
    DrawPath(flutter::DlPath::MakeOval(bounds), paint);
    return;
  }
  if (paint.getDrawStyle() != flutter::DlDrawStyle::kFill) {
    StrokePath(flutter::DlPath::MakeOval(bounds), paint);
    return;
  }
  Rect coverage = ComputeCoverage(bounds);
  if (coverage.IsEmpty()) {
    return;
  }
  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kRRect,
          .blend_mode = paint.getBlendMode(),
          .rect = bounds,
          .color = ComputeDrawPaintColor(&paint),
          .gradient = GetGradientIndex(paint.getColorSource()),
          .radii = RoundingRadii::MakeRadii(bounds.GetSize() / 2)  //
      },
      coverage, bounds);
}

void PrPictureBuilder::DrawCircle(const Point& center,
                                  Scalar radius,
                                  const flutter::DlPaint& paint) {
  if (WantsBlurBand(paint)) {
    DrawPath(flutter::DlPath::MakeCircle(center, radius), paint);
    return;
  }
  if (paint.getDrawStyle() != flutter::DlDrawStyle::kFill) {
    StrokePath(flutter::DlPath::MakeCircle(center, radius), paint);
    return;
  }
  Rect bounds = Rect::MakeCircleBounds(center, radius);
  Rect coverage = ComputeCoverage(bounds);
  if (coverage.IsEmpty()) {
    return;
  }
  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kRRect,
          .blend_mode = paint.getBlendMode(),
          .rect = bounds,
          .color = ComputeDrawPaintColor(&paint),
          .gradient = GetGradientIndex(paint.getColorSource()),
          .radii = RoundingRadii::MakeRadii(bounds.GetSize() / 2)  //
      },
      coverage, bounds);
}

void PrPictureBuilder::DrawRoundRect(const RoundRect& rrect,
                                     const flutter::DlPaint& paint) {
  if (WantsBlurBand(paint)) {
    DrawPath(flutter::DlPath::MakeRoundRect(rrect), paint);
    return;
  }
  if (paint.getDrawStyle() != flutter::DlDrawStyle::kFill) {
    StrokePath(flutter::DlPath::MakeRoundRect(rrect), paint);
    return;
  }
  if (rrect.IsRect()) {
    // Nothing rounds it, and the round rect mesh is two orders of
    // magnitude of geometry to say so.
    DrawRect(rrect.GetBounds(), paint);
    return;
  }
  Rect coverage = ComputeCoverage(rrect.GetBounds());
  if (coverage.IsEmpty()) {
    return;
  }
  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kRRect,
          .blend_mode = paint.getBlendMode(),
          .rect = rrect.GetBounds(),
          .color = ComputeDrawPaintColor(&paint),
          .gradient = GetGradientIndex(paint.getColorSource()),
          .radii = rrect.GetRadii()  //
      },
      coverage, rrect.GetBounds());
}

void PrPictureBuilder::DrawDiffRoundRect(const RoundRect& outer,
                                         const RoundRect& inner,
                                         const flutter::DlPaint& paint) {
  if (outer.IsEmpty()) {
    return;
  }
  if (inner.IsEmpty()) {
    // Nothing is taken out of it, so it is the outer shape and can have
    // the mesh that shape already has.
    DrawRoundRect(outer, paint);
    return;
  }
  // Both contours wound the same way and resolved even-odd, so the two
  // crossings over the inner shape cancel and leave the ring. Winding
  // them against each other would do as well, but which way a contour
  // turns is not something the builder exposes, and the fill rule is.
  DrawPath(flutter::DlPathBuilder{}
               .SetFillType(flutter::DlPathFillType::kOdd)
               .AddRoundRect(outer)
               .AddRoundRect(inner)
               .TakePath(),
           paint);
}

void PrPictureBuilder::DrawRoundSuperellipse(const RoundSuperellipse& rse,
                                             const flutter::DlPaint& paint) {}

void PrPictureBuilder::DrawPath(const flutter::DlPath& path,
                                const flutter::DlPaint& paint) {
  if (paint.getDrawStyle() != flutter::DlDrawStyle::kFill) {
    StrokePath(path, paint);
    return;
  }
  FillPath(path, paint);
}

std::optional<Rect> PrPictureBuilder::FlattenSilhouette(
    const flutter::DlPath& path) {
  shadow_contour_.edges.clear();
  if (const auto* edges = path.GetEdges()) {
    shadow_contour_.edges.assign(edges->begin(), edges->end());
  }
  FlattenContourInto(shadow_contour_, shadow_silhouette_);
  if (shadow_silhouette_.size() < 3) {
    return std::nullopt;
  }
  return Rect::MakePointBounds(shadow_silhouette_.begin(),
                               shadow_silhouette_.end());
}

bool PrPictureBuilder::FillBlurredPath(const flutter::DlPath& path,
                                       const flutter::DlPaint& paint) {
  const flutter::DlMaskFilter* filter = paint.getMaskFilterPtr();
  if (filter == nullptr) {
    return false;
  }
  const flutter::DlBlurMaskFilter* blur = filter->asBlur();
  // TODO: the solid, outer and inner styles, each of which ramps across
  // the band differently, and concave shapes, which have no one
  // silhouette to ring.
  if (blur == nullptr || blur->style() != flutter::DlBlurStyle::kNormal ||
      !path.IsConvex()) {
    return false;
  }
  const Scalar sigma = blur->sigma();
  if (!(sigma > 0)) {
    return false;
  }

  const std::optional<Rect> silhouette = FlattenSilhouette(path);
  if (!silhouette.has_value()) {
    return false;
  }

  // A sigma given in the device's space covers less of the shape's own
  // space the more the transform magnifies it.
  const Scalar scale = save_stack_.back().transform.GetMaxBasisLengthXY();
  const Scalar local_sigma =
      blur->respectCTM() || scale <= 0 ? sigma : sigma / scale;

  // The band reaches two sigma out, and half again past that is where
  // the Gaussian has nothing left to show.
  const Rect bounds =
      silhouette->Expand(2.0f * local_sigma * kShadowCoverageOfBlur);
  const Rect coverage = ComputeCoverage(bounds);
  if (coverage.IsEmpty()) {
    return true;  // Handled: there is simply nothing of it to see.
  }

  FlushTransform();
  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  const auto offset = static_cast<uint32_t>(picture->positions_.size());
  picture->positions_.insert(picture->positions_.end(),
                             shadow_silhouette_.begin(),
                             shadow_silhouette_.end());
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kBlurredFillPath,
          .blend_mode = paint.getBlendMode(),
          .rect = bounds,
          .color = ComputeDrawPaintColor(&paint),
          .gradient = GetGradientIndex(paint.getColorSource()),
          .blur_data =
              Draw::BlurData{
                  .offset = offset,
                  .length = static_cast<uint32_t>(shadow_silhouette_.size()),
                  .sigma = sigma,
                  .respect_ctm = blur->respectCTM(),  //
              },
      },
      coverage, bounds);
  return true;
}

void PrPictureBuilder::FillPath(const flutter::DlPath& path,
                                const flutter::DlPaint& paint) {
  if (FillBlurredPath(path, paint)) {
    return;
  }
  Rect coverage = ComputeCoverage(path.GetBounds());
  if (coverage.IsEmpty()) {
    return;
  }

  FlushTransform();
  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  const auto index = static_cast<uint32_t>(picture->paths_.size());
  picture->paths_.push_back(path);

  if (path.IsConvex()) {
    AppendDraw(
        Draw{
            .type = Draw::DrawType::kConvexFillPath,
            .blend_mode = paint.getBlendMode(),
            .rect = path.GetBounds(),
            .color = ComputeDrawPaintColor(&paint),
            .gradient = GetGradientIndex(paint.getColorSource()),
            .path_data = {.path_index = index}  //
        },
        coverage, path.GetBounds());
    return;
  }

  constexpr Scalar kConcaveResolveOutset = 2.0f;

  // Concave paths render as two draws, the first into a winding accumulator
  // and the second into the color attachment.
  const Rect bounds = path.GetBounds().Expand(
      kConcaveResolveOutset /
      save_stack_.back().transform.GetMaxBasisLengthXY());
  coverage = ComputeCoverage(bounds);
  if (coverage.IsEmpty()) {
    return;
  }

  AppendDraw(
      Draw{
          .type = Draw::DrawType::kConcaveWindingAccumulate,
          .blend_mode = paint.getBlendMode(),
          .rect = bounds,
          .color = paint.getColor(),
          .path_data = {.path_index = index}  //
      },
      coverage, bounds);
  AppendDraw(
      Draw{
          .type = path.GetFillType() == flutter::DlPathFillType::kOdd
                      ? Draw::DrawType::kConcaveWindingResolveEvenOdd
                      : Draw::DrawType::kConcaveWindingResolveNonZero,
          .blend_mode = paint.getBlendMode(),
          .rect = bounds,
          .color = ComputeDrawPaintColor(&paint),
          .gradient = GetGradientIndex(paint.getColorSource()),
          .path_data = {.path_index = index}  //
      },
      coverage, bounds);
}

void PrPictureBuilder::StrokePath(const flutter::DlPath& path,
                                  const flutter::DlPaint& paint) {
  if (paint.getStrokeWidth() <= 0) {
    // TODO(jonahwilliams): hairlines, which are a stroke of no width at
    // all and so have no outline to fill.
    return;
  }

  // Convert strokes into an equivalent filled path, which the path
  // itself remembers: the same stroke of the same path every frame is
  // the common case, and the outline is the expensive part.
  const flutter::DlPath::StrokeKey key{
      .width = paint.getStrokeWidth(),
      .miter_limit = paint.getStrokeMiter(),
      .cap = static_cast<uint8_t>(paint.getStrokeCap()),
      .join = static_cast<uint8_t>(paint.getStrokeJoin()),
  };
  std::shared_ptr<flutter::DlPath> outline = path.GetStroked(key);
  if (outline == nullptr) {
    SkPaint stroker;
    stroker.setStyle(SkPaint::kStroke_Style);
    stroker.setStrokeWidth(key.width);
    stroker.setStrokeCap(flutter::ToSk(paint.getStrokeCap()));
    stroker.setStrokeJoin(flutter::ToSk(paint.getStrokeJoin()));
    stroker.setStrokeMiter(key.miter_limit);
    outline = std::make_shared<flutter::DlPath>(
        skpathutils::FillPathWithPaint(path.GetSkPath(), stroker));
    path.SetStroked(key, outline);
  }
  FillPath(*outline, paint);
}

void PrPictureBuilder::DrawArc(const Rect& bounds,
                               Scalar start,
                               Scalar sweep,
                               bool use_center,
                               const flutter::DlPaint& paint) {
  // The angles arrive in degrees: dart:ui converts from radians before
  // the op is recorded.
  //
  // Materialized rather than given a mesh of its own. What an arc turns
  // into depends on all of use_center, the sweep and the fill: a wedge
  // for a pie, a segment cut by its chord without one, either of which
  // is concave past half a turn, and an open contour that takes caps
  // when it is stroked. The path already carries all of that.
  DrawPath(flutter::DlPath::MakeArc(bounds, Degrees(start), Degrees(sweep),
                                    use_center),
           paint);
}

void PrPictureBuilder::DrawPoints(flutter::DlPointMode mode,
                                  uint32_t count,
                                  const Point pts[],
                                  const flutter::DlPaint& paint) {
  if (count == 0) {
    return;
  }

  if (mode == flutter::DlPointMode::kPoints) {
    Scalar radius = paint.getStrokeWidth() / 2;
    const Rect point_bounds =
        Rect::MakePointBounds(pts, pts + count).value_or(Rect()).Expand(radius);
    Rect coverage = ComputeCoverage(point_bounds);
    if (coverage.IsEmpty()) {
      return;
    }

    FlushTransform();
    const std::shared_ptr<PrPicture>& picture = pictures_.back();
    Draw::PointData data;
    data.offset = picture->positions_.size();
    data.length = count;
    data.round = paint.getStrokeCap() == flutter::DlStrokeCap::kRound;
    data.radius = radius;

    picture->positions_.insert(picture->positions_.end(), pts, pts + count);

    AppendDraw(
        Draw{
            .type = Draw::DrawType::kPoints,
            .blend_mode = paint.getBlendMode(),
            .color = ComputeDrawPaintColor(&paint),
            .gradient = GetGradientIndex(paint.getColorSource()),
            .point_data = data,
        },
        coverage, point_bounds);
    return;
  }
}

void PrPictureBuilder::DrawVertices(
    const std::shared_ptr<flutter::DlVertices>& vertices,
    flutter::DlBlendMode mode,
    const flutter::DlPaint& paint) {
  // TODO, accurate blend modes and triangle mode.
  if (!vertices) {
    return;
  }
  Rect coverage = ComputeCoverage(vertices->GetBounds());
  if (coverage.IsEmpty()) {
    return;
  }

  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  const auto index = static_cast<uint32_t>(picture->vertices_.size());
  picture->vertices_.push_back(vertices);

  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kDrawVertices,
          .blend_mode = paint.getBlendMode(),
          .rect = vertices->GetBounds(),
          .color = paint.getColor(),
          .vertices_data = Draw::VerticesData{.vertices_index = index},
      },
      coverage, vertices->GetBounds());
}

void PrPictureBuilder::DrawImage(const sk_sp<flutter::DlImage>& image,
                                 const Point& point,
                                 flutter::DlImageSampling sampling,
                                 const flutter::DlPaint* paint) {
  if (!image) {
    return;
  }
  Rect src = Rect::Make(image->GetBounds());
  DrawImageRect(
      image, Rect::Make(image->GetBounds()),
      Rect::MakeXYWH(point.x, point.y, src.GetWidth(), src.GetHeight()),
      sampling, paint);
}

flutter::DlColor PrPictureBuilder::ComputeImagePaintColor(
    const flutter::DlPaint* paint) {
  if (paint == nullptr) {
    return flutter::DlColor::kWhite();
  }
  if (auto* filter = paint->getColorFilterPtr();
      filter != nullptr && filter->asBlend()) {
    // This isn't quite correct...
    return filter->asBlend()->color().modulateOpacity(paint->getOpacity());
  }

  return flutter::DlColor::kWhite().withAlpha(paint->getAlpha());
}

flutter::DlColor PrPictureBuilder::ComputeDrawPaintColor(
    const flutter::DlPaint* paint) {
  if (paint == nullptr) {
    return flutter::DlColor::kWhite();
  }
  if (auto* gradient = paint->getColorSourcePtr();
      gradient && gradient->isGradient()) {
    return flutter::DlColor::kWhite().modulateOpacity(paint->getOpacity());
  }
  return paint->getColor();
}

void PrPictureBuilder::DrawImageRect(const sk_sp<flutter::DlImage>& image,
                                     const Rect& src,
                                     const Rect& dst,
                                     flutter::DlImageSampling sampling,
                                     const flutter::DlPaint* paint,
                                     flutter::DlSrcRectConstraint constraint) {
  if (!image) {
    return;
  }
  Rect coverage = ComputeCoverage(dst);
  if (coverage.IsEmpty()) {
    return;
  }

  BlendMode mode =
      paint ? paint->getBlendMode() : flutter::DlBlendMode::kSrcOver;
  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kImageRect,
          .blend_mode = mode,
          .rect = dst,
          .color = ComputeImagePaintColor(paint),
          .uv_data = MakeUVData(src, Rect::Make(image->GetBounds()), sampling,
                                AddImage(image))  //
      },
      coverage, dst);
}

void PrPictureBuilder::DrawImageNine(const sk_sp<flutter::DlImage>& image,
                                     const IRect32& center,
                                     const Rect& dst,
                                     flutter::DlFilterMode filter,
                                     const flutter::DlPaint* paint) {
  if (!image || dst.IsEmpty()) {
    return;
  }
  Rect image_bounds = Rect::Make(image->GetBounds());
  Scalar width = image_bounds.GetWidth();
  Scalar height = image_bounds.GetHeight();

  const Scalar sx[4] = {0, static_cast<Scalar>(center.GetLeft()),
                        static_cast<Scalar>(center.GetRight()), width};
  const Scalar sy[4] = {0, static_cast<Scalar>(center.GetTop()),
                        static_cast<Scalar>(center.GetBottom()), height};

  const Scalar left = sx[1];
  const Scalar right = width - sx[2];
  const Scalar top = sy[1];
  const Scalar bottom = height - sy[2];
  const Scalar fx = left + right > 0
                        ? std::min<Scalar>(1, dst.GetWidth() / (left + right))
                        : 1;
  const Scalar fy = top + bottom > 0
                        ? std::min<Scalar>(1, dst.GetHeight() / (top + bottom))
                        : 1;
  const Scalar dx[4] = {dst.GetLeft(), dst.GetLeft() + left * fx,
                        dst.GetRight() - right * fx, dst.GetRight()};
  const Scalar dy[4] = {dst.GetTop(), dst.GetTop() + top * fy,
                        dst.GetBottom() - bottom * fy, dst.GetBottom()};

  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      if (sx[col + 1] <= sx[col] || sy[row + 1] <= sy[row] ||
          dx[col + 1] <= dx[col] || dy[row + 1] <= dy[row]) {
        continue;
      }
      DrawImageRect(image,
                    Rect::MakeLTRB(sx[col], sy[row], sx[col + 1], sy[row + 1]),
                    Rect::MakeLTRB(dx[col], dy[row], dx[col + 1], dy[row + 1]),
                    kFilterToSampling[static_cast<int>(filter)], paint);
    }
  }
}

void PrPictureBuilder::DrawAtlas(const sk_sp<flutter::DlImage>& atlas,
                                 const RSTransform xform[],
                                 const Rect tex[],
                                 const flutter::DlColor colors[],
                                 int count,
                                 flutter::DlBlendMode mode,
                                 flutter::DlImageSampling sampling,
                                 const Rect* cull_rect,
                                 const flutter::DlPaint* paint) {
  // TODO: the blend between a sprite's colour and the image, which is
  // taken to be a modulate here.
  if (!atlas || count <= 0) {
    return;
  }

  PrAtlas data;
  data.image = atlas;
  data.sampling = sampling;
  data.transforms.assign(xform, xform + count);
  data.textures.assign(tex, tex + count);
  if (colors != nullptr) {
    data.colors.assign(colors, colors + count);
  }

  Scalar left = std::numeric_limits<Scalar>::infinity();
  Scalar top = left;
  Scalar right = -left;
  Scalar bottom = -left;
  Quad quad;
  for (int i = 0; i < count; i++) {
    xform[i].GetQuad(tex[i].GetWidth(), tex[i].GetHeight(), quad);
    for (const Point& corner : quad) {
      left = std::min(left, corner.x);
      top = std::min(top, corner.y);
      right = std::max(right, corner.x);
      bottom = std::max(bottom, corner.y);
    }
  }
  if (!(left <= right && top <= bottom)) {
    return;  // Nothing finite came out of it.
  }
  const Rect bounds = Rect::MakeLTRB(left, top, right, bottom);

  Rect coverage = ComputeCoverage(bounds);
  if (coverage.IsEmpty()) {
    return;
  }

  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  const auto index = static_cast<uint32_t>(picture->atlases_.size());
  picture->atlases_.push_back(std::move(data));

  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kDrawAtlas,
          .blend_mode = paint ? paint->getBlendMode() : BlendMode::kSrcOver,
          .rect = bounds,
          .color = ComputeImagePaintColor(paint),
          .atlas_data = Draw::AtlasData{.atlas_index = index},
      },
      coverage, bounds);
}

void PrPictureBuilder::DrawDisplayList(
    const sk_sp<flutter::DisplayList> display_list,
    Scalar opacity) {}

void PrPictureBuilder::DrawPicture(const std::shared_ptr<PrPicture>& picture,
                                   Scalar opacity) {
  if (!picture || !picture->bounds_union_.has_value()) {
    return;
  }
  // The picture recorded its own coordinates, so what it covers here is
  // those bounds through the transform it is drawn under.
  Rect coverage = ComputeCoverage(picture->bounds_union_.value());
  if (coverage.IsEmpty()) {
    return;
  }

  if (opacity >= 1.0f) {
    AppendPicture(*picture);
    return;
  }

  const std::shared_ptr<PrPicture>& parent = pictures_.back();
  uint32_t picture_index = parent->pictures_.size();
  parent->pictures_.push_back(picture);

  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kLayer,
          .blend_mode = BlendMode::kSrcOver,
          .rect = picture->bounds_union_.value(),
          .color = flutter::DlColor::kWhite().withAlphaF(opacity),
          .layer =
              Draw::Layer{
                  .picture_index = picture_index,
                  .image_filter_index = Draw::kNoIndex,
                  .color_filter_index = Draw::kNoIndex,  //
              },
      },
      coverage, picture->bounds_union_.value());
}

void PrPictureBuilder::DrawOpaquePicture(const std::shared_ptr<void>& picture,
                                         flutter::DlScalar opacity) {
  DrawPicture(std::static_pointer_cast<PrPicture>(picture), opacity);
}

void PrPictureBuilder::DrawText(const std::shared_ptr<flutter::DlText>& text,
                                Scalar x,
                                Scalar y,
                                const flutter::DlPaint& paint) {
  const std::shared_ptr<TextFrame>& frame = text->GetTextFrame();
  if (!frame) {
    return;
  }
  Rect coverage = ComputeCoverage(text->GetBounds().Shift(x, y));
  if (coverage.IsEmpty()) {
    return;
  }

  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  uint32_t index = picture->text_frames_.size();
  picture->text_frames_.push_back(frame);

  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kText,
          .blend_mode = paint.getBlendMode(),
          .color = ComputeDrawPaintColor(&paint),
          .gradient = GetGradientIndex(paint.getColorSource()),
          .text_data =
              Draw::TextData{
                  .text_index = index, .position = Point(x, y)  //
              },
      },
      coverage, text->GetBounds().Shift(x, y));
}

Rect PrPictureBuilder::ComputeReach(const Rect& local_bounds) const {
  if (local_bounds.IsMaximum()) {
    return Rect::MakeMaximum();
  }
  return local_bounds.TransformBounds(save_stack_.back().transform);
}

Rect PrPictureBuilder::ComputeCoverage(const Rect& local_bounds) const {
  const CanvasStackEntry& state = save_stack_.back();
  const Rect& cull = state.cull_rect;
  if (local_bounds.IsMaximum()) {
    return cull;
  }
  Rect root = local_bounds.TransformBounds(state.transform);
  if (cull.IsMaximum()) {
    return root;
  }
  return root.IntersectionOrEmpty(cull);
}

void PrPictureBuilder::IntersectCullRect(const Rect& local_bounds) {
  Rect& cull = save_stack_.back().cull_rect;
  Rect root = local_bounds.TransformBounds(save_stack_.back().transform);
  cull = cull.IsMaximum() ? root : cull.IntersectionOrEmpty(root);
}

void PrPictureBuilder::DrawShadow(const flutter::DlPath& path,
                                  const flutter::DlColor color,
                                  const Scalar elevation,
                                  bool transparent_occluder,
                                  Scalar dpr) {
  // TODO: concave occluders, which MLR approximates by their bounds.
  if (elevation <= 0 || !path.IsConvex()) {
    return;
  }

  const std::optional<Rect> silhouette = FlattenSilhouette(path);
  if (!silhouette.has_value()) {
    return;
  }

  const Rect bounds =
      silhouette->Expand(AmbientShadowBlur(elevation) * kShadowCoverageOfBlur)
          .Union(
              silhouette->Shift(0, elevation)
                  .Expand(SpotShadowBlur(elevation) * kShadowCoverageOfBlur));

  Rect coverage = ComputeCoverage(bounds);
  if (coverage.IsEmpty()) {
    return;
  }

  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  const auto offset = static_cast<uint32_t>(picture->positions_.size());
  picture->positions_.insert(picture->positions_.end(),
                             shadow_silhouette_.begin(),
                             shadow_silhouette_.end());

  FlushTransform();
  AppendDraw(
      Draw{
          .type = Draw::DrawType::kShadow,
          .blend_mode = BlendMode::kSrcOver,
          .rect = bounds,
          .color = color,
          .shadow_data =
              Draw::ShadowData{
                  .offset = offset,
                  .length = static_cast<uint32_t>(shadow_silhouette_.size()),
                  .elevation = elevation,
                  .transparent_occluder = transparent_occluder,  //
              },
      },
      coverage, bounds);
}

uint32_t PrPictureBuilder::AddImage(const sk_sp<flutter::DlImage>& image) {
  return AddUnique(pictures_.back()->image_sources_, image);
}

uint32_t PrPictureBuilder::AddImageFilter(
    const std::shared_ptr<flutter::DlImageFilter>& filter) {
  return AddUnique(pictures_.back()->image_filters_, filter);
}

uint32_t PrPictureBuilder::AddColorFilter(
    const std::shared_ptr<const flutter::DlColorFilter>& filter) {
  return AddUnique(pictures_.back()->color_filters_, filter);
}

void PrPictureBuilder::RecordClip(const Draw& draw, Rect coverage) {
  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  picture->draws_.push_back(draw);
  picture->bounds_.push_back(coverage);
  // Note: clips do not actually contribute to the bounds union.
}

void PrPictureBuilder::RecordDraw(const Draw& draw, Rect coverage) {
  const std::shared_ptr<PrPicture>& picture = pictures_.back();

  auto append_draw = [](const std::shared_ptr<PrPicture>& picture,
                        const Draw& draw, const Rect& coverage) {
    picture->draws_.push_back(draw);
    picture->bounds_.push_back(coverage);
    if (picture->bounds_union_.has_value()) {
      picture->bounds_union_ = coverage.Union(picture->bounds_union_.value());
    } else {
      picture->bounds_union_ = coverage;
    }
  };

  if (IsStateful(draw.type)) {
    append_draw(picture, draw, coverage);
    return;
  }

  if (!picture->draws_.empty() &&
      GetMergeType(picture->draws_.back().type) != GetMergeType(draw.type)) {
    constexpr size_t kMaxLookback = 8;
    size_t count = 0;
    size_t swap_index = picture->draws_.size();

    for (size_t i = picture->draws_.size(); i > 0 && count < kMaxLookback;
         --i, ++count) {
      size_t idx = i - 1;
      const Draw& prev_draw = picture->draws_[idx];

      if (IsStateful(prev_draw.type)) {
        break;
      }

      if (GetMergeType(prev_draw.type) == GetMergeType(draw.type)) {
        swap_index = idx + 1;
        break;
      }
      if (coverage.Intersection(picture->bounds_[idx]).has_value() &&
          (GetDrawLayerMask(draw.type) & GetDrawLayerMask(prev_draw.type)) !=
              0) {
        break;
      }
    }

    if (swap_index < picture->draws_.size()) {
      picture->draws_.insert(picture->draws_.begin() + swap_index, draw);
      picture->bounds_.insert(picture->bounds_.begin() + swap_index, coverage);
      if (picture->bounds_union_.has_value()) {
        picture->bounds_union_ = picture->bounds_union_->Union(coverage);
      } else {
        picture->bounds_union_ = coverage;
      }
      return;
    }
  }

  append_draw(picture, draw, coverage);
}

void PrPictureBuilder::RecordScissor() {
  RecordScissor(GetLocalClipCoverage());
}

void PrPictureBuilder::RecordScissor(Rect local_bounds) {
  FlushTransform();
  RecordScissor(local_bounds, pictures_.back()->GetCurrentTransform());
}

void PrPictureBuilder::RecordScissor(Rect local_bounds, uint32_t transform) {
  Draw scissor{
      .type = Draw::DrawType::kScissor,
      .blend_mode = BlendMode::kSrcOver,
      .rect = local_bounds,
      .color = flutter::DlColor(),  //
  };
  scissor.transform = transform;
  // Maximum coverage: a scissor is state, and skipping it because the
  // pass missed its rect would leave the last one in force.
  RecordClip(scissor, Rect::MakeMaximum());
}

void PrPictureBuilder::AppendClip(Draw shape,
                                  Draw::DrawType resolve,
                                  Rect coverage,
                                  Rect interior) {
  shape.transform = pictures_.back()->GetCurrentTransform();
  shape.color = flutter::DlColor::kWhite();
  ClipEntry entry{
      .shape = shape,
      .resolve =
          Draw{
              .type = resolve,
              .blend_mode = BlendMode::kSrcOver,
              .rect = shape.rect,
              .color = flutter::DlColor(),  //
          },
      .coverage = coverage,
      .scissor_bounds = GetLocalClipCoverage(),
      // Only where the shape stays square to the axes: a turned rect's
      // bounds reach outside it, and a rect that reaches outside the
      // clip is no use for saying what the clip cannot cut.
      .interior =
          interior.IsEmpty() || !save_stack_.back().transform.IsAligned2D()
              ? Rect()
              : interior.TransformBounds(save_stack_.back().transform),
  };
  entry.resolve.transform = shape.transform;
  clip_stack_.push_back(entry);
}

void PrPictureBuilder::MaterializePendingClips(const Rect& reach) {
  bool reaches_past = false;
  for (const ClipEntry& clip : clip_stack_) {
    if (!clip.live && !clip.interior.Contains(reach)) {
      reaches_past = true;
      break;
    }
  }
  if (!reaches_past) {
    return;
  }
  for (ClipEntry& clip : clip_stack_) {
    if (clip.live) {
      continue;
    }
    RecordScissor(clip.scissor_bounds, clip.shape.transform);
    RecordClip(clip.shape, clip.coverage);
    RecordClip(clip.resolve, clip.coverage);
    clip.live = true;
  }
}

void PrPictureBuilder::AppendDraw(Draw draw,
                                  Rect coverage,
                                  const Rect& local_bounds) {
  MaterializePendingClips(ComputeReach(local_bounds));
  draw.transform = pictures_.back()->GetCurrentTransform();
  RecordDraw(draw, coverage);
}

uint32_t PrPictureBuilder::GetGradientIndex(
    const std::shared_ptr<const flutter::DlColorSource>& source) {
  if (!source || !source->isGradient()) {
    return Draw::kNoIndex;
  }
  const std::shared_ptr<PrPicture>& current = pictures_.back();
  if (current->color_sources_.empty() ||
      current->color_sources_.back() != source) {
    current->color_sources_.push_back(source);
  }
  return static_cast<uint32_t>(current->color_sources_.size() - 1);
}

void PrPictureBuilder::AppendPicture(const PrPicture& source) {
  const std::shared_ptr<PrPicture>& target = pictures_.back();
  const Matrix& placement = save_stack_.back().transform;
  const Rect& cull = save_stack_.back().cull_rect;

  // Each stream is appended whole, so rebasing an index is adding where
  // the source's copy of that stream begins.
  const auto transform_base = static_cast<uint32_t>(target->transforms_.size());
  for (const Matrix& transform : source.transforms_) {
    target->transforms_.push_back(placement * transform);
  }
  const auto position_base = static_cast<uint32_t>(target->positions_.size());
  target->positions_.insert(target->positions_.end(), source.positions_.begin(),
                            source.positions_.end());
  const auto atlas_base = static_cast<uint32_t>(target->atlases_.size());
  target->atlases_.insert(target->atlases_.end(), source.atlases_.begin(),
                          source.atlases_.end());
  const auto vertices_base = static_cast<uint32_t>(target->vertices_.size());
  target->vertices_.insert(target->vertices_.end(), source.vertices_.begin(),
                           source.vertices_.end());
  const auto text_base = static_cast<uint32_t>(target->text_frames_.size());
  target->text_frames_.insert(target->text_frames_.end(),
                              source.text_frames_.begin(),
                              source.text_frames_.end());
  const auto path_base = static_cast<uint32_t>(target->paths_.size());
  target->paths_.insert(target->paths_.end(), source.paths_.begin(),
                        source.paths_.end());
  const auto image_base = static_cast<uint32_t>(target->image_sources_.size());
  target->image_sources_.insert(target->image_sources_.end(),
                                source.image_sources_.begin(),
                                source.image_sources_.end());
  const auto layer_base = static_cast<uint32_t>(target->pictures_.size());
  target->pictures_.insert(target->pictures_.end(), source.pictures_.begin(),
                           source.pictures_.end());
  const auto image_filter_base =
      static_cast<uint32_t>(target->image_filters_.size());
  target->image_filters_.insert(target->image_filters_.end(),
                                source.image_filters_.begin(),
                                source.image_filters_.end());
  const auto color_filter_base =
      static_cast<uint32_t>(target->color_filters_.size());
  target->color_filters_.insert(target->color_filters_.end(),
                                source.color_filters_.begin(),
                                source.color_filters_.end());
  const auto gradient_base =
      static_cast<uint32_t>(target->color_sources_.size());
  target->color_sources_.insert(target->color_sources_.end(),
                                source.color_sources_.begin(),
                                source.color_sources_.end());

  auto rebase = [](uint32_t index, uint32_t base) {
    return index == Draw::kNoIndex ? Draw::kNoIndex : index + base;
  };

  // Its draws are recorded straight rather than through AppendDraw, so
  // whatever clips are standing are settled here instead.
  MaterializePendingClips(Rect::MakeMaximum());

  bool scissored = false;
  for (size_t i = 0; i < source.draws_.size(); i++) {
    Draw draw = source.draws_[i];
    draw.transform += transform_base;
    // Every draw type that can carry one, so it is rebased here rather
    // than in each of their cases.
    draw.gradient = rebase(draw.gradient, gradient_base);
    const Rect coverage =
        source.bounds_[i].TransformBounds(placement).IntersectionOrEmpty(cull);

    switch (draw.type) {
      case Draw::DrawType::kConvexClipPath:
      case Draw::DrawType::kConcaveClipPath:
        draw.path_data.path_index += path_base;
        [[fallthrough]];
      case Draw::DrawType::kRectClip:
      case Draw::DrawType::kRRectClip:
      case Draw::DrawType::kClipResolveNonZero:
      case Draw::DrawType::kClipResolveEvenOdd:
        // Always kept: the draws after it are recorded against it.
        RecordClip(draw, coverage);
        continue;
      case Draw::DrawType::kScissor:
        // The same, and never narrowed: a scissor is state, and one the
        // pass skipped would leave the last in force.
        RecordClip(draw, Rect::MakeMaximum());
        scissored = true;
        continue;
      case Draw::DrawType::kPoints:
        draw.point_data.offset += position_base;
        break;
      case Draw::DrawType::kShadow:
        draw.shadow_data.offset += position_base;
        break;
      case Draw::DrawType::kBlurredFillPath:
        draw.blur_data.offset += position_base;
        break;
      case Draw::DrawType::kText:
        draw.text_data.text_index += text_base;
        break;
      case Draw::DrawType::kDrawVertices:
        draw.vertices_data.vertices_index += vertices_base;
        break;
      case Draw::DrawType::kDrawAtlas:
        draw.atlas_data.atlas_index += atlas_base;
        break;
      case Draw::DrawType::kImageRect:
        draw.uv_data.image_index += image_base;
        break;
      case Draw::DrawType::kLayer:
        draw.layer.picture_index += layer_base;
        draw.layer.image_filter_index =
            rebase(draw.layer.image_filter_index, image_filter_base);
        draw.layer.color_filter_index =
            rebase(draw.layer.color_filter_index, color_filter_base);
        break;
      case Draw::DrawType::kConvexFillPath:
      case Draw::DrawType::kConcaveWindingAccumulate:
      case Draw::DrawType::kConcaveWindingResolveNonZero:
      case Draw::DrawType::kConcaveWindingResolveEvenOdd:
        draw.path_data.path_index += path_base;
        break;
      case Draw::DrawType::kRect:
      case Draw::DrawType::kRRect:
      case Draw::DrawType::kClipReset:
        break;
    }
    if (coverage.IsEmpty()) {
      continue;  // Nothing of it is visible here.
    }
    RecordDraw(draw, coverage);
  }

  // The transform table no longer ends on this recording's own current
  // transform, so the next draw has to put it back.
  transform_dirty_ = true;

  // A picture that clipped left the scissor wherever its own clips did,
  // so this recording's outer rect goes back.
  if (scissored) {
    RecordScissor();
  }
}

void PrPictureBuilder::FlushTransform() {
  if (!transform_dirty_) {
    return;
  }
  transform_dirty_ = false;
  const std::shared_ptr<PrPicture>& picture = pictures_.back();
  picture->transforms_.push_back(save_stack_.back().transform);
}

std::shared_ptr<PrPicture> PrPictureBuilder::Build() {
  // A picture ends with its coverage back to visible: a pass holds more
  // than one of them, and a clip still standing would leak onto
  // whatever is drawn next.
  FML_DCHECK(pictures_.size() >= 1);
  while (pictures_.size() > 1) {
    Restore();
  }
  // Nothing encloses the recording, so what its clips come back to is
  // everything. A clip taken without a save of its own leaves no record
  // of the rect it narrowed, so the pop has nothing else to read.
  save_stack_.back().cull_rect = Rect::MakeMaximum();
  PopClips(0);
  return BuildStandingClips();
}

std::shared_ptr<PrPicture> PrPictureBuilder::BuildStandingClips() {
  FML_DCHECK(pictures_.size() >= 1);
  while (pictures_.size() > 1) {
    Restore();
  }
  std::shared_ptr<PrPicture> result = std::move(pictures_.back());

  Reset();

  return result;
}

}  // namespace impeller
