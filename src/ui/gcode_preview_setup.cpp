// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_preview_setup.h"

#include "ui_gcode_viewer.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "gcode_render_mode_policy.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace helix::ui {

using helix::gcode_viewer::decide_preview_mode;
using helix::gcode_viewer::PreviewModeSource;

bool apply_preview_render_mode(lv_obj_t* viewer, const char* log_tag) {
    if (!viewer) {
        return false;
    }

    const auto* config = get_runtime_config();
    const auto decision = decide_preview_mode(
        config ? config->gcode_render_mode : -1, std::getenv("HELIX_GCODE_MODE") != nullptr,
        DisplaySettingsManager::instance().get_gcode_render_mode());

    if (decision.apply) {
        ui_gcode_viewer_set_render_mode(viewer, decision.mode);
    }

    switch (decision.source) {
    case PreviewModeSource::CommandLine:
        spdlog::debug("[{}]   ✓ Set G-code render mode: {} (cmdline)", log_tag,
                      static_cast<int>(decision.mode));
        break;
    case PreviewModeSource::Environment:
        // decide_render_mode() already applied this at widget creation.
        spdlog::debug("[{}]   ✓ G-code render mode: {} (env var)", log_tag,
                      ui_gcode_viewer_is_using_2d_mode(viewer) ? "2D" : "3D");
        break;
    case PreviewModeSource::Settings:
        spdlog::debug("[{}]   ✓ Set G-code render mode: {} (settings)", log_tag,
                      static_cast<int>(decision.mode));
        break;
    case PreviewModeSource::ThumbnailOnly:
        spdlog::debug("[{}]   ✓ G-code render mode: Thumbnail Only (settings)", log_tag);
        break;
    }

    return decision.source != PreviewModeSource::ThumbnailOnly;
}

void set_preview_bottom_occluder(lv_obj_t* viewer, lv_obj_t* occluder) {
    if (!viewer) {
        return;
    }
    ui_gcode_viewer_set_bottom_occluder(viewer, occluder);
}

} // namespace helix::ui
