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

void destroy_wizard_touch_calibration_step() {
    g_wizard_touch_calibration_step.reset();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardTouchCalibrationStep::WizardTouchCalibrationStep() {
    // Create the calibration panel
    panel_ = std::make_unique<helix::TouchCalibrationPanel>();

    // Set screen size from DisplayManager
    DisplayManager* display_mgr = DisplayManager::instance();
    if (display_mgr && display_mgr->is_initialized()) {
        panel_->set_screen_size(display_mgr->width(), display_mgr->height());
        spdlog::debug("[{}] Screen size set to {}x{}", get_name(), display_mgr->width(),
                      display_mgr->height());
    } else {
        // Fallback to defaults
        panel_->set_screen_size(800, 480);
        spdlog::warn("[{}] DisplayManager not available, using default 800x480", get_name());
    }

    // Set completion callback
    panel_->set_completion_callback(
        [this](const helix::TouchCalibration* cal) { on_calibration_complete(cal); });

    // Set failure callback for degenerate points (collinear/duplicate)
    // Panel auto-restarts to POINT_1, we show error with step instruction
    panel_->set_failure_callback([this](const char* reason) {
        spdlog::warn("[{}] Calibration failed: {}", get_name(), reason);

        if (screen_root_) {
            calibration_failed_ = true;
            update_instruction_text(); // Will concatenate error + step
            update_crosshair_position();
            update_button_visibility();
        }
    });

    // Set up sample progress callback for UI updates
    panel_->set_sample_progress_callback([this]() { update_instruction_text(); });

    // Note: No countdown/timeout/fast-revert callbacks needed for wizard mode.
    // The wizard auto-accepts calibration immediately upon entering VERIFY state
    // (see handle_screen_touched), so these timers never fire.

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

    // Find and reparent crosshair to screen for absolute positioning
    // Calibration targets are screen-absolute coordinates, so crosshair must be
    // a direct child of the screen (not nested in wizard content container)
    crosshair_ = lv_obj_find_by_name(screen_root_, "crosshair");
    if (crosshair_) {
        lv_obj_set_parent(crosshair_, lv_screen_active());
        lv_obj_add_flag(crosshair_, LV_OBJ_FLAG_FLOATING); // Ensure no layout interference
        spdlog::debug("[{}] Crosshair reparented to screen for absolute positioning", get_name());
    }

    // Reparent touch capture overlay to screen for full-screen touch capture
    // This allows calibration targets in header/footer areas to be tappable
    lv_obj_t* touch_overlay = lv_obj_find_by_name(screen_root_, "touch_capture_overlay");
    if (touch_overlay) {
        lv_obj_set_parent(touch_overlay, lv_screen_active());
        lv_obj_set_size(touch_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(touch_overlay, 0, 0);
        lv_obj_add_flag(touch_overlay, LV_OBJ_FLAG_FLOATING);
        lv_obj_move_foreground(touch_overlay); // Ensure it's on top for click capture
        spdlog::debug("[{}] Touch overlay reparented to screen for full-screen capture",
                      get_name());
    }

    // Find test area widgets (shown in COMPLETE state)
    test_area_container_ = lv_obj_find_by_name(screen_root_, "test_area_container");
    test_touch_area_ = lv_obj_find_by_name(screen_root_, "test_touch_area");

    // Center the wizard subtitle for this step (keeps it clear of crosshair targets)
    lv_obj_t* subtitle = lv_obj_find_by_name(lv_screen_active(), "wizard_subtitle");
    if (subtitle) {
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
    }

    // Start in IDLE — first tap anywhere begins calibration
    if (panel_) {
        panel_->cancel(); // Reset to IDLE
    }

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

    // Reset panel state - clear callback before cancel to prevent updates to
    // destroyed UI widgets (callback would call update_instruction_text() etc.)
    if (panel_) {
        panel_->set_completion_callback(nullptr);
        panel_->cancel();
    }

    // Clear pending calibration (user skipped or went back)
    has_pending_calibration_ = false;
    has_backup_ = false;
}

// ============================================================================
// Commit Calibration (called when user clicks 'Next')
// ============================================================================

bool WizardTouchCalibrationStep::commit_calibration() {
    if (!has_pending_calibration_) {
        spdlog::debug("[{}] No pending calibration to commit", get_name());
        return false;
    }

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::error("[{}] Cannot commit calibration: Config not available", get_name());
        return false;
    }

    config->set<bool>("/input/calibration/valid", true);
    config->set<double>("/input/calibration/a", static_cast<double>(pending_calibration_.a));
    config->set<double>("/input/calibration/b", static_cast<double>(pending_calibration_.b));
    config->set<double>("/input/calibration/c", static_cast<double>(pending_calibration_.c));
    config->set<double>("/input/calibration/d", static_cast<double>(pending_calibration_.d));
    config->set<double>("/input/calibration/e", static_cast<double>(pending_calibration_.e));
    config->set<double>("/input/calibration/f", static_cast<double>(pending_calibration_.f));
    config->set<bool>("/input/calibration/swap_axes", panel_->axes_swapped());
    config->save();

    spdlog::info("[{}] Calibration committed to config", get_name());
    has_pending_calibration_ = false;
    has_backup_ = false; // Calibration committed, no need to restore
    return true;
}

// ============================================================================
// Skip Logic
// ============================================================================

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
    // USB HID touchscreens (HDMI displays) report mapped coordinates natively
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

void WizardTouchCalibrationStep::on_test_area_touched_static(lv_event_t* e) {
    get_wizard_touch_calibration_step()->handle_test_area_touched(e);
}

// ============================================================================
// Instance Event Handlers
// ============================================================================

void WizardTouchCalibrationStep::handle_accept_clicked() {
    spdlog::info("[{}] Accept calibration clicked", get_name());

    if (!panel_) {
        return;
    }

    // Accept triggers the completion callback with calibration data
    panel_->accept();
}

void WizardTouchCalibrationStep::handle_retry_clicked() {
    spdlog::info("[{}] Retry calibration clicked", get_name());

    if (!panel_) {
        return;
    }

    // Use cancel()+start() rather than retry() because the wizard auto-accepts
    // calibration, so the panel may already be in COMPLETE state where retry()
    // (which guards on VERIFY) would be a no-op.
    panel_->cancel();
    panel_->start();

    // Clear pending calibration since user is recalibrating
    has_pending_calibration_ = false;

    // Reset button text back to "Skip" since calibration is starting over
    lv_subject_set_int(&wizard_show_skip, 1);

    lv_subject_set_int(&current_step_, 0);
    lv_subject_set_int(&calibration_valid_, 0);
    update_instruction_text();
    update_crosshair_position();
    update_button_visibility();
}

void WizardTouchCalibrationStep::handle_screen_touched(lv_event_t* e) {
    (void)e; // Event not used directly - we get touch position from active input device

    if (!panel_ || !screen_root_) {
        return;
    }

    // Get click position relative to the screen
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    auto state_before = panel_->get_state();
    spdlog::info("[{}] Screen touched at ({}, {}) during state {}", get_name(), point.x, point.y,
                 static_cast<int>(state_before));

    // add_sample() handles IDLE→POINT_1 auto-start and sample collection
    panel_->add_sample({point.x, point.y});

    // Flash crosshair for visual tap feedback (only during calibration points,
    // not on the initial "tap anywhere to begin" transition from IDLE)
    auto state = panel_->get_state();
    if (crosshair_ && state_before != helix::TouchCalibrationPanel::State::IDLE &&
        (state == helix::TouchCalibrationPanel::State::POINT_1 ||
         state == helix::TouchCalibrationPanel::State::POINT_2 ||
         state == helix::TouchCalibrationPanel::State::POINT_3)) {
        helix::ui::flash_object(crosshair_);
    }

    // Auto-accept when VERIFY state is reached (wizard doesn't need user to click Accept)
    if (panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY) {
        spdlog::info("[{}] Auto-accepting calibration (wizard mode)", get_name());
        panel_->accept();
    }

    // Update UI for next step
    update_instruction_text();
    update_crosshair_position();
    update_button_visibility();
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

            panel_->start();
            update_instruction_text(); // Will concatenate error + step
            update_crosshair_position();
            update_button_visibility();
            return;
        }

        spdlog::info("[{}] Calibration complete and valid", get_name());

        // Store calibration for later commit (saved only when user clicks 'Next')
        pending_calibration_ = *cal;
        has_pending_calibration_ = true;
        spdlog::debug("[{}] Calibration stored (will save when 'Next' is clicked)", get_name());

        // Backup current calibration before applying new one
        DisplayManager* dm = DisplayManager::instance();
        backup_calibration_ = dm->get_current_calibration();
        has_backup_ = true;

        // Apply calibration immediately (no restart required)
        if (dm && dm->apply_touch_calibration(*cal)) {
            spdlog::info("[{}] Calibration applied to touch input", get_name());
        } else {
            spdlog::debug("[{}] Could not apply calibration immediately (may require restart)",
                          get_name());
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
    if (!panel_) {
        return;
    }

    auto p = panel_->get_progress();

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

void WizardTouchCalibrationStep::update_crosshair_position() {
    if (!panel_) {
        return;
    }

    // Touch overlay was reparented to screen for full-screen capture
    lv_obj_t* touch_overlay = lv_obj_find_by_name(lv_screen_active(), "touch_capture_overlay");

    auto state = panel_->get_state();

    // IDLE: show touch overlay (for "tap anywhere to begin") but hide crosshair
    if (state == helix::TouchCalibrationPanel::State::IDLE) {
        if (crosshair_) {
            lv_obj_add_flag(crosshair_, LV_OBJ_FLAG_HIDDEN);
        }
        if (touch_overlay) {
            lv_obj_remove_flag(touch_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(touch_overlay);
        }
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

    int step = 0;
    switch (state) {
    case helix::TouchCalibrationPanel::State::POINT_1:
        step = 0;
        break;
    case helix::TouchCalibrationPanel::State::POINT_2:
        step = 1;
        break;
    case helix::TouchCalibrationPanel::State::POINT_3:
        step = 2;
        break;
    default:
        return;
    }

    helix::Point target = panel_->get_target_position(step);

    // Crosshair is a direct child of the screen, so we can use screen-absolute coordinates
    if (crosshair_) {
        lv_obj_set_pos(crosshair_, target.x - CROSSHAIR_HALF_SIZE, target.y - CROSSHAIR_HALF_SIZE);
    }
    lv_subject_set_int(&current_step_, step);

    spdlog::debug("[{}] Crosshair positioned at screen ({}, {}) for step {}", get_name(), target.x,
                  target.y, step);
}

void WizardTouchCalibrationStep::update_button_visibility() {
    if (!screen_root_ || !panel_) {
        return;
    }

    auto state = panel_->get_state();
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
