// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/dispatcher.h"

#include <cmath>
#include <iterator>
#include <limits>

#include "flutter/display_list/effects/image_filters/dl_matrix_image_filter.h"
#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {

namespace {

NoOpGeometryGenerator g_noop;
RectGeometryGenerator g_rect;
RRectGeometryGenerator g_rrect;
ConvexPathGeometryGenerator g_convex_path;
DrawPointsGeometryGenerator g_points;
TextGeometryGenerator g_text;
ImageRectGeometryGenerator g_image_rect;
ConcavePathGeometryGenerator g_concave_path;
VerticesGeometryGenerator g_vertices;
AtlasGeometryGenerator g_atlas;
ShadowGeometryGenerator g_shadow;

/// Indexed by Draw::DrawType.
GeometryGenerator* const kGeometryGenerators[] = {
    &g_rect,          // kRect
    &g_rrect,         // kRRect
    &g_points,        // kPoints
    &g_rect,          // kLayer
    &g_rect,          // kRectClip
    &g_rrect,         // kRRectClip
    &g_text,          // kText
    &g_image_rect,    // kImageRect
    &g_convex_path,   // kConvexFillPath
    &g_convex_path,   // kConvexClipPath
    &g_concave_path,  // kConcaveClipPath
    &g_rect,          // kClipResolveNonZero
    &g_rect,          // kClipResolveEvenOdd
    &g_concave_path,  // kConcaveWindingAccumulate
    &g_rect,          // kConcaveWindingResolveNonZero
    &g_rect,          // kConcaveWindingResolveEvenOdd
    &g_vertices,      // kDrawVertices
    &g_atlas,         // kDrawAtlas
    &g_shadow,        // kShadow
    &g_rect,          // kClipReset
    &g_rect,          // kScissor
};

/// Pipeline to draw type.
constexpr ProgramType kDrawPrograms[] = {
    ProgramType::kColor,                  // kRect
    ProgramType::kPath,                   // kRRect
    ProgramType::kColor,                  // kPoints
    ProgramType::kInvalid,                // kLayer
    ProgramType::kWindingAccumulateFlat,  // kRectClip
    ProgramType::kWindingAccumulate,      // kRRectClip
    ProgramType::kColor,                  // kText
    ProgramType::kColor,                  // kImageRect
    ProgramType::kPath,                   // kConvexFillPath
    ProgramType::kWindingAccumulate,      // kConvexClipPath
    ProgramType::kWindingAccumulate,      // kConcaveClipPath
    ProgramType::kClipResolveNonZero,     // kClipResolveNonZero
    ProgramType::kClipResolveEvenOdd,     // kClipResolveEvenOdd
    ProgramType::kWindingAccumulate,      // kConcaveWindingAccumulate
    ProgramType::kWindingResolveNonZero,  // kConcaveWindingResolveNonZero
    ProgramType::kWindingResolveEvenOdd,  // kConcaveWindingResolveEvenOdd
    ProgramType::kColor,                  // kDrawVertices
    ProgramType::kColor,                  // kDrawAtlas
    ProgramType::kColor,                  // kShadow
    ProgramType::kClipReset,              // kClipReset
    ProgramType::kInvalid,                // kScissor
};

static_assert(std::size(kGeometryGenerators) ==
              static_cast<size_t>(Draw::DrawType::kScissor) + 1);
static_assert(std::size(kDrawPrograms) == std::size(kGeometryGenerators));

void BindStreams(GpuCommandBuffer& cmd_buffer, const BufferBinds& binds) {
  cmd_buffer.SetBuffer(GPUShaderStage::kVertex, *binds.positions, 0, 0);
  cmd_buffer.SetBuffer(GPUShaderStage::kVertex, *binds.attributes, 0, 1);
  cmd_buffer.SetBuffer(GPUShaderStage::kVertex, *binds.paints, 0, 2);
  cmd_buffer.SetBuffer(GPUShaderStage::kVertex, *binds.transforms, 0, 4);
  cmd_buffer.SetBuffer(GPUShaderStage::kFragment, *binds.paints, 0, 0);
  cmd_buffer.SetBuffer(GPUShaderStage::kFragment, *binds.gradients, 0, 2);
}

/// Whether the draw at `index` paints the whole pass one flat colour.
bool CoversPass(const PrPicture& picture,
                size_t index,
                const Rect& pass,
                const Matrix& placement,
                const std::vector<Matrix>& placed) {
  // This is an extremely conservative check. Instead we should add a similar
  // type table/vtable method to compute coverage. Impeller has some existing
  // examples: round rects we can check the interior rect, and we can always
  // transform to conservative interior coverage.
  // We should also check if we're about to draw a scissor which completely
  // covers the rect.
  const Draw& draw = picture.GetDraws()[index];
  if (draw.gradient != Draw::kNoIndex) {
    return false;
  }
  if (draw.type != Draw::DrawType::kRect) {
    return false;
  }
  bool covers =
      placed[draw.transform].IsAligned2D() &&
      picture.GetBounds()[index].TransformBounds(placement).Contains(pass);
  return covers;
}

/// Where `root` sits in the pass, as the scissor takes it.
///
/// Rounded to nearest, which is where a hard edge rasterizes: a pixel
/// is covered when its centre is inside. Rounding out would give two
/// clips that meet at the same fractional edge the pixel they share,
/// and rounding in would give it to neither.
IRect32 DeviceScissor(const Rect& root, const RenderPlan& plan) {
  const IRect32 extent = IRect32::MakeWH(static_cast<int32_t>(plan.width),
                                         static_cast<int32_t>(plan.height));
  return IRect32::Round(root.Shift(-plan.origin)).IntersectionOrEmpty(extent);
}

/// What an item is bounded by: the pass, narrowed by the clip the scene
/// applied to it.
Rect Bound(const Rect& pass, const std::optional<Rect>& clip) {
  return clip.has_value() ? pass.IntersectionOrEmpty(*clip) : pass;
}

/// Whether a program paints into the pass's target, as against only
/// setting state or writing an attachment that does not outlive the
/// pass.
bool WritesTarget(ProgramType program) {
  switch (program) {
    case ProgramType::kInvalid:  // A scissor draw, which draws nothing.
    case ProgramType::kWindingAccumulate:
    case ProgramType::kWindingAccumulateFlat:
    case ProgramType::kClipResolveNonZero:
    case ProgramType::kClipResolveEvenOdd:
    case ProgramType::kClipReset:
      return false;
    case ProgramType::kColor:
    case ProgramType::kPath:
    case ProgramType::kWindingResolveNonZero:
    case ProgramType::kWindingResolveEvenOdd:
    case ProgramType::kGradientRamp:
    case ProgramType::kProgramLength:
      break;
  }
  return true;
}

/// What a group's filter does to the pass it resolves into. A matrix
/// filter's matrix is in the space of the saveLayer that took it, so
/// in root space it is that matrix conjugated by the transform there.
Matrix CompositeTransform(const PrSceneNode& group) {
  const flutter::DlMatrixImageFilter* matrix =
      group.image_filter ? group.image_filter->asMatrix() : nullptr;
  if (matrix == nullptr || !group.transform.IsInvertible()) {
    return Matrix();
  }
  return group.transform * matrix->matrix() * group.transform.Invert();
}

/// Whether what samples the pass takes the nearest texel. Nearest and
/// linear are all there is; everything else is linear.
bool CompositeNearest(const PrSceneNode& group) {
  const flutter::DlMatrixImageFilter* matrix =
      group.image_filter ? group.image_filter->asMatrix() : nullptr;
  return matrix != nullptr &&
         matrix->sampling() == flutter::DlImageSampling::kNearestNeighbor;
}

std::optional<Rect> Narrow(const std::optional<Rect>& clip,
                           const std::optional<Rect>& by) {
  if (!by.has_value()) {
    return clip;
  }
  return clip.has_value() ? clip->Intersection(*by) : by;
}

/// Draw a composite operation encoded by the scene hierarchy.
void EmitComposite(RenderPlan& plan,
                   BufferArena& arena,
                   const SceneFlattener::Pass& pass,
                   uint32_t slot) {
  BufferArena::Result result = arena.ReserveAllocation(4, 6);
  if (!result.IsValid()) {
    return;
  }

  // The quad is what the pass covers; a filter that moves it is the
  // transform the quad is drawn under.
  *result.transform_out = pass.composite_transform;
  *result.paint_out = PrPaint{
      .texture_index = static_cast<int32_t>(slot),
      .transform_index = static_cast<int32_t>(result.transform_start),
      .flags = pass.composite_nearest ? kPaintFlagSampleNearest : 0u,
  };

  const Rect& coverage = pass.coverage;
  result.position_out[0] = coverage.GetLeftTop();
  result.position_out[1] = coverage.GetLeftBottom();
  result.position_out[2] = coverage.GetRightTop();
  result.position_out[3] = coverage.GetRightBottom();

  constexpr Point kFullTexture[4] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
  const uint32_t color =
      flutter::DlColor::kWhite().withAlphaF(pass.opacity).premultipliedRGBA();
  const uint32_t paint = PackPaint(result.paint_start);
  for (int corner = 0; corner < 4; corner++) {
    result.attributes_out[corner] = Attributes{
        .uv = kFullTexture[corner],
        .color = color,
        .paint = paint,
    };
  }

  constexpr int kQuadCornerOrder[6] = {0, 1, 2, 1, 2, 3};
  for (int i = 0; i < 6; i++) {
    result.index_out[i] =
        static_cast<uint16_t>(result.vertex_start + kQuadCornerOrder[i]);
  }

  if (result.new_buffer || plan.buffers.empty()) {
    plan.buffers.push_back(arena.GetBinds());
  }
  const auto binds = static_cast<uint32_t>(plan.buffers.size() - 1);
  if (!plan.draws.empty()) {
    GPUDraw& previous = plan.draws.back();
    if (previous.program == ProgramType::kColor &&
        previous.buffer_binds == binds) {
      previous.count += 6;
      return;
    }
  }
  plan.draws.push_back(GPUDraw{
      .program = ProgramType::kColor,
      .start = result.index_start,
      .count = 6,
      .buffer_binds = binds,
  });
}

}  // namespace

