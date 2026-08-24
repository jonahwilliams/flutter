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
  PathEdgeVisitor::PathCallback collect = [&](const PathEdge& edge, bool,
                                                bool) {
    contour.edges.push_back(edge);
  };
  PathEdgeVisitor visitor(collect);
  path.Dispatch(visitor);
  visitor.Finish();
  return contour;
}

}  // namespace

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
