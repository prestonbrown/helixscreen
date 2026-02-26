// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_touch_calibration_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_effects.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"

#include "config.h"
#include "display_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"
#include "touch_calibration.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::ui {

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<TouchCalibrationOverlay> g_touch_calibration_overlay;

TouchCalibrationOverlay& get_touch_calibration_overlay() {
    if (!g_touch_calibration_overlay) {
        g_touch_calibration_overlay = std::make_unique<TouchCalibrationOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "TouchCalibrationOverlay", []() { g_touch_calibration_overlay.reset(); });
    }
    return *g_touch_calibration_overlay;
}

// ============================================================================
// Static Trampolines for LVGL Callbacks
// ============================================================================

static void on_touch_cal_accept_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[TouchCalibrationOverlay] accept clicked");
    get_touch_calibration_overlay().handle_accept_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_touch_cal_retry_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[TouchCalibrationOverlay] retry clicked");
    get_touch_calibration_overlay().handle_retry_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_touch_cal_overlay_touched(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TouchCalibrationOverlay] screen touched");
    get_touch_calibration_overlay().handle_screen_touched(e);
    LVGL_SAFE_EVENT_CB_END();
}

static void on_touch_cal_back_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[TouchCalibrationOverlay] back clicked");
    get_touch_calibration_overlay().handle_back_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void register_touch_calibration_overlay_callbacks() {
    get_touch_calibration_overlay().register_callbacks();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

TouchCalibrationOverlay::TouchCalibrationOverlay() {
    // Zero-initialize instruction buffer
    std::memset(instruction_buffer_, 0, sizeof(instruction_buffer_));

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
        [this](const TouchCalibration* cal) { on_calibration_complete(cal); });

    // Set failure callback to notify user of degenerate points
    panel_->set_failure_callback([this](const char* reason) {
        spdlog::warn("[{}] Calibration failed: {}", get_name(), reason);
        ToastManager::instance().show(ToastSeverity::WARNING, reason, 3000);
        // State subject will be updated by capture_point flow
        update_state_subject();
        update_instruction_text();
        update_crosshair_position();
    });

    // Set up countdown callback to update Accept button text
    panel_->set_countdown_callback([this](int remaining) {
        snprintf(accept_text_buffer_, sizeof(accept_text_buffer_), "Accept (%d)", remaining);
        lv_subject_copy_string(&accept_button_text_, accept_text_buffer_);
        spdlog::debug("[{}] Countdown: {} seconds remaining", get_name(), remaining);
    });

    // Set up timeout callback to revert and restart
    panel_->set_timeout_callback([this]() {
        spdlog::info("[{}] Calibration timeout - reverting to previous", get_name());

        // Restore backup calibration
        if (has_backup_) {
            DisplayManager::instance()->apply_touch_calibration(backup_calibration_);
            has_backup_ = false;
        }

        // Reset accept button text
        snprintf(accept_text_buffer_, sizeof(accept_text_buffer_), "Accept");
        lv_subject_copy_string(&accept_button_text_, accept_text_buffer_);

        // Update instruction text
        lv_subject_copy_string(&instruction_subject_,
                               lv_tr("Calibration timed out. Please try again."));

        // Restart calibration from POINT_1
        panel_->start();
        update_state_subject();
        update_crosshair_position();
    });

    // Set up sample progress callback for UI updates
    panel_->set_sample_progress_callback([this]() { update_instruction_text(); });

    // Set up fast-revert callback for broken matrix detection during verify
    panel_->set_fast_revert_callback([this]() {
        spdlog::warn("[{}] Fast-revert: broken matrix detected, reverting", get_name());

        // Restore backup calibration
        if (has_backup_) {
            DisplayManager* dm = DisplayManager::instance();
            if (dm) {
                dm->apply_touch_calibration(backup_calibration_);
            }
            has_backup_ = false;
        }

        panel_->retry();
        update_state_subject();
        update_instruction_text();
        update_crosshair_position();
    });

    spdlog::debug("[{}] Instance created", get_name());
}

TouchCalibrationOverlay::~TouchCalibrationOverlay() {
    // Clean up managers before widget destruction
    if (panel_) {
        panel_->set_completion_callback(nullptr);
    }

    // Deinitialize subjects to disconnect observers
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Clear widget pointers (owned by LVGL)
    overlay_root_ = nullptr;
    crosshair_ = nullptr;
}

// ============================================================================
// Subject Initialization
// ============================================================================

