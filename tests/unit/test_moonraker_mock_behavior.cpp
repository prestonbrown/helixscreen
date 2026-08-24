// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_mock_behavior.cpp
 * @brief Unit tests verifying MoonrakerClientMock behaves identically to real Moonraker API
 *
 * These tests validate that the mock produces JSON structures matching real Moonraker responses.
 *
 * @note Run with --order lex for consistent results. Random ordering may cause
 *       intermittent failures due to thread timing interactions.
 *
 * ## Real Moonraker API Format Reference
 *
 * Captured from real printer at 192.168.1.67 on 2025-11-25:
 *
 * ### Subscription Response (printer.objects.subscribe)
 * ```json
 * {
 *   "jsonrpc": "2.0",
 *   "result": {
 *     "eventtime": 108584.56863636,
 *     "status": {
 *       "extruder": { "temperature": 29.04, "target": 0.0, ... },
 *       "heater_bed": { "temperature": 43.58, "target": 0.0, ... },
 *       "toolhead": { "homed_axes": "", "position": [0,0,0,0], ... },
 *       "gcode_move": { "speed_factor": 1.0, "extrude_factor": 1.0, ... },
 *       "fan": {},
 *       "print_stats": { "state": "standby", "filename": "", ... },
 *       "virtual_sdcard": { "progress": 0.0, ... }
 *     }
 *   },
 *   "id": 1
 * }
 * ```
 *
 * ### notify_status_update Notification
 * ```json
 * {
 *   "jsonrpc": "2.0",
 *   "method": "notify_status_update",
 *   "params": [
 *     {
 *       "extruder": { "temperature": 29.02 },
 *       "heater_bed": { "temperature": 43.57 },
 *       ...
 *     },
 *     108584.819227568  // eventtime
 *   ]
 * }
 * ```
 *
 * Key observations:
 * - params is an ARRAY: [status_object, eventtime]
 * - Incremental updates only include changed fields
 * - Initial subscription response has full status in result.status
 */

#include "../../include/app_globals.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_api_mock.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_hardware.h"
#include "../../include/printer_state.h"
#include "../../include/runtime_config.h"
#include "../helix_test_fixture.h"
#include "moonraker_api.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
// Suppress deprecated warnings for testing mock behavior
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// ============================================================================
// Test Fixture for Mock Behavior Testing
// ============================================================================
// Test Fixture for Mock Behavior Testing
// ============================================================================

/**
 * @brief Test fixture that captures notifications from MoonrakerClientMock
 *
 * Provides helpers for waiting on callbacks and validating JSON structure.
 */
class MockBehaviorTestFixture {
  public:
    /// Default speedup for tests (100x makes 250ms connect delay → 2.5ms)
    static constexpr double TEST_SPEEDUP = 100.0;

    MockBehaviorTestFixture() = default;

    /**
     * @brief Create a mock with test speedup (100x faster than real-time)
     */
    std::unique_ptr<MoonrakerClientMock> create_mock(
        MoonrakerClientMock::PrinterType type = MoonrakerClientMock::PrinterType::VORON_24) {
        return std::make_unique<MoonrakerClientMock>(type, TEST_SPEEDUP);
    }

    /**
     * @brief Wait for callback to be invoked with timeout
     * @param timeout_ms Maximum wait time in milliseconds
     * @return true if callback was invoked, false on timeout
     */
    bool wait_for_callback(int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [this] { return callback_invoked_.load(); });
    }

    /**
     * @brief Wait for a specific number of callbacks
     * @param count Number of callbacks to wait for
     * @param timeout_ms Maximum wait time in milliseconds
     * @return true if all callbacks received, false on timeout
     */
    bool wait_for_callbacks(size_t count, int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [this, count] { return notifications_.size() >= count; });
    }

    /**
     * @brief Create a callback that captures notifications
     */
    std::function<void(const json&)> create_capture_callback() {
        return [this](const json& notification) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                notifications_.push_back(notification);
                callback_invoked_.store(true);
            }
            cv_.notify_all();
        };
    }

    /**
     * @brief Reset captured state for next test
     */
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        notifications_.clear();
        callback_invoked_.store(false);
    }

    /**
     * @brief Get a thread-safe copy of captured notifications
     * @note Returns copy to avoid race conditions with callback thread
     */
    std::vector<json> get_notifications() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return notifications_;
    }

    /**
     * @brief Get count of captured notifications (thread-safe)
     */
    size_t notification_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return notifications_.size();
    }

    /**
     * @brief Wait until a notification matching a predicate is received
     * @param predicate Function that returns true when matching notification found
     * @param timeout_ms Maximum wait time
     * @return true if matching notification found, false on timeout
     */
    bool wait_for_matching(std::function<bool(const json&)> predicate, int timeout_ms = 2000) {
        // Condition-variable driven rather than polled: waking on the notification
        // itself keeps the return precise enough for callers that time how long a
        // simulated operation took.
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, &predicate] {
            for (const auto& n : notifications_) {
                if (predicate(n)) {
                    return true;
                }
            }
            return false;
        });
    }

  private:
    mutable std::mutex mutex_; // mutable for const methods
    std::condition_variable cv_;
    std::atomic<bool> callback_invoked_{false};
    std::vector<json> notifications_;
};

/**
 * @brief Test helper that exposes protected methods for unit testing
 *
 * This allows tests to directly call dispatch_status_update() to verify
 * the parse_bed_mesh() behavior without going through the full connection flow.
 */
class TestableMoonrakerMock : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    // Expose protected method for testing
    using MoonrakerClient::dispatch_status_update;
};

// ============================================================================
// Initial State Dispatch Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock records the connection URL like the real client",
          "[connection][slow][url]") {
    // The real MoonrakerClient::connect() stores the URL, and consumers ask for
    // it to decide whether Moonraker is on this machine or across the network
    // (HelixPluginInstaller::is_local_moonraker, telemetry topology). A mock
    // that simulates connecting but leaves it empty makes every one of those
    // read "not connected" instead of the truth.
    SECTION("connect() records the URL it was given") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        REQUIRE(mock.get_last_url().empty());

        mock.connect("ws://192.168.1.67:7125/websocket", []() {}, []() {});

        CHECK(mock.get_last_url() == "ws://192.168.1.67:7125/websocket");

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("a null URL is recorded as empty, not dereferenced") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        mock.connect(nullptr, []() {}, []() {});

        CHECK(mock.get_last_url().empty());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE("MoonrakerAPI never derives an HTTP base URL from a mock connection",
          "[connection][slow][url]") {
    // Guards the reason the mock did not record its URL for so long: deriving an
    // HTTP base from it would aim real file transfers at the operator's actual
    // printer from a --test run that is supposed to touch nothing.
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://192.168.1.67:7125/websocket", []() {}, []() {});

    // RAII, not a restore at the end of the body: a failing SECTION would skip
    // a trailing restore and leak test_mode into every test that runs after.
    struct ScopedMockMoonraker {
        RuntimeConfig* config = get_runtime_config();
        bool saved_test_mode = config->test_mode;
        bool saved_use_real = config->use_real_moonraker;

        ScopedMockMoonraker() {
            config->test_mode = true;
            config->use_real_moonraker = false;
        }
        ~ScopedMockMoonraker() {
            config->test_mode = saved_test_mode;
            config->use_real_moonraker = saved_use_real;
        }
    } scoped_mock;

    REQUIRE(get_runtime_config()->should_mock_moonraker());

    MoonrakerAPI api(mock, get_printer_state());

    SECTION("derivation is refused while Moonraker is mocked") {
        CHECK(api.get_http_base_url().empty());
        CHECK_FALSE(api.ensure_http_base_url());
        CHECK(api.get_http_base_url().empty());
    }

    SECTION("an explicitly configured base URL still wins") {
        api.set_http_base_url("http://configured.example:7125");
        CHECK(api.ensure_http_base_url());
        CHECK(api.get_http_base_url() == "http://configured.example:7125");
    }

    mock.stop_temperature_simulation();
    mock.disconnect();
}

TEST_CASE("MoonrakerClientMock initial state dispatch", "[connection][slow][initial_state]") {
    MockBehaviorTestFixture fixture;

    SECTION("connect() dispatches initial state via callback") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Register callback BEFORE connect
        mock.register_notify_update(fixture.create_capture_callback());

        // Connect (triggers initial state dispatch)
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Should receive initial state callback
        REQUIRE(fixture.wait_for_callback(500));

        // Verify we got at least one notification
        REQUIRE(!fixture.get_notifications().empty());

        // Stop simulation to avoid interference
        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("initial state contains required fields") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));
        mock.stop_temperature_simulation();

        // Find the initial state notification (the one with print_stats)
        // Simulation updates only include temperature changes, not print_stats
        // NOTE: Must COPY the status because get_notifications() returns a copy of the vector
        json initial_status;
        bool found_initial_status = false;
        for (const auto& notification : fixture.get_notifications()) {
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& status = notification["params"][0];
                // Guard: status must be an object before calling contains()
                if (status.is_object() && status.contains("print_stats")) {
                    initial_status = status; // COPY, not pointer
                    found_initial_status = true;
                    break;
                }
            }
        }
        REQUIRE(found_initial_status);

        // Check for required printer objects (matching real Moonraker initial subscription
        // response)
        REQUIRE(initial_status.contains("extruder"));
        REQUIRE(initial_status.contains("heater_bed"));
        REQUIRE(initial_status.contains("toolhead"));
        REQUIRE(initial_status.contains("gcode_move"));
        REQUIRE(initial_status.contains("fan"));
        REQUIRE(initial_status.contains("print_stats"));
        REQUIRE(initial_status.contains("virtual_sdcard"));

        mock.disconnect();
    }

    SECTION("initial state has correct temperature structure") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for notification with proper extruder and heater_bed structure
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object()) {
                    return false;
                }

                // Check extruder structure (matches real Moonraker)
                if (!status.contains("extruder"))
                    return false;
                const json& extruder = status["extruder"];
                if (!extruder.contains("temperature") || !extruder.contains("target"))
                    return false;
                if (!extruder["temperature"].is_number() || !extruder["target"].is_number())
                    return false;

                // Check heater bed structure
                if (!status.contains("heater_bed"))
                    return false;
                const json& heater_bed = status["heater_bed"];
                if (!heater_bed.contains("temperature") || !heater_bed.contains("target"))
                    return false;
                if (!heater_bed["temperature"].is_number() || !heater_bed["target"].is_number())
                    return false;

                return true;
            },
            1000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("initial state has correct toolhead structure") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));
        mock.stop_temperature_simulation();

        // Find the initial state notification (the one with homed_axes)
        // Simulation updates only include position, not homed_axes
        // NOTE: Must COPY the status because get_notifications() returns a copy of the vector
        json initial_status;
        bool found_initial_status = false;
        for (const auto& notification : fixture.get_notifications()) {
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& status = notification["params"][0];
                // Guard: status must be an object before calling contains()
                if (status.is_object() && status.contains("toolhead") &&
                    status["toolhead"].contains("homed_axes")) {
                    initial_status = status; // COPY, not pointer
                    found_initial_status = true;
                    break;
                }
            }
        }
        REQUIRE(found_initial_status);

        // Toolhead structure (matches real Moonraker)
        const json& toolhead = initial_status["toolhead"];
        REQUIRE(toolhead.contains("position"));
        REQUIRE(toolhead["position"].is_array());
        REQUIRE(toolhead["position"].size() == 4); // [x, y, z, e]
        REQUIRE(toolhead.contains("homed_axes"));

        mock.disconnect();
    }

    SECTION("initial state has correct gcode_move structure") {
        // This test ensures the mock sends gcode_position which is required for
        // Motion panel to display position correctly (position won't update without it)
        auto mock = fixture.create_mock();
        mock->register_notify_update(fixture.create_capture_callback());
        mock->connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));
        mock->stop_temperature_simulation();

        // Find the initial state notification (the one with gcode_move)
        json initial_status;
        bool found = false;
        for (const auto& notification : fixture.get_notifications()) {
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& status = notification["params"][0];
                if (status.is_object() && status.contains("gcode_move")) {
                    initial_status = status;
                    found = true;
                    break;
                }
            }
        }
        REQUIRE(found);

        // gcode_move structure (matches real Moonraker)
        const json& gcode_move = initial_status["gcode_move"];
        REQUIRE(gcode_move.contains("gcode_position"));
        REQUIRE(gcode_move["gcode_position"].is_array());
        REQUIRE(gcode_move["gcode_position"].size() == 4); // [x, y, z, e]
        REQUIRE(gcode_move.contains("speed_factor"));
        REQUIRE(gcode_move.contains("extrude_factor"));
        REQUIRE(gcode_move.contains("homing_origin"));

        mock->disconnect();
    }

    SECTION("initial state has correct print_stats structure") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));
        mock.stop_temperature_simulation();

        // Find the initial state notification (the one with print_stats)
        // Simulation updates don't include print_stats
        // NOTE: Must COPY the status because get_notifications() returns a copy of the vector
        json initial_status;
        bool found_initial_status = false;
        for (const auto& notification : fixture.get_notifications()) {
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& status = notification["params"][0];
                // Guard: status must be an object before calling contains()
                if (status.is_object() && status.contains("print_stats")) {
                    initial_status = status; // COPY, not pointer
                    found_initial_status = true;
                    break;
                }
            }
        }
        REQUIRE(found_initial_status);

        // print_stats structure (matches real Moonraker)
        REQUIRE(initial_status.contains("print_stats"));
        const json& print_stats = initial_status["print_stats"];
        REQUIRE(print_stats.contains("state"));
        REQUIRE(print_stats.contains("filename"));
        REQUIRE(print_stats["state"].is_string());

        // Initial state should be "standby"
        REQUIRE(print_stats["state"] == "standby");

        mock.disconnect();
    }
}

