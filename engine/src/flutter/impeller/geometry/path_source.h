// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_GEOMETRY_PATH_SOURCE_H_
#define FLUTTER_IMPELLER_GEOMETRY_PATH_SOURCE_H_

#include <functional>
#include <vector>

#include "impeller/geometry/point.h"
#include "impeller/geometry/rect.h"

namespace impeller {

enum class FillType {
  kNonZero,  // The default winding order.
  kOdd,
};

enum class Convexity {
  kUnknown,
  kConvex,
};

struct PathEdge {
  Point p0;
  Point p1;
  Point control;
  bool is_curve = false;
  bool starts_contour = false;
};

/// @brief   Collection of functions to receive path segments from the
///          underlying path representation via the DlPath::Dispatch method.
///
/// The conic_to function is optional. If the receiver understands rational
/// quadratic Bezier curve forms then it should accept the curve parameters
/// and return true, otherwise it can return false and the dispatcher will
/// provide the path segment in a different form via the other methods.
///
/// The dispatcher might not call the recommend_size or recommend_bounds
/// functions if the original path does not contain such information. If
/// it does call these functions then they should be called before any
/// path segments are dispatched.
///
/// The dispatcher will always call the path_info function, though the
/// is_convex parameter may be conservatively reported as false if the
/// original path does not contain such info.
///
/// Finally the dispatcher will always call the PathEnd function as the
/// last action before returning control to the method that called it.
class PathReceiver {
 public:
  virtual ~PathReceiver() = default;
  virtual void MoveTo(const Point& p2, bool will_be_closed) = 0;
  virtual void LineTo(const Point& p2) = 0;
  virtual void QuadTo(const Point& cp, const Point& p2) = 0;
  virtual bool ConicTo(const Point& cp, const Point& p2, Scalar weight) {
    return false;
  }
  virtual void CubicTo(const Point& cp1, const Point& cp2, const Point& p2) = 0;
  virtual void Close() = 0;
};

class PathSource {
 public:
  virtual ~PathSource() = default;
  virtual FillType GetFillType() const = 0;
  virtual Rect GetBounds() const = 0;
  virtual bool IsConvex() const = 0;
  virtual void Dispatch(PathReceiver& receiver) const = 0;
  virtual const std::vector<PathEdge>* GetEdges() const { return nullptr; }
};

/// @brief A PathSource object that provides path iteration for any TRect.
class RectPathSource : public PathSource {
 public:
  template <class T>
  explicit RectPathSource(const TRect<T>& r) : rect_(r) {}

  ~RectPathSource();

  // |PathSource|
  FillType GetFillType() const override;

  // |PathSource|
  Rect GetBounds() const override;

  // |PathSource|
  bool IsConvex() const override;

  // |PathSource|
  void Dispatch(PathReceiver& receiver) const override;

 private:
  const Rect rect_;
};

/// @brief A PathSource object that provides path iteration for any ellipse
///        inscribed within a Rect bounds.
class EllipsePathSource : public PathSource {
 public:
  explicit EllipsePathSource(const Rect& bounds);

  ~EllipsePathSource();

  // |PathSource|
  FillType GetFillType() const override;

  // |PathSource|
  Rect GetBounds() const override;

  // |PathSource|
  bool IsConvex() const override;

  // |PathSource|
  void Dispatch(PathReceiver& receiver) const override;

 private:
  const Rect bounds_;
};

/// A utility class to receive path segments from a source, transform them
/// by a matrix, and pass them along to a subsequent receiver.
class PathTransformer : public impeller::PathReceiver {
 public:
  PathTransformer(PathReceiver& receiver [[clang::lifetimebound]],
                  const impeller::Matrix& matrix [[clang::lifetimebound]])
      : receiver_(receiver), matrix_(matrix) {}

  void MoveTo(const Point& p2, bool will_be_closed) override {
    receiver_.MoveTo(matrix_ * p2, will_be_closed);
  }

  void LineTo(const Point& p2) override { receiver_.LineTo(matrix_ * p2); }

  void QuadTo(const Point& cp, const Point& p2) override {
    receiver_.QuadTo(matrix_ * cp, matrix_ * p2);
  }

  bool ConicTo(const Point& cp, const Point& p2, Scalar weight) override {
    return receiver_.ConicTo(matrix_ * cp, matrix_ * p2, weight);
  }

  void CubicTo(const Point& cp1, const Point& cp2, const Point& p2) override {
    receiver_.CubicTo(matrix_ * cp1, matrix_ * cp2, matrix_ * p2);
  }

  void Close() override { receiver_.Close(); }

 private:
  PathReceiver& receiver_;
  const impeller::Matrix& matrix_;
};

//------------------------------------------------------------------------------
/// Walks a path as chopped lines and quadratics, telling the callback
/// where each contour begins and ends.
///
/// Serves a convex consumer and a concave one alike: a convex one has
/// one contour and can ignore the flags, and asks IsSingleContour
/// whether that held.
class PathEdgeVisitor final : public PathReceiver {
 public:
  using PathCallback = std::function<
      void(const PathEdge&, bool starts_contour, bool closes_contour)>;

  /// A fill implicitly closes every contour, so sealing the ones the
  /// path left open is the default. A stroke must not: an open contour
  /// is exactly what gets caps rather than a joint. It is a property of
  /// the whole walk, not of one contour, so it is settled here.
  explicit PathEdgeVisitor(const PathCallback& cb, bool implicitly_close = true)
      : cb_(cb), implicitly_close_(implicitly_close) {}

