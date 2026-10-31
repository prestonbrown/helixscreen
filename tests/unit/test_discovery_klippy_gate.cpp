// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_discovery_klippy_gate.cpp
 * @brief Tests for Klippy-readiness gate in the discovery sequence
 *
 * Verifies that the real MoonrakerDiscoverySequence checks klippy_state
 * via server.info BEFORE calling printer.objects.list, and aborts
 * discovery when Klippy is not ready (STARTUP/ERROR states).
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "ams_types.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_auth.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

/**
 * @brief Test subclass that exposes the base class discover_printer()
 *
 * MoonrakerClientMock overrides discover_printer() with mock logic.
 * This subclass provides access to the REAL discovery sequence
 * (MoonrakerClient::discover_printer → discovery_.start()) while
 * still routing send_jsonrpc() through the mock handler dispatch.
 */
class TestDiscoveryClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    /**
     * @brief Run the REAL discovery sequence (not the mock override)
     *
     * Calls MoonrakerClient::discover_printer which calls discovery_.start(),
     * exercising the real discovery sequence code. send_jsonrpc() calls
     * within the sequence are dispatched through the mock handler registry.
     */
    void discover_printer_real(helix::SharedCallback<void()> on_complete,
                               helix::SharedCallback<void(const std::string&)> on_error) {
        MoonrakerClient::discover_printer(on_complete, on_error);
    }
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("Discovery succeeds when Klippy is ready", "[discovery][klippy_gate]") {
    LVGLTestFixture fixture;

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    bool completed = false;
    bool errored = false;
    std::string error_reason;

    client.discover_printer_real([&completed]() { completed = true; },
                                 [&errored, &error_reason](const std::string& reason) {
                                     errored = true;
                                     error_reason = reason;
                                 });

    REQUIRE(completed);
    REQUIRE_FALSE(errored);
}

TEST_CASE("Discovery aborts when Klippy in STARTUP state", "[discovery][klippy_gate]") {
    LVGLTestFixture fixture;

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::STARTUP);

    bool completed = false;
    bool errored = false;
    std::string error_reason;

    client.discover_printer_real([&completed]() { completed = true; },
                                 [&errored, &error_reason](const std::string& reason) {
                                     errored = true;
                                     error_reason = reason;
                                 });

    REQUIRE_FALSE(completed);
    REQUIRE(errored);
    REQUIRE(error_reason.find("startup") != std::string::npos);
}

TEST_CASE("Discovery aborts when Klippy in ERROR state", "[discovery][klippy_gate]") {
    LVGLTestFixture fixture;

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::ERROR);

    bool completed = false;
    bool errored = false;
    std::string error_reason;

    client.discover_printer_real([&completed]() { completed = true; },
                                 [&errored, &error_reason](const std::string& reason) {
                                     errored = true;
                                     error_reason = reason;
                                 });

    REQUIRE_FALSE(completed);
    REQUIRE(errored);
    REQUIRE(error_reason.find("error") != std::string::npos);
}

TEST_CASE("Discovery succeeds when Klippy in SHUTDOWN state", "[discovery][klippy_gate]") {
    LVGLTestFixture fixture;

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::SHUTDOWN);

    bool completed = false;
    bool errored = false;
    std::string error_reason;

    client.discover_printer_real([&completed]() { completed = true; },
                                 [&errored, &error_reason](const std::string& reason) {
                                     errored = true;
                                     error_reason = reason;
                                 });

    REQUIRE(completed);
    REQUIRE_FALSE(errored);
}

TEST_CASE("Discovery onto a shut-down Klippy seeds the reason with the state",
          "[discovery][klippy_gate]") {
    LVGLTestFixture fixture;
    get_printer_state().init_subjects(false);
    get_printer_state().network_state().reset_klippy_state_freshness();

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::SHUTDOWN);

    bool completed = false;
    client.discover_printer_real([&completed]() { completed = true; }, [](const std::string&) {});
    REQUIRE(completed);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(static_cast<helix::KlippyState>(lv_subject_get_int(
                get_printer_state().network_state().get_klippy_state_subject())) ==
            helix::KlippyState::SHUTDOWN);
    // The mock's printer.info reason for a shutdown.
    REQUIRE(get_printer_state().network_state().get_klippy_fault_message() ==
            "Printer has been shut down");
}

