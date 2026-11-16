// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_mock_motion.cpp
 * @brief Unit tests for MoonrakerClientMock move/home command handling
 *
 * Tests the mock client's G-code motion command processing:
 * - G0/G1: Movement commands with position updates
 * - G28: Homing commands (all axes and individual)
 * - G90/G91: Absolute/relative positioning modes
 *
 * These tests verify position state is correctly updated and reflected
 * in the toolhead.position and toolhead.homed_axes notification fields.
 */

#include "error_classify.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "rpc_error_correlation.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Test Fixture for Motion Testing
// ============================================================================

/**
 * @brief Test fixture that captures notifications from MoonrakerClientMock
 *
 * Provides helpers for waiting on callbacks and validating position state.
 */
class MockMotionTestFixture {
  public:
    MockMotionTestFixture() = default;

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
        last_matched_ = json{};
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
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& n : notifications_) {
                    if (predicate(n)) {
                        // Capture the exact notification that matched so callers can
                        // assert against the validated snapshot instead of a racing
                        // backward re-scan (see get_matched_position()).
                        last_matched_ = n;
                        return true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    /**
     * @brief Get the latest toolhead position from notifications
     * @return Position array [x, y, z, e] or nullopt if not found
     */
    std::optional<std::array<double, 4>> get_latest_position() const {
        std::lock_guard<std::mutex> lock(mutex_);
        // Search backwards for most recent notification with position
        for (auto it = notifications_.rbegin(); it != notifications_.rend(); ++it) {
            const json& n = *it;
            if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
                const json& status = n["params"][0];
                if (status.is_object() && status.contains("toolhead") &&
                    status["toolhead"].contains("position") &&
                    status["toolhead"]["position"].is_array() &&
                    status["toolhead"]["position"].size() == 4) {
                    return std::array<double, 4>{status["toolhead"]["position"][0].get<double>(),
                                                 status["toolhead"]["position"][1].get<double>(),
                                                 status["toolhead"]["position"][2].get<double>(),
                                                 status["toolhead"]["position"][3].get<double>()};
                }
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Get the toolhead position from the notification that satisfied the
     *        most recent wait_for_matching() predicate.
     * @return Position array [x, y, z, e] or nullopt if the matched notification
     *         carried no toolhead.position.
     * @note Deterministic: reads the exact snapshot the wait validated, so it is
     *       immune to the temperature-simulation thread appending a newer
     *       notification with a stale position between the wait and this read.
     *       Uses the same null-safe extraction as get_latest_position().
     */
    std::optional<std::array<double, 4>> get_matched_position() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const json& n = last_matched_;
        if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
            const json& status = n["params"][0];
            if (status.is_object() && status.contains("toolhead") &&
                status["toolhead"].contains("position") &&
                status["toolhead"]["position"].is_array() &&
                status["toolhead"]["position"].size() == 4) {
                return std::array<double, 4>{status["toolhead"]["position"][0].get<double>(),
                                             status["toolhead"]["position"][1].get<double>(),
                                             status["toolhead"]["position"][2].get<double>(),
                                             status["toolhead"]["position"][3].get<double>()};
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Get the latest homed_axes from notifications
     * @return homed_axes string or nullopt if not found
     */
    std::optional<std::string> get_latest_homed_axes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        // Search backwards for most recent notification with homed_axes
        for (auto it = notifications_.rbegin(); it != notifications_.rend(); ++it) {
            const json& n = *it;
            if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
                const json& status = n["params"][0];
                if (status.is_object() && status.contains("toolhead") &&
                    status["toolhead"].contains("homed_axes")) {
                    return status["toolhead"]["homed_axes"].get<std::string>();
                }
            }
        }
        return std::nullopt;
    }

  private:
    mutable std::mutex mutex_; // mutable for const methods
    std::condition_variable cv_;
    std::atomic<bool> callback_invoked_{false};
    std::vector<json> notifications_;
    // The exact notification that satisfied the most recent wait_for_matching()
    // predicate. Reading position from this snapshot is race-free: the background
    // temperature-simulation thread keeps appending notifications concurrently, so
    // a fresh get_latest_position() backward scan can pick up a temp-sim
    // notification whose position snapshot was read before the move applied.
    json last_matched_;
};

/**
 * @brief Helper to compare doubles with tolerance
 */
inline bool approx_equal(double a, double b, double tolerance = 0.001) {
    return std::abs(a - b) < tolerance;
}

// ============================================================================
// Movement Command Tests (G0/G1)
// ============================================================================

TEST_CASE("MoonrakerClientMock G0/G1 movement commands", "[api][movement]") {
    MockMotionTestFixture fixture;

    SECTION("G0 X Y movement updates position") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for initial state
        REQUIRE(fixture.wait_for_callback(500));

        // Send move command
        mock.gcode_script("G0 X10 Y20");

        // Wait for position update in status notification
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead["position"].is_array()) {
                    return false;
                }
                // Check if X=10, Y=20
                double x = toolhead["position"][0].get<double>();
                double y = toolhead["position"][1].get<double>();
                return approx_equal(x, 10.0) && approx_equal(y, 20.0);
            },
            2000));

        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 10.0)); // X
        REQUIRE(approx_equal((*pos)[1], 20.0)); // Y

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G1 Z movement with feedrate updates position") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // Send Z move with feedrate (F parameter should be ignored for position)
        mock.gcode_script("G1 Z5 F600");

        // Wait for Z position update
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead["position"].is_array()) {
                    return false;
                }
                double z = toolhead["position"][2].get<double>();
                return approx_equal(z, 5.0);
            },
            2000));

        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[2], 5.0)); // Z

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G0 diagonal move updates multiple axes") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // Send diagonal move
        mock.gcode_script("G0 X10 Y10 Z5");

        // Wait for all axes to update
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead["position"].is_array()) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                double y = toolhead["position"][1].get<double>();
                double z = toolhead["position"][2].get<double>();
                return approx_equal(x, 10.0) && approx_equal(y, 10.0) && approx_equal(z, 5.0);
            },
            2000));

        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 10.0)); // X
        REQUIRE(approx_equal((*pos)[1], 10.0)); // Y
        REQUIRE(approx_equal((*pos)[2], 5.0));  // Z

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G91 relative mode with G0 incremental move") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // First move to absolute position
        mock.gcode_script("G90"); // Ensure absolute mode
        mock.gcode_script("G0 X10 Y10");

        // Wait for position update
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead["position"].is_array()) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                double y = toolhead["position"][1].get<double>();
                return approx_equal(x, 10.0) && approx_equal(y, 10.0);
            },
            2000));

        // Switch to relative mode and move
        mock.gcode_script("G91");
        mock.gcode_script("G0 X5 Y5");

        // Wait for incremental move result (10+5=15, 10+5=15)
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead["position"].is_array()) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                double y = toolhead["position"][1].get<double>();
                return approx_equal(x, 15.0) && approx_equal(y, 15.0);
            },
            2000));

        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 15.0)); // X = 10 + 5
        REQUIRE(approx_equal((*pos)[1], 15.0)); // Y = 10 + 5

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G90 returns to absolute mode after G91") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // Start in absolute mode, move to 10,10
        mock.gcode_script("G90");
        mock.gcode_script("G0 X10 Y10");

        // Switch to relative, move +5
        mock.gcode_script("G91");
        mock.gcode_script("G0 X5"); // Now at 15,10

        // Switch back to absolute, move to 20
        mock.gcode_script("G90");
        mock.gcode_script("G0 X20"); // Now at 20,10 (not 35!)

        // Wait for absolute position
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead["position"].is_array()) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                return approx_equal(x, 20.0);
            },
            2000));

        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 20.0)); // X = absolute 20

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Homing Command Tests (G28)
// ============================================================================