  void MoveTo(const Point& point, bool will_be_closed) override {
    if (started_) {
      multiple_contours_ = true;
      // The contour being left never got a Close of its own, and every
      // contour a fill covers is closed. Only the last one used to be
      // sealed, which left the others a chord short.
      CloseContour();
    }
    started_ = true;
    starts_ = true;
    start_ = current_ = point;
  }

  void LineTo(const Point& point) override { AppendLine(point); }

  void QuadTo(const Point& control, const Point& point) override {
    PathEdge edge;
    edge.p0 = current_;
    edge.p1 = point;
    edge.is_curve = true;
    edge.control = control;
    edge.starts_contour = starts_;
    Emit(edge);
    current_ = edge.p1;
  }

  bool ConicTo(const Point& control,
               const Point& point,
               Scalar weight) override {
    AppendConicAsQuads(current_, control, point, weight);
    current_ = point;
    return true;
  }

  void CubicTo(const Point& control1,
               const Point& control2,
               const Point& point) override {
    AppendCubicAsQuads(current_, control1, control2, point);
    current_ = point;
  }

  /// The path closed this contour itself, which is not the implicit
  /// close and happens whatever this visitor was built for.
  void Close() override { CloseContour(); }

  void Finish() {
    if (implicitly_close_) {
      CloseContour();
    }
  }

  bool IsSingleContour() const { return started_ && !multiple_contours_; }

 private:
  /// Run the chain back to where the contour began. A contour already
  /// closed is already there, and AppendLine has nothing to add.
  void CloseContour() {
    closing_ = true;
    AppendLine(start_);
    closing_ = false;
  }

  Point CubicLerp(Point a, Point b, Scalar t) { return a + (b - a) * t; }

  Point ConicMidpoint(Point p0, Point control, Point p1, Scalar weight) {
    const Scalar denominator = 2 + 2 * weight;
    return Point((p0.x + 2 * weight * control.x + p1.x) / denominator,
                 (p0.y + 2 * weight * control.y + p1.y) / denominator);
  }

  void SplitCubic(Point& p0,
                  Point& control1,
                  Point& control2,
                  Point p1,
                  Scalar t,
                  Point out_left[4]) {
    const Point ab = CubicLerp(p0, control1, t);
    const Point bc = CubicLerp(control1, control2, t);
    const Point cd = CubicLerp(control2, p1, t);
    const Point abc = CubicLerp(ab, bc, t);
    const Point bcd = CubicLerp(bc, cd, t);
    const Point mid = CubicLerp(abc, bcd, t);
    out_left[0] = p0;
    out_left[1] = ab;
    out_left[2] = abc;
    out_left[3] = mid;
    p0 = mid;
    control1 = bcd;
    control2 = cd;
  }

  void AppendConicAsQuads(Point p0, Point control, Point p1, Scalar weight) {
    const Point conic_mid = ConicMidpoint(p0, control, p1, weight);
    const Point left_control =
        Point((p0.x + weight * control.x) / (1 + weight),
              (p0.y + weight * control.y) / (1 + weight));
    const Point right_control =
        Point((weight * control.x + p1.x) / (1 + weight),
              (weight * control.y + p1.y) / (1 + weight));
    PathEdge left;
    left.p0 = p0;
    left.p1 = conic_mid;
    left.is_curve = true;
    left.control = left_control;
    left.starts_contour = starts_;
    Emit(left);
    PathEdge right;
    right.p0 = conic_mid;
    right.p1 = p1;
    right.is_curve = true;
    right.control = right_control;
    right.starts_contour = starts_;
    Emit(right);
  }

  void AppendCubicAsQuads(Point p0, Point control1, Point control2, Point p1) {
    Point left[4];
    SplitCubic(p0, control1, control2, p1, 1.0f / 3.0f, left);
    EmitCubicSegmentAsQuad(left[0], left[1], left[2], left[3]);
    SplitCubic(p0, control1, control2, p1, 0.5f, left);
    EmitCubicSegmentAsQuad(left[0], left[1], left[2], left[3]);
    EmitCubicSegmentAsQuad(p0, control1, control2, p1);
  }

  void EmitCubicSegmentAsQuad(Point p0,
                              Point control1,
                              Point control2,
                              Point p1) {
    PathEdge edge;
    edge.p0 = p0;
    edge.p1 = p1;
    edge.is_curve = true;
    edge.control = Point((3 * (control1.x + control2.x) - p0.x - p1.x) / 4,
                         (3 * (control1.y + control2.y) - p0.y - p1.y) / 4);
    edge.starts_contour = starts_;
    Emit(edge);
  }

  void AppendLine(Point point) {
    if (point == current_) {
      return;
    }
    PathEdge edge;
    edge.p0 = current_;
    edge.p1 = point;
    edge.starts_contour = starts_;
    Emit(edge);
    current_ = point;
  }

  void Emit(const PathEdge& edge) {
    cb_(edge, starts_, closing_);
    starts_ = false;
  }

  const PathCallback cb_;
  const bool implicitly_close_ = true;
  Point start_;
  Point current_;
  bool started_ = false;
  bool multiple_contours_ = false;
  bool starts_ = true;
  bool closing_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_GEOMETRY_PATH_SOURCE_H_