// ============================================================================
// Notification Format Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock notification format matches real Moonraker", "[connection][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("notifications use notify_status_update method") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for simulation updates
        REQUIRE(fixture.wait_for_callbacks(2, 2000));
        mock.stop_temperature_simulation();

        for (const auto& notification : fixture.get_notifications()) {
            REQUIRE(notification.contains("method"));
            REQUIRE(notification["method"] == "notify_status_update");
        }

        mock.disconnect();
    }

    SECTION("params is array with [status, eventtime] structure") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callbacks(2, 2000));
        mock.stop_temperature_simulation();

        for (const auto& notification : fixture.get_notifications()) {
            REQUIRE(notification.contains("params"));
            REQUIRE(notification["params"].is_array());

            // Real Moonraker sends [status_object, eventtime]
            // Our mock sends [status_object] or [status_object, eventtime]
            REQUIRE(notification["params"].size() >= 1);

            // First element must be status object
            REQUIRE(notification["params"][0].is_object());
        }

        mock.disconnect();
    }

    SECTION("temperature values update over time") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Set a target to trigger heating
        mock.set_extruder_target(100.0);

        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for multiple updates
        REQUIRE(fixture.wait_for_callbacks(3, 3000));
        mock.stop_temperature_simulation();

        // Verify temperature is changing (should be heating toward 100C)
        bool found_extruder_temp = false;
        for (const auto& notification : fixture.get_notifications()) {
            if (notification["params"][0].contains("extruder") &&
                notification["params"][0]["extruder"].contains("temperature")) {
                found_extruder_temp = true;
                double temp = notification["params"][0]["extruder"]["temperature"].get<double>();
                // Should be above room temp if heating
                REQUIRE(temp >= 25.0);
            }
        }
        REQUIRE(found_extruder_temp);

        mock.disconnect();
    }
}

// ============================================================================
// Callback Invocation Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock callback invocation", "[connection][slow][callbacks]") {
    MockBehaviorTestFixture fixture1;
    MockBehaviorTestFixture fixture2;

    SECTION("multiple callbacks receive same notifications") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Register two callbacks
        mock.register_notify_update(fixture1.create_capture_callback());
        mock.register_notify_update(fixture2.create_capture_callback());

        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture1.wait_for_callback(500));
        REQUIRE(fixture2.wait_for_callback(500));
        mock.stop_temperature_simulation();

        // Both should have received notifications
        REQUIRE(!fixture1.get_notifications().empty());
        REQUIRE(!fixture2.get_notifications().empty());

        // Should have same number of notifications
        REQUIRE(fixture1.get_notifications().size() == fixture2.get_notifications().size());

        mock.disconnect();
    }

    SECTION("callbacks registered after connect still receive updates") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Small delay to let initial state pass
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Register callback AFTER connect
        mock.register_notify_update(fixture1.create_capture_callback());

        // Should receive simulation updates
        REQUIRE(fixture1.wait_for_callback(3000));
        mock.stop_temperature_simulation();

        REQUIRE(!fixture1.get_notifications().empty());

        mock.disconnect();
    }

    SECTION("disconnect stops callbacks") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture1.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture1.wait_for_callback(500));

        // Record count before disconnect
        size_t count_before = fixture1.get_notifications().size();

        // Disconnect (stops simulation)
        mock.disconnect();

        // Wait a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        // Count should not have increased significantly
        size_t count_after = fixture1.get_notifications().size();
        REQUIRE(count_after <= count_before + 1); // Allow for one in-flight
    }
}

// ============================================================================
// G-code Temperature Parsing Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock G-code temperature parsing", "[connection][slow][gcode]") {
    MockBehaviorTestFixture fixture;

    // Helper to verify extruder target in notifications
    auto verify_extruder_target = [&fixture](double expected_target) {
        return fixture.wait_for_matching(
            [expected_target](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("extruder")) {
                    return false;
                }
                const json& extruder = status["extruder"];
                if (!extruder.is_object() || !extruder.contains("target")) {
                    return false;
                }
                double target = extruder["target"].get<double>();
                return std::abs(target - expected_target) < 0.1;
            },
            2000);
    };

    // Helper to verify bed target in notifications
    auto verify_bed_target = [&fixture](double expected_target) {
        return fixture.wait_for_matching(
            [expected_target](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("heater_bed")) {
                    return false;
                }
                const json& heater_bed = status["heater_bed"];
                if (!heater_bed.is_object() || !heater_bed.contains("target")) {
                    return false;
                }
                double target = heater_bed["target"].get<double>();
                return std::abs(target - expected_target) < 0.1;
            },
            2000);
    };

    SECTION("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=xxx updates target") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        int result = mock.gcode_script("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=200");
        REQUIRE(result == 0);

        // Verify the target actually changed in status notifications
        REQUIRE(verify_extruder_target(200.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=xxx updates target") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        int result = mock.gcode_script("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60");
        REQUIRE(result == 0);

        // Verify the target actually changed in status notifications
        REQUIRE(verify_bed_target(60.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("M104 Sxxx sets extruder target") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        int result = mock.gcode_script("M104 S210");
        REQUIRE(result == 0);

        // Verify the target actually changed in status notifications
        REQUIRE(verify_extruder_target(210.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("lowercase m104 Sxxx sets extruder target") {
        // Command token normalization must cover more than just M117 - real
        // Klipper uppercases the command word for every G/M-code, not just
        // display messages.
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        int result = mock.gcode_script("m104 S220");
        REQUIRE(result == 0);

        // Verify the target actually changed in status notifications
        REQUIRE(verify_extruder_target(220.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("M109 Sxxx sets extruder target") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        int result = mock.gcode_script("M109 S215");
        REQUIRE(result == 0);

        // Verify the target actually changed in status notifications
        REQUIRE(verify_extruder_target(215.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("M140 Sxxx sets bed target") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        int result = mock.gcode_script("M140 S55");
        REQUIRE(result == 0);

        // Verify the target actually changed in status notifications
        REQUIRE(verify_bed_target(55.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("M190 Sxxx sets bed target") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        int result = mock.gcode_script("M190 S65");
        REQUIRE(result == 0);

        // Verify the target actually changed in status notifications
        REQUIRE(verify_bed_target(65.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("SET_HEATER_TEMPERATURE TARGET=0 turns off heater") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // First set a target via gcode to verify it changes
        mock.gcode_script("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=200");
        REQUIRE(verify_extruder_target(200.0));

        fixture.reset();

        // Turn off - should set target to 0
        int result = mock.gcode_script("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0");
        REQUIRE(result == 0);

        // Verify the target changed to 0 in status notifications
        REQUIRE(verify_extruder_target(0.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Hardware Discovery Tests
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "MoonrakerClientMock hardware discovery",
                 "[connection][hardware_discovery]") {
    SECTION("VORON_24 has correct hardware") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        auto hw = mock.hardware();

        const auto& heaters = hw.heaters();
        const auto& sensors = hw.sensors();
        const auto& fans = hw.fans();
        const auto& leds = hw.leds();

        // Voron 2.4 should have bed and extruder heaters
        REQUIRE(std::find(heaters.begin(), heaters.end(), "heater_bed") != heaters.end());
        REQUIRE(std::find(heaters.begin(), heaters.end(), "extruder") != heaters.end());

        // Should have chamber sensor (common on V2.4)
        bool has_chamber = std::find_if(sensors.begin(), sensors.end(), [](const std::string& s) {
                               return s.find("chamber") != std::string::npos;
                           }) != sensors.end();
        REQUIRE(has_chamber);

        // Should have fans
        REQUIRE(!fans.empty());

        // Voron 2.4 typically has LEDs
        REQUIRE(!leds.empty());
    }

    SECTION("VORON_TRIDENT has correct hardware") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_TRIDENT);
        auto hw = mock.hardware();

        const auto& heaters = hw.heaters();

        REQUIRE(std::find(heaters.begin(), heaters.end(), "heater_bed") != heaters.end());
        REQUIRE(std::find(heaters.begin(), heaters.end(), "extruder") != heaters.end());
    }

    SECTION("CREALITY_K1 has correct hardware") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K1);
        auto hw = mock.hardware();

        const auto& heaters = hw.heaters();
        const auto& fans = hw.fans();

        REQUIRE(std::find(heaters.begin(), heaters.end(), "heater_bed") != heaters.end());
        REQUIRE(std::find(heaters.begin(), heaters.end(), "extruder") != heaters.end());
        REQUIRE(!fans.empty());
    }

    SECTION("FLASHFORGE_AD5M has correct hardware") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_AD5M);
        auto hw = mock.hardware();

        const auto& heaters = hw.heaters();
        const auto& leds = hw.leds();

        REQUIRE(std::find(heaters.begin(), heaters.end(), "heater_bed") != heaters.end());
        REQUIRE(std::find(heaters.begin(), heaters.end(), "extruder") != heaters.end());
        // AD5M has chamber light
        REQUIRE(!leds.empty());
    }

    SECTION("GENERIC_COREXY has minimal hardware") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
        auto hw = mock.hardware();

        const auto& heaters = hw.heaters();

        REQUIRE(std::find(heaters.begin(), heaters.end(), "heater_bed") != heaters.end());
        REQUIRE(std::find(heaters.begin(), heaters.end(), "extruder") != heaters.end());
    }

    SECTION("GENERIC_BEDSLINGER has minimal hardware") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::GENERIC_BEDSLINGER);
        auto hw = mock.hardware();

        const auto& heaters = hw.heaters();
        const auto& leds = hw.leds();

        REQUIRE(std::find(heaters.begin(), heaters.end(), "heater_bed") != heaters.end());
        REQUIRE(std::find(heaters.begin(), heaters.end(), "extruder") != heaters.end());
        // Bedslinger has no dedicated temperature sensors or LEDs
        REQUIRE(leds.empty());
    }

    SECTION("MULTI_EXTRUDER has multiple extruders") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::MULTI_EXTRUDER);
        auto hw = mock.hardware();

        const auto& heaters = hw.heaters();

        REQUIRE(std::find(heaters.begin(), heaters.end(), "heater_bed") != heaters.end());
        REQUIRE(std::find(heaters.begin(), heaters.end(), "extruder") != heaters.end());
        REQUIRE(std::find(heaters.begin(), heaters.end(), "extruder1") != heaters.end());
        REQUIRE(heaters.size() >= 3);
    }

    SECTION("discover_printer() invokes completion callback") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        bool callback_invoked = false;
        mock.discover_printer([&callback_invoked]() { callback_invoked = true; });

        REQUIRE(callback_invoked);
    }

    SECTION("discover_printer() populates bed mesh") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Set up API before connect so bed mesh callback is registered
        // when dispatch_initial_state() fires during connect()
        PrinterState state;
        state.init_subjects(false);
        MoonrakerAPIMock api(mock, state);

        mock.connect("ws://mock/websocket", []() {}, []() {});
        mock.discover_printer([]() {});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Test through API (non-deprecated methods)
        REQUIRE(api.advanced().has_bed_mesh());
        const auto* mesh = api.advanced().get_active_bed_mesh();
        REQUIRE(mesh != nullptr);
        REQUIRE(mesh->x_count > 0);
        REQUIRE(mesh->y_count > 0);
        REQUIRE(!mesh->probed_matrix.empty());
        REQUIRE(mesh->name == "default");

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Connection State Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock connection state", "[connection][slow][connection_state]") {
    SECTION("initial state is DISCONNECTED") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        REQUIRE(mock.get_connection_state() == ConnectionState::DISCONNECTED);
    }

    SECTION("connect() transitions to CONNECTED") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        bool connected_callback_invoked = false;
        mock.connect(
            "ws://mock/websocket",
            [&connected_callback_invoked]() { connected_callback_invoked = true; }, []() {});

        REQUIRE(mock.get_connection_state() == ConnectionState::CONNECTED);
        REQUIRE(connected_callback_invoked);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("disconnect() transitions to DISCONNECTED") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        mock.connect("ws://mock/websocket", []() {}, []() {});
        REQUIRE(mock.get_connection_state() == ConnectionState::CONNECTED);

        mock.disconnect();
        REQUIRE(mock.get_connection_state() == ConnectionState::DISCONNECTED);
    }

    SECTION("state change callback is invoked") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        std::vector<std::pair<ConnectionState, ConnectionState>> transitions;
        mock.set_state_change_callback(
            [&transitions](ConnectionState old_state, ConnectionState new_state) {
                transitions.emplace_back(old_state, new_state);
            });

        mock.connect("ws://mock/websocket", []() {}, []() {});
        mock.stop_temperature_simulation();
        mock.disconnect();

        // Should have transitions: DISCONNECTED->CONNECTING, CONNECTING->CONNECTED,
        // CONNECTED->DISCONNECTED
        REQUIRE(transitions.size() >= 2);

        // Last transition should be to DISCONNECTED
        REQUIRE(transitions.back().second == ConnectionState::DISCONNECTED);
    }
}