TEST_CASE("MoonrakerClientMock G28 homing commands", "[slow][api][homing]") {
    MockMotionTestFixture fixture;

    SECTION("G28 homes all axes and resets to zero") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // First move to a non-zero position
        mock.gcode_script("G0 X50 Y50 Z25");

        // Wait for position update
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead["position"].is_array()) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                return approx_equal(x, 50.0);
            },
            5000));

        // Home all axes
        mock.gcode_script("G28");

        // Wait for homed state: all axes at 0 and homed_axes = "xyz"
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead.contains("homed_axes")) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                double y = toolhead["position"][1].get<double>();
                double z = toolhead["position"][2].get<double>();
                std::string homed = toolhead["homed_axes"].get<std::string>();
                return approx_equal(x, 0.0) && approx_equal(y, 0.0) && approx_equal(z, 0.0) &&
                       homed == "xyz";
            },
            5000));

        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 0.0));
        REQUIRE(approx_equal((*pos)[1], 0.0));
        REQUIRE(approx_equal((*pos)[2], 0.0));

        auto homed = fixture.get_latest_homed_axes();
        REQUIRE(homed.has_value());
        REQUIRE(*homed == "xyz");

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G28 X homes only X axis") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // Move to non-zero position
        mock.gcode_script("G0 X50 Y50 Z25");

        // Wait for position update
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position")) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                return approx_equal(x, 50.0);
            },
            5000));

        // Home X only
        mock.gcode_script("G28 X");

        // Wait for X=0, Y and Z unchanged
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead.contains("homed_axes")) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                std::string homed = toolhead["homed_axes"].get<std::string>();
                // X should be 0, homed_axes should contain 'x'
                return approx_equal(x, 0.0) && homed.find('x') != std::string::npos;
            },
            5000));

        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 0.0));  // X homed to 0
        REQUIRE(approx_equal((*pos)[1], 50.0)); // Y unchanged
        REQUIRE(approx_equal((*pos)[2], 25.0)); // Z unchanged

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("G28 X Y homes only X and Y axes") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // Move to non-zero position
        mock.gcode_script("G0 X50 Y50 Z25");

        // Wait for position update
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position")) {
                    return false;
                }
                double z = toolhead["position"][2].get<double>();
                return approx_equal(z, 25.0);
            },
            5000));

        // Home X and Y only
        mock.gcode_script("G28 X Y");

        // Wait for X=0, Y=0, Z unchanged
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead.contains("homed_axes")) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                double y = toolhead["position"][1].get<double>();
                std::string homed = toolhead["homed_axes"].get<std::string>();
                // X and Y should be 0, homed_axes should contain 'x' and 'y'
                return approx_equal(x, 0.0) && approx_equal(y, 0.0) &&
                       homed.find('x') != std::string::npos && homed.find('y') != std::string::npos;
            },
            5000));

        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 0.0));  // X homed
        REQUIRE(approx_equal((*pos)[1], 0.0));  // Y homed
        REQUIRE(approx_equal((*pos)[2], 25.0)); // Z unchanged

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Homing from non-zero position resets correctly") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // Move to far position
        mock.gcode_script("G0 X100 Y150 Z50");

        // Wait for position update
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position")) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                double y = toolhead["position"][1].get<double>();
                double z = toolhead["position"][2].get<double>();
                return approx_equal(x, 100.0) && approx_equal(y, 150.0) && approx_equal(z, 50.0);
            },
            5000));

        // Home all
        mock.gcode_script("G28");

        // Wait for all positions at 0 with homed_axes = "xyz"
        // The wait_for_matching already validates the values, so if it returns true,
        // the mock correctly set positions to 0 after homing
        bool found_homed_state = fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position") || !toolhead.contains("homed_axes")) {
                    return false;
                }
                double x = toolhead["position"][0].get<double>();
                double y = toolhead["position"][1].get<double>();
                double z = toolhead["position"][2].get<double>();
                std::string homed = toolhead["homed_axes"].get<std::string>();
                return approx_equal(x, 0.0) && approx_equal(y, 0.0) && approx_equal(z, 0.0) &&
                       homed == "xyz";
            },
            5000);

        // The predicate itself validates position is 0,0,0 with homed_axes="xyz"
        REQUIRE(found_homed_state);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Position Reporting Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock position in status updates", "[api][position_reporting]") {
    MockMotionTestFixture fixture;

    SECTION("Position updates are reflected in status notifications") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));

        // Series of moves
        mock.gcode_script("G0 X25");
        mock.gcode_script("G0 Y35");
        mock.gcode_script("G0 Z10");

        // Wait for final position
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.is_object() || !status.contains("toolhead")) {
                    return false;
                }
                const json& toolhead = status["toolhead"];
                if (!toolhead.contains("position")) {
                    return false;
                }
                double z = toolhead["position"][2].get<double>();
                return approx_equal(z, 10.0);
            },
            2000));

        // Verify each axis was updated
        auto pos = fixture.get_matched_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 25.0));
        REQUIRE(approx_equal((*pos)[1], 35.0));
        REQUIRE(approx_equal((*pos)[2], 10.0));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Initial state shows position at origin") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));
        mock.stop_temperature_simulation();

        // Find initial state with position
        auto pos = fixture.get_latest_position();
        REQUIRE(pos.has_value());
        REQUIRE(approx_equal((*pos)[0], 0.0));
        REQUIRE(approx_equal((*pos)[1], 0.0));
        REQUIRE(approx_equal((*pos)[2], 0.0));

        mock.disconnect();
    }

    SECTION("toolhead structure matches Moonraker format with valid values") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.register_notify_update(fixture.create_capture_callback());
        mock.connect("ws://mock/websocket", []() {}, []() {});

        REQUIRE(fixture.wait_for_callback(500));
        mock.stop_temperature_simulation();

        // Find notification with toolhead
        bool found_toolhead = false;
        for (const auto& n : fixture.get_notifications()) {
            if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                continue;
            }
            const json& status = n["params"][0];
            if (!status.is_object() || !status.contains("toolhead")) {
                continue;
            }

            const json& toolhead = status["toolhead"];
            found_toolhead = true;

            // Verify required fields exist (Moonraker format)
            REQUIRE(toolhead.contains("position"));
            REQUIRE(toolhead["position"].is_array());
            REQUIRE(toolhead["position"].size() == 4); // [x, y, z, e]

            // Verify position values are valid numbers within reasonable bounds
            double x = toolhead["position"][0].get<double>();
            double y = toolhead["position"][1].get<double>();
            double z = toolhead["position"][2].get<double>();
            double e = toolhead["position"][3].get<double>();

            // Position values should be finite numbers (not NaN or inf)
            REQUIRE(std::isfinite(x));
            REQUIRE(std::isfinite(y));
            REQUIRE(std::isfinite(z));
            REQUIRE(std::isfinite(e));

            // Initial position should be at origin or within reasonable print bed bounds
            // Typical 3D printer bed is 0-300mm, Z 0-300mm
            REQUIRE(x >= -10.0); // Allow small negative for calibration
            REQUIRE(x <= 500.0);
            REQUIRE(y >= -10.0);
            REQUIRE(y <= 500.0);
            REQUIRE(z >= -5.0); // Allow small negative for bed mesh probing
            REQUIRE(z <= 500.0);

            // Verify homed_axes field
            REQUIRE(toolhead.contains("homed_axes"));
            REQUIRE(toolhead["homed_axes"].is_string());

            // homed_axes should only contain valid axis characters or be empty
            std::string homed = toolhead["homed_axes"].get<std::string>();
            for (char c : homed) {
                REQUIRE((c == 'x' || c == 'y' || c == 'z'));
            }

            break;
        }
        REQUIRE(found_toolhead);

        mock.disconnect();
    }
}