/////// Texture Cache

TextureCache::TextureCache(GPUContext* context) : context_(context) {}

bool TextureCache::OffscreenKey::operator==(const OffscreenKey& other) const {
  for (size_t i = 0; i < kMaxPictures; i++) {
    if (ids[i] != other.ids[i]) {
      return false;
    }
  }
  return width == other.width && height == other.height;
}

bool TextureCache::Placement::operator==(const Placement& other) const {
  for (int i = 0; i < 4; i++) {
    if (basis[i] != other.basis[i]) {
      return false;
    }
  }
  return offset == other.offset;
}

GPUTexture* TextureCache::AllocateWinding(uint32_t width, uint32_t height) {
  return AllocateTransient(width, height, TextureFormat::kR16Float);
}

GPUTexture* TextureCache::AllocateClip(uint32_t width, uint32_t height) {
  return AllocateTransient(width, height, TextureFormat::kR8UNorm);
}

GPUTexture* TextureCache::AllocateTransient(uint32_t width,
                                            uint32_t height,
                                            TextureFormat format) {
  for (auto& cached : transients_) {
    if (cached.width == width && cached.height == height &&
        cached.format == format) {
      cached.used_this_frame = true;
      return cached.texture.get();
    }
  }
  // Nothing like it is live, but one may be waiting to be dropped.
  for (auto it = pending_deletion_.begin(); it != pending_deletion_.end();
       ++it) {
    if (it->width != width || it->height != height || it->format != format) {
      continue;
    }
    transients_.push_back(std::move(*it));
    pending_deletion_.erase(it);
    transients_.back().used_this_frame = true;
    return transients_.back().texture.get();
  }

  std::unique_ptr<GPUTexture> texture = context_->CreateTexture(
      TextureDesc{
          .format = format,
          .width = width,
          .height = height,
          .transient = true,
      },
      false);
  if (texture == nullptr) {
    return nullptr;
  }
  transients_.push_back(CachedTransient{
      .width = width,
      .height = height,
      .format = format,
      .used_this_frame = true,
      .texture = std::move(texture),
  });
  return transients_.back().texture.get();
}

