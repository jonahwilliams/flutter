// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_PATH_GEOMETRY_H_
#define FLUTTER_IMPELLER_PROPELLER_PATH_GEOMETRY_H_

#include <algorithm>
#include <type_traits>
#include <vector>

#include "flutter/display_list/geometry/dl_path.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/rect.h"
#include "impeller/geometry/rounding_radii.h"
#include "impeller/geometry/scalar.h"

namespace impeller {

/// One edge of a closed convex contour: a line when `control` is unset,
/// otherwise the quadratic Bezier (p0, control, p1).
/// A closed convex contour of lines and quadratics, in recording order.
struct ConvexContour {
  std::vector<PathEdge> edges;
};

/// Flatten a contour to a closed polyline: line edges contribute their
/// start point; curve edges subdivide into `curve_segments` chords. For
/// blurry consumers (shadows) a handful of segments suffices. Into a
/// caller-owned buffer.
void FlattenContourInto(const ConvexContour& contour,
                        std::vector<Point>& points,
                        int curve_segments = 8);

//------------------------------------------------------------------------------
/// Where a shadow's penumbra band runs for one silhouette.
///
/// Both rims are mitered per vertex, so the band is a blur radius wide
/// measured perpendicular to each edge rather than to each corner.
///
/// The inner rim cannot always have the blur radius it asks for. A
/// corner turns through the same angle however tight it is, so a mitre
/// carries every vertex of it the full inset inward, and once the inset
/// passes what the corner can give those vertices cross each other: the
/// rim turns inside out and the band composites its own penumbra twice,
/// which reads as a dark speckle at every corner. The inset backs off
/// to what the tightest corner allows instead, and the coverage the rim
/// is read at follows it down.
struct ShadowRing {
  std::vector<Point> inner;
  std::vector<Point> outer;
  Point centroid;
  /// How far in the inner rim actually reached, which is what its
  /// coverage is read at.
  Scalar inset = 0;
  /// The largest circle the silhouette contains, which is as far in as
  /// the centroid can claim.
  Scalar inradius = 0;

  Rect GetOuterBounds() const;
};

/// Work out the band `silhouette` casts when blurred by `blur_radius`.
void BuildShadowRing(const Point* silhouette,
                     size_t count,
                     Scalar blur_radius,
                     ShadowRing& ring);

/// How wide each half of an elevation shadow blurs, and how much of the
/// colour's alpha it carries. The ambient half is tight and faint, the
/// spot half wide and dark, and an elevation reads as one only when
/// both are drawn.
constexpr Scalar kAmbientShadowAlpha = 0.039f;
constexpr Scalar kSpotShadowAlpha = 0.25f;
constexpr Scalar kShadowLightRadiusOverHeight = 800.0f / 600.0f;

/// How far a shadow ring's mitre may carry a corner out, so a near-cusp
/// gives a long spike rather than an unbounded one.
constexpr Scalar kShadowMiterLimit = 4.0f;

/// How far past a silhouette a shadow is worth covering, as a multiple
/// of its blur radius.
///
/// The band the mesh spans is two sigma wide either side, so three
/// sigma is one and a half of it: past that the Gaussian has nothing
/// left to show. It is an approximation of what the mitered rim
/// actually reaches, which is cheaper than building the rim to find
/// out, and only a corner mitered further than this pokes outside it.
constexpr Scalar kShadowCoverageOfBlur = 1.5f;

constexpr Scalar AmbientShadowBlur(Scalar elevation) {
  return std::max(1.0f, 0.5f * elevation);
}

constexpr Scalar SpotShadowBlur(Scalar elevation) {
  return std::max(1.0f, kShadowLightRadiusOverHeight * elevation);
}

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_PATH_GEOMETRY_H_
