// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_pa_nonlinear.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::ui::PaNonlinearOverlay;
using nlohmann::json;

namespace {

struct NonlinearFixture : LVGLTestFixture {
    // Firmware with selectable models, and its tower module unless a case
    // says otherwise. Declared before the client, which reads them.
    helix::ScopedEnv pa_model_env;
    helix::ScopedEnv pa_test_env;
    MoonrakerClientMock client;
    MoonrakerAPI api;
    PaNonlinearOverlay overlay;

    explicit NonlinearFixture(bool tower = true)
        : pa_model_env("HELIX_MOCK_PA_MODEL", "linear"),
          pa_test_env("HELIX_MOCK_PA_TEST", tower ? "1" : "0"), api(client, get_printer_state()) {
        auto& state = get_printer_state();
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        PrinterDiscovery hw;
        json objects = json::array({"extruder", "heater_bed", "print_stats"});
        if (tower) {
            objects.push_back("pa_test");
        }
        hw.parse_objects(objects);
        state.set_hardware(hw);
        state.temperature_state().init_extruders({"extruder"});
        client.connect("ws://mock/websocket", []() {}, []() {});
        overlay.init_subjects();
        overlay.set_api(&api);
        overlay.set_raise_on_start_for_test(false);
        status({{"extruder", {{"pressure_advance_model", "linear"}, {"pressure_advance", 0.04}}}});
    }
    ~NonlinearFixture() override {
        overlay.cleanup();
        status({{"print_stats", {{"state", "standby"}, {"filename", ""}}}});
        client.disconnect();
    }

    void status(const json& s) {
        get_printer_state().update_from_status(s);
        helix::ui::UpdateQueue::instance().drain();
    }

    pa::TestPrintParams params() const {
        pa::TestPrintParams p;
        p.heater = "extruder";
        p.nozzle_temp = 215;
        p.bed_temp = 60;
        p.nozzle_diameter_mm = 0.4f;
        p.material = "PLA";
        return p;
    }

    void tower(const char* state) {
        status({{"print_stats",
                 {{"state", state}, {"filename", PaNonlinearOverlay::kTowerPrintName}}}});
    }

    /// The scripts sent since @p from, joined.
    std::string sent_since(size_t from) const {
        const auto& h = client.gcode_script_history();
        std::string out;
        for (size_t i = from; i < h.size(); ++i) {
            out += h[i] + "\n";
        }
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a pass readies the machine, switches the model and starts the "
                 "firmware tower",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    REQUIRE(overlay.stage() == PaNonlinearOverlay::READY);
    const size_t before = client.gcode_script_history().size();
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);

    const std::string sent = sent_since(before);
    INFO(sent);
    const auto at = [&sent](const char* needle) { return sent.find(needle); };
    // Heated to exactly the target the tower checks, then the model, then the
    // sweep, and the tower last: PRINT_PA_TOWER returns as soon as it starts.
    REQUIRE(at("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215") != std::string::npos);
    REQUIRE(at("MODEL=tanh") != std::string::npos);
    REQUIRE(at("PARAMETER=OFFSET") != std::string::npos);
    REQUIRE(at("PRINT_PA_TOWER NOZZLE=0.40 TARGET_TEMP=215") != std::string::npos);
    CHECK(at("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215") < at("MODEL=tanh"));
    CHECK(at("MODEL=tanh") < at("PARAMETER=OFFSET"));
    CHECK(at("PARAMETER=OFFSET") < at("PRINT_PA_TOWER"));
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: the tower's own print drives the stages and the judgement "
                 "advances the procedure",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();

    // Another file printing is not the tower.
    status({{"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}});
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);
    status({{"print_stats", {{"state", "standby"}, {"filename", ""}}}});

    tower("printing");
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PRINTING);
    const size_t before_end = client.gcode_script_history().size();
    tower("complete");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::JUDGE);
    // The sweep's last value is replaced by the procedure's before the machine
    // is released.
    const std::string ending = sent_since(before_end);
    INFO(ending);
    CHECK(ending.find("MODEL=tanh") != std::string::npos);
    // The firmware's sweep outlives the tower; the ending replaces it.
    CHECK(ending.find("TUNING_TOWER COMMAND=G4") != std::string::npos);
    CHECK(ending.find("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0") != std::string::npos);

    // Pass 1 judges the left side alone: the height is the answer.
    overlay.adjust_height_for_test(5); // 25 -> 30 mm
    overlay.judge_for_test(1);
    helix::ui::UpdateQueue::instance().drain();
    const auto* proc = overlay.procedure_for_test();
    REQUIRE(proc != nullptr);
    CHECK(proc->values().nonlinear_offset == Catch::Approx(0.30f));
    CHECK(proc->pass_number() == 2);
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
    CHECK(client.last_send_script().find("OFFSET=0.300000") != std::string::npos);
}

TEST_CASE_METHOD(NonlinearFixture, "nonlinear tuner: a tower that errors goes back to the pass",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    tower("printing");
    tower("error");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
    REQUIRE(overlay.procedure_for_test() != nullptr);
    CHECK(overlay.procedure_for_test()->pass_number() == 1);
}

TEST_CASE_METHOD(NonlinearFixture, "nonlinear tuner: discard puts the linear K back",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.discard_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.last_send_script() ==
          "TUNING_TOWER COMMAND=G4 PARAMETER=P START=0 FACTOR=0\n"
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=linear ADVANCE=0.040000");
    CHECK_FALSE(overlay.session_active());
}

TEST_CASE("nonlinear tuner: without the firmware tower a pass does not start",
          "[pa_nonlinear][pa_tuner]") {
    NonlinearFixture fx(false);
    fx.overlay.start_for_test(fx.params(), pa::ExtruderKind::DirectDrive);
    const size_t before = fx.client.gcode_script_history().size();
    fx.overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.overlay.stage() == PaNonlinearOverlay::READY);
    CHECK(fx.client.gcode_script_history().size() == before);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "pa_nl_has_tower")) == 0);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a tower the firmware refuses after the command returns "
                 "leaves the heating screen",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    // The refusal arrives as an error in one frame, with no printing before it.
    const size_t before = client.gcode_script_history().size();
    tower("error");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
    CHECK(sent_since(before).find("TUNING_TOWER COMMAND=G4") != std::string::npos);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a tower that never starts gives up after the timeout",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    process_lvgl(PaNonlinearOverlay::kTowerStartTimeoutMs - 1000);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    process_lvgl(2000);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: discard works once the tower command has returned",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    overlay.discard_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(overlay.session_active());
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: starting over with another extruder kind puts the old model "
                 "back, but never mid-tower",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    REQUIRE(overlay.procedure_for_test()->pass_count() == 3);

    const size_t before = client.gcode_script_history().size();
    overlay.start_for_test(params(), pa::ExtruderKind::Bowden);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.procedure_for_test()->pass_count() == 4);
    CHECK(sent_since(before).find("MODEL=linear ADVANCE=0.040000") != std::string::npos);

    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);
    CHECK(overlay.procedure_for_test()->pass_count() == 4);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a start the printer never answered is still running, not "
                 "refused",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "request timed out");
    const size_t before = client.gcode_script_history().size();
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    // Nothing chases the script: a disarm would queue behind it and land mid-tower.
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);
    CHECK(sent_since(before).find("TUNING_TOWER COMMAND=G4") == std::string::npos);
    // The tower the script was always going to start is followed as usual.
    tower("printing");
    CHECK(overlay.stage() == PaNonlinearOverlay::PRINTING);
}

TEST_CASE_METHOD(NonlinearFixture, "nonlinear tuner: a start the printer refuses ends the pass",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Unknown command");
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
}