// ============================================================================
// Out-of-Range Movement Error Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock out-of-range move error handling", "[api][movement][errors]") {
    SECTION("Move beyond X_MAX returns error") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        // First clear any existing error
        mock.gcode_script("G28"); // Home to reset position
        REQUIRE(mock.get_last_gcode_error().empty());

        // Try to move beyond kXMax (250mm for the Voron 2.4 persona)
        int result = mock.gcode_script("G0 X400");

        // Should return non-zero error code
        REQUIRE(result != 0);

        // Should have error message
        std::string error = mock.get_last_gcode_error();
        REQUIRE(!error.empty());
        REQUIRE(error.find("out of range") != std::string::npos);
        REQUIRE(error.find("X=") != std::string::npos);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Move beyond Y_MAX returns error") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("G28");
        REQUIRE(mock.get_last_gcode_error().empty());

        // Try to move beyond kYMax (250mm)
        int result = mock.gcode_script("G0 Y500");

        REQUIRE(result != 0);
        std::string error = mock.get_last_gcode_error();
        REQUIRE(!error.empty());
        REQUIRE(error.find("out of range") != std::string::npos);
        REQUIRE(error.find("Y=") != std::string::npos);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Move beyond Z_MAX returns error") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("G28");
        REQUIRE(mock.get_last_gcode_error().empty());

        // Try to move beyond Z_MAX (300mm)
        int result = mock.gcode_script("G0 Z400");

        REQUIRE(result != 0);
        std::string error = mock.get_last_gcode_error();
        REQUIRE(!error.empty());
        REQUIRE(error.find("out of range") != std::string::npos);
        REQUIRE(error.find("Z=") != std::string::npos);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Move below X_MIN (negative) returns error") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("G28");
        REQUIRE(mock.get_last_gcode_error().empty());

        // Try to move below the persona's X minimum (-175 on the centre-origin
        // Voron 2.4)
        int result = mock.gcode_script("G0 X-180");

        REQUIRE(result != 0);
        std::string error = mock.get_last_gcode_error();
        REQUIRE(!error.empty());
        REQUIRE(error.find("out of range") != std::string::npos);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Valid move within bounds succeeds") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("G28");

        // Move within valid range
        int result = mock.gcode_script("G0 X100 Y100 Z50");

        REQUIRE(result == 0);
        REQUIRE(mock.get_last_gcode_error().empty());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Error is cleared on next successful command") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("G28");

        // First, cause an error
        int result1 = mock.gcode_script("G0 X400");
        REQUIRE(result1 != 0);
        REQUIRE(!mock.get_last_gcode_error().empty());

        // Then do a valid command - error should be cleared
        int result2 = mock.gcode_script("G0 X100");
        REQUIRE(result2 == 0);
        REQUIRE(mock.get_last_gcode_error().empty());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Relative move beyond bounds returns error") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("G28");     // Start at 0,0,0
        mock.gcode_script("G0 X100"); // Move to X=100

        // Switch to relative mode
        mock.gcode_script("G91");

        // Try to move +100 from X=100, which would put us at X=200 (out of range)
        int result = mock.gcode_script("G0 X100");

        REQUIRE(result != 0);
        std::string error = mock.get_last_gcode_error();
        REQUIRE(!error.empty());
        REQUIRE(error.find("out of range") != std::string::npos);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("RPC handler calls error callback for out-of-range move") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("G28"); // Reset position

        // Track callback invocations
        bool success_called = false;
        bool error_called = false;
        MoonrakerError captured_error;

        // Use send_jsonrpc with callbacks (this goes through the RPC handler)
        json params = {{"script", "G0 X400"}}; // Out of range
        mock.send_jsonrpc(
            "printer.gcode.script", params, [&success_called](json) { success_called = true; },
            [&error_called, &captured_error](const MoonrakerError& err) {
                error_called = true;
                captured_error = err;
            });

        // Error callback should be called, not success
        REQUIRE(!success_called);
        REQUIRE(error_called);
        REQUIRE(captured_error.has_error());
        REQUIRE(captured_error.message.find("out of range") != std::string::npos);
        REQUIRE(captured_error.method == "printer.gcode.script");

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("RPC handler calls success callback for valid move") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        mock.gcode_script("G28"); // Reset position

        bool success_called = false;
        bool error_called = false;

        // Valid move within bounds
        json params = {{"script", "G0 X100 Y100"}};
        mock.send_jsonrpc(
            "printer.gcode.script", params, [&success_called](json) { success_called = true; },
            [&error_called](const MoonrakerError&) { error_called = true; });

        // Success callback should be called
        REQUIRE(success_called);
        REQUIRE(!error_called);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Multi-line script error fidelity (mock must mirror real Moonraker)
