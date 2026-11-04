// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"
#include "operation_timeout_guard.h"
#include "overlay_base.h"
#include "save_config_restart.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "z_offset_utils.h"

#include <string>

namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

/**
 * @file ui_panel_calibration_zoffset.h
 * @brief Z-Offset calibration panel with strategy-aware dispatch
 *
 * Interactive panel that guides the user through the paper test calibration
 * process. Supports three strategies based on printer configuration:
 * - PROBE_CALIBRATE: Klipper's PROBE_CALIBRATE -> TESTZ -> ACCEPT -> SAVE_CONFIG
 * - ENDSTOP: Z_ENDSTOP_CALIBRATE -> TESTZ -> ACCEPT -> Z_OFFSET_APPLY_ENDSTOP -> SAVE_CONFIG
 * - FIRMWARE_MANAGED: G28 -> move to center -> G1 Z adjustments -> SET_GCODE_OFFSET
 *
 * ## State Machine:
 * - IDLE: Shows instructions and Start button
 * - PROBING: Waiting for calibration to begin (homes + positions)
 * - ADJUSTING: User adjusts Z with paper test (+/- buttons)
 * - SAVING: Saving offset (ACCEPT/SAVE_CONFIG or SET_GCODE_OFFSET)
 * - COMPLETE: Calibration successful
 * - ERROR: Something went wrong
 *
 * ## Usage:
 * ```cpp
 * auto& overlay = get_global_zoffset_cal_panel();
 * overlay.set_api(get_moonraker_api());
 * overlay.show(parent_screen);
 * ```
 */
class ZOffsetCalibrationPanel : public OverlayBase {
  public:
    /**
     * @brief Calibration state machine states
     */
    enum class State {
        IDLE,      ///< Ready to start, showing instructions
        PROBING,   ///< PROBE_CALIBRATE running
        ADJUSTING, ///< Interactive Z adjustment phase
        SAVING,    ///< ACCEPT sent, waiting for SAVE_CONFIG
        COMPLETE,  ///< Calibration finished successfully
        ERROR,     ///< Error occurred
        WARMING    ///< Bed warming before calibration (6)
    };

    ~ZOffsetCalibrationPanel() override;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize LVGL subjects for XML data binding
     *
     * Call once at startup before any panel instances are created.
     * Registers the zoffset_cal_state subject and all XML event callbacks.
     */
    void init_subjects() override;

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent Parent screen widget to attach overlay to
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* xml_component() const override {
        return "calibration_zoffset_panel";
    }