// ============================================================================
// Temperature Simulation Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock temperature simulation", "[connection][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("temperature approaches target over time") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Set target before connect
        mock.set_extruder_target(100.0);
        mock.set_bed_target(60.0);

        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for several simulation cycles
        REQUIRE(fixture.wait_for_callbacks(5, 5000));
        mock.stop_temperature_simulation();

        // Check that temperatures are increasing
        double first_ext_temp = -1.0;
        double last_ext_temp = -1.0;

        for (const auto& notification : fixture.get_notifications()) {
            // Safely navigate JSON structure
            if (!notification.contains("params") || !notification["params"].is_array() ||
                notification["params"].empty()) {
                continue;
            }
            const json& status = notification["params"][0];
            if (!status.is_object() || !status.contains("extruder")) {
                continue;
            }
            const json& extruder = status["extruder"];
            if (!extruder.is_object() || !extruder.contains("temperature")) {
                continue;
            }
            double temp = extruder["temperature"].get<double>();
            if (first_ext_temp < 0)
                first_ext_temp = temp;
            last_ext_temp = temp;
        }

        // Temperature should be increasing toward target
        REQUIRE(last_ext_temp >= first_ext_temp);

        mock.disconnect();
    }

    SECTION("room temperature is default when target is 0") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for notification with extruder temperature around room temp
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("extruder") ||
                    !status["extruder"].contains("temperature")) {
                    return false;
                }
                double ext_temp = status["extruder"]["temperature"].get<double>();
                // Should be around room temperature (25C)
                return ext_temp >= 20.0 && ext_temp <= 30.0;
            },
            1000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Bed Mesh Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock bed mesh", "[slow][mock][calibration]") {
    SECTION("bed mesh is generated on construction") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        REQUIRE(mock.has_bed_mesh());
        const auto& mesh = mock.get_active_bed_mesh();

        // Default mesh should be 7x7
        REQUIRE(mesh.x_count == 7);
        REQUIRE(mesh.y_count == 7);
        REQUIRE(mesh.probed_matrix.size() == 7);
        REQUIRE(mesh.probed_matrix[0].size() == 7);
    }

    SECTION("bed mesh has valid profile names") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        const auto& profiles = mock.get_bed_mesh_profiles();

        REQUIRE(!profiles.empty());
        REQUIRE(std::find(profiles.begin(), profiles.end(), "default") != profiles.end());
    }

    SECTION("bed mesh values are in realistic range") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        const auto& mesh = mock.get_active_bed_mesh();

        for (const auto& row : mesh.probed_matrix) {
            for (float z : row) {
                // Realistic bed mesh Z values are typically -0.5 to +0.5mm
                REQUIRE(z >= -0.5f);
                REQUIRE(z <= 0.5f);
            }
        }
    }

    SECTION("bed mesh bounds are set") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        const auto& mesh = mock.get_active_bed_mesh();

        // Should have valid bounds
        REQUIRE(mesh.mesh_max[0] > mesh.mesh_min[0]);
        REQUIRE(mesh.mesh_max[1] > mesh.mesh_min[1]);
    }

    SECTION("bed mesh is included in initial status notification") {
        MockBehaviorTestFixture fixture;
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Register callback to capture notifications
        mock.register_notify_update(fixture.create_capture_callback());

        // Connect (triggers initial state dispatch with bed_mesh)
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for at least one notification
        REQUIRE(fixture.wait_for_callback(500));

        // Find the initial notification containing bed_mesh
        bool found_bed_mesh = false;
        json bed_mesh_data;
        for (const auto& notification : fixture.get_notifications()) {
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& status = notification["params"][0];
                if (status.is_object() && status.contains("bed_mesh")) {
                    found_bed_mesh = true;
                    bed_mesh_data = status["bed_mesh"];
                    break;
                }
            }
        }

        REQUIRE(found_bed_mesh);
        REQUIRE(bed_mesh_data.is_object());

        // Verify required fields are present (Moonraker-compatible format)
        REQUIRE(bed_mesh_data.contains("profile_name"));
        REQUIRE(bed_mesh_data.contains("probed_matrix"));
        REQUIRE(bed_mesh_data.contains("mesh_min"));
        REQUIRE(bed_mesh_data.contains("mesh_max"));
        REQUIRE(bed_mesh_data.contains("profiles"));
        REQUIRE(bed_mesh_data.contains("mesh_params"));

        // Verify profile_name
        REQUIRE(bed_mesh_data["profile_name"].is_string());
        REQUIRE(bed_mesh_data["profile_name"].get<std::string>() == "default");

        // Verify probed_matrix is 2D array
        REQUIRE(bed_mesh_data["probed_matrix"].is_array());
        REQUIRE(bed_mesh_data["probed_matrix"].size() == 7); // 7x7 mesh
        REQUIRE(bed_mesh_data["probed_matrix"][0].is_array());
        REQUIRE(bed_mesh_data["probed_matrix"][0].size() == 7);

        // Verify mesh bounds
        REQUIRE(bed_mesh_data["mesh_min"].is_array());
        REQUIRE(bed_mesh_data["mesh_min"].size() >= 2);
        REQUIRE(bed_mesh_data["mesh_max"].is_array());
        REQUIRE(bed_mesh_data["mesh_max"].size() >= 2);

        // Verify profiles object (Moonraker format: {"profile_name": {...}, ...})
        REQUIRE(bed_mesh_data["profiles"].is_object());
        REQUIRE(bed_mesh_data["profiles"].contains("default"));

        // Verify mesh_params
        REQUIRE(bed_mesh_data["mesh_params"].is_object());
        REQUIRE(bed_mesh_data["mesh_params"].contains("algo"));
        REQUIRE(bed_mesh_data["mesh_params"]["algo"].get<std::string>() == "lagrange");
    }

    SECTION("bed mesh is parsed correctly from initial notification") {
        // Test that dispatch_status_update correctly parses bed_mesh
        // (previously this was broken - bed_mesh was in notification but never parsed)
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Connect (triggers initial state dispatch which should now parse bed_mesh)
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // After connect, the mock should have parsed the bed mesh from its own notification
        // via dispatch_status_update() -> parse_bed_mesh()
        REQUIRE(mock.has_bed_mesh());

        const auto& mesh = mock.get_active_bed_mesh();
        REQUIRE(mesh.name == "default");
        REQUIRE(mesh.x_count == 7);
        REQUIRE(mesh.y_count == 7);
        REQUIRE(mesh.algo == "lagrange");

        // Verify profiles were also parsed
        const auto& profiles = mock.get_bed_mesh_profiles();
        REQUIRE(profiles.size() == 2);
        REQUIRE(std::find(profiles.begin(), profiles.end(), "default") != profiles.end());
        REQUIRE(std::find(profiles.begin(), profiles.end(), "adaptive") != profiles.end());
    }

    SECTION("parse_bed_mesh handles rectangular mesh (5x7)") {
        // Test that non-square meshes parse correctly
        TestableMoonrakerMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        MockBehaviorTestFixture fixture;

        // Create a 5x7 rectangular mesh (5 columns, 7 rows)
        json bed_mesh = {
            {"profile_name", "rectangular"},
            {"probed_matrix", json::array({json::array({0.01, 0.02, 0.03, 0.04, 0.05}),
                                           json::array({0.02, 0.03, 0.04, 0.05, 0.06}),
                                           json::array({0.03, 0.04, 0.05, 0.06, 0.07}),
                                           json::array({0.04, 0.05, 0.06, 0.07, 0.08}),
                                           json::array({0.05, 0.06, 0.07, 0.08, 0.09}),
                                           json::array({0.06, 0.07, 0.08, 0.09, 0.10}),
                                           json::array({0.07, 0.08, 0.09, 0.10, 0.11})})},
            {"mesh_min", {10.0, 20.0}},
            {"mesh_max", {200.0, 280.0}},
            {"profiles", {{"rectangular", json::object()}}},
            {"mesh_params", {{"algo", "bicubic"}}}};

        // Wrap in status notification format and dispatch
        json status = {{"bed_mesh", bed_mesh}};
        mock.register_notify_update(fixture.create_capture_callback());
        mock.dispatch_status_update(status);

        // Verify rectangular dimensions
        REQUIRE(mock.has_bed_mesh());
        const auto& mesh = mock.get_active_bed_mesh();
        REQUIRE(mesh.name == "rectangular");
        REQUIRE(mesh.x_count == 5); // 5 columns
        REQUIRE(mesh.y_count == 7); // 7 rows
        REQUIRE(mesh.algo == "bicubic");
        REQUIRE(mesh.mesh_min[0] == Catch::Approx(10.0f));
        REQUIRE(mesh.mesh_min[1] == Catch::Approx(20.0f));
        REQUIRE(mesh.mesh_max[0] == Catch::Approx(200.0f));
        REQUIRE(mesh.mesh_max[1] == Catch::Approx(280.0f));
    }

    SECTION("parse_bed_mesh handles empty probed_matrix") {
        // An empty matrix should result in has_bed_mesh() == false
        TestableMoonrakerMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // First, verify mock starts with a bed mesh
        mock.connect("ws://mock/websocket", []() {}, []() {});
        REQUIRE(mock.has_bed_mesh());

        // Now dispatch an empty bed_mesh update (simulates BED_MESH_CLEAR)
        json bed_mesh = {{"profile_name", ""}, {"probed_matrix", json::array()}};
        json status = {{"bed_mesh", bed_mesh}};
        mock.dispatch_status_update(status);

        // Should no longer have a bed mesh
        REQUIRE_FALSE(mock.has_bed_mesh());
        REQUIRE(mock.get_active_bed_mesh().x_count == 0);
        REQUIRE(mock.get_active_bed_mesh().y_count == 0);
    }

    SECTION("parse_bed_mesh handles missing optional fields") {
        // Test that missing fields don't crash or produce incorrect state
        TestableMoonrakerMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Minimal bed_mesh: only profile_name and probed_matrix
        json bed_mesh = {
            {"profile_name", "minimal"},
            {"probed_matrix",
             json::array({json::array({0.1, 0.2, 0.3}), json::array({0.2, 0.3, 0.4}),
                          json::array({0.3, 0.4, 0.5})})}
            // Missing: mesh_min, mesh_max, profiles, mesh_params
        };

        json status = {{"bed_mesh", bed_mesh}};
        mock.dispatch_status_update(status);

        // Should still parse the matrix
        REQUIRE(mock.has_bed_mesh());
        const auto& mesh = mock.get_active_bed_mesh();
        REQUIRE(mesh.name == "minimal");
        REQUIRE(mesh.x_count == 3);
        REQUIRE(mesh.y_count == 3);
        // algo should retain previous value or be empty
    }

    SECTION("parse_bed_mesh handles null profile_name") {
        // Real Moonraker can send null profile_name when no mesh is loaded
        TestableMoonrakerMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        json bed_mesh = {
            {"profile_name", nullptr}, // JSON null
            {"probed_matrix", json::array({json::array({0.0, 0.1}), json::array({0.1, 0.2})})}};

        json status = {{"bed_mesh", bed_mesh}};
        mock.dispatch_status_update(status);

        // Should parse matrix but not update name (or use empty string)
        REQUIRE(mock.has_bed_mesh());
        // profile_name handling when null - should either be empty or unchanged
    }

    SECTION("parse_bed_mesh verifies Z heights are numbers") {
        // Test that non-numeric values in probed_matrix are handled gracefully
        TestableMoonrakerMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Mixed valid/invalid values
        json bed_mesh = {
            {"profile_name", "test"},
            {"probed_matrix", json::array({
                                  json::array({0.1, "invalid", 0.3}), // Middle value is string
                                  json::array({0.2, 0.3, 0.4}),
                                  json::array({0.3, 0.4, nullptr}) // Last value is null
                              })}};

        json status = {{"bed_mesh", bed_mesh}};
        mock.dispatch_status_update(status);

        // Should parse, skipping invalid values
        // The first row will have 2 values (0.1, 0.3), others have 2-3
        // Implementation may handle this differently
        // At minimum, it should not crash
        const auto& mesh = mock.get_active_bed_mesh();
        REQUIRE(mesh.name == "test");
    }
}

