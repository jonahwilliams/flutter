// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/propeller/dispatcher.h"

#include <cmath>
#include <iterator>
#include <limits>

#include "flutter/display_list/effects/image_filters/dl_blur_image_filter.h"
#include "flutter/display_list/effects/image_filters/dl_matrix_image_filter.h"
#include "impeller/propeller/renderer/gpu_context.h"

namespace impeller {

namespace {

NoOpGeometryGenerator g_noop;
RectGeometryGenerator g_rect;
RRectGeometryGenerator g_rrect;
BlurGeometryGenerator g_blur;
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
    &g_blur,          // kBlurredFillPath
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
    ProgramType::kColor,                  // kBlurredFillPath
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
  const Scalar to_pixels = plan.extent.x > 0
                               ? static_cast<Scalar>(plan.width) / plan.extent.x
                               : 1.0f;
  return IRect32::Round(root.Shift(-plan.origin).Scale(to_pixels))
      .IntersectionOrEmpty(extent);
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
    case ProgramType::kBlur:
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
                   uint32_t slot,
                   ProgramType program,
                   Scalar opacity,
                   const std::optional<Rect>& cut_to) {
  // Trim filter overspill by adjusting UV instead of inserting clip rect.s
  const Rect coverage = pass.coverage;
  Rect quad = coverage;
  Point uv_min(0, 0);
  Point uv_max(1, 1);
  if (cut_to.has_value()) {
    quad = coverage.IntersectionOrEmpty(*cut_to);
    if (quad.IsEmpty()) {
      return;
    }
    const Size span = coverage.GetSize();
    if (span.width <= 0 || span.height <= 0) {
      return;
    }
    const Point origin = coverage.GetLeftTop();
    uv_min = Point((quad.GetLeft() - origin.x) / span.width,
                   (quad.GetTop() - origin.y) / span.height);
    uv_max = Point((quad.GetRight() - origin.x) / span.width,
                   (quad.GetBottom() - origin.y) / span.height);
  }

  BufferArena::Result result = arena.ReserveAllocation(4, 6);
  if (!result.IsValid()) {
    return;
  }

  *result.transform_out = pass.composite_transform;
  *result.paint_out = PrPaint{
      .texture_index = static_cast<int32_t>(slot),
      .transform_index = static_cast<int32_t>(result.transform_start),
      .flags = pass.composite_nearest ? kPaintFlagSampleNearest : 0u,
  };

  result.position_out[0] = quad.GetLeftTop();
  result.position_out[1] = quad.GetLeftBottom();
  result.position_out[2] = quad.GetRightTop();
  result.position_out[3] = quad.GetRightBottom();

  // The same corners, as a fraction of what the texture spans.
  const Point kFullTexture[4] = {
      Point(uv_min.x, uv_min.y),
      Point(uv_min.x, uv_max.y),
      Point(uv_max.x, uv_min.y),
      Point(uv_max.x, uv_max.y),
  };
  const uint32_t color =
      flutter::DlColor::kWhite().withAlphaF(opacity).premultipliedRGBA();
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
    if (previous.program == program && previous.buffer_binds == binds) {
      previous.count += 6;
      return;
    }
  }
  plan.draws.push_back(GPUDraw{
      .program = program,
      .start = result.index_start,
      .count = 6,
      .buffer_binds = binds,
  });
}

}  // namespace

/////// Texture Cache

TextureCache::TextureCache(GPUContext* context) : context_(context) {}

