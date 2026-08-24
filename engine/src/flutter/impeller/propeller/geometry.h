// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_GEOMETRY_H_
#define FLUTTER_IMPELLER_PROPELLER_GEOMETRY_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "impeller/geometry/matrix.h"
#include "impeller/geometry/point.h"
#include "impeller/propeller/buffer_arena.h"
#include "impeller/propeller/gradient_atlas.h"
#include "impeller/propeller/path_geometry.h"
#include "impeller/propeller/picture.h"
#include "impeller/propeller/text_materializer.h"

namespace impeller {

/// Low 24 bits of an Attributes::paint word index the paint stream, high
/// 8 bits carry the path implicit class the vertex stage reads.
static constexpr uint32_t kPaintIndexMask = 0x00FFFFFF;
static constexpr uint32_t kPaintFlagsShift = 24;

constexpr uint32_t PackPaint(uint32_t paint_index, uint8_t flags = 0) {
  return (paint_index & kPaintIndexMask) |
         (static_cast<uint32_t>(flags) << kPaintFlagsShift);
}

/// The Loop-Blinn implicit a vertex carries, as the class the vertex
/// stage looks the canonical coordinate up by. Bits 0-2 of the paint
/// word's high byte, and one of these five: they are one to one with
/// kPathImplicitCoords in vertex.vert, which is as long as this list.
static constexpr uint8_t kPathImplicitCurveP0 = 0;       // (0, 0)
static constexpr uint8_t kPathImplicitCurveControl = 1;  // (0.5, 0)
static constexpr uint8_t kPathImplicitCurveP1 = 2;       // (1, 1)
static constexpr uint8_t kPathImplicitInterior = 3;      // (0, 1)
static constexpr uint8_t kPathImplicitFringeOuter = 4;   // (0, -1)
/// How far the fringe at a curve's pinched end runs along the tangent,
/// in device pixels. The tangent leaves the curve, so what the strip
/// carries is only right where the curve's own triangle has pinched out
/// -- a run of any real length paints half coverage outside the shape.
static constexpr Scalar kTangentPixels = 2.0f;

//------------------------------------------------------------------------------
/// Scratch for a generator whose output has to be produced before it can
/// be counted: it writes the mesh here once, and copies it out when the
/// room for it has been reserved.
struct GeometryStaging {
  std::vector<Point> positions;
  std::vector<Attributes> attributes;

  void Clear() {
    positions.clear();
    attributes.clear();
  }

  uint32_t GetVertexCount() const {
    return static_cast<uint32_t>(positions.size());
  }
};

//------------------------------------------------------------------------------
/// Writes the Loop-Blinn mesh of one convex contour, one edge at a time.
/// A joint needs the edges either side of it, so the previous edge and
/// the first are held until Finish closes the ring.
class ConvexMeshEmitter {
 public:
  /// `scale` is what the transform the draw lands under does to a local
  /// unit, so the fringe can be one device pixel wide wherever the shape
  /// comes out. The mesh goes to `staging`, which is emptied here: how
  /// many vertices it comes to is only known once it is all there.
  ConvexMeshEmitter(Point centroid,
                    Scalar scale,
                    uint32_t color,
                    GeometryStaging& staging);

  void Edge(const PathEdge& edge);

  void Finish();

  uint32_t GetVertexCount() const { return staging_.GetVertexCount(); }

 private:
  void PushVertex(Point position, uint8_t implicit_class);

  /// The unit normal at `at` pointing away from the centroid.
  Point OutwardNormal(Point at, Point along) const;

  /// The fringe either side of a curve, running out along its tangent.
  void TangentStrip(Point at, Point toward);

  /// The wedge of fringe bridging two edges' outward normals.
  void Joint(const PathEdge& prev, const PathEdge& next);

  const Point centroid_;
  /// A device pixel in local units: what an outward normal is scaled by
  /// to reach one pixel past the boundary.
  const Scalar fringe_;
  const uint32_t color_;
  GeometryStaging& staging_;

  PathEdge first_;
  PathEdge prev_;
  bool started_ = false;
};

//------------------------------------------------------------------------------
/// What a generator needs from the frame rather than from the draw.
struct GeometryContext {
  TextMaterializer* text = nullptr;
  GPUContext* context = nullptr;
  GPUTexture* shadow_lut = nullptr;
  GradientAtlas* gradient_atlas = nullptr;
  std::vector<GPUTexture*>* textures = nullptr;
};

//------------------------------------------------------------------------------
/// Turns one recorded draw into the vertices, indices and paint the
/// shaders read.
class GeometryGenerator {
 public:
  virtual ~GeometryGenerator() = 0;

  /// The vertices and indices, in that order, that `draw` will write.
  virtual std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) = 0;

