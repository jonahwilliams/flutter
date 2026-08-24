// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/path_geometry.h"

#include <algorithm>
#include <limits>

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

  // A shape narrower than the blur, or a corner tighter than it, would
  // carry the inner rim past the middle and fold it through itself,
  // compositing its own penumbra twice. Stopping at the centroid's own
  // depth is as far as the rim can go and still be inside.
  ring.inset = std::min(blur_radius, inradius);
  /// Whether the blur reaches the middle from every side at once, which
  /// is what a shape smaller than its own blur does.
  const bool collapsed = blur_radius >= inradius;

  ring.inner.resize(count);
  ring.outer.resize(count);
  for (size_t i = 0; i < count; i++) {
    const Point normal = MiteredNormal(silhouette, count, i, facing);
    ring.outer[i] = silhouette[i] + normal * blur_radius;

    // Never past the middle. A rim that crosses the centroid puts itself
    // on the far side of where it started, and the fan over it turns
    // inside out: neighbouring triangles disagree about which way they
    // face and overlap in a pinwheel, compositing the penumbra twice
    // along every spoke.
    //
    // A shape narrower than its blur collapses to the centroid outright
    // rather than vertex by vertex. Each vertex would otherwise land
    // within rounding of the middle and the sign of each sliver would be
    // noise, which is the pinwheel by another route. Pinned to the same
    // point, every triangle between them is exactly degenerate.
    if (collapsed) {
      ring.inner[i] = ring.centroid;
      continue;
    }
    // Wider than the blur overall, but a corner tight enough for the
    // mitre to carry it past the middle on its own still stops there.
    const Point candidate = silhouette[i] - normal * ring.inset;
    ring.inner[i] =
        (candidate - ring.centroid).Dot(silhouette[i] - ring.centroid) < 0
            ? ring.centroid
            : candidate;
  }
}

}  // namespace impeller
