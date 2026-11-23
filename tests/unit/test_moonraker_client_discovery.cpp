// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_client_discovery.cpp
 * @brief Tests for MoonrakerClient discover_printer() error callback
 *
 * These tests verify that:
 * 1. Discovery error callback is invoked when Klippy is not connected
 * 2. Discovery success callback works normally when Klippy is ready
 * 3. Error messages are descriptive and contain relevant info
 */

#include "../../include/moonraker_client_mock.h"
#include "../helix_test_fixture.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace std::chrono;

// Discovery Error Callback Tests

TEST_CASE_METHOD(HelixTestFixture, "MoonrakerClientMock: discover_printer error callback",
                 "[moonraker][discovery][callback]") {
    SECTION("Error callback invoked when Klippy in STARTUP state") {
        MoonrakerClientMock client;

        // Set Klippy to STARTUP state (simulates Klippy not connected)
        client.set_klippy_state(MoonrakerClientMock::KlippyState::STARTUP);

        std::atomic<bool> success_called{false};
        std::atomic<bool> error_called{false};
        std::string error_reason;

        client.discover_printer([&success_called]() { success_called.store(true); },
                                [&error_called, &error_reason](const std::string& reason) {
                                    error_called.store(true);
                                    error_reason = reason;
                                });

        REQUIRE(error_called.load() == true);
        REQUIRE(success_called.load() == false);
        REQUIRE(error_reason.find("Klippy") != std::string::npos);
    }

    SECTION("Error callback invoked when Klippy in ERROR state") {
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::ERROR);

        std::atomic<bool> success_called{false};
        std::atomic<bool> error_called{false};

        client.discover_printer([&success_called]() { success_called.store(true); },
                                [&error_called](const std::string&) { error_called.store(true); });

        REQUIRE(error_called.load() == true);
        REQUIRE(success_called.load() == false);
    }

    SECTION("Success callback invoked when Klippy is READY") {
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

        std::atomic<bool> success_called{false};
        std::atomic<bool> error_called{false};

        client.discover_printer([&success_called]() { success_called.store(true); },
                                [&error_called](const std::string&) { error_called.store(true); });

        REQUIRE(success_called.load() == true);
        REQUIRE(error_called.load() == false);
    }

    SECTION("Success callback invoked when Klippy is SHUTDOWN (emergency stop)") {
        // SHUTDOWN state means Klippy connected but M112 was triggered
        // Discovery should still work since Moonraker can communicate
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::SHUTDOWN);

        std::atomic<bool> success_called{false};
        std::atomic<bool> error_called{false};

        client.discover_printer([&success_called]() { success_called.store(true); },
                                [&error_called](const std::string&) { error_called.store(true); });

        REQUIRE(success_called.load() == true);
        REQUIRE(error_called.load() == false);
    }

    SECTION("Error reason contains descriptive message") {
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::STARTUP);

        std::string error_reason;

        client.discover_printer(
            []() {}, [&error_reason](const std::string& reason) { error_reason = reason; });

        REQUIRE(error_reason.empty() == false);
        REQUIRE(error_reason.find("Klippy not ready") != std::string::npos);
        REQUIRE(error_reason.find("startup") != std::string::npos);
    }

    SECTION("No crash when error callback is nullptr") {
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::STARTUP);

        // Should not crash when no error callback is provided
        REQUIRE_NOTHROW(client.discover_printer([]() {}, nullptr));
    }

    SECTION("No crash when both callbacks are nullptr") {
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::STARTUP);

        // Should not crash when no callbacks are provided
        REQUIRE_NOTHROW(client.discover_printer(nullptr, nullptr));
    }

    SECTION("Hardware not populated on error") {
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::STARTUP);

        // Clear any pre-existing hardware
        // Note: This relies on the mock not populating hardware on error

        std::atomic<bool> error_called{false};
        client.discover_printer([]() {},
                                [&error_called](const std::string&) { error_called.store(true); });

        REQUIRE(error_called.load() == true);

        // Hardware shouldn't be populated when discovery fails
        REQUIRE(client.hardware().hostname().empty() == true);
    }
}

// Backwards compatibility

TEST_CASE_METHOD(HelixTestFixture, "MoonrakerClientMock: discover_printer regression tests",
                 "[moonraker][discovery][regression]") {
    SECTION("Discovery completes normally without error callback (backwards compatibility)") {
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

        std::atomic<bool> success_called{false};

        // Call with only success callback (no error callback)
        client.discover_printer([&success_called]() { success_called.store(true); });

        REQUIRE(success_called.load() == true);
    }

    SECTION("Hardware is populated on successful discovery") {
        MoonrakerClientMock client;
        client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

        std::atomic<bool> success_called{false};
        client.discover_printer([&success_called]() { success_called.store(true); });

        REQUIRE(success_called.load() == true);

        // Default mock has heaters, sensors, etc.
        REQUIRE(client.hardware().heaters().empty() == false);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "MoonrakerClientMock: discovery callbacks may touch the mock's hardware lists",
                 "[moonraker][discovery][mock]") {
    // Each discovery runs on a detached thread so a self-deadlock fails the
    // test instead of hanging it; the thread shares ownership of the client.
    auto run_discovery = [](MoonrakerClientMock::KlippyState state) {
        auto client = std::make_shared<MoonrakerClientMock>();
        client->set_klippy_state(state);
        auto finished = std::make_shared<std::promise<void>>();
        auto done = finished->get_future();
        std::thread([client, finished]() {
            // Raw pointer: the client keeps its completion callback, so a
            // shared_ptr captured there would keep the client alive forever.
            MoonrakerClientMock* raw = client.get();
            client->discover_printer([raw]() { raw->set_fans({"fan"}); },
                                     [raw](const std::string&) { raw->set_heaters({"extruder"}); });
            finished->set_value();
        }).detach();
        return done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    };

    SECTION("on_complete") {
        CHECK(run_discovery(MoonrakerClientMock::KlippyState::READY));
    }
    SECTION("on_error") {
        CHECK(run_discovery(MoonrakerClientMock::KlippyState::STARTUP));
    }
}
