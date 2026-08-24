// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/geometry.h"

#include "flutter/fml/logging.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "flutter/display_list/dl_vertices.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/constants.h"
#include "impeller/propeller/renderer/shadow_lut.h"
#include "impeller/typographer/text_frame.h"
#include "impeller/typographer/text_run.h"

namespace impeller {

namespace {

/// Two triangles over corners written left-top, left-bottom, right-top,
/// right-bottom. Both wind the same way: the winding accumulator signs
/// coverage by facing, so a quad whose halves disagree cancels along
/// the diagonal they share.
constexpr int kQuadCornerOrder[6] = {0, 1, 2, 1, 3, 2};
constexpr Point kQuadCorners[4] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};

/// Where `texture` sits in the pass's table, appending it if this is
/// the first draw to name it: an image drawn twice in a pass is one
/// slot, not two.
int32_t SlotFor(std::vector<GPUTexture*>& textures, GPUTexture* texture) {
  for (size_t slot = 0; slot < textures.size(); slot++) {
    if (textures[slot] == texture) {
      return static_cast<int32_t>(slot);
    }
  }
  textures.push_back(texture);
  return static_cast<int32_t>(textures.size() - 1);
}

void WriteQuadIndices(uint16_t* index_out, uint32_t first_vertex) {
  for (int i = 0; i < 6; i++) {
    index_out[i] = static_cast<uint16_t>(first_vertex + kQuadCornerOrder[i]);
  }
}

}  // namespace

ConvexMeshEmitter::ConvexMeshEmitter(Point centroid,
                                     Scalar scale,
                                     uint32_t color,
                                     GeometryStaging& staging)
    : centroid_(centroid),
      fringe_(1.0f / scale),
      color_(color),
      staging_(staging) {
  staging_.Clear();
}

void ConvexMeshEmitter::PushVertex(Point position, uint8_t implicit_class) {
  staging_.positions.push_back(position);
  staging_.attributes.push_back(Attributes{
      .uv = Point(0, 0),
      .color = color_,
      .paint = PackPaint(0, implicit_class),
  });
}

Point ConvexMeshEmitter::OutwardNormal(Point at, Point along) const {
  const Scalar length = along.GetLength();
  if (length < 1e-6f) {
    return Point();
  }
  Point normal = Point(along.y, -along.x) / length;
  if (normal.Dot(at - centroid_) < 0) {
    normal = -normal;
  }
  return normal;
}

void ConvexMeshEmitter::TangentStrip(Point at, Point toward) {
  const Scalar tangent_length = toward.GetDistance(at);
  if (tangent_length < 1e-3f) {
    return;
  }
  // A couple of device pixels, not a fraction of the shape: the strip's
  // inner edge is the tangent, which carries half coverage and leaves
  // the curve, so it is spurious everywhere but the pixel or so of
  // pinched end it is here to cover.
  const Point tangent = (toward - at) / tangent_length *
                        std::min(tangent_length, kTangentPixels * fringe_);
  const Point out = OutwardNormal(at, toward - at) * fringe_;
  PushVertex(at, kPathImplicitCurveP0);
  PushVertex(at + tangent, kPathImplicitCurveP0);
  PushVertex(at + out, kPathImplicitFringeOuter);
  PushVertex(at + tangent, kPathImplicitCurveP0);
  PushVertex(at + tangent + out, kPathImplicitFringeOuter);
  PushVertex(at + out, kPathImplicitFringeOuter);
}

void ConvexMeshEmitter::Joint(const PathEdge& prev, const PathEdge& next) {
  const Point joint = prev.p1;
  const Point n_prev = OutwardNormal(
      joint, prev.is_curve ? joint - prev.control : joint - prev.p0);
  const Point n_next = OutwardNormal(
      joint, next.is_curve ? next.control - joint : next.p1 - joint);
  if ((n_prev - n_next).GetLengthSquared() < 1e-4f) {
    return;
  }
  PushVertex(joint, kPathImplicitCurveP0);
  PushVertex(joint + n_prev * fringe_, kPathImplicitFringeOuter);
  PushVertex(joint + n_next * fringe_, kPathImplicitFringeOuter);
}

void ConvexMeshEmitter::Edge(const PathEdge& edge) {
  if (started_) {
    Joint(prev_, edge);
  } else {
    first_ = edge;
    started_ = true;
  }
  prev_ = edge;

  if (edge.is_curve) {
    PushVertex(centroid_, kPathImplicitInterior);
    PushVertex(edge.p0, kPathImplicitInterior);
    PushVertex(edge.p1, kPathImplicitInterior);
    PushVertex(edge.p0, kPathImplicitCurveP0);
    PushVertex(edge.control, kPathImplicitCurveControl);
    PushVertex(edge.p1, kPathImplicitCurveP1);
    TangentStrip(edge.p0, edge.control);
    TangentStrip(edge.p1, edge.control);
    return;
  }

  if (edge.p0 == edge.p1) {
    return;
  }

  PushVertex(centroid_, kPathImplicitInterior);
  PushVertex(edge.p0, kPathImplicitCurveP0);
  PushVertex(edge.p1, kPathImplicitCurveP0);
  const Point normal = OutwardNormal(edge.p0, edge.p1 - edge.p0) * fringe_;
  const Point q0 = edge.p0 + normal;
  const Point q1 = edge.p1 + normal;
  PushVertex(edge.p0, kPathImplicitCurveP0);
  PushVertex(edge.p1, kPathImplicitCurveP0);
  PushVertex(q0, kPathImplicitFringeOuter);
  PushVertex(edge.p1, kPathImplicitCurveP0);
  PushVertex(q1, kPathImplicitFringeOuter);
  PushVertex(q0, kPathImplicitFringeOuter);
}

