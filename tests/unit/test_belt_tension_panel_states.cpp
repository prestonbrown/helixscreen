// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_tension_panel_states.cpp
 * @brief The belt tension panel's four view states, its run queue and its gate.
 *
 * Drives the real singleton panel through MoonrakerClientMock's TEST_RESONANCES
 * transcript and asserts on the bt_* subjects the XML binds:
 *
 * 1. Start queues both paths, runs them in order and lands on RESULTS with
 *    the similarity headline, verdict, paired-peak facts and chart markers
 *    derived from compare_belt_paths() over the printer's sweep range.
 * 2. Re-test keeps the sibling path's run and ghosts the re-measured path's
 *    previous curve.
 * 3. Every mock failure mode and a silent sweep reach ERROR; a path whose
 *    sweep never rises above the peak floor is an error naming the path.
 * 4. Deactivating mid-run cancels listening and returns to START.
 * 5. Start is disabled while klippy is not READY, and re-arms.
 * 6. EMBEDDED tier never creates a chart.
 * 7. Tune walks the guided tuning loop: the already-matched offer, the
 *    calibration turn's full sweeps, the narrow baseline pair and each
 *    round's narrow sweeps, with advice and outcome lines from
 *    BeltTuneSession, then Confirm or Good Enough.
 *
 * The XML side (containers, buttons, subject bindings) is pinned in the
 * container/binding cases below.
 */

#include "ui_belt_gap_gauge.h"
#include "ui_belt_path_names.h"
#include "ui_belt_path_sketch.h"
#include "ui_context_menu.h"
#include "ui_fonts.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_panel_belt_tension.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/mock_kinematics_env.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/sweep_preview_fakes.h"
#include "app_globals.h"
#include "belt_tension_types.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "mock_persona.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "translation_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

/// Registers the bt_* subjects the panel binds, seeds the global PrinterState
/// the gate observes, and wires the panel to a mock client. One instance per
/// TEST_CASE; the destructor unwires in the reverse order.
class BeltPanelFixture : public XMLTestFixture {
  public:
    explicit BeltPanelFixture(helix::PlatformTier tier = helix::PlatformTier::STANDARD)
        : client_(MoonrakerClientMock::PrinterType::VORON_24), api_(client_, api_state_) {
        // A previous test's mock run in THIS process may have left the
        // per-PID belt CSVs behind; the terminal-line path must start absent.
        MoonrakerClientMock::remove_belt_csvs();
        client_.set_belt_line_interval_ms(1);

        api_state_.init_subjects(false);
        api_state_.set_klippy_state_sync(KlippyState::READY);

        // The panel observes the GLOBAL PrinterState, not this fixture's own;
        // seed it the way a live connection would.
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        // Seed the INPUTS a live connection produces, not the derived subject:
        // nav_buttons_enabled is recomputed from connection state + klippy on
        // every klippy transition, so poking it directly would silently close
        // the gate again the moment klippy state changed.
        get_printer_state().set_printer_connection_state(
            static_cast<int>(ConnectionState::CONNECTED), nullptr);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 0);
        lv_subject_copy_string(get_printer_state().motion_state().get_homed_axes_subject(), "xyz");
        // The settling laps plan from the toolhead's limits and max velocity.
        set_toolhead_limits(500.0);
        set_accel_subject(1);

        // Names are per printer; every case starts from "not set".
        helix::SettingsManager::instance().init_subjects();
        helix::SettingsManager::instance().set_belt_layout(0);

        panel_ = &get_global_belt_tension_panel();
        ui_panel_belt_tension_register_callbacks();
        panel_->deinit_subjects();
        panel_->init_subjects();
        panel_->set_api(&client_, &api_);
        panel_->set_render_tier_for_test(tier, true);
        // The panel is a process-wide singleton, so a memory reading one case
        // injected would otherwise make the next case's Start ask about low RAM.
        helix::MemoryInfo roomy;
        roomy.total_kb = 4 * 1024 * 1024;
        roomy.available_kb = 3 * 1024 * 1024;
        panel_->set_memory_for_test(roomy);
        // Every case streams from a source the test drives, never klippy's socket.
        panel_->preview_.set_source_factory(helix::calibration::test::manual_factory(log_));

        helix::ui::register_belt_path_sketch_widget();
        helix::ui::register_belt_gap_gauge_widget();
        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("panel_belt_tension"));
        view_ = panel_->create(test_screen());
        REQUIRE(view_ != nullptr);
        UpdateQueue::instance().drain();

        panel_->on_activate();
        UpdateQueue::instance().drain();
    }

    ~BeltPanelFixture() override {
        panel_->on_deactivate(DeactivateReason::NavigateAway);
        UpdateQueue::instance().drain();
        // Tears the widget tree down through the panel (nulls its cached root,
        // destroys the chart while the tree is still whole) BEFORE this
        // fixture's base deletes the screen underneath it.
        panel_->destroy_overlay_ui(view_);
        UpdateQueue::instance().drain();
        panel_->set_api(nullptr, nullptr);
        panel_->preview_.stop();
        panel_->preview_.set_source_factory(nullptr);
        panel_->deinit_subjects();
        helix::SettingsManager::instance().set_belt_layout(0);
        MoonrakerClientMock::remove_belt_csvs();
    }

    BeltTensionPanel& panel() {
        return *panel_;
    }
    MoonrakerClientMock& mock() {
        return client_;
    }
    lv_obj_t* view() {
        return view_;
    }

    /// Friendship pass-throughs: the chart and ghost series are not exposed
    /// by any subject, so the ghost case reads them directly.
    ui_frequency_response_chart_t* panel_chart() {
        return panel_->chart_;
    }
    int panel_ghost_id(int path_index) {
        return panel_->ghost_series_[path_index];
    }
    int panel_series_id(int path_index) {
        return panel_->series_[path_index];
    }
    /// What the preview's sources saw.
    helix::calibration::test::SourceLog& source_log() {
        return log_;
    }
    helix::calibration::LiveSweepPreview& panel_preview() {
        return panel_->preview_;
    }
    int panel_preview_series_id(int path_index) {
        return panel_->preview_series_[path_index];
    }
    void panel_preview_points(helix::calibration::BeltPath path,
                              const std::vector<helix::calibration::SweepPoint>& points) {
        panel_->on_preview_points(path, points);
    }
    /// One "Testing frequency" line, as the collector reports it.
    void panel_sweep_progress(float freq_hz) {
        panel_->on_sweep_progress(0, freq_hz);
    }
    void panel_set_socket_reachable(bool reachable) {
        panel_->klippy_socket_reachable_ = reachable;
    }
    std::optional<helix::calibration::BeltSweepWindow> panel_run_window() {
        return panel_->run_window_;
    }
    lv_obj_t* panel_chart_host_tuning() {
        return panel_->chart_host_tuning_;
    }
    void panel_set_view_state(BeltTensionPanel::ViewState state) {
        panel_->set_view_state(state);
    }
    int panel_tune_ghost_id(int path_index, int k) {
        return panel_->tune_ghost_series_[path_index][k];
    }
    lv_obj_t* panel_low_ram_dialog() {
        return panel_->low_ram_dialog_;
    }
    /// What the latest tuning round booked as turned; nullopt for none.
    std::optional<float> panel_last_turns() {
        const auto& t = panel_->tune_;
        if (!t || t->rounds().empty()) {
            return std::nullopt;
        }
        return t->rounds().back().turns_made;
    }
    /// The suggested amount in stepper steps; 0 without one.
    int panel_advised_steps() {
        const auto& t = panel_->tune_;
        if (!t || !t->advice().turns) {
            return 0;
        }
        return static_cast<int>(
            std::lround(*t->advice().turns / helix::calibration::belt_tune::kTurnStep));
    }
    const auto& panel_run(int path_index) {
        return panel_->runs_[path_index];
    }
    /// The tallest in-band bin of a path's current curve.
    float panel_run_peak(int path_index) {
        float tallest = 0.0f;
        for (const auto& [f, v] : panel_->runs_[path_index].curve) {
            if (f >= panel_->sweep_cfg_.min_freq && f <= panel_->sweep_cfg_.max_freq) {
                tallest = std::max(tallest, v);
            }
        }
        return tallest;
    }
    void panel_set_similarity(float percent) {
        panel_->similarity_percent_ = percent;
    }
    lv_timer_t* panel_note_age_timer() {
        return panel_->note_age_timer_.get();
    }
    bool panel_detection_pending() {
        return panel_->detection_pending_;
    }
    size_t facts_buf_size() const {
        return sizeof(panel_->facts_buf_);
    }
    size_t instruction_buf_size() const {
        return sizeof(panel_->tune_instruction_buf_);
    }
    size_t confirm_label_buf_size() const {
        return sizeof(panel_->tune_confirm_label_buf_);
    }
    size_t next_zone_label_buf_size() const {
        return sizeof(panel_->tune_next_zone_label_buf_);
    }
    size_t last_buf_size() const {
        return sizeof(panel_->tune_last_buf_);
    }
    size_t hint_buf_size() const {
        return sizeof(panel_->tune_hint_buf_);
    }
    size_t power_hint_buf_size() const {
        return sizeof(panel_->tune_power_hint_buf_);
    }
    size_t measure_label_buf_size() const {
        return sizeof(panel_->tune_measure_label_buf_);
    }
    size_t gap_caption_buf_size() const {
        return sizeof(panel_->tune_gap_caption_buf_);
    }
    size_t tensioner_buf_size() const {
        return sizeof(panel_->tune_tensioner_buf_);
    }
    size_t path_name_buf_size() const {
        return sizeof(panel_->slot_name_buf_[0]);
    }
    size_t run_title_buf_size() const {
        return sizeof(panel_->run_title_buf_);
    }
    size_t unpaired_buf_size() const {
        return sizeof(panel_->unpaired_buf_);
    }
    size_t identify_status_buf_size() const {
        return sizeof(panel_->identify_status_buf_);
    }
    /// Re-asks the printer's [resonance_tester] range, as an activation does.
    void panel_query_hw_facts() {
        panel_->query_hw_facts();
    }
    /// Hands the panel a [resonance_tester] answer, as its queries do.
    void panel_set_sweep_config(const helix::calibration::ResonanceTesterConfig& cfg) {
        panel_->set_sweep_config(cfg);
    }
    /// The live tuning session; null outside TUNING.
    const helix::calibration::BeltTuneSession* panel_session() {
        return panel_->tune_ ? &*panel_->tune_ : nullptr;
    }

    int state_int(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        INFO("subject not registered: " << name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
    std::string text(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        INFO("subject not registered: " << name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }

    /// Advance virtual time in 100ms steps, pumping LVGL timers and the
    /// UpdateQueue so mock sweep lines, guard timers and marshalled callbacks
    /// all get their turn.
    void pump_ms(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 100) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
    }
    /// Pumps in 100ms steps until `done` holds or the budget runs out.
    template <typename Pred> bool pump_until(Pred done, uint32_t budget_ms) {
        for (uint32_t t = 0; t < budget_ms && !done(); t += 100) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
        return done();
    }
    bool pump_until_state(int want, uint32_t budget_ms = 60000) {
        return pump_until_int("belt_tension_state", want, budget_ms);
    }
    bool pump_until_int(const char* name, int want, uint32_t budget_ms = 60000) {
        return pump_until([&] { return state_int(name) == want; }, budget_ms);
    }
    bool pump_until_text(const char* name, const std::string& want, uint32_t budget_ms = 60000) {
        return pump_until([&] { return text(name) == want; }, budget_ms);
    }
    /// Pumps until the gate opens (hardware detect + config probe answered).
    bool wait_gate_open(uint32_t budget_ms = 10000) {
        return pump_until_int("bt_can_start", 1, budget_ms);
    }

    /// The persona's travel and a max velocity, as a toolhead status frame.
    void set_toolhead_limits(double max_velocity) {
        const auto axis_max = helix::mock::descriptor(client_.get_printer_type()).axis_max;
        get_printer_state().update_from_status(
            {{"toolhead",
              {{"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
               {"axis_maximum", {axis_max.x, axis_max.y, axis_max.z, 0.0}},
               {"max_velocity", max_velocity}}}});
        UpdateQueue::instance().drain();
    }

    void set_klippy_ready(bool ready) {
        get_printer_state().set_klippy_state_sync(ready ? KlippyState::READY
                                                        : KlippyState::SHUTDOWN);
        pump_ms(2000);
    }

  private:
    /// printer_has_accelerometer lives on PrinterCapabilitiesState, which unit
    /// tests never register; provide the name the panel looks up.
    static void set_accel_subject(int value) {
        static lv_subject_t s;
        lv_subject_init_int(&s, value);
        lv_xml_register_subject(nullptr, "printer_has_accelerometer", &s);
    }

    helix::calibration::test::SourceLog log_;
    MoonrakerClientMock client_;
    PrinterState api_state_;
    MoonrakerAPIMock api_;
    BeltTensionPanel* panel_ = nullptr;
    lv_obj_t* view_ = nullptr;
};

// ============================================================================
// XML shape
// ============================================================================

TEST_CASE("belt tension panel has a container for every view state", "[belt][panel][xml]") {
    BeltPanelFixture fx;

    // One per ViewState value, in enum order.
    for (const char* name : {"state_start", "state_running", "state_results", "state_error"}) {
        INFO("missing state container: " << name);
        CHECK(lv_obj_find_by_name(fx.view(), name) != nullptr);
    }
    // Every named object the state machine and ctl driving depend on.
    for (const char* name :
         {"btn_start", "btn_stop", "btn_retest_slot_0", "btn_retest_slot_1", "btn_test_both",
          "btn_retry", "bt_similarity", "bt_facts_label", "bt_unpaired_label", "chart_host_running",
          "chart_host_results", "bt_sketch", "error_label"}) {
        INFO("missing named object: " << name);
        CHECK(lv_obj_find_by_name(fx.view(), name) != nullptr);
    }
    // The pluck-tuner states these replaced must be gone, not left alongside.
    CHECK(lv_obj_find_by_name(fx.view(), "state_position") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "state_listen") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "state_compare") == nullptr);
    // The direction rail is gone until real captures show a dependable
    // signed signal.
    CHECK(lv_obj_find_by_name(fx.view(), "bt_rail") == nullptr);
    // The per-path hero numbers are gone: the strongest pair's frequencies say
    // the least about the belts, so similarity is the only headline number.
    CHECK(lv_obj_find_by_name(fx.view(), "bt_peak_a") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "bt_peak_b") == nullptr);
}

TEST_CASE("belt tension panel binds only subjects that exist", "[belt][panel][xml]") {
    BeltPanelFixture fx;

    for (const char* name :
         {"belt_tension_state", "bt_can_start", "bt_gate_message", "bt_hw_kinematics",
          "bt_hw_accel", "bt_hw_sweep", "bt_run_title", "bt_run_detail", "bt_running_path",
          "bt_slot_0_note", "bt_slot_1_note", "bt_verdict", "bt_verdict_text", "bt_similarity",
          "bt_facts", "bt_unpaired", "bt_has_unpaired", "bt_chart_available", "bt_error_message"}) {
        INFO("subject not registered: " << name);
        CHECK(lv_xml_get_subject(nullptr, name) != nullptr);
    }
    // Retired with the pluck tuner. Leaving them registered would let a stale
    // binding survive review by continuing to resolve.
    for (const char* name : {"bt_hw_adxl", "bt_live_freq", "bt_median_freq", "bt_committed",
                             "bt_pluck_count", "bt_rail_value"}) {
        INFO("retired subject still registered: " << name);
        CHECK(lv_xml_get_subject(nullptr, name) == nullptr);
    }
}

// ============================================================================
// Run flow
// ============================================================================

TEST_CASE("Start settles, then runs A then B and lands on RESULTS", "[belt][panel]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_peaks_hz(104.0f, 98.0f);
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));

    // The mock records a script line by line: the laps, then A, then B.
    const auto history = std::vector<std::string>(fx.mock().gcode_script_history());
    REQUIRE_FALSE(history.empty());
    CHECK(history.front() == "SAVE_GCODE_STATE NAME=helix_belt_settle");
    std::vector<std::string> order;
    for (const auto& g : history) {
        if (g == "SAVE_GCODE_STATE NAME=helix_belt_settle") {
            order.push_back("settle");
        } else if (g.rfind("TEST_RESONANCES AXIS=1,-1 ", 0) == 0) {
            order.push_back("A");
        } else if (g.rfind("TEST_RESONANCES AXIS=1,1 ", 0) == 0) {
            order.push_back("B");
        }
    }
    CHECK(order == std::vector<std::string>{"settle", "A", "B"});

    // With no previous run the note names the age and nothing else.
    CHECK(fx.text("bt_slot_0_note") == "just now");
    CHECK(fx.text("bt_slot_1_note") == "just now");
    CHECK(fx.state_int("bt_verdict") == static_cast<int>(helix::calibration::BeltVerdict::CLOSE));
    CHECK(fx.text("bt_verdict_text") == "Fair match");

    // The headline is the similarity; the facts line lists the pairs, A/B.
    const std::string similarity = fx.text("bt_similarity");
    CHECK(similarity.back() == '%');
    CHECK(fx.text("bt_facts").find("104/98") != std::string::npos);

    // The listed pairs are numbered on both curves, the strongest as 1.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto marks_a = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(0));
    const auto marks_b = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(1));
    REQUIRE_FALSE(marks_a.empty());
    REQUIRE_FALSE(marks_b.empty());
    CHECK(marks_a.front().number == 1);
    CHECK(marks_a.front().freq_hz == Catch::Approx(104.0f).margin(1.0f));
    CHECK(marks_b.front().number == 1);
    CHECK(marks_b.front().freq_hz == Catch::Approx(98.0f).margin(1.0f));
}

TEST_CASE("a full sweep keeps both curves", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));

    for (int i = 0; i < 2; ++i) {
        const auto& run = fx.panel_run(i);
        REQUIRE_FALSE(run.curve.empty());
        REQUIRE(run.in_plane.size() == run.curve.size());
        for (size_t k = 0; k < run.curve.size(); ++k) {
            CHECK(run.in_plane[k].first == run.curve[k].first);
        }
    }
}

TEST_CASE("Re-test A keeps B and ghosts the old A", "[belt][panel][chart]") {
    BeltPanelFixture fx; // mock defaults: A=110, B=98
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    // 110 and 98 sit 12 Hz apart, past the 10 Hz pairing cap, so the only pair
    // the defaults form is the ~42 Hz rig peak both curves share, and each
    // belt peak is listed as unpaired.
    const std::string similarity_before = fx.text("bt_similarity");
    REQUIRE(similarity_before.back() == '%');
    CHECK(fx.state_int("bt_has_unpaired") == 1);
    CHECK(fx.text("bt_unpaired").find("Only on Path A: 110") != std::string::npos);
    CHECK(fx.text("bt_unpaired").find("Only on Path B: 98") != std::string::npos);

    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
    REQUIRE(fx.state_int("belt_tension_state") ==
            static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
    CHECK(fx.text("bt_run_title") == "Settling belts");
    CHECK(fx.pump_until_text("bt_run_title", "Re-measuring Path A"));
    // A re-measure walks A toward B by at most 4 Hz: 110 -> 106, now 8 Hz from
    // B, inside the cap: the belt-hump pair outranks the 42 Hz one.
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    // "was" under the similarity repeats the one shown before the re-measure;
    // the notes carry no history.
    CHECK(fx.text("bt_similarity_was") == "was " + similarity_before);
    CHECK(fx.state_int("bt_has_similarity_was") == 1);
    CHECK(fx.text("bt_slot_0_note").find("was") == std::string::npos);
    CHECK(fx.text("bt_slot_1_note").find("was") == std::string::npos);
    CHECK(fx.state_int("bt_has_unpaired") == 0);

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    CHECK(ui_frequency_response_chart_is_series_visible(chart, fx.panel_ghost_id(0)));
    CHECK_FALSE(ui_frequency_response_chart_is_series_visible(chart, fx.panel_ghost_id(1)));
}

TEST_CASE("each mock failure reaches ERROR", "[belt][panel]") {
    BeltPanelFixture fx;
    for (const BeltMockFailure failure :
         {BeltMockFailure::ERROR, BeltMockFailure::NO_FILE, BeltMockFailure::MULTICHIP}) {
        fx.mock().set_belt_failure(failure);
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
        CHECK(fx.text("bt_error_message").empty() == false);

        fx.panel().handle_retry_clicked();
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));
    }
}

