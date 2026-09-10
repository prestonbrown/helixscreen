// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "ui_wizard_touch_calibration.h"

#include "ui_effects.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"

#include "config.h"
#include "display_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "touch_calibration_layout.h"
#include "touch_calibration_wrapper.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::ui::create_ripple;

// ============================================================================
// Constants
// ============================================================================

// Crosshair widget size in pixels (defined in XML as 50x50)
constexpr int CROSSHAIR_SIZE = 50;
constexpr int CROSSHAIR_HALF_SIZE = CROSSHAIR_SIZE / 2;

// External wizard subjects (defined in ui_wizard.cpp)
extern lv_subject_t connection_test_passed;
extern lv_subject_t wizard_show_skip;
extern lv_subject_t wizard_subtitle;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardTouchCalibrationStep> g_wizard_touch_calibration_step;

// Flag to force touch calibration step to show (for visual testing on SDL)
static bool g_force_touch_calibration_step = false;

void force_touch_calibration_step(bool force) {
    g_force_touch_calibration_step = force;
    if (force) {
        spdlog::debug("[WizardTouchCalibration] Force-showing step for visual testing");
    }
}

WizardTouchCalibrationStep* get_wizard_touch_calibration_step() {
    if (!g_wizard_touch_calibration_step) {
        g_wizard_touch_calibration_step = std::make_unique<WizardTouchCalibrationStep>();
        StaticPanelRegistry::instance().register_destroy(
            "WizardTouchCalibrationStep", []() { g_wizard_touch_calibration_step.reset(); });
    }
    return g_wizard_touch_calibration_step.get();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardTouchCalibrationStep::WizardTouchCalibrationStep() {
    controller_.attach(*this);
    // The screen size is NOT sampled here. This step is a singleton built when
    // the wizard registry is, and a display rotated after that would leave every
    // crosshair at the wrong ratio. The controller re-samples on each begin().
    helix::TouchCalibrationPanel* panel = controller_.panel();

    // Set completion callback
    panel->set_completion_callback(
        [this](const helix::TouchCalibration* cal) { on_calibration_complete(cal); });

    // Set failure callback for degenerate points (collinear/duplicate)
    // Panel auto-restarts to POINT_1, we show error with step instruction
    panel->set_failure_callback([this](const char* reason) {
        spdlog::warn("[{}] Calibration failed: {}", get_name(), reason);

        if (screen_root_) {
            calibration_failed_ = true;
            update_instruction_text(); // Will concatenate error + step
            update_crosshair_position();
            update_button_visibility();
        }
    });

    // Set up sample progress callback for UI updates
    panel->set_sample_progress_callback([this]() { update_instruction_text(); });

    // Auto-accept the instant the panel enters VERIFY. Wired to the panel's
    // verify-entry hook (NOT the press handler) so it fires on whichever commit
    // path actually reaches VERIFY — release event, 600ms stall timer, or legacy
    // sample-on-press. The #943 debounce moved the POINT_3->VERIFY transition off
    // the press edge to on_release()/stall; the old press-handler-only check then
    // never ran on clean capacitive panels (Goodix/Q2), hanging the wizard forever
    // on "Computing calibration..." (#1029).
    panel->set_verify_entry_callback([this]() { on_verify_entered(); });

    // Note: No countdown/timeout/fast-revert callbacks needed for wizard mode.
    // The wizard auto-accepts calibration immediately upon entering VERIFY state
    // (see on_verify_entered), so these timers never fire.

    spdlog::debug("[{}] Instance created", get_name());
}

WizardTouchCalibrationStep::~WizardTouchCalibrationStep() {
    // Deinit subjects before memory is freed — removes observers from LVGL widgets
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&calibration_valid_);
        lv_subject_deinit(&current_step_);
        subjects_initialized_ = false;
    }
    screen_root_ = nullptr;
}

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardTouchCalibrationStep::init_subjects() {
    // Guard against double initialization
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized, skipping", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Note: instruction text now uses wizard_subtitle (in header) instead of local subject
    UI_SUBJECT_INIT_AND_REGISTER_INT(current_step_, 0, "touch_cal_current_step");
    UI_SUBJECT_INIT_AND_REGISTER_INT(calibration_valid_, 0, "touch_cal_valid");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardTouchCalibrationStep::register_callbacks() {
    spdlog::debug("[{}] Registering callbacks", get_name());

    lv_xml_register_event_cb(nullptr, "on_touch_cal_accept_clicked", on_accept_clicked_static);
    lv_xml_register_event_cb(nullptr, "on_touch_cal_retry_clicked", on_retry_clicked_static);
    lv_xml_register_event_cb(nullptr, "on_touch_cal_screen_touched", on_screen_touched_static);
    lv_xml_register_event_cb(nullptr, "on_touch_cal_screen_released", on_screen_released_static);
    lv_xml_register_event_cb(nullptr, "on_touch_cal_test_area_touched",
                             on_test_area_touched_static);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardTouchCalibrationStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating touch calibration screen", get_name());

    // Safety check: cleanup should have been called by wizard navigation
    if (screen_root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        screen_root_ = nullptr;
    }

    // Create screen from XML
    screen_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_touch_calibration", nullptr));
    if (!screen_root_) {
        spdlog::error("[{}] Failed to create screen from XML", get_name());
        return nullptr;
    }

    // Reparent the crosshair + touch capture surface onto the active screen so
    // the capture surface spans the WHOLE screen (targets in the header/footer
    // strip stay tappable) and both use screen-absolute coordinates. Shared with
    // the Settings recalibration overlay — see touch_calibration_layout.h.
    helix::ui::CaptureSurfaceWidgets cap =
        helix::ui::reparent_capture_surface_fullscreen(screen_root_);
    crosshair_ = cap.crosshair;
    if (!crosshair_) {
        spdlog::error("[{}] Crosshair widget not found in XML", get_name());
        return screen_root_;
    }

    // Lift the Next/Skip button group above the full-screen capture surface so it
    // remains clickable during calibration. The group holds both Next and Skip
    // (toggled by wizard_show_skip), so lifting the group keeps both accessible.
    raised_skip_ = helix::ui::raise_control_above_capture(lv_screen_active(), "next_skip_group");

    // Find test area widgets (shown in COMPLETE state)
    test_area_container_ = lv_obj_find_by_name(screen_root_, "test_area_container");
    test_touch_area_ = lv_obj_find_by_name(screen_root_, "test_touch_area");

    // Center the wizard subtitle for this step (keeps it clear of crosshair targets)
    lv_obj_t* subtitle = lv_obj_find_by_name(lv_screen_active(), "wizard_subtitle");
    if (subtitle) {
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
    }

    // Re-set callbacks (cleanup() clears completion callback to prevent
    // updates to destroyed UI, so we must restore it for each create cycle)
    if (controller_.panel()) {
        controller_.panel()->set_completion_callback(
            [this](const helix::TouchCalibration* cal) { on_calibration_complete(cal); });
        controller_.panel()->set_failure_callback([this](const char* reason) {
            spdlog::warn("[{}] Calibration failed: {}", get_name(), reason);
            if (screen_root_) {
                calibration_failed_ = true;
                update_instruction_text();
                update_crosshair_position();
                update_button_visibility();
            }
        });
        // Re-arm auto-accept (mirrors completion/failure above; the panel's
        // verify-entry hook drives VERIFY->COMPLETE on every commit path, #1029).
        controller_.panel()->set_verify_entry_callback([this]() { on_verify_entered(); });
    }

    // Re-sample the screen, snapshot the live calibration and disable the affine
    // so capture sees raw (post-LVGL-linear) coordinates: leaving a bad calibration
    // in place would transform them and make recalibration produce garbage. This step
    // accepts as soon as VERIFY is reached (on_verify_entered) and defers writing
    // until 'Next', so it neither runs an interactive verify phase nor commits here.
    // end() (in cleanup) puts it all back.
    controller_.begin();

    // Enable Next button and set initial text to "Skip"
    lv_subject_set_int(&connection_test_passed, 1);
    lv_subject_set_int(&wizard_show_skip, 1);

    // Update UI for calibration state
    update_instruction_text();
    update_crosshair_position();
    update_button_visibility();

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardTouchCalibrationStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    // Reset button text to "Next" (in case user skipped without completing)
    lv_subject_set_int(&wizard_show_skip, 0);

    // Reset wizard subtitle alignment back to left (was centered for this step)
    lv_obj_t* subtitle = lv_obj_find_by_name(lv_screen_active(), "wizard_subtitle");
    if (subtitle) {
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_LEFT, 0);
    }

    // Restore Next/Skip group to its original parent before deleting reparented widgets
    helix::ui::restore_raised_control(raised_skip_);
    raised_skip_ = {};

    // Delete crosshair (it was reparented to screen, not part of screen_root_)
    helix::ui::safe_delete(crosshair_);

    // Delete touch overlay (it was also reparented to screen)
    lv_obj_t* touch_overlay = lv_obj_find_by_name(lv_screen_active(), "touch_capture_overlay");
    helix::ui::safe_delete(touch_overlay);

    // Clear widget pointers FIRST to prevent UI updates during cleanup
    // (test area widgets are children of screen_root_, so they're deleted with it)
    test_area_container_ = nullptr;
    test_touch_area_ = nullptr;
    screen_root_ = nullptr;

    // Drop the callbacks BEFORE unwinding: they capture `this` and a stall timer
    // left armed by a mid-press navigation would otherwise reach a torn-down UI
    // (#1029). end() then restores the pre-session calibration, re-enables the
    // affine and fully disarms the panel.
    if (controller_.panel()) {
        controller_.panel()->set_completion_callback(nullptr);
        controller_.panel()->set_verify_entry_callback(nullptr);
        controller_.panel()->reset();
    }
    controller_.end();

    // Clear pending calibration (user skipped or went back)
    controller_.clear_pending();
}