bool TextureCache::OffscreenKey::operator==(const OffscreenKey& other) const {
  return ids == other.ids && width == other.width && height == other.height &&
         variant == other.variant && sigma == other.sigma;
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
    plan.extent = Point(pass.coverage.GetWidth(), pass.coverage.GetHeight());
    plan.width = static_cast<uint32_t>(
        std::max(1.0f, std::ceil(plan.extent.x * pass.resolution_scale)));
    plan.height = static_cast<uint32_t>(
        std::max(1.0f, std::ceil(plan.extent.y * pass.resolution_scale)));
    plan.textures.resize(kReservedTextureSlots, nullptr);

    if (pass.filter_source != kNoPass) {
      Pass& previous = passes_[pass.filter_source];
      plan.filter = pass.filter_step;
      const auto slot = static_cast<uint32_t>(plan.textures.size());
      previous.texture_slot = slot;
      plan.textures.push_back(previous.texture);
      EmitComposite(plan, arena, pass, slot, ProgramType::kBlur, 1.0f,
                    std::nullopt);
      continue;
    }

    // Shrink the clip rect by parent scene clips via intersection.
    const Rect extent = Rect::MakeXYWH(plan.origin.x, plan.origin.y,
                                       plan.extent.x, plan.extent.y);
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
      const auto slot = static_cast<uint32_t>(plan.textures.size());
      child.texture_slot = slot;
      plan.textures.push_back(child.texture);
      EmitComposite(plan, arena, child, slot, ProgramType::kColor,
                    child.opacity,
                    child.filter_source != kNoPass ? std::optional<Rect>(bound)
                                                   : std::nullopt);
    }

    // Trailing clips can be dropped as they will have no impact on the final
    // picture.
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
    const Rect reach = bounds->TransformBounds(node.transform);
    Rect coverage = reach;
    if (clip.has_value()) {
      coverage = coverage.IntersectionOrEmpty(*clip);
    }
    if (coverage.IsEmpty()) {
      return;
    }
    // A clip that does not reach into what the item covers is not a clip
    // on it. Carrying it anyway costs a scissor either side of the item,
    // because the one before and the one after ask for something else --
    // and a row whose clip is its own bounds is the ordinary case, not a
    // corner of one.
    Item item{
        .picture = node.picture,
        .transform = node.transform,
        .clip = clip.has_value() && clip->Contains(reach) ? std::nullopt : clip,
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
    // What the child shows through itself goes down first, under its own
    // content: everything gathered so far is what the target will hold
    // by the time it draws.
    if (child.backdrop_filter != nullptr) {
      const uint32_t behind = OpenBackdrop(items, child, clip);
      if (behind != kNoPass) {
        const std::optional<Rect> inner = Narrow(clip, child.clip);
        const Rect covers = passes_[behind].coverage;
        const bool cuts = inner.has_value() && !inner->Contains(covers);
        items.push_back(Item{
            .pass = behind,
            .clip = cuts ? inner : std::nullopt,
            .coverage =
                inner.has_value() ? covers.IntersectionOrEmpty(*inner) : covers,
        });
      }
    }
    if (!child.NeedsComposite()) {
      Gather(child, clip, items);
      continue;
    }
    const uint32_t pass = Open(child, clip);
    if (pass == kNoPass) {
      continue;
    }
    // The child's own clip narrowed into what it inherited, which is what
    // Gather does for a leaf and what Open does for the pass itself. A
    // node carries its clip rather than sitting under one, so taking only
    // what was inherited leaves the composite unclipped beside siblings
    // that are not.
    //
    // A pass used to hold only what was already visible, so that cost
    // nothing. A filter widens what it holds past that, and then it is
    // the difference between clipping the overspill and throwing the
    // scissor wide to let it out -- once per filtered node, with the
    // scissor going back for the next sibling.
    const std::optional<Rect> inner = Narrow(clip, child.clip);
    // Where the pass lands, which a filter may move; what it holds is
    // its own coverage.
    const Rect covers = passes_[pass].coverage.TransformBounds(
        passes_[pass].composite_transform);
    // Dropped when it cannot reach into that, the same as for a leaf. A
    // filter widens what a pass holds, so this is the one place a clip
    // often does still cut -- and where it does not, keeping it would put
    // the composite out of step with the siblings around it.
    const bool cuts = inner.has_value() && !inner->Contains(covers);
    items.push_back(Item{
        .pass = pass,
        .clip = cuts ? inner : std::nullopt,
        .coverage =
            inner.has_value() ? covers.IntersectionOrEmpty(*inner) : covers,
    });
  }
}

void SceneFlattener::KeyFilterStep(Pass& pass) {
  const Pass& source = passes_[pass.filter_source];

  // The same contents, one step further along. A picture's id is already
  // a stable name for what it holds, so the step takes those names as
  // they are rather than folding them into something smaller: a source
  // that cannot be cached has no id to take, and the step inherits that
  // too.
  const uint32_t width = pass.key.width;
  const uint32_t height = pass.key.height;
  pass.key = source.key;
  pass.key.width = width;
  pass.key.height = height;
  pass.key.variant = source.key.variant + 1;
  pass.key.sigma = pass.filter_step.sigma;

  // Where the source sat is where this sits: a transform that moved it
  // out from under its cached texture moved this one too.
  pass.placements = source.placements;
}

