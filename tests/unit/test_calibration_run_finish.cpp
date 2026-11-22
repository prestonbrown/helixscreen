// SPDX-License-Identifier: GPL-3.0-or-later

// The guided run's real finish against the mock printer: Save all is one
// SAVE_CONFIG through SaveConfigWatch, and the history is written only once
// Klipper is back from the restart that save causes; Discard all is one host
// RESTART, which drops what the steps staged, and writes nothing.

#include "ui_calibration_run_overlay.h"
#include "ui_panel_bed_mesh.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "calibration_history_store.h"
#include "calibration_run.h"
#include "calibration_run_steps.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::calibration;

namespace {

/// A step that finishes as soon as it starts, with what a heater step measures.
class InstantStep : public CalibrationStep {
  public:
    InstantStep() {
        info_.kind = StepKind::Heaters;
        info_.id = "heaters";
        info_.name = "Heaters";
    }
    const StepInfo& info() const override {
        return info_;
    }
    void start(StepContext& ctx) override {
        ctx.done(StepOutcome::finished({"Nozzle PID", {{"kp", 22.1}}}));
    }
    void abort() override {}

  private:
    StepInfo info_;
};

class FinishFixture : public LVGLTestFixture {
  public:
    FinishFixture() : client(MoonrakerClientMock::PrinterType::VORON_24, 100.0) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        // SaveConfigWatch observes the process-wide PrinterState's klippy subject.
        get_printer_state().init_subjects(false);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(client, state);
        CalibrationHistory::instance().clear();
        finisher = std::make_unique<MoonrakerRunFinisher>(*api);
    }

    void start(std::vector<std::unique_ptr<CalibrationStep>> steps,
               std::function<void()> on_change = nullptr) {
        run = std::make_unique<CalibrationRun>(*api, std::move(steps), std::move(on_change));
        run->start();
    }

    /// A finished run waiting for Save or Discard.
    void finished_run() {
        std::vector<std::unique_ptr<CalibrationStep>> steps;
        steps.push_back(std::make_unique<InstantStep>());
        start(std::move(steps));
        REQUIRE(run->can_save());
        client.clear_gcode_script_history();
    }
    ~FinishFixture() override {
        run.reset();
        finisher.reset();
        CalibrationHistory::instance().clear();
        client.stop_temperature_simulation();
        client.disconnect();
        api.reset();
    }

    size_t sent(const std::string& script) const {
        const auto& h = client.gcode_script_history();
        return static_cast<size_t>(std::count(h.begin(), h.end(), script));
    }

    MoonrakerClientMock client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<CalibrationRun> run;
    std::unique_ptr<MoonrakerRunFinisher> finisher;
};

} // namespace

TEST_CASE_METHOD(FinishFixture,
                 "save all is one SAVE_CONFIG, and the history waits for Klipper to come back",
                 "[calibration_run][save_config_policy]") {
    finished_run();
    run->save_all(*finisher);
    REQUIRE(wait_until([&] { return sent("SAVE_CONFIG") == 1; }, 3000));
    process_lvgl(50);
    // The save's reply is dropped by its restart: nothing is known yet.
    CHECK(run->phase() == RunPhase::Saving);
    CHECK(CalibrationHistory::instance().entry("heaters") == nullptr);

    get_printer_state().set_klippy_state_sync(KlippyState::STARTUP);
    get_printer_state().set_klippy_state_sync(KlippyState::READY);
    REQUIRE(wait_until([&] { return run->phase() == RunPhase::Saved; }, 3000));

    const HistoryEntry* saved = CalibrationHistory::instance().entry("heaters");
    REQUIRE(saved != nullptr);
    CHECK(saved->summary == "Nozzle PID");
    CHECK(sent("SAVE_CONFIG") == 1);
}

