// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_temp_graph.h"

#include "klipper_config_editor.h"
#include "moonraker_advanced_api.h"
#include "overlay_base.h"
#include "pid_progress_tracker.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <string>

class IMoonrakerAPI;
class MoonrakerAdvancedAPI;
class TemperatureService;

/**
 * @file ui_panel_calibration_pid.h
 * @brief PID Tuning Calibration Panel
 *
 * Interactive calibration using Klipper's PID_CALIBRATE command.
 * Supports both extruder and heated bed PID tuning.
 *
 * ## Klipper Commands Used:
 * - `PID_CALIBRATE HEATER=extruder TARGET=<temp>` - Extruder tuning
 * - `PID_CALIBRATE HEATER=heater_bed TARGET=<temp>` - Bed tuning
 * - `SAVE_CONFIG` - Persist results (restarts Klipper)
 *
 * ## State Machine:
 * IDLE → CALIBRATING → SAVING → COMPLETE
 *                   ↘ ERROR
 *
 * ## Typical Duration:
 * - Extruder: 3-5 minutes
 * - Heated Bed: 5-10 minutes (larger thermal mass)
 */
namespace helix {
namespace ui {
struct PIDCalibrationPanelTestAccess; // test-only friend (tests/test_helpers/)
} // namespace ui
} // namespace helix

class PIDCalibrationPanel : public OverlayBase {
    friend struct helix::ui::PIDCalibrationPanelTestAccess;

  public:
    /**
     * @brief Calibration state machine states
     */
    enum class State {
        IDLE,        ///< Ready to start, heater selection shown
        CALIBRATING, ///< PID/MPC calibration running, showing progress
        SAVING,      ///< SAVE_CONFIG running, Klipper restarting
        COMPLETE,    ///< Calibration successful, showing results
        ERROR,       ///< Something went wrong
        MIGRATING    ///< Updating config for PID->MPC migration
    };

    /**
     * @brief Which heater is being calibrated
     */
    enum class Heater { EXTRUDER, BED };

    /**
     * @brief Calibration method
     */
    enum class CalibMethod { PID, MPC };

