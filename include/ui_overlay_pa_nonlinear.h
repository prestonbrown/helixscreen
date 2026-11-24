// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_emergency_stop.h"
#include "ui_modal.h"

#include "core/observer_guard.h"
#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "display_awake_hold.h"
#include "i_moonraker_api.h"
#include "json_fwd.h"
#include "klipper_config_editor.h"
#include "overlay_base.h"
#include "pa_model.h"
#include "pa_nonlinear_tuning.h"
#include "pa_tuning.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <optional>
#include <string>

/**
 * @file ui_overlay_pa_nonlinear.h
 * @brief Guided tuning of a nonlinear pressure-advance model on the firmware's
 *        own test tower
 *
 * Opened from the pressure advance tuner's setup, which has already chosen
 * the head, the material temperatures and the extruder kind
 * (pa::TestPrintParams). Each pass readies the machine the way the classic
 * tower does (pa::machine_ready_lines), switches the extruder to the nonlinear
 * model with the procedure's values, arms TUNING_TOWER for the swept parameter
 * and starts PRINT_PA_TOWER. When the tower ends the user says at which
 * height the judged side looked best (and, on a two-sided pass, whether the
 * left looked best lower, level or higher than the front);
 * pa::NonlinearProcedure decides what prints next. The result is kept for the
 * session or written to the extruder's config section, with a restart.
 *
 * Nothing is written to printer.cfg until Save; the runtime model switch is
 * undone by Discard. A printer without the tower module is offered the one
 * config section it needs first, since that write restarts Klipper.
 *
 *   READY --Print--> PREPARING --tower prints--> PRINTING --ends--> JUDGE
 *     ^                                                            |
 *     '---------------- next pass -----------------'   last pass --> RESULT
 *
 * ## Subject bindings
 *
 * - pa_nl_stage (int) - Stage below
 * - pa_nl_pass_title / pa_nl_pass_hint (string)
 * - pa_nl_has_tower (int), pa_nl_busy (int: 1 readying a print, 2 writing the config)
 * - pa_nl_live_value / pa_nl_live_text (string)
 * - pa_nl_layer_text / pa_nl_progress_text / pa_nl_time_left (string), pa_nl_progress (int 0-100)
 * - pa_nl_two_sided (int), pa_nl_height / pa_nl_height_value (string)
 * - pa_nl_can_back (int: a judgement can be taken back)
 * - pa_nl_error (string)
 * - pa_nl_pass_count / pa_nl_pass_index (int), pa_nl_pass_label_N (string): the pass bar
 * - pa_nl_value_N (string, N: 0 offset, 1 advance, 2 time offset, 3 velocity),
 *   pa_nl_value_hl (int: the row the pass sweeps), pa_nl_result_N (string)
 * - pa_nl_look (string), pa_nl_ref_src_N (string): the "where to look" card
 * - pa_nl_time_line / pa_nl_time_sub / pa_nl_run_note / pa_nl_run_chip (string)
 * - pa_nl_ready_primary / pa_nl_judge_primary / pa_nl_back_text (string)
 * - pa_nl_param / pa_nl_height_front / pa_nl_verdict (string): pass 2's two heights,
 *   pa_nl_heights_set (int: 0 while a two-sided pass has a height not yet judged)
 * - pa_nl_result_live / pa_nl_save_material (string)
 *
 * Every judgement is saved with the printer's settings, so a restart of the
 * screen offers to continue the procedure where it stopped.
 */
namespace helix {
enum class PrintJobState;
}

namespace helix::ui {

/// The name of a nonlinear model value, as the tuners show it.
const char* nonlinear_param_label(helix::pa::NlParam p);

class PaNonlinearOverlay : public OverlayBase {
  public:
    /// The print_stats filename the firmware's tower prints under.
    static constexpr const char* kTowerPrintName = "PA tower";
    /// How long after PRINT_PA_TOWER returns the tower must be printing.
    static constexpr uint32_t kTowerStartTimeoutMs = 30000;
    /// How long a start the printer never answered may still turn into a tower.
    static constexpr uint32_t kLateTowerStartTimeoutMs = 30 * 60 * 1000;

    enum Stage : int {
        READY = 0,
        PREPARING = 1,
        PRINTING = 2,
        JUDGE = 3,
        RESULT = 4,
    };

    PaNonlinearOverlay();
    ~PaNonlinearOverlay() override;

    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;

