// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix::ui {

/**
 * @brief What the print-status preview needs to (re)load to match desired state.
 *
 * Each flag is independent: the thumbnail (fallback content) and the gcode
 * viewer (3D/2D geometry) are reconciled separately.
 */
struct PreviewAction {
    bool load_thumbnail = false;
    bool load_gcode = false;
    /// The viewer currently renders a DIFFERENT file's geometry and must drop it
    /// now. Separate from load_gcode because the reload is deferred: until it
    /// lands the viewer keeps rendering whatever it already holds.
    bool clear_gcode = false;
};

/**
 * @brief Decide what to (re)load so the preview matches the desired print.
 *
 * Pure function (no LVGL deps). The caller reads the *actual* widget state
 * (does the thumbnail have an image source? does the viewer hold geometry?)
 * and the desired state (the current print's effective filename, view mode,
 * lifecycle intent) and this function reconciles the two. Because the decision
 * is driven by real widget state rather than intent bools, re-entry is
 * self-healing: a blank widget always reloads.
 *
 * @param thumbnail_displayed_file File whose content is CURRENTLY in the
 *                          thumbnail widget ("" = none/blank).
 * @param gcode_displayed_file     File whose geometry is CURRENTLY in the gcode
 *                          viewer ("" = none/blank). Tracked separately from the
 *                          thumbnail: the thumbnail subject observer can advance
 *                          its marker (even while the panel is hidden) long
 *                          before the deferred gcode load runs, so the gcode
 *                          mismatch MUST be computed against this marker, not the
 *                          thumbnail's, or a stale render is left on screen.
 * @param desired_file      The current print's effective filename
 *                          ("" = nothing to show).
 * @param thumbnail_has_src Does the thumbnail widget currently have an image
 *                          source.
 * @param gcode_has_content Does the gcode viewer currently hold geometry.
 * @param want_viewer       Lifecycle wants the 3D/2D viewer for the current
 *                          print state (independent of the render-mode setting).
 * @return Which resources to (re)load, and whether the viewer must drop
 *         geometry it holds for a different file before that happens.
 */
inline PreviewAction decide_preview_action(const std::string& thumbnail_displayed_file,
                                           const std::string& gcode_displayed_file,
                                           const std::string& desired_file, bool thumbnail_has_src,
                                           bool gcode_has_content, bool want_viewer) {
    PreviewAction action{};

    // Nothing to show: no print selected. Leave widgets untouched.
    if (desired_file.empty()) {
        return action;
    }

    // The two assets are reconciled against their OWN markers. The thumbnail's
    // marker can advance to the new print before the gcode viewer's does (the
    // thumbnail subject observer fires even while the panel is hidden, while the
    // gcode load is deferred and only scheduled when active), so a shared marker
    // would let the thumbnail mask a stale gcode render from the previous print.
    const bool thumbnail_mismatch = (thumbnail_displayed_file != desired_file);
    const bool gcode_mismatch = (gcode_displayed_file != desired_file);

    // Thumbnail is always the fallback content beneath the viewer. (Re)load it
    // whenever its displayed file differs from desired or the widget is blank.
    if (thumbnail_mismatch || !thumbnail_has_src) {
        action.load_thumbnail = true;
    }

    // Stale geometry leaves the screen NOW, which is a different question from
    // when to fetch its replacement. The gcode load is deliberately deferred
    // (seconds, while the printer is still preparing, to avoid a memory spike)
    // and the viewer goes on rendering what it holds in the meantime — so on a
    // new print the PREVIOUS print's model stays on screen for the whole
    // deferral. Not gated on want_viewer: the wrong model is wrong on screen
    // whether or not we intend to replace it.
    if (gcode_has_content && gcode_mismatch) {
        action.clear_gcode = true;
    }

    // Gcode geometry (re)loads whenever the lifecycle wants the viewer and the
    // viewer's file differs or it holds no geometry. Do NOT gate on the current
    // view-mode subject: the mode only flips to 3D/2D AFTER the gcode loads, so
    // gating here would deadlock the load and pin the preview to the thumbnail.
    // The render-mode setting (thumbnail-only / 3D-disabled) is enforced
    // downstream in load_gcode_for_viewing().
    if (want_viewer && (gcode_mismatch || !gcode_has_content)) {
        action.load_gcode = true;
    }

    return action;
}

} // namespace helix::ui
