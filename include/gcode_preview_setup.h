// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file gcode_preview_setup.h
 * @brief Wiring shared by every G-code preview card.
 *
 * PrintStatusPanel and PrintSelectDetailView both present the same thing: a
 * card stacking a gradient, the G-code viewer, and a thumbnail, with a
 * translucent metadata strip along the bottom. The content of that strip
 * differs; the setup around it does not. These helpers are the single place
 * that setup lives, so a change to preview policy is one edit rather than two
 * that drift.
 *
 * @threading Main thread only — both helpers touch LVGL widgets.
 */

#pragma once

#include "lvgl.h"

namespace helix::ui {

/**
 * @brief Resolve and apply the preview's render mode.
 *
 * Reads the live ladder sources (RuntimeConfig, HELIX_GCODE_MODE, the persisted
 * display setting), routes them through
 * helix::gcode_viewer::decide_preview_mode(), applies the result to @p viewer
 * when the winning tier calls for it, and logs which tier won.
 *
 * @param viewer  The gcode_viewer widget. Null is tolerated (no-op).
 * @param log_tag Prefix for the log line, e.g. the panel's get_name().
 * @return true when the viewer will be used, false for Thumbnail Only.
 */
bool apply_preview_render_mode(lv_obj_t* viewer, const char* log_tag);

/**
 * @brief Tell a preview which widget covers the bottom of it.
 *
 * The metadata strip is translucent and sits over the bottom of the preview in
 * some layouts and flush below it in others. Rather than every call site
 * guessing a shift, hand the viewer the strip: it measures the real overlap and
 * derives the offset, so the answer tracks breakpoints, orientation, and the
 * strip growing at runtime (an M117 adds a row).
 *
 * Pass a null @p occluder to clear. The viewer drops its reference when the
 * occluder is deleted, so the caller does not have to unwire on teardown.
 *
 * @param viewer   The gcode_viewer widget. Null is tolerated (no-op).
 * @param occluder Widget overlapping the viewer's bottom edge, or null.
 */
void set_preview_bottom_occluder(lv_obj_t* viewer, lv_obj_t* occluder);

} // namespace helix::ui