// ============================================================================

TEST_CASE("MoonrakerClientMock multi-line script surfaces the failing line's error",
          "[api][movement][errors][mock_fidelity]") {
    // Every jog the app sends is multi-line: "G91\nG0 X..\nG90". The RPC handler
    // splits the script and calls gcode_script() once per line, and gcode_script()
    // clears the error latch on entry, so the trailing G90 must not wipe the
    // G0's error: the caller would receive an EMPTY message, which
    // user_message() renders as "An unknown error occurred.".
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});
    helix::rpc_error_correlation::clear_for_test();

    mock.gcode_script("G28"); // home -> X=0

    bool success_called = false;
    bool error_called = false;
    MoonrakerError captured;

    // Relative +400 from X=0 lands at 400, beyond the Voron 2.4 kXMax of 250.
    json params = {{"script", "G91\nG0 X400 F6000\nG90"}};
    mock.send_jsonrpc(
        "printer.gcode.script", params, [&success_called](json) { success_called = true; },
        [&](const MoonrakerError& err) {
            error_called = true;
            captured = err;
        });

    CHECK_FALSE(success_called);
    REQUIRE(error_called);

    // The trailing G90 must not erase the failing G0's error.
    REQUIRE_FALSE(captured.message.empty());
    CHECK(captured.message.find("out of range") != std::string::npos);
    CHECK(captured.message.find("X=") != std::string::npos);

    // Real Moonraker's JSON-RPC error message carries no `!!` stream prefix.
    CHECK(captured.message.rfind("!!", 0) != 0);

    // The whole point: the user sees the real reason, not the unknown fallback.
    CHECK(captured.user_message() == captured.message);
    CHECK(captured.user_message() != "An unknown error occurred.");

    mock.stop_temperature_simulation();
    mock.disconnect();
    helix::rpc_error_correlation::clear_for_test();
}

