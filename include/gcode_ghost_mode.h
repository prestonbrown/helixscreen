// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file gcode_ghost_mode.h
 * @brief How the ghost pass draws the layers above the one on screen.
 *
 * Separate from gcode_ghost_sampling.h, which decides *which* layers that pass
 * visits. This is the appearance half, and it is shared because the mode is a
 * viewer-level setting applied to whichever renderer happens to be active:
 * ui_gcode_viewer.cpp reads it once and hands it to the GLES renderer or the
 * CPU wireframe renderer without knowing which it has.
 *
 * It lives in its own header because those two renderer headers are mutually
 * exclusive by construction - each is compiled into a different set of
 * platforms - and each previously carried its own identical copy of the enum.
 * Two definitions of one type in one namespace is an ODR violation waiting for
 * the first translation unit that includes both, which nothing does today only
 * by luck of the platform gating.
 */

#pragma once

#include <cstdint>

namespace helix {
namespace gcode {

/**
 * @brief Ghost layer rendering mode (for print progress visualization).
 *
 * Ghost rendering is primarily a 3D renderer feature. The 2D renderer accepts
 * the setting for API compatibility but does not render ghost layers.
 */
enum class GhostRenderMode : uint8_t {
    Dimmed = 0, ///< Reduce opacity of unprinted layers
    Stipple = 1 ///< Use stipple pattern for unprinted layers
};

/// Mode used when nothing has set one explicitly.
inline constexpr GhostRenderMode DEFAULT_GHOST_RENDER_MODE = GhostRenderMode::Stipple;

} // namespace gcode
} // namespace helix