void ConvexMeshEmitter::Finish() {
  if (started_) {
    Joint(prev_, first_);
  }
}

namespace {

//------------------------------------------------------------------------------
// The idealized round rect.
//
// Every vertex of a round rect is a corner of the rect, plus an offset
// in that corner's radii, plus a device pixel of AA fringe, plus a run
// along the contour. All four terms are known for the shape in the
// abstract, so the mesh is built once in those terms and a draw only
// fills them in.

/// What a vertex's radius offset is measured from. The centre is last:
/// no radius reaches it, and it is where the interior fans from.
enum RRectAnchor : uint8_t {
  kAnchorTopLeft,
  kAnchorTopRight,
  kAnchorBottomRight,
  kAnchorBottomLeft,
  kAnchorCentre,
  kAnchorCount,
};

/// Halving a quarter circle's conic puts its midpoint and the two
/// sub-controls these fractions of a radius from the sharp corner.
constexpr Scalar kConicMidWeight = 1.0f / (2.0f + kSqrt2);
constexpr Scalar kConicControlWeight = 2.0f / (2.0f + kSqrt2);
/// How far an arc end is from its control, in radii: the whole tangent
/// the fringe at a curve's pinched end could run along.
constexpr Scalar kTangentFraction = kSqrt2 - 1.0f;

struct RRectTemplateVertex {
  uint8_t anchor;
  uint8_t implicit_class;
  /// From the anchor, in units of its radii.
  Point radius_offset;
  /// A device pixel outward, for a fringe vertex.
  Point fringe;
  /// Along the contour, for the fringe at a curve's ends.
  Point tangent;
};

/// A point of the idealized contour.
struct TemplatePoint {
  uint8_t anchor;
  Point offset;
};

/// A corner, as the contour meets it: it comes in at `entry`, turns
/// through a quarter arc, and leaves at `exit`. Offsets are in the
/// corner's own radii, normals are directions in the rect's space.
struct TemplateCorner {
  uint8_t anchor;
  Point entry;
  Point entry_normal;
  Point exit;
  Point exit_normal;
};

/// Clockwise in y-down space from the top edge, which is the order
/// ForEachRRectEdge walks, so the two meshes come out vertex for vertex.
constexpr TemplateCorner kCorners[4] = {
    {kAnchorTopRight, {-1, 0}, {0, -1}, {0, 1}, {1, 0}},
    {kAnchorBottomRight, {0, -1}, {1, 0}, {-1, 0}, {0, 1}},
    {kAnchorBottomLeft, {1, 0}, {0, 1}, {0, -1}, {-1, 0}},
    {kAnchorTopLeft, {0, 1}, {-1, 0}, {1, 0}, {0, -1}},
};

/// The way the contour runs where its outward normal is `normal`.
constexpr Point Forward(Point normal) {
  return Point(-normal.y, normal.x);
}

/// The mesh of the idealized round rect, in the order ConvexMeshEmitter
/// walks a contour: per edge a joint with the edge before it, the
/// interior triangle, and the coverage the edge itself carries.
/// `feather_straight_edges` is false when the transform will leave the
/// four straight runs square to the pixel grid, which is where they need
/// no ramp of their own -- the same trade a plain rect makes.
std::vector<RRectTemplateVertex> BuildRRectTemplate(
    bool feather_straight_edges) {
  std::vector<RRectTemplateVertex> out;
  const TemplatePoint centre{kAnchorCentre, Point()};

  auto push = [&out](const TemplatePoint& at, Point fringe, Point tangent,
                     uint8_t implicit_class) {
    out.push_back(RRectTemplateVertex{
        .anchor = at.anchor,
        .implicit_class = implicit_class,
        .radius_offset = at.offset,
        .fringe = fringe,
        .tangent = tangent,
    });
  };

  /// The wedge of fringe bridging two edges' outward normals. Flat
  /// wherever the contour is smooth, which is everywhere but a corner
  /// with no radius.
  auto joint = [&push](const TemplatePoint& at, Point n_prev, Point n_next) {
    push(at, Point(), Point(), kPathImplicitCurveP0);
    push(at, n_prev, Point(), kPathImplicitFringeOuter);
    push(at, n_next, Point(), kPathImplicitFringeOuter);
  };

  /// The fringe either side of a curve's end, running out along the
  /// tangent to cover what the curve's own triangle pinches out of.
  auto strip = [&push](const TemplatePoint& at, Point normal, Point tangent) {
    push(at, Point(), Point(), kPathImplicitCurveP0);
    push(at, Point(), tangent, kPathImplicitCurveP0);
    push(at, normal, Point(), kPathImplicitFringeOuter);
    push(at, Point(), tangent, kPathImplicitCurveP0);
    push(at, normal, tangent, kPathImplicitFringeOuter);
    push(at, normal, Point(), kPathImplicitFringeOuter);
  };

  auto line = [&push, &centre, feather_straight_edges](const TemplatePoint& p0,
                                                       const TemplatePoint& p1,
                                                       Point normal) {
    push(centre, Point(), Point(), kPathImplicitInterior);
    push(p0, Point(), Point(), kPathImplicitCurveP0);
    push(p1, Point(), Point(), kPathImplicitCurveP0);

    if (!feather_straight_edges) {
      return;
    }
    push(p0, Point(), Point(), kPathImplicitCurveP0);
    push(p1, Point(), Point(), kPathImplicitCurveP0);
    push(p0, normal, Point(), kPathImplicitFringeOuter);
    push(p1, Point(), Point(), kPathImplicitCurveP0);
    push(p1, normal, Point(), kPathImplicitFringeOuter);
    push(p0, normal, Point(), kPathImplicitFringeOuter);
  };

  auto curve = [&push, &strip, &centre](
                   const TemplatePoint& p0, const TemplatePoint& control,
                   const TemplatePoint& p1, Point n0, Point n1) {
    push(centre, Point(), Point(), kPathImplicitInterior);
    push(p0, Point(), Point(), kPathImplicitInterior);
    push(p1, Point(), Point(), kPathImplicitInterior);

    push(p0, Point(), Point(), kPathImplicitCurveP0);
    push(control, Point(), Point(), kPathImplicitCurveControl);
    push(p1, Point(), Point(), kPathImplicitCurveP1);

    // Both run toward the control, which is forward from the start of
    // the curve and back from its end.
    strip(p0, n0, Forward(n0));
    strip(p1, n1, -Forward(n1));
  };

  for (int i = 0; i < 4; i++) {
    const TemplateCorner& previous = kCorners[(i + 3) % 4];
    const TemplateCorner& corner = kCorners[i];
    const TemplatePoint from{previous.anchor, previous.exit};
    const TemplatePoint to{corner.anchor, corner.entry};

    if (i > 0) {
      joint(from, previous.exit_normal, corner.entry_normal);
    }
    line(from, to, corner.entry_normal);

    // The quarter arc, as the two quadratics its conic halves into.
    const TemplatePoint mid{corner.anchor,
                            (corner.entry + corner.exit) * kConicMidWeight};
    const Point mid_normal =
        (corner.entry_normal + corner.exit_normal).Normalize();
    const TemplatePoint control_in{corner.anchor,
                                   corner.entry * kConicControlWeight};
    const TemplatePoint control_out{corner.anchor,
                                    corner.exit * kConicControlWeight};
    const TemplatePoint exit{corner.anchor, corner.exit};

    joint(to, corner.entry_normal, corner.entry_normal);
    curve(to, control_in, mid, corner.entry_normal, mid_normal);
    joint(mid, mid_normal, mid_normal);
    curve(mid, control_out, exit, mid_normal, corner.exit_normal);
  }

  // What closes the ring, between the last corner and the top edge.
  const TemplateCorner& last = kCorners[3];
  joint(TemplatePoint{last.anchor, last.exit}, last.exit_normal,
        kCorners[0].entry_normal);

  return out;
}

const std::vector<RRectTemplateVertex>& GetRRectTemplate(
    bool feather_straight_edges) {
  static const std::vector<RRectTemplateVertex>* feathered =
      new std::vector<RRectTemplateVertex>(BuildRRectTemplate(true));
  static const std::vector<RRectTemplateVertex>* square =
      new std::vector<RRectTemplateVertex>(BuildRRectTemplate(false));
  return feather_straight_edges ? *feathered : *square;
}

/// Whether the round rect's straight runs come out of `matrix` still
/// square to the pixel grid. They are axis aligned in the shape's own
/// space, so anything that only scales and translates leaves them so,
/// and a ramp along them would be a ramp across nothing.
bool StraightEdgesAreFeathered(const Matrix& matrix) {
  return !matrix.IsTranslationScaleOnly();
}

struct RRectAnchorPlacement {
  Point origin;
  Point radius;
  Scalar tangent;
};

/// Where a corner sits, what the mesh around it is scaled by, and how
/// far the fringe at its arc ends runs.
RRectAnchorPlacement PlaceAnchor(Point origin, Size radius, Scalar fringe) {
  return RRectAnchorPlacement{
      .origin = origin,
      .radius = Point(radius.width, radius.height),
      .tangent =
          std::min(kTangentFraction * std::min(radius.width, radius.height),
                   kTangentPixels * fringe),
  };
}

}  // namespace