void TouchCalibrationOverlay::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // State subject: 0=IDLE, 1=POINT_1, 2=POINT_2, 3=POINT_3, 4=VERIFY, 5=COMPLETE
    UI_MANAGED_SUBJECT_INT(state_subject_, STATE_IDLE, "touch_cal_state", subjects_);

    // Instruction text subject
    UI_MANAGED_SUBJECT_STRING(instruction_subject_, instruction_buffer_, "Tap anywhere to begin",
                              "touch_cal_instruction", subjects_);

    // Accept button text subject (for countdown display)
    UI_MANAGED_SUBJECT_STRING(accept_button_text_, accept_text_buffer_, "Accept",
                              "touch_cal_accept_text", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

void TouchCalibrationOverlay::register_callbacks() {
    spdlog::debug("[{}] Registering event callbacks", get_name());

    register_xml_callbacks({
        {"on_touch_cal_accept_clicked", on_touch_cal_accept_clicked},
        {"on_touch_cal_retry_clicked", on_touch_cal_retry_clicked},
        {"on_touch_cal_overlay_touched", on_touch_cal_overlay_touched},
        {"on_touch_cal_back_clicked", on_touch_cal_back_clicked},
    });

    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* TouchCalibrationOverlay::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating overlay from XML", get_name());

    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    // Reset cleanup flag when (re)creating
    cleanup_called_ = false;

    // Create overlay from XML
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "touch_calibration_overlay", nullptr));

    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find crosshair widget for positioning
    crosshair_ = lv_obj_find_by_name(overlay_root_, "crosshair");
    if (!crosshair_) {
        spdlog::warn("[{}] Crosshair widget not found in XML", get_name());
    }

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Show/Hide
// ============================================================================

void TouchCalibrationOverlay::show(CompletionCallback callback) {
    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show: overlay not created", get_name());
        return;
    }

    spdlog::debug("[{}] Showing overlay", get_name());

    // Store completion callback
    completion_callback_ = std::move(callback);
    callback_invoked_ = false;

    // Start in IDLE — first tap anywhere begins calibration
    if (panel_) {
        panel_->cancel(); // Reset to IDLE
    }
    lv_subject_set_int(&state_subject_, STATE_IDLE);
    update_instruction_text();
    update_crosshair_position();

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);

    spdlog::info("[{}] Overlay shown", get_name());
}

void TouchCalibrationOverlay::hide() {
    if (!overlay_root_) {
        return;
    }

    spdlog::debug("[{}] Hiding overlay", get_name());

    // Pop from navigation stack - on_deactivate() will be called by NavigationManager
    NavigationManager::instance().go_back();

    spdlog::info("[{}] Overlay hidden", get_name());
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void TouchCalibrationOverlay::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Initialize crosshair position if calibrating
    update_crosshair_position();
}

void TouchCalibrationOverlay::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Cancel any in-progress calibration
    if (panel_) {
        panel_->cancel();
    }

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// Cleanup
// ============================================================================

void TouchCalibrationOverlay::cleanup() {
    spdlog::debug("[{}] Cleaning up", get_name());

    // Unregister from NavigationManager before cleaning up
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Call base class to set cleanup_called_ flag
    OverlayBase::cleanup();

    // Cancel any in-progress calibration
    if (panel_) {
        panel_->set_completion_callback(nullptr);
        panel_->cancel();
    }

    // Clear widget pointers
    crosshair_ = nullptr;

    // Clear callback
    completion_callback_ = nullptr;
    callback_invoked_ = false;

    // Clear backup state
    has_backup_ = false;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Event Handlers
// ============================================================================

void TouchCalibrationOverlay::handle_accept_clicked() {
    spdlog::info("[{}] Accept calibration clicked", get_name());

    if (!panel_) {
        return;
    }

    // Get calibration data before accepting
    const TouchCalibration* cal = panel_->get_calibration();
    if (!cal || !cal->valid) {
        spdlog::error("[{}] No valid calibration to accept", get_name());
        return;
    }

    // Save calibration to config
    Config* config = Config::get_instance();
    if (config) {
        config->set<bool>("/input/calibration/valid", true);
        config->set<double>("/input/calibration/a", static_cast<double>(cal->a));
        config->set<double>("/input/calibration/b", static_cast<double>(cal->b));
        config->set<double>("/input/calibration/c", static_cast<double>(cal->c));
        config->set<double>("/input/calibration/d", static_cast<double>(cal->d));
        config->set<double>("/input/calibration/e", static_cast<double>(cal->e));
        config->set<double>("/input/calibration/f", static_cast<double>(cal->f));
        config->set<bool>("/input/calibration/swap_axes", panel_->axes_swapped());
        config->save();
        spdlog::info("[{}] Calibration saved to config", get_name());
    }

    // Apply calibration immediately via DisplayManager
    DisplayManager* dm = DisplayManager::instance();
    if (dm && dm->apply_touch_calibration(*cal)) {
        spdlog::info("[{}] Calibration applied to touch input", get_name());
    } else {
#ifndef HELIX_DISPLAY_FBDEV
        // Show warning on SDL that calibration cannot be applied at runtime
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("Calibration saved but cannot apply on SDL display"),
                                      3000);
#endif
        spdlog::debug("[{}] Could not apply calibration immediately (may require restart)",
                      get_name());
    }

    // Calibration accepted - no need to restore backup
    has_backup_ = false;

    // Reset accept button text for next calibration
    snprintf(accept_text_buffer_, sizeof(accept_text_buffer_), "Accept");
    lv_subject_copy_string(&accept_button_text_, accept_text_buffer_);

    // Accept in panel (transitions to COMPLETE state)
    panel_->accept();
    lv_subject_set_int(&state_subject_, STATE_COMPLETE);

    // Invoke completion callback with success
    if (completion_callback_ && !callback_invoked_) {
        callback_invoked_ = true;
        completion_callback_(true);
    }

    hide();
}

