// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_touch_calibration_overlay.h
 * @brief Touch calibration overlay for 3-point calibration workflow
 *
 * Provides a fullscreen overlay for touch calibration with:
 * - Visual crosshair targets for touch point capture
 * - State-driven UI progression (points -> verify -> complete)
 * - Completion callback with success status
 * - Sample progress feedback (touch N of 7)
 *
 * ## States:
 *   POINT_1 -> POINT_2 -> POINT_3 -> VERIFY -> COMPLETE
 *
 * ## Completion Callback:
 * - true  = Accepted and saved
 * - false = Cancelled (back button)
 *
 * ## Initialization Order:
 *   1. Register XML components (touch_calibration_overlay.xml)
 *   2. init_subjects()
 *   3. register_callbacks()
 *   4. create(parent_screen)
 *   5. show() when ready to display
 */

#pragma once

#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "touch_calibration.h"
#include "touch_calibration_layout.h"
#include "touch_calibration_panel.h"
#include "touch_calibration_session.h"

#include <functional>
#include <memory>

namespace helix::ui {

/**
 * @class TouchCalibrationOverlay
 * @brief Fullscreen overlay for 3-point touch calibration
 *
 * Manages the touch calibration UI workflow, displaying crosshair targets
 * and capturing touch points for calibration matrix computation. Integrates
 * with TouchCalibrationPanel for state machine logic.
 *
 * Inherits from OverlayBase for lifecycle management (on_activate/on_deactivate).
 */
class TouchCalibrationOverlay : public OverlayBase {
  public:
    /**
     * @brief Completion callback type
     *
     * @param success true if calibration was accepted and saved
     *
     * Callback interpretations:
     * - true  = Calibration accepted and saved
     * - false = Calibration cancelled (back button)
     */
    using CompletionCallback = std::function<void(bool success)>;

    TouchCalibrationOverlay();
    ~TouchCalibrationOverlay() override;

