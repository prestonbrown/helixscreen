// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

/**
 * @file touch_calibration_layout.h
 * @brief Shared full-screen capture-surface layout helpers for touch calibration.
 *
 * Both the first-run wizard (WizardTouchCalibrationStep) and the Settings
 * recalibration overlay (TouchCalibrationOverlay) capture raw touch points with
 * the affine transform disabled. For that capture to work the invisible touch
 * surface (and the crosshair that marks each target) must cover the ENTIRE screen
 * — including any header/chrome — otherwise an uncalibrated tap on a top-edge
 * target reports a coordinate that lands in the header strip, where a clickable
 * control (e.g. the header Back button) absorbs the press.
 *
 * These free functions hold the reparent-to-screen logic both callers share,
 * plus a small "lift a control above the capture surface"
 * helper used to keep an escape affordance (Next/Skip group, Cancel chip)
 * clickable while the full-screen capture surface is armed.
 */

namespace helix::ui {

/**
 * @brief Widgets located + reparented by reparent_capture_surface_fullscreen().
 *
 * The original parents are returned so a caller that REUSES its widget tree (the
 * singleton Settings overlay) can reparent them back on teardown. Callers that
 * rebuild their tree each show (the wizard) delete these widgets instead and can
 * ignore the original-parent fields.
 */
struct CaptureSurfaceWidgets {
    lv_obj_t* crosshair = nullptr;
    lv_obj_t* crosshair_original_parent = nullptr;
    lv_obj_t* capture_overlay = nullptr;
    lv_obj_t* capture_original_parent = nullptr;
};

/**
 * @brief Reparent the crosshair + touch capture surface onto the active screen.
 *
 * Finds the widgets named "crosshair" and "touch_capture_overlay" within
 * @p screen_root, reparents both to lv_screen_active(), sizes the capture surface
 * to 100%x100% at (0,0), and marks both FLOATING + foreground. After this the
 * capture surface covers the whole screen (header included) and both widgets use
 * screen-absolute coordinates.
 *
 * @param screen_root Subtree to search for the two named widgets.
 * @return The located widgets and their original parents (fields are nullptr for
 *         any widget not found).
 */
CaptureSurfaceWidgets reparent_capture_surface_fullscreen(lv_obj_t* screen_root);

/**
 * @brief Restore state for a control temporarily lifted above the capture surface.
 */
struct RaisedControl {
    lv_obj_t* obj = nullptr;
    lv_obj_t* original_parent = nullptr;
    lv_coord_t orig_w = 0;
    lv_coord_t orig_h = 0;
};

/**
 * @brief Lift a named control onto the active screen, above the capture surface.
 *
 * Finds @p name within @p search_root, records its current on-screen rect + style
 * size, reparents it to lv_screen_active() as a FLOATING child pinned at its
 * former screen position, and moves it to the foreground so it stays clickable
 * above a full-screen capture surface. Call restore_raised_control() on teardown.
 *
 * @param search_root Subtree to search for the control.
 * @param name        Widget name to lift.
 * @return Restore state; obj is nullptr if the control was not found.
 */
RaisedControl raise_control_above_capture(lv_obj_t* search_root, const char* name);

/**
 * @brief Reverse raise_control_above_capture(): reparent back, clear FLOATING,
 *        restore the original style size. No-op if @p r was never populated.
 */
void restore_raised_control(const RaisedControl& r);

} // namespace helix::ui
