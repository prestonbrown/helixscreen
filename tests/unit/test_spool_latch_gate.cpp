// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_spool_latch_gate.cpp
 * @brief The "spools are on the bed" latch: the allowlist, the five send-layer
 *        choke points it gates, and the derived machine_motion_blocked subject
 *        (prestonbrown/helixscreen#1730).
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/spool_latch_gate.h"
#include "../../include/ui_probe_overlay.h"
#include "../../src/api/moonraker_gcode_guards.h"
#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "core/update_queue.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("gcode_line_tokens splits lines and upper-cases each first token",
          "[spool_latch][gcode]") {
    using V = std::vector<std::string>;
    CHECK(gcode_line_tokens("") == V{});
    CHECK(gcode_line_tokens("g28") == V{"G28"});
    CHECK(gcode_line_tokens("  \tM140 S60\r\n\n   g1 z10\n; note\nM400") ==
          V{"M140", "G1", "M400"});
    CHECK(gcode_line_tokens("M104 S0\n") == V{"M104"});
    CHECK(gcode_line_tokens("\n\n  \n") == V{});
}

TEST_CASE("spool_latch_allows passes only commands that cannot move the toolhead",
          "[spool_latch][gcode]") {
    const std::vector<std::string> none;

    CHECK(spool_latch_allows("M140 S70", none));
    CHECK(spool_latch_allows("SET_IDLE_TIMEOUT TIMEOUT=9000\nM140 S0", none));
    CHECK(spool_latch_allows("set_heater_temperature HEATER=heater_bed TARGET=60", none));
    CHECK(spool_latch_allows("M112", none));
    CHECK(spool_latch_allows("TURN_OFF_HEATERS", none));
    CHECK(spool_latch_allows("; a comment only", none));
    CHECK(spool_latch_allows("", none));

    CHECK_FALSE(spool_latch_allows("G28", none));
    CHECK_FALSE(spool_latch_allows("G1 Z10", none));
    CHECK_FALSE(spool_latch_allows("M140 S60\nG28", none));
    CHECK_FALSE(spool_latch_allows("PRINT_START", none));
    CHECK_FALSE(spool_latch_allows("M84", none));
    // A restart releases the steppers, which can let a gantry sink onto the spools.
    CHECK_FALSE(spool_latch_allows("FIRMWARE_RESTART", none));
    CHECK_FALSE(spool_latch_allows("RESTART", none));

    SECTION("a running dry cycle adds its own commands") {
        const std::vector<std::string> dryer = {"APPLIANCE_DRY_START", "APPLIANCE_DRY_STOP"};
        CHECK(spool_latch_allows("APPLIANCE_DRY_START TEMP=55 HOURS=4", dryer));
        CHECK_FALSE(spool_latch_allows("APPLIANCE_DRY_START TEMP=55 HOURS=4", none));
    }
}

