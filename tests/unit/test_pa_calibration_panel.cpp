// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_calibration_pa.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "pa_tuning.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "spoolman_types.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::ui::PACalibrationPanel;
using nlohmann::json;

namespace {

struct PanelFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPI api;
    PACalibrationPanel panel;

    PanelFixture() : api(client, get_printer_state()) {
        auto& state = get_printer_state();
        state.init_subjects(false);
        const std::vector<std::string> objects = {"extruder", "helix_pa_gauge"};
        PrinterDiscovery hw;
        hw.parse_objects(objects);
        hw.set_printer_objects(objects);
        state.set_hardware(hw);
        client.connect("ws://mock/websocket", []() {}, []() {});
        client.set_mock_spoolman_enabled(true);
        panel.init_subjects();
        panel.set_api(&api);
    }
    ~PanelFixture() override {
        panel.cleanup();
        client.disconnect();
    }

    void drain() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    void finish_run(float k) {
        panel.set_state_for_test(PACalibrationPanel::MEASURING);
        panel.on_result_for_test(k);
        drain();
    }

    static int subject_int(const char* name) {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
    }
};

} // namespace

TEST_CASE_METHOD(PanelFixture, "a measured K is recorded with the provider that measured it",
                 "[pa_calibration][spoolman]") {
    finish_run(0.0812f);
    const json rec = client.mock_db_get(pa::kMoonrakerNamespace, pa::kMoonrakerKey);
    REQUIRE(rec.is_object());
    CHECK(rec["last"]["source"] == "measured:helix_gauge");
    CHECK(rec["last"]["k"].get<double>() == Catch::Approx(0.0812));
}

TEST_CASE_METHOD(PanelFixture, "a measured K saves to the active spool's filament",
                 "[pa_calibration][spoolman]") {
    auto& server = client.spoolman_mock();
    const int active = server.get_mock_active_spool_id();
    REQUIRE(active > 0);
    for (auto& spool : server.get_mock_spools()) {
        if (spool.id == active) {
            spool.filament_id = 7;
        }
    }

    finish_run(0.0812f);
    REQUIRE(subject_int("pa_cal_can_save_spool") == 1);

    const size_t patches = server.filament_updates.size();
    panel.save_to_spool_for_test();
    drain();
    REQUIRE(server.filament_updates.size() == patches + 1);
    CHECK(server.filament_updates.back().filament_id == 7);
    CHECK(server.filament_updates.back().data["extra"][kSpoolmanPaExtraKey] == "0.0812");
    // Saved once: the button goes away rather than inviting a second write.
    CHECK(subject_int("pa_cal_can_save_spool") == 0);
}

TEST_CASE_METHOD(PanelFixture, "without Spoolman the result offers no spool to save to",
                 "[pa_calibration][spoolman]") {
    client.set_mock_spoolman_enabled(false);
    finish_run(0.05f);
    CHECK(subject_int("pa_cal_can_save_spool") == 0);
    const size_t patches = client.spoolman_mock().filament_updates.size();
    panel.save_to_spool_for_test();
    drain();
    CHECK(client.spoolman_mock().filament_updates.size() == patches);
}

TEST_CASE("the measuring temperature follows the spool, then the material", "[pa_calibration]") {
    // The middle of the spool's stated range, on the panel's 5-degree step.
    CHECK(PACalibrationPanel::temp_for_filament("PLA", 200, 230) == 215);
    CHECK(PACalibrationPanel::temp_for_filament("PLA", 200, 222) == 210);
    // No range: the material's recommendation, inside the panel's limits.
    const int pla = PACalibrationPanel::temp_for_filament("PLA", 0, 0);
    CHECK(pla >= PACalibrationPanel::kTempMin);
    CHECK(pla <= PACalibrationPanel::kTempMax);
    CHECK(pla % PACalibrationPanel::kTempStep == 0);
    CHECK(PACalibrationPanel::temp_for_filament("PLA", 0, 0) !=
          PACalibrationPanel::temp_for_filament("PC", 0, 0));
    CHECK(PACalibrationPanel::temp_for_filament("", 400, 450) == PACalibrationPanel::kTempMax);
}

TEST_CASE_METHOD(PanelFixture, "a slot's result names its filament", "[pa_calibration]") {
    panel.prepare_for_filament("PETG-CF", 240, 260);
    CHECK(panel.target_temp_for_test() == 250);
    finish_run(0.06f);
    const json rec = client.mock_db_get(pa::kMoonrakerNamespace, pa::kMoonrakerKey);
    REQUIRE(rec.is_object());
    CHECK(rec["last"]["material"] == "PETG-CF");
    CHECK(rec["last"]["nozzle_temp"] == 250);
}
