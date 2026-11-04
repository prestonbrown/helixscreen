// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_discovery_klippy_gate.cpp
 * @brief Tests for Klippy-readiness gate in the discovery sequence
 *
 * Verifies that the real MoonrakerDiscoverySequence checks klippy_state
 * via server.info BEFORE calling printer.objects.list, and aborts
 * discovery when Klippy is not ready (STARTUP/ERROR states).
 */

#include "../lvgl_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "standard_macros.h"

#include <atomic>
#include <string>

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
    void discover_printer_real(std::function<void()> on_complete,
                               std::function<void(const std::string&)> on_error) {
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

/// The real discovery sequence with @c extra_settings merged into every
/// configfile.settings reply the mock serves.
class ConfigfileSettingsClient : public TestDiscoveryClient {
  public:
    using MoonrakerClientMock::send_jsonrpc;
    using TestDiscoveryClient::TestDiscoveryClient;

    helix::RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 std::function<void(const json&)> success_cb,
                 std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms,
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

TEST_CASE("A Centauri Carbon's Bed Screws runs the wrapped builtin, or stock without a wrapper",
          "[discovery][klippy_gate][screws_tilt]") {
    LVGLTestFixture fixture;
    ConfigfileSettingsClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    helix::PrinterState state;
    state.init_subjects(false);
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