GPUTexture* TextureCache::FindOffscreen(
    const OffscreenKey& key,
    const std::vector<Placement>& placements,
    TextureFormat format) {
  if (!key.IsCacheable()) {
    return nullptr;
  }
  for (CachedOffscreen& cached : offscreens_) {
    if (cached.key == key && cached.format == format &&
        cached.placements == placements) {
      cached.used_this_frame = true;
      return cached.texture.get();
    }
  }
  return nullptr;
}

GPUTexture* TextureCache::CreateOffscreen(
    const OffscreenKey& key,
    const std::vector<Placement>& placements,
    TextureFormat format) {
  CachedOffscreen* taken = nullptr;
  for (CachedOffscreen& candidate : offscreens_) {
    if (!candidate.used_this_frame && candidate.format == format &&
        candidate.key.width == key.width &&
        candidate.key.height == key.height) {
      taken = &candidate;
      break;
    }
  }

  if (taken == nullptr) {
    for (auto it = offscreens_pending_deletion_.begin();
         it != offscreens_pending_deletion_.end(); ++it) {
      if (it->format != format || it->key.width != key.width ||
          it->key.height != key.height) {
        continue;
      }
      offscreens_.push_back(std::move(*it));
      offscreens_pending_deletion_.erase(it);
      taken = &offscreens_.back();
      break;
    }
  }

  if (taken == nullptr) {
    std::unique_ptr<GPUTexture> texture = context_->CreateTexture(
        TextureDesc{
            .format = format,
            .width = key.width,
            .height = key.height,
        },
        false);
    if (texture == nullptr) {
      return nullptr;
    }
    offscreens_.push_back(CachedOffscreen{.texture = std::move(texture)});
    taken = &offscreens_.back();
  }

  taken->key = key;
  taken->placements = placements;
  taken->format = format;
  taken->used_this_frame = true;
  return taken->texture.get();
}

