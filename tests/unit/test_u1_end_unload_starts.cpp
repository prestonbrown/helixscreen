// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The U1 forgets its unload-at-end list when each print ends, so every way a
// print starts carries it: a reprint with no recorded routing and a restart
// from the beginning included.

#if HELIX_HAS_SNAPMAKER

#include "ui_print_start_controller.h"
#include "ui_resume_dispatch.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

constexpr const char* kUnload = "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,0,0]";

struct U1StartFx : LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;

    U1StartFx() {
        helix::Config::get_instance();
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().set_end_of_print_unload_tools(0b0001);
        ps.init_subjects(false);
        ps.set_klippy_state_sync(KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
    }
    ~U1StartFx() override {
        // A restart builds the process-wide print-select panel around the
        // case's own API, which dies with the case.
        StaticPanelRegistry::instance().destroy_all();
        SettingsManager::instance().set_end_of_print_unload_tools(0);
        client.stop_temperature_simulation();
        client.disconnect();
    }

    /// Every script sent, joined: the unload line may ride in any of them.
    std::string all_scripts() const {
        std::string out;
        for (const auto& s : client.gcode_script_history())
            out += s + "\n";
        return out;
    }
};

void report_heads(AmsBackendSnapmaker& backend) {
    SnapmakerTestAccess::handle_status(
        backend, nlohmann::json{{"print_task_config",
                                 {{"end_unload_filament", {false, false, false, false}}}}});
}

} // namespace

TEST_CASE_METHOD(U1StartFx, "a U1 reprint with no recorded routing still sends the unload list",
                 "[end_unload][preprint]") {
    MoonrakerAPI api{client, ps};
    helix::ui::PrintStartController controller{ps, &api};
    test::RegisteredBackend<AmsBackendSnapmaker> backend(nullptr, nullptr);
    report_heads(*backend);
    REQUIRE(backend->requires_preprint_send());

    controller.initiate_reprint("cal.gcode", "cal.gcode", {0}, []() {}, []() {});
    helix::ui::UpdateQueue::instance().drain();
    const std::string sent = all_scripts();
    CHECK(sent.find(kUnload) != std::string::npos);
    // No routing was recorded, so no map is invented.
    CHECK(sent.find("SET_PRINT_EXTRUDER_MAP") == std::string::npos);
}

TEST_CASE_METHOD(U1StartFx, "a U1 reprint withdrawn during the pre-print send never starts",
                 "[preparing_cancel][preprint]") {
    int starts = 0;
    MoonrakerClientMockTestAccess::set_method_handler(
        client, "printer.print.start",
        [&starts](MoonrakerClientMock*, const nlohmann::json&,
                  helix::SharedCallback<void(const nlohmann::json&)> ok,
                  helix::SharedCallback<void(const MoonrakerError&)>) -> bool {
            ++starts;
            ok(nlohmann::json{{"result", "ok"}});
            return true;
        });
    MoonrakerAPI api{client, ps};
    helix::ui::PrintStartController controller{ps, &api};
    test::RegisteredBackend<AmsBackendSnapmaker> backend(nullptr, nullptr);
    report_heads(*backend);
    REQUIRE(backend->requires_preprint_send());

    const bool withdraw = GENERATE(false, true);
    CAPTURE(withdraw);
    bool errored = false;
    controller.initiate_reprint(
        "cal.gcode", "cal.gcode", {0}, []() {}, [&errored]() { errored = true; });
    REQUIRE(ps.print_state().has_preparing_job());
    if (withdraw) {
        // Stop while the pre-print config is still on its way.
        ps.print_state().retire_preparing(PreparingExit::Cancelled);
    }
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }

    CHECK(starts == (withdraw ? 0 : 1));
    CHECK(errored == withdraw);
    // A start that went out is one only the printer can stop now.
    CHECK(ps.print_state().preparing_start() ==
          (withdraw ? PreparingStart::None : PreparingStart::Accepted));
}