// ============================================================================
// Commit Calibration (called when user clicks 'Next')
// ============================================================================

bool WizardTouchCalibrationStep::commit_calibration() {
    if (!controller_.has_pending()) {
        spdlog::debug("[{}] No pending calibration to commit", get_name());
        return false;
    }

    // This is where the evdev range is first re-programmed: everything up to
    // 'Next' is revertible through the session, which only handles the affine.
    const helix::ui::CommitOutcome outcome = controller_.commit();
    if (outcome == helix::ui::CommitOutcome::NoCalibration) {
        return false;
    }
    spdlog::info("[{}] Calibration committed to config", get_name());
    return true;
}

bool WizardTouchCalibrationStep::should_skip() const {
    // Force show if explicitly requested (for visual testing on SDL)
    if (g_force_touch_calibration_step) {
        spdlog::info("[{}] Force-showing: --wizard-step 0 requested", get_name());
        return false;
    }

    // Skip if not on framebuffer display
#ifndef HELIX_DISPLAY_FBDEV
    spdlog::info("[{}] Skipping touch calibration: not a framebuffer build", get_name());
    return true;
#endif

    // Skip if touch device doesn't need calibration (e.g., USB HID touchscreen)
    // USB HID touchscreens (HDMI displays) report mapped coordinates natively.
    //
    // needs_ is deliberate here and must NOT become supports_: this is the
    // auto-fire decision, and it stays conservative so the wizard doesn't
    // ambush users whose touch already works. supports_ is the wider predicate
    // behind the manual Settings entry point, which is how a user with an
    // undetectable orientation mismatch gets in (prestonbrown/helixscreen#1259).
    DisplayManager* dm = DisplayManager::instance();
    if (dm && !dm->needs_touch_calibration()) {
        spdlog::info("[{}] Skipping touch calibration: device doesn't require it", get_name());
        return true;
    }

    // Skip if already calibrated
    Config* config = Config::get_instance();
    if (config && config->get<bool>("/input/calibration/valid", false)) {
        spdlog::info("[{}] Skipping touch calibration: already calibrated", get_name());
        return true;
    }

    spdlog::info("[{}] Touch calibration needed — showing wizard step", get_name());
    return false;
}

