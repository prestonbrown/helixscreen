// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "i_moonraker_api.h"
#include "lvgl.h"
#include "printer_state.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <chrono>

/**
 * @brief Reason the recovery dialog is being shown
 *
 * Tracks which error condition(s) triggered the dialog so the message
 * and available actions can adapt. Multiple reasons can be active
 * simultaneously (e.g., SHUTDOWN then DISCONNECTED in sequence).
 */
enum class RecoveryReason {
    NONE,         ///< No active recovery
    SHUTDOWN,     ///< Klipper entered SHUTDOWN state (e-stop, thermal runaway, config error)
    ERROR,        ///< Klipper entered ERROR state (MCU connection failure, config error)
    DISCONNECTED, ///< Klipper firmware disconnected from Moonraker
};

/// Recovery dialog suppression durations (milliseconds)
namespace RecoverySuppression {
static constexpr uint32_t SHORT = 5000;   ///< Brief operations (settings switch)
static constexpr uint32_t NORMAL = 10000; ///< Standard restarts (firmware restart, power toggle)
/// Extended operations (calibration, service install).
///
/// Deliberately short. Widening this to cover printers that chain a second
/// config write + restart (Creality K2 + CFS writes the CFS Tn_data via
/// CXSAVE_CONFIG tens of seconds after SAVE_CONFIG) was tried and reverted: the
/// suppression check is edge-triggered on the klippy state transition, so a
/// window long enough to cover the chained restart also swallows a genuine
/// unrecoverable shutdown landing in the same span — observed at 57s after
/// SAVE_CONFIG, which a 60s window would have hidden from the user entirely.
/// Prefer a spurious dialog over a silently-eaten shutdown.
static constexpr uint32_t LONG = 15000;
static constexpr uint32_t EXTRA = 30000; ///< Multi-step operations (PID→MPC migration)
} // namespace RecoverySuppression

/**
 * @brief Emergency stop visibility coordinator
 *
 * Manages the estop_visible subject that drives contextual E-Stop buttons
 * embedded in home_panel, controls_panel, and print_status_panel.
 * Buttons are automatically shown during active prints (PRINTING or PAUSED)
 * via XML subject binding. The button triggers an M112 emergency stop
 * command via Moonraker.
 *
 * Features:
 * - Single-tap activation (default) or confirmation dialog (optional setting)
 * - Automatic visibility based on print state (via estop_visible subject)
 * - Klipper recovery dialog auto-popup on SHUTDOWN state
 * - Visual feedback via toast notifications
 *
 * Usage:
 *   // In main.cpp after LVGL and subjects initialized:
 *   EmergencyStopOverlay::instance().init(printer_state, api);
 *   EmergencyStopOverlay::instance().create();
 *
 * @see klipper_recovery_dialog.xml for post-shutdown recovery flow
 */
class EmergencyStopOverlay {
  public:
    /**
     * @brief Get singleton instance
     * @return Reference to the global EmergencyStopOverlay instance
     */
    static EmergencyStopOverlay& instance();

