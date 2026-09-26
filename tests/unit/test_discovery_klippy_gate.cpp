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

#include "../lvgl_test_fixture.h"
#include "ams_types.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

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

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms, bool silent,
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
