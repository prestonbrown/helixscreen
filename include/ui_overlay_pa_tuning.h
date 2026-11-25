// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_head_picker.h"
#include "ui_modal.h"

#include "core/observer_guard.h"
#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "display_awake_hold.h"
#include "i_moonraker_api.h"
#include "klipper_config_editor.h"
#include "overlay_base.h"
#include "pa_tuning.h"
#include "spoolman_types.h"
#include "subject_managed_panel.h"
#include "tool_state.h"

#include <functional>
#include <lvgl.h>
#include <map>
#include <optional>
#include <string>

/**
 * @file ui_overlay_pa_tuning.h
 * @brief Pressure advance tuned by eye, during a live test print
 *
 * HelixScreen generates a small hollow tower, uploads it and prints it, and
 * steps K with height itself (helix::pa::TuningSession). The full-screen
 * tuner shows the K in effect large, the layer and height, and big -/+
 * buttons: a nudge takes over from the ramp for the rest of the print, so the
 * user can hold a value steady and judge it. When the print ends the user
 * picks the height band that looked best from the log of what the printer
 * REPORTED, or keeps the current value.
 *
 * The run outlives the screen: the session and its printer observers belong to
 * this object, not to the widget tree, so backing out to Print Status leaves
 * the ramp running. The Tune overlay offers the way back.
 *
 *   SETUP --Start--> STARTING --printing--> RUNNING --print ends--> PICK
 *     ^                  |                                            |
 *     '----(failed)------'                         pick / keep --> RESULT
 *
 * ## Subject bindings
 *
 * - pa_tune_stage (int) - Stage below
 * - pa_tune_multi_tool / pa_tune_tool_count / pa_tune_tool_selected_N
 * - pa_tune_preset_selected_N, pa_tune_preset_temp_N (string)
 * - pa_tune_kind (int) - ExtruderKind
 * - pa_tune_start_k / pa_tune_end_k / pa_tune_summary (string)
 * - pa_tune_can_start (int)
 * - pa_tune_k_big / pa_tune_layer_text / pa_tune_mode_text (string)
 * - pa_tune_manual (int), pa_tune_step_idx (int)
 * - pa_tune_band_count (int), pa_tune_band_range_N / pa_tune_band_k_N (string),
 *   pa_tune_band_selected_N (int), pa_tune_keep_selected (int),
 *   pa_tune_keep_text (string), pa_tune_has_pick (int)
 * - pa_tune_result_k / pa_tune_result_note / pa_tune_spoolman_text (string),
 *   pa_tune_can_save_spool (int), pa_tune_can_save_config (int)
 * - pa_tune_error (string)
 * - pa_tune_time_line / pa_tune_time_sub / pa_tune_primary_text (string): the setup footer
 * - pa_tune_material_temps, pa_tune_nl_pass_N / pa_tune_nl_look_N (string): the setup;
 *   pa_tune_nl_look_short_N, the look in a few words for the smallest screens
 * - pa_tune_next_text / pa_tune_hint (string): the running dial
 * - pa_tune_look, pa_tune_ref_src_N, pa_tune_pick_band / pa_tune_pick_range /
 *   pa_tune_pick_k / pa_tune_before_k / pa_tune_apply_text (string): the pick;
 *   pa_tune_band_picked (int), 1 while a band is picked rather than a held value
 * - pa_tune_save_material (string): the result
 */
namespace helix {
enum class PrintJobState;
}

namespace helix::ui {

class PaTuningOverlay : public OverlayBase {
  public:
    static constexpr int kPresetSlots = 4;
    /// Band rows in the picker. The default ramp yields 11 (base + 10 bands);
    /// a nudge-heavy run can yield more, and the closest neighbours are merged
    /// (helix::pa::fit_bands).
    static constexpr int kMaxBands = 16;

    /// Generated file name in the gcodes root. One fixed name: a re-run
    /// overwrites the last test instead of littering the file list.
    static constexpr const char* TEST_FILENAME = "helix_pa_tuning.gcode";

    enum Stage : int {
        SETUP = 0,
        STARTING = 1,
        RUNNING = 2,
        PICK = 3,
        RESULT = 4,
    };

    PaTuningOverlay();
    ~PaTuningOverlay() override;

    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;

