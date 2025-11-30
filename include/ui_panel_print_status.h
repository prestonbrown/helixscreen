// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printer_state.h"
#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include <string>
#include <unordered_set>

/**
 * @brief Print status panel - shows active print progress and controls
 *
 * Displays filename, thumbnail, progress, layers, times, temperatures,
 * speed/flow, and provides pause/tune/cancel buttons.
 */

/**
 * @brief Print state machine states
 */
enum class PrintState {
    Idle,      ///< No active print
    Preparing, ///< Running pre-print operations (homing, leveling, etc.)
    Printing,  ///< Actively printing
    Paused,    ///< Print paused
    Complete,  ///< Print finished successfully
    Cancelled, ///< Print cancelled by user
    Error      ///< Print failed with error
};

// Legacy C-style enum for backwards compatibility
typedef enum {
    PRINT_STATE_IDLE = static_cast<int>(PrintState::Idle),
    PRINT_STATE_PREPARING = static_cast<int>(PrintState::Preparing),
    PRINT_STATE_PRINTING = static_cast<int>(PrintState::Printing),
    PRINT_STATE_PAUSED = static_cast<int>(PrintState::Paused),
    PRINT_STATE_COMPLETE = static_cast<int>(PrintState::Complete),
    PRINT_STATE_CANCELLED = static_cast<int>(PrintState::Cancelled),
    PRINT_STATE_ERROR = static_cast<int>(PrintState::Error)
} print_state_t;

class PrintStatusPanel : public PanelBase {
  public:
    /**
     * @brief Construct PrintStatusPanel with injected dependencies
     *
     * @param printer_state Reference to PrinterState
     * @param api Pointer to MoonrakerAPI (for pause/cancel commands)
     */
    PrintStatusPanel(PrinterState& printer_state, MoonrakerAPI* api);

    ~PrintStatusPanel() override;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Initialize subjects for XML binding
     *
     * Registers all 10 subjects for reactive data binding.
     */
    void init_subjects() override;

    /**
     * @brief Setup button handlers and image scaling
     *
     * - Wires pause, tune, cancel, light buttons
     * - Wires temperature card click handlers
     * - Configures progress bar
     * - Registers resize callback for thumbnail scaling
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen for navigation
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Print Status";
    }
    const char* get_xml_component_name() const override {
        return "print_status_panel";
    }

    //
    // === Public API - Print State Updates ===
    //

    /**
     * @brief Set the current print filename
     * @param filename Print file name to display
     */
    void set_filename(const char* filename);

    /**
     * @brief Set print progress percentage
     * @param percent Progress 0-100 (clamped to valid range)
     */
    void set_progress(int percent);

    /**
     * @brief Set layer progress
     * @param current Current layer number
     * @param total Total layers in print
     */
    void set_layer(int current, int total);

    /**
     * @brief Set elapsed and remaining time
     * @param elapsed_secs Elapsed time in seconds
     * @param remaining_secs Estimated remaining time in seconds
     */
    void set_times(int elapsed_secs, int remaining_secs);

    /**
     * @brief Set temperature readings
     * @param nozzle_cur Current nozzle temperature
     * @param nozzle_tgt Target nozzle temperature
     * @param bed_cur Current bed temperature
     * @param bed_tgt Target bed temperature
     */
    void set_temperatures(int nozzle_cur, int nozzle_tgt, int bed_cur, int bed_tgt);

    /**
     * @brief Set speed and flow percentages
     * @param speed_pct Speed multiplier percentage
     * @param flow_pct Flow multiplier percentage
     */
    void set_speeds(int speed_pct, int flow_pct);

    /**
     * @brief Set print state
     * @param state New print state
     */
    void set_state(PrintState state);

    /**
     * @brief Get current print state
     * @return Current PrintState
     */
    PrintState get_state() const {
        return current_state_;
    }

    //
    // === Pre-Print Preparation State ===
    //

    /**
     * @brief Set the preparing state with operation details
     *
     * Call this when starting a pre-print sequence. The panel will show
     * "Preparing..." with the current operation name and progress.
     *
     * @param operation_name Display name of current operation (e.g., "Homing", "Leveling Bed")
     * @param current_step Current step number (1-based)
     * @param total_steps Total number of steps
     */
    void set_preparing(const std::string& operation_name, int current_step, int total_steps);

    /**
     * @brief Update preparing progress
     *
     * Lighter-weight update for progress during a single operation.
     *
     * @param progress Fractional progress (0.0 to 1.0)
     */
    void set_preparing_progress(float progress);

    /**
     * @brief Clear preparing state and transition to Idle or Printing
     *
     * Call this when the pre-print sequence completes or is cancelled.
     *
     * @param success If true, transitions to Printing; if false, transitions to Idle
     */
    void end_preparing(bool success);

    /**
     * @brief Get current progress percentage
     * @return Progress 0-100
     */
    int get_progress() const {
        return current_progress_;
    }

  private:
    //
    // === Subjects (owned by this panel) ===
    //

    lv_subject_t filename_subject_;
    lv_subject_t progress_text_subject_;
    lv_subject_t layer_text_subject_;
    lv_subject_t elapsed_subject_;
    lv_subject_t remaining_subject_;
    lv_subject_t nozzle_temp_subject_;
    lv_subject_t bed_temp_subject_;
    lv_subject_t speed_subject_;
    lv_subject_t flow_subject_;
    lv_subject_t pause_button_subject_;

    // Preparing state subjects
    lv_subject_t preparing_visible_subject_;   // int: 1 if preparing, 0 otherwise
    lv_subject_t preparing_operation_subject_; // string: current operation name
    lv_subject_t preparing_progress_subject_;  // int: 0-100 progress percentage