// ============================================================================
// Static Event Handlers (Trampolines)
// ============================================================================

void WizardTouchCalibrationStep::on_accept_clicked_static(lv_event_t* e) {
    (void)e;
    get_wizard_touch_calibration_step()->handle_accept_clicked();
}

void WizardTouchCalibrationStep::on_retry_clicked_static(lv_event_t* e) {
    (void)e;
    get_wizard_touch_calibration_step()->handle_retry_clicked();
}

void WizardTouchCalibrationStep::on_screen_touched_static(lv_event_t* e) {
    get_wizard_touch_calibration_step()->handle_screen_touched(e);
}

void WizardTouchCalibrationStep::on_screen_released_static(lv_event_t* e) {
    (void)e;
    get_wizard_touch_calibration_step()->handle_screen_released();
}

void WizardTouchCalibrationStep::on_test_area_touched_static(lv_event_t* e) {
    get_wizard_touch_calibration_step()->handle_test_area_touched(e);
}

// ============================================================================
// Instance Event Handlers
// ============================================================================

void WizardTouchCalibrationStep::handle_accept_clicked() {
    spdlog::info("[{}] Accept calibration clicked", get_name());

    if (!controller_.panel()) {
        return;
    }

    // Accept triggers the completion callback with calibration data
    controller_.panel()->accept();
}