TEST_CASE("a stall trips the stall guard", "[belt][panel]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_failure(BeltMockFailure::STALL);
    // 1ms lines: the sweep's quiet half arrives in the first moments of the
    // pump, so the whole budget is the silent STALL_TIMEOUT the guard waits.
    fx.mock().set_belt_line_interval_ms(1);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RUNNING)));

    // The sweep dies at its midpoint and goes quiet. The guard re-arms on
    // every progress line, so its window starts at the LAST line, not at
    // Start: drain the live half first (one line per pump step, ~65 of them
    // over the mock's 5-135 Hz range), then wait out a full silent
    // STALL_TIMEOUT from that point.
    fx.pump_ms(15000);
    fx.pump_ms(BeltTensionPanel::kStallTimeoutMs + 2000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::ERROR));
    CHECK(fx.text("bt_error_message").find("stopped reporting progress") != std::string::npos);
    CHECK(fx.text("bt_error_message").find("Power-cycle the printer") != std::string::npos);
    CHECK(fx.text("bt_error_message").find("SSH") != std::string::npos);
}

TEST_CASE("closing mid-run stops listening", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RUNNING)));

    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    UpdateQueue::instance().drain();
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));

    // The mock sweep timer keeps firing, but the cancelled run may not act on
    // it: the panel has to stay on START.
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
}

TEST_CASE("Start stays disabled while klippy is not ready", "[belt][panel][gating]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());

    fx.set_klippy_ready(false);
    CHECK(fx.state_int("bt_can_start") == 0);

    fx.set_klippy_ready(true);
    INFO("gate message after re-ready: " << fx.text("bt_gate_message"));
    CHECK(fx.state_int("bt_can_start") == 1);
}

TEST_CASE("a CoreXZ reports a closed gate naming its kinematics", "[belt][panel][gating]") {
    // The persona must be in place before the fixture's on_activate fires the
    // hardware detect, and the mock reads the env when it is constructed.
    helix_test::MockKinematicsEnv corexz("corexz");
    BeltPanelFixture fx;

    // Wait for detection to land: bt_hw_kinematics leaves "Detecting..." and
    // shows the raw name (an UNKNOWN kinematics has no friendly label).
    bool detected = false;
    for (uint32_t t = 0; t < 10000 && !detected; t += 100) {
        detected = fx.text("bt_hw_kinematics") == "corexz";
        if (!detected) {
            fx.pump_ms(100);
        }
    }
    REQUIRE(detected);

    CHECK(fx.state_int("bt_can_start") == 0);
    const std::string message = fx.text("bt_gate_message");
    INFO("gate message: " << message);
    CHECK(message.find("CoreXY") != std::string::npos);
    CHECK(message.find("corexz") != std::string::npos);
}

TEST_CASE("a sweep with too few frequency bins is an error", "[belt][panel]") {
    BeltPanelFixture fx;
    // A 5-6 Hz sweep writes only two bins; the comparison needs at least three
    // in-band bins per curve to say anything.
    fx.mock().set_resonance_sweep_range(5.0, 6.0);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
    CHECK(fx.text("bt_error_message").find("Not enough frequency data") != std::string::npos);
}

TEST_CASE("EMBEDDED tier creates no chart", "[belt][panel][chart]") {
    BeltPanelFixture fx(helix::PlatformTier::EMBEDDED);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    CHECK(fx.state_int("bt_chart_available") == 0);
    CHECK(fx.panel_chart() == nullptr);
}

TEST_CASE("a rebuild drops the chart with the tree it lives in", "[belt][panel][chart]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    REQUIRE(fx.panel_chart() != nullptr);

    // What a hot-reload does: free the whole widget tree and create it again.
    // The chart's object is a child of a host inside that tree, so the panel
    // must not come out of rebuild still holding a pointer to it.
    REQUIRE(fx.panel().rebuild());
    UpdateQueue::instance().drain();
    CHECK(fx.panel_chart() == nullptr);
    CHECK(fx.state_int("bt_chart_available") == 0);

    // The rebuilt tree still carries both hosts, and a fresh run rebuilds the
    // chart into the re-cached one.
    CHECK(lv_obj_find_by_name(fx.panel().get_root(), "chart_host_results") != nullptr);
}

TEST_CASE("both curves share one percent scale set by the taller", "[belt][panel][chart]") {
    // Path A replays a capture whose peak is half the height of B's synthetic
    // one, so a shared scale puts A near 50% and B at 100%.
    const std::string csv_a = "/tmp/helix-belt-half-height-" + std::to_string(getpid()) + ".csv";
    {
        std::ofstream out(csv_a);
        out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
        for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
            const double df = freq - 104.0;
            const double psd = 150.0 + 1.5e4 / (1.0 + df * df / 100.0);
            out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
                << '\n';
        }
    }
    helix::ScopedEnv replay("HELIX_MOCK_BELT_CSV_A", csv_a.c_str());

    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    std::remove(csv_a.c_str());

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto peak = [chart](int id) {
        const auto amps = ui_frequency_response_chart_get_series_amplitudes(chart, id);
        REQUIRE_FALSE(amps.empty());
        return *std::max_element(amps.begin(), amps.end());
    };
    CHECK(peak(fx.panel_series_id(1)) == Catch::Approx(100.0f).margin(0.5f));
    CHECK(peak(fx.panel_series_id(0)) == Catch::Approx(50.0f).margin(5.0f));
}

/// Renders @p obj and returns the RGB of every label it drew, the chart's
/// axis labels among them.
static std::vector<uint32_t> drawn_label_colors(lv_obj_t* obj) {
    std::vector<uint32_t> colors;
    const auto capture = [](lv_event_t* e) {
        auto* out = static_cast<std::vector<uint32_t>*>(lv_event_get_user_data(e));
        lv_draw_task_t* task = lv_event_get_draw_task(e);
        if (const auto* dsc = lv_draw_task_get_label_dsc(task)) {
            out->push_back(lv_color_to_u32(dsc->color) & 0xFFFFFF);
        }
    };
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(obj, capture, LV_EVENT_DRAW_TASK_ADDED, &colors);
    // A snapshot renders the object whether or not the overlay is on screen.
    if (lv_draw_buf_t* snap = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888)) {
        lv_draw_buf_destroy(snap);
    }
    lv_obj_remove_event_cb_with_user_data(obj, capture, &colors);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    return colors;
}

TEST_CASE("a theme toggle recolors the open chart's curves", "[belt][panel][chart]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto rgb = [chart](int id) {
        return lv_color_to_u32(ui_frequency_response_chart_get_series_color(chart, id)) & 0xFFFFFF;
    };
    const uint32_t before = rgb(fx.panel_series_id(0));
    // The axis labels are drawn by the chart's own draw pass, not styled.
    lv_obj_t* plot = lv_obj_get_child(ui_frequency_response_chart_get_obj(chart), 0);
    REQUIRE(plot != nullptr);
    const uint32_t muted_before = lv_color_to_u32(theme_manager_get_color("text_muted")) & 0xFFFFFF;
    const auto labels_before = drawn_label_colors(plot);

    theme_manager_toggle_dark_mode();
    UpdateQueue::instance().drain();
    const uint32_t muted_after = lv_color_to_u32(theme_manager_get_color("text_muted")) & 0xFFFFFF;
    const auto labels_after = drawn_label_colors(plot);
    const uint32_t want_a = lv_color_to_u32(theme_manager_get_color("belt_path_a")) & 0xFFFFFF;
    const uint32_t want_b = lv_color_to_u32(theme_manager_get_color("belt_path_b")) & 0xFFFFFF;
    const uint32_t a = rgb(fx.panel_series_id(0));
    const uint32_t ghost_a = rgb(fx.panel_ghost_id(0));
    const uint32_t b = rgb(fx.panel_series_id(1));
    const uint32_t live_a = rgb(fx.panel_preview_series_id(0));
    const uint32_t live_b = rgb(fx.panel_preview_series_id(1));
    theme_manager_toggle_dark_mode();
    UpdateQueue::instance().drain();

    REQUIRE(want_a != before);
    CHECK(a == want_a);
    CHECK(ghost_a == want_a);
    CHECK(b == want_b);
    CHECK(live_a == want_a);
    CHECK(live_b == want_b);

    // Axis labels repaint in the new mode's text_muted.
    REQUIRE(muted_after != muted_before);
    const auto count = [](const std::vector<uint32_t>& v, uint32_t c) {
        return std::count(v.begin(), v.end(), c);
    };
    CHECK(count(labels_before, muted_before) > 0);
    CHECK(count(labels_after, muted_after) > 0);
    CHECK(count(labels_after, muted_before) == 0);
}

TEST_CASE("sweep fact line comes from the printer's resonance_tester config", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    // Mock defaults 5-135 Hz at 1 Hz/s: two sweeps take ceil(2*130/60) = 5 min.
    CHECK(fx.text("bt_hw_sweep") == "5-135 Hz · about 5 min");
}

TEST_CASE("the host's free RAM decides whether Start asks, refuses or just starts",
          "[belt][panel]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    REQUIRE(fx.wait_gate_open());
    const auto with_memory = [&fx](size_t total_mb, size_t available_mb) {
        helix::MemoryInfo m;
        m.total_kb = total_mb * 1024;
        m.available_kb = available_mb * 1024;
        fx.panel().set_memory_for_test(m);
    };

    SECTION("enough: starts without asking") {
        with_memory(1024, 600);
        fx.panel().handle_start_clicked();
        CHECK(fx.panel_low_ram_dialog() == nullptr);
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
    }
    SECTION("low: asks first, and Continue starts") {
        with_memory(128, 80);
        fx.panel().handle_start_clicked();
        fx.pump_ms(500);
        // Klipper analyses the sweep on this host: on a small board the warning
        // comes first and nothing moves until it is answered.
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        CHECK(fx.panel_low_ram_dialog() == dialog);
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));

        // Continuing starts the sweep and clears the handle.
        lv_obj_t* go = lv_obj_find_by_name(dialog, "btn_primary");
        REQUIRE(go != nullptr);
        lv_obj_send_event(go, LV_EVENT_CLICKED, nullptr);
        fx.pump_ms(500);
        CHECK(fx.panel_low_ram_dialog() == nullptr);
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
    }
    SECTION("too little headroom: refuses") {
        // The CC1 as measured: Klipper's analysis is OOM-killed and klippy wedges.
        with_memory(114, 25);
        fx.panel().handle_start_clicked();
        fx.pump_ms(500);
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        // Not the continue-anyway warning: there is no way to start from here.
        CHECK(fx.panel_low_ram_dialog() == nullptr);

        lv_obj_t* ok = lv_obj_find_by_name(dialog, "btn_primary");
        REQUIRE(ok != nullptr);
        lv_obj_send_event(ok, LV_EVENT_CLICKED, nullptr);
        fx.pump_ms(500);
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));
    }
}

// ============================================================================
// Tuning loop
// ============================================================================

namespace {

constexpr int kTuning = static_cast<int>(BeltTensionPanel::ViewState::TUNING);
constexpr int kResults = static_cast<int>(BeltTensionPanel::ViewState::RESULTS);
constexpr int kPhaseAdjust = 0;
constexpr int kPhaseSweeping = 1;
constexpr int kPhaseMatched = 2;
constexpr int kPhaseCalibrate = 3;
constexpr int kPhaseAlreadyMatched = 4;

/// Start, wait for RESULTS, then Tune.
void enter_tuning(BeltPanelFixture& fx) {
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == kTuning);
}

/// One Measure or Re-measure: every sweep it starts, a baseline chained onto
/// a registered calibration included, then back out of SWEEPING.
void tune_round(BeltPanelFixture& fx) {
    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == kTuning);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") != kPhaseSweeping);
}

/// From CALIBRATE, measure until a turn registers and the zone's baseline is in.
void calibrate(BeltPanelFixture& fx) {
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    for (int i = 0; i < 2 && fx.state_int("bt_tune_phase") == kPhaseCalibrate; ++i) {
        tune_round(fx);
    }
    INFO("instruction: " << fx.text("bt_tune_instruction") << " last: " << fx.text("bt_tune_last"));
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);
}

/// Start, Tune, then calibrate: the loop is advising on a zone.
void enter_adjust(BeltPanelFixture& fx) {
    enter_tuning(fx);
    calibrate(fx);
}

/// Step the stepper to the suggested amount, as a user who turned it would.
void step_as_advised(BeltPanelFixture& fx) {
    for (int i = fx.panel_advised_steps(); i > 0; --i) {
        fx.panel().handle_tune_amount_step(+1);
    }
}

std::vector<std::string> resonance_lines(BeltPanelFixture& fx) {
    std::vector<std::string> out;
    for (const auto& g : std::vector<std::string>(fx.mock().gcode_script_history())) {
        if (g.find("TEST_RESONANCES") != std::string::npos) {
            out.push_back(g);
        }
    }
    return out;
}

bool lv_hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* named(BeltPanelFixture& fx, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(fx.view(), name);
    INFO("missing: " << name);
    REQUIRE(obj != nullptr);
    return obj;
}

/// Parses "Zone 93-115 Hz".
std::pair<int, int> zone_range(const std::string& zone) {
    int lo = 0, hi = 0;
    if (std::sscanf(zone.c_str(), "Zone %d-%d Hz", &lo, &hi) != 2) {
        return {-1, -1};
    }
    return {lo, hi};
}

/// The FREQ_START/FREQ_END a resonance line sent; {-1, -1} for a full sweep.
std::pair<int, int> freq_window(const std::string& line) {
    const auto start = line.find("FREQ_START=");
    const auto end = line.find("FREQ_END=");
    if (start == std::string::npos || end == std::string::npos) {
        return {-1, -1};
    }
    return {std::atoi(line.c_str() + start + 11), std::atoi(line.c_str() + end + 9)};
}

constexpr const char* kSettleStart = "SAVE_GCODE_STATE NAME=helix_belt_settle";

/// What the run sent, in order: "settle" per lap script, "full" or "narrow"
/// per resonance test.
std::vector<std::string> run_steps(BeltPanelFixture& fx) {
    std::vector<std::string> out;
    for (const auto& g : std::vector<std::string>(fx.mock().gcode_script_history())) {
        if (g == kSettleStart) {
            out.push_back("settle");
        } else if (g.find("TEST_RESONANCES") != std::string::npos) {
            out.push_back(g.find("FREQ_START=") == std::string::npos ? "full" : "narrow");
        }
    }
    return out;
}

/// A replay capture with equal-height Gaussian peaks (2 Hz sigma) at
/// `peaks_hz`, over the mock's 5-135 Hz band. No peaks writes the floor alone.
void write_two_peak_csv(const std::string& path, std::initializer_list<float> peaks_hz) {
    std::ofstream out(path);
    out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
    for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
        double psd = 150.0;
        for (const float p : peaks_hz) {
            const double df = freq - p;
            psd += 1.5e4 * std::exp(-df * df / 8.0);
        }
        out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
            << '\n';
    }
}

/// Minutes `pairs` sweeps of both paths over `swept` take at the mock's 1 Hz/s,
/// the fixture's 500 mm/s laps included.
int sweep_minutes(helix::calibration::BeltSweepWindow swept, int pairs) {
    const auto persona = helix::mock::descriptor(MoonrakerClientMock::PrinterType::VORON_24);
    helix::calibration::BeltSettleInputs in;
    in.x_max = static_cast<float>(persona.axis_max.x);
    in.y_max = static_cast<float>(persona.axis_max.y);
    in.has_xy_limits = true;
    in.probe_point = std::array<float, 3>{
        static_cast<float>((persona.axis_min.x + persona.axis_max.x) / 2.0),
        static_cast<float>((persona.axis_min.y + persona.axis_max.y) / 2.0), 20.0f};
    in.max_velocity_mm_s = 500.0f;
    const float laps = helix::calibration::plan_belt_settle(in).seconds;
    const float seconds =
        2.0f * static_cast<float>(pairs) * (swept.freq_end_hz - swept.freq_start_hz) + laps;
    return std::max(1, static_cast<int>(std::lround(seconds / 60.0f)));
}

/// A capture with one broad peak (10 Hz half width) at `centre_hz`, `height`
/// tall over a floor a two-hundredth of it.
void write_peak_csv_at(const std::string& path, double centre_hz, double height) {
    std::ofstream out(path);
    out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
    for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
        const double df = freq - centre_hz;
        const double psd = height / 200.0 + height / (1.0 + df * df / 100.0);
        out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
            << '\n';
    }
}

/// Both paths replay narrow-peaked captures, rewritten between sweeps. Their
/// zone centroids sit on the peaks, so a script reads as written; the mock's
/// own broad curves pull every centroid toward the zone's middle.
class ReplayBelts {
  public:
    ReplayBelts(std::initializer_list<float> a, std::initializer_list<float> b)
        : a_("/tmp/helix-belt-replay-a-" + std::to_string(getpid()) + ".csv"),
          b_("/tmp/helix-belt-replay-b-" + std::to_string(getpid()) + ".csv") {
        set_a(a);
        set_b(b);
        env_a_.emplace("HELIX_MOCK_BELT_CSV_A", a_.c_str());
        env_b_.emplace("HELIX_MOCK_BELT_CSV_B", b_.c_str());
    }
    ~ReplayBelts() {
        env_a_.reset();
        env_b_.reset();
        std::remove(a_.c_str());
        std::remove(b_.c_str());
    }
    void set_a(std::initializer_list<float> peaks) {
        write_two_peak_csv(a_, peaks);
    }
    void set_b(std::initializer_list<float> peaks) {
        write_two_peak_csv(b_, peaks);
    }

  private:
    std::string a_;
    std::string b_;
    std::optional<helix::ScopedEnv> env_a_;
    std::optional<helix::ScopedEnv> env_b_;
};

/// With A at 110 and B at 98: B's calibration sweep reads 106 (registers),
/// and the baseline pair repeats it, a -4 Hz gap.
void replay_to_adjust(BeltPanelFixture& fx, ReplayBelts& belts) {
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    belts.set_b({106.0f});
    calibrate(fx);
    REQUIRE(fx.panel_session() != nullptr);
    INFO("gap " << fx.panel_session()->gap_hz());
    REQUIRE(fx.panel_session()->gap_hz() == Catch::Approx(-4.0f).margin(0.5f));
}

} // namespace

TEST_CASE("the Tune button names the job by verdict", "[belt][panel][tune]") {
    SECTION("Fair match: Tune Belts") {
        BeltPanelFixture fx;
        fx.mock().set_belt_peaks_hz(104.0f, 98.0f);
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(kResults));
        REQUIRE(fx.state_int("bt_verdict") !=
                static_cast<int>(helix::calibration::BeltVerdict::MATCHED));
        CHECK(fx.text("bt_tune_cta") == "Tune Belts");
    }
    SECTION("Good match: Fine-tune") {
        BeltPanelFixture fx;
        fx.mock().set_belt_peaks_hz(104.0f, 104.0f);
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(kResults));
        REQUIRE(fx.state_int("bt_verdict") ==
                static_cast<int>(helix::calibration::BeltVerdict::MATCHED));
        CHECK(fx.text("bt_tune_cta") == "Fine-tune");
    }
}