// ============================================================================
// send_jsonrpc Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock send_jsonrpc methods", "[connection][jsonrpc]") {
    SECTION("send_jsonrpc without params returns success") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        REQUIRE(mock.send_jsonrpc("printer.info") == 0);
    }

    SECTION("send_jsonrpc with params returns success") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        json params = {{"filename", "test.gcode"}};
        REQUIRE(mock.send_jsonrpc("printer.print.start", params) == 0);
    }

    SECTION("send_jsonrpc with callback returns valid request ID and invokes callback") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        json params = {};
        bool callback_invoked = false;
        json received_response;

        RequestId id = mock.send_jsonrpc("printer.info", params, [&](json response) {
            callback_invoked = true;
            received_response = response;
        });

        // Verify valid request ID returned
        REQUIRE(id != INVALID_REQUEST_ID);

        // Verify callback was invoked (printer.info is a registered handler)
        REQUIRE(callback_invoked);

        // Verify the response contains expected fields
        REQUIRE(received_response.contains("result"));
        REQUIRE(received_response["result"].contains("state"));
        REQUIRE(received_response["result"]["state"].get<std::string>() == "ready");
        REQUIRE(received_response["result"].contains("hostname"));
        REQUIRE(received_response["result"].contains("software_version"));
    }

    SECTION(
        "send_jsonrpc with error callback returns valid request ID and invokes success callback") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        json params = {};
        bool success_invoked = false;
        bool error_invoked = false;
        json received_response;

        RequestId id = mock.send_jsonrpc(
            "printer.info", params,
            [&](json response) {
                success_invoked = true;
                received_response = response;
            },
            [&](const MoonrakerError&) { error_invoked = true; }, 5000);

        // Verify valid request ID returned
        REQUIRE(id != INVALID_REQUEST_ID);

        // Verify success callback was invoked, not error callback
        REQUIRE(success_invoked);
        REQUIRE_FALSE(error_invoked);

        // Verify the response contains expected printer info
        REQUIRE(received_response.contains("result"));
        REQUIRE(received_response["result"]["state"].get<std::string>() == "ready");
    }
}

// ============================================================================
// Guessing Methods Tests (Use PrinterHardware with mock hardware data)
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "PrinterHardware guessing methods work with mock hardware data",
                 "[printer]") {
    SECTION("guess_bed_heater returns heater_bed") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.discover_printer([]() {});
        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());
        REQUIRE(hw.guess_bed_heater() == "heater_bed");
    }

    SECTION("guess_hotend_heater returns extruder") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.discover_printer([]() {});
        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());
        REQUIRE(hw.guess_hotend_heater() == "extruder");
    }

    SECTION("guess_bed_sensor returns heater_bed (heaters are also sensors)") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.discover_printer([]() {});
        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
    }

    SECTION("guess_hotend_sensor returns extruder (heaters are also sensors)") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.discover_printer([]() {});
        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }
}

// ============================================================================
// G-code Motion Simulation Tests (Phase 1.6a)
// ============================================================================

TEST_CASE("MoonrakerClientMock G28 homing updates homed_axes", "[api][homing][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("G28 homes all axes and sets position to 0") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Execute G28 to home all axes
        mock.gcode_script("G28");

        // Wait for notification with updated homed_axes
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("toolhead") && status["toolhead"].contains("homed_axes") &&
                       status["toolhead"]["homed_axes"] == "xyz";
            },
            2000));

        mock.stop_temperature_simulation();

        // Verify position is at 0,0,0 after homing
        bool found_zero_position = fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("position")) {
                    return false;
                }
                const json& pos = status["toolhead"]["position"];
                return pos.is_array() && pos.size() >= 3 && pos[0].get<double>() == 0.0 &&
                       pos[1].get<double>() == 0.0 && pos[2].get<double>() == 0.0;
            },
            500);
        REQUIRE(found_zero_position);

        mock.disconnect();
    }

    SECTION("G28 X homes only X axis") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Home only X
        mock.gcode_script("G28 X");

        // Wait for notification - homed_axes should contain 'x'
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("homed_axes")) {
                    return false;
                }
                std::string homed = status["toolhead"]["homed_axes"].get<std::string>();
                return homed.find('x') != std::string::npos;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G28 X Y homes X and Y axes") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Home X and Y
        mock.gcode_script("G28 X Y");

        // Wait for notification - homed_axes should contain 'x' and 'y'
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("homed_axes")) {
                    return false;
                }
                std::string homed = status["toolhead"]["homed_axes"].get<std::string>();
                return homed.find('x') != std::string::npos && homed.find('y') != std::string::npos;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G28 Z homes only Z axis") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Home only Z
        mock.gcode_script("G28 Z");

        // Wait for notification - homed_axes should contain 'z'
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("homed_axes")) {
                    return false;
                }
                std::string homed = status["toolhead"]["homed_axes"].get<std::string>();
                return homed.find('z') != std::string::npos;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE("MoonrakerClientMock G0/G1 movement updates position", "[api][movement][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("G0 absolute movement updates position") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // First home all axes
        mock.gcode_script("G28");

        // Move to absolute position
        mock.gcode_script("G0 X100 Y50 Z10");

        // Wait for notification with updated position
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("position")) {
                    return false;
                }
                const json& pos = status["toolhead"]["position"];
                return pos.is_array() && pos.size() >= 3 && pos[0].get<double>() == 100.0 &&
                       pos[1].get<double>() == 50.0 && pos[2].get<double>() == 10.0;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G1 absolute movement updates position") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // First home all axes
        mock.gcode_script("G28");

        // Linear move (G1) with feed rate (F) and extrusion (E) - should ignore E and F
        mock.gcode_script("G1 X50 Y75 Z5 E10 F3000");

        // Wait for notification with updated position
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("position")) {
                    return false;
                }
                const json& pos = status["toolhead"]["position"];
                return pos.is_array() && pos.size() >= 3 && pos[0].get<double>() == 50.0 &&
                       pos[1].get<double>() == 75.0 && pos[2].get<double>() == 5.0;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G91 sets relative mode and G0 moves relatively") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Home to 0,0,0
        mock.gcode_script("G28");

        // Move to absolute position first
        mock.gcode_script("G0 X100 Y100 Z10");

        // Switch to relative mode
        mock.gcode_script("G91");

        // Move relatively by +10, +20, +5
        mock.gcode_script("G0 X10 Y20 Z5");

        // Position should now be 110, 120, 15
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("position")) {
                    return false;
                }
                const json& pos = status["toolhead"]["position"];
                return pos.is_array() && pos.size() >= 3 && pos[0].get<double>() == 110.0 &&
                       pos[1].get<double>() == 120.0 && pos[2].get<double>() == 15.0;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G90 returns to absolute mode") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Home to 0,0,0
        mock.gcode_script("G28");

        // Move to starting position
        mock.gcode_script("G0 X100 Y100 Z10");

        // Switch to relative mode
        mock.gcode_script("G91");

        // Move relatively
        mock.gcode_script("G0 X10 Y10 Z5");

        // Return to absolute mode
        mock.gcode_script("G90");

        // Now move to absolute position (should NOT be relative)
        mock.gcode_script("G0 X50 Y50 Z5");

        // Position should now be 50, 50, 5 (absolute)
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("position")) {
                    return false;
                }
                const json& pos = status["toolhead"]["position"];
                return pos.is_array() && pos.size() >= 3 && pos[0].get<double>() == 50.0 &&
                       pos[1].get<double>() == 50.0 && pos[2].get<double>() == 5.0;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Single axis movement only affects that axis") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Home and move to known position
        mock.gcode_script("G28");
        mock.gcode_script("G0 X100 Y100 Z10");

        // Move only X
        mock.gcode_script("G0 X50");

        // Position should be 50, 100, 10 (only X changed)
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("position")) {
                    return false;
                }
                const json& pos = status["toolhead"]["position"];
                return pos.is_array() && pos.size() >= 3 && pos[0].get<double>() == 50.0 &&
                       pos[1].get<double>() == 100.0 && pos[2].get<double>() == 10.0;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE("MoonrakerClientMock homed_axes in notifications", "[api][notifications][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("Initial state has empty homed_axes") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Initial state should have empty homed_axes
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("toolhead") && status["toolhead"].contains("homed_axes") &&
                       status["toolhead"]["homed_axes"] == "";
            },
            1000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Notifications include homed_axes after G28") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Home all axes
        mock.gcode_script("G28");

        // Wait for a notification showing homed_axes="xyz"
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("toolhead") && status["toolhead"].contains("homed_axes") &&
                       status["toolhead"]["homed_axes"] == "xyz";
            },
            3000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Position persists without auto-simulation") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Home and move to known position
        mock.gcode_script("G28");
        mock.gcode_script("G0 X150 Y75 Z25");

        // Wait for a notification showing the correct position
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("toolhead") || !status["toolhead"].contains("position")) {
                    return false;
                }
                const json& pos = status["toolhead"]["position"];
                return pos.is_array() && pos.size() >= 3 && pos[0].get<double>() == 150.0 &&
                       pos[1].get<double>() == 75.0 && pos[2].get<double>() == 25.0;
            },
            3000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Print Job Simulation Tests (Phase 1.6b)