std::pair<uint32_t, uint32_t> RRectGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  const auto count = static_cast<uint32_t>(
      GetRRectTemplate(StraightEdgesAreFeathered(matrix)).size());
  return {count, count};
}

void RRectGeometryGenerator::Generate(const PrPicture& picture,
                                      const Draw& draw,
                                      const Matrix& matrix,
                                      Point* position_out,
                                      Attributes* attributes_out,
                                      uint16_t* index_out,
                                      PrPaint* paint_out,
                                      uint16_t index_start,
                                      uint32_t paint_index,
                                      const GeometryContext& frame) {
  const Rect& rect = draw.rect;
  const RoundingRadii& radii = draw.radii;
  const Scalar fringe = 1.0f / matrix.GetMaxBasisLengthXY();

  const RRectAnchorPlacement anchors[kAnchorCount] = {
      PlaceAnchor(rect.GetLeftTop(), radii.top_left, fringe),
      PlaceAnchor(rect.GetRightTop(), radii.top_right, fringe),
      PlaceAnchor(rect.GetRightBottom(), radii.bottom_right, fringe),
      PlaceAnchor(rect.GetLeftBottom(), radii.bottom_left, fringe),
      PlaceAnchor(rect.GetCenter(), Size(), fringe),
  };

  const uint32_t color = draw.color.premultipliedRGBA();
  const std::vector<RRectTemplateVertex>& mesh =
      GetRRectTemplate(StraightEdgesAreFeathered(matrix));
  for (uint32_t i = 0; i < mesh.size(); i++) {
    const RRectTemplateVertex& vertex = mesh[i];
    const RRectAnchorPlacement& anchor = anchors[vertex.anchor];
    position_out[i] = anchor.origin +                         //
                      vertex.radius_offset * anchor.radius +  //
                      vertex.fringe * fringe +                //
                      vertex.tangent * anchor.tangent;
    attributes_out[i] = Attributes{
        .uv = Point(0, 0),
        .color = color,
        .paint = PackPaint(paint_index, vertex.implicit_class),
    };
    index_out[i] = static_cast<uint16_t>(index_start + i);
  }
}