TEST_CASE("Tune opens on the calibration turn", "[belt][panel][tune]") {
    BeltPanelFixture fx; // mock defaults: A=110, B=98, each tallest peak unpaired
    get_printer_state().set_printer_type_sync("Voron 2.4");
    enter_tuning(fx);

    CHECK(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    CHECK(fx.text("bt_tune_instruction") == "Tighten Path B one full turn");
    CHECK(fx.state_int("bt_tune_path") == 1);
    CHECK(fx.state_int("bt_tune_dir") == 0);
    CHECK(lv_hidden(named(fx, "bt_tune_stepper")));
    CHECK(lv_hidden(named(fx, "bt_tune_row_stepper")));
    CHECK(fx.text("bt_tune_amount").empty());
    // No zone until the turn says which one moved.
    CHECK(fx.text("bt_tune_zone").empty());
    CHECK(fx.state_int("bt_tune_shape_warning") == 1);
    CHECK(fx.text("bt_tune_round") == "Round 1");
    CHECK(fx.text("bt_tune_gap_caption") == "Path B is looser");
    CHECK(fx.text("bt_tune_last").empty());
    // The calibration measures both paths in full, as the sweep fact line does.
    const std::string hw = fx.text("bt_hw_sweep");
    const auto about = hw.find("about ");
    REQUIRE(about != std::string::npos);
    CHECK(fx.text("bt_tune_measure_label") == "Measure (~" + hw.substr(about + 6) + ")");
    CHECK(fx.text("bt_tune_measure_short_label") == "Measure");
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_done")));
    CHECK(lv_hidden(named(fx, "btn_tune_calibrate_anyway")));

    // Until a zone is known the chart spans the band.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [f_lo, f_hi] = ui_frequency_response_chart_get_freq_range(chart);
    CHECK(f_lo < 10.0f);
    CHECK(f_hi > 130.0f);
}

TEST_CASE("matched belts open on Good Enough", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_peaks_hz(100.0f, 100.0f);
    enter_tuning(fx);

    CHECK(fx.state_int("bt_tune_phase") == kPhaseAlreadyMatched);
    CHECK(fx.text("bt_tune_instruction") == "Belts already match");
    const std::string last = fx.text("bt_tune_last");
    INFO("last: " << last);
    CHECK(last.rfind("Within 0.", 0) == 0);
    CHECK(last.find(" Hz of each other at 100 Hz") != std::string::npos);
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_calibrate_anyway")));
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_done")));
    for (const char* hidden :
         {"bt_tune_stepper", "bt_tune_row_stepper", "btn_tune_swap", "btn_tune_measure",
          "btn_tune_measure_short", "bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
        INFO(hidden);
        CHECK(lv_hidden(named(fx, hidden)));
    }
    // The instruction reads in the matched colour on both heroes, not the path's.
    const uint32_t success = lv_color_to_u32(theme_manager_get_color("success")) & 0xFFFFFF;
    REQUIRE(success != (lv_color_to_u32(theme_manager_get_color("belt_path_b")) & 0xFFFFFF));
    for (const char* label : {"bt_tune_instruction", "bt_tune_line_instruction"}) {
        INFO(label);
        CHECK((lv_color_to_u32(lv_obj_get_style_text_color(named(fx, label), LV_PART_MAIN)) &
               0xFFFFFF) == success);
    }
    // The gauge shows how close they are.
    CHECK(fx.state_int("bt_tune_has_gap") == 1);
    REQUIRE(fx.panel_session() != nullptr);
    CHECK(helix::ui::ui_belt_gap_gauge_get_gap(named(fx, "bt_tune_gauge")) ==
          Catch::Approx(fx.panel_session()->gap_hz()));

    // Good Enough: back to RESULTS, nothing swept.
    fx.mock().clear_gcode_script_history();
    fx.panel().handle_tune_done_clicked();
    CHECK(fx.state_int("belt_tension_state") == kResults);
    fx.pump_ms(2000);
    CHECK(fx.mock().gcode_script_history().empty());

    // Calibrate Anyway: on to the calibration turn, nothing turned yet.
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAlreadyMatched);
    // Through the button's XML callback, as a tap does.
    lv_obj_send_event(named(fx, "btn_tune_calibrate_anyway"), LV_EVENT_CLICKED, nullptr);
    CHECK(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    const std::string instruction = fx.text("bt_tune_instruction");
    INFO("instruction: " << instruction);
    CHECK((instruction == "Tighten Path A one full turn" ||
           instruction == "Tighten Path B one full turn"));
    CHECK(fx.text("bt_tune_last").empty());
    CHECK(lv_hidden(named(fx, "btn_tune_calibrate_anyway")));
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_measure")));
    fx.pump_ms(2000);
    CHECK(fx.mock().gcode_script_history().empty());
}

TEST_CASE("Tune Belts waits for the printer's sweep range", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    REQUIRE(fx.state_int("bt_tune_range_known") == 1);
    CHECK_FALSE(lv_obj_has_state(named(fx, "btn_tune"), LV_STATE_DISABLED));

    // Klipper's defaults (max 135) stand in until a query answers.
    helix::calibration::ResonanceTesterConfig defaults;
    REQUIRE_FALSE(defaults.from_printer);
    fx.panel_set_sweep_config(defaults);
    CHECK(fx.state_int("bt_tune_range_known") == 0);
    CHECK(lv_obj_has_state(named(fx, "btn_tune"), LV_STATE_DISABLED));

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_tune_clicked();
    CHECK(fx.state_int("belt_tension_state") == kResults);
    CHECK(fx.panel_session() == nullptr);
    fx.pump_ms(2000);
    CHECK(fx.mock().gcode_script_history().empty());
}

TEST_CASE("the settle's range query enables Tune Belts", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    // The entry query failed: Klipper's defaults stand in.
    fx.panel_set_sweep_config(helix::calibration::ResonanceTesterConfig{});
    REQUIRE(fx.state_int("bt_tune_range_known") == 0);
    // Start's settle re-reads [resonance_tester], and the printer answers.
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    CHECK(fx.state_int("bt_tune_range_known") == 1);
    CHECK_FALSE(lv_obj_has_state(named(fx, "btn_tune"), LV_STATE_DISABLED));
}

TEST_CASE("a failed range query keeps the range the printer reported", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    fx.mock().set_resonance_sweep_range(10.0, 120.0);
    fx.panel_query_hw_facts();
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    REQUIRE(fx.state_int("bt_tune_range_known") == 1);
    const std::string sweep = fx.text("bt_hw_sweep");
    INFO("sweep: " << sweep);
    REQUIRE(sweep.rfind("10-120 Hz", 0) == 0);

    // The query fails: its answer is Klipper's defaults, which say nothing new.
    fx.mock().fail_next("printer.objects.query");
    fx.panel_query_hw_facts();
    fx.pump_ms(500);
    CHECK(fx.state_int("bt_tune_range_known") == 1);
    CHECK(fx.text("bt_hw_sweep") == sweep);
    CHECK_FALSE(lv_obj_has_state(named(fx, "btn_tune"), LV_STATE_DISABLED));
    fx.panel().handle_tune_clicked();
    CHECK(fx.state_int("belt_tension_state") == kTuning);
}

TEST_CASE("a calibration turn that doesn't register asks for another", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_tune_script('B', {98.3f, 104.0f});
    enter_tuning(fx);
    const auto before_a = fx.panel_run(0).curve;
    const auto before_b = fx.panel_run(1).curve;
    fx.mock().clear_gcode_script_history();

    tune_round(fx);
    CHECK(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    CHECK(fx.text("bt_tune_last") ==
          "That turn didn't register yet. Tighten Path B one more full turn.");
    CHECK(fx.text("bt_tune_instruction") == "Tighten Path B one full turn");
    CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full", "full"});
    // A calibration sweep feeds the session, never RESULTS' runs.
    CHECK(fx.panel_run(0).curve == before_a);
    CHECK(fx.panel_run(1).curve == before_b);
    CHECK(fx.state_int("belt_tension_state") == kTuning);
}

TEST_CASE("the second turn registers and chains a narrow baseline without settling",
          "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_tune_script('B', {98.3f, 104.0f});
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    fx.mock().clear_gcode_script_history();

    tune_round(fx);
    CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
    CHECK(fx.text("bt_tune_last").empty());
    // The laps once, both full sweeps, then the baseline pair twice over with
    // no laps between: no belt changed since the calibration sweep.
    CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full", "full", "narrow", "narrow",
                                                    "narrow", "narrow"});
    REQUIRE(fx.panel_session() != nullptr);
    const auto window = fx.panel_session()->sweep_window();
    REQUIRE(window.has_value());
    for (const auto& line : resonance_lines(fx)) {
        if (line.find("FREQ_START=") == std::string::npos) {
            continue;
        }
        INFO(line);
        const auto [lo, hi] = freq_window(line);
        CHECK(lo == static_cast<int>(window->freq_start_hz));
        CHECK(hi == static_cast<int>(window->freq_end_hz));
        CHECK(lo >= 5);
        CHECK(hi <= 135);
    }
    // The zone is known now; the chart spans it.
    const auto [z_lo, z_hi] = zone_range(fx.text("bt_tune_zone"));
    CHECK(z_lo > 0);
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [f_lo, f_hi] = ui_frequency_response_chart_get_freq_range(chart);
    CHECK(f_lo >= static_cast<float>(z_lo) - 1.0f);
    CHECK(f_hi <= static_cast<float>(z_hi) + 1.0f);
}

TEST_CASE("a calibration that gives up says to turn back", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    // Paired tallest peaks: no shape warning of their own.
    fx.mock().set_belt_peaks_hz(104.0f, 98.0f);
    fx.mock().set_belt_tune_script('B', {98.2f, 98.4f});
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    tune_round(fx);

    CHECK(fx.state_int("bt_tune_phase") == kPhaseMatched);
    CHECK(fx.text("bt_tune_instruction") == "Nothing to tune here");
    CHECK(
        fx.text("bt_tune_last") ==
        "Nothing followed 2 turns of Path B. Turn Path B back 2 turns, then check the mechanics.");
    // The line says it all: no "gap is not closing" warning, since nothing was tuned.
    CHECK(fx.state_int("bt_tune_shape_warning") == 0);
    CHECK(fx.state_int("bt_tune_has_zone") == 0);
    CHECK(lv_hidden(named(fx, "btn_tune_keep")));
}

TEST_CASE("three Re-measures walk CLOSER, CLOSER, MATCHED", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    // A wider start than the mock's default, so its broad curves leave room
    // for two closer rounds before the match.
    fx.mock().set_belt_peaks_hz(120.0f, 96.0f);
    fx.mock().set_belt_tune_script('B', {96.3f, 101.0f, 101.0f, 101.0f, 106.0f, 112.0f, 119.8f});
    enter_adjust(fx);

    const auto windowed_both = [&](const std::vector<std::string>& lines) {
        bool a = false, b = false;
        for (const auto& l : lines) {
            if (freq_window(l).first < 0) {
                return false;
            }
            a = a || l.find("AXIS=1,-1 ") != std::string::npos;
            b = b || l.find("AXIS=1,1 ") != std::string::npos;
        }
        return lines.size() == 2 && a && b;
    };

    for (const char* expect : {"Hz closer", "Hz closer"}) {
        fx.mock().clear_gcode_script_history();
        step_as_advised(fx);
        tune_round(fx);
        CHECK(windowed_both(resonance_lines(fx)));
        CHECK(run_steps(fx).front() == "settle");
        CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
        INFO("last: " << fx.text("bt_tune_last"));
        CHECK(fx.text("bt_tune_last").rfind("last turn: ", 0) == 0);
        CHECK(fx.text("bt_tune_last").find(expect) != std::string::npos);
        CHECK(fx.text("bt_tune_instruction").rfind("Tighten Path B ", 0) == 0);
    }

    fx.mock().clear_gcode_script_history();
    step_as_advised(fx);
    tune_round(fx);
    CHECK(windowed_both(resonance_lines(fx)));
    CHECK(fx.state_int("bt_tune_phase") == kPhaseMatched);
    INFO("instruction: " << fx.text("bt_tune_instruction"));
    CHECK(fx.text("bt_tune_instruction").rfind("Matched at 1", 0) == 0);
    CHECK(fx.text("bt_tune_last").empty());
    // Swapping paths is an ADJUST action; the one-line hero is tappable in
    // every phase, so a tap once matched must do nothing.
    fx.panel().handle_tune_swap_clicked();
    CHECK(fx.state_int("bt_tune_path") == 1);

    // The chart marks each path's centroid with a hollow ring.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto marks_b = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(1));
    REQUIRE(marks_b.size() == 1);
    CHECK(marks_b.front().number == 0);
    CHECK(marks_b.front().freq_hz == Catch::Approx(fx.panel_session()->adjusted_hz()));
}

TEST_CASE("each outcome has its own last-turn line", "[belt][panel][tune]") {
    struct Case {
        float a_hz;
        float b_hz;
        const char* expect;
        const char* instruction;
    };
    // From a -4 Hz gap (B at 106 against A at 110), with no rate learned: the
    // sanity bound is 6 Hz.
    for (const Case c : {
             Case{110.0f, 104.0f,
                  "The gap grew after tightening Path B. Check you turned Path B's tensioner.",
                  "Tighten Path B"},
             Case{110.0f, 111.5f, "Went past Path A by 1.", "Loosen Path B"},
             Case{110.0f, 106.3f, "Try a bigger turn.", "Tighten Path B"},
             Case{110.0f, 108.0f, "Hz closer", "Tighten Path B"},
             // A 7 Hz jump: dropped, so the advice stands.
             Case{103.0f, 106.0f,
                  "That reading jumped too far to trust. Re-measure without turning.",
                  "Tighten Path B"},
         }) {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        replay_to_adjust(fx, belts);
        belts.set_a({c.a_hz});
        belts.set_b({c.b_hz});
        tune_round(fx);
        INFO("A at " << c.a_hz << ", B at " << c.b_hz << ": " << fx.text("bt_tune_last"));
        CHECK(fx.text("bt_tune_last").find(c.expect) != std::string::npos);
        CHECK(fx.text("bt_tune_instruction") == c.instruction);
    }
}

TEST_CASE("Adjust A instead swaps the instruction", "[belt][panel][tune]") {
    SECTION("the calibration turn loosens the tighter belt") {
        BeltPanelFixture fx;
        enter_tuning(fx);
        REQUIRE(fx.text("bt_tune_instruction") == "Tighten Path B noticeably");
        fx.panel().handle_tune_swap_clicked();
        CHECK(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
        CHECK(fx.text("bt_tune_instruction") == "Loosen Path A noticeably");
        CHECK(fx.state_int("bt_tune_path") == 0);
        CHECK(fx.state_int("bt_tune_dir") == 1);
        CHECK(fx.text("bt_tune_gap_caption") == "Path A is tighter");
    }
    SECTION("a zone being tuned") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        replay_to_adjust(fx, belts);
        fx.panel().handle_tune_swap_clicked();
        CHECK(fx.text("bt_tune_instruction") == "Loosen Path A");
        CHECK(fx.state_int("bt_tune_path") == 0);
        CHECK(fx.state_int("bt_tune_dir") == 1);
        CHECK(fx.text("bt_tune_gap_caption") == "Path A is tighter");
    }
}

TEST_CASE("the calibration belt can be swapped only before the first step", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    // Neither path moves: no step registers.
    fx.mock().set_belt_tune_script('A', {110.0f});
    fx.mock().set_belt_tune_script('B', {98.0f});
    enter_tuning(fx);
    CHECK(fx.state_int("bt_tune_can_swap") == 1);
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_swap")));

    SECTION("before: the other belt loosens one full turn") {
        fx.panel().handle_tune_swap_clicked();
        CHECK(fx.text("bt_tune_instruction") == "Loosen Path A one full turn");
        CHECK_FALSE(lv_hidden(named(fx, "bt_tune_icon_ccw")));
        CHECK(lv_hidden(named(fx, "bt_tune_icon_cw")));
        tune_round(fx);
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
        CHECK(fx.text("bt_tune_last") ==
              "That turn didn't register yet. Loosen Path A one more full turn.");
        CHECK(fx.state_int("bt_tune_can_swap") == 0);
    }
    SECTION("after a step: refused, and the button is gone") {
        tune_round(fx);
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
        CHECK(fx.state_int("bt_tune_can_swap") == 0);
        CHECK(lv_hidden(named(fx, "btn_tune_swap")));
        fx.panel().handle_tune_swap_clicked();
        CHECK(fx.state_int("bt_tune_path") == 1);
        CHECK(fx.text("bt_tune_instruction") == "Tighten Path B one full turn");
    }
}

TEST_CASE("the turn stepper shows only for screw tensioners", "[belt][panel][tune]") {
    SECTION("unknown kind: words only") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        REQUIRE(get_printer_state().profile_state().printer_type().empty());
        replay_to_adjust(fx, belts);
        CHECK(fx.state_int("bt_tune_kind") == 0);
        CHECK(fx.text("bt_tune_amount").empty());
        CHECK(fx.text("bt_tune_rate").empty());
        CHECK(lv_hidden(named(fx, "bt_tune_stepper")));
        // Words only: no screw arrow either way.
        for (const char* icon : {"bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
            INFO(icon);
            CHECK(lv_hidden(named(fx, icon)));
        }
    }
    SECTION("screw: a quarter-turn stepper that starts empty, clamped at two turns") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        get_printer_state().set_printer_type_sync("Voron 2.4");
        replay_to_adjust(fx, belts);
        CHECK(fx.state_int("bt_tune_kind") == 1);
        CHECK_FALSE(lv_hidden(named(fx, "bt_tune_stepper")));
        // "Tighten Path B": the clockwise arrow, not the counter-clockwise one.
        CHECK_FALSE(lv_hidden(named(fx, "bt_tune_icon_cw")));
        CHECK(lv_hidden(named(fx, "bt_tune_icon_ccw")));
        // The calibration taught a rate, one decimal.
        CHECK(fx.text("bt_tune_rate").rfind("~", 0) == 0);
        CHECK(fx.text("bt_tune_rate").find(".") != std::string::npos);
        CHECK(fx.text("bt_tune_rate").find(" Hz / turn") != std::string::npos);
        CHECK(fx.text("bt_tune_amount") == "Not turned");
        // Minus from empty stays empty; plus goes to the smallest step.
        fx.panel().handle_tune_amount_step(-1);
        CHECK(fx.text("bt_tune_amount") == "Not turned");
        fx.panel().handle_tune_amount_step(+1);
        CHECK(fx.text("bt_tune_amount") == "¼ turn");
        for (int i = 0; i < 10; ++i) {
            fx.panel().handle_tune_amount_step(+1);
        }
        CHECK(fx.text("bt_tune_amount") == "2 turns");
        for (int i = 0; i < 7; ++i) {
            fx.panel().handle_tune_amount_step(-1);
        }
        CHECK(fx.text("bt_tune_amount") == "¼ turn");
        fx.panel().handle_tune_amount_step(-1);
        CHECK(fx.text("bt_tune_amount") == "Not turned");
        fx.panel().handle_tune_amount_step(-1);
        CHECK(fx.text("bt_tune_amount") == "Not turned");
    }
}

TEST_CASE("the instruction carries the advised amount", "[belt][panel][tune]") {
    SECTION("screw") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        get_printer_state().set_printer_type_sync("Voron 2.4");
        replay_to_adjust(fx, belts);
        // An 8 Hz calibration move per turn against a 4 Hz gap: half a turn.
        CHECK(fx.text("bt_tune_instruction") == "Tighten Path B ½ turn");
    }
    SECTION("unknown kind: no amount") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        replay_to_adjust(fx, belts);
        CHECK(fx.text("bt_tune_instruction") == "Tighten Path B");
    }
}

TEST_CASE("Good enough returns to RESULTS without sweeping", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    const std::string similarity = fx.text("bt_similarity");
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_tune_done_clicked();
    CHECK(fx.state_int("belt_tension_state") == kResults);
    fx.pump_ms(2000);
    CHECK(resonance_lines(fx).empty());
    CHECK(fx.text("bt_similarity") == similarity);

    // RESULTS shows the full band again.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [f_lo, f_hi] = ui_frequency_response_chart_get_freq_range(chart);
    CHECK(f_lo < 10.0f);
    CHECK(f_hi > 130.0f);
}

