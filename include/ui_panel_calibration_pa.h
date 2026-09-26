// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "i_moonraker_api.h"
#include "overlay_base.h"
#include "pa_calibration.h"
#include "subject_managed_panel.h"

#include <functional>
#include <lvgl.h>
#include <string>

/**
 * @file ui_panel_calibration_pa.h
 * @brief Automatic pressure-advance calibration overlay
 *
 * Sits beside Z Calibration and Tool Offsets. Pick a tool and a nozzle
 * temperature, let the printer measure pressure advance on its own, then read
 * the number off the screen and type it into a slicer.
 *
 * ## Why this screen can exist at all
 *
 * Stock Klipper cannot do this: its documented method is TUNING_TOWER plus a
 * human judging a printed part. Only firmwares that measure extrusion
 * back-pressure themselves can, and helix::pacal owns which those are. When no
 * provider matches, `printer_has_pa_cal` stays 0 and the entry point is hidden
 * - the screen never claims a capability the machine lacks.
 *
 * ## The run
 *
 * The firmware command assumes a hot nozzle, so the panel owns the heat-up and
 * the procedure owns the measurement. That is also why the wait is presented as
 * two phases rather than one spinner: heating is two minutes of a rising
 * number, measuring is half a minute of the machine extruding, and they feel
 * nothing alike.
 *
 *   IDLE --(Start, confirmed)--> HEATING --(at target)--> MEASURING
 *        <-------------------- COMPLETE / ERROR <--------'
 *
 * ## Nothing is applied and nothing is saved
 *
 * The printer reports a number; this panel does not send SET_PRESSURE_ADVANCE
 * and does not SAVE_CONFIG. The value belongs in the user's slicer profile,
 * per filament - which is the only place it can be right for the next spool
 * too. That is why COMPLETE carries one quiet button (Re-measure) instead of
 * three unequal acts dressed as equals.
 *
 * ## Subject bindings
 *
 * - pa_cal_state (int) - State below; drives which stage card is visible
 * - pa_cal_multi_tool / pa_cal_tool_count (int) - tool row shape
 * - pa_cal_tool_selected_N / pa_cal_tool_sub_N - per-tool chip
 * - pa_cal_inputs_live (int) - 1 while the target column is editable
 * - pa_cal_temp_display / pa_cal_temp_note (string)
 * - pa_cal_preset_selected_N (int) / pa_cal_preset_temp_N (string)
 * - pa_cal_progress (int 0-100), pa_cal_phase_label / _big / _big_sub /
 *   _remaining / _prog_foot (string)
 * - pa_cal_phase_state_N (int 0=pending,1=now,2=done), _phase_name_N,
 *   _phase_meta_N (string)
 * - pa_cal_result / _result_sanity (string), pa_cal_result_plausible (int)
 * - pa_cal_error_title / _detail / _hint (string)
 * - pa_cal_action_text (string), pa_cal_action_is_stop (int)
 * - pa_cal_has_last (int), pa_cal_last_value / _last_context (string)
 */
namespace helix::ui {

class PACalibrationPanel : public OverlayBase {
  public:
    /// Fixed subject slots. Chips beyond the printer's tool count are never
    /// built - <repeat> is driven by pa_cal_tool_count.
    static constexpr int MAX_TOOLS = 4;

    /// Preset slots, mirroring helix::presets::PRESET_COUNT.
    static constexpr int PRESET_SLOTS = 4;

    /// The two phases of a run, in order. Heating is nearly the whole wait.
    static constexpr int PHASE_HEAT = 0;
    static constexpr int PHASE_MEASURE = 1;
    static constexpr int PHASE_COUNT = 2;

    /// Everything the stage looks like follows from this one int.
    enum State : int {
        IDLE = 0,      ///< target editable, explanation (and last value) shown
        HEATING = 1,   ///< bringing the nozzle to target; the long phase
        MEASURING = 2, ///< the firmware command is running
        COMPLETE = 3,  ///< a number exists and is the whole stage
        ERROR = 4      ///< refused or failed; nothing changed
    };