void TextureCache::Next() {
  // Remove unused last two frames.
  pending_deletion_.clear();
  offscreens_pending_deletion_.clear();

  // What this frame did not want is kept one frame longer, in case the
  // next one wants a texture that size.
  auto retire = [](auto& live, auto& pending) {
    for (auto it = live.begin(); it != live.end();) {
      if (it->used_this_frame) {
        it->used_this_frame = false;
        ++it;
        continue;
      }
      pending.push_back(std::move(*it));
      it = live.erase(it);
    }
  };
  retire(transients_, pending_deletion_);
  retire(offscreens_, offscreens_pending_deletion_);
}

/////// Scene Flattener

void SceneFlattener::EncodePasses(BufferArena& arena,
                                  TextureCache& cache,
                                  TextureFormat format,
                                  TextMaterializer* text,
                                  GPUTexture* shadow_lut,
                                  GradientAtlas* gradients) {
  render_plan_.clear();
  render_plan_.resize(passes_.size());

  // For each render pass, see if there is a cached texture we can recycle.
  // If not, allocate an offscreen texture for it.
  for (size_t i = 0; i + 1 < passes_.size(); i++) {
    passes_[i].texture =
        cache.FindOffscreen(passes_[i].key, passes_[i].placements, format);
    passes_[i].ready = passes_[i].texture != nullptr;
  }
  for (size_t i = 0; i + 1 < passes_.size(); i++) {
    if (!passes_[i].ready) {
      passes_[i].texture =
          cache.CreateOffscreen(passes_[i].key, passes_[i].placements, format);
    }
  }

  for (size_t i = 0; i < passes_.size(); i++) {
    Pass& pass = passes_[i];
    RenderPlan& plan = render_plan_[i];
    if (pass.ready) {
      // Cache hit, nothing needs to be drawn.
      continue;
    }

    plan.origin = pass.coverage.GetOrigin();
    plan.width = static_cast<uint32_t>(std::ceil(pass.coverage.GetWidth()));
    plan.height = static_cast<uint32_t>(std::ceil(pass.coverage.GetHeight()));
    plan.textures.resize(kReservedTextureSlots, nullptr);

    // Shrink the clip rect by parent scene clips via intersection.
    const Rect extent = Rect::MakeXYWH(plan.origin.x, plan.origin.y,
                                       static_cast<Scalar>(plan.width),
                                       static_cast<Scalar>(plan.height));
    Rect current_bound = extent;

    for (const Item& item : pass.items) {
      const Rect bound = Bound(extent, item.clip);
      if (bound != current_bound) {
        current_bound = bound;
        plan.draws.push_back(GPUDraw{
            .program = ProgramType::kInvalid,
            .scissor = DeviceScissor(bound, plan),
        });
      }

      if (!item.IsComposite()) {
        const GeometryContext frame{
            .text = text,
            .context = cache.GetContext(),
            .shadow_lut = shadow_lut,
            .gradient_atlas = gradients,
            .textures = &plan.textures,
        };
        FlattenPicture(*item.picture, plan, arena, frame, item.transform,
                       item.clip, item.layer_passes);
        continue;
      }
      Pass& child = passes_[item.pass];
      child.texture_slot = static_cast<uint32_t>(plan.textures.size());
      plan.textures.push_back(child.texture);
      EmitComposite(plan, arena, child, child.texture_slot);
    }

    // Nothing after them reads what they set, and what they write does
    // not outlive the pass.
    while (!plan.draws.empty() && !WritesTarget(plan.draws.back().program)) {
      plan.draws.pop_back();
    }
  }

  if (text != nullptr) {
    for (RenderPlan& plan : render_plan_) {
      if (plan.textures.size() > 0) {
        plan.textures[kGlyphAtlasTextureSlot] = text->GetAtlasTexture();
      }
    }
  }
  if (gradients != nullptr) {
    for (RenderPlan& plan : render_plan_) {
      if (plan.textures.size() > kGradientTextureSlot) {
        plan.textures[kGradientTextureSlot] = gradients->GetTexture();
      }
    }
  }
}

