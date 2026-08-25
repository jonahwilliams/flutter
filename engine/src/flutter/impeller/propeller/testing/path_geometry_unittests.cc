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

TEST(PathGeometryTest, AShadowRingsOuterRimStaysLevelWithTheSilhouette) {
  // The shape a shadow is usually cast by, walked and flattened exactly
  // as production does it.
  const flutter::DlPath path =
      flutter::DlPath::MakeRoundRectXY(Rect::MakeLTRB(0, 0, 200, 100), 20, 20);
  ConvexContour contour = Walk(path);
  std::vector<Point> silhouette;
  FlattenContourInto(contour, silhouette);
  ASSERT_GT(silhouette.size(), 8u);

  const Scalar blur = 15;
  ShadowRing ring;
  BuildShadowRing(silhouette.data(), silhouette.size(), blur, ring);
  ASSERT_EQ(ring.outer.size(), silhouette.size());

  // The rim is the silhouette pushed out by the blur. A mitre reaches
  // further than that to stay level with both of the edges at a corner,
  // but only by 1/cos of half the turn, and a contour flattened this
  // finely turns very little at each vertex.
  for (size_t i = 0; i < silhouette.size(); i++) {
    const Scalar reach = (ring.outer[i] - silhouette[i]).GetLength();
    EXPECT_LE(reach, blur * 1.1f)
        << "vertex " << i << " of " << silhouette.size();
    EXPECT_GE(reach, blur * 0.9f) << "vertex " << i;
  }
}

TEST(PathGeometryTest, ARepeatedSilhouettePointDoesNotSpikeTheRim) {
  // A contour that stalls: two of its points are the same, so the edge
  // between them has no direction to take a normal from. Flattening a
  // curve that has collapsed -- a rounded rect with a corner of no
  // radius, say -- produces exactly this.
  std::vector<Point> silhouette = Disc(50, 32);
  silhouette.insert(silhouette.begin() + 8, silhouette[8]);

  const Scalar blur = 10;
  ShadowRing ring;
  BuildShadowRing(silhouette.data(), silhouette.size(), blur, ring);
  ASSERT_EQ(ring.outer.size(), silhouette.size());

  for (size_t i = 0; i < silhouette.size(); i++) {
    const Scalar reach = (ring.outer[i] - silhouette[i]).GetLength();
    EXPECT_LE(reach, blur * 1.1f) << "vertex " << i;
  }
}

TEST(PathGeometryTest, AnInnerRimNeverTurnsInsideOut) {
  // A corner turns through the same angle however tight it is, so a
  // mitre pulls every vertex of it the full inset inward. Once the inset
  // passes the corner's radius those vertices cross each other and the
  // rim turns inside out, which composites the band over itself.
  const flutter::DlPath path =
      flutter::DlPath::MakeRoundRectXY(Rect::MakeLTRB(0, 0, 200, 100), 20, 20);
  ConvexContour contour = Walk(path);
  std::vector<Point> silhouette;
  FlattenContourInto(contour, silhouette);
  ASSERT_GT(silhouette.size(), 8u);

  // Either side of the corner radius, and past the point where the
  // whole interior goes.
  for (Scalar blur : {5.0f, 20.0f, 25.0f, 40.0f, 60.0f}) {
    ShadowRing ring;
    BuildShadowRing(silhouette.data(), silhouette.size(), blur, ring);
    ASSERT_EQ(ring.inner.size(), silhouette.size()) << "blur " << blur;

    int facing = 0;
    for (size_t i = 0; i < silhouette.size(); i++) {
      const size_t j = (i + 1) % silhouette.size();
      const size_t k = (i + 2) % silhouette.size();
      // No edge of the rim may run against the edge it came from.
      EXPECT_GE(
          (ring.inner[j] - ring.inner[i]).Dot(silhouette[j] - silhouette[i]),
          0.0f)
          << "blur " << blur << " edge " << i;

      const Scalar cross =
          (ring.inner[j] - ring.inner[i]).Cross(ring.inner[k] - ring.inner[j]);
      const int sign = cross > 1e-3f ? 1 : (cross < -1e-3f ? -1 : 0);
      if (sign != 0) {
        if (facing == 0) {
          facing = sign;
        }
        EXPECT_EQ(sign, facing) << "blur " << blur << " turns back at " << i;
      }
    }
  }
}

TEST(PathGeometryTest, AnInnerRimIsTheInsetInsideOfEveryEdge) {
  const flutter::DlPath path =
      flutter::DlPath::MakeRoundRectXY(Rect::MakeLTRB(0, 0, 200, 100), 20, 20);
  ConvexContour contour = Walk(path);
  std::vector<Point> silhouette;
  FlattenContourInto(contour, silhouette);

  const Scalar blur = 25;  // Past the corner radius, so corners close up.
  ShadowRing ring;
  BuildShadowRing(silhouette.data(), silhouette.size(), blur, ring);
  ASSERT_LT(blur, ring.inradius) << "there should be an interior left";
  // The corners cannot give the whole blur, so the rim stops at what
  // they can and says so.
  EXPECT_LT(ring.inset, blur);
  // The corner the rim is held back by is the flattened one, whose
  // chords turn a little less sharply than the arc they came from.
  EXPECT_NEAR(ring.inset, 20, 2.0f) << "about the corner radius";

  // Which perpendicular of an edge points out of this contour, which is
  // whichever one the middle is not on.
  const Point first = silhouette[1] - silhouette[0];
  const Point perpendicular = Point(first.y, -first.x).Normalize();
  const Scalar outward =
      (ring.centroid - silhouette[0]).Dot(perpendicular) < 0 ? 1.0f : -1.0f;

  Scalar shallowest = std::numeric_limits<Scalar>::max();
  for (const Point& inner : ring.inner) {
    // How far inside the whole silhouette this point sits: for a convex
    // contour, its distance to whichever edge line it is nearest.
    Scalar depth = std::numeric_limits<Scalar>::max();
    for (size_t i = 0; i < silhouette.size(); i++) {
      const size_t j = (i + 1) % silhouette.size();
      const Point along = silhouette[j] - silhouette[i];
      const Scalar length = along.GetLength();
      if (length < 1e-6f) {
        continue;
      }
      const Point normal = Point(along.y, -along.x) / length * outward;
      depth = std::min(depth, -(inner - silhouette[i]).Dot(normal));
    }
    EXPECT_GE(depth, ring.inset - 1e-2f)
        << "a rim point sits shallower than the inset it reports, so the "
           "band is read at a coverage it never reached";
    shallowest = std::min(shallowest, depth);
  }
  // And the rim really does reach it: the inset it reports is the one
  // the coverage ramp is read at, so a rim further in than it claims
  // would read the ramp short.
  EXPECT_NEAR(shallowest, ring.inset, 1e-2f);
}

}  // namespace testing
}  // namespace impeller