    const char* get_name() const override {
        return "PA Nonlinear";
    }
    const char* xml_component() const override {
        return "pa_nonlinear_overlay";
    }
    /// Kept: a tower print re-pushes this over Print Status, and the
    /// procedure outlives the widget tree between passes.
    bool destroy_on_close() const override {
        return false;
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;

    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /// Start, or resume, the procedure for @p params' extruder and show it.
    /// A procedure already under way on the same extruder resumes.
    void begin(const helix::pa::TestPrintParams& params, helix::pa::ExtruderKind kind);

    /// A procedure is under way: started and neither saved nor discarded.
    bool session_active() const {
        return proc_.has_value();
    }

    Stage stage() const {
        return stage_;
    }

    //
    // === Test seams ===
    //
    /// begin() without showing the screen.
    void start_for_test(const helix::pa::TestPrintParams& params, helix::pa::ExtruderKind kind) {
        start(params, kind);
    }
    void print_pass_for_test() {
        print_pass();
    }
    void adjust_height_for_test(int step_tenths) {
        adjust_height(step_tenths);
    }
    void edit_height_for_test() {
        edit_height();
    }
    void back_for_test() {
        go_back();
    }
    float judge_top_for_test() const {
        return judge_top_mm_;
    }
    float height_for_test() const {
        return height_mm_;
    }
    /// What a restart of the screen loses: the procedure in memory.
    void forget_for_test() {
        proc_.reset();
        set_stage(READY);
    }
    void stop_for_test() {
        stop_tower();
    }
    bool keeps_display_awake_for_test() const {
        return decision_awake_.held();
    }
    bool holds_machine_busy_for_test() const {
        return start_busy_.held();
    }
    void judge_for_test(int verdict) {
        judge(verdict);
    }
    /// Pass 2's front height, and the judge button the screen shows.
    void adjust_front_height_for_test(int step_tenths) {
        adjust_front_height(step_tenths);
    }
    float front_height_for_test() const {
        return front_mm_;
    }
    void judge_primary_for_test() {
        judge_primary();
    }
    void tower_tap_for_test(int face, float height_mm) {
        tower_tap(face, height_mm);
    }
    void save_for_test() {
        save_to_config();
    }
    void discard_for_test() {
        discard();
    }
    const helix::pa::NonlinearProcedure* procedure_for_test() const {
        return proc_ ? &*proc_ : nullptr;
    }
    void set_raise_on_start_for_test(bool raise) {
        raise_on_start_ = raise;
    }

  private:
    void start(const helix::pa::TestPrintParams& params, helix::pa::ExtruderKind kind);
    void refresh();
    void refresh_live();
    void set_stage(Stage s);
    void fail(const std::string& message);
    /// PREPARING while printer.cfg is written and Klipper restarts.
    void show_config_write();
    ConfirmOptions owned_confirm();

    bool tower_available() const;
    void add_tower_section();
    void confirm_print();
    void print_pass();
    void begin_observing();
    void stop_observing();
    void on_print_state(PrintJobState state);
    /// The tower's job ended in @p state: only a completed one runs the end macro.
    void finish_tower(PrintJobState state);
    /// Disarm the sweep, put the procedure's values back, and with
    /// @p release_machine run the end lines for a job that ended as @p end.
    void send_ending(bool release_machine, helix::pa::JobEnd end = helix::pa::JobEnd::Completed);
    /// PRINT_PA_TOWER returned for pass @p pass_seq.
    void on_script_done(int pass_seq);
    /// The ready script failed. Only a refusal ends the pass: a timeout or a
    /// lost connection leaves the script running on the printer.
    void on_script_failed(int pass_seq, const MoonrakerError& err);
    /// Fail pass @p pass_seq if no tower is printing after @p delay_ms; with
    /// @p disarm_on_expiry, also end the sweep (unsafe while a script may still
    /// be running ahead of it).
    void arm_start_watchdog(int pass_seq, uint32_t delay_ms, bool disarm_on_expiry);
    /// Put the procedure's extruder back on the model it ran before.
    void restore_original();

    void stop_tower();
    void raise(int attempts_left);
    static constexpr int kRaiseAttempts = 60;

    /// Move the judged height by @p step_tenths of a millimeter.
    void adjust_height(int step_tenths);
    /// Type the judged height on the numeric keypad.
    void edit_height();
    void set_height(float height_mm);
    /// Take back the latest judgement and judge its tower again.
    void go_back();
    /// One-sided: any value takes the height. Two-sided: a SideVerdict.
    void judge(int verdict);
    /// The judge screen's button: the height, or pass 2's two heights compared.
    void judge_primary();
    /// Pass 2's front height.
    void adjust_front_height(int step_tenths);
    void edit_front_height();
    void set_front_height(float height_mm);
    /// A tap on the judged tower: @p face is the face it landed on.
    void tower_tap(int face, float height_mm);
    /// READY's button: print the pass, or first add the tower to the config.
    void ready_primary();
    /// Push the stage's tower drawings (pa_tower) their models.
    void refresh_towers();
    void after_judgement();
    /// Run the procedure's current values on the printer.
    void send_values();

    void save_to_config();
    void discard();
    void close_procedure();

    /// Write the procedure, and the model to restore, to the printer's settings.
    void save_session();
    void clear_saved_session();
    /// Offer to continue the saved procedure @p saved in place of the fresh one.
    void offer_resume(const nlohmann::json& saved);
    void resume(const nlohmann::json& saved);

    IMoonrakerAPI* api_ = nullptr;
    Stage stage_ = READY;
    std::optional<helix::pa::NonlinearProcedure> proc_;
    helix::pa::TestPrintParams params_;
    helix::pa::ExtruderKind kind_ = helix::pa::ExtruderKind::DirectDrive;
    /// What the extruder ran before the procedure, for Discard.
    helix::pa::PaModel original_;
    float height_mm_ = 25.0f;
    /// Pass 2: where the front looked best (height_mm_ holds the left's).
    float front_mm_ = 25.0f;
    /// Each side's height has been set by a tap, a step or the keypad. A
    /// two-sided pass opens with neither, so it can't be accepted unjudged.
    bool left_set_ = true;
    bool front_set_ = true;
    bool saw_printing_ = false;
    /// The current pass's script returned: PRINT_PA_TOWER was accepted.
    bool script_done_ = false;
    /// Increments per pass, so a late callback from an earlier pass is ignored.
    int pass_seq_ = 0;
    bool tower_added_ = false;
    bool raise_on_start_ = true;
    helix::system::KlipperConfigEditor config_editor_;

    SubjectManager subjects_;
    helix::Subject stage_subject_{};
    helix::Subject pass_title_{};
    char pass_title_buf_[96] = {};
    helix::Subject pass_hint_{};
    char pass_hint_buf_[384] = {};
    helix::Subject has_tower_{};
    helix::Subject busy_{};
    helix::Subject live_value_{};
    char live_value_buf_[24] = {};
    helix::Subject live_text_{};
    char live_text_buf_[96] = {};
    // The tower's own progress, from the nozzle height: the firmware tower
    // reports no layers, and its file position runs slow over the brim.
    helix::Subject layer_text_{};
    char layer_text_buf_[64] = {};
    helix::Subject progress_{};
    helix::Subject progress_text_{};
    char progress_text_buf_[16] = {};
    helix::Subject time_left_{};
    char time_left_buf_[32] = {};
    std::optional<helix::pa::TowerTimeLeft> time_left_est_;
    /// The highest the nozzle reached this pass, and the top of what is
    /// judged: the whole tower, or what printed before a stop.
    float reached_mm_ = 0.0f;
    float judge_top_mm_ = helix::pa::kTowerHeightMm;
    helix::Subject two_sided_{};
    helix::Subject can_back_{};
    helix::Subject height_{};
    char height_buf_[24] = {};
    helix::Subject height_value_{};
    char height_value_buf_[48] = {};
    helix::Subject error_{};
    char error_buf_[192] = {};

    static constexpr int kPassSlots = 4;
    static constexpr int kValueRows = 4;
    helix::Subject pass_count_{};
    helix::Subject pass_index_{};
    helix::Subject pass_label_[kPassSlots]{};
    char pass_label_buf_[kPassSlots][64] = {};
    helix::Subject value_[kValueRows]{};
    char value_buf_[kValueRows][32] = {};
    helix::Subject result_[kValueRows]{};
    char result_buf_[kValueRows][40] = {};
    helix::Subject was_[kValueRows]{};
    char was_buf_[kValueRows][32] = {};
    helix::Subject value_hl_{};
    helix::Subject look_{};
    char look_buf_[192] = {};
    /// Image paths, as pointer subjects the samples' bind_src reads.
    helix::Subject ref_src_[3]{};
    helix::Subject time_line_{};
    char time_line_buf_[64] = {};
    helix::Subject time_sub_{};
    char time_sub_buf_[64] = {};
    helix::Subject run_note_{};
    char run_note_buf_[96] = {};
    helix::Subject run_chip_{};
    char run_chip_buf_[48] = {};
    helix::Subject ready_primary_{};
    char ready_primary_buf_[48] = {};
    helix::Subject judge_primary_{};
    char judge_primary_buf_[48] = {};
    helix::Subject back_text_{};
    char back_text_buf_[32] = {};
    helix::Subject param_{};
    char param_buf_[48] = {};
    helix::Subject height_front_{};
    char height_front_buf_[24] = {};
    helix::Subject heights_set_{};
    helix::Subject verdict_{};
    char verdict_buf_[160] = {};
    helix::Subject result_live_{};
    char result_live_buf_[96] = {};
    helix::Subject save_material_{};
    char save_material_buf_[48] = {};

    /// Held while a judgement waits on the user, so the screensaver does not
    /// cover it.
    helix::DisplayAwakeToken decision_awake_;
    /// Keeps the E-stop up while the start script homes and heats: no print
    /// job exists until the firmware tower begins.
    helix::MachineBusyToken start_busy_;

    ObserverGuard print_state_observer_;
    ObserverGuard z_observer_;
    ObserverGuard duration_observer_;
    ObserverGuard pa_observer_;
};

inline PaNonlinearOverlay& get_global_pa_nonlinear_overlay() {
    return lazy_global<PaNonlinearOverlay>("PaNonlinearOverlay");
}

} // namespace helix::ui
