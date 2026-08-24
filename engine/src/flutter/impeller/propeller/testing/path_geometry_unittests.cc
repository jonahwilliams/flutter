// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cmath>

#include "flutter/display_list/geometry/dl_path.h"
#include "flutter/testing/testing.h"
#include "impeller/propeller/path_geometry.h"

namespace impeller {
namespace testing {

namespace {

/// Max deviation of a quadratic's sampled points from a circle of `radius`
/// about `center`.
Scalar MaxRadiusError(const PathEdge& edge, Point center, Scalar radius) {
  Scalar max_error = 0;
  for (int i = 0; i <= 16; i++) {
    const Scalar t = i / 16.0f;
    const Scalar mt = 1 - t;
    const Point p = edge.is_curve
                        ? edge.p0 * (mt * mt) + edge.control * (2 * t * mt) +
                              edge.p1 * (t * t)
                        : edge.p0 * mt + edge.p1 * t;
    max_error = std::max(max_error, std::abs(p.GetDistance(center) - radius));
  }
  return max_error;
}

/// A path's edges as the walker hands them over, which is how every
/// consumer sees a path. Production walks them straight into vertices.
ConvexContour Walk(const flutter::DlPath& path) {
  ConvexContour contour;
  PathEdgeVisitor::PathCallback collect =
      [&](const PathEdge& edge, bool, bool) { contour.edges.push_back(edge); };
  PathEdgeVisitor visitor(collect);
  path.Dispatch(visitor);
  visitor.Finish();
  return contour;
}

}  // namespace

namespace {

/// A regular polygon about the origin, which is the shape a flattened
/// circle takes.
std::vector<Point> Disc(Scalar radius, int count) {
  std::vector<Point> out;
  for (int i = 0; i < count; i++) {
    const Scalar angle = 2 * kPi * i / count;
    out.push_back(Point(radius * std::cos(angle), radius * std::sin(angle)));
  }
  return out;
}

}  // namespace

TEST(PathGeometryTest, AShadowRingNeverFoldsThroughItsOwnMiddle) {
  const Scalar radius = 50;
  const std::vector<Point> disc = Disc(radius, 64);

  // Well inside, right at the middle, and past it. The middle is where
  // the rim used to cross itself: every vertex landed within rounding of
  // the centroid, the sign of each sliver was noise, and the fan came
  // out as a pinwheel of overlapping spokes.
  for (Scalar blur : {10.0f, 40.0f, 50.0f, 100.0f}) {
    ShadowRing ring;
    BuildShadowRing(disc.data(), disc.size(), blur, ring);
    ASSERT_EQ(ring.inner.size(), disc.size()) << "blur " << blur;

    int facing = 0;
    for (size_t i = 0; i < disc.size(); i++) {
      const size_t j = (i + 1) % disc.size();
      // No vertex may end up on the far side of the middle from where it
      // started.
      EXPECT_GE((ring.inner[i] - ring.centroid).Dot(disc[i] - ring.centroid),
                0.0f)
          << "blur " << blur << " vertex " << i;

      const Scalar area =
          (ring.inner[i] - ring.centroid).Cross(ring.inner[j] - ring.centroid);
      const int sign = area > 0 ? 1 : (area < 0 ? -1 : 0);
      if (sign != 0) {
        if (facing == 0) {
          facing = sign;
        }
        EXPECT_EQ(sign, facing)
            << "blur " << blur << " triangle " << i << " faces the other way";
      }
    }

    if (blur >= ring.inradius) {
      // Nothing is left of the interior to fan over, so the rim is the
      // middle exactly rather than a ring of near misses around it.
      for (const Point& inner : ring.inner) {
        EXPECT_EQ(inner, ring.centroid) << "blur " << blur;
      }
      EXPECT_EQ(facing, 0) << "every fan triangle should be degenerate";
    } else {
      EXPECT_NE(facing, 0) << "blur " << blur << " has an interior to fan";
    }
  }
}

TEST(PathGeometryTest, CircleChopsToQuadsThatTrackTheRadius) {
  const Scalar radius = 50;
  const ConvexContour contour =
      Walk(flutter::DlPath::MakeCircle(Point(radius, radius), radius));

  ASSERT_GT(contour.edges.size(), 4u);  // 90-degree arcs must subdivide.
  const Point center(radius, radius);
  for (const PathEdge& edge : contour.edges) {
    EXPECT_TRUE(edge.is_curve);
    // The residual of the fixed split, which scales with the radius.
    EXPECT_LE(MaxRadiusError(edge, center, radius), radius * 0.005f);
  }

  // Contour is closed and continuous.
  for (size_t i = 0; i < contour.edges.size(); i++) {
    const PathEdge& edge = contour.edges[i];
    const PathEdge& next = contour.edges[(i + 1) % contour.edges.size()];
    EXPECT_EQ(edge.p1, next.p0);
  }
}

TEST(PathGeometryTest, CurvesChopToAFixedCountWhateverTheirSize) {
  // Loop-Blinn evaluates each quad's coverage analytically, so splitting
  // finer wins nothing and only shrinks the triangles the implicit is
  // interpolated across.
  EXPECT_EQ(Walk(flutter::DlPath::MakeCircle(Point(10, 10), 10)).edges.size(),
            8u);
  EXPECT_EQ(
      Walk(flutter::DlPath::MakeCircle(Point(2000, 2000), 2000)).edges.size(),
      8u);
}

TEST(PathGeometryTest, ARoundRectClosesWhateverItsRadius) {
  const Rect bounds = Rect::MakeLTRB(0, 0, 100, 40);
  for (Scalar radius : {0.0f, 5.0f, 1000.0f}) {
    const ConvexContour contour =
        Walk(flutter::DlPath::MakeRoundRectXY(bounds, radius, radius));
    ASSERT_FALSE(contour.edges.empty()) << "radius " << radius;
    for (size_t i = 0; i < contour.edges.size(); i++) {
      const PathEdge& edge = contour.edges[i];
      const PathEdge& next = contour.edges[(i + 1) % contour.edges.size()];
      EXPECT_EQ(edge.p1, next.p0) << "radius " << radius;
    }
  }

  // A radius of zero is four lines and no arcs at all.
  const ConvexContour sharp =
      Walk(flutter::DlPath::MakeRoundRectXY(bounds, 0, 0));
  EXPECT_EQ(sharp.edges.size(), 4u);
  for (const PathEdge& edge : sharp.edges) {
    EXPECT_FALSE(edge.is_curve);
  }
}

}  // namespace testing
}  // namespace impeller