TEST_CASE("Confirm runs one full pair and reports the tuning", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_peaks_hz(120.0f, 96.0f);
    fx.mock().set_belt_tune_script('B', {96.3f, 101.0f, 101.0f, 101.0f, 106.0f, 112.0f, 119.8f});
    enter_tuning(fx);
    const std::string round0 = fx.text("bt_similarity");
    calibrate(fx);
    // Two calibration turns, then what each round's stepper recorded.
    float turned = 2.0f * helix::calibration::belt_tune::kCalibrationTurns;
    for (int i = 0; i < 3; ++i) {
        turned +=
            static_cast<float>(fx.panel_advised_steps()) * helix::calibration::belt_tune::kTurnStep;
        step_as_advised(fx);
        tune_round(fx);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseMatched);
    const int rounds = fx.panel_session()->total_rounds();

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_tune_confirm_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    const auto lines = resonance_lines(fx);
    REQUIRE(lines.size() == 2);
    for (const auto& l : lines) {
        CHECK(l.find("FREQ_START") == std::string::npos);
    }
    CHECK(fx.text("bt_similarity_was") == "was " + round0);
    for (const char* note : {"bt_slot_0_note", "bt_slot_1_note"}) {
        INFO(note << ": " << fx.text(note));
        CHECK(fx.text(note).find("was") == std::string::npos);
    }
    INFO("facts: " << fx.text("bt_facts"));
    const std::string expect =
        "Tuned in " + std::to_string(rounds) + " rounds · Path B tightened about " +
        helix::calibration::format_belt_turns(turned) + (turned > 1.0f ? " turns" : " turn");
    CHECK(fx.text("bt_facts").find(expect) != std::string::npos);
    CHECK(fx.text("bt_facts").find("Path A tightened") == std::string::npos);
    CHECK(fx.text("bt_facts").find("Path A loosened") == std::string::npos);
}

namespace {

/// Starts a sweep, confirms Stop's dialog, and checks the panel lands on
/// START and stays there.
void stop_lands_on_start(BeltPanelFixture& fx) {
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    fx.panel().handle_stop_clicked();
    fx.pump_ms(200);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* stop = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(stop != nullptr);
    lv_obj_send_event(stop, LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(200);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(fx.panel_session() == nullptr);
}

/// Rebuilds with a sweep in flight and checks its late callbacks change nothing.
void rebuild_drops_late_callbacks(BeltPanelFixture& fx) {
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    // Past the laps: a sweep is running on the printer.
    REQUIRE(fx.pump_until_text("bt_run_title", "Measuring Path A"));
    fx.pump_ms(300);
    // Frees the tree the chart lives in while the mock keeps sweeping.
    REQUIRE(fx.panel().rebuild());
    UpdateQueue::instance().drain();
    // Long enough for a full sweep's every line to land.
    fx.pump_ms(30000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(fx.panel_chart() == nullptr);
    CHECK(fx.panel_session() == nullptr);
}

} // namespace

TEST_CASE("a zone matched at its baseline reports no round count", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    // The calibration sweep finds B at A's peak: its baseline matches.
    fx.mock().set_belt_tune_script('B', {110.0f});
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseMatched);
    REQUIRE(fx.panel_session()->total_rounds() == 0);

    fx.panel().handle_tune_confirm_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    INFO("facts: " << fx.text("bt_facts"));
    CHECK(fx.text("bt_facts").find("Tuned in") == std::string::npos);
    // The calibration turn is still reported.
    CHECK(fx.text("bt_facts").find(" · Path B tightened about 1 turn") != std::string::npos);
}

TEST_CASE("Stop during a tuning sweep or a calibration run lands on START", "[belt][panel][tune]") {
    const bool calibrating = GENERATE(false, true);
    CAPTURE(calibrating);
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    if (calibrating) {
        enter_tuning(fx);
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    } else {
        enter_adjust(fx);
    }
    fx.panel().handle_tune_measure_clicked();
    stop_lands_on_start(fx);
}

TEST_CASE("a rebuild mid tuning sweep or calibration run drops the late callbacks",
          "[belt][panel][tune]") {
    BeltPanelFixture fx;
    SECTION("tuning sweep") {
        enter_adjust(fx);
        fx.panel().handle_tune_measure_clicked();
        rebuild_drops_late_callbacks(fx);
    }
    SECTION("calibration run") {
        enter_tuning(fx);
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
        const auto before = fx.panel_run(1).curve;
        fx.panel().handle_tune_measure_clicked();
        rebuild_drops_late_callbacks(fx);
        // Nothing the abandoned calibration measured reached RESULTS' runs.
        CHECK(fx.panel_run(1).curve == before);
    }
}

TEST_CASE("a round with no peak says so and can be re-measured", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    replay_to_adjust(fx, belts);
    const std::string gap = fx.text("bt_tune_gap");
    belts.set_b({});
    tune_round(fx);

    CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
    CHECK(fx.text("bt_tune_last") == "No clear peak in this zone");
    CHECK(fx.text("bt_tune_gap") == gap);
    lv_obj_t* measure = named(fx, "btn_tune_measure");
    CHECK_FALSE(lv_obj_has_state(measure, LV_STATE_DISABLED));
    CHECK_FALSE(lv_hidden(measure));
}

TEST_CASE("the text faces draw the turn fractions", "[belt][panel][tune][font]") {
    // The faces every build links: the turn amount and facts line use them.
    for (const lv_font_t* font :
         {&noto_sans_18, &noto_sans_26, &noto_sans_bold_28, &noto_sans_light_16}) {
        for (const uint32_t cp : {0xBCu, 0xBDu, 0xBEu, 0x215Bu, 0x215Cu, 0x215Du, 0x215Eu}) {
            lv_font_glyph_dsc_t dsc{};
            INFO("U+" << std::hex << cp << " in a " << std::dec << font->line_height << " px face");
            CHECK(lv_font_get_glyph_dsc(font, &dsc, cp, 0));
        }
        // A fraction outside the baked range is absent, so the lookup can fail.
        lv_font_glyph_dsc_t sixth{};
        CHECK_FALSE(lv_font_get_glyph_dsc(font, &sixth, 0x2159, 0));
    }
}

namespace {

/// Points PrinterDetector at a user extension naming one spring-tensioned
/// printer for the life of this object.
class SpringPrinterDb {
  public:
    SpringPrinterDb() : dir_("/tmp/helix-belt-spring-db-" + std::to_string(getpid())) {
        env_.emplace("HELIX_CONFIG_DIR", dir_.c_str());
        std::filesystem::create_directories(dir_ + "/printer_database.d");
        std::ofstream(dir_ + "/printer_database.d/spring.json")
            << R"({"printers": [{"id": "test_spring", "name": "Test Spring Printer",
                                  "belt_tensioner": "spring"}]})";
        PrinterDetector::reload();
    }
    ~SpringPrinterDb() {
        // The database reloads from the real config dir, not this one.
        env_.reset();
        PrinterDetector::reload();
        std::filesystem::remove_all(dir_);
    }

  private:
    std::string dir_;
    std::optional<helix::ScopedEnv> env_;
};

} // namespace

TEST_CASE("a spring printer skips calibration", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    SpringPrinterDb db;
    REQUIRE(PrinterDetector::get_belt_tensioner("Test Spring Printer") ==
            helix::calibration::BeltTensionerKind::SPRING);
    get_printer_state().set_printer_type_sync("Test Spring Printer");
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    fx.mock().clear_gcode_script_history();

    // The strongest zone at once: its baseline pair sweeps straight from Tune.
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == kTuning);
    CHECK(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    CHECK(fx.text("bt_tune_instruction") == "Measure this zone first");
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        fx.pump_ms(100);
    }
    CHECK(run_steps(fx) ==
          std::vector<std::string>{"settle", "narrow", "narrow", "narrow", "narrow"});

    CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
    CHECK(fx.state_int("bt_tune_kind") == 2);
    CHECK(fx.text("bt_tune_instruction") ==
          "Release Path B's tensioner, let the spring settle, lock it");
    CHECK(fx.text("bt_tune_amount").empty());
    for (const char* name : {"bt_tune_stepper", "bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
        INFO(name);
        CHECK(lv_hidden(named(fx, name)));
    }
}

TEST_CASE("Next Zone walks only registered zones", "[belt][panel][tune]") {
    // Two pairs on each path: 100/94 and 60/56.
    ReplayBelts belts({60.0f, 100.0f}, {56.0f, 94.0f});
    BeltPanelFixture fx;
    enter_tuning(fx);
    REQUIRE(fx.text("bt_tune_instruction") == "Tighten Path B noticeably");

    SECTION("both zones moved with the turn") {
        // The 94 peak moves 4 Hz, the 56 one 2 Hz: both register, the bigger first.
        belts.set_b({58.0f, 98.0f});
        calibrate(fx);
        const auto [lo, hi] = zone_range(fx.text("bt_tune_zone"));
        CHECK(lo <= 94);
        CHECK(hi >= 100);
        CHECK(fx.state_int("bt_tune_has_next_zone") == 1);

        // B's upper peak meets A's: this zone matches, the lower pair is next.
        belts.set_b({58.0f, 100.0f});
        tune_round(fx);
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseMatched);
        REQUIRE(fx.state_int("bt_tune_has_next_zone") == 1);

        fx.mock().clear_gcode_script_history();
        fx.panel().handle_tune_next_zone_clicked();
        // The baseline pair starts at once, with no advice on screen.
        CHECK(fx.state_int("bt_tune_phase") == kPhaseSweeping);
        CHECK(fx.state_int("bt_tune_baseline") == 1);
        CHECK(fx.text("bt_tune_instruction") == "Measure this zone first");
        CHECK(fx.text("bt_tune_amount").empty());
        CHECK(fx.text("bt_tune_gap") == "--");
        const auto [lo2, hi2] = zone_range(fx.text("bt_tune_zone"));
        CHECK(lo2 <= 56);
        CHECK(hi2 >= 60);
        CHECK(hi2 < 90);

        for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping;
             t += 100) {
            fx.pump_ms(100);
        }
        // Turns on the earlier zone moved this one: laps first, then the pair twice.
        CHECK(run_steps(fx) ==
              std::vector<std::string>{"settle", "narrow", "narrow", "narrow", "narrow"});
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);
        CHECK(fx.state_int("bt_tune_baseline") == 0);
        CHECK(fx.text("bt_tune_instruction") == "Tighten Path B");
        CHECK(std::stof(fx.text("bt_tune_gap")) == Catch::Approx(2.0f).margin(0.3f));
        // A baseline has no previous turn to report on.
        CHECK(fx.text("bt_tune_last").empty());
        CHECK(fx.state_int("bt_tune_has_next_zone") == 0);
    }
    SECTION("only the zone that moved") {
        belts.set_b({56.0f, 98.0f});
        calibrate(fx);
        CHECK(fx.state_int("bt_tune_has_next_zone") == 0);
        belts.set_b({56.0f, 100.0f});
        tune_round(fx);
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseMatched);
        CHECK(fx.state_int("bt_tune_has_next_zone") == 0);
        CHECK(fx.text("bt_tune_next_zone_label").empty());
    }
}

TEST_CASE("a closed gate stops the tuning sweeps", "[belt][panel][tune][gating]") {
    BeltPanelFixture fx;
    const auto close_gate = [&] {
        REQUIRE(fx.state_int("bt_can_start") == 1);
        // A print starts while the user is turning a screw.
        lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
        fx.pump_ms(200);
        CHECK(fx.state_int("bt_can_start") == 0);
        CHECK(lv_obj_has_state(named(fx, "btn_tune_measure"), LV_STATE_DISABLED));
        fx.mock().clear_gcode_script_history();
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(2000);
        CHECK(resonance_lines(fx).empty());
    };
    SECTION("a calibration") {
        enter_tuning(fx);
        close_gate();
        CHECK(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    }
    SECTION("a round") {
        enter_adjust(fx);
        close_gate();
        CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
    }
}

TEST_CASE("a wrong-way line names the turn the user made", "[belt][panel][tune]") {
    // Told to tighten B, B holds while A rises 110 -> 113: the gap grew, which
    // is what tightening A instead of B does.
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    replay_to_adjust(fx, belts);
    REQUIRE(fx.text("bt_tune_instruction") == "Tighten Path B ½ turn");
    step_as_advised(fx);
    belts.set_a({113.0f});
    tune_round(fx);
    INFO("last: " << fx.text("bt_tune_last"));
    CHECK(fx.panel_last_turns() == std::optional<float>(0.5f));
    CHECK(fx.text("bt_tune_last") ==
          "The gap grew after tightening Path B. Check you turned Path B's tensioner.");
}

TEST_CASE("both peaks drifting together is no move", "[belt][panel][tune]") {
    // No turn: A and B both read 1.5 Hz higher. The gap held at -4.
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    replay_to_adjust(fx, belts);
    belts.set_a({111.5f});
    belts.set_b({107.5f});
    tune_round(fx);
    INFO("last: " << fx.text("bt_tune_last"));
    CHECK(fx.text("bt_tune_last").find("The gap didn't close (moved 0.") == 0);
    CHECK(fx.text("bt_tune_instruction") == "Tighten Path B");
}

TEST_CASE("a reference peak that leaves the zone names the reference", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    replay_to_adjust(fx, belts);
    SECTION("the reference's zone is flat") {
        // Told to tighten B; A, the reference, shows no peak.
        belts.set_a({});
        tune_round(fx);
        CHECK(fx.text("bt_tune_last") ==
              "The peak of Path A left the zone. Check that only Path B's tensioner was turned. "
              "Not sure which is which? Tap the pencil to identify the belts.");
    }
    SECTION("the adjusted path losing its peak keeps the generic line") {
        belts.set_b({});
        tune_round(fx);
        CHECK(fx.text("bt_tune_last") == "No clear peak in this zone");
    }
}

TEST_CASE("readings that don't repeat give no advice", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    enter_tuning(fx);
    // B's calibration sweep registers at 106; the pair's second sweep reads
    // 2 Hz higher with nothing turned.
    belts.set_b({106.0f});
    fx.panel().handle_tune_measure_clicked();
    bool second_pair = false;
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        if (!second_pair && fx.panel_session() &&
            fx.panel_session()->stage() == helix::calibration::BeltTuneStage::BASELINE &&
            !fx.panel_session()->rounds().empty()) {
            belts.set_b({108.0f});
            second_pair = true;
        }
        fx.pump_ms(100);
    }
    REQUIRE(second_pair);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);

    CHECK(fx.state_int("bt_tune_unrepeatable") == 1);
    CHECK(fx.state_int("bt_tune_shape_warning") == 1);
    CHECK(fx.text("bt_tune_hint") == "These readings aren't repeatable, so tuning can't be "
                                     "steered. Check the mechanics first.");
    // No turn advice: neither a direction nor an amount.
    CHECK(fx.text("bt_tune_instruction") == "Readings don't repeat");
    CHECK(fx.text("bt_tune_amount").empty());
    for (const char* hidden :
         {"bt_tune_stepper", "bt_tune_row_stepper", "bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
        INFO(hidden);
        CHECK(lv_hidden(named(fx, hidden)));
    }
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_done")));
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_measure")));
    // The wrapping line carries the whole sentence.
    CHECK_FALSE(lv_hidden(named(fx, "bt_tune_hint_wrap")));
    // Re-measure sweeps the pair twice: both counted.
    const auto window = fx.panel_session()->sweep_window();
    REQUIRE(window.has_value());
    REQUIRE(sweep_minutes(*window, 1) != sweep_minutes(*window, 2));
    CHECK(fx.text("bt_tune_measure_label") ==
          "Re-measure (~" + std::to_string(sweep_minutes(*window, 2)) + " min)");

    // Re-measure: a fresh pair, laps once; this one repeats.
    fx.mock().clear_gcode_script_history();
    tune_round(fx);
    CHECK(run_steps(fx) ==
          std::vector<std::string>{"settle", "narrow", "narrow", "narrow", "narrow"});
    CHECK(fx.state_int("bt_tune_unrepeatable") == 0);
    CHECK(fx.text("bt_tune_instruction").rfind("Tighten Path B ", 0) == 0);
    CHECK_FALSE(lv_hidden(named(fx, "bt_tune_stepper")));
}

TEST_CASE("the power hint names the stronger path", "[belt][panel][tune]") {
    // A spring printer goes straight to its zone. B's peak is four times A's.
    const std::string csv_a = "/tmp/helix-belt-power-a-" + std::to_string(getpid()) + ".csv";
    const std::string csv_b = "/tmp/helix-belt-power-b-" + std::to_string(getpid()) + ".csv";
    BeltPanelFixture fx;
    SpringPrinterDb db;
    get_printer_state().set_printer_type_sync("Test Spring Printer");

    const auto run = [&](double b_height) {
        write_peak_csv_at(csv_a, 108.0, 1.0e4);
        write_peak_csv_at(csv_b, 100.0, b_height);
        helix::ScopedEnv replay_a("HELIX_MOCK_BELT_CSV_A", csv_a.c_str());
        helix::ScopedEnv replay_b("HELIX_MOCK_BELT_CSV_B", csv_b.c_str());
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(kResults));
        fx.panel().handle_tune_clicked();
        for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping;
             t += 100) {
            fx.pump_ms(100);
        }
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);
    };
    SECTION("B four times A") {
        run(4.0e4);
        CHECK(fx.state_int("bt_tune_has_power_hint") == 1);
        INFO("hint: " << fx.text("bt_tune_power_hint"));
        const auto hint = fx.panel_session()->power_hint();
        REQUIRE(hint.has_value());
        CHECK(hint->stronger == helix::calibration::BeltPath::PATH_B);
        CHECK(fx.text("bt_tune_power_hint") ==
              fmt::format("Path B resonates much more than Path A at {:.0f} Hz. That points at a "
                          "mechanical difference (pulley grub screws, idler bearings, belt "
                          "tracking), not tension.",
                          hint->freq_hz));
        CHECK_FALSE(lv_hidden(named(fx, "bt_tune_power_row")));
    }
    SECTION("equal heights") {
        run(1.0e4);
        CHECK(fx.state_int("bt_tune_has_power_hint") == 0);
        CHECK(lv_hidden(named(fx, "bt_tune_power_row")));
    }
    std::remove(csv_a.c_str());
    std::remove(csv_b.c_str());
}

TEST_CASE("waking the screen mid-tuning keeps the session", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    replay_to_adjust(fx, belts);
    belts.set_b({108.0f});
    tune_round(fx);
    // The baseline pair is not a round: this was the first, the next is the second.
    REQUIRE(fx.text("bt_tune_round") == "Round 2");
    const std::string zone = fx.text("bt_tune_zone");
    const std::string last = fx.text("bt_tune_last");
    REQUIRE_FALSE(fx.panel_detection_pending());

    SECTION("between rounds, at the tensioner") {
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.panel().on_activate();
        fx.pump_ms(500);
        CHECK(fx.state_int("belt_tension_state") == kTuning);
        CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
        CHECK(fx.text("bt_tune_round") == "Round 2");
        CHECK(fx.text("bt_tune_zone") == zone);
        CHECK(fx.text("bt_tune_last") == last);
        // The calibrator is not asked to detect hardware again.
        CHECK_FALSE(fx.panel_detection_pending());
        // The loop carries on from where it was.
        tune_round(fx);
        CHECK(fx.text("bt_tune_round") == "Round 3");
    }
    SECTION("while a round sweeps") {
        fx.panel().handle_tune_measure_clicked();
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
        fx.pump_ms(300);
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.panel().on_activate();
        CHECK(fx.state_int("belt_tension_state") == kTuning);
        CHECK(fx.state_int("bt_tune_phase") == kPhaseSweeping);
        CHECK_FALSE(fx.panel_detection_pending());
        for (uint32_t t = 0; t < 60000 && fx.state_int("bt_tune_phase") == kPhaseSweeping;
             t += 100) {
            fx.pump_ms(100);
        }
        CHECK(fx.state_int("belt_tension_state") == kTuning);
        CHECK(fx.text("bt_tune_round") == "Round 3");
    }
    SECTION("leaving after a suspension makes the next entry a fresh one") {
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.panel().on_deactivate(DeactivateReason::NavigateAway);
        fx.panel().on_activate();
        fx.pump_ms(200);
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));
    }
}

