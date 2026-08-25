// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/path_geometry.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace impeller {

namespace {

/// A zero length edge has no direction to take a normal from, and a
/// bisector this short is two edges doubling back on each other.
constexpr Scalar kDegenerate = 1e-6f;

/// The point at `t` along the quadratic (p0, control, p1).
Point QuadraticAt(const PathEdge& edge, Scalar t) {
  const Scalar mt = 1 - t;
  return edge.p0 * (mt * mt) + edge.control * (2 * t * mt) + edge.p1 * (t * t);
}

/// The unit normal of the edge a->b pointing out of a contour wound in
/// `facing`, or nothing at all if the edge has no length to face along.
Point EdgeNormal(Point a, Point b, Scalar facing) {
  const Point along = b - a;
  const Scalar length = along.GetLength();
  if (length < kDegenerate) {
    return Point();
  }
  return Point(along.y, -along.x) / length * facing;
}

/// The outward direction at vertex `i`, mitered so that offsetting by it
/// holds the offset a constant distance from both of the edges meeting
/// there rather than from the vertex.
Point MiteredNormal(const Point* silhouette,
                    size_t count,
                    size_t i,
                    Scalar facing) {
  const Point previous = silhouette[(i + count - 1) % count];
  const Point vertex = silhouette[i];
  const Point next = silhouette[(i + 1) % count];

  const Point n_previous = EdgeNormal(previous, vertex, facing);
  const Point n_next = EdgeNormal(vertex, next, facing);

  // An edge with no length has no normal, and there is no corner
  // between a direction and nothing to reach around: the mitre is the
  // one normal there is. Falling through with a zero here would take
  // the cosine below to zero and send the mitre out to its limit, which
  // is a spike where the contour merely stalled.
  if (n_previous.IsZero()) {
    return n_next;
  }
  if (n_next.IsZero()) {
    return n_previous;
  }

  Point bisector = n_previous + n_next;
  const Scalar length = bisector.GetLength();
  if (length < kDegenerate) {
    // The edges face opposite ways, so there is no direction between
    // them to miter along. Either one is as good as the other.
    return n_next;
  }
  bisector = bisector / length;

  // The mitre has to reach 1/cos(half the corner) further out than the
  // edge normals do to stay level with both edges. The cosine is what
  // gets clamped rather than its reciprocal: a corner tight enough to
  // send the mitre to infinity would divide by nothing at all here.
  const Scalar cos_half = bisector.Dot(n_next);
  const Scalar scale =
      cos_half > 1.0f / kShadowMiterLimit ? 1.0f / cos_half : kShadowMiterLimit;
  return bisector * scale;
}

/// The tangent of half the angle the contour turns through at vertex
/// `i`, which is how far along each of the edges meeting there a mitre
/// of unit depth slides.
Scalar TanHalfTurn(const Point* silhouette,
                   size_t count,
                   size_t i,
                   Scalar facing) {
  const Point n_previous =
      EdgeNormal(silhouette[(i + count - 1) % count], silhouette[i], facing);
  const Point n_next =
      EdgeNormal(silhouette[i], silhouette[(i + 1) % count], facing);
  if (n_previous.IsZero() || n_next.IsZero()) {
    return 0;  // No turn, because there is no edge to turn from.
  }
  const Scalar cosine = n_previous.Dot(n_next);
  if (cosine <= -1.0f + kDegenerate) {
    return std::numeric_limits<Scalar>::max();  // A cusp, which closes at once.
  }
  return std::abs(n_previous.Cross(n_next)) / (1 + cosine);
}

/// How far a convex contour can be pulled in before its own corners
/// close up.
///
/// Insetting slides each end of an edge along it by the inset times the
/// tangent of half the turn there, and the edge has only its own length
/// to give. Once it has given all of it the edge has vanished and its
/// neighbours have crossed: the rim turns inside out. The contour can
/// go as far as its tightest edge and no further.
Scalar MaxInset(const Point* silhouette, size_t count, Scalar facing) {
  Scalar limit = std::numeric_limits<Scalar>::max();
  for (size_t i = 0; i < count; i++) {
    const size_t j = (i + 1) % count;
    const Scalar length = (silhouette[j] - silhouette[i]).GetLength();
    if (length < kDegenerate) {
      continue;
    }
    const Scalar slide = TanHalfTurn(silhouette, count, i, facing) +
                         TanHalfTurn(silhouette, count, j, facing);
    if (slide > kDegenerate) {
      limit = std::min(limit, length / slide);
    }
  }
  return limit;
}

}  // namespace

void FlattenContourInto(const ConvexContour& contour,
                        std::vector<Point>& points,
                        int curve_segments) {
  points.clear();
  const int segments = std::max(1, curve_segments);
  // The contour closes, so every edge stops short of its own end point:
  // the edge after it starts there, and the last one ends where the
  // first began.
  for (const PathEdge& edge : contour.edges) {
    if (!edge.is_curve) {
      points.push_back(edge.p0);
      continue;
    }
    for (int i = 0; i < segments; i++) {
      points.push_back(QuadraticAt(
          edge, static_cast<Scalar>(i) / static_cast<Scalar>(segments)));
    }
  }
}

Rect ShadowRing::GetOuterBounds() const {
  return Rect::MakePointBounds(outer.begin(), outer.end()).value_or(Rect());
}

void BuildShadowRing(const Point* silhouette,
                     size_t count,
                     Scalar blur_radius,
                     ShadowRing& ring) {
  ring.inner.clear();
  ring.outer.clear();
  ring.centroid = Point();
  ring.inset = 0;
  ring.inradius = 0;
  if (count < 3) {
    return;
  }

  // Which way the silhouette is wound decides which perpendicular of an
  // edge points out of it, and the shoelace sum is the cheapest thing
  // that knows.
  Point sum;
  Scalar twice_area = 0;
  for (size_t i = 0; i < count; i++) {
    const Point vertex = silhouette[i];
    sum += vertex;
    twice_area += vertex.Cross(silhouette[(i + 1) % count]);
  }
  ring.centroid = sum / static_cast<Scalar>(count);
  const Scalar facing = twice_area < 0 ? -1.0f : 1.0f;

  // The largest circle about the centroid the silhouette contains, which
  // is how far in the centroid sits and so the furthest in anything
  // anchored there can claim to be.
  Scalar inradius = std::numeric_limits<Scalar>::max();
  for (size_t i = 0; i < count; i++) {
    inradius =
        std::min(inradius, ring.centroid.GetDistanceToSegment(
                               silhouette[i], silhouette[(i + 1) % count]));
  }
  ring.inradius = inradius;

  // As far in as the rim can go: no further than the blur asked for, no
  // further than the middle, and no further than the tightest corner
  // can be pulled before it closes up. Backing off here rather than
  // folding is what keeps the band from compositing over itself, and
  // the coverage the rim is read at follows the inset it actually got.
  ring.inset =
      std::min({blur_radius, inradius, MaxInset(silhouette, count, facing)});
  const bool collapsed = blur_radius >= inradius;

  ring.inner.resize(count);
  ring.outer.resize(count);
  for (size_t i = 0; i < count; i++) {
    ring.outer[i] = silhouette[i] +
                    MiteredNormal(silhouette, count, i, facing) * blur_radius;
  }

  if (collapsed) {
    // A shape narrower than its blur has no interior left to inset.
    std::fill(ring.inner.begin(), ring.inner.end(), ring.centroid);
    return;
  }
  for (size_t i = 0; i < count; i++) {
    ring.inner[i] = silhouette[i] -
                    MiteredNormal(silhouette, count, i, facing) * ring.inset;
  }
}

}  // namespace impeller