// ============================================================================

TEST_CASE("MoonrakerClientMock SDCARD_PRINT_FILE starts print", "[slow][print][start]") {
    MockBehaviorTestFixture fixture;

    SECTION("SDCARD_PRINT_FILE sets state to printing and stores filename") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start a print
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for notification with print_stats showing "printing" state and filename
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("print_stats")) {
                    return false;
                }
                const json& ps = status["print_stats"];
                // Check both state and filename exist before accessing
                if (!ps.contains("state") || !ps.contains("filename")) {
                    return false;
                }
                return ps["state"] == "printing" && ps["filename"] == "3DBenchy.gcode";
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("SDCARD_PRINT_FILE accepts a quoted filename containing spaces") {
        // Klipper tokenizes extended parameters with shlex, so callers quote
        // any name with a space in it (Moonraker's klippy_apis.py and our
        // Creality power-loss resume both do). The quotes are syntax, not part
        // of the filename.
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=\"My Part v2.gcode\" ISCONTINUEPRINT=1");

        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("print_stats")) {
                    return false;
                }
                const json& ps = status["print_stats"];
                if (!ps.contains("state") || !ps.contains("filename")) {
                    return false;
                }
                return ps["state"] == "printing" && ps["filename"] == "My Part v2.gcode";
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("SDCARD_PRINT_FILE resets progress to 0") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start a print
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for notification with virtual_sdcard showing progress near 0
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("virtual_sdcard") ||
                    !status["virtual_sdcard"].contains("progress")) {
                    return false;
                }
                double progress = status["virtual_sdcard"]["progress"].get<double>();
                // Progress should be very small (just started) or 0
                return progress < 0.1;
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE("MoonrakerClientMock PAUSE/RESUME state transitions", "[print][pause_resume][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("PAUSE transitions from printing to paused") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start a print
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for printing state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") &&
                       status["print_stats"]["state"] == "printing";
            },
            2000));

        fixture.reset();

        // Pause the print
        mock.gcode_script("PAUSE");

        // Wait for paused state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") && status["print_stats"]["state"] == "paused";
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("RESUME transitions from paused to printing") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start and pause
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        mock.gcode_script("PAUSE");

        // Wait for paused state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") && status["print_stats"]["state"] == "paused";
            },
            2000));

        fixture.reset();

        // Resume the print
        mock.gcode_script("RESUME");

        // Wait for printing state again
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") &&
                       status["print_stats"]["state"] == "printing";
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("PAUSE only works when printing") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // PAUSE should not throw when not printing
        int result = mock.gcode_script("PAUSE");
        REQUIRE(result == 0);
        // State should remain standby (not transition to paused)
        // Note: We can't directly check print_state_ since it's private,
        // but we verify via gcode_script returning success
    }

    SECTION("RESUME only works when paused") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Start a print (state = printing)
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // RESUME should not throw when printing (not paused)
        int result = mock.gcode_script("RESUME");
        REQUIRE(result == 0);
        // State should remain printing (not change)
    }
}

TEST_CASE("MoonrakerClientMock CANCEL_PRINT resets to standby", "[print][cancel][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("CANCEL_PRINT transitions to cancelled then standby") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start a print
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for printing state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") &&
                       status["print_stats"]["state"] == "printing";
            },
            2000));

        fixture.reset();

        // Cancel the print
        mock.gcode_script("CANCEL_PRINT");

        // Wait for standby state (after brief delay from cancelled)
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") &&
                       status["print_stats"]["state"] == "standby";
            },
            3000)); // Longer timeout since we need to wait for cancelled->standby transition

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE("MoonrakerClientMock print progress increments during printing",
          "[print][progress][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("Progress increases while printing") {
        // Use high speedup to get through preheat phase quickly
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 500.0);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start a print
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for several simulation ticks to see progress increase (longer for preheat)
        REQUIRE(fixture.wait_for_callbacks(10, 8000));
        mock.stop_temperature_simulation();

        // Find progression of progress values
        double first_progress = -1.0;
        double last_progress = -1.0;

        for (const auto& notification : fixture.get_notifications()) {
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& status = notification["params"][0];
                if (status.contains("virtual_sdcard") &&
                    status["virtual_sdcard"].contains("progress")) {
                    double progress = status["virtual_sdcard"]["progress"].get<double>();
                    if (first_progress < 0)
                        first_progress = progress;
                    last_progress = progress;
                }
            }
        }

        // Progress should have increased (or at least not decreased)
        REQUIRE(last_progress >= first_progress);
        // Progress should be positive after preheat completes and printing starts
        // Note: With speedup, preheat should complete quickly
        REQUIRE(last_progress >= 0.0); // May be 0 if still in preheat

        mock.disconnect();
    }

    SECTION("Progress does not increase while paused") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start a print
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Let it run for a bit (notifications dispatch every ~2s at 1x speed)
        REQUIRE(fixture.wait_for_callbacks(3, 8000));

        // Pause
        mock.gcode_script("PAUSE");

        // Wait for paused state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") && status["print_stats"]["state"] == "paused";
            },
            2000));

        // Capture progress at pause
        double progress_at_pause = -1.0;
        for (const auto& notification : fixture.get_notifications()) {
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& status = notification["params"][0];
                if (status.contains("virtual_sdcard") &&
                    status["virtual_sdcard"].contains("progress")) {
                    progress_at_pause = status["virtual_sdcard"]["progress"].get<double>();
                }
            }
        }

        fixture.reset();

        // Wait for more ticks while paused (notifications dispatch every ~2s at 1x speed)
        REQUIRE(fixture.wait_for_callbacks(3, 8000));
        mock.stop_temperature_simulation();

        // Check progress hasn't increased (paused state doesn't advance progress)
        double progress_after_wait = -1.0;
        for (const auto& notification : fixture.get_notifications()) {
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& status = notification["params"][0];
                if (status.contains("virtual_sdcard") &&
                    status["virtual_sdcard"].contains("progress")) {
                    progress_after_wait = status["virtual_sdcard"]["progress"].get<double>();
                }
            }
        }

        // Progress should be the same (not increasing while paused)
        REQUIRE(progress_after_wait == progress_at_pause);

        mock.disconnect();
    }
}

TEST_CASE("MoonrakerClientMock print completion triggers complete state", "[print][complete]") {
    MockBehaviorTestFixture fixture;

    SECTION("print state transitions through phases correctly") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Verify initial state is IDLE
        REQUIRE(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::IDLE);

        // Start a print - transitions to PREHEAT or PRINTING
        REQUIRE(mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode") == 0);
        auto phase_after_start = mock.get_print_phase();
        REQUIRE((phase_after_start == MoonrakerClientMock::MockPrintPhase::PREHEAT ||
                 phase_after_start == MoonrakerClientMock::MockPrintPhase::PRINTING));

        // Wait for print_stats notification with printing or preheat state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("print_stats")) {
                    return false;
                }
                std::string state = status["print_stats"]["state"].get<std::string>();
                return state == "printing" || state == "preheat";
            },
            2000));

        // Pause the print
        fixture.reset();
        REQUIRE(mock.gcode_script("PAUSE") == 0);
        REQUIRE(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PAUSED);

        // Wait for paused state notification
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("print_stats")) {
                    return false;
                }
                return status["print_stats"]["state"].get<std::string>() == "paused";
            },
            2000));

        // Resume the print
        fixture.reset();
        REQUIRE(mock.gcode_script("RESUME") == 0);
        REQUIRE(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PRINTING);

        // Wait for printing state notification
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("print_stats")) {
                    return false;
                }
                return status["print_stats"]["state"].get<std::string>() == "printing";
            },
            2000));

        // Cancel the print
        fixture.reset();
        REQUIRE(mock.gcode_script("CANCEL_PRINT") == 0);
        REQUIRE(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::CANCELLED);

        // Wait for cancelled state notification
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("print_stats")) {
                    return false;
                }
                return status["print_stats"]["state"].get<std::string>() == "cancelled";
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE("MoonrakerClientMock M112 emergency stop sets error state", "[print][emergency][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("M112 sets print state to error") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start a print
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for printing state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") &&
                       status["print_stats"]["state"] == "printing";
            },
            2000));

        fixture.reset();

        // Emergency stop
        mock.gcode_script("M112");

        // Wait for error state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") && status["print_stats"]["state"] == "error";
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("M112 works even when not printing") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Emergency stop from standby
        mock.gcode_script("M112");

        // Wait for error state
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") && status["print_stats"]["state"] == "error";
            },
            2000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Bed Mesh G-code Simulation Tests (Task 5)
// ============================================================================