void SceneFlattener::KeyPass(Pass& pass) {
  // Said again from scratch each time. A pass is keyed when it opens and
  // again when a filter chain settles what size it is held at, and the
  // contents are named by appending -- so anything left over from the
  // first telling would be counted twice.
  pass.key.ids.clear();
  pass.placements.clear();

  // What the cache allocates, so it is the size the pass is held at
  // rather than the size of the coverage it spans. A pass gathered at a
  // reduced resolution renders into an offscreen that size: give it one
  // the size of its coverage and the render pass has a target of one
  // size and transient attachments of another.
  pass.key.width = static_cast<uint32_t>(std::max(
      1.0f, std::ceil(pass.coverage.GetWidth() * pass.resolution_scale)));
  pass.key.height = static_cast<uint32_t>(std::max(
      1.0f, std::ceil(pass.coverage.GetHeight() * pass.resolution_scale)));

  if (pass.filter_source != kNoPass) {
    KeyFilterStep(pass);
    return;
  }
  const Point origin = pass.coverage.GetOrigin();
  pass.key.ids.reserve(pass.items.size());
  for (const Item& item : pass.items) {
    if (item.IsComposite()) {
      // TODO: name a composited child by the key of the pass it
      // resolved into, which would be exact the same way these are.
      // What another pass resolved into is not something this one can
      // be keyed by: it is that pass's business whether it changed.
      pass.key = TextureCache::OffscreenKey{.width = pass.key.width,
                                            .height = pass.key.height};
      pass.placements.clear();
      return;
    }
    pass.key.ids.push_back(item.picture->GetId());
    const Matrix& transform = item.transform;
    pass.placements.push_back(TextureCache::Placement{
        .basis = {transform.m[0], transform.m[1], transform.m[4],
                  transform.m[5]},
        .offset = Point(transform.m[12], transform.m[13]) - origin,
    });
  }
}

namespace {

/// How many taps either side of centre a sigma needs. Three sigma is
/// where the Gaussian has nothing left to add, and the cap keeps the
/// loop bounded for a blur wider than a pass can afford to sample one
/// texel at a time.
/// As far down as a blur may be held, per axis -- so a quarter of the
/// pixels at the floor. A sixteenth of an axis is what the signal itself
/// supports, but one halving is as far as this goes for now: past it the
/// gathered image is small enough that what the composite magnifies back
/// up starts to show.
constexpr Scalar kMinBlurDownscale = 0.5f;

/// How far down a blur is held before it is gathered, the way Impeller's
/// Gaussian picks it: a power of two that brings the sigma to about four
/// texels, so the kernel is a fixed width however wide the blur is.
///
/// Powers of two only, because halving is what a bilinear sample does
/// cleanly, and never past the floor above.
Scalar BlurDownscale(Scalar sigma) {
  if (sigma <= 4) {
    return 1.0f;
  }
  return std::pow(2.0f, std::max(std::log2(kMinBlurDownscale),
                                 std::round(std::log2(4.0f / sigma))));
}

int32_t TapRadius(Scalar sigma) {
  if (!(sigma > 0)) {
    return 0;
  }
  return std::min(static_cast<int32_t>(std::ceil(3.0f * sigma)), 64);
}

}  // namespace

uint32_t SceneFlattener::AppendBlurChain(uint32_t source,
                                         const flutter::DlImageFilter* filter,
                                         const Matrix& transform) {
  const flutter::DlBlurImageFilter* blur =
      filter != nullptr ? filter->asBlur() : nullptr;
  if (blur == nullptr) {
    return source;
  }

  const Rect coverage = passes_[source].coverage;

  // The texture is in device pixels and the sigma is in the layer's own
  // space, so the transform is what says how far it reaches across it.
  const Scalar scale = transform.GetMaxBasisLengthXY();
  const Scalar sigma_x = blur->sigma_x() * scale;
  const Scalar sigma_y = blur->sigma_y() * scale;

  // Held smaller the wider the blur is, so the kernel stays about the
  // same width whatever the sigma. One factor for both axes: the steps
  // share their textures, and the second reads what the first wrote.
  const Scalar down = std::min(BlurDownscale(sigma_x), BlurDownscale(sigma_y));

  // What the kernel spans in the texture it actually gathers from.
  const Scalar held_x = sigma_x * down;
  const Scalar held_y = sigma_y * down;
  const Scalar width = std::max(1.0f, std::ceil(coverage.GetWidth() * down));
  const Scalar height = std::max(1.0f, std::ceil(coverage.GetHeight() * down));
  const FilterUniform steps[2] = {
      FilterUniform{.step = Point(1.0f / width, 0),
                    .sigma = held_x,
                    .radius = TapRadius(held_x)},
      FilterUniform{.step = Point(0, 1.0f / height),
                    .sigma = held_y,
                    .radius = TapRadius(held_y)},
  };

  // Whatever the parent draws the layer at belongs to the pass it
  // composites, which is the last step rather than the content.
  const Scalar opacity = passes_[source].opacity;
  passes_[source].opacity = 1.0f;

  // The content is held at the size the blur gathers from, rather than
  // drawn full size and reduced afterwards. A tap steps one texel of what
  // it writes, so anything bigger on the way in has taps striding over
  // source texels that are never read. Rasterizing the content at that
  // size to begin with is cheaper than resampling a larger one and truer
  // than any resampling could be: nothing is resampled at all.
  passes_[source].resolution_scale = down;
  KeyPass(passes_[source]);

  uint32_t previous = source;
  for (const FilterUniform& step : steps) {
    if (step.radius <= 0) {
      continue;  // Narrower than a texel: no tap would reach past centre.
    }
    const auto index = static_cast<uint32_t>(passes_.size());
    passes_.push_back(Pass{
        .filter_source = previous,
        .filter_step = step,
        .resolution_scale = down,
        .coverage = coverage,
    });
    KeyPass(passes_[index]);
    previous = index;
  }
  passes_[previous].opacity = opacity;
  return previous;
}