TEST_CASE("a wake re-asks what the suspension cancelled", "[belt][panel]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));

    SECTION("hardware detection still pending") {
        fx.panel().on_deactivate(DeactivateReason::NavigateAway);
        fx.panel().on_activate();
        REQUIRE(fx.panel_detection_pending());
        // The screen sleeps before detection answers; its callbacks expire.
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.pump_ms(2000);
        REQUIRE(fx.panel_detection_pending());
        fx.panel().on_activate();
        fx.pump_ms(2000);
        CHECK_FALSE(fx.panel_detection_pending());
        CHECK(fx.wait_gate_open());
    }
    SECTION("the low-RAM warning closes with the screen") {
        enter_tuning(fx);
        helix::MemoryInfo small;
        small.total_kb = 128 * 1024;
        small.available_kb = 80 * 1024;
        fx.panel().set_memory_for_test(small);
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(200);
        REQUIRE(fx.panel_low_ram_dialog() != nullptr);
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.pump_ms(500);
        CHECK(fx.panel_low_ram_dialog() == nullptr);
        CHECK(ModalStack::instance().top_dialog() == nullptr);
        // After the wake, Measure asks again and Continue sweeps.
        fx.panel().on_activate();
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(200);
        lv_obj_t* dialog = fx.panel_low_ram_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
        fx.pump_ms(200);
        CHECK(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    }
}

TEST_CASE("Continue on the low-RAM warning re-checks the gate", "[belt][panel][tune][gating]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    helix::MemoryInfo small;
    small.total_kb = 128 * 1024;
    small.available_kb = 80 * 1024;

    const auto continue_dialog = [&] {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        REQUIRE(fx.panel_low_ram_dialog() == dialog);
        // A print starts from another client while the warning is up.
        lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
        fx.pump_ms(200);
        fx.mock().clear_gcode_script_history();
        lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
        fx.pump_ms(2000);
        CHECK(resonance_lines(fx).empty());
    };

    SECTION("a calibration") {
        enter_tuning(fx);
        fx.panel().set_memory_for_test(small);
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(200);
        continue_dialog();
        CHECK(fx.state_int("belt_tension_state") == kTuning);
        CHECK(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    }
    SECTION("a tuning round") {
        enter_adjust(fx);
        const std::string round = fx.text("bt_tune_round");
        fx.panel().set_memory_for_test(small);
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(200);
        continue_dialog();
        CHECK(fx.state_int("belt_tension_state") == kTuning);
        CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
        CHECK(fx.text("bt_tune_round") == round);
    }
    SECTION("a Re-test leaves both results in place") {
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(kResults));
        const std::string note_a = fx.text("bt_slot_0_note");
        fx.panel().set_memory_for_test(small);
        fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
        fx.pump_ms(200);
        continue_dialog();
        CHECK(fx.state_int("belt_tension_state") == kResults);
        CHECK(fx.text("bt_slot_0_note") == note_a);
        // Path A's result survived: Tune can still start from the comparison.
        lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 0);
        fx.pump_ms(200);
        fx.panel().handle_tune_clicked();
        CHECK(fx.state_int("belt_tension_state") == kTuning);
    }
}

TEST_CASE("the low-RAM warning is asked once per tuning session", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    enter_tuning(fx);
    helix::MemoryInfo small;
    small.total_kb = 128 * 1024;
    small.available_kb = 80 * 1024;
    fx.panel().set_memory_for_test(small);

    fx.panel().handle_tune_measure_clicked();
    fx.pump_ms(200);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(fx.panel_low_ram_dialog() == dialog);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(200);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    // The calibration registers and its baseline follows without asking.
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);

    // The next round sweeps at once.
    fx.panel().handle_tune_measure_clicked();
    CHECK(fx.panel_low_ram_dialog() == nullptr);
    CHECK(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    for (uint32_t t = 0; t < 60000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        fx.pump_ms(100);
    }

    // A new tuning session asks again.
    fx.panel().handle_tune_done_clicked();
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == kTuning);
    fx.panel().handle_tune_measure_clicked();
    fx.pump_ms(200);
    CHECK(fx.panel_low_ram_dialog() != nullptr);
    CHECK(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
}

TEST_CASE("a Stop confirmed after the round ended stops nothing", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    enter_tuning(fx);
    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    fx.panel().handle_stop_clicked();
    fx.pump_ms(200);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);

    // The calibration and its baseline finish while the dialog is still up.
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);
    const std::string round = fx.text("bt_tune_round");

    fx.mock().clear_gcode_script_history();
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(2000);
    CHECK(fx.state_int("belt_tension_state") == kTuning);
    CHECK(fx.text("bt_tune_round") == round);
    for (const auto& g : std::vector<std::string>(fx.mock().gcode_script_history())) {
        INFO(g);
        CHECK(g.find("FIRMWARE_RESTART") == std::string::npos);
    }
}

TEST_CASE("an untouched stepper is a repeat check", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    replay_to_adjust(fx, belts);
    REQUIRE(fx.text("bt_tune_instruction") == "Tighten Path B ½ turn");
    REQUIRE(fx.text("bt_tune_amount") == "Not turned");

    // B reads 0.6 Hz lower with nothing turned: against the advice, but no turn.
    belts.set_b({105.4f});
    tune_round(fx);
    INFO("last: " << fx.text("bt_tune_last"));
    // No verdict on a turn nobody made: the line reports the gap's move.
    CHECK(fx.text("bt_tune_last") == "No turn: the gap moved 0.6 Hz");
    CHECK(fx.text("bt_tune_last").find("grew") == std::string::npos);
    CHECK_FALSE(fx.panel_last_turns().has_value());
    // Nothing was turned, so the advice does not double.
    CHECK(fx.text("bt_tune_instruction").rfind("Tighten Path B ", 0) == 0);
    CHECK(fx.text("bt_tune_amount") == "Not turned");

    // A stepper set to a half books a half, then reads empty again.
    fx.panel().handle_tune_amount_step(+1);
    fx.panel().handle_tune_amount_step(+1);
    REQUIRE(fx.text("bt_tune_amount") == "½ turn");
    belts.set_b({108.0f});
    tune_round(fx);
    CHECK(fx.panel_last_turns() == std::optional<float>(0.5f));
    CHECK(fx.text("bt_tune_amount") == "Not turned");

    // A swap starts the stepper empty too.
    fx.panel().handle_tune_amount_step(+1);
    fx.panel().handle_tune_swap_clicked();
    fx.panel().handle_tune_swap_clicked();
    CHECK(fx.text("bt_tune_amount") == "Not turned");

    step_as_advised(fx);
    belts.set_b({110.0f});
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseMatched);
    fx.panel().handle_tune_confirm_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    // Only the turned rounds booked anything.
    INFO("facts: " << fx.text("bt_facts"));
    CHECK(fx.text("bt_facts").find("Path B tightened about") != std::string::npos);
}

TEST_CASE("the gate re-reads the calibrator when a sweep ends", "[belt][panel][gating]") {
    BeltPanelFixture fx;
    lv_subject_t* print_active = get_printer_state().print_state().get_print_active_subject();
    // A gate subject notifying mid-sweep reads the calibrator as busy.
    const auto notify_mid_sweep = [&] {
        fx.pump_ms(300);
        lv_subject_notify(print_active);
        fx.pump_ms(100);
        REQUIRE(fx.state_int("bt_can_start") == 0);
    };

    SECTION("a full check") {
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        notify_mid_sweep();
        REQUIRE(fx.pump_until_state(kResults));
        CHECK(fx.state_int("bt_can_start") == 1);
    }
    SECTION("a tuning round") {
        enter_adjust(fx);
        fx.panel().handle_tune_measure_clicked();
        notify_mid_sweep();
        for (uint32_t t = 0; t < 60000 && fx.state_int("bt_tune_phase") == kPhaseSweeping;
             t += 100) {
            fx.pump_ms(100);
        }
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);
        CHECK(fx.state_int("bt_can_start") == 1);
    }
}

TEST_CASE("Next zone names how long its sweep takes", "[belt][panel][tune]") {
    ReplayBelts belts({60.0f, 100.0f}, {56.0f, 94.0f});
    BeltPanelFixture fx;
    enter_tuning(fx);
    belts.set_b({58.0f, 98.0f});
    calibrate(fx);
    belts.set_b({58.0f, 100.0f});
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_has_next_zone") == 1);

    // The margins past the zone count too.
    const auto minutes = [](helix::calibration::BeltSweepWindow zone, int pairs) {
        return sweep_minutes(helix::calibration::zone_sweep_window(zone, 5.0f, 135.0f), pairs);
    };
    const auto next = fx.panel_session()->peek_next_zone();
    REQUIRE(next.has_value());
    // The 56/58 zone and its margins: about 50 Hz, swept four times.
    CHECK(fx.text("bt_tune_next_zone_label") ==
          "Next Zone (~" + std::to_string(minutes(next->window, 2)) + " min)");

    fx.panel().handle_tune_next_zone_clicked();
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);
    // After the baseline, a round sweeps the pair once.
    const auto zone = fx.panel_session()->zone();
    REQUIRE(zone.has_value());
    CHECK(fx.text("bt_tune_measure_label") ==
          "Re-measure (~" + std::to_string(minutes(zone->window, 1)) + " min)");
    // No third zone: the label empties with the button.
    CHECK(fx.state_int("bt_tune_has_next_zone") == 0);
    CHECK(fx.text("bt_tune_next_zone_label").empty());
}

TEST_CASE("every locale's tuning strings fit their buffers", "[belt][panel][tune][i18n]") {
    BeltPanelFixture fx;
    constexpr int kMaxListed = 3;
    // The longest each line can get: three-digit peaks, two-digit counts,
    // turn totals past one.
    for (const char* lang : {"en", "de", "es", "fr", "it", "ja", "pt", "ru", "zh"}) {
        INFO("locale " << lang);
        if (std::string(lang) != "en") {
            helix::ui::ensure_translation_loaded(lang);
            lv_translation_set_language(lang);
            REQUIRE(std::string(lv_tr("Next Zone")) != "Next Zone");
            REQUIRE(std::string(lv_tr("Belts already match")) != "Belts already match");
        }
        const auto f = [](const char* tag, auto... args) {
            return fmt::format(fmt::runtime(lv_tr(tag)), args...);
        };
        // The longest name this locale gives a path, in either form, under any layout.
        std::string name;
        for (int layout = 0; layout < helix::calibration::kBeltLayoutCount; ++layout) {
            for (const auto path :
                 {helix::calibration::BeltPath::PATH_A, helix::calibration::BeltPath::PATH_B}) {
                const auto n = helix::ui::belt_path_name(
                    path, static_cast<helix::calibration::BeltLayout>(layout));
                for (const auto& form : {n.title, n.phrase}) {
                    if (form.size() > name.size()) {
                        name = form;
                    }
                }
            }
        }
        REQUIRE(name.size() > 2);
        std::string pairs;
        for (int i = 0; i < kMaxListed; ++i) {
            pairs += std::string(i == 0 ? "" : " · ") + "133/132";
        }
        const std::string turns = f("{} turns", "10¾");
        std::string facts = f("Peaks {} Hz", pairs) + f(" · Tuned in {} rounds", 99);
        for (const char* per_belt : {" · {} tightened about {}", " · {} loosened about {}"}) {
            facts += f(per_belt, name, turns);
        }
        CHECK(facts.size() < fx.facts_buf_size());

        for (const char* instruction :
             {"Release {}'s tensioner, let the spring settle, lock it", "Tighten {}", "Loosen {}",
              "Tighten {0} one full turn", "Tighten {0} noticeably", "Loosen {0} one full turn",
              "Loosen {0} noticeably"}) {
            INFO(instruction);
            CHECK(f(instruction, name).size() < fx.instruction_buf_size());
        }
        for (const char* instruction : {"Tighten {0} {1}", "Loosen {0} {1}"}) {
            CHECK(f(instruction, name, turns).size() < fx.instruction_buf_size());
        }
        for (const char* instruction : {"Belts already match", "Nothing to tune here",
                                        "Readings don't repeat", "Measure this zone first"}) {
            INFO(instruction);
            CHECK(std::string(lv_tr(instruction)).size() < fx.instruction_buf_size());
        }
        CHECK(f("Matched at {:.1f} Hz", 133.3).size() < fx.instruction_buf_size());
        CHECK(f("Confirm (full, ~{} min)", 99).size() < fx.confirm_label_buf_size());
        CHECK(f("Next Zone (~{} min)", 99).size() < fx.next_zone_label_buf_size());
        CHECK(f("Measure (~{} min)", 99).size() < fx.measure_label_buf_size());
        CHECK(f("Re-measure (~{} min)", 99).size() < fx.measure_label_buf_size());
        for (const char* last :
             {"The gap grew after tightening {0}. Check you turned {0}'s tensioner.",
              "The gap grew after loosening {0}. Check you turned {0}'s tensioner.",
              "That turn didn't register yet. Tighten {0} one more full turn.",
              "That turn didn't register yet. Tighten {0} a little more.",
              "That turn didn't register yet. Loosen {0} one more full turn.",
              "That turn didn't register yet. Loosen {0} a little more.",
              "Nothing followed the turns on {0}. Undo them, then check the mechanics."}) {
            INFO(last);
            CHECK(f(last, name).size() < fx.last_buf_size());
        }
        CHECK(f("Nothing followed {1} turns of {0}. Turn {0} back {1} turns, then check the "
                "mechanics.",
                name, 10)
                  .size() < fx.last_buf_size());
        CHECK(f("Within {0:.1f} Hz of each other at {1:.0f} Hz", 1.5, 133.0).size() <
              fx.last_buf_size());
        CHECK(std::string(lv_tr("That reading jumped too far to trust. Re-measure without "
                                "turning."))
                  .size() < fx.last_buf_size());
        CHECK(f("No turn: the gap moved {:.1f} Hz", 12.3).size() < fx.last_buf_size());
        CHECK(f("The peak of {0} left the zone. Check that only {1}'s tensioner was turned. Not "
                "sure which is which? Tap the pencil to identify the belts.",
                name, name)
                  .size() < fx.last_buf_size());
        CHECK(f("Went past {} by {:.1f} Hz. Back it off a little.", name, 12.3).size() <
              fx.last_buf_size());
        CHECK(f("The peak of {} moved out of the zone. Widening it.", name).size() <
              fx.last_buf_size());
        for (const char* caption : {"{} is looser", "{} is tighter"}) {
            CHECK(f(caption, name).size() < fx.gap_caption_buf_size());
        }
        CHECK(f("Tensioner: {}", name).size() < fx.tensioner_buf_size());
        CHECK(name.size() < fx.path_name_buf_size());
        for (const char* title : {"Measuring {}", "Re-measuring {}"}) {
            CHECK(f(title, name).size() < fx.run_title_buf_size());
        }
        CHECK((f("Only on {0}: {1} Hz", name, "120, 129, 133") + " · " +
               f("Only on {0}: {1} Hz", name, "120, 129, 133"))
                  .size() < fx.unpaired_buf_size());
        CHECK(f("Buzzing the motor of {}", name).size() < fx.identify_status_buf_size());
        for (const char* hint :
             {"These readings aren't repeatable, so tuning can't be steered. Check the mechanics "
              "first.",
              "Curves differ in shape. Check the mechanics before chasing a perfect match.",
              "The gap is not closing. Something besides tension may be shaping it."}) {
            INFO(hint);
            CHECK(std::string(lv_tr(hint)).size() < fx.hint_buf_size());
        }
        CHECK(f("{0} resonates much more than {1} at {2:.0f} Hz. That points at a mechanical "
                "difference (pulley grub screws, idler bearings, belt tracking), not tension.",
                name, name, 133.0)
                  .size() < fx.power_hint_buf_size());
    }
    lv_translation_set_language(helix::ui::kIdentityLocale);
}

// ============================================================================
// Compact layouts: below medium, or portrait
// ============================================================================

namespace {

/// Pins one int layout subject (ui_breakpoint, ui_is_portrait) for a case and
/// puts it back after.
class ScopedLayoutSubject {
  public:
    ScopedLayoutSubject(const char* name, int value) : subject_(lv_xml_get_subject(nullptr, name)) {
        INFO("layout subject not registered: " << name);
        REQUIRE(subject_ != nullptr);
        original_ = lv_subject_get_int(subject_);
        lv_subject_set_int(subject_, value);
    }
    ~ScopedLayoutSubject() {
        lv_subject_set_int(subject_, original_);
    }
    void set(int value) {
        lv_subject_set_int(subject_, value);
    }

  private:
    lv_subject_t* subject_;
    int original_ = 0;
};

constexpr int kBpMicro = 0;
constexpr int kBpSmall = 2;
constexpr int kBpMedium = 3;

std::vector<std::string> visible_of(BeltPanelFixture& fx,
                                    std::initializer_list<const char*> names) {
    std::vector<std::string> out;
    for (const char* name : names) {
        if (!lv_hidden(named(fx, name))) {
            out.emplace_back(name);
        }
    }
    return out;
}

std::vector<std::string> visible_results_buttons(BeltPanelFixture& fx) {
    return visible_of(fx, {"btn_retest_slot_0", "btn_retest_slot_1", "btn_retest_menu",
                           "btn_test_both", "btn_tune"});
}

const std::vector<std::string> kFolded{"btn_retest_menu", "btn_test_both", "btn_tune"};
const std::vector<std::string> kUnfolded{"btn_retest_slot_0", "btn_retest_slot_1", "btn_test_both",
                                         "btn_tune"};

/// Opens the Re-test menu, returning the row named `row` inside it.
lv_obj_t* open_retest_menu(BeltPanelFixture& fx, const char* row) {
    lv_obj_send_event(named(fx, "btn_retest_menu"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(100);
    REQUIRE(helix::ui::ContextMenu::active() != nullptr);
    lv_obj_t* item = lv_obj_find_by_name(lv_obj_get_screen(fx.view()), row);
    INFO("menu row: " << row);
    REQUIRE(item != nullptr);
    return item;
}

/// Opens the Re-test menu and taps one of its rows.
void pick_retest(BeltPanelFixture& fx, const char* row) {
    lv_obj_send_event(open_retest_menu(fx, row), LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    CHECK(helix::ui::ContextMenu::active() == nullptr);
}

void enter_results_with_menu(BeltPanelFixture& fx) {
    helix::ui::ContextMenu::register_shared_callbacks();
    REQUIRE(fx.register_component("belt_retest_menu"));
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
}

} // namespace

TEST_CASE("RESULTS folds Re-test A/B into one menu below medium and in portrait",
          "[belt][panel][compact]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", kBpMicro);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    enter_results_with_menu(fx);

    CHECK(visible_results_buttons(fx) == kFolded);

    SECTION("every breakpoint below medium folds them") {
        for (int narrow : {1, kBpSmall}) {
            bp.set(narrow);
            INFO("ui_breakpoint " << narrow);
            CHECK(visible_results_buttons(fx) == kFolded);
        }
    }
    SECTION("landscape medium keeps the two Re-test buttons") {
        bp.set(kBpMedium);
        CHECK(visible_results_buttons(fx) == kUnfolded);
    }
    SECTION("portrait medium folds them") {
        bp.set(kBpMedium);
        portrait.set(1);
        CHECK(visible_results_buttons(fx) == kFolded);
    }
    SECTION("Re-test A from the menu re-measures A") {
        pick_retest(fx, "btn_retest_menu_slot_0");
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
        CHECK(fx.pump_until_text("bt_run_title", "Re-measuring Path A"));
    }
    SECTION("Re-test B from the menu re-measures B") {
        pick_retest(fx, "btn_retest_menu_slot_1");
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
        CHECK(fx.pump_until_text("bt_run_title", "Re-measuring Path B"));
    }
}

TEST_CASE("leaving the panel closes the Re-test menu so it cannot start a sweep",
          "[belt][panel][compact]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", kBpMicro);
    enter_results_with_menu(fx);

    lv_obj_t* row = open_retest_menu(fx, "btn_retest_menu_slot_0");
    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    UpdateQueue::instance().drain();
    fx.pump_ms(100);
    CHECK(helix::ui::ContextMenu::active() == nullptr);
    CHECK(lv_obj_find_by_name(lv_obj_get_screen(fx.view()), "btn_retest_menu_slot_0") == nullptr);

    // A tap that was already in flight on the row: nothing may start.
    fx.mock().clear_gcode_script_history();
    if (lv_obj_is_valid(row)) {
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    }
    fx.pump_ms(2000);
    CHECK(resonance_lines(fx).empty());
    CHECK(fx.state_int("belt_tension_state") !=
          static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
}

TEST_CASE("TUNING below medium is one tappable line over the chart",
          "[belt][panel][compact][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    ScopedLayoutSubject bp("ui_breakpoint", kBpMicro);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_kind") == 1);