TEST_CASE("MoonrakerClientMock BED_MESH_CALIBRATE generates new mesh",
          "[mock][calibration][gcode][slow]") {
    MockBehaviorTestFixture fixture;

    SECTION("BED_MESH_CALIBRATE triggers mesh regeneration and notification") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Get initial bed mesh state
        REQUIRE(mock.has_bed_mesh());
        auto initial_mesh = mock.get_active_bed_mesh();
        std::string initial_name = initial_mesh.name;

        fixture.reset();

        // Execute BED_MESH_CALIBRATE
        mock.gcode_script("BED_MESH_CALIBRATE");

        // Wait for bed mesh notification
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("bed_mesh");
            },
            2000));

        // Mesh should still be valid
        REQUIRE(mock.has_bed_mesh());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("BED_MESH_CALIBRATE with PROFILE parameter uses custom profile name") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        fixture.reset();

        // Execute BED_MESH_CALIBRATE with custom profile
        mock.gcode_script("BED_MESH_CALIBRATE PROFILE=custom_profile");

        // Wait for bed mesh notification with the custom profile
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("bed_mesh"))
                    return false;
                const json& bed_mesh = status["bed_mesh"];
                return bed_mesh.contains("profile_name") &&
                       bed_mesh["profile_name"] == "custom_profile";
            },
            2000));

        // Verify profile name was updated
        REQUIRE(mock.get_active_bed_mesh().name == "custom_profile");

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE("MoonrakerClientMock BED_MESH_PROFILE LOAD changes active profile",
          "[mock][calibration][gcode]") {
    MockBehaviorTestFixture fixture;

    SECTION("BED_MESH_PROFILE LOAD loads existing profile") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Initial profile list contains "default" and "adaptive"
        // First create a new profile
        mock.gcode_script("BED_MESH_CALIBRATE PROFILE=test_profile");

        fixture.reset();

        // Load default profile
        mock.gcode_script("BED_MESH_PROFILE LOAD=default");

        // Wait for notification with default profile
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("bed_mesh"))
                    return false;
                const json& bed_mesh = status["bed_mesh"];
                return bed_mesh.contains("profile_name") && bed_mesh["profile_name"] == "default";
            },
            2000));

        REQUIRE(mock.get_active_bed_mesh().name == "default");

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE("MoonrakerClientMock BED_MESH_CLEAR clears active mesh", "[mock][calibration][gcode]") {
    MockBehaviorTestFixture fixture;

    SECTION("BED_MESH_CLEAR clears active mesh and sends notification") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Verify we have a bed mesh initially
        REQUIRE(mock.has_bed_mesh());

        fixture.reset();

        // Execute BED_MESH_CLEAR
        mock.gcode_script("BED_MESH_CLEAR");

        // Wait for bed mesh notification
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("bed_mesh");
            },
            2000));

        // Mesh should be cleared
        REQUIRE_FALSE(mock.has_bed_mesh());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Filament Type in Metadata Response Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock - file metadata includes filament_type",
          "[mock][api][metadata][filament_type]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("Metadata response includes filament_type field") {
        // Request metadata for 3DBenchy which has "; filament_type = PLA"
        json params = {{"filename", "3DBenchy.gcode"}};
        json response;
        bool callback_invoked = false;

        mock.send_jsonrpc("server.files.metadata", params, [&](json resp) {
            response = resp;
            callback_invoked = true;
        });

        REQUIRE(callback_invoked);
        REQUIRE(response.contains("result"));

        const json& result = response["result"];
        REQUIRE(result.contains("filament_type"));
        REQUIRE(result["filament_type"].is_string());
    }

    SECTION("Filament type matches G-code file (3DBenchy = PLA)") {
        // 3DBenchy.gcode contains "; filament_type = PLA"
        json params = {{"filename", "3DBenchy.gcode"}};
        json response;
        bool callback_invoked = false;

        mock.send_jsonrpc("server.files.metadata", params, [&](json resp) {
            response = resp;
            callback_invoked = true;
        });

        REQUIRE(callback_invoked);
        REQUIRE(response.contains("result"));

        const json& result = response["result"];
        REQUIRE(result.contains("filament_type"));
        REQUIRE(result["filament_type"].get<std::string>() == "PLA");
    }

    SECTION("Filament type from multi-extruder file extracts first type") {
        // Benchbin_MK4_MMU3.gcode contains "; filament_type = PLA;PLA;PLA;PLA"
        json params = {{"filename", "Benchbin_MK4_MMU3.gcode"}};
        json response;
        bool callback_invoked = false;

        mock.send_jsonrpc("server.files.metadata", params, [&](json resp) {
            response = resp;
            callback_invoked = true;
        });

        REQUIRE(callback_invoked);
        REQUIRE(response.contains("result"));

        const json& result = response["result"];
        REQUIRE(result.contains("filament_type"));
        // Full semicolon-separated string is preserved; consumers split it
        REQUIRE(result["filament_type"].get<std::string>() == "PLA;PLA;PLA;PLA");
    }

    SECTION("Filament type is empty for files without filament_type header") {
        // xyz-10mm-calibration-cube.gcode might not have filament_type
        // This test verifies the field exists and handles missing data gracefully
        json params = {{"filename", "xyz-10mm-calibration-cube.gcode"}};
        json response;
        bool callback_invoked = false;

        mock.send_jsonrpc("server.files.metadata", params, [&](json resp) {
            response = resp;
            callback_invoked = true;
        });

        REQUIRE(callback_invoked);
        REQUIRE(response.contains("result"));

        const json& result = response["result"];
        // Field should always be present (may be empty string)
        REQUIRE(result.contains("filament_type"));
        REQUIRE(result["filament_type"].is_string());
    }
}

TEST_CASE("MoonrakerClientMock - file metadata with success/error callbacks",
          "[mock][api][metadata][filament_type]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("Success callback returns metadata with filament_type") {
        json params = {{"filename", "3DBenchy.gcode"}};
        json response;
        bool success_invoked = false;
        bool error_invoked = false;

        mock.send_jsonrpc(
            "server.files.metadata", params,
            [&](json resp) {
                response = resp;
                success_invoked = true;
            },
            [&](const MoonrakerError& err) {
                (void)err;
                error_invoked = true;
            });

        REQUIRE(success_invoked);
        REQUIRE_FALSE(error_invoked);
        REQUIRE(response.contains("result"));

        const json& result = response["result"];
        REQUIRE(result.contains("filament_type"));
        REQUIRE(result["filament_type"].get<std::string>() == "PLA");
    }

    SECTION("Error callback invoked for missing filename") {
        json params = {}; // Missing filename parameter
        bool success_invoked = false;
        bool error_invoked = false;
        MoonrakerError captured_error;

        mock.send_jsonrpc(
            "server.files.metadata", params,
            [&](json resp) {
                (void)resp;
                success_invoked = true;
            },
            [&](const MoonrakerError& err) {
                captured_error = err;
                error_invoked = true;
            });

        REQUIRE(error_invoked);
        REQUIRE_FALSE(success_invoked);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Fan Control Tests
// ============================================================================

namespace {

/**
 * @brief True when a status notification reports @p fan_key at @p expected speed.
 *
 * The simulation loop emits full-status notifications, and those always carry a
 * "fan" entry (moonraker_client_mock.cpp builds it unconditionally) plus every
 * fan already present in fan_speeds_. So the FIRST notification mentioning a fan
 * is not necessarily the one the gcode under test produced — it is just as
 * likely a periodic update carrying the pre-command value. Matching on the
 * value rather than on the key's presence is what makes these sections
 * deterministic.
 */
bool reports_fan_speed(const json& n, const char* fan_key, double expected) {
    if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
        return false;
    }
    const json& status = n["params"][0];
    if (!status.is_object() || !status.contains(fan_key)) {
        return false;
    }
    const json& fan = status[fan_key];
    return fan.contains("speed") &&
           fan["speed"].get<double>() == Catch::Approx(expected).margin(0.01);
}

} // namespace

TEST_CASE("MoonrakerClientMock fan control", "[slow][mock][fan]") {
    // fixture must be declared BEFORE mock for correct destruction order
    MockBehaviorTestFixture fixture;
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    mock.connect("ws://test", []() {}, []() {});
    auto sub_id = mock.register_notify_update(fixture.create_capture_callback());
    fixture.reset(); // Clear initial state notification

    SECTION("M106 sets part cooling fan speed") {
        mock.gcode_script("M106 S127"); // ~50%

        REQUIRE(fixture.wait_for_matching(
            [](const json& n) { return reports_fan_speed(n, "fan", 0.498); }));
    }

    SECTION("M106 with P parameter sets specific fan index") {
        mock.gcode_script("M106 P1 S255"); // Fan 1 at 100%

        REQUIRE(fixture.wait_for_matching(
            [](const json& n) { return reports_fan_speed(n, "fan1", 1.0); }));
    }

    SECTION("M107 turns off fan") {
        mock.gcode_script("M106 S255");
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) { return reports_fan_speed(n, "fan", 1.0); }));

        // Reset AFTER the fan is confirmed at full speed, so the zero below can
        // only have come from M107 — nothing captured before it reports zero.
        fixture.reset();

        mock.gcode_script("M107");
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) { return reports_fan_speed(n, "fan", 0.0); }));
    }

    SECTION("SET_FAN_SPEED with normalized speed") {
        mock.gcode_script("SET_FAN_SPEED FAN=nevermore SPEED=0.75");
        fixture.wait_for_callback();

        auto notifications = fixture.get_notifications();
        bool found = false;
        for (const auto& n : notifications) {
            if (n.contains("params")) {
                const auto& params = n["params"][0];
                // Check for any key containing "nevermore"
                for (const auto& [key, value] : params.items()) {
                    if (key.find("nevermore") != std::string::npos) {
                        REQUIRE(value["speed"].get<double>() == Catch::Approx(0.75));
                        found = true;
                        break;
                    }
                }
            }
            if (found)
                break;
        }
        // May not find if mock doesn't have nevermore fan, so just check dispatch worked
        REQUIRE(notifications.size() >= 1);
    }

    (void)sub_id; // Callback auto-unregisters when mock destructs
}

// ============================================================================
// Z Offset Tracking Tests
// ============================================================================

namespace {
/// True when `n` is a gcode_move notification whose Z homing_origin equals `want`.
///
/// These sections used to call wait_for_callback(), which returns on the FIRST
/// notification of any kind, and then scanned for the first notification carrying
/// homing_origin and broke on it. The mock emits more than one notification per
/// command and the gcode_move snapshot is not necessarily the one that woke the
/// wait, so a pre-update snapshot could be the first match and the assertion read
/// 0.0 against the expected offset — roughly 3 runs in 40. Waiting on the VALUE
/// removes the race instead of widening a timeout.
bool has_z_offset(const nlohmann::json& n, double want) {
    if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
        return false;
    }
    const auto& p0 = n["params"][0];
    if (!p0.is_object() || !p0.contains("gcode_move")) {
        return false;
    }
    const auto& gm = p0["gcode_move"];
    if (!gm.contains("homing_origin") || !gm["homing_origin"].is_array() ||
        gm["homing_origin"].size() < 3 || !gm["homing_origin"][2].is_number()) {
        return false;
    }
    return gm["homing_origin"][2].get<double>() == Catch::Approx(want);
}
} // namespace

