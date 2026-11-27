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
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "core/update_queue.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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

/// Records every JSON-RPC the send layer lets out, then forwards it.
class RecordingClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;
    std::vector<std::pair<std::string, nlohmann::json>> sent;

    helix::RequestId send_jsonrpc(
        const std::string& method, const nlohmann::json& params,
        helix::SharedCallback<void(const nlohmann::json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        sent.emplace_back(method, params);
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }
};

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

    RecordingClient mock_client;
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

namespace {

enum class JobHeld { Printing, Paused, Preparing };

void hold_machine(PrinterState& state, JobHeld held) {
    switch (held) {
    case JobHeld::Printing:
        helix::test::set_wire_state(state, PrintJobState::PRINTING);
        break;
    case JobHeld::Paused:
        helix::test::set_wire_state(state, PrintJobState::PAUSED);
        break;
    case JobHeld::Preparing:
        state.print_state().begin_preparing(PrintJobRef{"chosen.gcode", "", ""});
        break;
    }
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

const char* const kRestartPrinting = "Can't restart Klipper while a print is running.";

} // namespace

TEST_CASE_METHOD(SpoolLatchFixture,
                 "the send layer never refuses a restart for a job: recovery must go out",
                 "[restart_guard][mock]") {
    // An e-stop escalation restarts while print_stats still reads printing and
    // Klippy still reads READY.
    hold_machine(state, GENERATE(JobHeld::Printing, JobHeld::Paused, JobHeld::Preparing));
    // The proof is that each restart REACHED the client, not what an error said.
    const auto reaches = [this](const std::function<void()>& call, const std::string& method,
                                const nlohmann::json& params) {
        mock_client.sent.clear();
        call();
        return std::any_of(mock_client.sent.begin(), mock_client.sent.end(), [&](const auto& sent) {
            return sent.first == method && sent.second == params;
        });
    };
    CHECK(reaches([&] { api->restart_firmware([] {}, on_error()); }, "printer.firmware_restart",
                  nlohmann::json::object()));
    CHECK(reaches([&] { api->restart_klipper([] {}, on_error()); }, "printer.restart",
                  nlohmann::json::object()));
    CHECK(reaches([&] { api->restart_service("klipper", [] {}, on_error()); },
                  "machine.services.restart", nlohmann::json{{"service", "klipper"}}));
    for (const char* script : {"FIRMWARE_RESTART", "SAVE_CONFIG"}) {
        CAPTURE(script);
        const size_t before = mock_client.gcode_script_history().size();
        api->execute_gcode(script, nullptr, on_error());
        CHECK(mock_client.gcode_script_history().size() == before + 1);
    }
}

#if HELIX_HAS_PLUGINS
TEST_CASE_METHOD(SpoolLatchFixture, "a plugin's restart is refused while a job holds the machine",
                 "[restart_guard][plugin]") {
    hold_machine(state, GENERATE(JobHeld::Printing, JobHeld::Paused, JobHeld::Preparing));
    using json = nlohmann::json;
    const bool refused = GENERATE(true, false);
    CAPTURE(refused);
    if (refused) {
        const auto call = GENERATE(
            std::make_pair(std::string("printer.firmware_restart"), json::object()),
            std::make_pair(std::string("printer.restart"), json::object()),
            std::make_pair(std::string("machine.services.restart"), json{{"service", "klipper"}}),
            std::make_pair(std::string("printer.gcode.script"), json{{"script", "SAVE_CONFIG"}}),
            std::make_pair(std::string("printer.gcode.script"),
                           json{{"script", "M117 x\nrestart"}}));
        CAPTURE(call.first, call.second.dump());
        CHECK(helix::api::reject_plugin_rpc(state, call.first, call.second, on_error()));
        CHECK(captured_error.message == kRestartPrinting);
    } else {
        const auto call = GENERATE(
            std::make_pair(std::string("machine.services.restart"), json{{"service", "moonraker"}}),
            std::make_pair(std::string("printer.gcode.script"),
                           json{{"script", "SAVE_CONFIG RESTART=0"}}),
            std::make_pair(std::string("printer.gcode.script"),
                           json{{"script", "save_config restart=0"}}),
            std::make_pair(std::string("printer.gcode.script"),
                           json{{"script", "M117 SAVE_CONFIG_DONE"}}));
        CAPTURE(call.first, call.second.dump());
        CHECK_FALSE(helix::api::reject_plugin_rpc(state, call.first, call.second, on_error()));
        CHECK_FALSE(error_called);
    }
}

TEST_CASE_METHOD(SpoolLatchFixture, "a plugin's restart goes out when idle or halted",
                 "[restart_guard][plugin]") {
    using json = nlohmann::json;
    SECTION("idle") {
        CHECK_FALSE(helix::api::reject_plugin_rpc(state, "printer.firmware_restart", json::object(),
                                                  on_error()));
    }
    SECTION("halted with a job on the books") {
        hold_machine(state, JobHeld::Printing);
        state.set_klippy_state_sync(GENERATE(KlippyState::SHUTDOWN, KlippyState::ERROR));
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK_FALSE(helix::api::reject_plugin_rpc(state, "printer.firmware_restart", json::object(),
                                                  on_error()));
    }
    CHECK_FALSE(error_called);
}
#endif

TEST_CASE("gcode_restarts_klipper reads a line's own command", "[restart_guard][gcode]") {
    using helix::api::gcode_restarts_klipper;
    CHECK(gcode_restarts_klipper("RESTART"));
    CHECK(gcode_restarts_klipper("firmware_restart"));
    CHECK(gcode_restarts_klipper("  SAVE_CONFIG"));
    CHECK(gcode_restarts_klipper("M117 a\nSAVE_CONFIG\nM117 b"));
    CHECK(gcode_restarts_klipper("SAVE_CONFIG RESTART=1"));
    CHECK_FALSE(gcode_restarts_klipper("SAVE_CONFIG RESTART=0"));
    CHECK_FALSE(gcode_restarts_klipper("save_config restart=0"));
    CHECK_FALSE(gcode_restarts_klipper("M117 SAVE_CONFIG"));
    CHECK_FALSE(gcode_restarts_klipper("; RESTART"));
    CHECK_FALSE(gcode_restarts_klipper("RESTART_MACRO"));
    CHECK_FALSE(gcode_restarts_klipper(""));
}