    auto named = [&](const char* name) { return ::named(fx, name); };
    // The calibration turn records nothing: no stepper, the short button measures.
    CHECK(lv_hidden(named("bt_tune_row_stepper")));
    CHECK_FALSE(lv_hidden(named("btn_tune_measure_short")));
    CHECK(fx.text("bt_tune_measure_short_label") == "Measure");
    calibrate(fx);
    CHECK(fx.text("bt_tune_measure_short_label") == "Re-measure");

    CHECK(lv_hidden(named("bt_tune_hero")));
    CHECK_FALSE(lv_hidden(named("bt_tune_line")));
    // The stepper and the short Re-measure share the button row; the swap
    // button gives its place to the tappable line.
    CHECK_FALSE(lv_hidden(named("bt_tune_row_stepper")));
    CHECK_FALSE(lv_hidden(named("btn_tune_measure_short")));
    CHECK(lv_hidden(named("btn_tune_measure")));
    CHECK(lv_hidden(named("btn_tune_swap")));

    const std::string amount = fx.text("bt_tune_amount");
    lv_obj_send_event(named("btn_tune_row_more"), LV_EVENT_CLICKED, nullptr);
    CHECK(fx.text("bt_tune_amount") != amount);
    lv_obj_send_event(named("btn_tune_row_less"), LV_EVENT_CLICKED, nullptr);
    CHECK(fx.text("bt_tune_amount") == amount);

    REQUIRE(fx.text("bt_tune_instruction").rfind("Tighten Path B ", 0) == 0);
    lv_obj_send_event(named("bt_tune_line"), LV_EVENT_CLICKED, nullptr);
    CHECK(fx.state_int("bt_tune_path") == 0);

    SECTION("landscape medium keeps the full hero") {
        bp.set(kBpMedium);
        CHECK_FALSE(lv_hidden(named("bt_tune_hero")));
        CHECK(lv_hidden(named("bt_tune_line")));
        CHECK(lv_hidden(named("bt_tune_row_stepper")));
        CHECK_FALSE(lv_hidden(named("btn_tune_swap")));
    }
    SECTION("micro portrait uses the one-line hero") {
        portrait.set(1);
        CHECK(lv_hidden(named("bt_tune_hero")));
        CHECK_FALSE(lv_hidden(named("bt_tune_line")));
        CHECK_FALSE(lv_hidden(named("bt_tune_row_stepper")));
        CHECK(lv_hidden(named("btn_tune_swap")));
    }
}

TEST_CASE("the one-line hero's gauge shows the round's gap", "[belt][panel][compact][tune]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", kBpMicro);
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseAdjust);

    lv_obj_t* line_gauge = named(fx, "bt_tune_line_gauge");
    CHECK_FALSE(lv_hidden(line_gauge));
    const float hero_gap = helix::ui::ui_belt_gap_gauge_get_gap(named(fx, "bt_tune_gauge"));
    INFO("hero gauge gap " << hero_gap);
    REQUIRE(std::abs(hero_gap) > 1.0f);
    CHECK(helix::ui::ui_belt_gap_gauge_get_gap(line_gauge) == Catch::Approx(hero_gap));
}

TEST_CASE("Confirm drops its duration on compact layouts and beside Next zone",
          "[belt][panel][compact][tune]") {
    BeltPanelFixture fx;
    // The calibration sweep finds B at A's peak: its baseline matches.
    fx.mock().set_belt_tune_script('B', {110.0f});
    ScopedLayoutSubject bp("ui_breakpoint", kBpMedium);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseMatched);
    lv_subject_t* next_zone = lv_xml_get_subject(nullptr, "bt_tune_has_next_zone");
    REQUIRE(next_zone != nullptr);
    const auto confirms = [&] {
        return visible_of(fx, {"btn_tune_confirm", "btn_tune_confirm_short"});
    };
    const std::vector<std::string> full{"btn_tune_confirm"};
    const std::vector<std::string> shortened{"btn_tune_confirm_short"};

    lv_subject_set_int(next_zone, 0);
    CHECK(confirms() == full);
    CHECK(lv_hidden(named(fx, "btn_tune_next_zone")));

    SECTION("Next zone beside it at medium") {
        lv_subject_set_int(next_zone, 1);
        CHECK_FALSE(lv_hidden(named(fx, "btn_tune_next_zone")));
        CHECK(confirms() == shortened);
    }
    SECTION("micro, no Next zone") {
        bp.set(kBpMicro);
        CHECK(confirms() == shortened);
    }
    SECTION("portrait medium, no Next zone") {
        portrait.set(1);
        CHECK(confirms() == shortened);
    }
}

TEST_CASE("the tuning view hosts the chart", "[belt][panel][tune][chart]") {
    BeltPanelFixture fx;
    enter_tuning(fx);

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    lv_obj_t* host = lv_obj_find_by_name(fx.view(), "chart_host_tuning");
    REQUIRE(host != nullptr);
    CHECK(fx.panel_chart_host_tuning() == host);
    CHECK(lv_obj_get_parent(ui_frequency_response_chart_get_obj(chart)) == host);
}

TEST_CASE("destroying the UI drops every chart host and the tuning ghosts",
          "[belt][panel][tune][chart]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    REQUIRE(fx.panel_chart_host_tuning() != nullptr);
    REQUIRE(fx.panel_tune_ghost_id(0, 0) >= 0);
    REQUIRE(fx.panel_tune_ghost_id(1, 1) >= 0);
    REQUIRE(fx.panel_preview_series_id(0) >= 0);

    fx.panel().on_ui_destroyed();
    CHECK(fx.panel_chart() == nullptr);
    CHECK(fx.panel_chart_host_tuning() == nullptr);
    for (int path = 0; path < 2; ++path) {
        CHECK(fx.panel_preview_series_id(path) == -1);
        for (int k = 0; k < 2; ++k) {
            CHECK(fx.panel_tune_ghost_id(path, k) == -1);
        }
    }
}

TEST_CASE("leaving the panel or a fresh Start ends the tuning session", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_tune_script('B', {110.0f});
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseMatched);

    SECTION("leaving") {
        fx.panel().on_deactivate(DeactivateReason::NavigateAway);
        UpdateQueue::instance().drain();
        CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));
    }
    SECTION("a fresh Start") {
        fx.panel().handle_start_clicked();
        CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
        REQUIRE(fx.pump_until_state(kResults));
        CHECK(fx.state_int("bt_tune_phase") == kPhaseAdjust);
    }
}

TEST_CASE("a tuning round keeps the peak markers on screen while it sweeps",
          "[belt][panel][tune][chart]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    for (int path = 0; path < 2; ++path) {
        REQUIRE(ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(path)).size() ==
                1);
    }

    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    for (int path = 0; path < 2; ++path) {
        CHECK(ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(path)).size() == 1);
    }
}

TEST_CASE("a gate that closes during a tuning sweep ends it in ERROR",
          "[belt][panel][tune][gating]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);

    lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
    fx.pump_ms(200);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::ERROR));
}

TEST_CASE("a theme toggle recolors the tuning ghosts", "[belt][panel][tune][chart]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto rgb = [chart](int id) {
        return lv_color_to_u32(ui_frequency_response_chart_get_series_color(chart, id)) & 0xFFFFFF;
    };
    const uint32_t before = rgb(fx.panel_tune_ghost_id(0, 0));

    theme_manager_toggle_dark_mode();
    UpdateQueue::instance().drain();
    const uint32_t want_a = lv_color_to_u32(theme_manager_get_color("belt_path_a")) & 0xFFFFFF;
    const uint32_t want_b = lv_color_to_u32(theme_manager_get_color("belt_path_b")) & 0xFFFFFF;
    std::vector<uint32_t> after;
    for (int path = 0; path < 2; ++path) {
        for (int k = 0; k < 2; ++k) {
            after.push_back(rgb(fx.panel_tune_ghost_id(path, k)));
        }
    }
    theme_manager_toggle_dark_mode();
    UpdateQueue::instance().drain();

    REQUIRE(want_a != before);
    CHECK(after[0] == want_a);
    CHECK(after[1] == want_a);
    CHECK(after[2] == want_b);
    CHECK(after[3] == want_b);
}

TEST_CASE("a closed gate stops a Re-test", "[belt][panel][gating]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));

    lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
    fx.pump_ms(200);
    REQUIRE(fx.state_int("bt_can_start") == 0);

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
    fx.pump_ms(2000);
    CHECK(resonance_lines(fx).empty());
    CHECK(fx.state_int("belt_tension_state") == kResults);
}

TEST_CASE("cleanup closes the Re-test menu", "[belt][panel][compact]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", kBpMicro);
    enter_results_with_menu(fx);
    open_retest_menu(fx, "btn_retest_menu_slot_0");

    fx.panel().cleanup();
    UpdateQueue::instance().drain();
    CHECK(helix::ui::ContextMenu::active() == nullptr);
}

// ============================================================================
// Ultrawide: the chart grows on the left, the rest sits in a sidebar
// ============================================================================

TEST_CASE("TUNING on ultrawide puts the chart beside a sidebar with the full hero",
          "[belt][panel][ultrawide][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    // 1920x440 reads small, which alone would pick the one-line hero.
    ScopedLayoutSubject bp("ui_breakpoint", kBpSmall);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 1);
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_kind") == 1);

    auto named = [&](const char* name) { return ::named(fx, name); };
    CHECK_FALSE(lv_hidden(named("chart_host_wide")));
    CHECK(lv_hidden(named("chart_host_tuning")));
    CHECK_FALSE(lv_hidden(named("bt_tune_hero")));
    CHECK(lv_hidden(named("bt_tune_line")));
    CHECK(lv_hidden(named("bt_tune_line_gauge")));
    CHECK(lv_hidden(named("bt_tune_row_stepper")));
    CHECK_FALSE(lv_hidden(named("btn_tune_swap")));
    CHECK_FALSE(lv_hidden(named("btn_tune_measure")));
    CHECK(lv_hidden(named("btn_tune_measure_short")));

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    lv_obj_t* chart_obj = ui_frequency_response_chart_get_obj(chart);
    CHECK(lv_obj_get_parent(chart_obj) == named("chart_host_wide"));

    // The sidebar keeps the token's width instead of stretching.
    lv_obj_update_layout(fx.view());
    const char* side_w = lv_xml_get_const(nullptr, "belt_sidebar_w");
    REQUIRE(side_w != nullptr);
    CHECK(lv_obj_get_width(named("bt_main")) == std::atoi(side_w));

    SECTION("leaving ultrawide hands the chart back to the state's own host") {
        ultrawide.set(0);
        UpdateQueue::instance().drain();
        fx.pump_ms(100);
        CHECK(lv_obj_get_parent(chart_obj) == named("chart_host_tuning"));
        CHECK(lv_hidden(named("chart_host_wide")));
        CHECK(lv_hidden(named("bt_tune_hero")));
        CHECK_FALSE(lv_hidden(named("bt_tune_line")));
    }
}

TEST_CASE("RESULTS on ultrawide keeps both Re-test buttons in the sidebar",
          "[belt][panel][ultrawide]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", kBpSmall);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 1);
    enter_results_with_menu(fx);

    CHECK(visible_results_buttons(fx) == kUnfolded);
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    CHECK(lv_obj_get_parent(ui_frequency_response_chart_get_obj(chart)) ==
          named(fx, "chart_host_wide"));
    CHECK(lv_hidden(named(fx, "chart_host_results")));

    SECTION("START has no chart, so the panel takes the full width") {
        fx.panel_set_view_state(BeltTensionPanel::ViewState::START);
        CHECK(lv_hidden(named(fx, "chart_host_wide")));
        lv_obj_update_layout(fx.view());
        CHECK(lv_obj_get_width(named(fx, "bt_main")) ==
              lv_obj_get_content_width(named(fx, "overlay_content")));
    }
}

// ============================================================================
// Portrait: width is scarce, height is spare
// ============================================================================

namespace {

int32_t chart_max_h_token() {
    const char* v = lv_xml_get_const(nullptr, "belt_chart_max_h");
    REQUIRE(v != nullptr);
    return std::atoi(v);
}

} // namespace

TEST_CASE("portrait gives the primary action its own line and caps the chart",
          "[belt][panel][portrait]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", kBpMedium);
    ScopedLayoutSubject portrait("ui_is_portrait", 1);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 0);
    enter_results_with_menu(fx);

    lv_obj_update_layout(fx.view());
    lv_obj_t* tune = named(fx, "btn_tune");
    lv_obj_t* both = named(fx, "btn_test_both");
    CHECK(lv_obj_get_y(tune) > lv_obj_get_y(both));
    CHECK(lv_obj_get_width(tune) > lv_obj_get_width(both));
    CHECK(lv_obj_get_style_max_height(named(fx, "chart_host_results"), LV_PART_MAIN) ==
          chart_max_h_token());

    SECTION("landscape keeps one row and an uncapped chart") {
        portrait.set(0);
        lv_obj_update_layout(fx.view());
        CHECK(lv_obj_get_y(tune) == lv_obj_get_y(both));
        CHECK(lv_obj_get_style_max_height(named(fx, "chart_host_results"), LV_PART_MAIN) !=
              chart_max_h_token());
    }
}

TEST_CASE("portrait START stacks the sketch over the text", "[belt][panel][portrait]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", kBpMedium);
    ScopedLayoutSubject portrait("ui_is_portrait", 1);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 0);
    REQUIRE(fx.state_int("belt_tension_state") ==
            static_cast<int>(BeltTensionPanel::ViewState::START));

    lv_obj_update_layout(fx.view());
    lv_obj_t* sketch = named(fx, "bt_sketch");
    lv_obj_t* text = named(fx, "bt_start_text");
    CHECK(lv_obj_get_y(text) > lv_obj_get_y(sketch));
    CHECK(lv_obj_get_x(text) == lv_obj_get_x(sketch));

    SECTION("landscape puts them side by side") {
        portrait.set(0);
        lv_obj_update_layout(fx.view());
        CHECK(lv_obj_get_x(text) > lv_obj_get_x(sketch));
    }
}

TEST_CASE("portrait shows the full tuning hero, stacked", "[belt][panel][portrait][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    ScopedLayoutSubject bp("ui_breakpoint", kBpMedium);
    ScopedLayoutSubject portrait("ui_is_portrait", 1);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 0);
    enter_adjust(fx);
    REQUIRE(fx.state_int("bt_tune_kind") == 1);

    auto named = [&](const char* name) { return ::named(fx, name); };
    CHECK_FALSE(lv_hidden(named("bt_tune_hero")));
    CHECK(lv_hidden(named("bt_tune_line")));
    CHECK(lv_hidden(named("bt_tune_line_gauge")));
    CHECK(lv_hidden(named("bt_tune_row_stepper")));
    CHECK_FALSE(lv_hidden(named("bt_tune_stepper")));
    // The tappable line is gone, so the swap is a button again.
    CHECK_FALSE(lv_hidden(named("btn_tune_swap")));
    CHECK_FALSE(lv_hidden(named("btn_tune_measure_short")));

    lv_obj_update_layout(fx.view());
    lv_obj_t* left = named("bt_tune_left");
    lv_obj_t* centre = named("bt_tune_centre");
    lv_obj_t* right = named("bt_tune_right");
    CHECK(lv_obj_get_y(centre) == lv_obj_get_y(left));
    CHECK(lv_obj_get_y(right) > lv_obj_get_y(left));
    CHECK(lv_obj_get_y(named("btn_tune_measure_short")) > lv_obj_get_y(named("btn_tune_done")));

    // The warning wraps instead of truncating.
    CHECK(lv_hidden(named("bt_tune_hint")));
    CHECK_FALSE(lv_hidden(named("bt_tune_hint_wrap")));

    SECTION("micro portrait keeps the one-line hero") {
        bp.set(kBpMicro);
        CHECK(lv_hidden(named("bt_tune_hero")));
        CHECK_FALSE(lv_hidden(named("bt_tune_line")));
        CHECK(lv_hidden(named("btn_tune_swap")));
    }
    SECTION("landscape keeps the one-line warning") {
        portrait.set(0);
        CHECK_FALSE(lv_hidden(named("bt_tune_hint")));
        CHECK(lv_hidden(named("bt_tune_hint_wrap")));
    }
}

// ============================================================================
// Settling laps
// ============================================================================

TEST_CASE("the run title reads Settling belts while the laps run", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());

    SECTION("a full check") {
        fx.panel().handle_start_clicked();
        REQUIRE(fx.state_int("belt_tension_state") ==
                static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
        CHECK(fx.text("bt_run_title") == "Settling belts");
        CHECK(fx.state_int("bt_running_path") == 2); // neither path is sweeping
        CHECK(fx.text("bt_slot_0_note") != "sweeping");
        CHECK(fx.text("bt_run_detail").find("elapsed") != std::string::npos);
        CHECK(fx.pump_until_text("bt_run_title", "Measuring Path A"));
        CHECK(fx.state_int("bt_running_path") == 0);
    }
    SECTION("a tuning round, in the sweeping row") {
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(kResults));
        fx.panel().handle_tune_clicked();
        REQUIRE(fx.state_int("belt_tension_state") == kTuning);
        fx.panel().handle_tune_measure_clicked();
        REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
        lv_obj_t* title = lv_obj_find_by_name(fx.view(), "tune_run_title");
        REQUIRE(title != nullptr);
        CHECK(std::string(lv_label_get_text(title)) == "Settling belts");
        REQUIRE(fx.pump_until_text("bt_run_title", "Measuring Path A"));
        CHECK(std::string(lv_label_get_text(title)) == "Measuring Path A");
    }
}

TEST_CASE("Re-test settles once before its single sweep", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_B);
    REQUIRE(fx.pump_until_state(kResults));
    CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full"});
}

TEST_CASE("Confirm settles once before its pair", "[belt][panel][settle][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_peaks_hz(120.0f, 96.0f);
    fx.mock().set_belt_tune_script('B', {101.0f, 101.0f, 101.0f, 106.0f});
    enter_adjust(fx);

    SECTION("a tuning round settles before both sweeps") {
        fx.mock().clear_gcode_script_history();
        step_as_advised(fx);
        tune_round(fx);
        CHECK(run_steps(fx) == std::vector<std::string>{"settle", "narrow", "narrow"});
    }
    SECTION("Confirm") {
        fx.mock().clear_gcode_script_history();
        fx.panel().handle_tune_confirm_clicked();
        REQUIRE(fx.pump_until_state(kResults));
        CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full", "full"});
    }
}

TEST_CASE("a settle that cannot run or is rejected lands on ERROR", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    SECTION("no probe point: refused before anything is sent") {
        fx.mock().set_resonance_probe_points(false);
        REQUIRE(fx.wait_gate_open());
        fx.mock().clear_gcode_script_history();
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
        CHECK(fx.text("bt_error_message") ==
              "[resonance_tester] has no probe_points, so the toolhead has no safe height for "
              "the settling laps.");
        CHECK(fx.mock().gcode_script_history().empty());
    }
    SECTION("the printer rejects the laps") {
        REQUIRE(fx.wait_gate_open());
        fx.mock().force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Move out of range",
                                         kSettleStart);
        fx.mock().clear_gcode_script_history();
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
        CHECK(fx.text("bt_error_message").find("Move out of range") != std::string::npos);
        CHECK(run_steps(fx) == std::vector<std::string>{"settle"});
    }
}