void WizardTouchCalibrationStep::handle_retry_clicked() {
    spdlog::info("[{}] Retry calibration clicked", get_name());

    if (!controller_.panel()) {
        return;
    }

    // Revert the just-applied affine and re-disable it BEFORE re-capturing, so the
    // re-run reads raw (post-LVGL-linear) coordinates instead of feeding the new
    // (possibly bad) affine back through the transform — a feedback loop that made
    // recalibration produce garbage. Mirrors the Settings overlay retry
    // (ui_touch_calibration_overlay.cpp) and the begin_capture() disable in
    // create() (#943). The backup is retained across retry.
    controller_.retry();

    // Reset button text back to "Skip" since calibration is starting over
    lv_subject_set_int(&wizard_show_skip, 1);

    lv_subject_set_int(&current_step_, 0);
    lv_subject_set_int(&calibration_valid_, 0);
    update_instruction_text();
    update_crosshair_position();
    update_button_visibility();
}

void WizardTouchCalibrationStep::handle_screen_touched(lv_event_t* e) {
    (void)e; // The controller reads the position from the active input device
    if (!screen_root_) {
        return;
    }
    controller_.on_press();
}

void WizardTouchCalibrationStep::handle_screen_released() {
    controller_.on_release();
}

void WizardTouchCalibrationStep::on_progress() {
    update_instruction_text();
    update_crosshair_position();
    update_button_visibility();
}

void WizardTouchCalibrationStep::on_capture_feedback(helix::Point landed) {
    // The wizard marks the tap on the crosshair only; the lingering dot the
    // Settings overlay draws is a feature of that view, not of calibration.
    (void)landed;
    if (crosshair_) {
        helix::ui::flash_object(crosshair_, 200, true);
    }
}

void WizardTouchCalibrationStep::on_verify_entered() {
    if (!controller_.panel()) {
        return;
    }

    // The wizard has no interactive verify step — accept the freshly computed
    // calibration immediately. Fired from the panel's verify-entry hook, which
    // runs on every commit path (#1029), so this is the single place auto-accept
    // happens regardless of how POINT_3 committed (release / stall / press).
    spdlog::info("[{}] Auto-accepting calibration (wizard mode)", get_name());
    controller_.panel()->accept();

    // Refresh UI only when a live screen exists. accept()'s completion callback
    // (on_calibration_complete) is itself screen_root_-guarded; mirror that here
    // so the verify-entry hook is safe to fire from the stall timer before the
    // screen is built, and from unit tests that drive the panel without UI.
    if (screen_root_) {
        update_instruction_text();
        update_crosshair_position();
        update_button_visibility();
    }
}