void SceneFlattener::Adopt(const std::vector<Item>& items, uint32_t index) {
  for (const Item& item : items) {
    if (item.IsComposite()) {
      passes_[item.pass].parent = index;
      continue;
    }
    for (uint32_t pass : item.layer_passes) {
      if (pass != kNoPass) {
        passes_[pass].parent = index;
      }
    }
  }
}

void SceneFlattener::OpenLayers(const PrPicture& picture,
                                const Matrix& transform,
                                const std::optional<Rect>& clip,
                                std::vector<uint32_t>& passes) {
  for (const Draw& draw : picture.GetDraws()) {
    if (draw.type != Draw::DrawType::kLayer) {
      continue;
    }
    // The group's transform is the one the saveLayer was taken under,
    // which is what a matrix filter is conjugated by. It is not where
    // the layer is placed: its content is already in the holding
    // picture's space, so the leaf keeps that.
    PrSceneNode group;
    group.transform = transform * picture.GetTransforms()[draw.transform];
    group.clip = clip;
    group.opacity = draw.color.getAlphaF();
    if (draw.layer.image_filter_index != Draw::kNoIndex) {
      group.image_filter =
          picture.GetImageFilters()[draw.layer.image_filter_index];
    }
    if (draw.layer.color_filter_index != Draw::kNoIndex) {
      group.color_filter =
          picture.GetColorFilters()[draw.layer.color_filter_index];
    }

    PrSceneNode leaf;
    leaf.picture = picture.GetPictures()[draw.layer.picture_index];
    leaf.transform = transform;
    group.children.push_back(std::move(leaf));

    passes.push_back(Open(group, clip));
  }
}

void SceneFlattener::Gather(const PrSceneNode& node,
                            const std::optional<Rect>& inherited,
                            std::vector<Item>& items) {
  const std::optional<Rect> clip = Narrow(inherited, node.clip);
  if (node.clip.has_value() && !clip.has_value()) {
    return;  // Nothing of the node is left visible.
  }

  if (node.IsLeaf()) {
    const std::optional<Rect>& bounds = node.picture->GetBoundsUnion();
    if (!bounds.has_value()) {
      return;
    }
    Rect coverage = bounds->TransformBounds(node.transform);
    if (clip.has_value()) {
      coverage = coverage.IntersectionOrEmpty(*clip);
    }
    if (coverage.IsEmpty()) {
      return;
    }
    Item item{
        .picture = node.picture,
        .transform = node.transform,
        .clip = clip,
        .coverage = coverage,
    };
    OpenLayers(*node.picture, node.transform, clip, item.layer_passes);
    items.push_back(std::move(item));
    return;
  }

  GatherChildren(node, clip, items);
}

void SceneFlattener::GatherChildren(const PrSceneNode& node,
                                    const std::optional<Rect>& clip,
                                    std::vector<Item>& items) {
  for (const PrSceneNode& child : node.children) {
    if (!child.NeedsComposite()) {
      Gather(child, clip, items);
      continue;
    }
    const uint32_t pass = Open(child, clip);
    if (pass == kNoPass) {
      continue;
    }
    // Where the pass lands, which a filter may move; what it holds is
    // its own coverage.
    items.push_back(Item{
        .pass = pass,
        .coverage = passes_[pass].coverage.TransformBounds(
            passes_[pass].composite_transform),
    });
  }
}

void SceneFlattener::KeyPass(Pass& pass) {
  pass.key.width = static_cast<uint32_t>(std::ceil(pass.coverage.GetWidth()));
  pass.key.height = static_cast<uint32_t>(std::ceil(pass.coverage.GetHeight()));
  if (pass.items.size() > TextureCache::OffscreenKey::kMaxPictures) {
    return;  // More pictures than the key has room to name.
  }

  const Point origin = pass.coverage.GetOrigin();
  for (size_t i = 0; i < pass.items.size(); i++) {
    const Item& item = pass.items[i];
    if (item.IsComposite()) {
      // What another pass resolved into is not something this one can
      // be keyed by: it is that pass's business whether it changed.
      pass.key = TextureCache::OffscreenKey{.width = pass.key.width,
                                            .height = pass.key.height};
      pass.placements.clear();
      return;
    }
    pass.key.ids[i] = item.picture->GetId();
    const Matrix& transform = item.transform;
    pass.placements.push_back(TextureCache::Placement{
        .basis = {transform.m[0], transform.m[1], transform.m[4],
                  transform.m[5]},
        .offset = Point(transform.m[12], transform.m[13]) - origin,
    });
  }
}

