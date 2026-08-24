// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_DISPLAY_LIST_GEOMETRY_DL_PATH_H_
#define FLUTTER_DISPLAY_LIST_GEOMETRY_DL_PATH_H_

#include <cstdint>
#include <functional>
#include <memory>

#include "flutter/display_list/geometry/dl_geometry_types.h"
#include "flutter/impeller/geometry/path_source.h"
#include "flutter/third_party/skia/include/core/SkPath.h"

namespace flutter {

using DlPathFillType = impeller::FillType;
using DlPathReceiver = impeller::PathReceiver;

class DlPath : public impeller::PathSource {
 public:
  static constexpr uint32_t kMaxVolatileUses = 2;

  /// What a stroked outline was built for. The caps and joins are the
  /// DlStrokeCap and DlStrokeJoin the paint carried, as their own
  /// values: this header cannot see dl_paint.h.
  struct StrokeKey {
    DlScalar width = 0;
    DlScalar miter_limit = 0;
    uint8_t cap = 0;
    uint8_t join = 0;

    bool operator==(const StrokeKey& other) const = default;
  };

  /// This path's stroked outline, if the one it holds was stroked with
  /// `key`, and nullptr otherwise.
  ///
  /// One slot only: a path is stroked the same way on every frame it is
  /// drawn, so a second would never be read. The stroking itself
  /// belongs to the renderer; this only remembers the result.
  const std::shared_ptr<DlPath>& GetStroked(const StrokeKey& key) const;

  /// Hold `stroked` as this path's outline under `key`, replacing
  /// whatever was there.
  void SetStroked(const StrokeKey& key, std::shared_ptr<DlPath> stroked) const;

  static DlPath MakeRect(const DlRect& rect);
  static DlPath MakeRectLTRB(DlScalar left,
                             DlScalar top,
                             DlScalar right,
                             DlScalar bottom);
  static DlPath MakeRectXYWH(DlScalar x,
                             DlScalar y,
                             DlScalar width,
                             DlScalar height);

  static DlPath MakeOval(const DlRect& bounds);
  static DlPath MakeOvalLTRB(DlScalar left,
                             DlScalar top,
                             DlScalar right,
                             DlScalar bottom);

  static DlPath MakeCircle(const DlPoint center, DlScalar radius);

  static DlPath MakeRoundRect(const DlRoundRect& rrect);
  static DlPath MakeRoundRectXY(const DlRect& rect,
                                DlScalar x_radius,
                                DlScalar y_radius,
                                bool counter_clock_wise = false);
  static DlPath MakeRoundSuperellipse(const DlRoundSuperellipse& rse);

  static DlPath MakeLine(const DlPoint a, const DlPoint b);
  static DlPath MakePoly(const DlPoint pts[],
                         int count,
                         bool close,
                         DlPathFillType fill_type = DlPathFillType::kNonZero);

  static DlPath MakeArc(const DlRect& bounds,
                        DlDegrees start,
                        DlDegrees sweep,
                        bool use_center);

  DlPath();
  explicit DlPath(const SkPath& path);

  DlPath(const DlPath& path) = default;
  DlPath(DlPath&& path) = default;
  DlPath& operator=(const DlPath&) = default;

  ~DlPath() override = default;

  const SkPath& GetSkPath() const;

  void Dispatch(DlPathReceiver& receiver) const override;

  /// Intent to render an SkPath multiple times will make the path
  /// non-volatile to enable caching in Skia. Calling this method
  /// before every rendering call that uses the SkPath will count
  /// down the uses and eventually reset the volatile flag.
  ///
  /// @see |kMaxVolatileUses|
  void WillRenderSkPath() const;

  [[nodiscard]] DlPath WithOffset(const DlPoint offset) const;
  [[nodiscard]] DlPath WithFillType(DlPathFillType type) const;

  bool IsEmpty() const;
  bool IsRect(DlRect* rect = nullptr, bool* is_closed = nullptr) const;
  bool IsOval(DlRect* bounds = nullptr) const;
  bool IsLine(DlPoint* start = nullptr, DlPoint* end = nullptr) const;
  bool IsRoundRect(DlRoundRect* rrect = nullptr) const;

  bool Contains(const DlPoint point) const;

  DlRect GetBounds() const override;
  DlPathFillType GetFillType() const override;

  bool operator==(const DlPath& other) const;

  bool IsVolatile() const;
  bool IsConvex() const override;

  const std::vector<impeller::PathEdge>* GetEdges() const override;
  void SetEdges(std::vector<impeller::PathEdge> edges) const;

  DlPath operator+(const DlPath& other) const;

 private:
  struct Data {
    explicit Data(const SkPath& path) : sk_path(path) {
      FML_DCHECK(!SkPathFillType_IsInverse(path.getFillType()));
    }

    SkPath sk_path;
    uint32_t render_count = 0u;

    /// The outline of a stroke of this path, and what it was stroked
    /// with. One only: see GetStroked.
    StrokeKey stroke_key;
    std::shared_ptr<DlPath> stroked;

    /// The flattened edges (lines and quads) of this path, used by Propeller
    /// to avoid re-evaluating curves and verbs at draw time.
    std::vector<impeller::PathEdge> edges;
    bool has_edges = false;
  };

  std::shared_ptr<Data> data_;

  static void ReduceConic(DlPathReceiver& receiver,
                          const DlPoint& p1,
                          const DlPoint& cp,
                          const DlPoint& p2,
                          DlScalar weight);
};

}  // namespace flutter

#endif  // FLUTTER_DISPLAY_LIST_GEOMETRY_DL_PATH_H_