uint32_t SceneFlattener::OpenBackdrop(const std::vector<Item>& behind,
                                      const PrSceneNode& node,
                                      const std::optional<Rect>& clip) {
  if (behind.empty()) {
    return kNoPass;  // Nothing has been drawn for it to show.
  }
  const std::optional<Rect> inner = Narrow(clip, node.clip);
  if (node.clip.has_value() && !inner.has_value()) {
    return kNoPass;
  }

  // Where it shows the backdrop: its own bounds rather than everything
  // that has ever been drawn, narrowed by the clip. With no bounds it
  // shows wherever it is allowed to.
  std::optional<Rect> shows = node.backdrop_bounds;
  if (inner.has_value()) {
    shows = shows.has_value() ? shows->IntersectionOrEmpty(*inner) : inner;
  }
  if (shows.has_value() && shows->IsEmpty()) {
    return kNoPass;
  }

  // What the filter has to read to fill that, which reaches past it.
  const Rect needs = shows.has_value() ? ExpandForFilterInput(
                                             *shows, node.backdrop_filter.get(),
                                             node.transform)
                                       : Rect::MakeMaximum();

  // Only what lands in it. A picture the backdrop cannot see is not one
  // it is decided by, so it is neither drawn again for it nor named
  // among the contents it is held against -- which is what lets a
  // backdrop survive a frame that changed something elsewhere.
  std::vector<Item> reads;
  std::optional<Rect> drawn;
  for (const Item& item : behind) {
    if (!item.coverage.IntersectsWithRect(needs)) {
      continue;
    }
    drawn = drawn.has_value() ? drawn->Union(item.coverage) : item.coverage;
    reads.push_back(item);
  }
  if (!drawn.has_value()) {
    return kNoPass;  // Nothing behind it reaches where it shows.
  }

  // Held back to what was actually drawn: reaching further than the
  // content went gathers nothing but the clear.
  const Rect coverage = needs.IsMaximum()
                            ? drawn.value()
                            : needs.IntersectionOrEmpty(drawn.value());
  if (coverage.IsEmpty()) {
    return kNoPass;
  }

  // A copy of what it reads, which is cheap: an item names a picture
  // rather than holding one.
  const auto index = static_cast<uint32_t>(passes_.size());
  passes_.push_back(Pass{
      .image_filter = node.backdrop_filter,
      .items = std::move(reads),
      .coverage = coverage,
  });
  KeyPass(passes_[index]);
  return AppendBlurChain(index, node.backdrop_filter.get(), node.transform);
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

  // The texture this pass resolves into is sized from the coverage, so
  // the filter's reach has to be in it before KeyPass reads it.
  //
  // Deliberately not clipped back afterwards. The margin past the
  // content is what the filter fades into: a texture cropped to the clip
  // would leave the filter sampling content texels at its edge instead
  // of nothing, which smears rather than fades.
  coverage = ExpandForFilter(coverage.value(), group.image_filter.get(),
                             group.transform);

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
  return AppendBlurChain(index, group.image_filter.get(), group.transform);
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
  const Rect pass = Rect::MakeXYWH(plan.origin.x, plan.origin.y, plan.extent.x,
                                   plan.extent.y);

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
      const auto slot = static_cast<uint32_t>(plan.textures.size());
      child.texture_slot = slot;
      plan.textures.push_back(child.texture);
      EmitComposite(plan, arena, child, slot, ProgramType::kColor,
                    child.opacity,
                    child.filter_source != kNoPass
                        ? std::optional<Rect>(Bound(pass, clip))
                        : std::nullopt);
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
  const float viewport_origin[4] = {plan.extent.x, plan.extent.y, plan.origin.x,
                                    plan.origin.y};
  cmd_buffer.SetConstantData(GPUShaderStage::kVertex, viewport_origin,
                             sizeof(viewport_origin), 3);
  if (plan.filter.has_value()) {
    cmd_buffer.SetConstantData(GPUShaderStage::kFragment, &plan.filter.value(),
                               sizeof(FilterUniform), 6);
  }
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
