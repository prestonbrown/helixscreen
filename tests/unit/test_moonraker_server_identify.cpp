// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_server_identify.cpp
 * @brief Unit tests for server.connection.identify functionality
 *
 * Tests the client identification flow that enables Moonraker to send
 * notifications like notify_filelist_changed to the client.
 */

#include "../../include/moonraker_client_mock.h"
#include "../../include/moonraker_discovery_sequence.h"
#include "../helix_test_fixture.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// server.connection.identify Mock Handler Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock handles server.connection.identify",
          "[moonraker][connection][identify]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});

    SECTION("Identify returns successful response with connection_id") {
        std::atomic<bool> callback_invoked{false};
        int connection_id = -1;

        json identify_params = {{"client_name", "TestClient"},
                                {"version", "1.0.0"},
                                {"type", "display"},
                                {"url", "https://example.com"}};

        mock.send_jsonrpc(
            "server.connection.identify", identify_params,
            [&](json response) {
                // Verify response structure
                REQUIRE(response.contains("result"));
                REQUIRE(response["result"].contains("connection_id"));
                connection_id = response["result"]["connection_id"].get<int>();
                callback_invoked.store(true);
            },
            [](const MoonrakerError& err) { FAIL("Error callback invoked: " << err.message); });

        // Give the mock time to invoke callback
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        REQUIRE(callback_invoked.load());
        REQUIRE(connection_id >= 1000); // Mock starts at 1000
    }

    SECTION("Multiple identify calls return unique connection IDs") {
        std::vector<int> connection_ids;
        std::atomic<int> callbacks_received{0};

        for (int i = 0; i < 3; i++) {
            json params = {{"client_name", "Test"}, {"version", "1.0"}, {"type", "display"}};

            mock.send_jsonrpc(
                "server.connection.identify", params,
                [&](json response) {
                    connection_ids.push_back(response["result"]["connection_id"].get<int>());
                    callbacks_received.fetch_add(1);
                },
                [](const MoonrakerError&) {});
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        REQUIRE(callbacks_received.load() == 3);
        REQUIRE(connection_ids.size() == 3);

        // Each ID should be unique
        std::set<int> unique_ids(connection_ids.begin(), connection_ids.end());
        REQUIRE(unique_ids.size() == 3);
    }

    mock.stop_temperature_simulation();
    mock.disconnect();
}

// ============================================================================
// Identify Integration with Discovery Flow
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "MoonrakerClientMock discover_printer doesn't fail due to identify",
                 "[moonraker][connection][discovery]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});

    SECTION("discover_printer completes successfully") {
        std::atomic<bool> discovery_complete{false};

        mock.discover_printer([&]() { discovery_complete.store(true); });

        // Mock's discover_printer is synchronous - callback should be invoked immediately
        REQUIRE(discovery_complete.load());

        // Verify discovery populated expected data
        REQUIRE_FALSE(mock.hardware().hostname().empty());
        REQUIRE_FALSE(mock.hardware().heaters().empty());
    }

    mock.stop_temperature_simulation();
    mock.disconnect();
}

// ============================================================================
// Identification State Tracking Tests
// ============================================================================

namespace {

/// Counts server.connection.identify sends while still serving them through the
/// mock's real handler registry, so the response the discovery sequence parses is
/// the same one every other test sees.
class IdentifyCountingClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "server.connection.identify") {
            ++identify_calls;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    int identify_calls = 0;
};

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "MoonrakerClient identification state tracking",
                 "[moonraker][connection][identify][state]") {
    // Drive MoonrakerDiscoverySequence directly rather than the mock's
    // discover_printer() override, which bypasses the identify step entirely.
    // start() is the only code that ever sets identified_, and its short-circuit
    // is the reason the flag exists: re-identifying an already-identified
    // connection makes Moonraker answer "Connection already identified".
    IdentifyCountingClient mock(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerDiscoverySequence discovery(mock);

    SECTION("is_identified starts false before any identify round-trip") {
        REQUIRE_FALSE(discovery.is_identified());
        REQUIRE(mock.identify_calls == 0);
    }

    SECTION("a successful identify response sets the flag") {
        mock.connect("ws://mock/websocket", []() {}, []() {});

        discovery.start([]() {}, [](const std::string&) {});

        REQUIRE(mock.identify_calls == 1);
        REQUIRE(discovery.is_identified());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("a second discovery on the same connection skips identify") {
        mock.connect("ws://mock/websocket", []() {}, []() {});

        discovery.start([]() {}, [](const std::string&) {});
        REQUIRE(mock.identify_calls == 1);
        REQUIRE(discovery.is_identified());

        // Wizard-tests-then-finishes, or any rediscovery: the flag must short-circuit
        // the identify step instead of sending a second one.
        discovery.start([]() {}, [](const std::string&) {});
        REQUIRE(mock.identify_calls == 1);
        REQUIRE(discovery.is_identified());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("reset_identified clears the flag so the next connection re-identifies") {
        mock.connect("ws://mock/websocket", []() {}, []() {});

        discovery.start([]() {}, [](const std::string&) {});
        REQUIRE(discovery.is_identified());

        // MoonrakerClient calls this on every disconnect — a new WebSocket is a new
        // Moonraker connection and must identify again.
        discovery.reset_identified();
        REQUIRE_FALSE(discovery.is_identified());

        discovery.start([]() {}, [](const std::string&) {});
        REQUIRE(mock.identify_calls == 2);
        REQUIRE(discovery.is_identified());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}