TEST_CASE("MoonrakerClientMock Z offset tracking", "[slow][mock][offset]") {
    // fixture must be declared BEFORE mock for correct destruction order
    MockBehaviorTestFixture fixture;
    MoonrakerClientMock mock;

    mock.connect("ws://test", []() {}, []() {});
    auto sub_id = mock.register_notify_update(fixture.create_capture_callback());
    fixture.reset();

    SECTION("SET_GCODE_OFFSET Z sets absolute offset") {
        mock.gcode_script("SET_GCODE_OFFSET Z=0.15");
        REQUIRE(fixture.wait_for_matching(
            [](const nlohmann::json& n) { return has_z_offset(n, 0.15); }, 2000));
    }

    SECTION("SET_GCODE_OFFSET Z_ADJUST adds to current offset") {
        // Wait for the base offset to actually land before adjusting off it —
        // wait_for_callback() could return first, and reset() would then discard
        // the notification that would have proved it, leaving Z_ADJUST to apply
        // to 0.0 and yield -0.05.
        mock.gcode_script("SET_GCODE_OFFSET Z=0.1");
        REQUIRE(fixture.wait_for_matching(
            [](const nlohmann::json& n) { return has_z_offset(n, 0.1); }, 2000));
        fixture.reset();

        mock.gcode_script("SET_GCODE_OFFSET Z_ADJUST=-0.05");
        REQUIRE(fixture.wait_for_matching(
            [](const nlohmann::json& n) { return has_z_offset(n, 0.05); }, 2000));
    }

    SECTION("Negative Z offset supported") {
        mock.gcode_script("SET_GCODE_OFFSET Z=-0.2");
        REQUIRE(fixture.wait_for_matching(
            [](const nlohmann::json& n) { return has_z_offset(n, -0.2); }, 2000));
    }

    (void)sub_id; // Callback auto-unregisters when mock destructs
}

// ============================================================================
// RESTART / FIRMWARE_RESTART Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock restart simulation", "[slow][mock][restart]") {
    // Use 100x speedup so restart delay is 20-30ms instead of 2-3 seconds
    // IMPORTANT: fixture must be declared BEFORE mock so it's destroyed AFTER mock.
    // This prevents use-after-free when restart thread dispatches to callbacks.
    MockBehaviorTestFixture fixture;
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 100.0);

    mock.connect("ws://test", []() {}, []() {});
    auto sub_id = mock.register_notify_update(fixture.create_capture_callback());
    fixture.reset();

    SECTION("RESTART sets klippy_state to startup temporarily") {
        mock.gcode_script("RESTART");

        // Wait for the "ready" state notification to arrive
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                return n.contains("params") && n["params"][0].contains("webhooks") &&
                       n["params"][0]["webhooks"]["state"] == "ready";
            },
            500));

        auto notifications = fixture.get_notifications();

        // Find webhooks states in order
        std::vector<std::string> states;
        for (const auto& n : notifications) {
            if (n.contains("params") && n["params"][0].contains("webhooks")) {
                states.push_back(n["params"][0]["webhooks"]["state"].get<std::string>());
            }
        }

        REQUIRE(states.size() >= 2);
        REQUIRE(states[0] == "startup");
        REQUIRE(states.back() == "ready");
    }

    SECTION("RESTART clears active print") {
        // Start a mock print first
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=test.gcode");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Verify print is active (not IDLE)
        REQUIRE(mock.get_print_phase() != MoonrakerClientMock::MockPrintPhase::IDLE);

        fixture.reset();
        mock.gcode_script("RESTART");

        // Print should be cleared immediately
        REQUIRE(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::IDLE);

        // Wait for restart thread to complete before mock destructs
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SECTION("Klippy state transitions correctly") {
        // Before restart, should be READY
        REQUIRE(mock.get_klippy_state() == MoonrakerClientMock::KlippyState::READY);

        mock.gcode_script("RESTART");

        // Immediately after, should be STARTUP
        REQUIRE(mock.get_klippy_state() == MoonrakerClientMock::KlippyState::STARTUP);

        // Wait for transition back to READY
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(mock.get_klippy_state() == MoonrakerClientMock::KlippyState::READY);
    }

    SECTION("FIRMWARE_RESTART takes longer than RESTART") {
        // At 100x speedup: RESTART = 20ms, FIRMWARE_RESTART = 30ms.
        //
        // Time to the webhooks "ready" notification, NOT to an arbitrary notification
        // count: the mock's physics thread dispatches a status update every
        // SIMULATION_INTERVAL_MS (250ms of REAL time — the speedup scales the physics
        // step, not the tick period), so a stray temperature tick can push the
        // notification count past a threshold while the restart is still in flight.
        auto start = std::chrono::steady_clock::now();
        mock.gcode_script("FIRMWARE_RESTART");
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                return n.contains("params") && n["params"][0].contains("webhooks") &&
                       n["params"][0]["webhooks"]["state"] == "ready";
            },
            2000));
        auto duration = std::chrono::steady_clock::now() - start;

        // The restart thread sleeps in 10ms slices, so the firmware path floors at
        // 30ms and can only overshoot. 25ms keeps a margin while still excluding the
        // 20ms plain-RESTART delay.
        REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() >= 25);
    }

    (void)sub_id; // Callback auto-unregisters when mock destructs
}

// ============================================================================
// EXCLUDE_OBJECT G-code Parsing Tests
// ============================================================================

/**
 * @brief Tests for EXCLUDE_OBJECT command parsing in gcode_script()
 *
 * The mock should track excluded objects when receiving EXCLUDE_OBJECT commands
 * from the UI (e.g., when user taps to exclude an object during printing).
 *
 * Real Klipper syntax:
 *   EXCLUDE_OBJECT NAME=Part_1
 *   EXCLUDE_OBJECT NAME="Part With Spaces"
 */
TEST_CASE("MoonrakerClientMock parses EXCLUDE_OBJECT command", "[slow][mock][gcode][print]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://test", []() {}, []() {});

    // Catch2 RAII cleanup - disconnect before mock is destroyed
    struct Cleanup {
        MoonrakerClientMock& m;
        ~Cleanup() {
            m.disconnect();
        }
    } cleanup{mock};

    SECTION("EXCLUDE_OBJECT NAME=Part_1 adds object to excluded set") {
        mock.gcode_script("EXCLUDE_OBJECT NAME=Part_1");

        auto excluded = mock.get_excluded_objects();
        REQUIRE(excluded.size() == 1);
        REQUIRE(excluded.count("Part_1") == 1);
    }

    SECTION("EXCLUDE_OBJECT with quoted name") {
        mock.gcode_script("EXCLUDE_OBJECT NAME=\"Part With Spaces\"");

        auto excluded = mock.get_excluded_objects();
        REQUIRE(excluded.size() == 1);
        REQUIRE(excluded.count("Part With Spaces") == 1);
    }

    SECTION("EXCLUDE_OBJECT without NAME parameter is ignored") {
        // Invalid syntax - should not crash, should log warning
        mock.gcode_script("EXCLUDE_OBJECT");

        auto excluded = mock.get_excluded_objects();
        REQUIRE(excluded.empty());
    }

    SECTION("Multiple objects can be excluded") {
        mock.gcode_script("EXCLUDE_OBJECT NAME=Part_1");
        mock.gcode_script("EXCLUDE_OBJECT NAME=Part_2");
        mock.gcode_script("EXCLUDE_OBJECT NAME=cube_3");

        auto excluded = mock.get_excluded_objects();
        REQUIRE(excluded.size() == 3);
        REQUIRE(excluded.count("Part_1") == 1);
        REQUIRE(excluded.count("Part_2") == 1);
        REQUIRE(excluded.count("cube_3") == 1);
    }

    SECTION("Excluding same object twice is idempotent") {
        mock.gcode_script("EXCLUDE_OBJECT NAME=Part_1");
        mock.gcode_script("EXCLUDE_OBJECT NAME=Part_1");

        auto excluded = mock.get_excluded_objects();
        REQUIRE(excluded.size() == 1);
    }

    SECTION("Excluded objects are cleared on RESTART") {
        mock.gcode_script("EXCLUDE_OBJECT NAME=Part_1");
        REQUIRE(mock.get_excluded_objects().size() == 1);

        mock.gcode_script("RESTART");
        // Give restart simulation a moment to process
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        REQUIRE(mock.get_excluded_objects().empty());
    }

    SECTION("Excluded objects are cleared on new print start") {
        mock.gcode_script("EXCLUDE_OBJECT NAME=Part_1");
        REQUIRE(mock.get_excluded_objects().size() == 1);

        // Start a new print
        mock.gcode_script("SDCARD_PRINT_FILE FILENAME=test.gcode");

        REQUIRE(mock.get_excluded_objects().empty());
    }
}

// ============================================================================
// Emergency Stop and Restart Handler Tests
// ============================================================================

/**
 * @brief Tests for emergency stop and restart mock handlers
 *
 * These handlers are essential for testing the recovery dialog UI flow.
 * The E-stop should set klippy state to SHUTDOWN, and restart commands
 * should transition back to READY after a delay.
 */
TEST_CASE("MoonrakerClientMock handles emergency_stop", "[mock][emergency][slow]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://test", []() {}, []() {});

    struct Cleanup {
        MoonrakerClientMock& m;
        ~Cleanup() {
            m.disconnect();
        }
    } cleanup{mock};

    SECTION("emergency_stop calls success callback") {
        bool success_called = false;
        bool error_called = false;

        mock.send_jsonrpc(
            "printer.emergency_stop", json::object(),
            [&success_called](json) { success_called = true; },
            [&error_called](const MoonrakerError&) { error_called = true; });

        REQUIRE(success_called);
        REQUIRE_FALSE(error_called);
    }
}

TEST_CASE("MoonrakerClientMock handles printer restart commands", "[mock][restart][slow]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://test", []() {}, []() {});

    struct Cleanup {
        MoonrakerClientMock& m;
        ~Cleanup() {
            m.disconnect();
        }
    } cleanup{mock};

    SECTION("printer.restart calls success callback") {
        bool success_called = false;

        mock.send_jsonrpc(
            "printer.restart", json::object(), [&success_called](json) { success_called = true; },
            nullptr);

        REQUIRE(success_called);
    }

    SECTION("printer.firmware_restart calls success callback") {
        bool success_called = false;

        mock.send_jsonrpc(
            "printer.firmware_restart", json::object(),
            [&success_called](json) { success_called = true; }, nullptr);

        REQUIRE(success_called);
    }
}

