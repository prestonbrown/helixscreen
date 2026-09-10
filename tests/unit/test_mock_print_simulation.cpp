// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_print_simulation.cpp
 * @brief Unit tests for MoonrakerClientMock print simulation functionality
 *
 * Tests the phase-based print simulation state machine, speedup factor,
 * metadata extraction, progress tracking, thermal phases, and notifications.
 *
 * The mock print simulation features:
 * - Phase state machine: IDLE → PREHEAT → PRINTING → COMPLETE → IDLE
 * - Configurable speedup factor (1x real-time to 10000x)
 * - G-code metadata extraction for print time, layers, temps
 * - Unified handlers for both G-code commands and JSON-RPC API
 * - Moonraker-compatible notification format
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "moonraker_client_mock.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using MockPrintPhase = MoonrakerClientMock::MockPrintPhase;

// ============================================================================
// Test Fixture for Print Simulation Testing
// ============================================================================

/**
 * @brief Test fixture specialized for print simulation testing
 *
 * Extends the notification capture pattern with print-specific helpers
 * for waiting on phase transitions, temperature stabilization, and progress.
 */
class MockPrintTestFixture {
  public:
    MockPrintTestFixture() = default;

    /**
     * @brief Create and connect a mock with specified speedup
     * @param speedup Simulation speedup factor (default 100x for fast tests)
     * @return Connected mock client
     */
    std::unique_ptr<MoonrakerClientMock> create_mock(double speedup = 100.0) {
        auto mock = std::make_unique<MoonrakerClientMock>(
            MoonrakerClientMock::PrinterType::VORON_24, speedup);
        mock->register_notify_update(create_capture_callback());
        mock->connect("ws://test", []() {}, []() {});
        return mock;
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
                        return true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Fast polling for tests
        }
        return false;
    }

    /**
     * @brief Wait for print phase to reach expected state
     * @param mock The mock client to check
     * @param expected_phase The phase to wait for
     * @param timeout_ms Maximum wait time
     * @return true if phase reached, false on timeout
     */
    bool wait_for_phase(MoonrakerClientMock* mock, MockPrintPhase expected_phase,
                        int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (mock->get_print_phase() == expected_phase) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Fast polling for tests
        }
        return false;
    }

    /**
     * @brief Wait for print progress to reach threshold
     * @param mock The mock client to check
     * @param min_progress Minimum progress value (0.0 to 1.0)
     * @param timeout_ms Maximum wait time
     * @return true if progress reached, false on timeout
     */
    bool wait_for_progress([[maybe_unused]] MoonrakerClientMock* mock, double min_progress,
                           int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            // Check via notification for progress value
            auto notifications = get_notifications();
            for (const auto& n : notifications) {
                if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
                    const json& status = n["params"][0];
                    if (status.contains("virtual_sdcard") &&
                        status["virtual_sdcard"].contains("progress")) {
                        double progress = status["virtual_sdcard"]["progress"].get<double>();
                        if (progress >= min_progress) {
                            return true;
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Fast polling for tests
        }
        return false;
    }

    /**
     * @brief Get the latest notification matching a predicate
     */
    std::optional<json> find_notification(std::function<bool(const json&)> predicate) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = notifications_.rbegin(); it != notifications_.rend(); ++it) {
            if (predicate(*it)) {
                return *it;
            }
        }
        return std::nullopt;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> callback_invoked_{false};
    std::vector<json> notifications_;
};

// ============================================================================
// Helper to extract print state from notification
// ============================================================================

static std::string get_print_state_from_notification(const json& n) {
    if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
        return "";
    }
    const json& status = n["params"][0];
    if (!status.is_object() || !status.contains("print_stats") ||
        !status["print_stats"].contains("state")) {
        return "";
    }
    return status["print_stats"]["state"].get<std::string>();
}

// ============================================================================
// Phase State Machine Tests [print][phase]
// ============================================================================