TEST_CASE("MoonrakerClientMock records RPC error correlation like the request tracker",
          "[api][movement][errors][mock_fidelity]") {
    // On real hardware MoonrakerRequestTracker::route_response calls
    // record_caller_handled() whenever the caller supplied its own error_cb, so
    // the `!!` broadcast handler suppresses its independent toast and the user
    // sees ONE readable toast. MoonrakerClientMock::send_jsonrpc dispatches into
    // method_handlers_ inline and bypasses the tracker entirely, so it must
    // record the correlation itself or mock runs double-toast.
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});
    helix::rpc_error_correlation::clear_for_test();

    mock.gcode_script("G28");

    MoonrakerError captured;
    json params = {{"script", "G91\nG0 X400 F6000\nG90"}};
    mock.send_jsonrpc(
        "printer.gcode.script", params, [](json) {},
        [&captured](const MoonrakerError& err) { captured = err; });

    REQUIRE_FALSE(captured.message.empty());
    CHECK(helix::rpc_error_correlation::was_recently_handled(captured.message));

    // End-to-end proof of the dedup contract: the text the `!!` broadcast
    // channel hands GcodeErrorRouter (after error_classify strips the prefix)
    // must EXACTLY equal the recorded RPC message — was_recently_handled() is
    // an exact-string match, so any divergence silently re-enables the second
    // toast. This is the assertion that fails if the latch keeps its `!!`.
    helix::ClassifyContext ctx;
    auto ev = helix::error_classify::classify("!! Move out of range: X=400.000000", ctx);
    REQUIRE(ev.has_value());
    CHECK(ev->detail == captured.message);
    CHECK(helix::rpc_error_correlation::was_recently_handled(ev->detail));

    mock.stop_temperature_simulation();
    mock.disconnect();
    helix::rpc_error_correlation::clear_for_test();
}

TEST_CASE("MoonrakerClientMock does not record correlation when nobody handles the error",
          "[api][movement][errors][mock_fidelity]") {
    // Mirrors the tracker: it records ONLY when an error_cb exists. With no
    // error_cb the caller surfaces nothing, so the `!!` broadcast toast is the
    // user's only signal and must NOT be suppressed.
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});
    helix::rpc_error_correlation::clear_for_test();

    mock.gcode_script("G28");

    json params = {{"script", "G91\nG0 X400 F6000\nG90"}};
    mock.send_jsonrpc("printer.gcode.script", params, [](json) {}, nullptr);

    CHECK_FALSE(
        helix::rpc_error_correlation::was_recently_handled("Move out of range: X=400.000000"));

    mock.stop_temperature_simulation();
    mock.disconnect();
    helix::rpc_error_correlation::clear_for_test();
}