void WizardTouchCalibrationStep::handle_test_area_touched(lv_event_t* e) {
    (void)e;

    if (!test_touch_area_) {
        return;
    }

    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Convert screen coords to test_touch_area local coords
    // lv_obj_get_coords returns screen-absolute coordinates of the object
    lv_area_t area_coords;
    lv_obj_get_coords(test_touch_area_, &area_coords);
    lv_coord_t local_x = point.x - area_coords.x1;
    lv_coord_t local_y = point.y - area_coords.y1;

    spdlog::debug("[{}] Test area touched at screen ({}, {}), local ({}, {})", get_name(), point.x,
                  point.y, local_x, local_y);

    create_ripple_at(local_x, local_y);
}

void WizardTouchCalibrationStep::create_ripple_at(lv_coord_t x, lv_coord_t y) {
    if (!test_touch_area_) {
        return;
    }
    create_ripple(test_touch_area_, x, y);
}

// ============================================================================
// Calibration Complete Callback
// ============================================================================

void WizardTouchCalibrationStep::on_calibration_complete(const helix::TouchCalibration* cal) {
    // Guard against callback during cleanup (screen_root_ is nulled first in cleanup())
    if (!screen_root_) {
        spdlog::debug("[{}] Ignoring callback during cleanup", get_name());
        return;
    }

    if (cal && cal->valid) {
        // Additional validation: check coefficients are finite and within bounds
        if (!helix::is_calibration_valid(*cal)) {
            spdlog::error("[{}] Calibration coefficients failed validation (NaN/Inf/out of bounds)",
                          get_name());

            // Mark failure so update_instruction_text() shows error with step
            calibration_failed_ = true;

            lv_subject_set_int(&calibration_valid_, 0);
            lv_subject_set_int(&wizard_show_skip, 1);

            controller_.panel()->start();
            update_instruction_text(); // Will concatenate error + step
            update_crosshair_position();
            update_button_visibility();
            return;
        }

        spdlog::info("[{}] Calibration complete and valid", get_name());

        // Store calibration for later commit (saved only when user clicks 'Next')
        controller_.stash_pending(*cal, controller_.panel()->get_range_fit());
        spdlog::debug("[{}] Calibration stored (will save when 'Next' is clicked)", get_name());

        // The pre-session calibration was already snapshotted in create() via
        // controller_.session().begin_capture(); apply the new one immediately (no restart
        // required). controller_.session().restore() reverts it if the user backs out before
        // committing on 'Next'.
        DisplayManager* dm = DisplayManager::instance();
        if (dm) {
            if (dm->apply_touch_calibration(*cal)) {
                spdlog::info("[{}] Calibration applied to touch input", get_name());
            } else {
                spdlog::debug("[{}] Could not apply calibration immediately (may require restart)",
                              get_name());
            }
        }

        lv_subject_set_int(&calibration_valid_, 1);

        // Update header subtitle to show success
        lv_subject_copy_string(&wizard_subtitle,
                               lv_tr("Calibration complete! Press 'Next' to continue."));

        // Change button text from "Skip" to "Next" since calibration is complete
        lv_subject_set_int(&wizard_show_skip, 0);
    } else {
        spdlog::warn("[{}] Calibration cancelled or invalid", get_name());
        lv_subject_set_int(&calibration_valid_, 0);
    }

    update_instruction_text();
    update_button_visibility();
}

// ============================================================================
// UI Update Helpers
// ============================================================================

