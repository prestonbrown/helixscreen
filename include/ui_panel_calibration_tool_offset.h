// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "helix/xml/indexed_subject_pool.h"
#include "moonraker_error.h"
#include "operation_timeout_guard.h"
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
 * A run is one blocking gcode, and its completion is the only progress this
 * screen follows: the status line reads Calibrating until the rpc answers,
 * and the rows keep showing the offsets as the macro writes them (ToolState
 * publishes every write). No row says which tool is under the probe: status
 * only says which tool is mounted, and the tool the macro measures the
 * others against is mounted without being probed, so the two differ.
 *
 * ## Subject Bindings
 *
 * - tool_cal_status (string) - one-line status
 * - tool_cal_hint (string) - the macro's `description:`, or a built-in note
 * - tool_cal_active (int) - a run is in flight
 * - tool_cal_tool_count (int) - rows the <repeat> builds, one per tool
 * - tool_cal_x_N / _y_N / _z_N (string) - the tool's offsets, mm
 *
 * Save binds ToolState's own any_tool_offset_dirty: an offset the macro wrote
 * (or anything else changed at runtime) that printer.cfg does not yet hold.
 *
 * ## Stopping
 *
 * The macro blocks Klipper's gcode queue, so there is no clean cancel: Stop
 * is M112 + FIRMWARE_RESTART, with the disconnect it causes suppressed as an
 * expected one. The restart re-reads printer.cfg, so the offsets measured
 * before the stop are discarded with it - as the bed mesh and PID panels
 * discard an interrupted run.
 */
namespace helix::ui {

class ToolOffsetCalibrationPanel : public OverlayBase {
  public:
    /// Ceiling on the calibration rpc. Moonraker never times out
    /// printer.gcode.script, so this is ours alone. The macro heats each tool
    /// before probing it, one after another: ~30 s to measure and up to ~2 min
    /// to heat from cold per tool, so a cold four-tool run is ~10 min. Expiry
    /// does not fail the run while Klipper still reports busy - see
    /// on_run_rpc_error().
    static constexpr uint32_t CALIBRATION_TIMEOUT_MS = 900000; // 15 min

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
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;
    void on_ui_destroyed() override;

    /// Whether the connected printer can run the calibration at all.
    static bool printer_supports_calibration();

    /// Confirm, then run CALIBRATE_TOOL_OFFSETS (no-op while a run is in flight)
    void start_calibration();
    /// The run itself, once the user has confirmed
    void begin_run();
    /// The rpc failed. A TIMEOUT while Klipper still reports idle_timeout
    /// "Printing" is not a failure: the macro is still running, so the run
    /// completes on the busy->idle edge instead (public so a test can feed the
    /// timeout without waiting one out).
    void on_run_rpc_error(const MoonrakerError& err);
    /// Stop a run: M112 + firmware restart. Returns false if nothing was running.
    bool abort_in_progress_calibration();
    /// Confirm, then persist every unsaved tool offset (restarts Klipper)
    void save_offsets();
    /// The save itself, once the user has accepted the restart
    void send_save();

    // State access for tests
    [[nodiscard]] bool is_calibration_active() const {
        return run_active_;
    }
    lv_subject_t* get_status_subject() {
        return &status_;
    }
    lv_subject_t* get_active_subject() {
        return &active_;
    }
    lv_subject_t* get_tool_count_subject() {
        return &tool_count_;
    }
    lv_subject_t* get_hint_subject() {
        return &hint_;
    }

  private:
    void on_run_finished(bool ok, const std::string& error);
    /// Repaint every row from ToolState
    void refresh_rows();
    void refresh_row_values(int tool);
    /// ToolState's tools changed: the rows follow its offsets
    void on_tools_changed();
    /// The rpc timed out under a still-busy printer: finish on the idle edge,
    /// with one more ceiling as the backstop
    void begin_idle_wait();
    void finish_idle_wait();
    /// The confirmation text: heat, probe, what to check first
    std::string start_prompt() const;

    // XML event trampolines
    static void on_start_clicked(lv_event_t* e);
    static void on_stop_clicked(lv_event_t* e);
    static void on_save_clicked(lv_event_t* e);

    /// A calibration rpc is in flight (or being waited out, see
    /// begin_idle_wait). Cleared by its completion, a failure, or Stop.
    bool run_active_ = false;
    /// Text of the failure that ended the last run; empty when it succeeded.
    std::string last_error_;

    char status_buffer_[160] = "";
    char hint_buffer_[320] = "";
    lv_subject_t status_;
    lv_subject_t hint_;
    lv_subject_t active_;
    /// Drives the XML <repeat>: one row per tool, however many the printer
    /// has. Published LAST by refresh_rows(), after the pools below hold every
    /// row's values, so a rebuild binds to populated subjects (the macros
    /// panel's rule for its own list).
    lv_subject_t tool_count_;

    // === Per-row subject pools (grow-only; reclaimed when the UI goes) ===
    helix::xml::IndexedSubjectPool row_x_{"tool_cal_x",
                                          helix::xml::IndexedSubjectPool::Type::String, 16};
    helix::xml::IndexedSubjectPool row_y_{"tool_cal_y",
                                          helix::xml::IndexedSubjectPool::Type::String, 16};
    helix::xml::IndexedSubjectPool row_z_{"tool_cal_z",
                                          helix::xml::IndexedSubjectPool::Type::String, 16};

    SubjectManager subjects_;
    /// Follows ToolState's tools_version so the values track the printer.
    ObserverGuard tools_observer_;
    helix::ui::SaveConfigWatch save_watch_;
    /// The wait for the busy->idle edge after the rpc ceiling expired under a
    /// macro that was still running (see on_run_rpc_error).
    bool idle_wait_active_ = false;
    OperationTimeoutGuard idle_wait_backstop_;
    ObserverGuard idle_wait_observer_;
    /// Guards the run's completion and the save's outcome - NOT lifetime_,
    /// which OverlayBase expires on every deactivation: a run deliberately
    /// outlives the screen (see on_deactivating), so its callbacks must too.
    /// Expired by Stop, cleanup() and destruction only. Declared last so
    /// reverse-order destruction expires it before the subjects it writes.
    helix::AsyncLifetimeGuard run_lifetime_;
};

/// Register the Advanced-panel row click callback ("on_tool_offset_row_clicked")
void init_tool_offset_row_handler();

/// Singleton accessor (lazily created, destroyed via StaticPanelRegistry)
ToolOffsetCalibrationPanel& get_global_tool_offset_cal_panel();

} // namespace helix::ui
