// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gcode_gles_renderer.h"

namespace helix {
namespace gcode {

/// What the 3D renderer holds that a test without a GL context can still read.
class GCodeGLESRendererTestAccess {
  public:
    /// Stand in for a finished VBO upload. With no GL context no upload ever
    /// completes, so a test marks the geometry uploaded and then watches whether
    /// anything clears the flag, which is what forces a re-upload.
    static void mark_uploaded(GCodeGLESRenderer& renderer) {
        renderer.geometry_uploaded_ = true;
    }

    static bool is_uploaded(const GCodeGLESRenderer& renderer) {
        return renderer.geometry_uploaded_;
    }

    static const SelectionState& selection(const GCodeGLESRenderer& renderer) {
        return renderer.selection_;
    }
};

} // namespace gcode
} // namespace helix