namespace {

/// The two halves of a shadow, in the order they are drawn.
struct ShadowHalf {
  Scalar blur = 0;
  Scalar alpha = 0;
  /// How far the silhouette is thrown before it is blurred.
  Scalar offset_y = 0;
  ShadowInterior interior = ShadowInterior::kFull;
};

std::array<ShadowHalf, 2> ShadowHalves(const Draw::ShadowData& shadow) {
  // An opaque occluder covers its own interior, so the halves under it
  // are not drawn: the spot's umbra is provably inside a convex
  // occluder, and the ambient's is what the silhouette bounds.
  const bool transparent = shadow.transparent_occluder;
  return {
      ShadowHalf{
          .blur = AmbientShadowBlur(shadow.elevation),
          .alpha = kAmbientShadowAlpha,
          .offset_y = 0,
          .interior = transparent ? ShadowInterior::kFull
                                  : ShadowInterior::kFromSilhouette,
      },
      ShadowHalf{
          .blur = SpotShadowBlur(shadow.elevation),
          .alpha = kSpotShadowAlpha,
          .offset_y = shadow.elevation,
          .interior =
              transparent ? ShadowInterior::kFull : ShadowInterior::kRingOnly,
      },
  };
}

}  // namespace

namespace {

/// One band of blurred coverage around a silhouette: the fan over its
/// interior when the interior is covered at all, and the ring between the
/// inner and outer rims either way. Coverage across the band comes from
/// the Gaussian LUT, which a shadow and a mask blur read the same way.
struct BlurBand {
  /// Half the band's width. The LUT spans two sigma either side of the
  /// silhouette, so this is two sigma.
  Scalar blur = 0;
  uint32_t color = 0;
  ShadowInterior interior = ShadowInterior::kFull;
};

uint32_t BlurBandVertexCount(uint32_t count, ShadowInterior interior) {
  const uint32_t fan = interior == ShadowInterior::kFull ? 3u : 0u;
  return count * (fan + 6u);
}

void EmitBlurBand(const Point* silhouette,
                  uint32_t count,
                  const BlurBand& band,
                  uint32_t paint,
                  ShadowRing& ring,
                  Point* position_out,
                  Attributes* attributes_out,
                  uint16_t* index_out,
                  uint16_t index_start,
                  uint32_t& vertex) {
  BuildShadowRing(silhouette, count, band.blur, ring);

  auto push = [&](Point position, Scalar u) {
    position_out[vertex] = position;
    attributes_out[vertex] = Attributes{
        .uv = Point(u, 0.5f),
        .color = band.color,
        .paint = paint,
    };
    index_out[vertex] = static_cast<uint16_t>(index_start + vertex);
    vertex++;
  };

  // Where a distance inside the silhouette falls on the ramp, which runs
  // from a blur radius outside through the silhouette to a blur radius
  // inside.
  const Scalar u_outer = 0.5f / kShadowLUTWidth;
  const Scalar u_inner = 1.0f - 0.5f / kShadowLUTWidth;
  auto u_at = [&](Scalar inside) {
    const Scalar t =
        band.blur > 0 ? (inside + band.blur) / (2 * band.blur) : 1.0f;
    return u_outer + std::clamp(t, 0.0f, 1.0f) * (u_inner - u_outer);
  };
  // The inner rim stops where it actually reached, which is short of a
  // blur radius on anything narrower than one.
  const Scalar u_ring_inner = u_at(ring.inset);
  const Scalar u_centroid = u_at(ring.inradius);

  const Point* fan_edge =
      band.interior == ShadowInterior::kFull ? ring.inner.data() : nullptr;
  const Point* ring_from = ring.inner.data();
  Scalar ring_from_u = u_ring_inner;
  if (band.interior == ShadowInterior::kFromSilhouette) {
    ring_from = silhouette;
    ring_from_u = 0.5f;
  }

  for (uint32_t i = 0; i < count; i++) {
    const uint32_t j = (i + 1) % count;
    if (fan_edge != nullptr) {
      push(ring.centroid, u_centroid);
      push(fan_edge[i], u_ring_inner);
      push(fan_edge[j], u_ring_inner);
    }
    push(ring_from[i], ring_from_u);
    push(ring.outer[i], u_outer);
    push(ring_from[j], ring_from_u);
    push(ring_from[j], ring_from_u);
    push(ring.outer[i], u_outer);
    push(ring.outer[j], u_outer);
  }
}

/// Point the paint at the Gaussian LUT, which it reads as coverage
/// rather than as colour.
void BindBlurLUT(PrPaint* paint_out, const GeometryContext& frame) {
  if (frame.shadow_lut == nullptr) {
    return;
  }
  paint_out->texture_index = SlotFor(*frame.textures, frame.shadow_lut);
  paint_out->flags |= kPaintFlagTextureIsCoverage;
}

}  // namespace

