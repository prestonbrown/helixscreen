// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "print_lifecycle_state.h"

#include <cstdint>
#include <string>
#include <utility>

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

/// Render modes as stored in detail_gcode_viewer_mode: thumbnail, 3D, 2D.
constexpr int kPreviewModeThumbnail = 0;
constexpr int kPreviewMode3d = 1;
constexpr int kPreviewMode2d = 2;

/// The pixel size a hidden preview should build at.
///
/// LV_OBJ_FLAG_HIDDEN takes a widget out of layout, so a hidden viewer measures
/// zero however its width is declared. It fills its parent, so the parent's
/// content box is the size it will occupy the instant it is revealed - building
/// at that avoids a resize-and-rebuild on the first visible frame.
///
/// @return {w, h}, or {0, 0} when neither is usable yet.
constexpr std::pair<int, int> preview_build_size(int own_w, int own_h, int parent_w, int parent_h) {
    if (own_w > 0 && own_h > 0) {
        return {own_w, own_h};
    }
    if (parent_w > 0 && parent_h > 0) {
        return {parent_w, parent_h};
    }
    return {0, 0};
}

/**
 * @brief Should the preview's viewer widget be hidden right now?
 *
 * The 2D viewer is hidden until it has real content and the thumbnail carries
 * the wait, so the two swap rather than one being drawn underneath the other -
 * an occluded viewer is still composited every frame, which is the cost that
 * matters on a slow panel. Its build is driven by a timer while hidden.
 *
 * 3D is NOT hidden while it works: it uploads its VBOs during the draw pass, so
 * a hidden 3D viewer would never upload, never finish, and never reveal. It
 * stays visible under the thumbnail and reveals on its first complete frame.
 */
constexpr bool preview_viewer_hidden(int mode, bool has_first_frame) {
    if (mode == kPreviewModeThumbnail) {
        return true;
    }
    return mode == kPreviewMode2d && !has_first_frame;
}

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
 * @param viewer_enabled    The render-mode setting permits the viewer at all.
 *                          False is Thumbnail Only, where the G-code is never
 *                          fetched, indexed or rendered. Gates ONLY load_gcode:
 *                          the thumbnail is the content the user sees in that
 *                          mode, and stale geometry still has to leave the
 *                          screen.
 * @return Which resources to (re)load, and whether the viewer must drop
 *         geometry it holds for a different file before that happens.
 */
inline PreviewAction decide_preview_action(const std::string& thumbnail_displayed_file,
                                           const std::string& gcode_displayed_file,
                                           const std::string& desired_file, bool thumbnail_has_src,
                                           bool gcode_has_content, bool want_viewer,
                                           bool viewer_enabled) {
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

    // Gcode geometry (re)loads whenever the lifecycle wants the viewer, the
    // render mode admits one, and the viewer's file differs or it holds no
    // geometry. Do NOT gate on the current view-mode subject: the mode only
    // flips to 3D/2D AFTER the gcode loads, so gating here would deadlock the
    // load and pin the preview to the thumbnail. viewer_enabled is the whole
    // pipeline's switch, not a display choice — a Thumbnail Only install must
    // never download and re-read a multi-hundred-megabyte file it will not draw,
    // because that work competes with the print itself.
    if (want_viewer && viewer_enabled && (gcode_mismatch || !gcode_has_content)) {
        action.load_gcode = true;
    }

    return action;
}

/**
 * @brief Should closing the print status overlay destroy its widget tree?
 *
 * Destroying the tree gives a low-memory host back ~400-800KB, and costs the
 * preview: the next open rebuilds the thumbnail and re-renders the G-code from
 * nothing. While a job holds the machine the user comes back to that preview,
 * so the tree is kept; the memory monitor's pressure responder is what drops a
 * hidden tree when memory actually runs out.
 *
 * Asked when the overlay closes, not when the tree is created: both the print
 * and available memory move in between.
 *
 * @param low_memory MemoryInfo::is_low_memory() sampled at close time.
 * @param lifecycle  The derived lifecycle, not the wire state. A host-side
 *                   pre-start block is Preparing while print_stats still
 *                   reports the previous job's state.
 */
constexpr bool print_status_destroy_on_close(bool low_memory, PrintState lifecycle) {
    return low_memory && !job_holds_machine(lifecycle);
}

/// How a print status widget tree came to be destroyed.
enum class PrintStatusTreeDestroyCause : uint8_t {
    OverlayClose,          ///< A close print_status_destroy_on_close() said destroys it
    JobEndedWhileHidden,   ///< A tree a close kept, released once the job let go
    MemoryReclaim,         ///< The memory monitor's pressure responder
    ReplacedByRebuild,     ///< OverlayBase::rebuild() built its successor
    WidgetTreeDeleted,     ///< LVGL deleted the tree without the panel asking
    PanelRegistryTeardown, ///< StaticPanelRegistry teardown (printer switch, restart)
};

/// The cause as the destruction log line spells it.
constexpr const char* print_status_tree_destroy_cause_name(PrintStatusTreeDestroyCause cause) {
    switch (cause) {
    case PrintStatusTreeDestroyCause::OverlayClose:
        return "overlay close";
    case PrintStatusTreeDestroyCause::JobEndedWhileHidden:
        return "job ended while hidden";
    case PrintStatusTreeDestroyCause::MemoryReclaim:
        return "memory reclaim";
    case PrintStatusTreeDestroyCause::ReplacedByRebuild:
        return "replaced by a rebuild";
    case PrintStatusTreeDestroyCause::WidgetTreeDeleted:
        return "widget tree deleted";
    case PrintStatusTreeDestroyCause::PanelRegistryTeardown:
        return "panel registry teardown";
    }
    return "unknown";
}

/**
 * @brief Should a print status tree created after an earlier one log at WARN?
 *
 * A device logging at WARN sees only these lines, so WARN is kept for a rebuild
 * the user may have lost a preview to: a job holds the machine now, or held it
 * when the previous tree went, or nothing recorded how that tree went. A rebuild
 * after a panel registry teardown is expected whatever the print is doing.
 *
 * @param created_while        Lifecycle as the new tree is created.
 * @param destruction_recorded Whether the previous tree's destruction was logged.
 * @param cause                How the previous tree went; read only when recorded.
 * @param destroyed_while      Lifecycle when it went; read only when recorded.
 */
constexpr bool print_status_recreation_warns(PrintState created_while, bool destruction_recorded,
                                             PrintStatusTreeDestroyCause cause,
                                             PrintState destroyed_while) {
    if (!destruction_recorded) {
        return true;
    }
    if (cause == PrintStatusTreeDestroyCause::PanelRegistryTeardown) {
        return false;
    }
    return job_holds_machine(created_while) || job_holds_machine(destroyed_while);
}

} // namespace helix::ui
