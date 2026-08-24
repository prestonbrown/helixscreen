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
#include "touch_calibration_layout.h"

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

static void on_touch_cal_overlay_released(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[TouchCalibrationOverlay] screen released");
    get_touch_calibration_overlay().handle_screen_released();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_touch_cal_back_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[TouchCalibrationOverlay] back clicked");
    get_touch_calibration_overlay().handle_back_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_touch_cal_cancel_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[TouchCalibrationOverlay] cancel chip clicked");
    get_touch_calibration_overlay().handle_cancel_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_touch_cal_hold_abort(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[TouchCalibrationOverlay] hold abort");
    get_touch_calibration_overlay().handle_hold_abort();
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

    // Screen size is (re)sampled from DisplayManager in show() — the overlay
    // is a singleton constructed early in startup before the display has
    // finished rotating/sizing, so constructor-time dimensions can be stale.

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

        // Revert to the pre-session calibration and disable affine for the next
        // capture attempt.
        if (helix::ICalibrationSink* sink = calibration_sink()) {
            session_.revert_for_retry(*sink);
        }

        // Restarting forever is the trap: a user who cannot press Accept under
        // this panel's mapping will not be able to press it on the next pass
        // either, and the countdown put them straight back into capture with no
        // way out (#943).
        if (++unattended_verify_rounds_ >= MAX_UNATTENDED_VERIFY_ROUNDS) {
            abort_session(lv_tr("Calibration kept timing out. Your previous settings were kept."));
            return;
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

    // Install the JUST-CAPTURED calibration the instant the panel enters VERIFY.
    //
    // Fires on the actual state transition (from the release-commit, the stall
    // timer, or legacy sample-on-press), so it cannot be defeated by which input
    // edge happened to trigger the commit.
    //
    // VERIFY exists to test the new matrix, so the new matrix is what has to be
    // active: Accept and Retry sit at bottom-center, and only the transform the
    // user just captured can put a finger there. Re-installing the PRE-SESSION
    // matrix instead inverts the assumption the screen is built on — the stored
    // matrix being wrong is the sole reason anyone opens recalibration. On a
    // digitizer that over-reports its ABS range every tap collapses into a
    // top-left sub-rectangle under the stored matrix, so the buttons could never
    // be pressed and the session could only ever time out (#943, Qidi Q2).
    //
    // This also covers the case the previous behaviour was written for (#943/#986,
    // AD5M): a resistive panel whose raw Y runs opposite to screen Y. The new
    // matrix is precisely the one that un-inverts Y, so Accept is reachable under
    // it as well.
    //
    // The session backup stays armed, so Retry, a timeout, a fast-revert, and a
    // plain dismiss all still revert to the pre-session calibration; only Accept
    // calls session_.commit() and keeps this matrix.
    panel_->set_verify_entry_callback([this]() {
        helix::ICalibrationSink* sink = calibration_sink();
        const TouchCalibration* fresh = panel_ ? panel_->get_calibration() : nullptr;
        if (sink && fresh && fresh->valid && sink->apply_calibration(*fresh)) {
            spdlog::info("[{}] Entered VERIFY under the newly captured calibration "
                         "(a={:.4f} e={:.4f}); reverts unless accepted",
                         get_name(), fresh->a, fresh->e);
        } else if (sink) {
            // No usable new matrix to test — fall back to whatever was stored so
            // the screen is at least as usable as it was on entry.
            sink->enable_affine();
            spdlog::warn("[{}] Entered VERIFY without a usable new calibration; "
                         "kept the pre-session one",
                         get_name());
        }
        update_state_subject();
        update_instruction_text();
        update_crosshair_position();
    });

    // Set up fast-revert callback for broken matrix detection during verify
    panel_->set_fast_revert_callback([this]() {
        spdlog::warn("[{}] Fast-revert: broken matrix detected, reverting", get_name());

        // Revert to the pre-session calibration and disable affine for retry.
        if (helix::ICalibrationSink* sink = calibration_sink()) {
            session_.revert_for_retry(*sink);
        }

        // Shares the timeout's budget: a fast-revert also sends the user back
        // into capture without them asking, so on its own it is the same loop.
        if (++unattended_verify_rounds_ >= MAX_UNATTENDED_VERIFY_ROUNDS) {
            abort_session(lv_tr("Touch input did not track the new calibration. "
                                "Your previous settings were kept."));
            return;
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
// Calibration Sink
// ============================================================================

helix::ICalibrationSink* TouchCalibrationOverlay::calibration_sink() {
    if (calibration_sink_override_) {
        return calibration_sink_override_;
    }
    // DisplayManager implements ICalibrationSink; it is null before the display
    // is brought up and in unit tests.
    return DisplayManager::instance();
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
        {"on_touch_cal_overlay_released", on_touch_cal_overlay_released},
        {"on_touch_cal_back_clicked", on_touch_cal_back_clicked},
        {"on_touch_cal_cancel_clicked", on_touch_cal_cancel_clicked},
        {"on_touch_cal_hold_abort", on_touch_cal_hold_abort},
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

    // Find crosshair and touch capture widgets. Reparenting to screen root
    // is deferred to show() so z-order lands above the pushed overlay panel.
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

    // Re-sample screen dimensions every time the overlay opens. The display
    // may have rotated/resized since construction (singleton is built early
    // in startup). Stale dimensions cause crosshairs to land at wrong screen
    // ratios and produce a systematically biased Y affine — the bottom 22%
    // of the screen is pure extrapolation from the (50%, 78%) target.
    if (panel_) {
        DisplayManager* display_mgr = DisplayManager::instance();
        if (display_mgr && display_mgr->is_initialized()) {
            panel_->set_screen_size(display_mgr->width(), display_mgr->height());
            spdlog::debug("[{}] Screen size set to {}x{}", get_name(), display_mgr->width(),
                          display_mgr->height());
        } else {
            panel_->set_screen_size(800, 480);
            spdlog::warn("[{}] DisplayManager not available, using default 800x480", get_name());
        }
    }

    // Reset ALL per-session state to a fresh-constructed baseline so the
    // singleton overlay behaves like the first-run wizard, which builds a brand
    // new panel each time (#943). cancel() alone only set IDLE + invalidated
    // the calibration — it left the press-debounce gate armed, a half-filled
    // sample buffer, and stale verify counters from the previous show, so the
    // SECOND Settings -> Touch Calibration session misbehaved. reset() also
    // re-reads the debounce setting and is silent (no completion callback).
    if (panel_) {
        panel_->reset();
    }
    unattended_verify_rounds_ = 0;
    hold_repeat_count_ = 0;

    // Snapshot the active calibration and disable affine so we capture raw
    // coordinates. session_.restore() (in on_deactivate/cleanup) re-enables it
    // however this session ends.
    if (helix::ICalibrationSink* sink = calibration_sink()) {
        session_.begin_capture(*sink);
    }
    if (DisplayManager* dm = DisplayManager::instance()) {
        // Suppress the global debug-touches ripple while this overlay is up —
        // it draws its own ripple, and the global one would render raw coords
        // during capture (#943).
        dm->set_touch_calibration_active(true);
    }

    lv_subject_set_int(&state_subject_, STATE_IDLE);
    update_instruction_text();
    update_crosshair_position();

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    // (which is where we reparent crosshair + capture layer to screen root, see
    // on_activate() below). Reparenting MUST happen after push_overlay's queued
    // lambda runs and calls lv_obj_move_foreground(overlay_root_) — otherwise
    // the reparented widgets land below the overlay in z-order.
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

    // Lift the crosshair AND the touch capture surface onto the screen root,
    // AFTER push_overlay moves overlay_root_ to foreground. The capture surface
    // must cover the FULL screen — header included — so that an uncalibrated tap
    // on a top-edge target (affine is disabled during capture) can't report a
    // coordinate that lands in the header strip and route to the header Back
    // button (which is clickable + full-width). LVGL hit-tests by widget geometry,
    // so a capture surface confined to the content area below the header leaves
    // that top strip owned by the Back button. Shared with the first-run wizard —
    // see touch_calibration_layout.h.
    helix::ui::CaptureSurfaceWidgets cap =
        helix::ui::reparent_capture_surface_fullscreen(overlay_root_);
    crosshair_ = cap.crosshair;
    if (cap.crosshair && !crosshair_orig_parent_) {
        crosshair_orig_parent_ = cap.crosshair_original_parent;
    }
    if (cap.capture_overlay) {
        capture_overlay_ = cap.capture_overlay;
        if (!capture_orig_parent_) {
            capture_orig_parent_ = cap.capture_original_parent;
        }
    }

    // The header Back button is now covered by the full-screen capture surface,
    // so provide an abort affordance that does NOT sit under any calibration
    // target: the Cancel chip lives in the bottom-left corner (targets occupy
    // top-left, top-right and bottom-center). Lift it above the capture surface
    // so it stays clickable, then pin it to the screen's bottom-left corner
    // (independent of content layout — the capture surface is screen-absolute).
    raised_cancel_ = helix::ui::raise_control_above_capture(overlay_root_, "cancel_chip");
    if (raised_cancel_.obj) {
        constexpr lv_coord_t CANCEL_CHIP_INSET = 12;
        lv_obj_t* scr = lv_screen_active();
        lv_obj_update_layout(scr);
        lv_coord_t chip_h = lv_obj_get_height(raised_cancel_.obj);
        // Absolute screen-corner placement (align is TOP_LEFT so set_pos is
        // absolute, not an offset from an alignment anchor).
        lv_obj_set_align(raised_cancel_.obj, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(raised_cancel_.obj, CANCEL_CHIP_INSET,
                       lv_obj_get_height(scr) - chip_h - CANCEL_CHIP_INSET);
    }

    // Initialize crosshair position if calibrating
    update_crosshair_position();
}

void TouchCalibrationOverlay::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Reparent the capture surface + Cancel chip back into the overlay subtree
    // BEFORE the slide-out, so a live full-screen touch target doesn't linger on
    // the screen behind the dismissed overlay. (crosshair too.)
    restore_reparented_widgets();

    // Cancel any in-progress calibration
    if (panel_) {
        panel_->cancel();
    }

    // Restore the pre-session calibration and re-enable the affine transform.
    // The navigation stack calls on_deactivate() — NOT cleanup() — on a plain
    // dismiss, so this is the path that must re-arm touch. Without it, aborting
    // recalibration before accepting left the affine disabled until the next
    // reboot, so touch reverted to raw/unscaled coordinates (#943).
    if (helix::ICalibrationSink* sink = calibration_sink()) {
        session_.restore(*sink);
    }
    if (DisplayManager* dm = DisplayManager::instance()) {
        // Re-allow the global debug-touches ripple now that the overlay is gone.
        dm->set_touch_calibration_active(false);
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

    // Restore the crosshair, capture surface, and Cancel chip to their original
    // XML parents so the overlay can be re-shown cleanly and nothing lingers on
    // screen when hidden. Clears the FLOATING flags added in on_activate() so
    // layout matches the XML default on next show.
    restore_reparented_widgets();
    crosshair_orig_parent_ = nullptr;
    capture_orig_parent_ = nullptr;

    // Clear widget pointers
    crosshair_ = nullptr;

    // Clear callback
    completion_callback_ = nullptr;
    callback_invoked_ = false;

    // Restore the pre-session calibration and re-enable the affine transform.
    // (on_deactivate() already does this on a normal dismiss; this covers
    // teardown/destruction paths that bypass it.)
    if (helix::ICalibrationSink* sink = calibration_sink()) {
        session_.restore(*sink);
    }

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
        config->save();
        spdlog::info("[{}] Calibration saved to config", get_name());
    }

    // Apply calibration immediately via the live input device
    helix::ICalibrationSink* sink = calibration_sink();
    if (sink && sink->apply_calibration(*cal)) {
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

    // Calibration accepted — keep it; teardown must not revert it.
    session_.commit();
    unattended_verify_rounds_ = 0;

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

    // Revert to the pre-session calibration and disable affine for raw capture.
    if (helix::ICalibrationSink* sink = calibration_sink()) {
        session_.revert_for_retry(*sink);
    }

    // A pressed Retry is proof the user can reach the buttons, so the
    // unattended-round budget starts over rather than counting toward an abort
    // they did not ask for.
    unattended_verify_rounds_ = 0;

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

    // A fresh press ends any hold in progress.
    hold_repeat_count_ = 0;

    auto state_before = panel_->get_state();

    // Handle VERIFY state - show calibration accuracy visualization with ripple.
    //
    // The NEW calibration is active in the touch wrapper (installed by the
    // panel's verify_entry_callback), so `point` already IS where the matrix
    // under test places the finger — no round trip through the old matrix.
    if (state_before == helix::TouchCalibrationPanel::State::VERIFY) {
        spdlog::debug("[{}] Verify touch at ({}, {})", get_name(), point.x, point.y);

        // Verify phase: a transient ripple is enough — the user is just checking
        // that touches track. No lingering dot here (that's only useful in the
        // alignment phase, to see where each target press landed) (#1082).
        // Drawn on the top layer so its coordinates are SCREEN-ABSOLUTE.
        create_ripple(lv_layer_top(), point.x, point.y);

        // Broken-matrix signal. calibrated_read_cb() clamps every transformed
        // coordinate to the panel, so a matrix that throws touches off-screen
        // does not surface as an out-of-range value — it surfaces as one pinned
        // to an extreme edge. Treat an edge-pinned touch as "did not land where
        // the finger was", which is what the 3s fast-revert net counts.
        lv_display_t* disp = lv_display_get_default();
        const int32_t w = disp ? lv_display_get_horizontal_resolution(disp) : 0;
        const int32_t h = disp ? lv_display_get_vertical_resolution(disp) : 0;
        const bool on_screen =
            w > 1 && h > 1 && point.x > 0 && point.y > 0 && point.x < w - 1 && point.y < h - 1;
        panel_->report_verify_touch(on_screen);
        return;
    }

    // on_press() captures the press; commit happens on release / stall (#943).
    // Handles IDLE→POINT_1 auto-start and sample collection.
    spdlog::debug("[{}] Screen touched at ({}, {}) during state {}", get_name(), point.x, point.y,
                  static_cast<int>(state_before));
    panel_->on_press({point.x, point.y});

    // Flash crosshair for visual tap feedback (only during calibration points,
    // not on the initial "tap anywhere to begin" transition from IDLE)
    auto state_after = panel_->get_state();
    if (crosshair_ && state_before != helix::TouchCalibrationPanel::State::IDLE &&
        (state_after == helix::TouchCalibrationPanel::State::POINT_1 ||
         state_after == helix::TouchCalibrationPanel::State::POINT_2 ||
         state_after == helix::TouchCalibrationPanel::State::POINT_3)) {
        flash_object(crosshair_, 200, true);

        // Drop a touch marker (ripple + lingering dot) at the finger's landing
        // point so the user can compare where they actually touched against
        // the target crosshair (#1082). Capture runs with the affine transform
        // disabled, so `point` is in raw capture space — which is NOT screen
        // space on panels whose digitizer over-reports its ABS range (Qidi Q2,
        // #943): raw space is compressed ~0.5x and a marker drawn there lands
        // centimetres from the crosshair, reading as "broken" even while
        // calibration is working. Map it through the session's backup — the
        // calibration the user's touches were tracking under when the session
        // opened — so the dot shows where the finger lands under the CURRENT
        // mapping. First-ever calibration (no backup) draws at the raw point:
        // no mapping exists to show yet, and the pre-calibration state really
        // is that broken.
        Point marker{point.x, point.y};
        const TouchCalibration& pre_session = session_.backup();
        if (pre_session.valid) {
            lv_display_t* disp = lv_display_get_default();
            const int max_x = disp ? lv_display_get_horizontal_resolution(disp) - 1 : 0;
            const int max_y = disp ? lv_display_get_vertical_resolution(disp) - 1 : 0;
            marker = transform_point(pre_session, marker, max_x, max_y);
        }
        create_touch_marker(lv_layer_top(), marker.x, marker.y);
    }

    // VERIFY entry (re-enabling the original calibration) is handled by the
    // panel's verify_entry_callback so it fires on the real state transition
    // rather than a specific input edge — see the callback in the constructor.

    // Map panel state to subject state
    update_state_subject();
    update_instruction_text();
    update_crosshair_position();
}

void TouchCalibrationOverlay::handle_screen_released() {
    // Forward finger-lift to the panel so the pending press commits
    // (issue #943). No-op when debounce is disabled. Main-thread input only.
    if (!panel_) {
        return;
    }
    // Lifting the finger ends any hold in progress.
    hold_repeat_count_ = 0;
    panel_->on_release();

    // The commit happens here (not on press), so refresh the UI now that the
    // sample count / state may have advanced — otherwise the instruction label
    // keeps showing the pre-commit "touch N of 3" until the next press.
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

void TouchCalibrationOverlay::handle_hold_abort() {
    if (++hold_repeat_count_ < HOLD_ABORT_REPEATS) {
        return;
    }
    hold_repeat_count_ = 0;
    spdlog::info("[{}] Press-and-hold abort", get_name());
    abort_session(lv_tr("Calibration cancelled."));
}

// Reached from three places that all run inside an LVGL dispatch: the Cancel
// chip's `clicked` callback, the capture surface's LONG_PRESSED_REPEAT callback,
// and the panel's countdown / fast-revert lv_timer callbacks. Nothing here
// destroys a widget synchronously, which is what threading rule 3 forbids:
//
//   - The exit is handle_back_clicked() -> hide() -> NavigationManager::go_back(),
//     whose entire body is a queue_update() lambda. UpdateQueue::queue() only
//     pushes onto pending_ — it has no same-thread fast path — so go_back()
//     returns having merely enqueued. on_deactivate(), restore_reparented_widgets()
//     (which moves the very object a long press is dispatching on) and the
//     slide-out all run on a later process_pending tick, after LVGL's event and
//     timer dispatch have unwound.
//   - handle_back_clicked() is the LAST statement here, and abort_session() is
//     the last statement in each of its callers, so no path touches `this`, the
//     overlay root, or the pressed object after the enqueue.
//   - The overlay is a StaticPanelRegistry singleton and go_back() never deletes
//     overlay_root_ (it animates it out and pops the stack), so there is no
//     object for a late callback to land on either way.
//
// Ordering guarantee for the timer paths: panel_->reset() below runs BEFORE
// handle_back_clicked(), and reset() stops the countdown, fast-revert and stall
// timers. All three are therefore cancelled before anything is enqueued.
// Cancelling the countdown timer from inside its own callback is safe —
// lv_timer_delete() sets state.timer_deleted, which makes lv_timer_handler
// restart its walk, and lv_timer_exec() touches the timer only before the
// callback, never after.
void TouchCalibrationOverlay::abort_session(const char* reason) {
    spdlog::warn("[{}] Aborting session: {}", get_name(), reason);

    // Put the device back on the calibration it had on entry and re-arm the
    // affine, so an abort never leaves touch running on an unaccepted matrix or
    // on raw coordinates.
    if (helix::ICalibrationSink* sink = calibration_sink()) {
        session_.restore(*sink);
    }

    // Silent fresh baseline: reset() does not fire the completion callback, so
    // handle_back_clicked() below stays the single "cancelled" report.
    if (panel_) {
        panel_->reset();
    }
    unattended_verify_rounds_ = 0;
    hold_repeat_count_ = 0;

    snprintf(accept_text_buffer_, sizeof(accept_text_buffer_), "Accept");
    lv_subject_copy_string(&accept_button_text_, accept_text_buffer_);

    update_state_subject();
    update_instruction_text();
    update_crosshair_position();

    ToastManager::instance().show(ToastSeverity::WARNING, reason, 4000);

    handle_back_clicked();
}

void TouchCalibrationOverlay::handle_cancel_clicked() {
    // Same abort path as Back: the Cancel chip only exists because the header
    // Back button is covered by the full-screen capture surface during capture.
    spdlog::info("[{}] Cancel chip clicked", get_name());
    handle_back_clicked();
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

void TouchCalibrationOverlay::restore_reparented_widgets() {
    // Cancel chip back into the overlay subtree.
    helix::ui::restore_raised_control(raised_cancel_);
    raised_cancel_ = {};

    // Capture surface back into its content parent. It is 100%x100% by XML, so the
    // parent re-lays it out once FLOATING is cleared.
    if (capture_overlay_ && capture_orig_parent_) {
        lv_obj_set_parent(capture_overlay_, capture_orig_parent_);
        lv_obj_remove_flag(capture_overlay_, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(capture_overlay_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(capture_overlay_, 0, 0);
    }
    capture_overlay_ = nullptr;

    // Crosshair back into its content parent (hidden until the next capture).
    if (crosshair_ && crosshair_orig_parent_) {
        lv_obj_set_parent(crosshair_, crosshair_orig_parent_);
        lv_obj_remove_flag(crosshair_, LV_OBJ_FLAG_FLOATING);
        lv_obj_add_flag(crosshair_, LV_OBJ_FLAG_HIDDEN);
    }
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
