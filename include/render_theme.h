// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file render_theme.h
 * @brief Theme colors for the renderers, without a dependency on the UI layer.
 *
 * Renderers ask for a theme color token by name ("text", "card_bg",
 * "filament_metal"). The UI theme installs the lookup that answers, so a
 * renderer sees whatever the active theme and dark mode say at the moment it
 * asks, including right after a theme switch.
 */

#include "lvgl/lvgl.h"

namespace helix::render_theme {

/// Resolves a theme color token name to a color.
using ColorSource = lv_color_t (*)(const char* token);

/// Install the lookup color() answers through. nullptr removes it.
void set_color_source(ColorSource source);

/// The active theme's color for `token`, or black when no source is installed.
lv_color_t color(const char* token);

} // namespace helix::render_theme