TEST_CASE("Discovery does not call printer.objects.list when Klippy not ready",
          "[discovery][klippy_gate]") {
    LVGLTestFixture fixture;

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::STARTUP);

    // Track whether printer.objects.list was called by counting send_jsonrpc calls
    // The mock handler dispatch already tracks this implicitly - if Klippy is STARTUP
    // and the gate works, printer.objects.list should never be called.
    // We verify this by checking that the error callback fires with a Klippy-related
    // message (not a "Method not found" from printer.objects.list failing).
    bool errored = false;
    std::string error_reason;

    client.discover_printer_real(
        []() { FAIL("Discovery should not succeed when Klippy is in STARTUP"); },
        [&errored, &error_reason](const std::string& reason) {
            errored = true;
            error_reason = reason;
        });

    REQUIRE(errored);
    // The error should mention "Klippy not ready" (from the gate),
    // NOT "Method not found" (from printer.objects.list failing)
    REQUIRE(error_reason.find("Klippy not ready") != std::string::npos);
    REQUIRE(error_reason.find("Method not found") == std::string::npos);
}

// ============================================================================
// Status claims settled before the hardware callback (OpenAMS)
// ============================================================================

namespace {

/// The real discovery sequence against a printer that lists oams_manager
/// as its only filament system. The claim query is answered with @c claim_reply as the
/// manager's status, or failed while it is null. Every method sent is
/// recorded in order, and the subscribe request's params are kept.
class ClaimQueryClient : public TestDiscoveryClient {
  public:
    using MoonrakerClientMock::send_jsonrpc;
    using TestDiscoveryClient::TestDiscoveryClient;

    helix::RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms,
                 bool silent,
                 std::optional<helix::rpc_error_policy::CallerIntent> intent) override {
        const bool claim_query = method == "printer.objects.query" && params.contains("objects") &&
                                 params["objects"].contains("oams_manager");
        methods.push_back(claim_query ? "claim_query" : method);
        if (claim_query) {
            if (claim_reply.is_null()) {
                MoonrakerError error;
                error.message = "Request timed out";
                error_cb(error);
            } else {
                success_cb(json{{"result", {{"status", {{"oams_manager", claim_reply}}}}}});
            }
            return 1;
        }
        if (method == "printer.objects.list" && success_cb) {
            success_cb = [inner = std::move(success_cb)](const json& response) {
                // The mock ships Happy Hare's "mmu", which would claim the
                // printer before OpenAMS is asked.
                json listed = response;
                json objects = json::array();
                for (const auto& object : listed["result"]["objects"]) {
                    if (object != "mmu") {
                        objects.push_back(object);
                    }
                }
                objects.push_back("oams_manager");
                listed["result"]["objects"] = objects;
                inner(listed);
            };
        }
        if (method == "printer.objects.subscribe") {
            subscribe_params = params;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    [[nodiscard]] long index_of(const std::string& method) const {
        auto it = std::find(methods.begin(), methods.end(), method);
        return it == methods.end() ? -1 : static_cast<long>(it - methods.begin());
    }

    json claim_reply;
    std::vector<std::string> methods;
    json subscribe_params;
};

struct ClaimRun {
    int callbacks = 0;
    std::vector<helix::AmsType> ams_types;
    bool completed = false;
};

ClaimRun run_discovery(ClaimQueryClient& client) {
    ClaimRun run;
    client.set_on_hardware_discovered([&run](const helix::PrinterDiscovery& hw) {
        ++run.callbacks;
        run.ams_types.clear();
        for (const auto& system : hw.detected_ams_systems()) {
            run.ams_types.push_back(system.type);
        }
    });
    client.discover_printer_real([&run]() { run.completed = true; },
                                 [](const std::string& reason) { FAIL(reason); });
    return run;
}

} // namespace

TEST_CASE("Discovery settles the OpenAMS claim before the hardware callback",
          "[discovery][klippy_gate][openams]") {
    LVGLTestFixture fixture;
    ClaimQueryClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    SECTION("a v1 manager claims the printer and is subscribed") {
        client.claim_reply = json{{"api_version", 1}, {"schema", "openams.manager"}};
        const ClaimRun run = run_discovery(client);

        REQUIRE(run.completed);
        REQUIRE(client.index_of("claim_query") >= 0);
        // Sent straight from the object list, ahead of every later request.
        CHECK(client.index_of("claim_query") == client.index_of("printer.objects.list") + 1);
        CHECK(client.index_of("claim_query") < client.index_of("printer.objects.subscribe"));
        CHECK(run.callbacks == 1);
        CHECK(run.ams_types == std::vector<helix::AmsType>{helix::AmsType::OPENAMS});
        REQUIRE(client.subscribe_params.contains("objects"));
        CHECK(client.subscribe_params["objects"].contains("oams_manager"));
    }

    SECTION("a failed query leaves it unclaimed and still fires the callback") {
        client.claim_reply = nullptr;
        const ClaimRun run = run_discovery(client);

        REQUIRE(run.completed);
        CHECK(run.callbacks == 1);
        CHECK(run.ams_types.empty());
        CHECK_FALSE(client.subscribe_params["objects"].contains("oams_manager"));
    }

    SECTION("a manager that predates the API leaves it unclaimed") {
        client.claim_reply = json{{"current_group", "T0"}};
        const ClaimRun run = run_discovery(client);

        REQUIRE(run.completed);
        CHECK(run.callbacks == 1);
        CHECK(run.ams_types.empty());
        CHECK_FALSE(client.subscribe_params["objects"].contains("oams_manager"));
    }
}

// ============================================================================
// Sensor toggle command resolved from configfile.settings
// ============================================================================

namespace {

/// The real discovery sequence with @c extra_settings merged into every
/// configfile.settings reply the mock serves.
class ConfigfileSettingsClient : public TestDiscoveryClient {
  public:
    using MoonrakerClientMock::send_jsonrpc;
    using TestDiscoveryClient::TestDiscoveryClient;