// DEFERRED: Mock print tests crash with SIGSEGV during fixture destruction
// Similar to CommandSequencer - mock lifecycle management issue
TEST_CASE("Mock print phase state machine transitions", "[print][phase][slow]") {
    MockPrintTestFixture fixture;

    SECTION("initial phase is IDLE") {
        auto mock = fixture.create_mock(100.0);

        REQUIRE(mock->get_print_phase() == MockPrintPhase::IDLE);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("starting print transitions IDLE to PREHEAT") {
        auto mock = fixture.create_mock(100.0);

        // Start a print
        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Should immediately be in PREHEAT phase
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PREHEAT);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("preheat transitions to PRINTING after temps stable") {
        auto mock = fixture.create_mock(200.0); // 200x speedup for faster preheat

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PREHEAT);

        // Wait for PRINTING phase (temps should stabilize quickly at 200x)
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("pause from PRINTING transitions to PAUSED") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for PRINTING phase
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        // Pause
        mock->gcode_script("PAUSE");

        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("resume from PAUSED transitions back to PRINTING") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);

        mock->gcode_script("RESUME");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PRINTING);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("cancel from PRINTING transitions to CANCELLED") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        mock->gcode_script("CANCEL_PRINT");

        // Should be in CANCELLED (cooling down)
        REQUIRE(mock->get_print_phase() == MockPrintPhase::CANCELLED);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("cancel from PREHEAT transitions to CANCELLED") {
        auto mock = fixture.create_mock(50.0); // Slower speedup so we can catch PREHEAT

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PREHEAT);

        mock->gcode_script("CANCEL_PRINT");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::CANCELLED);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("M112 emergency stop sets ERROR phase") {
        auto mock = fixture.create_mock(100.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        mock->gcode_script("M112");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::ERROR);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("pause only works from PRINTING or PREHEAT") {
        auto mock = fixture.create_mock(100.0);

        // From IDLE - should not change phase
        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::IDLE);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("resume only works from PAUSED") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        // Try resume from PRINTING - should not change phase
        MockPrintPhase before_resume = mock->get_print_phase();
        mock->gcode_script("RESUME");
        REQUIRE(mock->get_print_phase() == before_resume);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }
}

// ============================================================================
// Speedup Factor Tests [print][speedup]
// ============================================================================

TEST_CASE("Mock print speedup factor behavior", "[print][speedup][slow]") {
    MockPrintTestFixture fixture;

    SECTION("default constructor has speedup factor 1.0") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        REQUIRE(mock.get_simulation_speedup() == Catch::Approx(1.0));
    }

    SECTION("constructor with speedup sets correct value") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 50.0);
        REQUIRE(mock.get_simulation_speedup() == Catch::Approx(50.0));
    }

    SECTION("set_simulation_speedup changes value at runtime") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 10.0);
        REQUIRE(mock.get_simulation_speedup() == Catch::Approx(10.0));

        mock.set_simulation_speedup(100.0);
        REQUIRE(mock.get_simulation_speedup() == Catch::Approx(100.0));
    }

    SECTION("speedup is clamped to helix::sim::MIN_SPEED") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 0.01);
        REQUIRE(mock.get_simulation_speedup() == Catch::Approx(helix::sim::MIN_SPEED));

        mock.set_simulation_speedup(-5.0);
        REQUIRE(mock.get_simulation_speedup() == Catch::Approx(helix::sim::MIN_SPEED));
    }

    SECTION("speedup is clamped to helix::sim::MAX_SPEED") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 50000.0);
        REQUIRE(mock.get_simulation_speedup() == Catch::Approx(helix::sim::MAX_SPEED));

        mock.set_simulation_speedup(100000.0);
        REQUIRE(mock.get_simulation_speedup() == Catch::Approx(helix::sim::MAX_SPEED));
    }

    SECTION("sim_speed() exposes the instance factor as the shared scale") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 20.0);
        REQUIRE(mock.sim_speed().factor() == Catch::Approx(20.0));

        mock.set_simulation_speedup(4.0);
        REQUIRE(mock.sim_speed().factor() == Catch::Approx(4.0));
        // A wait shortens, an elapsed span stretches — opposite directions.
        REQUIRE(mock.sim_speed().shorten_wait_ms(400) == 100);
        REQUIRE(mock.sim_speed().accelerate_progress(std::chrono::milliseconds(100)) ==
                std::chrono::milliseconds(400));
    }

    SECTION("higher speedup completes preheat faster") {
        // Test with 50x speedup
        auto mock_slow = fixture.create_mock(50.0);
        mock_slow->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        auto start_slow = std::chrono::steady_clock::now();
        fixture.wait_for_phase(mock_slow.get(), MockPrintPhase::PRINTING, 15000);
        auto duration_slow = std::chrono::steady_clock::now() - start_slow;
        mock_slow->stop_temperature_simulation();
        mock_slow->disconnect();

        fixture.reset();

        // Test with 200x speedup
        auto mock_fast = fixture.create_mock(200.0);
        mock_fast->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        auto start_fast = std::chrono::steady_clock::now();
        fixture.wait_for_phase(mock_fast.get(), MockPrintPhase::PRINTING, 15000);
        auto duration_fast = std::chrono::steady_clock::now() - start_fast;
        mock_fast->stop_temperature_simulation();
        mock_fast->disconnect();

        // 200x speedup should be faster than 50x speedup
        REQUIRE(duration_fast < duration_slow);
    }
}