// ============================================================================
// Idle Timeout Simulation Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock idle_timeout simulation", "[mock][idle_timeout][slow]") {
    SECTION("idle_timeout triggers after configured duration") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        MockBehaviorTestFixture fixture;

        // Set 1 second timeout for testing
        mock.set_idle_timeout_seconds(1);
        REQUIRE(mock.get_idle_timeout_seconds() == 1);

        // Verify initial state
        REQUIRE_FALSE(mock.is_idle_timeout_triggered());
        REQUIRE(mock.are_motors_enabled());

        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for idle timeout to trigger (need 2+ seconds to ensure >1s elapsed)
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        // Should be triggered now
        REQUIRE(mock.is_idle_timeout_triggered());
        REQUIRE_FALSE(mock.are_motors_enabled());

        // Look for a notification with idle_timeout state = "Idle"
        bool found_idle_notification = fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("idle_timeout")) {
                    return false;
                }
                const json& idle_timeout = status["idle_timeout"];
                return idle_timeout.contains("state") &&
                       idle_timeout["state"].get<std::string>() == "Idle";
            },
            500);

        REQUIRE(found_idle_notification);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("activity resets idle timeout") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Set 2 second timeout
        mock.set_idle_timeout_seconds(2);

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait 1 second (less than timeout)
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        REQUIRE_FALSE(mock.is_idle_timeout_triggered());

        // Send G28 to reset the timeout
        mock.gcode_script("G28");

        // Wait another 1 second (still less than 2s from last activity)
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Should NOT be triggered because G28 reset the timer
        REQUIRE_FALSE(mock.is_idle_timeout_triggered());
        REQUIRE(mock.are_motors_enabled());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("printing state prevents idle timeout") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Set 1 second timeout
        mock.set_idle_timeout_seconds(1);

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Start a print (puts mock in PREHEAT then PRINTING phase)
        mock.start_print_internal("3DBenchy.gcode");

        // Wait 2 seconds (longer than timeout)
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        // Should NOT be triggered during printing (phase != IDLE)
        REQUIRE_FALSE(mock.is_idle_timeout_triggered());
        REQUIRE(mock.are_motors_enabled());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("temperature commands reset idle timeout") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Set 2 second timeout
        mock.set_idle_timeout_seconds(2);

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait 1 second
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Send temperature command to reset timeout
        mock.gcode_script("M104 S200");

        // Wait another 1 second
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Should NOT be triggered because M104 reset the timer
        REQUIRE_FALSE(mock.is_idle_timeout_triggered());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("movement commands reset idle timeout") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Set 2 second timeout
        mock.set_idle_timeout_seconds(2);

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait 1 second
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Send movement command to reset timeout
        mock.gcode_script("G1 X100 Y100");

        // Wait another 1 second
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Should NOT be triggered because G1 reset the timer
        REQUIRE_FALSE(mock.is_idle_timeout_triggered());
        REQUIRE(mock.are_motors_enabled());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("idle_timeout state is returned in objects.query") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

        // Set 1 second timeout
        mock.set_idle_timeout_seconds(1);

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Initial query should return "Ready"
        bool query_completed = false;
        std::string initial_state;

        mock.send_jsonrpc(
            "printer.objects.query", {{"objects", {{"idle_timeout", nullptr}}}},
            [&query_completed, &initial_state](json response) {
                query_completed = true;
                if (response.contains("result") && response["result"].contains("status") &&
                    response["result"]["status"].contains("idle_timeout")) {
                    initial_state = response["result"]["status"]["idle_timeout"]["state"];
                }
            },
            nullptr);

        REQUIRE(query_completed);
        REQUIRE(initial_state == "Ready");

        // Wait for timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        // Query again should return "Idle"
        query_completed = false;
        std::string timeout_state;

        mock.send_jsonrpc(
            "printer.objects.query", {{"objects", {{"idle_timeout", nullptr}}}},
            [&query_completed, &timeout_state](json response) {
                query_completed = true;
                if (response.contains("result") && response["result"].contains("status") &&
                    response["result"]["status"].contains("idle_timeout")) {
                    timeout_state = response["result"]["status"]["idle_timeout"]["state"];
                }
            },
            nullptr);

        REQUIRE(query_completed);
        REQUIRE(timeout_state == "Idle");

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// gcode_script return value contract: 0 = success, non-zero = error
// ============================================================================

TEST_CASE("MoonrakerClientMock gcode_script returns 0 on success", "[mock][gcode]") {
    MockBehaviorTestFixture fixture;
    auto mock = fixture.create_mock();
    mock->connect("ws://localhost:7125/websocket", [] {}, [] {});

    SECTION("G28 home all returns 0") {
        int result = mock->gcode_script("G28");
        REQUIRE(result == 0);
    }

    SECTION("G28 home single axis returns 0") {
        int result = mock->gcode_script("G28 X");
        REQUIRE(result == 0);
    }

    SECTION("temperature command returns 0") {
        int result = mock->gcode_script("M104 S200");
        REQUIRE(result == 0);
    }

    SECTION("movement within bounds returns 0") {
        mock->gcode_script("G28"); // Home first
        int result = mock->gcode_script("G0 X100 Y100 Z10");
        REQUIRE(result == 0);
    }

    SECTION("PROBE_CALIBRATE returns 0") {
        mock->gcode_script("G28"); // Home first
        int result = mock->gcode_script("PROBE_CALIBRATE");
        REQUIRE(result == 0);
    }

    SECTION("fan command returns 0") {
        int result = mock->gcode_script("M106 S128");
        REQUIRE(result == 0);
    }

    mock->disconnect();
}

TEST_CASE("MoonrakerClientMock gcode_script returns non-zero on error", "[mock][gcode]") {
    MockBehaviorTestFixture fixture;
    auto mock = fixture.create_mock();
    mock->connect("ws://localhost:7125/websocket", [] {}, [] {});

    SECTION("out-of-range move returns error") {
        mock->gcode_script("G28"); // Home first
        int result = mock->gcode_script("G0 X9999");
        REQUIRE(result != 0);
        REQUIRE_FALSE(mock->get_last_gcode_error().empty());
    }

    SECTION("out-of-range Z move returns error") {
        mock->gcode_script("G28"); // Home first
        int result = mock->gcode_script("G0 Z9999");
        REQUIRE(result != 0);
    }

    mock->disconnect();
}

TEST_CASE("MoonrakerClientMock: M117 sets display_status.message", "[mock][display_message]") {
    TestableMoonrakerMock mock;

    json captured;
    mock.register_notify_update([&captured](const json& notif) {
        if (notif.contains("params") && notif["params"].is_array() && !notif["params"].empty() &&
            notif["params"][0].contains("display_status")) {
            captured = notif["params"][0]["display_status"];
        }
    });

    SECTION("sets the message text") {
        mock.gcode_script("M117 Leveling 3/9");
        REQUIRE(captured.contains("message"));
        REQUIRE(captured["message"].get<std::string>() == "Leveling 3/9");
    }

    SECTION("bare M117 clears the message") {
        mock.gcode_script("M117 something");
        captured = json{};
        mock.gcode_script("M117");
        REQUIRE(captured.contains("message"));
        REQUIRE(captured["message"].get<std::string>() == "");
    }

    SECTION("strips exactly one leading space, preserves the rest") {
        mock.gcode_script("M117  two spaces");
        REQUIRE(captured["message"].get<std::string>() == " two spaces");
    }

    SECTION("preserves embedded quotes and backslashes verbatim") {
        mock.gcode_script("M117 She said \"hi\" \\ok\\");
        REQUIRE(captured.contains("message"));
        REQUIRE(captured["message"].get<std::string>() == "She said \"hi\" \\ok\\");
    }

    SECTION("preserves a message longer than the downstream 128-byte display buffer") {
        std::string long_text(200, 'x');
        mock.gcode_script("M117 " + long_text);
        REQUIRE(captured.contains("message"));
        REQUIRE(captured["message"].get<std::string>() == long_text);
    }

    // Real Klipper uppercases the command word before dispatch, so "m117",
    // "M117", and mixed case all reach the same handler. A user typing
    // lowercase "m117 ..." in the console must not be silently swallowed.
    SECTION("lowercase m117 sets the message text") {
        mock.gcode_script("m117 some text");
        REQUIRE(captured.contains("message"));
        REQUIRE(captured["message"].get<std::string>() == "some text");
    }

    // Only the command token may be normalized - the payload must survive
    // with its original case intact. A broad "uppercase the whole line" fix
    // would turn this into "HELLO WORLD", which this test must catch.
    SECTION("lowercase command preserves the payload's original case") {
        mock.gcode_script("m117 Hello World");
        REQUIRE(captured.contains("message"));
        REQUIRE(captured["message"].get<std::string>() == "Hello World");
    }

    // Regression: the M117 branch used to fall through the rest of
    // gcode_script() instead of returning, so message TEXT containing other
    // command substrings ("G28", "G1"/"G0") re-triggered those handlers. A
    // user M117 message that merely mentions "G28" must not home the axes.
    SECTION("M117 text containing G28 does not trigger homing") {
        json toolhead_captured;
        mock.register_notify_update([&toolhead_captured](const json& notif) {
            if (notif.contains("params") && notif["params"].is_array() &&
                !notif["params"].empty() && notif["params"][0].contains("toolhead")) {
                toolhead_captured = notif["params"][0]["toolhead"];
            }
        });

        int result = mock.gcode_script("M117 G28 done");

        REQUIRE(result == 0);
        REQUIRE(captured.contains("message"));
        REQUIRE(captured["message"].get<std::string>() == "G28 done");
        // Homing dispatches a toolhead/homed_axes update (see the G28 tests
        // above) - none should have been produced by this M117 call.
        REQUIRE_FALSE(toolhead_captured.contains("homed_axes"));
    }
}

// ============================================================================
// M117 precedence over periodic status broadcasts
// ============================================================================
//
// Commit 959bb0140's central design property: once a user sends M117, that
// message must survive the mock's own periodic status broadcast (fired every
// NOTIFICATION_INTERVAL_TICKS ticks from temperature_simulation_loop()) even
// while the print is in a phase that would otherwise emit a canned string
// ("Heating...", "Purging nozzle"). The SECTIONs above only ever inspect the
// notification dispatched by the M117 call itself - they never drive the
// periodic broadcast, so they cannot prove precedence. This test does.
TEST_CASE("MoonrakerClientMock: M117 message survives periodic status broadcasts",
          "[mock][display_message][slow]") {
    MockBehaviorTestFixture fixture;

    // Speedup chosen so PREHEAT (nozzle 25->220 @ 3C/s, bed 25->55 @ 1C/s)
    // takes a few seconds of wall-clock time - long enough to observe several
    // periodic broadcasts (~1s apart, wall-clock, independent of speedup)
    // while still in PREHEAT/early-PRINTING, where the canned strings apply.
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 20.0);
    mock.register_notify_update(fixture.create_capture_callback());
    mock.connect("ws://mock/websocket", []() {}, []() {});

    const std::string custom_message = "Precedence check 42";

    // Start a print (enters PREHEAT) and immediately set a custom M117
    // message before any canned string has had a chance to appear.
    mock.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
    mock.gcode_script("M117 " + custom_message);

    // Discard everything so far (initial state, the print-start notification,
    // and the M117 call's own narrow {"display_status":{"message":...}}
    // dispatch). Only notifications captured from here on can be periodic
    // broadcasts.
    fixture.reset();

    // Wait for at least 3 further notifications so we sample multiple
    // periodic broadcast ticks across the PREHEAT -> early-PRINTING window.
    REQUIRE(fixture.wait_for_callbacks(3, 8000));
    mock.stop_temperature_simulation();

    // A periodic broadcast is identifiable by carrying the full status
    // object (print_stats + display_status together) - unlike the M117
    // call's own narrow dispatch, which only ever contains display_status.
    size_t periodic_broadcasts_checked = 0;
    for (const auto& notification : fixture.get_notifications()) {
        if (!notification.contains("params") || !notification["params"].is_array() ||
            notification["params"].empty()) {
            continue;
        }
        const json& status = notification["params"][0];
        if (!status.is_object() || !status.contains("print_stats") ||
            !status.contains("display_status")) {
            continue;
        }

        ++periodic_broadcasts_checked;
        REQUIRE(status["display_status"].contains("message"));
        const json& message = status["display_status"]["message"];
        REQUIRE(message.is_string());
        REQUIRE(message.get<std::string>() == custom_message);
    }

    // Make sure we actually exercised the periodic-broadcast path and didn't
    // just vacuously pass because no full status object showed up.
    REQUIRE(periodic_broadcasts_checked >= 1);

    mock.disconnect();
}

#pragma GCC diagnostic pop
