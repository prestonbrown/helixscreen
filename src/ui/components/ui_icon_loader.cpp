// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_icon_loader.h"

#include <spdlog/spdlog.h>

// SDL window icon is only available when SDL is enabled
#ifdef HELIX_DISPLAY_SDL
#include "helix_icon_data.h"
#include "lvgl.h"
#endif

bool ui_set_window_icon(lv_display_t* disp) {
#ifdef HELIX_DISPLAY_SDL
    spdlog::trace("[Icon] Setting window icon...");

    if (!disp) {
        spdlog::error("[Icon] Cannot set icon: display is NULL");
        return false;
    }

    // Use embedded icon data from helix_icon_data.h
    // 128x128 pixels, ARGB8888 format
    lv_sdl_window_set_icon(disp, (void*)helix_icon_128x128, 128, 128);

    spdlog::trace("[Icon] Window icon set (128x128 embedded data)");
    return true;
#else
    // Window icons are not supported on embedded displays (framebuffer/DRM)
    (void)disp;
    return false;
#endif
}
