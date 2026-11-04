// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_nozzle_offset_calibration.cpp
 * @brief The mock's U1 offset calibration steps, driven by the real runner
 *
 * The firmware's steps refuse outside its calibration state and answer only
 * when the heat, wipe or probe pass ends; the plate check refuses while the PEI
 * sheet is on. The --test flow is only worth anything if the mock keeps those
 * three facts, so these run the production Runner against it end to end.
 */

#include "ui_snapmaker_nozzle_offset_overlay.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "ams_backend_snapmaker.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "snapmaker_nozzle_offset.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

using nlohmann::json;
using namespace helix::snapmaker::nozzle_offset;

namespace {

/// The mock, plus every JSON-RPC method sent through it.
struct RecordingClient : public MoonrakerClientMock {
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;
    std::vector<std::string> methods;

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        methods.push_back(method);
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    [[nodiscard]] bool sent(const std::string& method) const {
        return std::find(methods.begin(), methods.end(), method) != methods.end();
    }
};

struct NozzleCalFixture : public LVGLTestFixture {
    helix::ScopedEnv plate_env{"HELIX_MOCK_NOZZLE_CAL_PLATE_REFUSE"};
    helix::ScopedEnv fail_env{"HELIX_MOCK_NOZZLE_CAL_FAIL"};
    RecordingClient client{MoonrakerClientMock::PrinterType::SNAPMAKER_U1, 100.0};
    int asked = 0;
    Runner runner{Runner::Hooks{
        [this](const std::string& gcode, uint32_t timeout_ms, Runner::Completion done) {
            client.send_jsonrpc(
                "printer.gcode.script", json{{"script", gcode}},
                [done](const json&) { done(true, {}); },
                [done](const MoonrakerError& e) { done(false, e.message); }, timeout_ms);
        },
        nullptr, nullptr}};

    // The mock never answers EXIT inside leave_live_run()'s wait, so every
    // wait here runs to its end.
    NozzleCalFixture() {
        clear_exit_pending();
        set_teardown_exit_wait_ms(10);
    }
    ~NozzleCalFixture() {
        set_teardown_exit_wait_ms(kTeardownExitWaitMs);
        clear_exit_pending();
    }

    /// Enters the state the way another client's run would.
    void enter_state_elsewhere() {
        client.send_jsonrpc(
            "printer.gcode.script", json{{"script", "EXTRUDER_OFFSET_ACTION_PRESTART"}},
            [](const json&) {}, [](const MoonrakerError&) {});
        process_lvgl(1300);
        REQUIRE(client.nozzle_calibration_active());
    }

    void reconcile() {
        reconcile_on_connect(client, [this] { ++asked; });
        process_lvgl(1300);
    }

    /// Pumps the mock's step timers, confirming every prompt, until the run ends.
    void run_to_end() {
        for (int guard = 0; guard < 200; ++guard) {
            const auto phase = runner.phase();
            if (phase == Runner::Phase::Done || phase == Runner::Phase::Failed ||
                phase == Runner::Phase::Cancelled) {
                return;
            }
            if (phase == Runner::Phase::AwaitingUser) {
                runner.confirm();
            }
            process_lvgl(1300);
        }
        FAIL("the run did not end");
    }
};

} // namespace