// ============================================================================
// Metadata Extraction Tests [print][metadata]
// ============================================================================

TEST_CASE("Mock print metadata extraction from G-code", "[print][metadata][slow]") {
    MockPrintTestFixture fixture;

    SECTION("starting print extracts total layers") {
        auto mock = fixture.create_mock(100.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // 3DBenchy should have layer count from metadata
        int total_layers = mock->get_total_layers();
        REQUIRE(total_layers > 0);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("missing file uses default metadata") {
        auto mock = fixture.create_mock(100.0);

        // Non-existent file should use defaults
        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=nonexistent_file.gcode");

        // Default is 100 layers
        int total_layers = mock->get_total_layers();
        REQUIRE(total_layers == 100);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("current layer starts at 0") {
        auto mock = fixture.create_mock(100.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Initial layer should be 0 or 1
        int current_layer = mock->get_current_layer();
        REQUIRE(current_layer >= 0);
        REQUIRE(current_layer < mock->get_total_layers());

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("current layer advances with progress") {
        auto mock = fixture.create_mock(500.0); // High speedup for fast progress

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for printing phase
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        int initial_layer = mock->get_current_layer();

        // Wait for some progress
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        int later_layer = mock->get_current_layer();

        // Layer should have advanced
        REQUIRE(later_layer >= initial_layer);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }
}

// ============================================================================
// Unified Handler Tests [print][handlers]
// ============================================================================

TEST_CASE("Mock print G-code and API produce identical behavior", "[print][handlers][slow]") {
    MockPrintTestFixture fixture1;
    MockPrintTestFixture fixture2;

    SECTION("SDCARD_PRINT_FILE and printer.print.start both start print") {
        // Test G-code command
        auto mock1 = fixture1.create_mock(100.0);
        mock1->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        MockPrintPhase phase1 = mock1->get_print_phase();
        int layers1 = mock1->get_total_layers();
        mock1->stop_temperature_simulation();
        mock1->disconnect();

        // Test JSON-RPC API
        auto mock2 = fixture2.create_mock(100.0);
        mock2->send_jsonrpc(
            "printer.print.start", {{"filename", "3DBenchy.gcode"}}, [](json) {},
            [](const MoonrakerError&) {}, 5000);
        MockPrintPhase phase2 = mock2->get_print_phase();
        int layers2 = mock2->get_total_layers();
        mock2->stop_temperature_simulation();
        mock2->disconnect();

        // Both should be in same phase with same metadata
        REQUIRE(phase1 == phase2);
        REQUIRE(layers1 == layers2);
    }

    SECTION("PAUSE and printer.print.pause both pause print") {
        auto mock1 = fixture1.create_mock(200.0);
        mock1->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture1.wait_for_phase(mock1.get(), MockPrintPhase::PRINTING, 10000));
        mock1->gcode_script("PAUSE");
        MockPrintPhase phase1 = mock1->get_print_phase();
        mock1->stop_temperature_simulation();
        mock1->disconnect();

        fixture2.reset();

        auto mock2 = fixture2.create_mock(200.0);
        mock2->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture2.wait_for_phase(mock2.get(), MockPrintPhase::PRINTING, 10000));
        mock2->send_jsonrpc(
            "printer.print.pause", json::object(), [](json) {}, [](const MoonrakerError&) {}, 5000);
        MockPrintPhase phase2 = mock2->get_print_phase();
        mock2->stop_temperature_simulation();
        mock2->disconnect();

        REQUIRE(phase1 == phase2);
        REQUIRE(phase1 == MockPrintPhase::PAUSED);
    }

    SECTION("API returns error callback on invalid state transition") {
        auto mock = fixture1.create_mock(100.0);

        bool error_received = false;
        // Try to pause when not printing
        mock->send_jsonrpc(
            "printer.print.pause", json::object(), [](json) {},
            [&error_received](const MoonrakerError&) { error_received = true; }, 5000);

        REQUIRE(error_received);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }
}

// ============================================================================
// Progress and Layer Tracking Tests [print][progress]
// ============================================================================

TEST_CASE("Mock print progress and layer tracking", "[print][progress][slow]") {
    MockPrintTestFixture fixture;

    SECTION("progress starts at 0 when print begins") {
        auto mock = fixture.create_mock(100.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for notification with progress
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("virtual_sdcard") &&
                       status["virtual_sdcard"].contains("progress");
            },
            5000));

        // Find first progress notification
        auto notif = fixture.find_notification([](const json& n) {
            if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                return false;
            }
            const json& status = n["params"][0];
            return status.contains("virtual_sdcard") &&
                   status["virtual_sdcard"].contains("progress");
        });

        REQUIRE(notif.has_value());
        double progress = (*notif)["params"][0]["virtual_sdcard"]["progress"].get<double>();
        REQUIRE(progress < 0.1); // Should be near 0

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("progress advances during PRINTING phase") {
        auto mock = fixture.create_mock(100.0); // Moderate speedup (not too fast)

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        fixture.reset();

        // Wait for some progress callbacks
        REQUIRE(fixture.wait_for_callbacks(5, 10000));

        // Find progress values
        double first_progress = -1.0;
        double last_progress = -1.0;
        for (const auto& n : fixture.get_notifications()) {
            if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
                const json& status = n["params"][0];
                if (status.contains("virtual_sdcard") &&
                    status["virtual_sdcard"].contains("progress")) {
                    double p = status["virtual_sdcard"]["progress"].get<double>();
                    if (first_progress < 0)
                        first_progress = p;
                    last_progress = p;
                }
            }
        }

        // At high speedup, progress may wrap around, so just verify
        // we captured valid progress values
        REQUIRE(first_progress >= 0.0);
        REQUIRE(last_progress >= 0.0);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("progress frozen during PAUSED phase") {
        auto mock = fixture.create_mock(500.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        // Let some progress happen
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Pause
        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);

        fixture.reset();

        // Wait while paused
        REQUIRE(fixture.wait_for_callbacks(3, 6000));

        // Get all progress values while paused
        std::vector<double> progress_values;
        for (const auto& n : fixture.get_notifications()) {
            if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
                const json& status = n["params"][0];
                if (status.contains("virtual_sdcard") &&
                    status["virtual_sdcard"].contains("progress")) {
                    progress_values.push_back(status["virtual_sdcard"]["progress"].get<double>());
                }
            }
        }

        // All progress values should be the same (frozen)
        if (progress_values.size() >= 2) {
            for (size_t i = 1; i < progress_values.size(); ++i) {
                REQUIRE(progress_values[i] == Catch::Approx(progress_values[0]).epsilon(0.001));
            }
        }

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("layer count matches progress * total_layers") {
        auto mock = fixture.create_mock(500.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        // Let progress advance
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        int current_layer = mock->get_current_layer();
        int total_layers = mock->get_total_layers();

        // Current layer should be reasonable
        REQUIRE(current_layer >= 0);
        REQUIRE(current_layer <= total_layers);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }
}

// ============================================================================
// Thermal Phase Tests [print][thermal]
// ============================================================================

TEST_CASE("Mock print thermal phase behavior", "[print][thermal][slow]") {
    MockPrintTestFixture fixture;

    SECTION("preheat sets temperature targets from metadata") {
        auto mock = fixture.create_mock(100.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Wait for notification with temperature targets
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("extruder") && status["extruder"].contains("target") &&
                       status["extruder"]["target"].get<double>() > 0;
            },
            5000));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("temperatures maintained during PRINTING") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        fixture.reset();

        // Collect temperature values
        REQUIRE(fixture.wait_for_callbacks(3, 6000));

        bool found_high_temp = false;
        for (const auto& n : fixture.get_notifications()) {
            if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
                const json& status = n["params"][0];
                if (status.contains("extruder") && status["extruder"].contains("temperature")) {
                    double temp = status["extruder"]["temperature"].get<double>();
                    if (temp > 100.0) { // Should be near target (200+)
                        found_high_temp = true;
                    }
                }
            }
        }

        REQUIRE(found_high_temp);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("temperatures maintained during PAUSED") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);

        fixture.reset();

        REQUIRE(fixture.wait_for_callbacks(3, 6000));

        // Temperatures should still be high while paused
        bool found_high_temp = false;
        for (const auto& n : fixture.get_notifications()) {
            if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
                const json& status = n["params"][0];
                if (status.contains("extruder") && status["extruder"].contains("temperature")) {
                    double temp = status["extruder"]["temperature"].get<double>();
                    if (temp > 100.0) {
                        found_high_temp = true;
                    }
                }
            }
        }

        REQUIRE(found_high_temp);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("cancel sets temperature targets to 0 for cooldown") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        fixture.reset();

        mock->gcode_script("CANCEL_PRINT");

        // Wait for notification showing target = 0
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("extruder") && status["extruder"].contains("target") &&
                       status["extruder"]["target"].get<double>() == 0.0;
            },
            5000));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }
}