void TouchCalibrationOverlay::handle_retry_clicked() {
    spdlog::info("[{}] Retry calibration clicked", get_name());

    if (!panel_) {
        return;
    }

    // Restore previous calibration before retrying
    if (has_backup_) {
        DisplayManager* dm = DisplayManager::instance();
        if (dm) {
            dm->apply_touch_calibration(backup_calibration_);
            spdlog::info("[{}] Restored previous calibration for retry", get_name());
        }
        has_backup_ = false;
    }

    panel_->retry();
    lv_subject_set_int(&state_subject_, STATE_POINT_1);
    update_instruction_text();
    update_crosshair_position();
}

void TouchCalibrationOverlay::handle_screen_touched(lv_event_t* e) {
    (void)e; // Event not used directly - we get touch position from active input device

    if (!panel_ || !overlay_root_) {
        return;
    }

    // Get click position relative to the screen
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    auto state_before = panel_->get_state();

    // Handle VERIFY state - show calibration accuracy visualization with ripple
    if (state_before == helix::TouchCalibrationPanel::State::VERIFY) {
        spdlog::debug("[{}] Verify touch at ({}, {})", get_name(), point.x, point.y);

        lv_obj_t* content = lv_obj_find_by_name(overlay_root_, "calibration_content");
        if (content) {
            create_ripple(content, point.x, point.y);
        }

        DisplayManager* dm = DisplayManager::instance();
        bool on_screen =
            dm && point.x >= 0 && point.x < dm->width() && point.y >= 0 && point.y < dm->height();
        panel_->report_verify_touch(on_screen);
        return;
    }

    // add_sample() handles IDLE→POINT_1 auto-start and sample collection
    spdlog::info("[{}] Screen touched at ({}, {}) during state {}", get_name(), point.x, point.y,
                 static_cast<int>(state_before));
    panel_->add_sample({point.x, point.y});

    // Flash crosshair for visual tap feedback (only during calibration points,
    // not on the initial "tap anywhere to begin" transition from IDLE)
    auto state_after = panel_->get_state();
    if (crosshair_ && state_before != helix::TouchCalibrationPanel::State::IDLE &&
        (state_after == helix::TouchCalibrationPanel::State::POINT_1 ||
         state_after == helix::TouchCalibrationPanel::State::POINT_2 ||
         state_after == helix::TouchCalibrationPanel::State::POINT_3)) {
        flash_object(crosshair_);
    }

    // If we just entered VERIFY, temporarily apply new calibration so accept/retry buttons
    // are tappable even if the previous calibration was bad
    if (panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY) {
        const TouchCalibration* cal = panel_->get_calibration();
        DisplayManager* dm = DisplayManager::instance();
        if (cal && cal->valid && dm) {
            backup_calibration_ = dm->get_current_calibration();
            has_backup_ = true;
            if (dm->apply_touch_calibration(*cal)) {
                spdlog::info("[{}] New calibration applied for verification", get_name());
            }
        }
    }

    // Map panel state to subject state
    update_state_subject();
    update_instruction_text();
    update_crosshair_position();
}