namespace {

class SpoolLatchFixture : public LVGLTestFixture {
  public:
    SpoolLatchFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~SpoolLatchFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    helix::SharedCallback<void(const MoonrakerError&)> on_error() {
        return [this](const MoonrakerError& err) {
            error_called = true;
            captured_error = err;
        };
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    bool error_called = false;
    MoonrakerError captured_error;
};

} // namespace

TEST_CASE_METHOD(SpoolLatchFixture, "the latch refuses motion at both gcode choke points",
                 "[spool_latch][mock]") {
    state.print_state().set_spool_latch(true);

    SECTION("MoonrakerAPI::execute_gcode refuses a macro that may move") {
        api->execute_gcode("PRINT_START", nullptr, on_error());
        CHECK(error_called);
        CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
        CHECK(mock_client.gcode_script_history().empty());
    }

    SECTION("MoonrakerAPI::execute_gcode still sends a heater command") {
        api->execute_gcode("M140 S70", nullptr, on_error());
        CHECK_FALSE(error_called);
        CHECK_FALSE(mock_client.gcode_script_history().empty());
    }

    SECTION("the motion API refuses homing and a Z move") {
        api->motion().home_axes("", nullptr, on_error());
        CHECK(error_called);
        error_called = false;
        api->motion().move_axis('Z', -5.0, 600.0, nullptr, on_error());
        CHECK(error_called);
        CHECK(mock_client.gcode_script_history().empty());
    }

    SECTION("a dry cycle's own commands pass once registered") {
        state.print_state().set_spool_latch(true, {"APPLIANCE_DRY_STOP"});
        api->execute_gcode("APPLIANCE_DRY_STOP", nullptr, on_error());
        CHECK_FALSE(error_called);
    }
}

TEST_CASE_METHOD(SpoolLatchFixture, "the latch refuses every print start and resume",
                 "[spool_latch][mock]") {
    state.print_state().set_spool_latch(true);

    SECTION("start_print") {
        api->job().start_print("benchy.gcode", nullptr, on_error());
        CHECK(error_called);
        CHECK(mock_client.last_send_method() != "printer.print.start");
    }
    SECTION("start_modified_print") {
        api->job().start_modified_print("benchy.gcode", "/tmp/x.gcode", {}, nullptr, on_error());
        CHECK(error_called);
    }
    SECTION("resume_print") {
        api->job().resume_print(nullptr, on_error());
        CHECK(error_called);
        CHECK(mock_client.last_send_method() != "printer.print.resume");
    }
    SECTION("start_queue") {
        api->queue().start_queue(nullptr, on_error());
        CHECK(error_called);
        CHECK(mock_client.last_send_method() != "server.job_queue.start");
    }
}

TEST_CASE_METHOD(SpoolLatchFixture, "with the latch clear, motion and print starts go out",
                 "[spool_latch][mock]") {
    state.print_state().set_spool_latch(false);
    api->motion().home_axes("", nullptr, on_error());
    CHECK_FALSE(error_called);
    api->job().start_print("benchy.gcode", [] {}, on_error());
    CHECK_FALSE(error_called);
    CHECK(mock_client.last_send_method() == "printer.print.start");
}

TEST_CASE_METHOD(SpoolLatchFixture, "machine_motion_blocked is job_holds_machine or the latch",
                 "[spool_latch][job_holds_machine]") {
    lv_subject_t* blocked = state.print_state().get_machine_motion_blocked_subject();
    REQUIRE(lv_subject_get_int(blocked) == 0);

    state.print_state().set_spool_latch(true);
    CHECK(lv_subject_get_int(blocked) == 1);
    CHECK(lv_subject_get_int(state.print_state().get_job_holds_machine_subject()) == 0);

    state.print_state().set_spool_latch(false);
    CHECK(lv_subject_get_int(blocked) == 0);

    state.print_state().set_print_start_state(PrintStartPhase::BED_MESH, "", 0);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(lv_subject_get_int(blocked) == 1);
    state.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(lv_subject_get_int(blocked) == 0);
}

TEST_CASE_METHOD(SpoolLatchFixture, "probe calibration commands go through the latch",
                 "[spool_latch][probe][1730]") {
    IMoonrakerAPI* previous = get_moonraker_api();
    IMoonrakerClient* previous_client = get_moonraker_client();
    // Both globals set, as in the app: a probe command must still take the API.
    set_moonraker_api(api.get());
    set_moonraker_client(&mock_client);
    state.print_state().set_spool_latch(true);

    CHECK_FALSE(helix::ui::probe_send_gcode("CARTOGRAPHER_TOUCH_CALIBRATE",
                                            "Cartographer Touch Calibrate"));
    CHECK_FALSE(helix::ui::probe_send_gcode("BLTOUCH_DEBUG COMMAND=pin_down", "BLTouch Deploy"));
    CHECK(mock_client.gcode_script_history().empty());

    state.print_state().set_spool_latch(false);
    CHECK(helix::ui::probe_send_gcode("BEACON_CALIBRATE", "Beacon Calibrate"));
    CHECK_FALSE(mock_client.gcode_script_history().empty());

    set_moonraker_api(previous);
    set_moonraker_client(previous_client);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(SpoolLatchFixture, "the restart paths refuse while spools are on the bed",
                 "[spool_latch][mock][1730]") {
    state.print_state().set_spool_latch(true);
    SECTION("firmware restart") {
        api->restart_firmware([] {}, on_error());
        CHECK(error_called);
        CHECK(captured_error.message.find("tap the drying banner") != std::string::npos);
    }
    SECTION("Klipper restart") {
        api->restart_klipper([] {}, on_error());
        CHECK(error_called);
    }
    SECTION("the Klipper service") {
        api->restart_service("klipper", [] {}, on_error());
        CHECK(error_called);
    }
    SECTION("an emergency stop still goes out") {
        api->execute_gcode("M112", nullptr, on_error());
        CHECK_FALSE(error_called);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a probe command's late error writes into nothing it returned from",
                 "[spool_latch][probe][1730]") {
    helix::test::FakeMoonrakerClient client;
    PrinterState state;
    state.init_subjects(false);
    state.set_klippy_state_sync(KlippyState::READY);
    MoonrakerAPI api(client, state);
    IMoonrakerAPI* previous = get_moonraker_api();
    set_moonraker_api(&api);
    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity, const std::string& m, uint32_t) { toasts.push_back(m); });

    REQUIRE(helix::ui::probe_send_gcode("BEACON_CALIBRATE", "Beacon Calibrate"));

    // The sender's frame is gone; the error still reaches the user.
    REQUIRE(client.fail_last("printer.gcode.script",
                             MoonrakerError::not_ready("printer.gcode.script", "probe timed out")));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(std::find(toasts.begin(), toasts.end(), "probe timed out") != toasts.end());

    helix::ui::set_test_toast_hook(nullptr);
    set_moonraker_api(previous);
}