uint32_t SceneFlattener::Open(const PrSceneNode& group,
                              const std::optional<Rect>& clip) {
  std::optional<Rect> inner = Narrow(clip, group.clip);
  if (group.clip.has_value() && !inner.has_value()) {
    return kNoPass;  // Nothing of the group is left visible.
  }
  const Matrix composite = CompositeTransform(group);
  // A matrix filter moves what the layer resolved into, so what the
  // pass has to hold is the pre-image of what is visible.
  if (inner.has_value() && !composite.IsIdentity()) {
    inner = inner->TransformBounds(composite.Invert());
  }

  std::vector<Item> items;
  GatherChildren(group, inner, items);

  std::optional<Rect> coverage;
  for (const Item& item : items) {
    coverage = coverage.has_value() ? coverage->Union(item.coverage)  //
                                    : item.coverage;
  }
  if (!coverage.has_value()) {
    // Nothing reaches the pass. A pass that got this far with children
    // would have taken their coverage, so none are orphaned by leaving.
    return kNoPass;
  }

  // In front of what it clips, and only now that the coverage is
  // known: a clip bounds a pass, it never grows one.
  if (group.clip_shape != nullptr) {
    items.insert(items.begin(), Item{
                                    .picture = group.clip_shape,
                                    .transform = group.transform,
                                    .clip = inner,
                                    .coverage = coverage.value(),
                                });
  }

  const auto index = static_cast<uint32_t>(passes_.size());
  Adopt(items, index);
  passes_.push_back(Pass{
      .opacity = group.opacity,
      .image_filter = group.image_filter,
      .color_filter = group.color_filter,
      .composite_transform = composite,
      .composite_nearest = CompositeNearest(group),
      .items = std::move(items),
      .coverage = coverage.value(),
  });
  KeyPass(passes_[index]);
  return index;
}

void SceneFlattener::FlattenScene(const PrSceneNode& root,
                                  const Rect& surface) {
  passes_.clear();
  std::vector<Item> items;
  Gather(root, std::nullopt, items);

  const auto index = static_cast<uint32_t>(passes_.size());
  Adopt(items, index);
  passes_.push_back(Pass{
      .items = std::move(items),
      .coverage = surface,
  });
}