    /// Nozzle bounds for the stepper. Deliberately the same span the brief
    /// states, not the heater's absolute maximum: this is a printing
    /// temperature, not a stress test.
    static constexpr int TEMP_MIN = 170;
    static constexpr int TEMP_MAX = 300;
    static constexpr int TEMP_STEP = 5;
    static constexpr int TEMP_DEFAULT = 245;

    /// Considered "at target" once within this many degrees. Klipper settles
    /// slowly and the firmware command does its own final wait.
    static constexpr float AT_TARGET_TOLERANCE_C = 2.0f;

    /// Rough cost of the measurement itself, for the remaining-time clock.
    /// Heating dominates; this only has to stop the clock reading 0:00 for the
    /// last half-minute.
    static constexpr int MEASURE_SECONDS_ESTIMATE = 40;

    PACalibrationPanel();
    ~PACalibrationPanel() override;

    void init_subjects() override;
    void deinit_subjects();

    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "PA Calibration";
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;

    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    State get_state() const {
        return state_;
    }

    /// The measured value, or 0 when no run has completed this session.
    float last_result() const {
        return result_k_;
    }

    /// Whether this printer can measure pressure advance at all. The Controls
    /// entry is bound to the same capability through `printer_has_pa_cal`.
    static bool printer_supports_calibration();

    //
    // === Test seams ===
    //

    /// Drive the state machine directly, without a printer.
    void set_state_for_test(State s) {
        set_state(s);
    }
    void on_result_for_test(float k) {
        on_result(k);
    }
    void on_error_for_test(const std::string& message) {
        on_error(message);
    }
    int target_temp_for_test() const {
        return target_temp_;
    }
    int selected_tool_for_test() const {
        return selected_tool_;
    }
    lv_timer_t* eta_timer_for_test() const {
        return eta_timer_;
    }

  private:
    // --- run control ---
    void confirm_and_start();
    void begin_run();
    void begin_measure();
    void stop_run(bool user_requested);
    /// Typical minutes for a run on this printer, for the pre-run copy.
    int estimate_minutes() const;
    void on_result(float k);
    void on_error(const std::string& message);
    void on_attempt(int attempt, int expected, float k_so_far);

    // --- state / display ---
    void set_state(State s);
    /// Repaint the tool chips. `adopt_active` seeds the selection from the
    /// tool currently on the carriage - true only when the screen opens. On a
    /// plain refresh the selection is the USER's, and must not be dragged back
    /// to the mounted tool: a toolchange takes ~20s to confirm (and on some
    /// backends never reports at all), so following it would snap the chip back
    /// under the finger and read as a dead button.
    void refresh_tools(bool adopt_active = false);
    void refresh_presets();
    void update_temp_display();
    void update_action_button();
    void update_phase_rows();
    void update_progress_display();
    void publish_last_result();

    /// Split a firmware refusal into a short title and the verbatim detail.
    /// The machine's own sentence is the most valuable text on the screen when
    /// it appears, so it is never truncated into a title.
    void show_error(const std::string& message);

    // --- heating ---
    void start_heat_tracking();
    void stop_heat_tracking();
    void on_nozzle_temp(int temp_deci);
    /// Cancel the ETA timer. Safe with none armed and safe from the
    /// destructor, which is why it is separate from stop_heat_tracking().
    void cancel_eta_timer();
    static void on_eta_tick(lv_timer_t* timer);

    /// Klipper name of the heater behind the selected tool.
    std::string selected_heater() const;

    /// Mount `tool` on the carriage, when the printer is a tool changer and
    /// that tool is not already the active one. A real toolchange: it moves the
    /// machine, so it is refused outright while a run owns it.
    void select_tool(int tool);

    // --- XML event trampolines ---
    static void on_action_clicked(lv_event_t* e);
    static void on_start_clicked(lv_event_t* e);
    static void on_reset_clicked(lv_event_t* e);
    static void on_tool_clicked(lv_event_t* e);
    static void on_preset_clicked(lv_event_t* e);
    static void on_temp_up(lv_event_t* e);
    static void on_temp_down(lv_event_t* e);