    /**
     * @brief Initialize with dependencies
     *
     * Must be called before create(). Sets up references to printer state
     * and API for operation.
     *
     * @param printer_state Reference to helix::PrinterState for print job state
     * @param api Pointer to IMoonrakerAPI for emergency_stop() calls
     */
    void init(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    /**
     * @brief Initialize subjects for XML binding
     *
     * Registers the estop_visible subject used by XML binding.
     * Must be called during subject initialization phase (before XML creation).
     */
    void init_subjects();

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Must be called before lv_deinit() to prevent observer corruption.
     */
    void deinit_subjects();

    /**
     * @brief Initialize visibility coordination
     *
     * Sets up observers to update the estop_visible subject based on print
     * state. E-Stop buttons embedded in panels (home, controls, print_status)
     * bind to this subject for reactive visibility.
     *
     * Must be called after:
     * - init() with valid dependencies
     * - init_subjects() for XML binding
     */
    void create();

    /**
     * @brief Force visibility update
     *
     * Recalculates and applies estop_visible subject based on current
     * print state. Called automatically by state observers, but can be
     * called manually if needed.
     */
    void update_visibility();

    /**
     * @brief Set whether confirmation dialog is required
     *
     * When enabled, clicking E-Stop shows a confirmation dialog before
     * executing. When disabled (default), E-Stop executes immediately.
     *
     * @param require true to require confirmation, false for immediate action
     */
    void set_require_confirmation(bool require);

    /**
     * @brief Show recovery dialog for a specific reason
     *
     * Called for both SHUTDOWN state and KLIPPY_DISCONNECTED events.
     * If the dialog is already showing, updates the content to reflect
     * the combined error state (e.g., SHUTDOWN + DISCONNECTED).
     *
     * Safe to call from any thread — callers include MoonrakerClient's event
     * handler on the libhv event-loop thread and AbortManager. Only the atomic
     * suppression check runs on the caller's thread; the rest is marshalled.
     *
     * @param reason Why the recovery dialog is being shown
     */
    void show_recovery_for(RecoveryReason reason);

    /**
     * @brief Suppress recovery dialog for a duration
     *
     * Unified suppression for both SHUTDOWN and DISCONNECTED modals.
     * Used before expected restarts (SAVE_CONFIG, PID calibration).
     *
     * @param duration_ms How long to suppress (default LONG)
     */
    void suppress_recovery_dialog(uint32_t duration_ms = RecoverySuppression::LONG);

    /**
     * @brief Check if recovery dialog suppression is active
     * @return true if suppression window is still active
     */
    bool is_recovery_suppressed() const;

    /**
     * @brief Whether Klipper is expected to bounce through a transient SHUTDOWN.
     *
     * True while a SAVE_CONFIG-style suppression window is active
     * (is_recovery_suppressed()) or a user-initiated restart is in flight
     * (restart_in_progress_). UI consumers of klippy_state use this to treat the
     * transient SHUTDOWN as a restart rather than an error — no red status icon,
     * no navigate-to-home, no firmware_restart widget injection.
     *
     * @return true if a SHUTDOWN right now should be read as an expected restart
     */
    bool is_expected_restart() const;

  private:
    EmergencyStopOverlay() = default;
    ~EmergencyStopOverlay() = default;

    // Non-copyable
    EmergencyStopOverlay(const EmergencyStopOverlay&) = delete;
    EmergencyStopOverlay& operator=(const EmergencyStopOverlay&) = delete;

    // Dependencies (set via init())
    helix::PrinterState* printer_state_ = nullptr;
    IMoonrakerAPI* api_ = nullptr;

    // Confirmation requirement (set via set_require_confirmation())
    bool require_confirmation_ = false;

    // Dialog widget references (created on-demand)
    lv_obj_t* confirmation_dialog_ = nullptr;
    lv_obj_t* recovery_dialog_ = nullptr;

    // Restart operation tracking - prevents recovery dialog during expected SHUTDOWN.
    // Atomic: written from the klippy_state observer (may run on the WebSocket
    // thread) and read by is_expected_restart() on the LVGL main thread.
    std::atomic<bool> restart_in_progress_{false};

    // Skip the first klippy_state observer fire — it carries the subject's
    // default (SHUTDOWN) before Moonraker has reported real state. A real
    // shutdown at startup is delivered separately via the
    // MoonrakerEventType::KLIPPY_SHUTDOWN event in MoonrakerManager.
    bool klippy_state_initial_seen_ = false;

    // Recovery dialog state
    RecoveryReason recovery_reason_ = RecoveryReason::NONE;

    // Time-based suppression for expected restarts (SAVE_CONFIG, PID calibration).
    // Atomic: suppress_recovery_dialog() is called from Moonraker gcode callbacks
    // that run on the WebSocket background thread (e.g. z_offset_utils.cpp), while
    // is_recovery_suppressed() / is_expected_restart() read it from the LVGL main
    // thread. Plain uint32_t here was a data race.
    std::atomic<uint32_t> suppress_recovery_until_{0};

    // Visibility subject (1=visible, 0=hidden) - drives XML bindings
    lv_subject_t estop_visible_;

    // Recovery dialog subjects (drive XML bindings in klipper_recovery_dialog.xml)
    lv_subject_t recovery_title_subject_;
    char recovery_title_buf_[64]{};
    lv_subject_t recovery_message_subject_;
    char recovery_message_buf_[512]{};
    lv_subject_t recovery_can_restart_; // 1=show restart buttons, 0=hide (disconnected)
    // Klipper error code split out of a JSON state_message (e.g. "key1"). Rendered
    // dim in the dialog header; recovery_has_code_ drives its visibility, since
    // bind_flag_if_eq compares ints and cannot test a string for emptiness.
    lv_subject_t recovery_code_subject_;
    char recovery_code_buf_[64]{};
    lv_subject_t recovery_has_code_; // 1=code present, 0=none (hides the label)

    bool subjects_initialized_ = false;

    // RAII subject manager for automatic cleanup
    SubjectManager subjects_;

    // State observers
    ObserverGuard print_state_observer_;
    ObserverGuard klippy_state_observer_;

    // Event handlers
    void handle_click();
    void execute_emergency_stop();
    void show_confirmation_dialog();
    void dismiss_confirmation_dialog();
    friend class EmergencyStopOverlayTestAccess;

    void show_recovery_dialog();
    /// Main-thread half of show_recovery_for(). Reads and writes
    /// recovery_dialog_/recovery_reason_ and queries ModalStack, none of which
    /// may be touched from the libhv thread.
    void show_recovery_for_main(RecoveryReason reason);
    void dismiss_recovery_dialog();
    void update_recovery_dialog_content();
    void restart_klipper();
    void firmware_restart();

    // Static callbacks
    static void emergency_stop_clicked(lv_event_t* e);
    static void estop_dialog_cancel_clicked(lv_event_t* e);
    static void estop_dialog_confirm_clicked(lv_event_t* e);
    static void recovery_restart_klipper_clicked(lv_event_t* e);
    static void recovery_firmware_restart_clicked(lv_event_t* e);
    static void recovery_dismiss_clicked(lv_event_t* e);
    static void advanced_estop_clicked(lv_event_t* e);
    static void advanced_restart_klipper_clicked(lv_event_t* e);
    static void advanced_firmware_restart_clicked(lv_event_t* e);
    static void home_firmware_restart_clicked(lv_event_t* e);
};

namespace helix {
namespace ui {

/// Disconnect-modal suppression window armed by begin_expected_klippy_restart().
/// Mirrors RecoverySuppression::LONG - the reconnect blip from a klippy restart
/// resolves in roughly the same span as the recovery-dialog window. 15 s is the
/// value input-shaper armed before the helper existed; it is uniform now.
constexpr uint32_t EXPECTED_RESTART_DISCONNECT_MODAL_MS = 15000;

/**
 * @brief Initiate an action that will restart Klipper
 *
 * The initiation counterpart of the klippy_state READY observer's completion
 * in this file's .cpp: arms the recovery-dialog suppression window and the
 * api-level disconnect-modal window, then shows one INFO toast so the user
 * knows a restart is coming. The READY observer completes the contract -
 * dismissing the recovery dialog with a "Printer ready" success toast, or
 * firing that toast directly when the dialog was suppressed by the flow that
 * initiated the restart. Mutual coverage: if the recovery window expires
 * before klippy returns, the recovery dialog shows and the dialog path
 * completes instead.
 *
 * The toast is a direct ToastManager call (no ui_notification_* severity, so
 * no notification-history row) and hops through the update queue, which makes
 * this safe from any thread - callers include Moonraker gcode callbacks that
 * run on the WebSocket thread (z_offset_utils.cpp).
 *
 * @param message UNTRANSLATED source string; translated on the main thread
 *                when the toast is shown
 */
void begin_expected_klippy_restart(const char* message);

} // namespace ui
} // namespace helix