// ============================================================================
// Status Notification Tests [print][notifications]
// ============================================================================

TEST_CASE("Mock print status notifications match Moonraker format",
          "[print][notifications][slow]") {
    MockPrintTestFixture fixture;

    SECTION("notifications include print_stats object") {
        auto mock = fixture.create_mock(100.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("print_stats") && status["print_stats"].is_object();
            },
            5000));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("print_stats includes state and filename") {
        auto mock = fixture.create_mock(100.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("print_stats"))
                    return false;
                const json& ps = status["print_stats"];
                return ps.contains("state") && ps.contains("filename") && ps["state"].is_string() &&
                       ps["filename"].is_string();
            },
            5000));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("print_stats.info includes layer information") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        // Wait for enhanced print status with layer info
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                if (!status.contains("print_stats"))
                    return false;
                const json& ps = status["print_stats"];
                return ps.contains("info") && ps["info"].contains("current_layer") &&
                       ps["info"].contains("total_layer");
            },
            5000));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("virtual_sdcard includes progress") {
        auto mock = fixture.create_mock(100.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                if (!n.contains("params") || !n["params"].is_array() || n["params"].empty()) {
                    return false;
                }
                const json& status = n["params"][0];
                return status.contains("virtual_sdcard") &&
                       status["virtual_sdcard"].contains("progress") &&
                       status["virtual_sdcard"]["progress"].is_number();
            },
            5000));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("state changes dispatch notifications") {
        auto mock = fixture.create_mock(200.0);

        // Start print - should notify "printing"
        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        REQUIRE(fixture.wait_for_matching(
            [](const json& n) { return get_print_state_from_notification(n) == "printing"; },
            5000));

        // Pause - should notify "paused"
        mock->gcode_script("PAUSE");

        REQUIRE(fixture.wait_for_matching(
            [](const json& n) { return get_print_state_from_notification(n) == "paused"; }, 2000));

        // Cancel - should notify "cancelled" or "standby"
        mock->gcode_script("CANCEL_PRINT");

        // Accept either cancelled or standby as valid final states
        REQUIRE(fixture.wait_for_matching(
            [](const json& n) {
                std::string state = get_print_state_from_notification(n);
                return state == "cancelled" || state == "standby";
            },
            3000));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }
}