void SceneFlattener::FlattenPicture(const PrPicture& picture,
                                    RenderPlan& plan,
                                    BufferArena& arena,
                                    const GeometryContext& frame,
                                    const Matrix& placement,
                                    const std::optional<Rect>& clip,
                                    const std::vector<uint32_t>& layer_passes) {
  const Rect pass = Rect::MakeXYWH(plan.origin.x, plan.origin.y,
                                   static_cast<Scalar>(plan.width),
                                   static_cast<Scalar>(plan.height));

  // Precomputed matrix transform.
  std::vector<Matrix> placed;
  placed.reserve(picture.GetTransforms().size());
  for (const Matrix& transform : picture.GetTransforms()) {
    placed.push_back(placement * transform);
  }

  const Rect picutre_bound = Bound(pass, clip);
  const Rect local_clip = clip.has_value()
                              ? clip->TransformBounds(placement.Invert())
                              : Rect::MakeMaximum();

  // Check leading draws for clear color optimization.
  const std::vector<Draw>& draws = picture.GetDraws();
  size_t start_index = 0;
  Color clear = Color::BlackTransparent();
  const bool can_clear = plan.draws.empty() && !clip.has_value();
  if (draws.size() == 0 && clip.has_value()) {
    std::cerr << "Can't clear because clip: " << clip.value() << " compared to "
              << pass << "\n";
  }
  for (; can_clear && start_index < draws.size(); start_index++) {
    if (!CoversPass(picture, start_index, pass, placement, placed)) {
      break;
    }
    const flutter::DlColor& color = draws[start_index].color;
    clear = clear.Blend(Color(color.getRedF(), color.getGreenF(),
                              color.getBlueF(), color.getAlphaF()),
                        draws[start_index].blend_mode);
  }

  if (start_index > 0) {
    const Color premultiplied = clear.Premultiply();
    plan.clear_color[0] = premultiplied.red;
    plan.clear_color[1] = premultiplied.green;
    plan.clear_color[2] = premultiplied.blue;
    plan.clear_color[3] = premultiplied.alpha;
  }

  uint32_t layer = 0;
  for (auto i = start_index; i < draws.size(); i++) {
    const Draw& draw = draws[i];
    const size_t type = static_cast<size_t>(draw.type);

    if (draw.type == Draw::DrawType::kLayer) {
      const uint32_t index = layer_passes[layer++];
      if (index == kNoPass ||
          !picture.GetBounds()[i].IntersectsWithRect(local_clip)) {
        continue;
      }
      Pass& child = passes_[index];
      child.texture_slot = static_cast<uint32_t>(plan.textures.size());
      plan.textures.push_back(child.texture);
      EmitComposite(plan, arena, child, child.texture_slot);
      continue;
    }

    if (!picture.GetBounds()[i].IntersectsWithRect(local_clip)) {
      continue;
    }

    if (draw.type == Draw::DrawType::kScissor) {
      const Rect root = draw.rect.IsMaximum()
                            ? picutre_bound
                            : draw.rect.TransformBounds(placed[draw.transform])
                                  .IntersectionOrEmpty(picutre_bound);
      plan.draws.push_back(GPUDraw{
          .program = ProgramType::kInvalid,
          .scissor = DeviceScissor(root, plan),
      });
      continue;
    }

    // What a clip resolves is the only thing that masks this pass, so
    // a reset with none of it behind is putting back what was never
    // taken away.
    // TODO: replace claudish.
    switch (draw.type) {
      case Draw::DrawType::kClipResolveNonZero:
      case Draw::DrawType::kClipResolveEvenOdd:
        plan.clip_resolved = true;
        break;
      case Draw::DrawType::kClipReset:
        if (!plan.clip_resolved) {
          continue;
        }
        // It puts back everything the clips replayed after it cover, so
        // nothing is masked again until one of them resolves.
        plan.clip_resolved = false;
        break;
      default:
        break;
    }

    // Axis aligned rectangles are replaced with a scissor.
    if (draw.type == Draw::DrawType::kRectClip &&
        placed[draw.transform].IsAligned2D()) {
      if (i + 1 < draws.size() &&
          draws[i + 1].type == Draw::DrawType::kClipResolveNonZero) {
        i++;
      }
      continue;
    }

    const ProgramType program = kDrawPrograms[type];
    if (program == ProgramType::kInvalid) {
      continue;
    }

    const Matrix& transform = placed[draw.transform];
    GeometryGenerator& generator = *kGeometryGenerators[type];
    const auto [vertex_count, index_count] =
        generator.GetAllocationCount(picture, draw, transform);
    if (index_count == 0) {
      continue;
    }

    // uint32_t gradient_count = draw.gradient == Draw::kNoIndex ? 0 : 1;
    BufferArena::Result result =
        arena.ReserveAllocation(vertex_count, index_count);
    if (!result.IsValid()) {
      continue;
    }
    *result.transform_out = transform;
    *result.paint_out = PrPaint{
        .transform_index = static_cast<int32_t>(result.transform_start),
    };
    generator.Generate(picture, draw, transform, result.position_out,
                       result.attributes_out, result.index_out,
                       result.paint_out, result.vertex_start,
                       result.paint_start, frame);

    if (result.new_buffer || plan.buffers.empty()) {
      plan.buffers.push_back(arena.GetBinds());
    }

    if (draw.gradient != Draw::kNoIndex) {
      auto& source = picture.GetColorSources()[draw.gradient];
      auto ramp = frame.gradient_atlas->RegisterGradient(source);
      if (ramp.has_value()) {
        WriteGradientData(*source, *result.gradient_out);
        result.gradient_out->ramp_row = static_cast<int32_t>(ramp->row);
        result.gradient_out->ramp_span = ramp->Pack();
        result.paint_out->gradient_index =
            static_cast<int32_t>(result.gradient_start);
      }
    }

    uint32_t binds = static_cast<uint32_t>(plan.buffers.size() - 1);

    // Attempt to batch to previous draws.
    if (!plan.draws.empty()) {
      GPUDraw& previous = plan.draws.back();
      if (previous.program == program &&  //
          previous.buffer_binds == binds) {
        previous.count += index_count;
        continue;
      }
    }
    plan.draws.push_back(GPUDraw{
        .program = program,
        .start = result.index_start,
        .count = index_count,
        .buffer_binds = binds,
    });
  }
}