std::pair<uint32_t, uint32_t> ShadowGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  const uint32_t count = draw.shadow_data.length;
  uint32_t vertices = 0;
  for (const ShadowHalf& half : ShadowHalves(draw.shadow_data)) {
    vertices += BlurBandVertexCount(count, half.interior);
  }
  return {vertices, vertices};
}

void ShadowGeometryGenerator::Generate(const PrPicture& picture,
                                       const Draw& draw,
                                       const Matrix& matrix,
                                       Point* position_out,
                                       Attributes* attributes_out,
                                       uint16_t* index_out,
                                       PrPaint* paint_out,
                                       uint16_t index_start,
                                       uint32_t paint_index,
                                       const GeometryContext& frame) {
  const Draw::ShadowData& shadow = draw.shadow_data;
  const uint32_t count = shadow.length;
  BindBlurLUT(paint_out, frame);

  const Point* recorded = picture.GetPositions().data() + shadow.offset;
  const uint32_t paint = PackPaint(paint_index);
  uint32_t vertex = 0;
  for (const ShadowHalf& half : ShadowHalves(shadow)) {
    silhouette_.assign(recorded, recorded + count);
    for (Point& point : silhouette_) {
      point.y += half.offset_y;
    }
    EmitBlurBand(
        silhouette_.data(), count,
        BlurBand{
            .blur = half.blur,
            .color = draw.color.withAlphaF(draw.color.getAlphaF() * half.alpha)
                         .premultipliedRGBA(),
            .interior = half.interior,
        },
        paint, ring_, position_out, attributes_out, index_out, index_start,
        vertex);
  }
}

std::pair<uint32_t, uint32_t> BlurGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  const uint32_t count =
      BlurBandVertexCount(draw.blur_data.length, ShadowInterior::kFull);
  return {count, count};
}

void BlurGeometryGenerator::Generate(const PrPicture& picture,
                                     const Draw& draw,
                                     const Matrix& matrix,
                                     Point* position_out,
                                     Attributes* attributes_out,
                                     uint16_t* index_out,
                                     PrPaint* paint_out,
                                     uint16_t index_start,
                                     uint32_t paint_index,
                                     const GeometryContext& frame) {
  const Draw::BlurData& blur = draw.blur_data;
  BindBlurLUT(paint_out, frame);

  // The mesh is built in the shape's own space. A sigma the paint gave
  // in the shape's space is already there; one it gave in the device's
  // has to come back through the transform to get there.
  const Scalar scale = matrix.GetMaxBasisLengthXY();
  const Scalar sigma =
      blur.respect_ctm || scale <= 0 ? blur.sigma : blur.sigma / scale;

  const Point* recorded = picture.GetPositions().data() + blur.offset;
  uint32_t vertex = 0;
  EmitBlurBand(recorded, blur.length,
               BlurBand{
                   .blur = 2.0f * sigma,
                   .color = draw.color.premultipliedRGBA(),
                   // A normal blur is fuzzy on both sides, so the
                   // interior is the fan under the inner rim.
                   .interior = ShadowInterior::kFull,
               },
               PackPaint(paint_index), ring_, position_out, attributes_out,
               index_out, index_start, vertex);
}

std::pair<uint32_t, uint32_t> AtlasGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  const auto sprites = static_cast<uint32_t>(
      picture.GetAtlases()[draw.atlas_data.atlas_index].transforms.size());
  return {sprites * 4, sprites * 6};
}