TEST_CASE_METHOD(FinishFixture, "discard all restarts Klipper, dropping what was staged",
                 "[calibration_run][expectedrestart]") {
    finished_run();
    // What a step leaves behind: a value staged for the next SAVE_CONFIG.
    client.send_jsonrpc(
        "printer.gcode.script", nlohmann::json{{"script", "Z_OFFSET_APPLY_PROBE"}},
        [](const nlohmann::json&) {}, [](const MoonrakerError&) {});
    REQUIRE(client.save_config_pending());

    run->discard_all(*finisher);
    REQUIRE(wait_until([&] { return run->phase() == RunPhase::Discarded; }, 3000));

    CHECK_FALSE(client.save_config_pending());
    CHECK(sent("SAVE_CONFIG") == 0);
    CHECK(CalibrationHistory::instance().entry("heaters") == nullptr);
}

TEST_CASE_METHOD(FinishFixture, "the heater and input shaper steps measure without saving",
                 "[calibration_run]") {
    StepInfo heaters;
    heaters.kind = StepKind::Heaters;
    heaters.id = "heaters";
    heaters.name = "Heaters";
    StepInfo shaper;
    shaper.kind = StepKind::InputShaper;
    shaper.id = "input_shaper";
    shaper.name = "Input shaper";
    std::vector<std::unique_ptr<CalibrationStep>> steps;
    steps.push_back(make_run_step(heaters));
    steps.push_back(make_run_step(shaper));
    REQUIRE(steps[0]);
    REQUIRE(steps[1]);
    client.clear_gcode_script_history();
    start(std::move(steps));

    REQUIRE(wait_until([&] { return run->phase() == RunPhase::Results; }, 30000));
    for (const auto& e : run->entries()) {
        INFO(e.step->info().id << ": " << e.message);
        CHECK(e.status == StepStatus::Finished);
    }
    // The run's step list and results table read the method per heater; the
    // tuned numbers stay in the summary the history keeps.
    CHECK(run->entries()[0].result.brief == "nozzle PID");
    CHECK(run->entries()[0].result.summary.find("Nozzle ") == 0);
    const auto& sent_scripts = client.gcode_script_history();
    CHECK(std::any_of(sent_scripts.begin(), sent_scripts.end(), [](const std::string& g) {
        return g.find("PID_CALIBRATE") != std::string::npos ||
               g.find("MPC_CALIBRATE") != std::string::npos;
    }));
    CHECK(std::none_of(sent_scripts.begin(), sent_scripts.end(), [](const std::string& g) {
        return g.find("SAVE_CONFIG") != std::string::npos;
    }));
}

TEST_CASE_METHOD(FinishFixture, "the heater and input shaper steps tell the live card what runs",
                 "[calibration_run][calibration_live]") {
    StepInfo heaters;
    heaters.kind = StepKind::Heaters;
    heaters.id = "heaters";
    heaters.name = "Heaters";
    StepInfo shaper;
    shaper.kind = StepKind::InputShaper;
    shaper.id = "input_shaper";
    shaper.name = "Input shaper";
    std::vector<std::unique_ptr<CalibrationStep>> steps;
    steps.push_back(make_run_step(heaters));
    steps.push_back(make_run_step(shaper));
    std::map<std::string, int> heater_targets;
    std::set<char> swept_axes;
    start(std::move(steps), [&] {
        const int c = run ? run->current() : -1;
        if (c < 0) {
            return;
        }
        const auto& live = run->entries()[static_cast<size_t>(c)].progress.live;
        if (!live.heater.empty()) {
            heater_targets[live.heater] = live.target_c;
        }
        if (live.sweep_hz > 0.0f) {
            swept_axes.insert(live.axis);
        }
    });

    REQUIRE(wait_until([&] { return run->phase() == RunPhase::Results; }, 30000));
    REQUIRE(heater_targets.count("extruder") == 1);
    CHECK(heater_targets["extruder"] == kRunNozzleTempC);
    if (heater_targets.count("heater_bed")) {
        CHECK(heater_targets["heater_bed"] == kRunBedTempC);
    }
    CHECK(swept_axes == std::set<char>{'X', 'Y'});
}