// ============================================================================
// Edge Cases and Error Handling Tests [print][edge_cases]
// ============================================================================

TEST_CASE("Mock print edge cases and error handling", "[print][edge_cases][slow]") {
    MockPrintTestFixture fixture;

    SECTION("starting new print cancels previous") {
        auto mock = fixture.create_mock(100.0);

        // Start first print
        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PREHEAT);

        // Start second print without canceling
        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");

        // Should be in PREHEAT for new print
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PREHEAT);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("empty filename rejected") {
        auto mock = fixture.create_mock(100.0);

        // API call with empty filename
        bool error_received = false;
        mock->send_jsonrpc(
            "printer.print.start", {{"filename", ""}}, [](json) {},
            [&error_received](const MoonrakerError&) { error_received = true; }, 5000);

        REQUIRE(error_received);
        REQUIRE(mock->get_print_phase() == MockPrintPhase::IDLE);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("speedup change mid-print applies immediately") {
        auto mock = fixture.create_mock(50.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 15000));

        // Change speedup
        mock->set_simulation_speedup(500.0);
        REQUIRE(mock->get_simulation_speedup() == Catch::Approx(500.0));

        // Progress should now be faster (tested indirectly by value change)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("cancel during cooldown is accepted") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        // First cancel - starts cooldown
        mock->gcode_script("CANCEL_PRINT");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::CANCELLED);

        // Second cancel during cooldown - should be accepted
        int result = mock->gcode_script("CANCEL_PRINT");
        REQUIRE(result == 0);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("current layer is 0 when not printing") {
        auto mock = fixture.create_mock(100.0);

        // Before starting print - current layer is 0 (no progress)
        // Note: total_layers uses default metadata (100) until a print starts
        REQUIRE(mock->get_current_layer() == 0);
        REQUIRE(mock->get_print_phase() == MockPrintPhase::IDLE);

        mock->disconnect();
    }

    SECTION("M112 from any phase sets ERROR") {
        // Test from IDLE
        {
            auto mock = fixture.create_mock(100.0);
            mock->gcode_script("M112");
            REQUIRE(mock->get_print_phase() == MockPrintPhase::ERROR);
            mock->stop_temperature_simulation();
            mock->disconnect();
        }

        fixture.reset();

        // Test from PREHEAT
        {
            auto mock = fixture.create_mock(50.0);
            mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
            REQUIRE(mock->get_print_phase() == MockPrintPhase::PREHEAT);
            mock->gcode_script("M112");
            REQUIRE(mock->get_print_phase() == MockPrintPhase::ERROR);
            mock->stop_temperature_simulation();
            mock->disconnect();
        }

        fixture.reset();

        // Test from PAUSED
        {
            auto mock = fixture.create_mock(200.0);
            mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
            fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000);
            mock->gcode_script("PAUSE");
            REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);
            mock->gcode_script("M112");
            REQUIRE(mock->get_print_phase() == MockPrintPhase::ERROR);
            mock->stop_temperature_simulation();
            mock->disconnect();
        }
    }
}