    // Non-copyable
    TouchCalibrationOverlay(const TouchCalibrationOverlay&) = delete;
    TouchCalibrationOverlay& operator=(const TouchCalibrationOverlay&) = delete;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize reactive subjects for XML binding
     *
     * Creates and registers subjects:
     * - touch_cal_state (int): Current state 0-5
     * - touch_cal_instruction (string): Instruction text
     *
     * MUST be called BEFORE create() to ensure bindings work.
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks:
     * - on_touch_cal_accept_clicked
     * - on_touch_cal_retry_clicked
     * - on_touch_cal_overlay_touched
     * - on_touch_cal_back_clicked
     */
    void register_callbacks() override;

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Touch Calibration"
     */
    const char* get_name() const override {
        return "Touch Calibration";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Initializes crosshair position and prepares for calibration.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Cancels any in-progress calibration.
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    //
    // === Public API ===
    //

    /**
     * @brief Show overlay and begin calibration workflow
     *
     * @param callback Optional callback invoked on completion/cancel/skip
     *
     * Pushes overlay onto navigation stack and shows initial UI state.
     */
    void show(CompletionCallback callback = nullptr);

    /**
     * @brief Hide overlay and return to previous screen
     *
     * Pops overlay from navigation stack via NavigationManager::go_back().
     */
    void hide();

    //
    // === Event Handlers (called by static trampolines) ===
    //

    /** @brief Handle accept button click - saves calibration */
    void handle_accept_clicked();

    /** @brief Handle retry button click - restarts calibration */
    void handle_retry_clicked();

    /**
     * @brief Handle screen touch event - captures calibration point
     * @param e LVGL event with touch coordinates
     */
    void handle_screen_touched(lv_event_t* e);

    /**
     * @brief Handle screen release event - clears the press-debounce gate
     *
     * Forwards LV_EVENT_RELEASED to the panel so one physical contact records
     * at most one sample when HELIX_TOUCH_CAL_DEBOUNCE=1 (issue #943).
     */
    void handle_screen_released();

    /** @brief Handle back button click - cancels calibration */
    void handle_back_clicked();

    /**
     * @brief Handle the in-capture Cancel chip - same abort path as Back
     *
     * During active point capture the full-screen capture surface covers the
     * header Back button, so a corner Cancel chip provides the abort affordance.
     */
    void handle_cancel_clicked();

    /**
     * @brief Handle one LV_EVENT_LONG_PRESSED_REPEAT tick of a press-and-hold
     *
     * Aborts the session once HOLD_ABORT_REPEATS consecutive repeats have
     * arrived. Counting repeats rather than acting on the first one keeps a
     * deliberate hold distinct from the taps calibration is made of; the counter
     * resets on every fresh press and release.
     *
     * This is the escape that does not depend on where a touch lands. Under a
     * mapping that compresses every tap toward one corner, no fixed rectangle
     * works: the reachable region and the rays that mis-mapped crosshair taps
     * travel both start at that same corner.
     */
    void handle_hold_abort();

    //
    // === Accessors ===
    //

    /**
     * @brief Check if overlay widget exists
     * @return true if overlay has been created
     */
    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    /**
     * @brief Get the underlying calibration panel
     * @return Pointer to TouchCalibrationPanel, or nullptr if not created
     */
    helix::TouchCalibrationPanel* get_panel() {
        return panel_.get();
    }

    /**
     * @brief Route this overlay's calibration handoffs to an injected sink.
     *
     * DisplayManager is the production sink (it implements ICalibrationSink), but
     * it has no backend in a unit test, which makes every apply/revert/restore a
     * silent no-op. Injecting a fake sink lets a test assert WHICH matrix each
     * phase installs. Pass nullptr to go back to DisplayManager.
     */
    void set_calibration_sink(helix::ICalibrationSink* sink) {
        calibration_sink_override_ = sink;
    }

  private:
    /**
     * @brief The sink that receives this session's calibration operations.
     * @return The injected sink if one is set, else DisplayManager (may be null).
     */
    helix::ICalibrationSink* calibration_sink();

    /** @brief Update state subject from panel state */
    void update_state_subject();

    /** @brief Update instruction text based on current state */
    void update_instruction_text();

    /** @brief Position crosshair at current calibration target */
    void update_crosshair_position();

    /**
     * @brief Reparent crosshair + capture surface + Cancel chip back into the
     *        overlay subtree so nothing is orphaned on the active screen.
     *
     * The full-screen capture surface and Cancel chip are lifted onto
     * lv_screen_active() during capture; they MUST be returned to the overlay
     * before it slides away, otherwise a live full-screen touch target would
     * linger on the screen behind it. Idempotent.
     */
    void restore_reparented_widgets();

    /**
     * @brief Handle calibration completion from panel
     * @param cal Calibration data if successful, nullptr if cancelled
     */
    void on_calibration_complete(const TouchCalibration* cal);

    /**
     * @brief Give up on this session: restore the pre-session calibration, tell
     *        the user why, and leave the overlay.
     *
     * Used by the bounded-retry guard and the press-and-hold escape. Takes the
     * same exit as handle_back_clicked() so the completion callback still reports
     * "cancelled" exactly once.
     *
     * @param reason Toast text explaining what happened.
     */
    void abort_session(const char* reason);

    //
    // === State Machine ===
    //

    std::unique_ptr<helix::TouchCalibrationPanel> panel_;

    //
    // === Subjects (managed by SubjectManager) ===
    //

    SubjectManager subjects_;
    lv_subject_t state_subject_;       ///< int: 0-5 for states
    lv_subject_t instruction_subject_; ///< string: instruction text
    char instruction_buffer_[128];

    // Accept button countdown text
    lv_subject_t accept_button_text_;
    char accept_text_buffer_[32] = "Accept";

    //
    // === Callbacks ===
    //

    CompletionCallback completion_callback_;
    bool callback_invoked_ = false; ///< Guard against double-invoke

    // Backup/disable/restore of the pre-session calibration. Shared with the
    // first-run wizard; guarantees the affine transform is re-enabled however
    // the session ends (#943).
    helix::TouchCalibrationSession session_;

    // Test seam for the above: nullptr means "use DisplayManager".
    helix::ICalibrationSink* calibration_sink_override_ = nullptr;

    //
    // === Widget References ===
    //

    lv_obj_t* crosshair_ = nullptr;

    // Original parent for crosshair. The widget is reparented to screen root
    // on activation so its coordinates are screen-absolute (required for
    // calibration accuracy — the overlay's title bar offsets the default XML
    // nesting). Restored on deactivate/cleanup.
    lv_obj_t* crosshair_orig_parent_ = nullptr;

    // The touch capture surface is lifted onto lv_screen_active() at 100%x100%
    // during capture so it covers the header too (uncalibrated top-edge taps
    // can't leak to the header Back button). Original parent is kept so it can
    // be reparented back into the overlay before dismiss.
    lv_obj_t* capture_overlay_ = nullptr;
    lv_obj_t* capture_orig_parent_ = nullptr;

    // In-capture Cancel chip lifted above the full-screen capture surface (the
    // Back button is unreachable while capturing). Restore state owned here.
    helix::ui::RaisedControl raised_cancel_;

    //
    // === State Constants ===
    //

    static constexpr int STATE_IDLE = 0;
    static constexpr int STATE_POINT_1 = 1;
    static constexpr int STATE_POINT_2 = 2;
    static constexpr int STATE_POINT_3 = 3;
    static constexpr int STATE_VERIFY = 4;
    static constexpr int STATE_COMPLETE = 5;

    static constexpr int CROSSHAIR_SIZE = 48;
    static constexpr int CROSSHAIR_HALF_SIZE = CROSSHAIR_SIZE / 2;

    //
    // === Unattended-loop Guards ===
    //

    /// VERIFY rounds this session has left on its own — a countdown expiry or a
    /// fast-revert, neither of which the user asked for. Cleared by an Accept, a
    /// user-pressed Retry, and each show().
    int unattended_verify_rounds_ = 0;

    /// Restarting capture after this many unattended VERIFY rounds is a loop, not
    /// a retry: a user who cannot reach Accept will never reach it on the next
    /// pass either. Abort instead so the screen is escapable.
    static constexpr int MAX_UNATTENDED_VERIFY_ROUNDS = 2;

    /// Consecutive LV_EVENT_LONG_PRESSED_REPEAT ticks seen on the current press.
    int hold_repeat_count_ = 0;

    /// Repeats required to abort. LVGL emits LONG_PRESSED after the indev's
    /// long-press time (400ms by default) and LONG_PRESSED_REPEAT every 100ms
    /// after that, so this is roughly a 1.4s hold — far longer than the taps
    /// calibration collects, and short enough to find by leaning on the screen.
    static constexpr int HOLD_ABORT_REPEATS = 10;

    friend class TouchCalibrationOverlayTestAccess;
};

// ============================================================================
// Global Instance Access
// ============================================================================

/**
 * @brief Get the global TouchCalibrationOverlay instance
 *
 * Creates the instance on first call. Singleton pattern.
 *
 * @return Reference to the global TouchCalibrationOverlay
 */
TouchCalibrationOverlay& get_touch_calibration_overlay();

/**
 * @brief Register touch calibration overlay event callbacks
 *
 * Registers static callback trampolines with lv_xml_register_event_cb().
 * Call during application initialization before creating overlay.
 */
void register_touch_calibration_overlay_callbacks();

} // namespace helix::ui