namespace {
bool sent_any(const MoonrakerClientMock& client, const char* word) {
    const auto& h = client.gcode_script_history();
    return std::any_of(h.begin(), h.end(),
                       [word](const std::string& g) { return g.find(word) != std::string::npos; });
}
} // namespace

TEST_CASE_METHOD(FinishFixture, "stopping the heaters before their calibration runs sends no M112",
                 "[calibration_run]") {
    StepInfo heaters;
    heaters.kind = StepKind::Heaters;
    heaters.id = "heaters";
    heaters.name = "Heaters";
    std::vector<std::unique_ptr<CalibrationStep>> steps;
    steps.push_back(make_run_step(heaters));
    client.clear_gcode_script_history();
    start(std::move(steps)); // the control-type query is in flight
    REQUIRE(run->phase() == RunPhase::Running);

    run->stop();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    process_lvgl(50);
    CHECK(client.get_klippy_state() == MoonrakerClientMock::KlippyState::READY);
    CHECK_FALSE(sent_any(client, "PID_CALIBRATE"));
    CHECK_FALSE(sent_any(client, "MPC_CALIBRATE"));
    CHECK(run->entries()[0].status == StepStatus::Aborted);
}

TEST_CASE_METHOD(FinishFixture,
                 "stopping a bed mesh while it prepares sends no M112 and turns its heat off",
                 "[calibration_run]") {
    StepInfo mesh;
    mesh.kind = StepKind::BedMesh;
    mesh.id = "bed_mesh";
    mesh.name = "Bed mesh";
    std::vector<std::unique_ptr<CalibrationStep>> steps;
    steps.push_back(make_run_step(mesh));
    client.clear_gcode_script_history();
    start(std::move(steps));
    // One pass: the stored meshes are read and the preheat begins.
    helix::ui::UpdateQueue::instance().drain();
    auto& panel = get_global_bed_mesh_panel();
    REQUIRE(panel.holds_owned_heat());
    REQUIRE_FALSE(panel.calibration_command_sent());

    // The heaters go off at once only with the printer connected.
    lv_subject_t* link = get_printer_state().network_state().get_printer_connection_state_subject();
    const int was = lv_subject_get_int(link);
    lv_subject_set_int(link, static_cast<int>(ConnectionState::CONNECTED));
    run->stop();
    CHECK_FALSE(panel.holds_owned_heat());
    lv_subject_set_int(link, was);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    process_lvgl(50);
    CHECK(client.get_klippy_state() == MoonrakerClientMock::KlippyState::READY);
    CHECK_FALSE(sent_any(client, "BED_MESH_CALIBRATE"));
    CHECK(run->entries()[0].status == StepStatus::Aborted);
    // The panel is process-wide; it must not keep this fixture's API.
    panel.set_api(nullptr);
}

TEST_CASE("the results button promises a restart unless Klipper said the save skips it",
          "[calibration_run]") {
    using helix::ui::results_primary_label;
    CHECK(results_primary_label(true, false, true) == "Save and restart Klipper");
    CHECK(results_primary_label(true, true, true) == "Save and restart Klipper");
    CHECK(results_primary_label(true, true, false) == "Save");
    CHECK(results_primary_label(false, true, false) == "Done");
    CHECK(results_primary_label(false, false, true) == "Done");
}

TEST_CASE("the stop confirmation warns of a restart only when the stop restarts",
          "[calibration_run]") {
    using Effect = CalibrationRun::StopEffect;
    using helix::ui::stop_confirm_message;
    const std::string quiet = stop_confirm_message(Effect::EndsStep);
    const std::string restarts = stop_confirm_message(Effect::Restarts);
    const std::string drops = stop_confirm_message(Effect::RestartDropsResults);
    CHECK(quiet.find("restarts") == std::string::npos);
    CHECK(restarts.find("restarts") != std::string::npos);
    CHECK(restarts.find("drops") == std::string::npos);
    CHECK(drops.find("restarts") != std::string::npos);
    CHECK(drops.find("drops the results") != std::string::npos);
}