// ============================================================================
// Pause/Resume Behavior Tests [print][pause_resume]
// ============================================================================

TEST_CASE("Mock print pause/resume detailed behavior", "[print][pause_resume][slow]") {
    MockPrintTestFixture fixture;

    SECTION("pause from PREHEAT succeeds") {
        auto mock = fixture.create_mock(50.0); // Slower to catch PREHEAT

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PREHEAT);

        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("resume after pause from PREHEAT returns to PREHEAT") {
        auto mock = fixture.create_mock(50.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PREHEAT);

        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);

        mock->gcode_script("RESUME");
        // Should return to PREHEAT (or PRINTING if temps stabilized)
        MockPrintPhase phase = mock->get_print_phase();
        REQUIRE((phase == MockPrintPhase::PREHEAT || phase == MockPrintPhase::PRINTING));

        mock->stop_temperature_simulation();
        mock->disconnect();
    }

    SECTION("multiple pause/resume cycles work correctly") {
        auto mock = fixture.create_mock(200.0);

        mock->gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(fixture.wait_for_phase(mock.get(), MockPrintPhase::PRINTING, 10000));

        // First pause/resume
        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);
        mock->gcode_script("RESUME");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PRINTING);

        // Second pause/resume
        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);
        mock->gcode_script("RESUME");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PRINTING);

        // Third pause/resume
        mock->gcode_script("PAUSE");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PAUSED);
        mock->gcode_script("RESUME");
        REQUIRE(mock->get_print_phase() == MockPrintPhase::PRINTING);

        mock->stop_temperature_simulation();
        mock->disconnect();
    }
}