void AtlasGeometryGenerator::Generate(const PrPicture& picture,
                                      const Draw& draw,
                                      const Matrix& matrix,
                                      Point* position_out,
                                      Attributes* attributes_out,
                                      uint16_t* index_out,
                                      PrPaint* paint_out,
                                      uint16_t index_start,
                                      uint32_t paint_index,
                                      const GeometryContext& frame) {
  const PrAtlas& atlas = picture.GetAtlases()[draw.atlas_data.atlas_index];

  // One image for every sprite, so one slot for the whole draw.
  GPUTexture* texture = frame.context->GetDLImageTexture(atlas.image);
  if (texture != nullptr) {
    paint_out->texture_index = SlotFor(*frame.textures, texture);
    if (atlas.sampling == flutter::DlImageSampling::kNearestNeighbor) {
      paint_out->flags |= kPaintFlagSampleNearest;
    }
  }

  const Size image =
      Size(atlas.image->GetSize().width, atlas.image->GetSize().height);
  const uint32_t paint = PackPaint(paint_index);
  const uint32_t tint = draw.color.premultipliedRGBA();
  for (size_t sprite = 0; sprite < atlas.transforms.size(); sprite++) {
    const Rect& source = atlas.textures[sprite];
    // Both in the order Rect::GetPoints uses, so corner i of the quad
    // is corner i of what it cuts out.
    const Quad corners = atlas.transforms[sprite].GetQuad(source.GetSize());
    const std::array<Point, 4> uvs =
        Rect::MakeLTRB(
            source.GetLeft() / image.width, source.GetTop() / image.height,
            source.GetRight() / image.width, source.GetBottom() / image.height)
            .GetPoints();

    const uint32_t color = sprite < atlas.colors.size()
                               ? atlas.colors[sprite].premultipliedRGBA()
                               : tint;
    const auto vertex = static_cast<uint32_t>(sprite * 4);
    for (int corner = 0; corner < 4; corner++) {
      position_out[vertex + corner] = corners[corner];
      attributes_out[vertex + corner] = Attributes{
          .uv = uvs[corner],
          .color = color,
          .paint = paint,
      };
    }
    WriteQuadIndices(index_out + sprite * 6, index_start + vertex);
  }
}

std::pair<uint32_t, uint32_t> VerticesGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  const flutter::DlVertices& vertices =
      *picture.GetVertices()[draw.vertices_data.vertices_index];
  const auto count = static_cast<uint32_t>(vertices.vertex_count());
  const auto indices = static_cast<uint32_t>(vertices.index_count());
  return {count, indices > 0 ? indices : count};
}

void VerticesGeometryGenerator::Generate(const PrPicture& picture,
                                         const Draw& draw,
                                         const Matrix& matrix,
                                         Point* position_out,
                                         Attributes* attributes_out,
                                         uint16_t* index_out,
                                         PrPaint* paint_out,
                                         uint16_t index_start,
                                         uint32_t paint_index,
                                         const GeometryContext& frame) {
  const flutter::DlVertices& vertices =
      *picture.GetVertices()[draw.vertices_data.vertices_index];
  const auto count = static_cast<uint32_t>(vertices.vertex_count());
  const Point* positions = vertices.vertex_data();
  const Point* uvs = vertices.texture_coordinate_data();
  const flutter::DlColor* colors = vertices.colors();

  const uint32_t paint = PackPaint(paint_index);
  const uint32_t color = draw.color.premultipliedRGBA();
  for (uint32_t i = 0; i < count; i++) {
    position_out[i] = positions[i];
    attributes_out[i] = Attributes{
        // The uvs sample nothing yet: what a vertex buffer samples is
        // the paint's colour source, which is not recorded.
        .uv = uvs != nullptr ? uvs[i] : Point(0, 0),
        .color = colors != nullptr ? colors[i].premultipliedRGBA() : color,
        .paint = paint,
    };
  }

  const auto index_count = static_cast<uint32_t>(vertices.index_count());
  if (index_count == 0) {
    for (uint32_t i = 0; i < count; i++) {
      index_out[i] = static_cast<uint16_t>(index_start + i);
    }
    return;
  }
  const uint16_t* indices = vertices.indices();
  for (uint32_t i = 0; i < index_count; i++) {
    index_out[i] = static_cast<uint16_t>(index_start + indices[i]);
  }
}

std::pair<uint32_t, uint32_t> ConcavePathGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  const flutter::DlPath& path = picture.GetPaths()[draw.path_data.path_index];

  vertex_count_ = 0;
  if (const auto* edges = path.GetEdges()) {
    for (const auto& edge : *edges) {
      if (edge.p0 == edge.p1) {
        continue;
      }
      vertex_count_ += edge.is_curve ? 6 : 9;
    }
  }
  return {vertex_count_, vertex_count_};
}

