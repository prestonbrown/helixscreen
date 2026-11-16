// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_pa_tuning.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_indx.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "spoolman_types.h"
#include "tool_state.h"
#include "tune_controller.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::ui::PaTuningOverlay;
using nlohmann::json;

namespace {

struct TunerFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPI api;
    PaTuningOverlay overlay;

    TunerFixture() : api(client, get_printer_state()) {
        auto& state = get_printer_state();
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        state.temperature_state().init_extruders({"extruder"});
        client.connect("ws://mock/websocket", []() {}, []() {});
        overlay.init_subjects();
        overlay.set_api(&api);
        overlay.set_extruder_for_test("extruder");
        overlay.set_raise_on_start_for_test(false);
    }
    ~TunerFixture() override {
        overlay.cleanup();
        status({{"print_stats", {{"state", "standby"}, {"filename", ""}}}});
        client.stop_temperature_simulation();
        client.disconnect();
    }

    void status(const json& s) {
        get_printer_state().update_from_status(s);
        helix::ui::UpdateQueue::instance().drain();
    }

    void start_session() {
        overlay.session_for_test().start(pa::TowerGeometry{}, pa::Ramp{0.0f, 0.09f, 10}, 0.04f);
        overlay.begin_observing_for_test();
    }

    void printing(int layer) {
        status({{"print_stats",
                 {{"state", "printing"},
                  {"filename", PaTuningOverlay::TEST_FILENAME},
                  {"info", {{"current_layer", layer}, {"total_layer", 156}}}}}});
    }

    /// The printer echoes K the way Klipper reports it.
    void report_k(double k) {
        status({{"extruder", {{"pressure_advance", k}}}});
    }
};

} // namespace

TEST_CASE_METHOD(TunerFixture, "the tuner ramps K on its own print and takes a nudge",
                 "[pa_tune][pa_tuner]") {
    start_session();
    printing(1);
    REQUIRE(overlay.session_for_test().state() == pa::TuningSession::State::Printing);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0000");
    report_k(0.0);

    printing(22);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0100");
    report_k(0.01);

    overlay.nudge_for_test(1); // default step 0.005
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0150");
    CHECK(overlay.session_for_test().mode() == pa::TuningSession::Mode::Manual);
    report_k(0.015);

    status({{"print_stats", {{"state", "complete"}}}});
    REQUIRE(overlay.stage() == PaTuningOverlay::PICK);
    const auto bands = overlay.session_for_test().bands();
    REQUIRE(bands.size() == 2);
    CHECK(bands[1].k == Catch::Approx(0.015f));

    overlay.select_band_for_test(0);
    overlay.apply_pick_for_test();
    CHECK(overlay.stage() == PaTuningOverlay::RESULT);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0000");
}