    IMoonrakerAPI* api_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    State state_ = IDLE;
    int selected_tool_ = 0;
    int target_temp_ = TEMP_DEFAULT;
    int selected_preset_ = -1;
    float result_k_ = 0.0f;
    bool have_result_ = false;
    /// Tool and temperature the stored result came from. A K with no material
    /// or temperature attached to it is nearly useless a week later.
    int result_tool_ = 0;
    int result_temp_ = 0;
    std::string result_material_;

    /// Stops listening to the run in progress. Klipper cannot interrupt a
    /// running command, so a stop mid-measurement leaves the firmware to
    /// finish while the screen stops waiting for it.
    std::function<void()> pa_cancel_;

    /// Ticks when the current phase began, for the remaining-time clock.
    uint32_t phase_start_tick_ms_ = 0;
    float heat_start_temp_ = 0.0f;
    int attempts_seen_ = 0;
    int attempts_expected_ = 0;

    // Subjects
    SubjectManager subjects_;

    lv_subject_t state_subject_;
    lv_subject_t multi_tool_;
    lv_subject_t tool_count_;
    lv_subject_t inputs_live_;
    lv_subject_t progress_;
    lv_subject_t result_plausible_;
    lv_subject_t action_is_stop_;
    lv_subject_t has_last_;

    lv_subject_t tool_selected_[MAX_TOOLS];
    lv_subject_t tool_sub_[MAX_TOOLS];
    char tool_sub_buf_[MAX_TOOLS][32] = {};

    lv_subject_t preset_selected_[PRESET_SLOTS];
    lv_subject_t preset_temp_[PRESET_SLOTS];
    char preset_temp_buf_[PRESET_SLOTS][16] = {};

    lv_subject_t phase_state_[PHASE_COUNT];
    lv_subject_t phase_name_[PHASE_COUNT];
    lv_subject_t phase_meta_[PHASE_COUNT];
    char phase_name_buf_[PHASE_COUNT][32] = {};
    char phase_meta_buf_[PHASE_COUNT][32] = {};

    lv_subject_t temp_display_;
    char temp_display_buf_[16] = {};
    lv_subject_t temp_note_;
    char temp_note_buf_[48] = {};

    lv_subject_t phase_label_;
    char phase_label_buf_[32] = {};
    lv_subject_t big_;
    char big_buf_[16] = {};
    lv_subject_t big_sub_;
    char big_sub_buf_[24] = {};
    lv_subject_t remaining_;
    char remaining_buf_[16] = {};
    lv_subject_t prog_foot_;
    char prog_foot_buf_[192] = {};

    lv_subject_t result_;
    char result_buf_[16] = {};
    lv_subject_t result_sanity_;
    char result_sanity_buf_[128] = {};
    lv_subject_t keep_note_;
    char keep_note_buf_[128] = {};

    lv_subject_t error_title_;
    char error_title_buf_[64] = {};
    lv_subject_t error_detail_;
    char error_detail_buf_[320] = {};
    lv_subject_t error_hint_;
    char error_hint_buf_[160] = {};

    lv_subject_t action_text_;
    char action_text_buf_[24] = {};

    lv_subject_t last_value_;
    char last_value_buf_[16] = {};
    lv_subject_t last_context_;
    char last_context_buf_[64] = {};

    /// Follows the printer's own active tool, so the chips show what is
    /// actually mounted rather than what was last tapped.
    ObserverGuard active_tool_observer_;

    // Heat-up tracking
    ObserverGuard temp_observer_;
    SubjectLifetime temp_lifetime_;
    lv_timer_t* eta_timer_ = nullptr;
};

/// Singleton accessor (lazily created, destroyed via StaticPanelRegistry)
PACalibrationPanel& get_global_pa_cal_panel();
} // namespace helix::ui