TEST_CASE_METHOD(NozzleCalFixture, "mock: a step outside the calibration state is refused",
                 "[mock][snapmaker][nozzle_offset]") {
    bool failed = false;
    std::string error;
    client.send_jsonrpc(
        "printer.gcode.script", json{{"script", "EXTRUDER_OFFSET_ACTION_HEAT TOOL_ID=T0"}},
        [](const json&) {},
        [&](const MoonrakerError& e) {
            failed = true;
            error = e.message;
        });
    process_lvgl(1300);
    REQUIRE(failed);
    CHECK(error.find("0003-0530-0000-0009") != std::string::npos);
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: a full run saves new offsets and leaves the state",
                 "[mock][snapmaker][nozzle_offset]") {
    const auto before = client.nozzle_offset(2);
    runner.start();
    run_to_end();
    REQUIRE(runner.phase() == Runner::Phase::Done);
    CHECK(client.nozzle_offset(2)[0] != Catch::Approx(before[0]));

    json status;
    client.send_jsonrpc(
        "printer.objects.query", offsets_query_params(), [&](const json& r) { status = r; },
        [](const MoonrakerError&) {});
    process_lvgl(50);
    const auto offsets = offsets_from_query(status);
    REQUIRE(offsets.has_value());
    CHECK((*offsets)[2][0] == Catch::Approx(client.nozzle_offset(2)[0]));

    // EXIT left the state: the next step is refused again.
    bool refused = false;
    client.send_jsonrpc(
        "printer.gcode.script",
        json{{"script", "EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T0"}}, [](const json&) {},
        [&](const MoonrakerError&) { refused = true; });
    process_lvgl(1300);
    CHECK(refused);
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: a sheet left on the bed is asked for again",
                 "[mock][snapmaker][nozzle_offset]") {
    setenv("HELIX_MOCK_NOZZLE_CAL_PLATE_REFUSE", "1", 1);
    runner.start();
    bool asked_again = false;
    for (int guard = 0; guard < 200 && runner.phase() != Runner::Phase::Done; ++guard) {
        if (runner.phase() == Runner::Phase::AwaitingUser) {
            asked_again = asked_again || !runner.error().empty();
            runner.confirm();
        }
        process_lvgl(1300);
    }
    CHECK(asked_again);
    CHECK(runner.phase() == Runner::Phase::Done);
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: a failed probe ends the run",
                 "[mock][snapmaker][nozzle_offset]") {
    setenv("HELIX_MOCK_NOZZLE_CAL_FAIL", "1", 1);
    runner.start();
    run_to_end();
    REQUIRE(runner.phase() == Runner::Phase::Failed);
    CHECK(runner.current_step()->tool == 1);
    CHECK(runner.error().find("tolerance") != std::string::npos);
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: reconcile ends a state a run here left",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    // Our run lost its EXIT to a disconnect.
    enter_state_elsewhere();
    mark_exit_pending();

    reconcile();
    CHECK_FALSE(client.nozzle_calibration_active());
    CHECK(asked == 0);
    CHECK_FALSE(exit_pending());
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: reconcile asks before ending a state no run here owns",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    enter_state_elsewhere();

    reconcile();
    CHECK(asked == 1);
    CHECK(client.nozzle_calibration_active());

    // "End calibration".
    send_exit(client);
    process_lvgl(1300);
    CHECK_FALSE(client.nozzle_calibration_active());
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: connect-time reconcile leaves a live run alone",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    runner.start();
    process_lvgl(1300);
    REQUIRE(client.nozzle_calibration_active());
    REQUIRE(run_live());
    mark_exit_pending();

    reconcile();
    CHECK(client.nozzle_calibration_active());
    CHECK(asked == 0);
    runner.cancel();
    process_lvgl(1300);
    CHECK_FALSE(client.nozzle_calibration_active());
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: leaving a live run before disconnect exits the state",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    client.connect("ws://mock/websocket", [] {}, [] {});
    runner.start();
    process_lvgl(1300);
    REQUIRE(client.nozzle_calibration_active());

    SECTION("connected: EXIT goes out and the run ends") {
        leave_live_run(&client);
        CHECK(runner.phase() == Runner::Phase::Cancelled);
        // The mock answers on this thread, so the bounded wait sees no reply.
        CHECK(exit_unconfirmed());
        CHECK_FALSE(exit_pending());
        process_lvgl(1300);
        CHECK_FALSE(client.nozzle_calibration_active());
        CHECK_FALSE(client.sent(kProbeAbortMethod));
        CHECK_FALSE(exit_pending());
    }
    SECTION("connected mid-probe: the pass is aborted first") {
        for (int guard = 0; guard < 200 && !runner.probing(); ++guard) {
            if (runner.phase() == Runner::Phase::AwaitingUser) {
                runner.confirm();
            }
            process_lvgl(1300);
        }
        REQUIRE(runner.probing());
        leave_live_run(&client);
        CHECK(client.sent(kProbeAbortMethod));
        process_lvgl(1300);
        CHECK_FALSE(client.nozzle_calibration_active());
    }
    SECTION("no client: the exit is left pending") {
        leave_live_run(nullptr);
        CHECK(exit_pending());
        CHECK(runner.phase() == Runner::Phase::Cancelled);
        CHECK(client.nozzle_calibration_active());
    }
    SECTION("an ended run sends nothing") {
        runner.cancel();
        process_lvgl(1300);
        client.methods.clear();
        leave_live_run(&client);
        CHECK(client.methods.empty());
    }
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: reconcile forgets a pending exit once the state is free",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    reconcile();
    mark_exit_pending();
    REQUIRE(exit_pending());

    // The state ended some other way while we were gone.
    reconcile();
    CHECK_FALSE(exit_pending());

    // So a later run from another screen is asked about, not ended.
    enter_state_elsewhere();
    reconcile();
    CHECK(asked == 1);
    CHECK(client.nozzle_calibration_active());
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: an exit sent but unconfirmed is asked about, not resent",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    reconcile();
    mark_exit_unconfirmed();
    REQUIRE_FALSE(exit_pending());

    // Another screen may have started its own run since our EXIT went out.
    enter_state_elsewhere();
    reconcile();
    CHECK(asked == 1);
    CHECK(client.nozzle_calibration_active());
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: a pending exit belongs to the printer it was left on",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    client.connect("ws://printer-a/websocket", [] {}, [] {});
    enter_state_elsewhere();
    reconcile();
    REQUIRE(asked == 1);
    mark_exit_pending();

    RecordingClient other{MoonrakerClientMock::PrinterType::SNAPMAKER_U1, 100.0};
    other.connect("ws://printer-b/websocket", [] {}, [] {});
    other.send_jsonrpc(
        "printer.gcode.script", json{{"script", "EXTRUDER_OFFSET_ACTION_PRESTART"}},
        [](const json&) {}, [](const MoonrakerError&) {});
    process_lvgl(1300);
    REQUIRE(other.nozzle_calibration_active());

    reconcile_on_connect(other, [this] { ++asked; });
    process_lvgl(1300);
    CHECK(asked == 2);
    CHECK(other.nozzle_calibration_active());

    // Back on the printer it was left on, it is ours again.
    reconcile();
    CHECK(asked == 2);
    CHECK_FALSE(client.nozzle_calibration_active());
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: connect-time reconcile sends nothing when idle",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    std::vector<std::string> console;
    client.register_method_callback("notify_gcode_response", "test_console",
                                    [&console](const json& msg) {
                                        for (const auto& p : msg.at("params")) {
                                            if (p.is_string()) {
                                                console.push_back(p.get<std::string>());
                                            }
                                        }
                                    });
    reconcile();
    client.unregister_method_callback("notify_gcode_response", "test_console");
    // An EXIT outside the state would be refused with the firmware's fault.
    for (const auto& line : console) {
        CHECK(line.find("0003-0530-0000-0009") == std::string::npos);
    }
    CHECK_FALSE(client.nozzle_calibration_active());
}