TEST_CASE_METHOD(TunerFixture, "a finished state from an earlier print does not end the run",
                 "[pa_tune][pa_tuner]") {
    status({{"print_stats", {{"state", "complete"}, {"filename", "benchy.gcode"}}}});
    start_session();
    CHECK(overlay.session_for_test().state() == pa::TuningSession::State::Starting);

    // Someone else's print starting is not ours either.
    status({{"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}});
    CHECK(overlay.session_for_test().state() == pa::TuningSession::State::Starting);
}

TEST_CASE_METHOD(TunerFixture, "a pick after someone else's print started sends nothing",
                 "[pa_tune][pa_tuner]") {
    start_session();
    printing(1);
    report_k(0.0);
    printing(30);
    report_k(0.01);
    status({{"print_stats", {{"state", "complete"}}}});
    REQUIRE(overlay.stage() == PaTuningOverlay::PICK);

    // A different job takes the machine while the picker sits open.
    status({{"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}});
    helix::tune::set_speed_percent(&api, 110);
    overlay.select_band_for_test(0);
    overlay.apply_pick_for_test();
    CHECK(client.last_send_script() == "M220 S110");
    CHECK(overlay.stage() == PaTuningOverlay::PICK);
}

TEST_CASE_METHOD(TunerFixture, "the Tune row's nudge during a run goes through the session",
                 "[pa_tune][pa_tuner]") {
    CHECK_FALSE(overlay.nudge_by(0.001f)); // no run yet: the caller sends itself
    start_session();
    printing(1);
    report_k(0.0);
    REQUIRE(overlay.nudge_by(0.001f));
    CHECK(overlay.session_for_test().mode() == pa::TuningSession::Mode::Manual);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0010");
    // The ramp no longer overwrites it at the next band.
    printing(22);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0010");
}

TEST_CASE_METHOD(TunerFixture, "with beta features off a result saves to the active spool",
                 "[pa_tune][pa_tuner][spoolman]") {
    struct BetaOff {
        bool prev = Config::get_instance()->is_beta_features_enabled();
        BetaOff() {
            Config::get_instance()->set<bool>("/beta_features", false);
        }
        ~BetaOff() {
            Config::get_instance()->set<bool>("/beta_features", prev);
        }
    } beta_off;
    client.set_mock_spoolman_enabled(true);
    auto& server = client.spoolman_mock();
    const int active = server.get_mock_active_spool_id();
    REQUIRE(active > 0);
    // Mock spools carry no filament id; Save to Spool writes to the filament.
    for (auto& spool : server.get_mock_spools()) {
        if (spool.id == active) {
            spool.filament_id = 7;
        }
    }

    start_session();
    printing(1);
    report_k(0.0);
    printing(22);
    report_k(0.01);
    status({{"print_stats", {{"state", "complete"}}}});
    REQUIRE(overlay.stage() == PaTuningOverlay::PICK);
    overlay.select_band_for_test(1);
    overlay.apply_pick_for_test();
    // Spoolman status, then the spool itself: one queued reply each.
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaTuningOverlay::RESULT);
    INFO(lv_subject_get_string(lv_xml_get_subject(nullptr, "pa_tune_spoolman_text")));

    lv_subject_t* can_save = lv_xml_get_subject(nullptr, "pa_tune_can_save_spool");
    REQUIRE(can_save != nullptr);
    CHECK(lv_subject_get_int(can_save) == 1);

    const size_t patches = server.filament_updates.size();
    overlay.save_to_spool_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(server.filament_updates.size() == patches + 1);
    CHECK(server.filament_updates.back().data["extra"][kSpoolmanPaExtraKey] == "0.0100");
}

TEST_CASE("AMS lanes into one hotend are one head, and select no tool", "[pa_tune][pa_tuner]") {
    // Four lanes, one extruder: what a Happy Hare or AFC rig publishes.
    PhysicalHead lanes;
    lanes.extruder_name = "extruder";
    lanes.tool_indices = {0, 1, 2, 3};
    auto one = PaTuningOverlay::choose_head({lanes}, /*multi_extruder=*/false, 0, 2, "extruder");
    CHECK(one.tool == -1);
    CHECK(one.extruder == "extruder");

    // A changer with sparse numbering: the head's own tool is selected.
    PhysicalHead t0{std::string("extruder"), {0}};
    PhysicalHead t5{std::string("extruder5"), {5}};
    auto changer = PaTuningOverlay::choose_head({t0, t5}, true, 1, 0, "extruder");
    CHECK(changer.tool == 5);
    CHECK(changer.extruder == "extruder5");
    // Out-of-range picks clamp to a real head.
    CHECK(PaTuningOverlay::choose_head({t0, t5}, true, 9, 0, "extruder").tool == 5);
    CHECK(PaTuningOverlay::choose_head({}, true, 0, 0, "extruder1").extruder == "extruder1");
}

TEST_CASE_METHOD(TunerFixture,
                 "one status frame with the restore and the end marker keeps "
                 "the top band",
                 "[pa_tune][pa_tuner]") {
    start_session();
    printing(1);
    report_k(0.0);
    printing(156);
    report_k(0.09);
    // As Moonraker delivers it: both changes in a single notification.
    status({{"extruder", {{"pressure_advance", 0.04}}},
            {"print_stats", {{"info", {{"current_layer", 157}, {"total_layer", 156}}}}}});
    status({{"print_stats", {{"state", "complete"}}}});
    REQUIRE(overlay.stage() == PaTuningOverlay::PICK);
    CHECK(overlay.session_for_test().bands().back().k == Catch::Approx(0.09f));
}

TEST_CASE_METHOD(TunerFixture, "after the tower is done the Tune row sends on its own",
                 "[pa_tune][pa_tuner]") {
    start_session();
    printing(1);
    report_k(0.0);
    printing(157); // the end gcode's marker
    CHECK_FALSE(overlay.nudge_by(0.001f));
}

TEST_CASE("the tower's pre-print routing is its own tool on its own head", "[pa_tune][pa_tuner]") {
    CHECK(PaTuningOverlay::tower_routing(2) == std::map<int, int>{{2, 2}});
    CHECK(PaTuningOverlay::tower_routing(-1) == std::map<int, int>{{0, 0}});
}

namespace {

/// The overlay over the API mock, which records deletes.
struct TunerMockApiFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api{client, get_printer_state()};
    PaTuningOverlay overlay;

    TunerMockApiFixture() {
        get_printer_state().init_subjects(false);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        get_printer_state().temperature_state().init_extruders({"extruder"});
        overlay.init_subjects();
        overlay.set_api(&api);
        overlay.set_extruder_for_test("extruder");
        overlay.set_raise_on_start_for_test(false);
    }
    ~TunerMockApiFixture() override {
        overlay.cleanup();
        status({{"print_stats", {{"state", "standby"}, {"filename", ""}}}});
    }
    void status(const json& s) {
        get_printer_state().update_from_status(s);
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(TunerMockApiFixture, "the tower file is deleted when its print ends",
                 "[pa_tune][pa_tuner]") {
    overlay.session_for_test().start(pa::TowerGeometry{}, pa::Ramp{}, 0.04f);
    overlay.begin_observing_for_test();
    status({{"print_stats",
             {{"state", "printing"},
              {"filename", PaTuningOverlay::TEST_FILENAME},
              {"info", {{"current_layer", 3}, {"total_layer", 156}}}}}});
    CHECK(api.files_mock().deleted_files().empty());
    status({{"print_stats", {{"state", "cancelled"}}}});
    CHECK(api.files_mock().deleted_files() ==
          std::vector<std::string>{std::string("gcodes/") + PaTuningOverlay::TEST_FILENAME});
}

TEST_CASE_METHOD(TunerFixture, "the tower does not start while the nozzle may not be heated",
                 "[pa_tune][pa_tuner][indx][heat]") {
    helix::test::RegisteredBackend<helix::test::GcodeCaptureBackend<helix::AmsBackendIndx>> harness;
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"indx", "save_variables", "extruder",
                                  "gcode_macro TOOL_POSITIONS", "gcode_macro _TOOLCHANGE_VARS",
                                  "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL"}));
    harness->set_discovery(hw);
    harness->feed(json{{"save_variables", {{"variables", {{"active_tool", -1}}}}}});
    REQUIRE_FALSE(AmsState::instance().nozzle_heat_blocked_reason().empty());

    overlay.confirm_and_start_for_test();
    CHECK(overlay.error_for_test() == AmsState::instance().nozzle_heat_blocked_reason());
}
