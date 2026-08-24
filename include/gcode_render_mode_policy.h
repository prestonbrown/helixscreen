// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_gcode_viewer.h"

#include <cstring>

namespace helix::gcode_viewer {

/// Why the viewer ended up in a given render mode. Drives the startup log line.
enum class RenderModeReason {
    DefaultAuto,      ///< HELIX_GCODE_MODE unset — auto-detect at draw time
    EnvForced3D,      ///< HELIX_GCODE_MODE=3D, 3D renderer compiled in
    Env3DUnavailable, ///< HELIX_GCODE_MODE=3D on a build without ENABLE_3D_RENDERER
    EnvForced2D,      ///< HELIX_GCODE_MODE=2D
    EnvUnrecognized,  ///< Any other value, including empty
};

/// Outcome of the render-mode override decision.
struct RenderModeDecision {
    GcodeViewerRenderMode mode;
    RenderModeReason reason;
};

/**
 * @brief Resolve the viewer's startup render mode from HELIX_GCODE_MODE.
 *
 * Pure function. Only the exact strings "3D" and "2D" are honored — matching is
 * case-sensitive, so "3d" is an unrecognized value, not a request for 3D.
 *
 * Note the asymmetry with decide_ssao_enabled(): there, an unrecognized value
 * falls through to the memory tier, because the tier is the safe answer. Here an
 * unrecognized value resolves to Layer2D rather than Auto, so a typo'd override
 * lands on the renderer that works everywhere instead of silently behaving as if
 * nothing was set. Unset is the only path to Auto.
 *
 * @param mode_env          Raw HELIX_GCODE_MODE value, or nullptr when unset.
 * @param have_3d_renderer  Whether this build has ENABLE_3D_RENDERER. Passed in
 *                          rather than read from the macro so the unavailable
 *                          branch is reachable from a 3D-enabled test build.
 */
inline RenderModeDecision decide_render_mode(const char* mode_env, bool have_3d_renderer) {
    if (mode_env == nullptr) {
        return RenderModeDecision{GcodeViewerRenderMode::Auto, RenderModeReason::DefaultAuto};
    }
    if (std::strcmp(mode_env, "3D") == 0) {
        if (have_3d_renderer) {
            return RenderModeDecision{GcodeViewerRenderMode::Render3D,
                                      RenderModeReason::EnvForced3D};
        }
        return RenderModeDecision{GcodeViewerRenderMode::Layer2D,
                                  RenderModeReason::Env3DUnavailable};
    }
    if (std::strcmp(mode_env, "2D") == 0) {
        return RenderModeDecision{GcodeViewerRenderMode::Layer2D, RenderModeReason::EnvForced2D};
    }
    return RenderModeDecision{GcodeViewerRenderMode::Layer2D, RenderModeReason::EnvUnrecognized};
}

/// Which tier of the preview ladder supplied the mode. Drives the log line.
enum class PreviewModeSource {
    CommandLine,   ///< RuntimeConfig::gcode_render_mode (--render-2d and friends)
    Environment,   ///< HELIX_GCODE_MODE — already applied at widget creation
    Settings,      ///< Persisted display setting
    ThumbnailOnly, ///< Persisted setting is 3: the viewer is not used at all
};

/// Outcome of the preview ladder.
struct PreviewModeDecision {
    GcodeViewerRenderMode mode; ///< Only meaningful when `apply` is true
    PreviewModeSource source;
    bool apply; ///< false: leave the viewer's current mode alone
};

/// Thumbnail Only, as stored in the display settings.
constexpr int RENDER_MODE_THUMBNAIL_ONLY = 3;

/**
 * @brief Resolve a preview's render mode from the full ladder.
 *
 * Pure function. Every G-code preview in the app answers this the same way —
 * command line beats HELIX_GCODE_MODE beats the persisted setting — so the
 * ladder lives here rather than being restated at each preview. Pair it with
 * apply_preview_render_mode(), which reads the live sources and logs.
 *
 * Two tiers deliberately resolve to `apply == false`:
 *  - Environment: decide_render_mode() already ran at widget creation, so the
 *    viewer is in the requested mode. Re-applying would be a no-op that hides
 *    which tier actually won.
 *  - ThumbnailOnly: the viewer stays untouched because it is never shown.
 *
 * @param cmdline_mode   RuntimeConfig::gcode_render_mode; negative when unset.
 * @param env_mode_set   Whether HELIX_GCODE_MODE is present.
 * @param settings_mode  Persisted display setting (3 = Thumbnail Only).
 */
inline PreviewModeDecision decide_preview_mode(int cmdline_mode, bool env_mode_set,
                                               int settings_mode) {
    if (cmdline_mode >= 0) {
        return PreviewModeDecision{static_cast<GcodeViewerRenderMode>(cmdline_mode),
                                   PreviewModeSource::CommandLine, true};
    }
    if (env_mode_set) {
        return PreviewModeDecision{GcodeViewerRenderMode::Auto, PreviewModeSource::Environment,
                                   false};
    }
    if (settings_mode == RENDER_MODE_THUMBNAIL_ONLY) {
        return PreviewModeDecision{GcodeViewerRenderMode::Auto, PreviewModeSource::ThumbnailOnly,
                                   false};
    }
    return PreviewModeDecision{static_cast<GcodeViewerRenderMode>(settings_mode),
                               PreviewModeSource::Settings, true};
}

} // namespace helix::gcode_viewer