TEST_CASE_METHOD(NozzleCalFixture, "mock: teardown's before_disconnect ends the overlay's live run",
                 "[mock][snapmaker][nozzle_offset][reconcile]") {
    helix::PrinterState state;
    state.init_subjects(false);
    state.set_klippy_state_sync(helix::KlippyState::READY);
    MoonrakerAPIMock api{client, state};
    client.connect("ws://mock/websocket", [] {}, [] {});
    set_moonraker_api(&api);
    set_moonraker_client(&client);

    nlohmann::json objects = json::array({"gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART",
                                          "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT"});
    for (int t = 0; t < kHeads; ++t) {
        for (const char* step :
             {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN", "WAIT_COOL", "PROBE_CALIBRATE"}) {
            objects.push_back(std::string("gcode_macro _EXTRUDER_OFFSET_ACTION_") + step + "_T" +
                              std::to_string(t));
        }
    }
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    helix::AmsBackendSnapmaker backend(&api, &client);
    backend.set_discovery(hw);

    auto& overlay = helix::ui::get_snapmaker_nozzle_offset_overlay();
    overlay.runner().start();
    for (int guard = 0; guard < 20 && !client.nozzle_calibration_active(); ++guard) {
        process_lvgl(200);
    }
    REQUIRE(client.nozzle_calibration_active());
    REQUIRE(run_live());

    // PrinterSession::teardown_printer_scope reaches the backend through
    // AmsState::before_disconnect() while the client is still connected
    // (tests/shell/test_teardown_order_gate.bats pins that order).
    backend.before_disconnect();
    CHECK(overlay.runner().phase() == Runner::Phase::Cancelled);
    process_lvgl(1300);
    CHECK_FALSE(client.nozzle_calibration_active());

    helix::ui::UpdateQueue::instance().drain_until_empty();
    overlay.runner().reset();
    set_moonraker_client(nullptr);
    set_moonraker_api(nullptr);
}