    /**
     * @brief Get human-readable overlay name
     * @return "Z-Offset Calibration"
     */
    const char* get_name() const override {
        return "Z-Offset Calibration";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Resets state to IDLE.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Aborts calibration if in progress.
     */
    void on_deactivating(DeactivateReason reason) override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    /// Freed on close; the next open rebuilds it. The calibration state lives in
    /// state_ and the zoffset_cal_state subject, never in the widgets.
    bool destroy_on_close() const override {
        return true;
    }

    //
    // === Public API ===
    //

    /**
     * @brief Set the IMoonrakerAPI for G-code commands
     *
     * @param api IMoonrakerAPI for sending commands
     */
    void set_api(helix::IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Get current calibration state
     * @return Current State
     */
    State get_state() const {
        return state_;
    }

    /**
     * @brief Update Z position display (called from external state updates)
     * @param z_position Current Z position from printer state
     */
    void update_z_position(float z_position);

    /**
     * @brief Handle calibration completion/error from Moonraker
     * @param success true if calibration completed successfully
     * @param message Status message
     */
    void on_calibration_result(bool success, const std::string& message);

  private:
    void on_ui_destroyed() override;

    // API reference
    // Note: overlay_root_ inherited from OverlayBase
    helix::IMoonrakerAPI* api_ = nullptr;

    // State management
    State state_ = State::IDLE;
    void set_state(State new_state);

    // UI setup (called by create())
    void setup_widgets();

    // Strategy-aware gcode command helpers
    void start_calibration();
    void begin_probe_sequence(); ///< Start homing/probing (called after warming or directly)
    void adjust_z(float delta);
    void send_accept();
    void send_abort();
    /// Puts ABORT on the wire and leaves the panel state alone.
    void send_manual_probe_abort();
    /// Ends a manual probe session this panel's run left open. Klipper registers
    /// ABORT only during a session, so without one nothing is sent.
    void abort_manual_probe_if_active();

    // Event handlers
    void handle_start_clicked();
    void handle_z_adjust(float delta);
    void handle_accept_clicked();
    void handle_abort_clicked();
    void handle_done_clicked();
    void handle_retry_clicked();
    void handle_warm_bed_toggled();

    /// Turn off bed heater if we turned it on for calibration
    void turn_off_bed_if_needed();

    // Interactive elements
    lv_obj_t* saved_z_offset_display_ = nullptr;
    lv_obj_t* z_position_display_ = nullptr;
    lv_obj_t* final_offset_label_ = nullptr;
    lv_obj_t* error_message_ = nullptr;

    // Current Z position during calibration
    float current_z_ = 0.0f;
    float final_offset_ = 0.0f;
    float cumulative_z_delta_ = 0.0f; ///< Tracks total Z adjustment in gcode_offset mode

    // Warm bed for calibration
    bool bed_was_warmed_ = false;       ///< True if we sent M140 to warm bed this session
    int warm_bed_target_deci_ = 0;      ///< Target temp in decidegrees during WARMING
    SubjectLifetime bed_temp_lifetime_; ///< Lifetime token for bed temp subject (#734)
    ObserverGuard bed_temp_observer_;   ///< Watches bed temp during WARMING phase

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // Observer guards for manual_probe state changes (RAII cleanup)
    ObserverGuard manual_probe_active_observer_;
    ObserverGuard manual_probe_z_observer_;

    // Operation timeout guard (PROBING: 180s, SAVING: 30s)
    OperationTimeoutGuard operation_guard_;
    static constexpr uint32_t kWarmingTimeoutMs = 300000; // 5 min for bed to reach temp
    static constexpr uint32_t kSavingTimeoutMs = 30000;

    /// Arm (or re-arm) the SAVING-state timeout.
    void arm_saving_timeout();

    /// Start/stop watching klippy state for the duration of a save.
    void begin_saving_restart_watch();
    void end_saving_restart_watch();

    /// SAVE_CONFIG restarts Klipper; some printers chain a second config write +
    /// restart tens of seconds later. Each extension buys another
    /// kSavingTimeoutMs, but only if this save actually restarted Klipper — so
    /// a genuinely hung save still fails after one kSavingTimeoutMs.
    static constexpr unsigned kSavingTimeoutMaxExtensions = 4;
    unsigned saving_timeout_extensions_ = 0;

    /// Latches restart activity seen during a save. Reset on every entry to and
    /// exit from State::SAVING so repeated saves in one session start clean.
    /// Owns the SAVE_CONFIG contract for the z-offset save: absorbs the rpc the
    /// restart drops and reports success only once Klipper is back (#1359).
    helix::ui::SaveConfigWatch save_config_watch_;

    helix::ui::SaveRestartLatch save_restart_latch_;

    friend class ZOffsetCalibrationTestAccess;

    /// Watches klippy state while saving, to feed save_restart_latch_.
    /// No paired SubjectLifetime: PrinterState::get_klippy_state_subject() is a
    /// static singleton-lifetime subject (no lifetime-token overload), unlike the
    /// per-sensor bed temp subject above.
    ObserverGuard klippy_state_observer_;
};

// Global instance accessor
// Defined out of line: the ESP32 build, which excludes this panel, supplies its own.
ZOffsetCalibrationPanel& get_global_zoffset_cal_panel();

/**
 * @brief Initialize row click callback for opening from Advanced panel
 *
 * Must be called during app initialization before XML creation.
 * Registers "on_zoffset_row_clicked" callback.
 */
void init_zoffset_row_handler();

/**
 * @brief Initialize event callbacks for the Z-Offset calibration panel
 *
 * Must be called during app initialization before XML creation.
 * Registers all button click callbacks used by calibration_zoffset_panel.xml.
 */
void init_zoffset_event_callbacks();
