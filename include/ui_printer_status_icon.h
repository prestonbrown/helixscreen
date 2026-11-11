// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_guard.h"
#include "lvgl.h"
#include "subject_managed_panel.h"

/**
 * @brief Printer status icon states for XML binding
 */
enum class PrinterIconState {
    READY = 0,       ///< Green - connected and klippy ready
    WARNING = 1,     ///< Orange - startup, reconnecting, was connected
    ERROR = 2,       ///< Red - klippy error/shutdown, connection failed
    DISCONNECTED = 3 ///< Gray - never connected
};

/**
 * @brief Singleton manager for printer status icon
 *
 * Manages the printer connection status icon in the navbar, combining
 * WebSocket connection state and Klippy state into a single visual indicator.
 *
 * Icon states:
 * - Green (READY): Connected and Klippy ready
 * - Orange (WARNING): Klippy starting up, reconnecting, or was previously connected
 * - Red (ERROR): Klippy error/shutdown or connection failed
 * - Gray (DISCONNECTED): Never connected
 *
 * Uses LVGL subjects for reactive XML bindings and ObserverGuard for RAII cleanup.
 *
 * Usage:
 *   PrinterStatusIcon::instance().init_subjects();  // Before XML creation
 *   // Create XML...
 *   PrinterStatusIcon::instance().init();           // After XML creation
 */
class PrinterStatusIcon {
  public:
    static PrinterStatusIcon& instance();

    PrinterStatusIcon(const PrinterStatusIcon&) = delete;
    PrinterStatusIcon& operator=(const PrinterStatusIcon&) = delete;
    PrinterStatusIcon(PrinterStatusIcon&&) = delete;
    PrinterStatusIcon& operator=(PrinterStatusIcon&&) = delete;

    /**
     * @brief Initialize printer icon subjects for XML reactive bindings
     *
     * Must be called BEFORE app_layout XML is created so XML bindings can find subjects.
     * Registers the following subject:
     * - printer_icon_state (int: 0=ready, 1=warning, 2=error, 3=disconnected)
     */
    void init_subjects();

    /**
     * @brief Initialize the printer status icon system
     *
     * Sets up observers on PrinterState subjects to update printer icon state.
     * Should be called after XML is created.
     */
    void init();

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Must be called before lv_deinit() to prevent observer corruption.
     */
    void deinit_subjects();

    /**
     * @brief Pure mapping from connection + klippy state to the displayed icon.
     *
     * Pure, so the state logic is unit-testable without LVGL subjects or the
     * singleton. When @p expected_restart is true, a transient Klipper SHUTDOWN
     * (from a SAVE_CONFIG bounce or user-initiated restart) is shown as WARNING
     * rather than ERROR — no red flash mid-calibration.
     *
     * @param connection_state ConnectionState enum value
     * @param klippy_state     KlippyState enum value
     * @param ever_connected   Whether the printer was ever connected this session
     * @param expected_restart EmergencyStopOverlay::is_expected_restart()
     */
    static PrinterIconState compute_state(int connection_state, int klippy_state,
                                          bool ever_connected, bool expected_restart);

  private:
    PrinterStatusIcon() = default;
    ~PrinterStatusIcon() = default;

    /**
     * @brief Update printer icon based on combined connection and klippy state
     */
    void update_icon_state();

    // RAII subject manager for automatic cleanup
    SubjectManager subjects_;

    // Printer icon state: 0=ready(green), 1=warning(orange), 2=error(red), 3=disconnected(gray)
    lv_subject_t printer_icon_state_subject_{};

    // RAII observer guards for automatic cleanup
    ObserverGuard connection_observer_;
    ObserverGuard klippy_observer_;

    // Cached state for combined printer icon logic
    int32_t cached_connection_state_ = 0;
    int32_t cached_klippy_state_ = 2; // 0=READY, 1=STARTUP, 2=SHUTDOWN, 3=ERROR

    bool subjects_initialized_ = false;
    bool initialized_ = false;
};