void ConcavePathGeometryGenerator::Generate(const PrPicture& picture,
                                            const Draw& draw,
                                            const Matrix& matrix,
                                            Point* position_out,
                                            Attributes* attributes_out,
                                            uint16_t* index_out,
                                            PrPaint* paint_out,
                                            uint16_t index_start,
                                            uint32_t paint_index,
                                            const GeometryContext& frame) {
  const uint32_t color = draw.color.premultipliedRGBA();
  uint32_t index = 0;
  auto push = [&](Point position, uint8_t implicit_class) {
    position_out[index] = position;
    attributes_out[index] = Attributes{
        // Untextured coverage, so nothing is sampled for it.
        .uv = Point(0, 0),
        .color = color,
        .paint = PackPaint(paint_index, implicit_class),
    };
    index_out[index] = static_cast<uint16_t>(index_start + index);
    index++;
  };

  /// A device pixel in local units: what the fringe runs out by.
  const Scalar fringe = 1.0f / matrix.GetMaxBasisLengthXY();

  const flutter::DlPath& path = picture.GetPaths()[draw.path_data.path_index];
  const auto* edges = path.GetEdges();
  if (edges == nullptr) {
    return;
  }

  Point anchor;
  for (const PathEdge& edge : *edges) {
    // Taken before the degenerate skip: a contour that opens on an edge
    // of no length still anchors its fan there.
    if (edge.starts_contour) {
      anchor = edge.p0;
    }
    if (edge.p0 == edge.p1) {
      continue;
    }

    if (edge.is_curve) {
      push(anchor, kPathImplicitInterior);
      push(edge.p0, kPathImplicitInterior);
      push(edge.p1, kPathImplicitInterior);
      push(edge.p0, kPathImplicitCurveP0);
      push(edge.control, kPathImplicitCurveControl);
      push(edge.p1, kPathImplicitCurveP1);
      continue;
    }

    push(anchor, kPathImplicitInterior);
    push(edge.p0, kPathImplicitCurveP0);
    push(edge.p1, kPathImplicitCurveP0);

    const Point direction = edge.p1 - edge.p0;
    const Scalar length = direction.GetLength();
    Point normal =
        length < 1e-6f ? Point() : Point(direction.y, -direction.x) / length;
    if (normal.Dot(edge.p0 - anchor) < 0) {
      normal = -normal;
    }

    const Point q0 = edge.p0 + normal * fringe;
    const Point q1 = edge.p1 + normal * fringe;
    push(edge.p0, kPathImplicitCurveP0);
    push(q0, kPathImplicitFringeOuter);
    push(q1, kPathImplicitFringeOuter);
    push(edge.p0, kPathImplicitCurveP0);
    push(q1, kPathImplicitFringeOuter);
    push(edge.p1, kPathImplicitCurveP0);
  }
  FML_DCHECK(index == vertex_count_);
}

std::pair<uint32_t, uint32_t> ConvexPathGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  // Counting the mesh means building it, so it is built here and kept
  // for the write that follows rather than walked a second time.
  ConvexMeshEmitter emitter(draw.rect.GetCenter(), matrix.GetMaxBasisLengthXY(),
                            draw.color.premultipliedRGBA(), staging_);
  const flutter::DlPath& path = picture.GetPaths()[draw.path_data.path_index];
  if (const auto* edges = path.GetEdges()) {
    for (const auto& edge : *edges) {
      emitter.Edge(edge);
    }
  }
  emitter.Finish();
  return {emitter.GetVertexCount(), emitter.GetVertexCount()};
}

void ConvexPathGeometryGenerator::Generate(const PrPicture& picture,
                                           const Draw& draw,
                                           const Matrix& matrix,
                                           Point* position_out,
                                           Attributes* attributes_out,
                                           uint16_t* index_out,
                                           PrPaint* paint_out,
                                           uint16_t index_start,
                                           uint32_t paint_index,
                                           const GeometryContext& frame) {
  const uint32_t count = staging_.GetVertexCount();
  std::memcpy(position_out, staging_.positions.data(), count * sizeof(Point));
  for (uint32_t i = 0; i < count; i++) {
    index_out[i] = static_cast<uint16_t>(index_start + i);

    Attributes attributes = staging_.attributes[i];
    attributes.paint |= paint_index;
    attributes_out[i] = attributes;
  }
}

void DrawPointsGeometryGenerator::Generate(const PrPicture& picture,
                                           const Draw& draw,
                                           const Matrix& matrix,
                                           Point* position_out,
                                           Attributes* attributes_out,
                                           uint16_t* index_out,
                                           PrPaint* paint_out,
                                           uint16_t index_start,
                                           uint32_t paint_index,
                                           const GeometryContext& frame) {
  const Draw::PointData& points = draw.point_data;
  const std::vector<Point>& centres = picture.GetPositions();
  const uint32_t color = draw.color.premultipliedRGBA();
  const uint32_t paint = PackPaint(paint_index);

  for (uint32_t i = 0; i < points.length; i++) {
    const Point centre = centres[points.offset + i];
    const uint32_t vertex = i * 4;
    for (int corner = 0; corner < 4; corner++) {
      position_out[vertex + corner] =
          centre + kQuadCorners[corner] * points.radius;
      attributes_out[vertex + corner] = Attributes{
          // TODO: a round point is still a square until the fragment
          // stage has a mode that reads this back as a circle.
          .uv = points.round ? kQuadCorners[corner] : Point(0, 0),
          .color = color,
          .paint = paint,
      };
    }
    WriteQuadIndices(index_out + i * 6, index_start + vertex);
  }
}

namespace {

/// Whether the rect comes out of the transform still square to the pixel grid.
bool RectIsFeathered(const Draw& draw, const Matrix& matrix) {
  return draw.type == Draw::DrawType::kRect && !matrix.IsTranslationScaleOnly();
}

/// The ring of a feathered rect, as a loop rather than the corner order
/// an unfeathered one is written in.
constexpr int kFeatherIndices[30] = {
    // The interior, fully covered.
    0, 1, 2, 0, 2, 3,
    // A quad per side, from the inner corner out. Each one holds the
    // corner ahead of it as well, so the four of them tile the ring.
    0, 4, 5, 0, 5,
    1,  //
    1, 5, 6, 1, 6,
    2,  //
    2, 6, 7, 2, 7,
    3,  //
    3, 7, 4, 3, 4, 0};
}  // namespace

std::pair<uint32_t, uint32_t> RectGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  return RectIsFeathered(draw, matrix) ? std::pair<uint32_t, uint32_t>{8, 30}
                                       : std::pair<uint32_t, uint32_t>{4, 6};
}

