// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "blend.h"
#include "color_filter.h"
#include "common.h"

// The scope composites that read a virtual canvas as their source are
// gone with it: a pass now composites from a texture it samples, not
// from an attachment it shares.