void WizardTouchCalibrationStep::update_instruction_text() {
    if (!controller_.panel()) {
        return;
    }

    auto p = controller_.panel()->get_progress();

    // Clear failure flag once user successfully captures a point (moved past POINT_1)
    if (p.state != helix::TouchCalibrationPanel::State::POINT_1 &&
        p.state != helix::TouchCalibrationPanel::State::IDLE) {
        calibration_failed_ = false;
    }

    switch (p.state) {
    case helix::TouchCalibrationPanel::State::IDLE:
        lv_subject_copy_string(&wizard_subtitle, lv_tr("Tap anywhere to begin calibration"));
        return;
    case helix::TouchCalibrationPanel::State::VERIFY:
        lv_subject_copy_string(&wizard_subtitle, lv_tr("Computing calibration..."));
        return;
    case helix::TouchCalibrationPanel::State::COMPLETE:
        // Don't overwrite — on_calibration_complete sets the success message
        return;
    default:
        break;
    }

    // POINT states — show which touch is next (1-indexed)
    // current_sample=0 → "touch 1 of 7" (waiting for first), current_sample=1 → "touch 2 of 7",
    // etc.
    char step_text[128];
    // TRANSLATORS: %1$d = point number (1-3), %2$d = next touch number (1-7), %3$d = total
    snprintf(step_text, sizeof(step_text),
             lv_tr("Touch the target (point %1$d of 3) \xe2\x80\x94 touch %2$d of %3$d"),
             p.point_num, p.current_sample + 1, p.total_samples);

    // Prepend error message if calibration just failed
    if (calibration_failed_ && p.state == helix::TouchCalibrationPanel::State::POINT_1) {
        char combined[256];
        snprintf(combined, sizeof(combined), "%s %s",
                 lv_tr("Calibration failed - touch targets more precisely."), step_text);
        lv_subject_copy_string(&wizard_subtitle, combined);
    } else {
        lv_subject_copy_string(&wizard_subtitle, step_text);
    }
}

void WizardTouchCalibrationStep::ensure_skip_on_top() {
    // After any lv_obj_move_foreground(touch_overlay), the skip group may end up
    // behind the overlay. Re-assert its z-order so it stays clickable.
    if (raised_skip_.obj) {
        lv_obj_move_foreground(raised_skip_.obj);
    }
}

void WizardTouchCalibrationStep::update_crosshair_position() {
    if (!controller_.panel()) {
        return;
    }

    // Touch overlay was reparented to screen for full-screen capture
    lv_obj_t* touch_overlay = lv_obj_find_by_name(lv_screen_active(), "touch_capture_overlay");

    auto state = controller_.panel()->get_state();

    // IDLE: show touch overlay (for "tap anywhere to begin") but hide crosshair
    if (state == helix::TouchCalibrationPanel::State::IDLE) {
        if (crosshair_) {
            lv_obj_add_flag(crosshair_, LV_OBJ_FLAG_HIDDEN);
        }
        if (touch_overlay) {
            lv_obj_remove_flag(touch_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(touch_overlay);
        }
        ensure_skip_on_top();
        return;
    }

    // Hide crosshair and touch overlay in VERIFY and COMPLETE states
    if (state == helix::TouchCalibrationPanel::State::VERIFY ||
        state == helix::TouchCalibrationPanel::State::COMPLETE) {
        if (crosshair_) {
            lv_obj_add_flag(crosshair_, LV_OBJ_FLAG_HIDDEN);
        }
        if (touch_overlay) {
            lv_obj_add_flag(touch_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Show crosshair and touch overlay for calibration points
    if (crosshair_) {
        lv_obj_remove_flag(crosshair_, LV_OBJ_FLAG_HIDDEN);
    }
    if (touch_overlay) {
        lv_obj_remove_flag(touch_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(touch_overlay); // Keep on top for click capture
    }
    ensure_skip_on_top();

    const int step = controller_.active_target_index();
    if (step < 0) {
        return;
    }
    const helix::Point target = controller_.active_target_position();

    // Crosshair is a direct child of the screen, so we can use screen-absolute coordinates
    if (crosshair_) {
        lv_obj_set_pos(crosshair_, target.x - CROSSHAIR_HALF_SIZE, target.y - CROSSHAIR_HALF_SIZE);
    }
    lv_subject_set_int(&current_step_, step);

    spdlog::debug("[{}] Crosshair positioned at screen ({}, {}) for step {}", get_name(), target.x,
                  target.y, step);
}

void WizardTouchCalibrationStep::update_button_visibility() {
    if (!screen_root_ || !controller_.panel()) {
        return;
    }

    auto state = controller_.panel()->get_state();
    bool is_complete = (state == helix::TouchCalibrationPanel::State::COMPLETE);

    // Show test area container only in COMPLETE state
    if (test_area_container_) {
        if (is_complete) {
            lv_obj_remove_flag(test_area_container_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(test_area_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