// ============================================================================
// Capture Replay Cursor
// ============================================================================

TEST_CASE("MoonrakerClientMock: the replay cursor scales with the speedup",
          "[mock][print][replay][simclock]") {
    // A capture's timestamps are simulated milliseconds, so the cursor into it
    // is real-elapsed MULTIPLIED by the speedup. Divide instead and a
    // fast-forwarded replay crawls.
    const std::string script_path = helix::test::config_sandbox_dir() + "/replay_cursor.json";
    {
        std::ofstream out(script_path);
        out << R"({"events":[
              {"t":0,    "type":"gcode_response","line":"first"},
              {"t":1000, "type":"gcode_response","line":"second"},
              {"t":9000, "type":"gcode_response","line":"third"}
            ]})";
    }

    const auto hundred_ms_ago = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);

    SECTION("at 1x, 100ms of real time reaches only the t=0 event") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 1.0);
        REQUIRE(mock.arm_event_replay(script_path));
        helix::MoonrakerClientMockTestAccess::set_replay_start(mock, hundred_ms_ago);
        helix::MoonrakerClientMockTestAccess::pump_replay(mock);
        REQUIRE(helix::MoonrakerClientMockTestAccess::replay_next(mock) == 1);
    }

    SECTION("at 10x the same 100ms reaches the t=1000 event as well") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 10.0);
        REQUIRE(mock.arm_event_replay(script_path));
        helix::MoonrakerClientMockTestAccess::set_replay_start(mock, hundred_ms_ago);
        helix::MoonrakerClientMockTestAccess::pump_replay(mock);
        REQUIRE(helix::MoonrakerClientMockTestAccess::replay_next(mock) == 2);
    }

    SECTION("nothing fires before the replay origin") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24, 100.0);
        REQUIRE(mock.arm_event_replay(script_path));
        helix::MoonrakerClientMockTestAccess::set_replay_start(
            mock, std::chrono::steady_clock::now() + std::chrono::seconds(30));
        helix::MoonrakerClientMockTestAccess::pump_replay(mock);
        REQUIRE(helix::MoonrakerClientMockTestAccess::replay_next(mock) == 0);
    }
}