    const char* get_name() const override {
        return "PA Tuning";
    }
    const char* xml_component() const override {
        return "pa_tuning_overlay";
    }
    /// Kept: a running test re-pushes the tuner over Print Status, and the
    /// run's widgets must survive the pop that takes it out of the stack.
    bool destroy_on_close() const override {
        return false;
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;

    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /// Whether a tuning print is running or awaiting its pick. The Tune
    /// overlay shows its way back here while this is true.
    bool session_active() const;

    Stage stage() const {
        return stage_;
    }

    /// Which extruder and tool a head choice means. Pure, for tests.
    struct HeadChoice {
        int tool;
        std::string extruder;
    };
    /// The pre-print routing for the tower: its tool on its own head.
    static std::map<int, int> tower_routing(int tuned_tool);
    /// What Save to Config hands the config editor for @p extruder: the K as its
    /// pressure_advance option, and the setting Klipper must report after the restart.
    struct ConfigWrite {
        std::vector<helix::system::ConfigEdit> edits;
        std::vector<helix::system::ExpectedSetting> expected;
    };
    static ConfigWrite config_write_for(const std::string& extruder, float k);
    /// Why the tower cannot start on @p params' head, or empty when it can. A
    /// tower aimed at a tool needs the command that mounts that tool's head;
    /// without it the tower would heat one head and print through whichever
    /// is on the carriage.
    static std::string head_mount_refusal(const helix::pa::TestPrintParams& params);

    static HeadChoice choose_head(const std::vector<PhysicalHead>& heads, bool multi_extruder,
                                  int selected, int active_tool,
                                  const std::string& active_extruder);

    //
    // === Test seams ===
    //
    helix::pa::TuningSession& session_for_test() {
        return session_;
    }
    /// Begin observing the printer as if the upload and start had succeeded.
    void begin_observing_for_test() {
        begin_observing();
    }
    void nudge_for_test(int direction) {
        nudge(direction);
    }
    void select_band_for_test(int index) {
        select_band(index);
    }
    void apply_pick_for_test() {
        apply_pick();
    }
    void save_to_spool_for_test() {
        save_to_spool();
    }
    /// Where the tower's Print Status page comes from, for tests with no panel of
    /// their own to stand in for it.
    void set_print_status_page_source_for_test(std::function<lv_obj_t*()> source) {
        print_status_page_source_ = std::move(source);
    }
    bool awake_held_for_test() const {
        return decision_awake_.held();
    }
    void save_to_config_for_test() {
        save_to_config();
    }
    /// A tap on the pick screen's tower at @p height_mm.
    void tower_tap_for_test(float height_mm) {
        tower_tap(height_mm);
    }
    int picked_for_test() const {
        return picked_;
    }
    /// Nudge K by @p delta when a run is printing; false (nothing sent)
    /// otherwise. The Tune overlay's PA row goes through here during a run.
    bool nudge_by(float delta);

    void set_extruder_for_test(const std::string& extruder) {
        extruder_ = extruder;
    }
    /// Tests drive the session without a navigation stack to raise it on.
    void confirm_and_start_for_test() {
        confirm_and_start();
    }
    [[nodiscard]] std::string error_for_test() {
        return lv_subject_get_string(&error_);
    }
    void set_raise_on_start_for_test(bool raise) {
        raise_on_start_ = raise;
    }
    /// What the setup screen shows, as on_activate() refreshes it.
    void refresh_setup_for_test() {
        refresh_setup();
    }
    /// Ask the printer for the acceleration the towers print at.
    void query_max_accel_for_test() {
        query_max_accel();
    }
    /// The Direct drive / Bowden chips.
    void set_kind_for_test(helix::pa::ExtruderKind kind) {
        set_kind(kind);
    }
    /// The Linear K / Nonlinear chips.
    void choose_model_for_test(bool nonlinear) {
        nl_mode_choice_ = nonlinear;
        refresh_model_choice();
    }

  private:
    /// Pick the band printed at @p height_mm.
    void tower_tap(float height_mm);
    /// Push the stage's tower drawings (pa_tower) their models.
    void refresh_towers();
    /// The setup's button: the linear tower, or the nonlinear tuner.
    void setup_primary();
    /// The pick's band and value rows.
    void refresh_pick_values();

    // --- setup ---
    void refresh_setup();
    void refresh_presets();
    void set_kind(helix::pa::ExtruderKind kind);
    void adjust_ramp_k(bool start, int direction);
    void confirm_and_start();
    /// Hand the chosen head, temperatures and extruder kind to the guided
    /// nonlinear tuner.
    void start_nonlinear();
    /// Whether the chosen head's firmware offers a nonlinear model, and the
    /// Linear K / Nonlinear choice that follows from it.
    void refresh_model_choice();
    /// Learn once whether the start macro draws its own purge line, so the tower
    /// does not add a second one.
    void analyze_start_macro();
    /// The toolhead's max_accel, which the firmware tower prints at, into the
    /// nonlinear setup's acceleration note.
    void query_max_accel();
    void refresh_accel_note();
    /// Confirm-dialog options whose callbacks die with this object.
    ConfirmOptions owned_confirm();
    void begin_upload();
    /// Fail the start when the filament system refuses to heat the nozzle.
    bool refuse_blocked_nozzle();
    /// Fail the start on a nonlinear model: a bare ADVANCE= would replace only part
    /// of the tuned model. Steers the setup to the Nonlinear tuner.
    bool refuse_nonlinear_model(const helix::pa::TestPrintParams& params);
    /// Fail the start when the chosen head has no command that mounts it.
    bool refuse_unmountable_head(const helix::pa::TestPrintParams& params);
    void fail_start(const std::string& message);
    helix::pa::TestPrintParams build_params() const;
    /// The printer's own start and end macros, from the discovered macro set.
    static helix::pa::MachineMacros machine_macros();
    /// The extruder heater and the tool to select for the chosen head.
    using ChosenHead = HeadChoice;
    ChosenHead chosen_head() const;
    /// The bed area the tower may use, from the printer's axes, plate and
    /// kinematics. nullopt when the axes are unknown.
    static std::optional<helix::pa::PrintArea> printable_area();
    void start_uploaded_print();
    /// Remove the generated file from the printer, so it cannot be reprinted.
    void delete_tower_file();

    // --- run ---
    void begin_observing();
    void stop_observing();
    void on_print_state(PrintJobState state);
    void on_layer(int layer);
    /// Put the tuner on top of the navigation stack, re-pushing it when
    /// something buried it. Retries while the stack is still releasing it.
    void raise(int attempts_left);
    static constexpr int kRaiseAttempts = 60;
    void on_pa_reported(int subject_value);
    /// Put back the K the run started from when its print ended without the
    /// tower's own restore (a cancel), so a test K never stays live unaccepted.
    void restore_original_k();
    /// Take the tower's Print Status page off the stack once the print is over.
    void leave_print_status();
    void nudge(int direction);
    void end_test();
    void refresh_running();

    // --- pick / result ---
    /// Fill the band rows; the running log reads newest first, the picker lowest first.
    void refresh_bands(bool newest_first = false);
    void select_band(int index);
    void apply_pick();
    void show_result();
    void save_to_spool();
    /// Ask, then write the result as pressure_advance in the extruder's config section.
    void save_to_config();
    void write_config();
    /// Show @p spool_id as the spool Save to Spool writes to.
    void fetch_result_spool(int spool_id);
    /// Whether sending K now is ours to do: always during our print, otherwise
    /// only while no job holds the machine.
    bool may_send() const;
    void send_k(float k);
    /// What the user calls the tuned extruder ("Tool 2", or "the extruder").
    std::string extruder_label() const;
    void publish_to_moonraker(float k);
    void close_session();

    void set_stage(Stage s);

    IMoonrakerAPI* api_ = nullptr;
    Stage stage_ = SETUP;
    helix::pa::TuningSession session_;

    HeadPicker heads_;
    /// Tool the run selected, -1 for none; its spool is the one saved to.
    int tuned_tool_ = -1;
    int selected_preset_ = 0;
    /// The user tapped a material chip; until the tuner closes, the loaded
    /// spool no longer picks one.
    bool preset_picked_ = false;
    int nozzle_temp_ = 210;
    int bed_temp_ = 60;
    helix::pa::ExtruderKind kind_ = helix::pa::ExtruderKind::DirectDrive;
    helix::pa::Ramp ramp_ = helix::pa::default_ramp(helix::pa::ExtruderKind::DirectDrive);
    int step_idx_ = helix::pa::kDefaultStepIndex;
    /// Extruder the run tunes, fixed when it starts.
    std::string extruder_ = "extruder";
    /// Picker selection: a band index, kMaxBands for "keep current", -1 none.
    int picked_ = -1;
    /// The print reached "printing" for our file at least once.
    bool saw_printing_ = false;
    /// The layer the printer showed when this run began observing; a leftover from
    /// the previous job.
    int layer_at_observe_ = 0;
    /// Spoolman's active spool when the result was shown, if any.
    std::optional<SpoolInfo> active_spool_;
    bool raise_on_start_ = true;
    std::function<lv_obj_t*()> print_status_page_source_;
    bool start_analysis_in_flight_ = false;
    bool start_macro_purges_ = false;
    /// The toolhead's max_accel, mm/s²; 0 until the printer has said.
    double max_accel_ = 0.0;

    SubjectManager subjects_;

    helix::Subject stage_subject_{};
    helix::Subject preset_selected_[kPresetSlots]{};
    helix::Subject preset_temp_[kPresetSlots]{};
    char preset_temp_buf_[kPresetSlots][24] = {};
    helix::Subject kind_subject_{};
    helix::Subject start_k_{};
    char start_k_buf_[16] = {};
    helix::Subject end_k_{};
    char end_k_buf_[16] = {};
    helix::Subject summary_{};
    char summary_buf_[640] = {};
    helix::Subject nl_accel_{};
    char nl_accel_buf_[192] = {};
    helix::Subject can_start_{};
    helix::Subject nl_available_{};
    helix::Subject nl_mode_{};
    /// The user's Linear K / Nonlinear choice; nullopt follows the firmware's
    /// current model.
    std::optional<bool> nl_mode_choice_;

    helix::Subject k_big_{};
    char k_big_buf_[16] = {};
    helix::Subject layer_text_{};
    char layer_text_buf_[64] = {};
    helix::Subject mode_text_{};
    char mode_text_buf_[96] = {};
    helix::Subject manual_{};
    helix::Subject step_idx_subject_{};

    helix::Subject band_count_{};
    helix::Subject band_range_[kMaxBands]{};
    char band_range_buf_[kMaxBands][40] = {};
    helix::Subject band_k_[kMaxBands]{};
    char band_k_buf_[kMaxBands][16] = {};
    helix::Subject band_selected_[kMaxBands]{};
    helix::Subject keep_selected_{};
    helix::Subject keep_text_{};
    char keep_text_buf_[48] = {};
    helix::Subject has_pick_{};

    helix::Subject result_k_{};
    char result_k_buf_[16] = {};
    helix::Subject result_note_{};
    char result_note_buf_[192] = {};
    helix::Subject spoolman_text_{};
    char spoolman_text_buf_[160] = {};
    helix::Subject can_save_spool_{};
    helix::Subject can_save_config_{};

    helix::Subject error_{};
    char error_buf_[192] = {};

    /// Direct drive prints three towers, Bowden four.
    static constexpr int kNlPassCards = 4;
    helix::Subject nl_pass_count_{};
    /// 1 while Linear K is chosen on an extruder that runs a nonlinear model: the
    /// start refuses, so the button is disabled and the reason shows.
    helix::Subject lin_blocked_{};
    helix::Subject time_line_{};
    char time_line_buf_[64] = {};
    helix::Subject time_sub_{};
    char time_sub_buf_[96] = {};
    helix::Subject primary_text_{};
    char primary_text_buf_[48] = {};
    helix::Subject material_temps_{};
    char material_temps_buf_[32] = {};
    helix::Subject nl_pass_[kNlPassCards]{};
    char nl_pass_buf_[kNlPassCards][48] = {};
    helix::Subject nl_look_[kNlPassCards]{};
    char nl_look_buf_[kNlPassCards][64] = {};
    helix::Subject nl_look_short_[kNlPassCards]{};
    char nl_look_short_buf_[kNlPassCards][64] = {};
    helix::Subject nl_thumb_[kNlPassCards]{};
    helix::Subject next_text_{};
    char next_text_buf_[64] = {};
    helix::Subject hint_{};
    char hint_buf_[96] = {};
    helix::Subject look_{};
    char look_buf_[96] = {};
    /// Image paths, as pointer subjects the samples' bind_src reads.
    helix::Subject ref_src_[3]{};
    helix::Subject band_picked_{};
    helix::Subject pick_band_{};
    char pick_band_buf_[32] = {};
    helix::Subject pick_range_{};
    char pick_range_buf_[40] = {};
    helix::Subject pick_k_{};
    char pick_k_buf_[16] = {};
    helix::Subject before_k_{};
    char before_k_buf_[16] = {};
    helix::Subject apply_text_{};
    char apply_text_buf_[32] = {};
    helix::Subject save_material_{};
    char save_material_buf_[48] = {};

    helix::system::KlipperConfigEditor config_editor_;

    /// Held while the pick or the result waits on the user, so the screensaver
    /// never covers the decision.
    helix::DisplayAwakeToken decision_awake_;

    ObserverGuard print_state_observer_;
    ObserverGuard layer_observer_;
    ObserverGuard pa_observer_;
    ObserverGuard active_tool_observer_;
};

inline PaTuningOverlay& get_global_pa_tuning_overlay() {
    return lazy_global<PaTuningOverlay>("PaTuningOverlay");
}

} // namespace helix::ui