TEST_CASE("Stop during the laps lands on START", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    REQUIRE(fx.wait_gate_open());
    // The laps' ack never arrives, so the run stays in its laps.
    fx.mock().force_next_gcode_dropped_response(kSettleStart);
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    fx.pump_ms(500);
    REQUIRE(run_steps(fx) == std::vector<std::string>{"settle"});
    REQUIRE(fx.text("bt_run_title") == "Settling belts");

    const int stops_before = fx.mock().call_count("printer.emergency_stop");
    fx.panel().handle_stop_clicked();
    fx.pump_ms(200);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(200);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(fx.mock().call_count("printer.emergency_stop") == stops_before + 1);
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(run_steps(fx) == std::vector<std::string>{"settle"});
}

TEST_CASE("a rebuild or a teardown during the laps drops the late ack", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    // One drain runs the probe-point query's answer, which sends the laps; their
    // ack is still queued.
    UpdateQueue::instance().drain();
    REQUIRE(run_steps(fx) == std::vector<std::string>{"settle"});

    SECTION("rebuild") {
        REQUIRE(fx.panel().rebuild());
        UpdateQueue::instance().drain();
        fx.pump_ms(5000);
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));
    }
    SECTION("teardown") {
        // Destruction's order: cleanup(), then the members, the calibrator among them.
        fx.panel().cleanup();
        fx.panel().set_api(nullptr, nullptr);
        fx.pump_ms(5000);
    }
    CHECK(run_steps(fx) == std::vector<std::string>{"settle"});
}

TEST_CASE("the sweep fact line counts the laps, even when the toolhead reports late",
          "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    // At 500 mm/s the laps take seconds: two 130 s sweeps round to 5 min.
    REQUIRE(fx.text("bt_hw_sweep") == "5-135 Hz · about 5 min");

    // A toolhead frame after the [resonance_tester] answer: 10 mm/s max
    // velocity laps at 6 mm/s, minutes instead of seconds.
    fx.set_toolhead_limits(10.0);
    fx.pump_ms(200);

    const auto persona = helix::mock::descriptor(MoonrakerClientMock::PrinterType::VORON_24);
    helix::calibration::BeltSettleInputs in;
    in.x_max = static_cast<float>(persona.axis_max.x);
    in.y_max = static_cast<float>(persona.axis_max.y);
    in.has_xy_limits = true;
    in.probe_point = std::array<float, 3>{
        static_cast<float>((persona.axis_min.x + persona.axis_max.x) / 2.0),
        static_cast<float>((persona.axis_min.y + persona.axis_max.y) / 2.0), 20.0f};
    in.max_velocity_mm_s = 10.0f;
    const auto plan = helix::calibration::plan_belt_settle(in);
    REQUIRE(plan.refusal == helix::calibration::BeltSettleRefusal::NONE);
    const int minutes = static_cast<int>(std::ceil((260.0f + plan.seconds) / 60.0f));
    REQUIRE(minutes != 5); // the laps change the answer
    CHECK(fx.text("bt_hw_sweep") == "5-135 Hz · about " + std::to_string(minutes) + " min");
}

TEST_CASE("a wake re-asks the probe point the suspension expired", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    // The screen sleeps before the query answers; its callback expires.
    fx.panel().on_deactivate(DeactivateReason::Suspended);
    fx.pump_ms(500);
    REQUIRE(run_steps(fx).empty());
    fx.panel().on_activate();
    REQUIRE(fx.pump_until_state(kResults));
    CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full", "full"});
}

TEST_CASE("a tool changer's laps keep to the central half", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    auto& tools = helix::ToolState::instance();
    const auto use_tools = [&tools](nlohmann::json objects) {
        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        tools.deinit_subjects();
        tools.init_subjects(/*register_xml=*/false);
        tools.init_tools(hw);
        UpdateQueue::instance().drain();
    };
    // Later cases expect a single-tool printer, whatever happens here.
    struct RestoreOneTool {
        std::function<void(nlohmann::json)> use;
        ~RestoreOneTool() {
            use({"extruder", "heater_bed"});
        }
    } restore{use_tools};
    use_tools({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3", "extruder", "extruder1",
               "extruder2", "extruder3"});
    REQUIRE(tools.has_multiple_toolheads());
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    const auto axis_max =
        helix::mock::descriptor(MoonrakerClientMock::PrinterType::VORON_24).axis_max;
    // A quarter of the span in from each end.
    const std::string first_corner =
        fmt::format("G1 X{:.1f} Y{:.1f} F18000", axis_max.x / 4.0, axis_max.y / 4.0);
    const auto history = std::vector<std::string>(fx.mock().gcode_script_history());
    CHECK(std::find(history.begin(), history.end(), first_corner) != history.end());
}

// ============================================================================
// Notes, similarity history, ghost scale, labels
// ============================================================================

TEST_CASE("a path measured earlier in this run reads done", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_text("bt_run_title", "Measuring Path B"));
    CHECK(fx.text("bt_slot_0_note") == "done");
    CHECK(fx.text("bt_slot_1_note") == "sweeping");
}

TEST_CASE("ages refresh on RESULTS", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    REQUIRE(fx.text("bt_slot_0_note") == "just now");
    lv_timer_t* timer = fx.panel_note_age_timer();
    REQUIRE(timer != nullptr);
    // The harness runs only timers with a finite repeat count: lend it one
    // firing, then make it periodic again.
    lv_tick_inc(90000);
    lv_timer_set_repeat_count(timer, 2);
    lv_timer_handler_safe();
    lv_timer_set_repeat_count(timer, -1);
    CHECK(fx.text("bt_slot_0_note") == "1 min ago");
    CHECK(fx.text("bt_slot_1_note") == "1 min ago");

    // Only RESULTS shows ages: leaving it stops the timer.
    fx.panel().handle_start_clicked();
    CHECK(fx.panel_note_age_timer() == nullptr);
}

TEST_CASE("was N% sits under the similarity only after a re-test", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    // A fresh check replaced nothing.
    CHECK(fx.text("bt_similarity_was").empty());
    CHECK(fx.state_int("bt_has_similarity_was") == 0);
    lv_obj_t* was = lv_obj_find_by_name(fx.view(), "bt_similarity_was");
    REQUIRE(was != nullptr);
    CHECK(lv_hidden(was));

    const std::string before = fx.text("bt_similarity");
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_B);
    REQUIRE(fx.pump_until_state(kResults));
    CHECK(fx.text("bt_similarity_was") == "was " + before);
    CHECK_FALSE(lv_hidden(was));

    // A fresh Start drops the history again.
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    CHECK(fx.text("bt_similarity_was").empty());
    CHECK(lv_hidden(was));
}

namespace {

/// A capture-shaped CSV with one peak at @p centre_hz, @p height tall over a
/// floor a two-hundredth of it.
void write_peak_csv(const std::string& path, double centre_hz, double height) {
    std::ofstream out(path);
    out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
    for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
        const double df = freq - centre_hz;
        const double psd = height / 200.0 + height / (1.0 + df * df / 100.0);
        out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
            << '\n';
    }
}

float series_peak(ui_frequency_response_chart_t* chart, int id) {
    const auto amps = ui_frequency_response_chart_get_series_amplitudes(chart, id);
    REQUIRE_FALSE(amps.empty());
    return *std::max_element(amps.begin(), amps.end());
}

} // namespace

TEST_CASE("a ghost keeps its own scale", "[belt][panel][chart]") {
    // Path A replays a capture a tenth the height of the mock's own curves, in
    // both its sweeps, so its ghost and its current curve are both quiet.
    const std::string csv_a = "/tmp/helix-belt-quiet-" + std::to_string(getpid()) + ".csv";
    write_peak_csv(csv_a, 104.0, 3.0e3);
    helix::ScopedEnv replay("HELIX_MOCK_BELT_CSV_A", csv_a.c_str());
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
    REQUIRE(fx.pump_until_state(kResults));
    std::remove(csv_a.c_str());

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    REQUIRE(ui_frequency_response_chart_is_series_visible(chart, fx.panel_ghost_id(0)));
    // The ghost on its own scale.
    CHECK(series_peak(chart, fx.panel_ghost_id(0)) == Catch::Approx(100.0f).margin(0.5f));
    // The current curves on one scale, set by the taller (B).
    const float shared = std::max(fx.panel_run_peak(0), fx.panel_run_peak(1));
    const float a_share = 100.0f * fx.panel_run_peak(0) / shared;
    REQUIRE(a_share < 50.0f);
    CHECK(series_peak(chart, fx.panel_series_id(0)) == Catch::Approx(a_share).margin(1.0f));
    CHECK(series_peak(chart, fx.panel_series_id(1)) == Catch::Approx(100.0f).margin(0.5f));
}

TEST_CASE("a tuning ghost keeps its own scale", "[belt][panel][chart][tune]") {
    // B's round-0 sweep replays a quiet capture (loud enough to name a zone);
    // the tuning round's sweep is the mock's own, about three times taller. With B adjusted, the
    // quiet round-0 curve becomes its ghost.
    const std::string csv_b = "/tmp/helix-belt-quiet-b-" + std::to_string(getpid()) + ".csv";
    write_peak_csv(csv_b, 98.0, 9.0e3);
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    {
        helix::ScopedEnv replay("HELIX_MOCK_BELT_CSV_B", csv_b.c_str());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(kResults));
    }
    std::remove(csv_b.c_str());
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == kTuning);
    INFO("phase " << fx.state_int("bt_tune_phase") << " instr " << fx.text("bt_tune_instruction")
                  << " zone " << fx.text("bt_tune_zone"));
    if (fx.state_int("bt_tune_path") != 1) {
        fx.panel().handle_tune_swap_clicked();
    }
    REQUIRE(fx.state_int("bt_tune_path") == 1);
    tune_round(fx);

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const int ghost = fx.panel_tune_ghost_id(1, 0);
    REQUIRE(ui_frequency_response_chart_is_series_visible(chart, ghost));
    CHECK(series_peak(chart, ghost) == Catch::Approx(100.0f).margin(0.5f));
}

namespace {

/// Every label's text under @p obj.
void collect_label_texts(lv_obj_t* obj, std::vector<std::string>& out) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        out.emplace_back(lv_label_get_text(obj));
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        collect_label_texts(lv_obj_get_child(obj, static_cast<int32_t>(i)), out);
    }
}

} // namespace

TEST_CASE("the tuning buttons read in Title Case", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    std::vector<std::string> labels;
    collect_label_texts(fx.view(), labels);
    for (const char* text : {"Good Enough", "Next Zone", "Keep Tuning"}) {
        INFO(text);
        CHECK(std::find(labels.begin(), labels.end(), text) != labels.end());
    }
    for (const char* text : {"Good enough", "Next zone", "Keep tuning", "Tune belts"}) {
        INFO(text);
        CHECK(std::find(labels.begin(), labels.end(), text) == labels.end());
    }
}

TEST_CASE("a genuine 0% earlier similarity still shows", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    fx.panel_set_similarity(0.0f);
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_B);
    REQUIRE(fx.pump_until_state(kResults));
    CHECK(fx.text("bt_similarity_was") == "was 0%");
    CHECK(fx.state_int("bt_has_similarity_was") == 1);
}

// ============================================================================
// Live sweep preview (prestonbrown/helixscreen#1812)
// ============================================================================

namespace {

using helix::calibration::BeltPath;
using helix::calibration::LiveSweepPreview;
using helix::calibration::SweepPoint;

/// Clean points 1 Hz apart over [lo_hz, hi_hz], psd_x = f, y = z = 0.
std::vector<SweepPoint> ramp_points(int lo_hz, int hi_hz) {
    std::vector<SweepPoint> points;
    for (int f = lo_hz; f <= hi_hz; ++f) {
        SweepPoint p;
        p.freq_hz = static_cast<float>(f);
        p.psd_x = static_cast<float>(f);
        p.clean = true;
        points.push_back(p);
    }
    return points;
}

bool pump_until_created(BeltPanelFixture& fx, int want, uint32_t budget_ms = 60000) {
    for (uint32_t t = 0; t < budget_ms && fx.source_log().created < want; t += 100) {
        fx.pump_ms(100);
    }
    return fx.source_log().created == want;
}

bool series_visible(BeltPanelFixture& fx, int series_id) {
    return ui_frequency_response_chart_is_series_visible(fx.panel_chart(), series_id);
}

/// A chip at rest: gravity on z and nothing else, so no drive ever locks.
class QuietStream {
  public:
    /// Push `seconds` of samples into the attached source, then drain.
    void push(helix::calibration::test::SourceLog& log, double seconds) {
        constexpr float kRate = 3053.0f;
        const double until = t_ + seconds;
        while (log.last != nullptr && t_ < until) {
            helix::calibration::AccelBatch batch;
            for (int i = 0; i < 300; ++i) {
                batch.samples.push_back({static_cast<float>(t_), 0.0f, 0.0f, 9810.0f});
                t_ += 1.0 / kRate;
            }
            log.last->push(batch);
            UpdateQueue::instance().drain();
        }
    }

  private:
    double t_ = 0.0;
};

} // namespace

TEST_CASE("a full check draws each path live until its CSV lands", "[belt][panel][preview]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    auto& log = fx.source_log();

    fx.panel().handle_start_clicked();
    // The laps are no sweep: nothing is armed, let alone streaming.
    REQUIRE(fx.text("bt_run_title") == "Settling belts");
    CHECK(log.created == 0);
    CHECK(fx.panel_preview().state() == LiveSweepPreview::State::IDLE);

    // Path A's first console line starts the stream.
    REQUIRE(pump_until_created(fx, 1));
    CHECK(fx.text("bt_run_title") == "Measuring Path A");
    CHECK(log.accel_chip == "adxl345");
    CHECK_FALSE(log.socket_path.empty());
    CHECK(log.plan.freq_start_hz == 5.0f);
    CHECK(log.plan.freq_end_hz == 135.0f);
    CHECK(log.plan.hz_per_sec == 1.0f);

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const int live_a = fx.panel_preview_series_id(0);
    REQUIRE(live_a >= 0);
    CHECK_FALSE(series_visible(fx, live_a));

    // One clean point on each of the axis's 1 Hz slots up to 60 Hz, psd_x = f.
    // The axis is the band, or a curve an earlier run of this panel left.
    const auto [lo, hi] = ui_frequency_response_chart_get_freq_range(chart);
    REQUIRE(hi > 60.0f);
    const size_t slots = static_cast<size_t>(std::lround(hi - lo)) + 1;
    const auto slot_hz = [lo = lo, hi = hi, slots](size_t i) {
        return lo + static_cast<float>(i) * (hi - lo) / static_cast<float>(slots - 1);
    };
    std::vector<SweepPoint> points;
    for (size_t i = 0; i < slots && slot_hz(i) <= 60.0f; ++i) {
        SweepPoint p;
        p.freq_hz = slot_hz(i);
        p.psd_x = slot_hz(i);
        p.clean = true;
        points.push_back(p);
    }
    REQUIRE(points.size() > 10);
    const float tallest = points.back().psd_x;
    fx.panel_preview_points(BeltPath::PATH_A, points);
    CHECK(series_visible(fx, live_a));
    CHECK_FALSE(series_visible(fx, fx.panel_series_id(0)));
    // Each value is a share of the preview's own tallest; nothing past 60 Hz.
    const auto amps = ui_frequency_response_chart_get_series_amplitudes(chart, live_a);
    REQUIRE(amps.size() == slots);
    for (size_t i = 0; i < amps.size(); ++i) {
        INFO("slot " << i << " at " << slot_hz(i) << " Hz");
        if (i < points.size()) {
            CHECK(amps[i] == Catch::Approx(100.0f * slot_hz(i) / tallest));
        } else {
            CHECK(std::isnan(amps[i]));
        }
    }
    const auto live_style = ui_frequency_response_chart_get_series_style(chart, live_a);
    const auto csv_style =
        ui_frequency_response_chart_get_series_style(chart, fx.panel_series_id(0));
    CHECK(live_style.line_width == csv_style.line_width);
    CHECK(live_style.glow == csv_style.glow);
    CHECK(live_style.fill == csv_style.fill);

    // A's CSV replaces its preview the moment it lands; B arms its own stream.
    REQUIRE(pump_until_created(fx, 2));
    CHECK(fx.text("bt_run_title") == "Measuring Path B");
    CHECK_FALSE(series_visible(fx, live_a));
    CHECK(series_visible(fx, fx.panel_series_id(0)));
    CHECK(fx.panel_run(0).has);
    CHECK(log.stopped >= 1);
    CHECK(log.accel_chip == "adxl345");
}

TEST_CASE("a calibration turn drops its previews once its sweeps are in",
          "[belt][panel][preview][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_tune_script('B', {98.3f, 104.0f});
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    auto& log = fx.source_log();
    const int before = log.created;

    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    REQUIRE(pump_until_created(fx, before + 1));
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [lo, hi] = ui_frequency_response_chart_get_freq_range(chart);
    const int live_a = fx.panel_preview_series_id(0);
    fx.panel_preview_points(BeltPath::PATH_A, ramp_points(static_cast<int>(std::ceil(lo)),
                                                          static_cast<int>(std::floor(hi))));
    REQUIRE(series_visible(fx, live_a));

    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        fx.pump_ms(100);
    }
    // The first turn does not register: the calibration stays open, and the
    // finished turn's preview is gone.
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    CHECK_FALSE(series_visible(fx, live_a));
    CHECK_FALSE(series_visible(fx, fx.panel_preview_series_id(1)));
    CHECK(log.stopped == log.created);
}

TEST_CASE("a tuning round keeps each preview until the round is in",
          "[belt][panel][preview][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_tune_script('B', {98.3f, 104.0f});
    enter_tuning(fx);
    calibrate(fx);
    auto& log = fx.source_log();
    const int before = log.created;

    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseSweeping);
    REQUIRE(pump_until_created(fx, before + 1));
    REQUIRE(fx.text("bt_run_title") == "Measuring Path A");
    // The stream ramps over the window Klipper sweeps, floored and ceiled.
    const auto window = fx.panel_run_window();
    REQUIRE(window.has_value());
    CHECK(log.plan.freq_start_hz == std::floor(window->freq_start_hz));
    CHECK(log.plan.freq_end_hz == std::ceil(window->freq_end_hz));

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [lo, hi] = ui_frequency_response_chart_get_freq_range(chart);
    const int live_a = fx.panel_preview_series_id(0);
    const int live_b = fx.panel_preview_series_id(1);
    fx.panel_preview_points(BeltPath::PATH_A, ramp_points(static_cast<int>(std::ceil(lo)),
                                                          static_cast<int>(std::floor(hi))));
    REQUIRE(series_visible(fx, live_a));

    // A's sweep is in and B is streaming: A's preview stays frozen in place.
    REQUIRE(pump_until_created(fx, before + 2));
    REQUIRE(fx.text("bt_run_title") == "Measuring Path B");
    CHECK(series_visible(fx, live_a));
    CHECK_FALSE(series_visible(fx, fx.panel_series_id(0)));
    // B's points sit on the axis's 1 Hz slots, in-plane f, with a z that falls
    // as f rises: the tuning curves are in-plane, so the preview ignores z.
    const size_t slots = static_cast<size_t>(std::lround(hi - lo)) + 1;
    const auto slot_hz = [lo = lo, hi = hi, slots](size_t i) {
        return lo + static_cast<float>(i) * (hi - lo) / static_cast<float>(slots - 1);
    };
    std::vector<SweepPoint> b_points;
    for (size_t i = 0; i < slots; ++i) {
        SweepPoint p;
        p.freq_hz = slot_hz(i);
        p.psd_x = 0.5f * slot_hz(i);
        p.psd_y = 0.5f * slot_hz(i);
        p.psd_z = 1000.0f - slot_hz(i);
        p.clean = true;
        b_points.push_back(p);
    }
    fx.panel_preview_points(BeltPath::PATH_B, b_points);
    CHECK(series_visible(fx, live_b));
    const auto b_amps = ui_frequency_response_chart_get_series_amplitudes(chart, live_b);
    REQUIRE(b_amps.size() == slots);
    for (size_t i = 0; i < slots; ++i) {
        INFO("slot " << i << " at " << slot_hz(i) << " Hz");
        CHECK(b_amps[i] == Catch::Approx(100.0f * slot_hz(i) / slot_hz(slots - 1)));
    }

    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == kPhaseSweeping; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") != kPhaseSweeping);
    CHECK_FALSE(series_visible(fx, live_a));
    CHECK_FALSE(series_visible(fx, live_b));
    CHECK(series_visible(fx, fx.panel_series_id(0)));
    CHECK(series_visible(fx, fx.panel_series_id(1)));
    CHECK(log.stopped == log.created);
}