    PIDCalibrationPanel();
    ~PIDCalibrationPanel() override;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize LVGL subjects for XML data binding
     *
     * Call once at startup before any panel instances are created.
     * Registers the pid_cal_state subject and all XML event callbacks.
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize LVGL subjects for clean shutdown
     *
     * Disconnects all observers and deinitializes subjects.
     * Called automatically by destructor, but can be called earlier
     * for explicit cleanup before LVGL deinit.
     */
    void deinit_subjects();

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent Parent screen widget to attach overlay to
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "PID Calibration"
     */
    const char* get_name() const override {
        return "PID Calibration";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Resets state to IDLE, refreshes UI to defaults.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Cancels pending timers, aborts calibration if in progress.
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
     * @brief Show overlay panel
     *
     * Pushes overlay onto navigation stack and registers with NavigationManager.
     * on_activate() will be called automatically after animation completes.
     */
    void show();

    /**
     * @brief Set the Moonraker API for high-level operations
     *
     * @param api IMoonrakerAPI for PID calibrate and save_config
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Get current state
     */
    State get_state() const {
        return state_;
    }

    /**
     * @brief Request demo results injection after next on_activate()
     *
     * Sets a pending flag so that on_activate() will call inject_demo_results()
     * after finishing its normal reset. Call before show().
     */
    void request_demo_inject() {
        demo_inject_pending_ = true;
    }

    /**
     * @brief Inject demo results for screenshot/demo mode
     *
     * Populates the panel with realistic PID calibration results
     * matching mock backend values, then transitions to COMPLETE state.
     */
    void inject_demo_results();

    /**
     * @brief Set TemperatureService for graph registration
     *
     * @param tcp TemperatureService that manages temperature graph updates
     */
    void set_temp_control_panel(TemperatureService* tcp);

    /**
     * @brief Called when calibration completes with results
     *
     * @param success True if calibration succeeded
     * @param kp Proportional gain (only valid if success)
     * @param ki Integral gain (only valid if success)
     * @param kd Derivative gain (only valid if success)
     * @param error_message Error description (only valid if !success)
     */
    void on_calibration_result(bool success, float kp = 0, float ki = 0, float kd = 0,
                               const std::string& error_message = "");

    /// Test-only: arm the ETA refresh timer without running a calibration, so the
    /// teardown paths that must cancel it can be exercised directly.
    void arm_eta_timer_for_test();

    /// Test-only: the armed ETA timer, or nullptr when none is armed.
    lv_timer_t* eta_timer_for_test() const {
        return eta_update_timer_;
    }

  private:
    // Client/API references
    // Note: overlay_root_ inherited from OverlayBase
    lv_obj_t* parent_screen_ = nullptr;
    IMoonrakerAPI* api_ = nullptr;

    // State
    State state_ = State::IDLE;
    Heater selected_heater_ = Heater::EXTRUDER;
    int target_temp_ = 200;         // Default for extruder
    int fan_speed_ = 0;             // Part cooling fan speed for extruder PID
    std::string selected_material_; // Active material preset name

    // Temperature limits
    static constexpr int EXTRUDER_MIN_TEMP = 150;
    static constexpr int EXTRUDER_MAX_TEMP = 280;
    static constexpr int EXTRUDER_DEFAULT_TEMP = 200;
    static constexpr int BED_MIN_TEMP = 40;
    static constexpr int BED_MAX_TEMP = 110;
    static constexpr int BED_DEFAULT_TEMP = 60;

    // Wattage limits
    static constexpr int WATTAGE_MIN = 20;
    static constexpr int WATTAGE_MAX = 1000;
    static constexpr int WATTAGE_STEP = 5;
    static constexpr int WATTAGE_DEFAULT_EXTRUDER = 50;
    static constexpr int WATTAGE_DEFAULT_BED = 400;

    // Fan breakpoint presets
    static constexpr int FAN_BP_QUICK = 3;
    static constexpr int FAN_BP_DETAILED = 5;
    static constexpr int FAN_BP_THOROUGH = 7;

    // Demo mode: inject results after on_activate() resets state
    bool demo_inject_pending_ = false;

    // PID results
    float result_kp_ = 0;
    float result_ki_ = 0;
    float result_kd_ = 0;

    // Previous PID values (fetched before calibration starts)
    float old_kp_ = 0;
    float old_ki_ = 0;
    float old_kd_ = 0;
    bool has_old_values_ = false;

    // MPC/Kalico state
    CalibMethod selected_method_ = CalibMethod::PID;
    int heater_wattage_ = WATTAGE_DEFAULT_EXTRUDER;
    int fan_breakpoints_ = FAN_BP_QUICK;
    bool needs_migration_ = false;
    bool is_kalico_ = false;

    // MPC results
    MoonrakerAdvancedAPI::MPCResult mpc_result_;

    // KlipperConfigEditor for migration
    helix::system::KlipperConfigEditor config_editor_;

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // String subjects and buffers for reactive text updates
    lv_subject_t subj_temp_display_;
    char buf_temp_display_[16];

    lv_subject_t subj_temp_hint_;
    char buf_temp_hint_[64];

    lv_subject_t subj_calibrating_heater_;
    char buf_calibrating_heater_[32];

    lv_subject_t subj_pid_kp_;
    char buf_pid_kp_[32];

    lv_subject_t subj_pid_ki_;
    char buf_pid_ki_[32];

    lv_subject_t subj_pid_kd_;
    char buf_pid_kd_[32];

    lv_subject_t subj_result_summary_;
    char buf_result_summary_[128];

    lv_subject_t subj_error_message_;
    char buf_error_message_[256];

    // Int subject for showing/hiding extruder-only sections
    lv_subject_t subj_heater_is_extruder_;

    // MPC result string subjects and buffers
    lv_subject_t subj_mpc_heat_capacity_;
    char buf_mpc_heat_capacity_[32];
    lv_subject_t subj_mpc_sensor_resp_;
    char buf_mpc_sensor_resp_[32];
    lv_subject_t subj_mpc_ambient_transfer_;
    char buf_mpc_ambient_transfer_[32];
    lv_subject_t subj_mpc_fan_transfer_;
    char buf_mpc_fan_transfer_[64];

    // Fan speed display subject and buffer
    lv_subject_t subj_fan_speed_text_;
    char buf_fan_speed_text_[8];

    // Wattage display subject and buffer
    lv_subject_t subj_wattage_display_;
    char buf_wattage_display_[16];

    // MPC-related int subjects
    lv_subject_t subj_is_kalico_;       // int 0/1
    lv_subject_t subj_method_is_mpc_;   // int 0/1
    lv_subject_t subj_show_wattage_;    // int 0/1
    lv_subject_t subj_needs_migration_; // int 0/1
    lv_subject_t subj_show_fan_config_; // int 0/1
    lv_subject_t subj_fan_is_quick_;    // int 0/1
    lv_subject_t subj_fan_is_detailed_; // int 0/1
    lv_subject_t subj_fan_is_thorough_; // int 0/1
    lv_subject_t subj_show_pid_fan_;    // 1 when extruder + PID method

    // Int subject: 1 when not idle (disables Start button in header)
    lv_subject_t subj_cal_not_idle_;

    // Progress tracking for calibration
    lv_subject_t subj_pid_progress_; // int 0-100
    lv_subject_t subj_pid_progress_text_;
    char buf_pid_progress_text_[64];
    lv_subject_t subj_pid_eta_;
    char buf_pid_eta_[32];
    int pid_estimated_total_ = 3;      // Dynamic estimate, starts at 3
    bool has_kalico_progress_ = false; // True once first sample callback arrives

    // Tick (lv_tick_get) when the current CALIBRATING run began; 0 when not calibrating.
    // Drives the "taking longer than expected" backstop (#988): once a run exceeds a
    // generous per-heater threshold the status text reassures the user and points at the
    // existing Abort button, rather than hard-failing on the RPC timeout.
    uint32_t cal_start_tick_ms_ = 0;

    // Phase-aware progress tracker
    PidProgressTracker progress_tracker_;

    // Temperature observer for progress tracking
    ObserverGuard progress_temp_observer_;
    SubjectLifetime progress_temp_lifetime_; // for dynamic extruder subject

    // ETA update timer (fires every ~3 seconds to refresh ETA display)
    lv_timer_t* eta_update_timer_ = nullptr;

    void start_progress_tracking();
    void stop_progress_tracking();

    /// Cancel the ETA refresh timer. Safe to call when none is armed, and safe
    /// from the destructor — unlike stop_progress_tracking(), it touches only
    /// the timer, not the observer guards.
    void cancel_eta_timer();
    void on_progress_temperature(int temp_tenths);
    void update_progress_display();
    static void on_eta_timer_tick(lv_timer_t* timer);

    // Progress handler (called from UI thread via queue)
    void on_pid_progress(int sample, float tolerance);

    // Persist measured calibration rates for future ETA estimates
    void save_calibration_history();

    // Widget references
    lv_obj_t* fan_slider_ = nullptr;

    // Temperature graph for calibrating state
    TemperatureService* temp_control_panel_ = nullptr;
    ui_temp_graph_t* pid_graph_ = nullptr;
    int pid_graph_series_id_ = -1;

    // State management
    void set_state(State new_state);

    // Fan control
    void turn_off_fan();

    // UI setup (called by create())
    void setup_widgets();

    // Temperature graph management
    void setup_pid_graph();
    void teardown_pid_graph();

    // UI updates
    void update_fan_slider(int speed);
    void update_temp_display();
    void update_temp_hint();

    // G-code commands
    void send_pid_calibrate();
    void send_mpc_calibrate();
    void send_save_config();
    void fetch_old_pid_values();

    // MPC-specific methods
    void detect_heater_control_type();
    void start_migration();
    void on_mpc_result(const MoonrakerAdvancedAPI::MPCResult& result);
    void on_mpc_progress(int phase, int total_phases, const std::string& desc);

    // Format a PID value with optional delta percentage against old value
    void format_pid_value(char* buf, size_t buf_size, float new_val, float old_val);

    // Update the wattage display subject from heater_wattage_
    void update_wattage_display();

    // Fan section visibility — single source of truth for both subjects
    void update_fan_section_visibility();

    // Event handlers
    void handle_heater_extruder_clicked();
    void handle_heater_bed_clicked();
    void handle_temp_up();
    void handle_temp_down();
    void handle_start_clicked();
    void handle_abort_clicked();
    void handle_done_clicked();
    void handle_retry_clicked();
    void handle_preset_clicked(int temp, const char* material_name);
    void handle_method_pid_clicked();
    void handle_method_mpc_clicked();
    void handle_wattage_up();
    void handle_wattage_down();
    void handle_fan_quick_clicked();
    void handle_fan_detailed_clicked();
    void handle_fan_thorough_clicked();

    // Static trampolines
    static void on_heater_extruder_clicked(lv_event_t* e);
    static void on_heater_bed_clicked(lv_event_t* e);
    static void on_temp_up(lv_event_t* e);
    static void on_temp_down(lv_event_t* e);
    static void on_start_clicked(lv_event_t* e);
    static void on_abort_clicked(lv_event_t* e);
    static void on_done_clicked(lv_event_t* e);
    static void on_retry_clicked(lv_event_t* e);
    static void on_fan_slider_changed(lv_event_t* e);
    // MPC method/config trampolines
    static void on_method_pid_clicked(lv_event_t* e);
    static void on_method_mpc_clicked(lv_event_t* e);
    static void on_wattage_up(lv_event_t* e);
    static void on_wattage_down(lv_event_t* e);
    static void on_fan_quick_clicked(lv_event_t* e);
    static void on_fan_detailed_clicked(lv_event_t* e);
    static void on_fan_thorough_clicked(lv_event_t* e);
    // Material preset trampolines (extruder)
    // One handler per heater; the preset slot is parsed from the clicked
    // button's name ("btn_preset_mN" / "btn_preset_bed_mN") and resolved to a
    // material through helix::presets. Replaces the eight per-material
    // trampolines that hardcoded PLA/PETG/ABS/PA/TPU.
    static void on_pid_preset_material(lv_event_t* e);
    static void on_pid_preset_bed_material(lv_event_t* e);
};

// Global instance accessor
PIDCalibrationPanel& get_global_pid_cal_panel();
