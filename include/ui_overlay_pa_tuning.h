// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_head_picker.h"
#include "ui_modal.h"
#include "ui_observer_guard.h"

#include "i_moonraker_api.h"
#include "overlay_base.h"
#include "pa_tuning.h"
#include "spoolman_types.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "tool_state.h"

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
 *   pa_tune_can_save_spool (int)
 * - pa_tune_error (string)
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

  private:
    // --- setup ---
    void refresh_setup();
    void refresh_presets();
    void set_kind(helix::pa::ExtruderKind kind);
    void adjust_ramp_k(bool start, int direction);
    void confirm_and_start();
    /// Confirm-dialog options whose callbacks die with this object.
    ConfirmOptions owned_confirm();
    void begin_upload();
    /// Fail the start when the filament system refuses to heat the nozzle.
    bool refuse_blocked_nozzle();
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
    /// Whether sending K now is ours to do: always during our print, otherwise
    /// only while no job holds the machine.
    bool may_send() const;
    void send_k(float k);
    /// What the user calls the tuned extruder ("Tool 2", or "the extruder").
    std::string extruder_label() const;
    /// Record the result for other clients, naming the spool when there is one.
    void publish_result();
    void close_session();

    void set_stage(Stage s);

    IMoonrakerAPI* api_ = nullptr;
    Stage stage_ = SETUP;
    helix::pa::TuningSession session_;

    HeadPicker heads_;
    /// Tool the run selected, -1 for none; its spool is the one saved to.
    int tuned_tool_ = -1;
    int selected_preset_ = 0;
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
    /// Spoolman's active spool when the result was shown, if any.
    std::optional<SpoolInfo> active_spool_;
    bool raise_on_start_ = true;

    SubjectManager subjects_;

    lv_subject_t stage_subject_{};
    lv_subject_t preset_selected_[kPresetSlots]{};
    lv_subject_t preset_temp_[kPresetSlots]{};
    char preset_temp_buf_[kPresetSlots][24] = {};
    lv_subject_t kind_subject_{};
    lv_subject_t start_k_{};
    char start_k_buf_[16] = {};
    lv_subject_t end_k_{};
    char end_k_buf_[16] = {};
    lv_subject_t summary_{};
    char summary_buf_[384] = {};
    lv_subject_t can_start_{};

    lv_subject_t k_big_{};
    char k_big_buf_[16] = {};
    lv_subject_t layer_text_{};
    char layer_text_buf_[64] = {};
    lv_subject_t mode_text_{};
    char mode_text_buf_[96] = {};
    lv_subject_t manual_{};
    lv_subject_t step_idx_subject_{};

    lv_subject_t band_count_{};
    lv_subject_t band_range_[kMaxBands]{};
    char band_range_buf_[kMaxBands][40] = {};
    lv_subject_t band_k_[kMaxBands]{};
    char band_k_buf_[kMaxBands][16] = {};
    lv_subject_t band_selected_[kMaxBands]{};
    lv_subject_t keep_selected_{};
    lv_subject_t keep_text_{};
    char keep_text_buf_[48] = {};
    lv_subject_t has_pick_{};

    lv_subject_t result_k_{};
    char result_k_buf_[16] = {};
    lv_subject_t result_note_{};
    char result_note_buf_[192] = {};
    lv_subject_t spoolman_text_{};
    char spoolman_text_buf_[160] = {};
    lv_subject_t can_save_spool_{};

    lv_subject_t error_{};
    char error_buf_[192] = {};

    ObserverGuard print_state_observer_;
    ObserverGuard layer_observer_;
    ObserverGuard pa_observer_;
    ObserverGuard active_tool_observer_;
};

inline PaTuningOverlay& get_global_pa_tuning_overlay() {
    return lazy_global<PaTuningOverlay>("PaTuningOverlay");
}

} // namespace helix::ui
