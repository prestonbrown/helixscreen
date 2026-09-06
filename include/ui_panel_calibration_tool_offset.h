// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"

#include "overlay_base.h"
#include "save_config_restart.h"
#include "subject_managed_panel.h"
#include "tool_offset_calibration.h"

#include <lvgl.h>
#include <string>

/**
 * @file ui_panel_calibration_tool_offset.h
 * @brief Automatic tool offset calibration overlay for tool changers
 *
 * Shown from the Controls / Advanced calibration entry points when the
 * printer is a tool changer that defines klipper-toolchanger's
 * CALIBRATE_TOOL_OFFSETS macro (helix::tool_offset_calibration::supported).
 * The macro owns the whole procedure - heating, selecting each tool, probing
 * it on the sensor, writing the result into the tool's gcode_x/y/z_offset -
 * so this panel runs it, follows it, and offers the one thing the macro does
 * not do: the SAVE_CONFIG that persists the result.
 *
 * ## What the rows show
 *
 * One row per tool. Every row shows the tool's current X/Y/Z offset - the
 * same numbers ToolState carries for the tune overlay and the save path, read
 * off the `tool T<n>` objects through helix::tool_offsets, never parsed off
 * the console. Which tool the others are measured against, how the nozzles
 * are heated and what the sensor is are the macro's business: nothing here
 * assumes a reference tool, and no sensor position is shown.
 *
 * During a run each row's whole appearance follows one int subject
 * (ToolStep, mirrored 1:1 into tool_cal_state_N): the measuring row
 * highlights in place, later rows read Queued, finished rows read Done.
 * Progress comes from status the app already subscribes to, never from the
 * console: ToolState's active tool says which tool is on the carriage, and a
 * tool whose offsets change during the run has been measured. Both feed
 * helix::tool_offset_calibration::Run.
 *
 * ## Subject Bindings
 *
 * - tool_cal_status (string) - one-line status
 * - tool_cal_hint (string) - the macro's `description:`, or a built-in note
 * - tool_cal_active (int) - a run is in flight
 * - tool_cal_row_visible_N (int) - row N exists on this printer
 * - tool_cal_state_N (int) - ToolStep for tool N
 * - tool_cal_state_text_N (string) - "Queued" / "Measuring... 4s" / "Done" / "Failed"
 * - tool_cal_x_N / _y_N / _z_N (string) - the tool's offsets, mm
 *
 * Save binds ToolState's own any_tool_offset_dirty: an offset the macro wrote
 * (or anything else changed at runtime) that printer.cfg does not yet hold.
 *
 * ## Stopping
 *
 * The macro blocks Klipper's gcode queue, so there is no clean cancel: Stop
 * is M112 + FIRMWARE_RESTART, with the disconnect it causes suppressed as an
 * expected one. Tools measured before the stop keep their (unsaved) offsets.
 */
namespace helix::ui {

class ToolOffsetCalibrationPanel : public OverlayBase {
  public:
    /// Fixed subject slots; rows beyond the printer's tool count stay hidden.
    static constexpr int MAX_TOOLS = 8;

    ToolOffsetCalibrationPanel();
    ~ToolOffsetCalibrationPanel() override;

    ToolOffsetCalibrationPanel(const ToolOffsetCalibrationPanel&) = delete;
    ToolOffsetCalibrationPanel& operator=(const ToolOffsetCalibrationPanel&) = delete;
    ToolOffsetCalibrationPanel(ToolOffsetCalibrationPanel&&) = delete;
    ToolOffsetCalibrationPanel& operator=(ToolOffsetCalibrationPanel&&) = delete;

    // === OverlayBase Interface ===
    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Tool Offset Calibration";
    }
    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;

    /// Push the overlay onto the navigation stack (create() must have run)
    void show();

    /// Whether the connected printer can run the calibration at all.
    static bool printer_supports_calibration();

    /// Confirm, then run CALIBRATE_TOOL_OFFSETS (no-op while a run is in flight)
    void start_calibration();
    /// The run itself, once the user has confirmed
    void begin_run();
    /// Stop a run: M112 + firmware restart. Returns false if nothing was running.
    bool abort_in_progress_calibration();
    /// Confirm, then persist every unsaved tool offset (restarts Klipper)
    void save_offsets();
    /// The save itself, once the user has accepted the restart
    void send_save();

    // State access for tests
    [[nodiscard]] bool is_calibration_active() const {
        return run_.active();
    }
    [[nodiscard]] const helix::tool_offset_calibration::Run& run() const {
        return run_;
    }
    lv_subject_t* get_status_subject() {
        return &status_;
    }
    lv_subject_t* get_hint_subject() {
        return &hint_;
    }
    lv_subject_t* get_row_state_subject(int tool) {
        return &row_state_[tool];
    }

  private:
    void on_run_finished(bool ok, const std::string& error);
    /// Repaint every row from ToolState (values, visibility) and run_ (state)
    void refresh_rows();
    void refresh_row_state(int tool);
    void refresh_row_values(int tool);
    /// ToolState's active tool changed: during a run, that tool is Measuring
    void on_active_tool_changed(int tool);
    /// ToolState's tools changed: during a run, a tool whose offsets moved
    /// since the run began has been measured
    void on_tools_changed();
    void fetch_macro_description();
    /// The confirmation text: heat, probe, what to check first
    std::string start_prompt() const;

    // XML event trampolines
    static void on_start_clicked(lv_event_t* e);
    static void on_stop_clicked(lv_event_t* e);
    static void on_save_clicked(lv_event_t* e);

    helix::tool_offset_calibration::Run run_;
    /// Every tool's offsets as they stood when the run began, so a change
    /// during the run reads as "this tool was measured".
    float run_baseline_mm_[MAX_TOOLS][3] = {};
    bool run_baseline_known_[MAX_TOOLS][3] = {};
    /// Text of the failure that ended the last run; empty when it succeeded.
    std::string last_error_;

    char status_buffer_[160] = "";
    char hint_buffer_[320] = "";
    lv_subject_t status_;
    lv_subject_t hint_;
    lv_subject_t active_;

    lv_subject_t row_visible_[MAX_TOOLS];
    lv_subject_t row_state_[MAX_TOOLS];
    lv_subject_t row_state_text_[MAX_TOOLS];
    lv_subject_t row_x_[MAX_TOOLS];
    lv_subject_t row_y_[MAX_TOOLS];
    lv_subject_t row_z_[MAX_TOOLS];
    char row_state_text_buffer_[MAX_TOOLS][48];
    char row_x_buffer_[MAX_TOOLS][16];
    char row_y_buffer_[MAX_TOOLS][16];
    char row_z_buffer_[MAX_TOOLS][16];

    SubjectManager subjects_;
    /// Follows ToolState's tools_version so the values track the printer.
    ObserverGuard tools_observer_;
    /// Follows ToolState's active tool: the row under the probe during a run.
    ObserverGuard active_tool_observer_;
    helix::ui::ElapsedLabelTimer elapsed_;
    helix::ui::SaveConfigWatch save_watch_;
};

/// Register the Advanced-panel row click callback ("on_tool_offset_row_clicked")
void init_tool_offset_row_handler();

/// Singleton accessor (lazily created, destroyed via StaticPanelRegistry)
ToolOffsetCalibrationPanel& get_global_tool_offset_cal_panel();

} // namespace helix::ui
