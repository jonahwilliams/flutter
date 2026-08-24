// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROPELLER_INPUTS_GLSL_
#define PROPELLER_INPUTS_GLSL_

// The pass's own attachments as subpass inputs, for the composites and
// fetch blends that read what they are about to write over. Requires
// same-attachment feedback (rasterization order attachment access) at
// the pipeline level.
layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput
    in_d0;
layout(input_attachment_index = 1, set = 2, binding = 1) uniform subpassInput
    in_d1;
layout(input_attachment_index = 2, set = 2, binding = 2) uniform subpassInput
    in_w;

#endif  // PROPELLER_INPUTS_GLSL_
