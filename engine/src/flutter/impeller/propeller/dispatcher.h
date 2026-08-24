// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_DISPATCHER_H_
#define FLUTTER_IMPELLER_PROPELLER_DISPATCHER_H_

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "impeller/propeller/buffer_arena.h"
#include "impeller/propeller/geometry.h"
#include "impeller/propeller/picture.h"
#include "impeller/propeller/renderer/gpu_context.h"
#include "impeller/propeller/scene.h"

namespace impeller {

struct GPUDraw {
  ProgramType program;
  uint32_t start;
  uint32_t count;
  uint32_t buffer_binds;
  /// Pass relative, and read only by a scissor draw: one carries
  /// ProgramType::kInvalid and sets this instead of drawing.
  IRect32 scissor;
};

struct RenderPlan {
  /// Encode-time: has any clip in this pass masked anything yet? The
  /// attachment starts fully visible, so until one has, a reset paints
  /// 1 over 1 and is dropped.
  bool clip_resolved = false;
  std::vector<GPUTexture*> textures;
  Matrix transform;
  /// Where the pass starts in root space.
  Point origin;
  uint32_t width;
  uint32_t height;
  float clear_color[4] = {0, 0, 0, 0};
  std::vector<GPUDraw> draws;
  std::vector<BufferBinds> buffers;
};

/// The texture cache retains the texture associated with a planned render pass.
///
/// On subsequent frames, we can skip the offscreen render pass encoding as long
/// as the identity of all pictures which compose the pass are unchanged. There
/// are a few different aspects to this:
///
///   * When an offscreen is rendered into its parent with a trivial composite
///     like a color blend, color filter, or opacity, we only cache the child
///     texture.
///   * When an offscreen is rendered with a complex image filter operation, we
///     cache both the input texture and the resulting filtered texture. This
///     allows to optimize for animated image filters.
///
///  Additionally, we optimize texture allocation with a simplified recycling
///  strategy. Textures which are transient in a render pass (such as the
///  winding accumulator) are allocated as few times as possible: subsequent
///  requests will reuse the initially allocated texture as long as the
///  dimensions match. When a cached texture isn't used in a frame, we retain it
///  until the plan period of the text frame, to see if a different render pass
///  needs a texture of the same dimensions.
///
/// This optimization relies on the fact that we resolve all cached render
/// passes before allocating any additional textures.
class TextureCache {
 public:
  explicit TextureCache(GPUContext* context);

  /// A unique identifier for an offscreen texture.
  struct OffscreenKey {
    static constexpr size_t kMaxPictures = 4;

    uint64_t ids[kMaxPictures] = {0, 0, 0, 0};
    uint32_t width = 0;
    uint32_t height = 0;

    bool IsCacheable() const { return ids[0] != 0; }
    bool operator==(const OffscreenKey& other) const;
  };

  /// Placemnet contains minimimal transform information to determine if
  /// a parent transform may have invalidated the texture.
  struct Placement {
    Scalar basis[4] = {1, 0, 0, 1};
    Point offset;

    bool operator==(const Placement& other) const;
  };

  /// Lookup an existing cached offscreen against `key`, return nullptr
  /// if none is found.
  GPUTexture* FindOffscreen(const OffscreenKey& key,
                            const std::vector<Placement>& placements,
                            TextureFormat format);

  /// Create a new offscreen texture identified by `key`.
  GPUTexture* CreateOffscreen(const OffscreenKey& key,
                              const std::vector<Placement>& placements,
                              TextureFormat format);

  /// What made every texture here, and what turns an engine image into
  /// one.
  GPUContext* GetContext() const { return context_; }

  /// Create a new winding accumulator, or return an existing allocation.
  GPUTexture* AllocateWinding(uint32_t width, uint32_t height);

  /// The same, for the pass's clip coverage.
  GPUTexture* AllocateClip(uint32_t width, uint32_t height);

  /// Increment the frame.
  ///
  /// Free unused textures and move anything not used this frame into the
  /// pending deletion cache.
  void Next();

 private:
  GPUContext* context_;

  /// A transient of that size and format: a live one, one waiting to be
  /// dropped, or a new allocation.
  GPUTexture* AllocateTransient(uint32_t width,
                                uint32_t height,
                                TextureFormat format);

  struct CachedTransient {
    uint32_t width;
    uint32_t height;
    TextureFormat format;
    bool used_this_frame;
    std::unique_ptr<GPUTexture> texture;
  };
  std::vector<CachedTransient> transients_;
  std::vector<CachedTransient> pending_deletion_;

  struct CachedOffscreen {
    OffscreenKey key;
    std::vector<Placement> placements;
    TextureFormat format;
    bool used_this_frame;
    std::unique_ptr<GPUTexture> texture;
  };
  std::vector<CachedOffscreen> offscreens_;
  std::vector<CachedOffscreen> offscreens_pending_deletion_;
};

//------------------------------------------------------------------------------
/// Turns a scene into the passes that render it.
class SceneFlattener {
 public:
  /// A pass index that names no pass.
  static constexpr uint32_t kNoPass = std::numeric_limits<uint32_t>::max();