void TouchCalibrationOverlay::handle_back_clicked() {
    spdlog::info("[{}] Back button clicked", get_name());

    // Invoke completion callback with cancelled
    if (completion_callback_ && !callback_invoked_) {
        callback_invoked_ = true;
        completion_callback_(false);
    }

    hide();
}

// ============================================================================
// UI Update Helpers
// ============================================================================

void TouchCalibrationOverlay::update_state_subject() {
    if (!panel_) {
        return;
    }

    auto state = panel_->get_state();
    int state_value = STATE_IDLE;

    switch (state) {
    case helix::TouchCalibrationPanel::State::IDLE:
        state_value = STATE_IDLE;
        break;
    case helix::TouchCalibrationPanel::State::POINT_1:
        state_value = STATE_POINT_1;
        break;
    case helix::TouchCalibrationPanel::State::POINT_2:
        state_value = STATE_POINT_2;
        break;
    case helix::TouchCalibrationPanel::State::POINT_3:
        state_value = STATE_POINT_3;
        break;
    case helix::TouchCalibrationPanel::State::VERIFY:
        state_value = STATE_VERIFY;
        break;
    case helix::TouchCalibrationPanel::State::COMPLETE:
        state_value = STATE_COMPLETE;
        break;
    }

    lv_subject_set_int(&state_subject_, state_value);
}

void TouchCalibrationOverlay::update_instruction_text() {
    if (!panel_) {
        return;
    }

    auto p = panel_->get_progress();

    switch (p.state) {
    case helix::TouchCalibrationPanel::State::IDLE:
        lv_subject_copy_string(&instruction_subject_, lv_tr("Tap anywhere to begin"));
        return;
    case helix::TouchCalibrationPanel::State::VERIFY:
        lv_subject_copy_string(&instruction_subject_, lv_tr("Touch anywhere to verify accuracy"));
        return;
    case helix::TouchCalibrationPanel::State::COMPLETE:
        lv_subject_copy_string(&instruction_subject_, lv_tr("Calibration complete"));
        return;
    default:
        break;
    }

    // POINT states — show which touch is next (1-indexed)
    // current_sample=0 → "touch 1 of 7" (waiting for first), current_sample=1 → "touch 2 of 7",
    // etc. After the last sample (7), state advances so we never show "touch 8 of 7" TRANSLATORS:
    // %1$d = point number (1-3), %2$d = next touch number (1-7), %3$d = total
    snprintf(instruction_buffer_, sizeof(instruction_buffer_),
             lv_tr("Tap the crosshair (point %1$d of 3) \xe2\x80\x94 touch %2$d of %3$d"),
             p.point_num, p.current_sample + 1, p.total_samples);
    lv_subject_copy_string(&instruction_subject_, instruction_buffer_);
}

void TouchCalibrationOverlay::update_crosshair_position() {
    if (!crosshair_ || !panel_) {
        return;
    }

    auto state = panel_->get_state();

    // Hide crosshair in IDLE, VERIFY, and COMPLETE states
    if (state == helix::TouchCalibrationPanel::State::IDLE ||
        state == helix::TouchCalibrationPanel::State::VERIFY ||
        state == helix::TouchCalibrationPanel::State::COMPLETE) {
        lv_obj_add_flag(crosshair_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Show crosshair for calibration points
    lv_obj_remove_flag(crosshair_, LV_OBJ_FLAG_HIDDEN);

    // Determine which step we're on
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

    // Get target position from panel
    helix::Point target = panel_->get_target_position(step);

    // Center crosshair on target
    lv_obj_set_pos(crosshair_, target.x - CROSSHAIR_HALF_SIZE, target.y - CROSSHAIR_HALF_SIZE);

    spdlog::debug("[{}] Crosshair positioned at ({}, {}) for step {}", get_name(), target.x,
                  target.y, step);
}

void TouchCalibrationOverlay::on_calibration_complete(const TouchCalibration* cal) {
    // Guard against callback during cleanup
    if (cleanup_called_ || !overlay_root_) {
        spdlog::debug("[{}] Ignoring callback during cleanup", get_name());
        return;
    }

    if (cal && cal->valid) {
        spdlog::info("[{}] Calibration accepted", get_name());
    } else {
        spdlog::debug("[{}] Calibration cancelled or invalid", get_name());
    }
}

} // namespace helix::ui