TEST_CASE("the sweep's last console line ends a stream that never locks",
          "[belt][panel][preview]") {
    using helix::calibration::kPreviewFinishGraceS;
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    auto& log = fx.source_log();
    fx.panel().handle_start_clicked();
    REQUIRE(pump_until_created(fx, 1));
    REQUIRE(log.last != nullptr);
    REQUIRE(fx.panel_preview().state() == LiveSweepPreview::State::STREAMING);
    const float end = log.plan.freq_end_hz;

    // Mid-sweep, a stream with nothing to place keeps running.
    QuietStream quiet;
    fx.panel_sweep_progress(end - 10.0f);
    quiet.push(log, kPreviewFinishGraceS + 1.0);
    CHECK(fx.panel_preview().state() == LiveSweepPreview::State::STREAMING);

    // Klipper's last line can read one below the plan's end. The stream ends
    // a grace later, long before the CSV line that stops it otherwise.
    fx.panel_sweep_progress(end - 1.0f);
    quiet.push(log, kPreviewFinishGraceS - 0.5);
    CHECK(fx.panel_preview().state() == LiveSweepPreview::State::STREAMING);
    quiet.push(log, 1.0);
    CHECK(fx.panel_preview().state() == LiveSweepPreview::State::DONE);
    CHECK(log.stopped == log.created);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
}

TEST_CASE("a Stop or a sweep that dies drops the preview and its stream",
          "[belt][panel][preview]") {
    const bool dies = GENERATE(false, true);
    CAPTURE(dies);
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    if (dies) {
        fx.mock().set_belt_failure(BeltMockFailure::STALL);
    }
    REQUIRE(fx.wait_gate_open());
    auto& log = fx.source_log();

    fx.panel().handle_start_clicked();
    REQUIRE(pump_until_created(fx, 1));
    fx.panel_preview_points(BeltPath::PATH_A, ramp_points(5, 40));
    const int live_a = fx.panel_preview_series_id(0);
    REQUIRE(series_visible(fx, live_a));

    if (dies) {
        fx.pump_ms(15000);
        fx.pump_ms(BeltTensionPanel::kStallTimeoutMs + 2000);
        REQUIRE(fx.state_int("belt_tension_state") ==
                static_cast<int>(BeltTensionPanel::ViewState::ERROR));
    } else {
        fx.panel().handle_stop_clicked();
        fx.pump_ms(200);
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
        fx.pump_ms(200);
        REQUIRE(fx.state_int("belt_tension_state") ==
                static_cast<int>(BeltTensionPanel::ViewState::START));
        CHECK_FALSE(series_visible(fx, fx.panel_preview_series_id(1)));
    }
    CHECK(log.stopped == log.created);
    CHECK(fx.panel_preview().state() == LiveSweepPreview::State::IDLE);
    CHECK_FALSE(series_visible(fx, live_a));
}

TEST_CASE("no chart means no stream", "[belt][panel][preview]") {
    BeltPanelFixture fx(helix::PlatformTier::EMBEDDED);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    CHECK(fx.panel_chart() == nullptr);
    CHECK(fx.source_log().created == 0);
}

TEST_CASE("an unreachable socket sweeps on the CSV alone", "[belt][panel][preview]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.text("bt_run_title") == "Settling belts");
    fx.panel_set_socket_reachable(false);
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    CHECK(fx.source_log().created == 0);
    CHECK(fx.panel_run(0).has);
    CHECK(fx.panel_run(1).has);
}

TEST_CASE("the screensaver keeps the stream; leaving stops it", "[belt][panel][preview]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    auto& log = fx.source_log();
    fx.panel().handle_start_clicked();
    REQUIRE(pump_until_created(fx, 1));
    REQUIRE(log.last != nullptr);

    fx.panel().on_deactivate(DeactivateReason::Suspended);
    UpdateQueue::instance().drain();
    CHECK(log.stopped == 0);
    REQUIRE(log.last != nullptr);
    CHECK(log.last->running());
    CHECK(fx.panel_preview().state() == LiveSweepPreview::State::STREAMING);

    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    UpdateQueue::instance().drain();
    CHECK(log.stopped == 1);
    CHECK(fx.panel_preview().state() == LiveSweepPreview::State::IDLE);
}

// ============================================================================
// Which belt is which: names and Identify
// ============================================================================

namespace {

/// The STEPPER_BUZZ lines the mock has handled, in order.
std::vector<std::string> buzz_lines(BeltPanelFixture& fx) {
    std::vector<std::string> out;
    for (const auto& g : std::vector<std::string>(fx.mock().gcode_script_history())) {
        if (g.find("STEPPER_BUZZ") != std::string::npos) {
            out.push_back(g);
        }
    }
    return out;
}

void set_layout(helix::calibration::BeltLayout layout) {
    helix::SettingsManager::instance().set_belt_layout(static_cast<int>(layout));
    UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE("a saved belt layout renames the results and the tuning advice",
          "[belt][panel][names][tune]") {
    BeltPanelFixture fx; // mock defaults: A=110, B=98
    get_printer_state().set_printer_type_sync("Voron 2.4");
    CHECK(fx.text("bt_slot_0_name") == "Path A");

    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    REQUIRE(fx.text("bt_unpaired").find("Only on Path A: ") != std::string::npos);

    set_layout(helix::calibration::BeltLayout::A_LEFT_B_RIGHT);
    CHECK(fx.text("bt_slot_0_name") == "Left belt");
    CHECK(fx.text("bt_slot_1_name") == "Right belt");
    CHECK(fx.text("bt_unpaired").find("Only on the left belt: ") != std::string::npos);
    CHECK(fx.text("bt_unpaired").find("Only on the right belt: ") != std::string::npos);

    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == kTuning);
    CHECK(fx.text("bt_tune_instruction") == "Tighten the right belt one full turn");
    CHECK(fx.text("bt_tune_gap_caption") == "Right belt is looser");
    CHECK(fx.text("bt_tune_tensioner") == "Tensioner: Right belt");

    // A change while the advice is on screen renames it at once.
    set_layout(helix::calibration::BeltLayout::A_UPPER_B_LOWER);
    CHECK(fx.text("bt_tune_instruction") == "Tighten the lower belt one full turn");
    CHECK(fx.text("bt_tune_tensioner") == "Tensioner: Lower belt");
}

TEST_CASE("the measuring title names the path by its saved name", "[belt][panel][names]") {
    BeltPanelFixture fx;
    set_layout(helix::calibration::BeltLayout::A_RIGHT_B_LEFT);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    CHECK(fx.pump_until_text("bt_run_title", "Measuring the right belt"));
    CHECK(fx.pump_until_text("bt_run_title", "Measuring the left belt"));
}

TEST_CASE("Identify buzzes the motor that drives each path", "[belt][panel][names][identify]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_identify_clicked(helix::calibration::BeltPath::PATH_A);
    fx.pump_ms(500);
    fx.panel().handle_identify_clicked(helix::calibration::BeltPath::PATH_B);
    fx.pump_ms(500);
    CHECK(buzz_lines(fx) == std::vector<std::string>{"STEPPER_BUZZ STEPPER=stepper_y",
                                                     "STEPPER_BUZZ STEPPER=stepper_x"});
    CHECK(fx.state_int("bt_identify_state") == 0);
    CHECK(fx.state_int("bt_can_start") == 1);
}

TEST_CASE("one buzz at a time, and it holds Start", "[belt][panel][names][identify][gating]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();
    fx.mock().defer_next("printer.gcode.script");

    fx.panel().handle_identify_clicked(helix::calibration::BeltPath::PATH_A);
    UpdateQueue::instance().drain();
    // In flight: Identify (bound to bt_can_start) and Start are both shut.
    CHECK(fx.state_int("bt_can_start") == 0);
    CHECK(fx.state_int("bt_identify_state") == 1);
    CHECK(fx.text("bt_identify_status") == "Buzzing the motor of Path A");
    // START says why it is shut instead of a bare "Ready".
    CHECK(fx.text("bt_gate_message") == "Wait for the motor to stop");
    fx.panel().handle_identify_clicked(helix::calibration::BeltPath::PATH_B);
    fx.panel().handle_start_clicked();
    UpdateQueue::instance().drain();
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));

    fx.mock().fire_deferred("printer.gcode.script");
    fx.pump_ms(500);
    CHECK(buzz_lines(fx) == std::vector<std::string>{"STEPPER_BUZZ STEPPER=stepper_y"});
    CHECK(fx.state_int("bt_can_start") == 1);
    CHECK(fx.state_int("bt_identify_state") == 0);
}

TEST_CASE("a refused buzz says why and frees Identify", "[belt][panel][names][identify]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().defer_next("printer.gcode.script");
    fx.panel().handle_identify_clicked(helix::calibration::BeltPath::PATH_B);
    fx.mock().fire_deferred_error("printer.gcode.script",
                                  MoonrakerError::unknown("Unknown stepper stepper_x"));
    fx.pump_ms(500);
    CHECK(fx.state_int("bt_identify_state") == 2);
    CHECK(fx.text("bt_identify_status").find("Unknown stepper stepper_x") != std::string::npos);
    CHECK(fx.state_int("bt_can_start") == 1);
    // A refused buzz is not a failed check: the panel stays where it was.
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
}

TEST_CASE("Identify is refused while a sweep runs or the gate is shut",
          "[belt][panel][names][identify][gating]") {
    SECTION("mid-sweep") {
        BeltPanelFixture fx;
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RUNNING)));
        fx.panel().handle_identify_clicked(helix::calibration::BeltPath::PATH_A);
        fx.pump_ms(200);
        CHECK(buzz_lines(fx).empty());
    }
    SECTION("klippy not ready") {
        BeltPanelFixture fx;
        REQUIRE(fx.wait_gate_open());
        fx.set_klippy_ready(false);
        fx.mock().clear_gcode_script_history();
        fx.panel().handle_identify_clicked(helix::calibration::BeltPath::PATH_A);
        fx.pump_ms(200);
        CHECK(buzz_lines(fx).empty());
    }
}

TEST_CASE("the names dialog sets the layout and closes with the panel", "[belt][panel][names]") {
    BeltPanelFixture fx;
    REQUIRE(fx.register_component("belt_names_modal"));
    fx.panel().handle_names_clicked();
    UpdateQueue::instance().drain();
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* lower_a = lv_obj_find_by_name(screen, "btn_layout_4");
    REQUIRE(lower_a != nullptr);
    lv_obj_send_event(lower_a, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    CHECK(helix::SettingsManager::instance().get_belt_layout() == 4);
    CHECK(fx.text("bt_slot_0_name") == "Lower belt");
    // The body keeps its height after a choice: the layout buttons and the
    // Identify rows stay on screen under the heading.
    lv_obj_update_layout(screen);
    lv_obj_t* body = lv_obj_find_by_name(screen, "belt_names_content");
    REQUIRE(body != nullptr);
    CHECK(lv_obj_get_height(body) > 3 * lv_obj_get_height(lower_a));
    for (const char* name : {"btn_identify_a", "btn_identify_b", "btn_layout_0", "btn_layout_4"}) {
        INFO(name);
        lv_obj_t* obj = lv_obj_find_by_name(screen, name);
        REQUIRE(obj != nullptr);
        CHECK(lv_obj_get_height(obj) > 0);
    }

    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    fx.pump_ms(1000);
    CHECK(lv_obj_find_by_name(screen, "btn_layout_4") == nullptr);
}

TEST_CASE("the tensioner line opens the names dialog without ending the tuning session",
          "[belt][panel][names][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    REQUIRE(fx.register_component("belt_names_modal"));
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    const auto* session = fx.panel_session();
    REQUIRE(session != nullptr);

    lv_obj_t* row = named(fx, "bt_tune_tensioner_row");
    REQUIRE_FALSE(lv_hidden(row));
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    lv_obj_t* screen = lv_screen_active();
    REQUIRE(lv_obj_find_by_name(screen, "btn_identify_b") != nullptr);

    // Identify works mid-session: the adjusted path's motor buzzes.
    fx.mock().clear_gcode_script_history();
    lv_obj_send_event(lv_obj_find_by_name(screen, "btn_identify_b"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(500);
    CHECK(buzz_lines(fx) == std::vector<std::string>{"STEPPER_BUZZ STEPPER=stepper_x"});

    lv_obj_send_event(lv_obj_find_by_name(screen, "btn_names_done"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(1000);
    CHECK(lv_obj_find_by_name(screen, "btn_identify_b") == nullptr);
    CHECK(fx.state_int("belt_tension_state") == kTuning);
    CHECK(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    CHECK(fx.panel_session() == session);
}

TEST_CASE("A as the right belt puts Path B in the left slot and its Re-test",
          "[belt][panel][names]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    CHECK(fx.text("bt_slot_0_name") == "Path A");
    CHECK(fx.state_int("bt_slots_swapped") == 0);

    set_layout(helix::calibration::BeltLayout::A_RIGHT_B_LEFT);
    CHECK(fx.text("bt_slot_0_name") == "Left belt");
    CHECK(fx.text("bt_slot_1_name") == "Right belt");
    CHECK(fx.state_int("bt_slots_swapped") == 1);
    CHECK(fx.text("bt_slot_0_note") == "just now");
    CHECK(fx.text("bt_unpaired").rfind("Only on the left belt: ", 0) == 0);

    // The left Re-test button measures the left belt, which is Path B.
    lv_obj_send_event(named(fx, "btn_retest_slot_0"), LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    CHECK(fx.pump_until_text("bt_run_title", "Re-measuring the left belt"));
    CHECK(fx.state_int("bt_running_path") == 1);
    CHECK(fx.text("bt_slot_0_note") == "sweeping");
    CHECK(fx.text("bt_slot_1_note") != "sweeping");
}

TEST_CASE("compact tuning's pencil opens the names dialog instead of swapping",
          "[belt][panel][names][tune][compact]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    ScopedLayoutSubject bp("ui_breakpoint", kBpMicro);
    REQUIRE(fx.register_component("belt_names_modal"));
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == kPhaseCalibrate);
    const auto* session = fx.panel_session();
    const int adjusted = fx.state_int("bt_tune_path");
    // On compact layouts the line is the only tuning header; the full one,
    // with its tensioner row, is hidden.
    REQUIRE_FALSE(lv_hidden(named(fx, "bt_tune_line")));
    REQUIRE(lv_hidden(named(fx, "bt_tune_hero")));

    lv_obj_t* pencil = named(fx, "bt_tune_line_names");
    REQUIRE_FALSE(lv_hidden(pencil));
    lv_obj_send_event(pencil, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    lv_obj_t* screen = lv_screen_active();
    CHECK(lv_obj_find_by_name(screen, "btn_identify_a") != nullptr);
    CHECK(fx.state_int("bt_tune_path") == adjusted);
    CHECK(fx.panel_session() == session);
    CHECK(fx.state_int("belt_tension_state") == kTuning);
}

TEST_CASE("the peak pairs read in header-slot order", "[belt][panel][names]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_peaks_hz(104.0f, 101.0f);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(kResults));
    INFO("facts: " << fx.text("bt_facts"));
    REQUIRE(fx.text("bt_facts").find("104/101") != std::string::npos);

    set_layout(helix::calibration::BeltLayout::A_RIGHT_B_LEFT);
    INFO("facts after the swap: " << fx.text("bt_facts"));
    CHECK(fx.text("bt_facts").find("101/104") != std::string::npos);
    CHECK(fx.text("bt_facts").find("104/101") == std::string::npos);
}

namespace {

struct DrawnLabel {
    std::string text;
    uint32_t color;
};

/// The labels a widget draws on one refresh, read from its draw tasks.
std::vector<DrawnLabel> drawn_labels(lv_obj_t* obj) {
    std::vector<DrawnLabel> out;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t* e) {
            auto* labels = static_cast<std::vector<DrawnLabel>*>(lv_event_get_user_data(e));
            const lv_draw_label_dsc_t* dsc = lv_draw_task_get_label_dsc(lv_event_get_draw_task(e));
            if (dsc && dsc->text) {
                labels->push_back({dsc->text, lv_color_to_u32(dsc->color) & 0xFFFFFF});
            }
        },
        LV_EVENT_DRAW_TASK_ADDED, &out);
    lv_obj_invalidate(obj);
    lv_refr_now(nullptr);
    lv_obj_remove_event_cb_with_user_data(obj, nullptr, &out);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    return out;
}

} // namespace

TEST_CASE("the START sketch labels each diagonal with its path's name and color",
          "[belt][panel][names]") {
    BeltPanelFixture fx;
    set_layout(helix::calibration::BeltLayout::A_LEFT_B_RIGHT);
    lv_obj_t* sketch = named(fx, "bt_sketch");
    // Created but never pushed, the overlay is still hidden; show it to draw it.
    lv_obj_remove_flag(fx.view(), LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(lv_obj_get_screen(sketch));
    INFO("sketch " << lv_obj_get_width(sketch) << "x" << lv_obj_get_height(sketch)
                   << " on active screen " << (lv_obj_get_screen(sketch) == lv_screen_active())
                   << " visible " << lv_obj_is_visible(sketch));
    const auto labels = drawn_labels(sketch);
    const auto color_of = [&](const std::string& text) -> std::optional<uint32_t> {
        for (const auto& label : labels) {
            if (label.text == text) {
                return label.color;
            }
        }
        return std::nullopt;
    };
    INFO("labels drawn: " << labels.size());
    REQUIRE(color_of("BACK"));
    REQUIRE(color_of("Left belt"));
    REQUIRE(color_of("Right belt"));
    CHECK(*color_of("Left belt") ==
          (lv_color_to_u32(theme_manager_get_color("belt_path_a")) & 0xFFFFFF));
    CHECK(*color_of("Right belt") ==
          (lv_color_to_u32(theme_manager_get_color("belt_path_b")) & 0xFFFFFF));
}

TEST_CASE("the path names are filled in from the moment the subjects exist",
          "[belt][panel][names]") {
    BeltPanelFixture fx;
    set_layout(helix::calibration::BeltLayout::A_UPPER_B_LOWER);
    // A live panel's subjects can be rebuilt without an activation (the
    // shutdown registry, a printer switch's re-init).
    fx.panel().deinit_subjects();
    fx.panel().init_subjects();
    CHECK(fx.text("bt_slot_0_name") == "Upper belt");
    CHECK(fx.text("bt_slot_1_name") == "Lower belt");
}

TEST_CASE("a panel that has been left stops following the layout", "[belt][panel][names]") {
    BeltPanelFixture fx;
    REQUIRE(fx.text("bt_slot_0_name") == "Path A");
    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    UpdateQueue::instance().drain();
    // A layout change off-screen does no work on a panel nobody sees; the
    // next activation reads the layout afresh.
    set_layout(helix::calibration::BeltLayout::A_LEFT_B_RIGHT);
    CHECK(fx.text("bt_slot_0_name") == "Path A");
    fx.panel().on_activate();
    UpdateQueue::instance().drain();
    CHECK(fx.text("bt_slot_0_name") == "Left belt");
}