  /// One thing a pass draws: a picture where the scene put it, or what
  /// another pass resolved into.
  struct Item {
    /// Null when this draws a pass rather than a picture.
    std::shared_ptr<PrPicture> picture;
    uint32_t pass = kNoPass;

    Matrix transform;
    std::optional<Rect> clip;
    Rect coverage;
    std::vector<uint32_t> layer_passes;

    bool IsComposite() const { return picture == nullptr; }
  };

  /// One planned render pass.
  struct Pass {
    /// The pass that composites this one, kNoPass for the frame's own.
    uint32_t parent = kNoPass;
    /// Which slot of the parent's texture table holds what this pass
    /// resolved into.
    uint32_t texture_slot = 0;

    Scalar opacity = 1.0f;
    std::shared_ptr<flutter::DlImageFilter> image_filter;
    std::shared_ptr<const flutter::DlColorFilter> color_filter;
    /// What compositing this pass places it by: a matrix filter is a
    /// transform on the quad that samples it and nothing more.
    Matrix composite_transform;
    bool composite_nearest = false;

    std::vector<Item> items;
    /// Coveragae in root space.
    Rect coverage;

    /// The unique identifier for this render passes cached texture.
    TextureCache::OffscreenKey key;
    std::vector<TextureCache::Placement> placements;

    /// The texture which samples this one, if its an offscreen draw.
    GPUTexture* texture = nullptr;
    bool ready = false;
  };

  /// Write the geometry every pass draws into `arena`, and the commands
  /// that draw it into a plan of its own.
  ///
  /// A pass whose texture the cache already holds is not encoded: its
  /// plan stays empty and the pass that composites it samples what is
  /// there. `format` is what an offscreen is allocated as, which is the
  /// format of the target the frame resolves into.
  void EncodePasses(BufferArena& arena,
                    TextureCache& cache,
                    TextureFormat format,
                    TextMaterializer* text = nullptr,
                    GPUTexture* shadow_lut = nullptr,
                    GradientAtlas* gradients = nullptr);

  /// Order `root` into passes, child before parent.
  ///
  /// `surface` is the bounds of the onscreen framebuffer.
  void FlattenScene(const PrSceneNode& root, const Rect& surface);

  const std::vector<Pass>& GetPasses() const { return passes_; }

  const std::vector<RenderPlan>& GetPlan() const { return render_plan_; }

 private:
  /// Open the pass a group resolves in.
  ///
  /// kNoPass is returned if nothing can reach the pass due to culling or
  /// coverage.
  uint32_t Open(const PrSceneNode& group, const std::optional<Rect>& clip);

  /// Collect what draws into the pass being opened, in paint order.
  /// The same, for what a node holds rather than the node itself: a
  /// group applies its own clip before its children are gathered.
  void GatherChildren(const PrSceneNode& node,
                      const std::optional<Rect>& clip,
                      std::vector<Item>& items);

  void Gather(const PrSceneNode& node,
              const std::optional<Rect>& inherited,
              std::vector<Item>& items);

  /// Claim the passes `items` composites: they resolve into `index`.
  void Adopt(const std::vector<Item>& items, uint32_t index);

  /// Open a pass for every layer `picture` holds, innermost first, and
  /// name them in `passes` in the order the picture draws them.
  void OpenLayers(const PrPicture& picture,
                  const Matrix& transform,
                  const std::optional<Rect>& clip,
                  std::vector<uint32_t>& passes);

  /// What the cache should hold a pass of `items` covering `coverage`
  /// against. A key with no ids is a pass that cannot be cached.
  void KeyPass(Pass& pass);

  /// Write a picture's geometry into `arena` and record what drawing it
  /// takes into `plan`.
  void FlattenPicture(const PrPicture& picture,
                      RenderPlan& plan,
                      BufferArena& arena,
                      const GeometryContext& frame,
                      const Matrix& placement,
                      const std::optional<Rect>& clip,
                      const std::vector<uint32_t>& layer_passes);

  std::vector<Pass> passes_;
  std::vector<RenderPlan> render_plan_;
};

/// Encode a plan's draws into a pass that has already started.
///
void EncodePlan(const RenderPlan& plan,
                const ProgramSet& programs,
                GpuCommandBuffer& cmd_buffer);

/// Draw a scene into `target`.
///
/// Every texture the frame needs comes from `textures`, which holds
/// them past the frame: what a pass resolved into is what lets the next
/// frame skip drawing it again.
void Dispatch(const PrSceneNode& scene,
              BufferArena& arena,
              const ProgramSet& programs,
              GPUTexture& target,
              TextureCache& textures,
              TextMaterializer* text,
              GpuCommandBuffer& cmd_buffer,
              GPUTexture* shadow_lut = nullptr,
              GradientAtlas* gradients = nullptr);

/// Draw one picture into `target`, as a scene of just that picture.
void Dispatch(const PrPicture& picture,
              BufferArena& arena,
              const ProgramSet& programs,
              GPUTexture& target,
              TextureCache& textures,
              TextMaterializer* text,
              GpuCommandBuffer& cmd_buffer,
              GPUTexture* shadow_lut = nullptr,
              GradientAtlas* gradients = nullptr);

}  // namespace impeller

#endif  //  FLUTTER_IMPELLER_PROPELLER_DISPATCHER_H_