void RectGeometryGenerator::Generate(const PrPicture& picture,
                                     const Draw& draw,
                                     const Matrix& matrix,
                                     Point* position_out,
                                     Attributes* attributes_out,
                                     uint16_t* index_out,
                                     PrPaint* paint_out,
                                     uint16_t index_start,
                                     uint32_t paint_index,
                                     const GeometryContext& frame) {
  const uint32_t color = draw.color.premultipliedRGBA();
  const uint32_t paint = PackPaint(paint_index);

  if (!RectIsFeathered(draw, matrix)) {
    position_out[0] = draw.rect.GetLeftTop();
    position_out[1] = draw.rect.GetLeftBottom();
    position_out[2] = draw.rect.GetRightTop();
    position_out[3] = draw.rect.GetRightBottom();
    for (int corner = 0; corner < 4; corner++) {
      attributes_out[corner] = Attributes{
          .uv = Point(0, 0),
          .color = color,
          .paint = paint,
      };
    }
    WriteQuadIndices(index_out, index_start);
    return;
  }

  // A device pixel coverage ramp.
  const Scalar reach = 0.5f / matrix.GetMaxBasisLengthXY();
  const Size size = draw.rect.GetSize();
  const Rect inner = draw.rect.Expand(-std::min(reach, size.width * 0.5f),
                                      -std::min(reach, size.height * 0.5f));
  const Rect outer = draw.rect.Expand(reach, reach);

  const Point corners[8] = {
      inner.GetLeftTop(),     inner.GetRightTop(),   inner.GetRightBottom(),
      inner.GetLeftBottom(),  outer.GetLeftTop(),    outer.GetRightTop(),
      outer.GetRightBottom(), outer.GetLeftBottom(),
  };
  for (int corner = 0; corner < 8; corner++) {
    position_out[corner] = corners[corner];
    attributes_out[corner] = Attributes{
        .uv = Point(0, 0),
        .color = corner < 4 ? color : 0u,
        .paint = paint,
    };
  }
  for (int i = 0; i < 30; i++) {
    index_out[i] = static_cast<uint16_t>(index_start + kFeatherIndices[i]);
  }
}

void ImageRectGeometryGenerator::Generate(const PrPicture& picture,
                                          const Draw& draw,
                                          const Matrix& matrix,
                                          Point* position_out,
                                          Attributes* attributes_out,
                                          uint16_t* index_out,
                                          PrPaint* paint_out,
                                          uint16_t index_start,
                                          uint32_t paint_index,
                                          const GeometryContext& frame) {
  // What the paint samples: the image as a texture of this backend's,
  // in a slot of the pass's own table.
  GPUTexture* texture = frame.context->GetDLImageTexture(
      picture.GetImages()[draw.uv_data.image_index]);
  if (texture != nullptr) {
    paint_out->texture_index = SlotFor(*frame.textures, texture);
    // TODO: cubic sampling, which falls back to linear here.
    if (draw.uv_data.sampling == flutter::DlImageSampling::kNearestNeighbor) {
      paint_out->flags |= kPaintFlagSampleNearest;
    }
  }

  // In the corner order UVData is recorded in, which is the order
  // Rect::GetPoints uses: uv[i] belongs to corner i, and pairing them
  // any other way transposes the image.
  const std::array<Point, 4> corners = draw.rect.GetPoints();
  const uint32_t color = draw.color.premultipliedRGBA();
  const uint32_t paint = PackPaint(paint_index);
  for (int corner = 0; corner < 4; corner++) {
    position_out[corner] = corners[corner];
    attributes_out[corner] = Attributes{
        .uv = draw.uv_data.uv[corner],
        .color = color,
        .paint = paint,
    };
  }

  WriteQuadIndices(index_out, index_start);
}

std::pair<uint32_t, uint32_t> TextGeometryGenerator::GetAllocationCount(
    const PrPicture& picture,
    const Draw& draw,
    const Matrix& matrix) {
  uint32_t count = 0;
  for (const TextRun& run :
       picture.GetTextFrames()[draw.text_data.text_index]->GetRuns()) {
    count += static_cast<uint32_t>(run.GetGlyphCount());
  }
  return {count * 4, count * 6};
}

void TextGeometryGenerator::Generate(const PrPicture& picture,
                                     const Draw& draw,
                                     const Matrix& matrix,
                                     Point* position_out,
                                     Attributes* attributes_out,
                                     uint16_t* index_out,
                                     PrPaint* paint_out,
                                     uint16_t index_start,
                                     uint32_t paint_index,
                                     const GeometryContext& frame) {
  // TODO: one atlas page, so it is the slot the table reserves.
  paint_out->texture_index = kGlyphAtlasTextureSlot;
  paint_out->flags |= kPaintFlagTextureIsCoverage;

  const uint32_t glyphs = frame.text->ResolveGlyphs(
      *picture.GetTextFrames()[draw.text_data.text_index],
      draw.text_data.position, matrix, draw.color.premultipliedRGBA(),
      PackPaint(paint_index), position_out, attributes_out);

  for (uint32_t i = 0; i < glyphs; i++) {
    WriteQuadIndices(index_out + i * 6, index_start + i * 4);
  }
}

}  // namespace impeller