TEST_CASE_METHOD(U1StartFx, "a U1 reprint whose start loses its answer stays preparing",
                 "[preparing_cancel][preprint][1898]") {
    // The printer may still be running its start sequence; only a refusal fails it.
    const int kind = GENERATE(0, 1, 2); // timeout, timeout with Stop waiting, refusal
    CAPTURE(kind);
    const bool refused = kind == 2;
    const MoonrakerError error =
        refused ? MoonrakerError::from_json_rpc({{"code", 400}, {"message", "File not found"}},
                                                "printer.print.start")
                : MoonrakerError::timeout("printer.print.start", 600000);
    MoonrakerClientMockTestAccess::set_method_handler(
        client, "printer.print.start",
        [error](MoonrakerClientMock*, const nlohmann::json&,
                helix::SharedCallback<void(const nlohmann::json&)>,
                helix::SharedCallback<void(const MoonrakerError&)> err) -> bool {
            err(error);
            return true;
        });
    MoonrakerAPI api{client, ps};
    helix::ui::PrintStartController controller{ps, &api};
    test::RegisteredBackend<AmsBackendSnapmaker> backend(nullptr, nullptr);
    report_heads(*backend);

    bool errored = false;
    controller.initiate_reprint(
        "cal.gcode", "cal.gcode", {0}, []() {}, [&errored]() { errored = true; });
    if (kind == 1) {
        ps.print_state().note_preparing_cancel_requested();
    }
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }

    CHECK(errored == refused);
    CHECK(ps.print_state().has_preparing_job() == !refused);
    if (refused) {
        CHECK(ps.print_state().last_preparing_exit() == PreparingExit::Failed);
        return;
    }
    CHECK(ps.print_state().preparing_start() == PreparingStart::Unanswered);

    ps.update_from_status(
        nlohmann::json{{"print_stats", {{"state", "printing"}, {"filename", "cal.gcode"}}}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(ps.print_state().has_preparing_job());
    CHECK(ps.print_state().last_preparing_exit() == PreparingExit::Confirmed);
}

TEST_CASE_METHOD(U1StartFx,
                 "a U1 restart from the beginning sends the unload list after the cancel",
                 "[end_unload][preprint]") {
    MoonrakerAPI api{client, ps};
    test::RegisteredBackend<AmsBackendSnapmaker> backend(nullptr, nullptr);
    report_heads(*backend);

    helix::ui::restart_print_from_beginning(&api, "cal.gcode", "[test]", nullptr);
    // The restart reads the file's tools first; this one is not on the printer.
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    CHECK(all_scripts().find(std::string("CANCEL_PRINT_BASE\n") + kUnload) != std::string::npos);
}

TEST_CASE_METHOD(U1StartFx, "a U1 restart from the beginning names the file's heads",
                 "[end_unload][preprint][reprint]") {
    MoonrakerAPI api{client, ps};
    test::RegisteredBackend<AmsBackendSnapmaker> backend(nullptr, nullptr);
    report_heads(*backend);
    helix::PlantedGcode file("restart_t0t2.gcode", "",
                             "; filament_type = PLA;PLA;PETG\n"
                             "; filament_colour = #FF0000;#00FF00;#0000FF\n"
                             "G28\nT0\nT2\n"
                             "; filament used [g] = 1.50, 0.00, 2.25\n");

    helix::ui::restart_print_from_beginning(&api, file.name(), "[test]", nullptr);
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    const std::string sent = all_scripts();
    CHECK(sent.find("CANCEL_PRINT_BASE\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2") !=
          std::string::npos);
    CHECK(sent.find("SET_PRINT_EXTRUDER_MAP") == std::string::npos);
}

TEST_CASE_METHOD(U1StartFx, "a U1 restart of an Orca job names its heads from the footer",
                 "[end_unload][preprint][reprint]") {
    // The API mock serves the planted file's bytes to the footer's range read.
    MoonrakerAPIMock api{client, ps};
    test::RegisteredBackend<AmsBackendSnapmaker> backend(nullptr, nullptr);
    report_heads(*backend);
    // Orca's U1 profile leaves per-tool weights out of Moonraker's metadata;
    // its footer carries them ("BMCU 370C_ASA_18.4g_54m6s.gcode", off a U1).
    helix::PlantedGcode file("restart_orca_u1.gcode", "",
                             "G28\nT0\nT1\n; CONFIG_BLOCK_END\n\n"
                             "; filament used [mm] = 7140.18, 245.42, 0.00, 0.00\n"
                             "; filament used [cm3] = 17.17, 0.59, 0.00, 0.00\n"
                             "; filament used [g] = 17.86, 0.63, 0.00, 0.00\n"
                             "; total filament used [g] = 18.49\n");
    client.fail_next("server.files.metadata");

    helix::ui::restart_print_from_beginning(&api, file.name(), "[test]", nullptr);
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    CHECK(all_scripts().find("CANCEL_PRINT_BASE\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=0,1") !=
          std::string::npos);
}

TEST_CASE_METHOD(U1StartFx, "a U1 restart drops when the printer connection changed meanwhile",
                 "[end_unload][preprint][reprint]") {
    MoonrakerAPI api{client, ps};
    MoonrakerAPI next_printer{client, ps};
    test::RegisteredBackend<AmsBackendSnapmaker> backend(nullptr, nullptr);
    report_heads(*backend);
    IMoonrakerAPI* previous = get_moonraker_api();

    int failures = 0;
    helix::ui::restart_print_from_beginning(&api, "cal.gcode", "[test]",
                                            [&failures]() { ++failures; });
    // A printer switch lands while the file's tools are read.
    set_moonraker_api(&next_printer);
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    set_moonraker_api(previous);

    CHECK(all_scripts().find("CANCEL_PRINT_BASE") == std::string::npos);
    CHECK(failures == 1);
}

#endif // HELIX_HAS_SNAPMAKER
