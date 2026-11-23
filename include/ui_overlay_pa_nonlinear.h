// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "core/observer_guard.h"
#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "i_moonraker_api.h"
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
 * - pa_nl_pass_title / pa_nl_pass_hint / pa_nl_values (string)
 * - pa_nl_has_tower (int), pa_nl_busy (int: 1 readying a print, 2 writing the config)
 * - pa_nl_live_value / pa_nl_live_text (string)
 * - pa_nl_two_sided (int), pa_nl_height / pa_nl_height_value (string)
 * - pa_nl_error (string)
 */
namespace helix {
enum class PrintJobState;
}

namespace helix::ui {

class PaNonlinearOverlay : public OverlayBase {
  public:
    /// The print_stats filename the firmware's tower prints under.
    static constexpr const char* kTowerPrintName = "PA tower";

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
    void adjust_height_for_test(int step_mm) {
        adjust_height(step_mm);
    }
    void judge_for_test(int verdict) {
        judge(verdict);
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
    void finish_tower();
    void stop_tower();
    void raise(int attempts_left);
    static constexpr int kRaiseAttempts = 60;

    void adjust_height(int step_mm);
    /// One-sided: any value takes the height. Two-sided: a SideVerdict.
    void judge(int verdict);
    void after_judgement();
    /// Run the procedure's current values on the printer.
    void send_values();

    void save_to_config();
    void discard();
    void close_procedure();

    IMoonrakerAPI* api_ = nullptr;
    Stage stage_ = READY;
    std::optional<helix::pa::NonlinearProcedure> proc_;
    helix::pa::TestPrintParams params_;
    helix::pa::ExtruderKind kind_ = helix::pa::ExtruderKind::DirectDrive;
    /// What the extruder ran before the procedure, for Discard.
    helix::pa::PaModel original_;
    float height_mm_ = 25.0f;
    bool saw_printing_ = false;
    bool tower_added_ = false;
    bool raise_on_start_ = true;
    helix::system::KlipperConfigEditor config_editor_;

    SubjectManager subjects_;
    helix::Subject stage_subject_{};
    helix::Subject pass_title_{};
    char pass_title_buf_[96] = {};
    helix::Subject pass_hint_{};
    char pass_hint_buf_[256] = {};
    helix::Subject values_{};
    char values_buf_[192] = {};
    helix::Subject has_tower_{};
    helix::Subject busy_{};
    helix::Subject live_value_{};
    char live_value_buf_[24] = {};
    helix::Subject live_text_{};
    char live_text_buf_[96] = {};
    helix::Subject two_sided_{};
    helix::Subject height_{};
    char height_buf_[24] = {};
    helix::Subject height_value_{};
    char height_value_buf_[48] = {};
    helix::Subject error_{};
    char error_buf_[192] = {};

    ObserverGuard print_state_observer_;
    ObserverGuard z_observer_;
};

inline PaNonlinearOverlay& get_global_pa_nonlinear_overlay() {
    return lazy_global<PaNonlinearOverlay>("PaNonlinearOverlay");
}

} // namespace helix::ui