    helix::RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms,
                 bool silent,
                 std::optional<helix::rpc_error_policy::CallerIntent> intent) override {
        if (method == "printer.objects.query" && success_cb && params.contains("objects") &&
            params["objects"].contains("configfile")) {
            success_cb = [inner = std::move(success_cb), extra = extra_settings](const json& r) {
                json patched = r;
                auto& status = patched["result"]["status"];
                if (status.contains("configfile") && status["configfile"].contains("settings")) {
                    status["configfile"]["settings"].merge_patch(extra);
                }
                inner(patched);
            };
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    json extra_settings = json::object();
};

std::string discovered_toggle_command(ConfigfileSettingsClient& client) {
    std::string command;
    bool completed = false;
    client.set_on_discovery_complete(
        [&command](const helix::PrinterDiscovery& hw, const json& /*initial_status*/) {
            command = hw.sensor_toggle_command();
        });
    client.discover_printer_real([&completed]() { completed = true; },
                                 [](const std::string& reason) { FAIL(reason); });
    REQUIRE(completed);
    return command;
}

} // namespace

TEST_CASE("Discovery resolves the sensor toggle command from configfile.settings",
          "[discovery][klippy_gate][filament]") {
    LVGLTestFixture fixture;
    ConfigfileSettingsClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    SECTION("a SET_FILAMENT_SENSOR wrapper names the renamed builtin") {
        client.extra_settings = json{
            {"gcode_macro set_filament_sensor", {{"rename_existing", "_SET_FILAMENT_SENSOR"}}}};
        CHECK(discovered_toggle_command(client) == "_SET_FILAMENT_SENSOR");
    }

    SECTION("no wrapper keeps the builtin") {
        CHECK(discovered_toggle_command(client) == "SET_FILAMENT_SENSOR");
    }
}

TEST_CASE("Discovery reads whether the firmware starts the print before the file",
          "[discovery][klippy_gate][start_macro]") {
    LVGLTestFixture fixture;
    ConfigfileSettingsClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    auto discover = [&client]() {
        bool completed = false;
        client.discover_printer_real([&completed]() { completed = true; },
                                     [](const std::string& reason) { FAIL(reason); });
        REQUIRE(completed);
        return client.hardware().firmware_runs_start_macro_before_file();
    };

    SECTION("OpenCreator's virtual_sdcard runs C5_PRINT_START before the file") {
        client.extra_settings = json{{"virtual_sdcard", {{"auto_creator5_start", true}}}};
        CHECK(discover());
    }

    SECTION("a stock virtual_sdcard streams the file from its first line") {
        CHECK_FALSE(discover());
    }
}

TEST_CASE("A Centauri Carbon's Bed Screws runs the wrapped builtin, or stock without a wrapper",
          "[discovery][klippy_gate][screws_tilt]") {
    LVGLTestFixture fixture;
    ConfigfileSettingsClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    helix::PrinterState state;
    state.init_subjects(false);
    state.set_klippy_state_sync(helix::KlippyState::READY);
    MoonrakerAPI api(client, state);

    // Every line sent for one Bed Screws press; the mock records a script line by line.
    auto run_bed_screws = [&]() {
        bool completed = false;
        client.discover_printer_real([&completed]() { completed = true; },
                                     [](const std::string& reason) { FAIL(reason); });
        REQUIRE(completed);
        StandardMacros::instance().init(client.hardware(), "Elegoo Centauri Carbon");
        client.clear_gcode_script_history();
        api.advanced().calculate_screws_tilt([](const std::vector<ScrewTiltResult>&) {},
                                             [](const MoonrakerError&) {});
        StandardMacros::instance().reset();
        return client.gcode_script_history();
    };

    SECTION("COSMOS's wrapper is bypassed: tare, its builtin, park") {
        client.extra_settings = json{{"gcode_macro screws_tilt_calculate",
                                      {{"rename_existing", "SCREWS_TILT_CALCULATE_BASE"}}}};
        CHECK(run_bed_screws() ==
              std::vector<std::string>{"LOAD_CELL_CALIBRATE TARE=TRUE SAVE=TRUE",
                                       "SCREWS_TILT_CALCULATE_BASE", "G90",
                                       "G1 X128 Y220 Z20 F6000"});
    }

    SECTION("without the wrapper the stock command runs") {
        CHECK(run_bed_screws() == std::vector<std::string>{"SCREWS_TILT_CALCULATE"});
    }

    SECTION("a quick-action button for the slot sends the same sequence") {
        client.extra_settings = json{{"gcode_macro screws_tilt_calculate",
                                      {{"rename_existing", "SCREWS_TILT_CALCULATE_BASE"}}}};
        bool completed = false;
        client.discover_printer_real([&completed]() { completed = true; },
                                     [](const std::string& reason) { FAIL(reason); });
        REQUIRE(completed);
        api.hardware() = client.hardware();
        StandardMacros::instance().init(api.hardware(), "Elegoo Centauri Carbon");
        client.clear_gcode_script_history();

        const bool sent = StandardMacros::instance().execute(StandardMacroSlot::ScrewsTilt, &api,
                                                             nullptr, nullptr);
        StandardMacros::instance().reset();
        REQUIRE(sent);
        CHECK(client.gcode_script_history() ==
              std::vector<std::string>{"LOAD_CELL_CALIBRATE TARE=TRUE SAVE=TRUE",
                                       "SCREWS_TILT_CALCULATE_BASE", "G90",
                                       "G1 X128 Y220 Z20 F6000"});
    }
}

TEST_CASE("Discovery reads the printer's fit-check limits from its config",
          "[discovery][plugin_fitcheck]") {
    LVGLTestFixture fixture;

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    REQUIRE_FALSE(client.hardware().fit_machine().has_value());

    bool completed = false;
    client.discover_printer_real([&completed]() { completed = true; }, [](const std::string&) {});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(completed);

    // Unset limits read as "config unread", which judges no file at all.
    const helix::PrinterDiscovery hw = client.hardware();
    const auto& machine = hw.fit_machine();
    REQUIRE(machine.has_value());
    CHECK_FALSE(machine->kinematics.empty());
    CHECK(machine->axis_max[0].value_or(0.0) > 0.0);
}

TEST_CASE("Discovery stops at a refused login and reports it once", "[discovery][api-key]") {
    LVGLTestFixture fixture;
    helix::ScopedEnv force_logins("HELIX_MOCK_FORCE_LOGINS", "1");
    helix::moonraker_auth::set_credential({"printer.lan", 7125, ""});

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    std::vector<MoonrakerEventType> events;
    client.register_event_handler([&events](const MoonrakerEvent& e) { events.push_back(e.type); });

    for (int attempt = 0; attempt < 2; ++attempt) {
        bool completed = false;
        std::string error_reason;
        client.discover_printer_real(
            [&completed]() { completed = true; },
            [&error_reason](const std::string& reason) { error_reason = reason; });
        INFO("attempt " << attempt);
        CHECK_FALSE(completed);
        CHECK(error_reason == "Unauthorized");
    }

    // One report for the refusal, and no other error behind it: discovery asked nothing more.
    CHECK(events == std::vector<MoonrakerEventType>{MoonrakerEventType::LOGIN_REFUSED});
    CHECK(helix::moonraker_auth::unauthorized());
    helix::moonraker_auth::set_credential({});
}

TEST_CASE("A login fixed on the Moonraker side clears the refusal on the next identify",
          "[discovery][api-key]") {
    LVGLTestFixture fixture;
    helix::moonraker_auth::set_credential({"printer.lan", 7125, ""});
    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    {
        helix::ScopedEnv force_logins("HELIX_MOCK_FORCE_LOGINS", "1");
        client.discover_printer_real([]() {}, [](const std::string&) {});
        REQUIRE(helix::moonraker_auth::unauthorized());
    }

    // trusted_clients now admits the screen; the reconnect keeps the same credential.
    bool completed = false;
    client.discover_printer_real([&completed]() { completed = true; }, [](const std::string&) {});
    CHECK(completed);
    CHECK_FALSE(helix::moonraker_auth::unauthorized());
    helix::moonraker_auth::set_credential({});
}
