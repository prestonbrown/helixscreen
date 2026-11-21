// SPDX-License-Identifier: GPL-3.0-or-later

// The guided run's real finish against the mock printer: Save all is one
// SAVE_CONFIG through SaveConfigWatch, and the history is written only once
// Klipper is back from the restart that save causes; Discard all is one host
// RESTART, which drops what the steps staged, and writes nothing.

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "calibration_history_store.h"
#include "calibration_run.h"
#include "calibration_run_steps.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>
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

    void start(std::vector<std::unique_ptr<CalibrationStep>> steps) {
        run = std::make_unique<CalibrationRun>(*api, std::move(steps), nullptr);
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
    const auto& sent_scripts = client.gcode_script_history();
    CHECK(std::any_of(sent_scripts.begin(), sent_scripts.end(), [](const std::string& g) {
        return g.find("PID_CALIBRATE") != std::string::npos ||
               g.find("MPC_CALIBRATE") != std::string::npos;
    }));
    CHECK(std::none_of(sent_scripts.begin(), sent_scripts.end(), [](const std::string& g) {
        return g.find("SAVE_CONFIG") != std::string::npos;
    }));
}