  /// Write the draw into the room reserved for it. Called for a draw
  /// right after GetAllocationCount for that same draw, which is what
  /// lets a generator that had to stage its output copy it from there.
  ///
  /// All positions are local.
  /// `index_start` is the allocation's first vertex.
  /// `paint_index` is the slot `paint_out` was reserved at.
  virtual void Generate(const PrPicture& picture,
                        const Draw& draw,
                        const Matrix& matrix,
                        Point* position_out,
                        Attributes* attributes_out,
                        uint16_t* index_out,
                        PrPaint* paint_out,
                        uint16_t index_start,
                        uint32_t paint_index,
                        const GeometryContext& frame) = 0;
};

inline GeometryGenerator::~GeometryGenerator() = default;

//------------------------------------------------------------------------------
/// Generates geometry for a point cloud.
class DrawPointsGeometryGenerator final : public GeometryGenerator {
 public:
  ~DrawPointsGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override {
    // 4 corners for each circle regardless of size.
    return {draw.point_data.length * 4, draw.point_data.length * 6};
  }

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;
};

/// A draw type nothing draws yet.
class NoOpGeometryGenerator final : public GeometryGenerator {
 public:
  ~NoOpGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override {
    return {0, 0};
  }

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override {}
};

/// Glyph quads: one per glyph, positioned and uv'd by the materializer,
/// which rasterizes and queues an atlas upload for anything missing.
class TextGeometryGenerator final : public GeometryGenerator {
 public:
  ~TextGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override;

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;
};

//------------------------------------------------------------------------------
/// A round rect off a static table.
class RRectGeometryGenerator final : public GeometryGenerator {
 public:
  ~RRectGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override;

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;
};

//------------------------------------------------------------------------------
/// How much of a shadow's band is drawn.
///
/// The band runs from a blur radius inside the silhouette (full
/// coverage) through the silhouette itself (half) to a blur radius
/// outside (none). What the modes differ in is where the interior fan
/// stops and which stretch of the band the ring spans.
enum class ShadowInterior {
  /// Fan and the whole ring: a transparent occluder, which the shadow
  /// shows through.
  kFull,
  /// The ring from its inner rim outward, no fan: an opaque occluder's
  /// spot shadow, whose interior the occluder covers anyway.
  kRingOnly,
  /// The ring from the silhouette outward: an opaque occluder's ambient
  /// shadow.
  kFromSilhouette,
};

//------------------------------------------------------------------------------
/// A shadow: its ambient and spot halves, both out of one draw.
///
/// The two differ in everything but the shape -- the spot is cast from
/// the silhouette shifted by the elevation, blurred wider, and darker --
/// so they are two meshes, sampling the Gaussian LUT as coverage the
/// way glyphs sample the atlas.
class ShadowGeometryGenerator final : public GeometryGenerator {
 public:
  ~ShadowGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override;

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;

 private:
  ShadowRing ring_;
  std::vector<Point> silhouette_;
};

//------------------------------------------------------------------------------
/// Sprites cut out of one image: a quad each, placed by its own
/// transform and sampling its own part of the image.
///
/// TODO: the blend between a sprite's colour and the image, which is
/// taken to be a modulate here.
class AtlasGeometryGenerator final : public GeometryGenerator {
 public:
  ~AtlasGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override;

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;
};

//------------------------------------------------------------------------------
/// A vertex buffer the recording was handed, copied out as it is.
///
/// The positions, and the texture coordinates and colours where the
/// vertices carry them; a vertex with no colour of its own is drawn in
/// the paint's. Indices come across too, rebased onto the allocation.
///
/// TODO: the vertex mode, which is taken to be triangles here, and the
/// blend between the vertices' colours and the paint's.
class VerticesGeometryGenerator final : public GeometryGenerator {
 public:
  ~VerticesGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override;

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;
};

//------------------------------------------------------------------------------
/// Any filled concave or self intersecting path.
class ConcavePathGeometryGenerator final : public GeometryGenerator {
 public:
  ~ConcavePathGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override;

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;

 private:
  /// The mesh, counted by walking the contours and kept for the write
  /// that follows.
  GeometryStaging staging_;
};

//------------------------------------------------------------------------------
/// Any filled convex contour, by walking it.
class ConvexPathGeometryGenerator final : public GeometryGenerator {
 public:
  ~ConvexPathGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override;

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;

 private:
  GeometryStaging staging_;
};

class RectGeometryGenerator final : public GeometryGenerator {
 public:
  ~RectGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override;

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;
};

// Texture mapped to a rectangle geometry.
class ImageRectGeometryGenerator final : public GeometryGenerator {
 public:
  ~ImageRectGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override {
    return {4, 6};
  }

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override;
};

class NoopGeometryGenerator final : public GeometryGenerator {
 public:
  ~NoopGeometryGenerator() override = default;

  std::pair<uint32_t, uint32_t> GetAllocationCount(
      const PrPicture& picture,
      const Draw& draw,
      const Matrix& matrix) override {
    return {0, 0};
  }

  void Generate(const PrPicture& picture,
                const Draw& draw,
                const Matrix& matrix,
                Point* position_out,
                Attributes* attributes_out,
                uint16_t* index_out,
                PrPaint* paint_out,
                uint16_t index_start,
                uint32_t paint_index,
                const GeometryContext& frame) override {}
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_GEOMETRY_H_