    // Viewer mode subject (0=thumbnail mode, 1=gcode viewer mode)
    lv_subject_t gcode_viewer_mode_subject_;

    // Subject storage buffers
    char filename_buf_[128] = "No print active";
    char progress_text_buf_[32] = "0%";
    char layer_text_buf_[64] = "Layer 0 / 0";
    char preparing_operation_buf_[64] = "Preparing...";
    char elapsed_buf_[32] = "0h 00m";
    char remaining_buf_[32] = "0h 00m";
    char nozzle_temp_buf_[32] = "0 / 0°C";
    char bed_temp_buf_[32] = "0 / 0°C";
    char speed_buf_[32] = "100%";
    char flow_buf_[32] = "100%";
    char pause_button_buf_[32] = "Pause";

    //
    // === Instance State ===
    //

    PrintState current_state_ = PrintState::Idle;
    int current_progress_ = 0;
    int current_layer_ = 0;
    int total_layers_ = 0;
    int elapsed_seconds_ = 0;
    int remaining_seconds_ = 0;
    int nozzle_current_ = 0;
    int nozzle_target_ = 0;
    int bed_current_ = 0;
    int bed_target_ = 0;
    int speed_percent_ = 100;
    int flow_percent_ = 100;

    // Child widgets
    lv_obj_t* progress_bar_ = nullptr;
    lv_obj_t* gcode_viewer_ = nullptr;
    lv_obj_t* print_thumbnail_ = nullptr;
    lv_obj_t* gradient_background_ = nullptr;

    // Resize callback registration flag
    bool resize_registered_ = false;

    //
    // === Private Helpers ===
    //

    void update_all_displays();
    void show_gcode_viewer(bool show);
    void load_gcode_file(const char* file_path);

    static void format_time(int seconds, char* buf, size_t buf_size);

    //
    // === Instance Handlers ===
    //

    void handle_nozzle_card_click();
    void handle_bed_card_click();
    void handle_light_button();
    void handle_pause_button();
    void handle_tune_button();
    void handle_cancel_button();
    void handle_resize();

    //
    // === Static Trampolines ===
    //

    static void on_nozzle_card_clicked(lv_event_t* e);
    static void on_bed_card_clicked(lv_event_t* e);
    static void on_light_clicked(lv_event_t* e);
    static void on_pause_clicked(lv_event_t* e);
    static void on_tune_clicked(lv_event_t* e);
    static void on_cancel_clicked(lv_event_t* e);

    // Static resize callback (registered with ui_resize_handler)
    static void on_resize_static();

    //
    // === PrinterState Observer Callbacks ===
    //

    static void extruder_temp_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void extruder_target_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void bed_temp_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void bed_target_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void print_progress_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void print_state_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void print_filename_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void speed_factor_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void flow_factor_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void led_state_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void print_layer_observer_cb(lv_observer_t* observer, lv_subject_t* subject);

    //
    // === Observer Instance Methods ===
    //

    void on_temperature_changed();
    void on_print_progress_changed(int progress);
    void on_print_state_changed(PrintJobState state);
    void on_print_filename_changed(const char* filename);
    void on_speed_factor_changed(int speed);
    void on_flow_factor_changed(int flow);
    void on_led_state_changed(int state);
    void on_print_layer_changed(int current_layer);

    // PrinterState observers (ObserverGuard handles cleanup)
    ObserverGuard extruder_temp_observer_;
    ObserverGuard extruder_target_observer_;
    ObserverGuard bed_temp_observer_;
    ObserverGuard bed_target_observer_;
    ObserverGuard print_progress_observer_;
    ObserverGuard print_state_observer_;
    ObserverGuard print_filename_observer_;
    ObserverGuard speed_factor_observer_;
    ObserverGuard flow_factor_observer_;
    ObserverGuard led_state_observer_;
    ObserverGuard print_layer_observer_;

    bool led_on_ = false;
    std::string configured_led_;

    //
    // === Exclude Object State ===
    //

    /// Objects already excluded (sent to Klipper, cannot be undone)
    std::unordered_set<std::string> excluded_objects_;

    /// Object pending exclusion (in undo window, not yet sent to Klipper)
    std::string pending_exclude_object_;

    /// Timer for undo window (5 seconds before sending EXCLUDE_OBJECT to Klipper)
    lv_timer_t* exclude_undo_timer_{nullptr};

    /// Active confirmation dialog (if showing)
    lv_obj_t* exclude_confirm_dialog_{nullptr};

    //
    // === Exclude Object Handlers ===
    //

    /**
     * @brief Handle long-press on object in G-code viewer
     * Shows confirmation dialog for excluding the object
     */
    void handle_object_long_press(const char* object_name);

    /**
     * @brief Handle confirmation of object exclusion
     * Starts the delayed undo window and shows undo toast
     */
    void handle_exclude_confirmed();

    /**
     * @brief Handle cancellation of exclusion dialog
     */
    void handle_exclude_cancelled();

    /**
     * @brief Handle undo button press on toast (cancels pending exclusion)
     */
    void handle_exclude_undo();

    /**
     * @brief Timer callback when undo window expires - sends EXCLUDE_OBJECT to Klipper
     */
    static void exclude_undo_timer_cb(lv_timer_t* timer);

    //
    // === Exclude Object Static Trampolines ===
    //

    static void on_object_long_pressed(lv_obj_t* viewer, const char* object_name, void* user_data);
    static void on_exclude_confirm_clicked(lv_event_t* e);
    static void on_exclude_cancel_clicked(lv_event_t* e);
};

// Global instance accessor (needed by main.cpp)
PrintStatusPanel& get_global_print_status_panel();