void EncodePlan(const RenderPlan& plan,
                const ProgramSet& programs,
                GpuCommandBuffer& cmd_buffer) {
  const float viewport_origin[4] = {static_cast<float>(plan.width),
                                    static_cast<float>(plan.height),
                                    plan.origin.x, plan.origin.y};
  cmd_buffer.SetConstantData(GPUShaderStage::kVertex, viewport_origin,
                             sizeof(viewport_origin), 3);
  cmd_buffer.SetTextureTable(plan.textures.data(), plan.textures.size());

  ProgramType current_program = ProgramType::kInvalid;
  uint32_t current_binds = std::numeric_limits<uint32_t>::max();
  IRect32 current_scissor = IRect32::MakeWH(static_cast<int32_t>(plan.width),
                                            static_cast<int32_t>(plan.height));
  for (const GPUDraw& draw : plan.draws) {
    if (draw.program == ProgramType::kInvalid) {
      if (draw.scissor != current_scissor) {
        current_scissor = draw.scissor;
        cmd_buffer.SetScissorRect(
            current_scissor.GetX(), current_scissor.GetY(),
            current_scissor.GetWidth(), current_scissor.GetHeight());
      }
      continue;
    }
    if (draw.program != current_program) {
      current_program = draw.program;
      cmd_buffer.SetRenderPipeline(*programs[current_program]);
    }
    if (draw.buffer_binds != current_binds) {
      current_binds = draw.buffer_binds;
      BindStreams(cmd_buffer, plan.buffers[current_binds]);
    }
    cmd_buffer.DrawTrianglesIndexed(*plan.buffers[current_binds].indices,
                                    draw.start, draw.count);
  }
}

void Dispatch(const PrSceneNode& scene,
              BufferArena& arena,
              const ProgramSet& programs,
              GPUTexture& target,
              TextureCache& textures,
              TextMaterializer* text,
              GpuCommandBuffer& cmd_buffer,
              GPUTexture* shadow_lut,
              GradientAtlas* gradients) {
  const Rect surface =
      Rect::MakeWH(static_cast<Scalar>(target.GetDesc().width),
                   static_cast<Scalar>(target.GetDesc().height));

  SceneFlattener flattener;
  flattener.FlattenScene(scene, surface);
  flattener.EncodePasses(arena, textures, target.GetDesc().format, text,
                         shadow_lut, gradients);

  if (text != nullptr) {
    text->FlushAtlas(cmd_buffer);
  }
  if (gradients != nullptr) {
    gradients->RecordUploads(cmd_buffer, *programs[ProgramType::kGradientRamp]);
  }

  const std::vector<SceneFlattener::Pass>& passes = flattener.GetPasses();
  const std::vector<RenderPlan>& plans = flattener.GetPlan();
  for (size_t i = 0; i < plans.size(); i++) {
    if (passes[i].ready) {
      continue;  // Its texture already holds what it would draw.
    }
    const RenderPlan& plan = plans[i];
    // The frame's pass is last and renders into the target; every other
    // resolves into the texture the pass compositing it samples.
    const bool frame = i + 1 == plans.size();
    GPUTexture* render_target = frame ? &target : passes[i].texture;
    GPUTexture* accumulator = textures.AllocateWinding(plan.width, plan.height);
    GPUTexture* clip = textures.AllocateClip(plan.width, plan.height);
    const RenderPassDesc::Transient transients[] = {
        {.texture = accumulator, .clear = 0},
        {.texture = clip, .clear = 1},
    };

    cmd_buffer.StartRenderPass(RenderPassDesc{
        .target = render_target,
        .clear_color = {plan.clear_color[0], plan.clear_color[1],
                        plan.clear_color[2], plan.clear_color[3]},
        .transients = transients,
        .transient_count = std::size(transients),
        .label = "propeller",
    });
    EncodePlan(plan, programs, cmd_buffer);
    cmd_buffer.EndRenderPass();
  }
}

void Dispatch(const PrPicture& picture,
              BufferArena& arena,
              const ProgramSet& programs,
              GPUTexture& target,
              TextureCache& textures,
              TextMaterializer* text,
              GpuCommandBuffer& cmd_buffer,
              GPUTexture* shadow_lut,
              GradientAtlas* gradients) {
  PrSceneNode scene;
  PrSceneNode leaf;
  leaf.picture = std::shared_ptr<PrPicture>(std::shared_ptr<void>(),
                                            const_cast<PrPicture*>(&picture));
  scene.children.push_back(std::move(leaf));
  Dispatch(scene, arena, programs, target, textures, text, cmd_buffer,
           shadow_lut, gradients);
}

}  // namespace impeller
