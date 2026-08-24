// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_PROPELLER_METAL_PROPELLER_ENGINE_H_
#define FLUTTER_IMPELLER_PROPELLER_METAL_PROPELLER_ENGINE_H_

#import <Metal/Metal.h>

#include <memory>

#include "flutter/display_list/image/dl_image.h"  // nogncheck
#include "flutter/flow/layers/layer.h"            // nogncheck
#include "impeller/propeller/metal/metal_renderer.h"
#include "impeller/propeller/picture.h"
#include "impeller/propeller/scene.h"
#include "impeller/propeller/text_materializer.h"

namespace impeller {

class FreeTypeGlyphRasterizer;

//------------------------------------------------------------------------------
/// The Propeller renderer.
class PropellerEngine {
 public:
  static PropellerEngine* GetIfEnabled();

  /// Render one frame of a flow layer tree.
  bool RenderLayerTree(const flutter::Layer* root,
                       id<MTLTexture> target,
                       Scalar device_pixel_ratio,
                       const flutter::Stopwatch* raster_time = nullptr,
                       const flutter::Stopwatch* ui_time = nullptr);

  /// Render `picture` into a fresh texture and wrap it as an engine
  /// image: the raster half of Picture.toImageSync.
  sk_sp<flutter::DlImage> RenderPictureToImage(
      const std::shared_ptr<PrPicture>& picture,
      uint32_t width,
      uint32_t height);

 private:
  PropellerEngine();

  bool valid_ = false;
  std::shared_ptr<GPUContextMTL> context_;
  std::shared_ptr<PagedAtlas> atlas_;
  std::shared_ptr<FreeTypeGlyphRasterizer> glyph_rasterizer_;
  std::shared_ptr<TextMaterializer> materializer_;
  std::unique_ptr<MetalRenderer> renderer_;
  std::unique_ptr<PrSceneBuilder> builder_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_PROPELLER_METAL_PROPELLER_ENGINE_H_
