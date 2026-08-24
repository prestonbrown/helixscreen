// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_input_shaper.cpp
 * @brief Unit tests for MoonrakerAPI input shaper calibration methods
 *
 * Tests the InputShaperCollector pattern and API methods:
 * - start_resonance_test() - SHAPER_CALIBRATE command execution
 * - set_input_shaper() - SET_INPUT_SHAPER command execution
 * - Response parsing for calibration results
 * - Error handling for missing accelerometer
 *
 * Uses mock client to simulate G-code responses from Klipper.
 */

#include "../../include/calibration_types.h" // For InputShaperResult
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../ui_test_utils.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <regex>
#include <thread>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Global LVGL Initialization (called once)
// ============================================================================

namespace {
struct LVGLInitializerInputShaper {
    LVGLInitializerInputShaper() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerInputShaper lvgl_init;

/// RAII spdlog capture, so watchdog warnings can be asserted on.
class LogCapture {
  public:
    LogCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256)) {
        logger_ = spdlog::default_logger();
        prev_level_ = logger_->level();
        sink_->set_level(spdlog::level::trace);
        logger_->sinks().push_back(sink_);
        logger_->set_level(spdlog::level::trace);
    }

    ~LogCapture() {
        auto& sinks = logger_->sinks();
        for (auto it = sinks.begin(); it != sinks.end(); ++it) {
            if (*it == sink_) {
                sinks.erase(it);
                break;
            }
        }
        logger_->set_level(prev_level_);
    }

    [[nodiscard]] int count_containing(const std::string& needle) const {
        int n = 0;
        for (const auto& l : sink_->last_formatted(256)) {
            if (l.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }

  private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> logger_;
    spdlog::level::level_enum prev_level_;
};
} // namespace

// ============================================================================
// Test Fixture
// ============================================================================

/**
 * @brief Test fixture for input shaper API testing with mock client
 */
class InputShaperTestFixture {
  public:
    InputShaperTestFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false); // Don't register XML bindings in tests
        // execute_gcode() halted gate would otherwise reject every command.
        state_.set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
        reset_callbacks();
    }

    ~InputShaperTestFixture() {
        api_.reset();
    }

    void reset_callbacks() {
        result_received_ = false;
        error_received_ = false;
        captured_result_ = InputShaperResult{};
        captured_error_.clear();
    }

    // Callback for successful calibration result
    void on_result(const InputShaperResult& result) {
        result_received_ = true;
        captured_result_ = result;
    }

    // Callback for errors
    void on_error(const MoonrakerError& err) {
        error_received_ = true;
        captured_error_ = err.message;
    }

  protected:
    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;

    std::atomic<bool> result_received_{false};
    std::atomic<bool> error_received_{false};
    InputShaperResult captured_result_;
    std::string captured_error_;
};

// ============================================================================
// start_resonance_test() Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperTestFixture, "start_resonance_test accepts X axis",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {}, // progress callback
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

    // Wait for async callback (mock dispatches synchronously)
    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    REQUIRE(captured_result.axis == 'X');
    REQUIRE(captured_result.is_valid());
    REQUIRE(captured_result.shaper_type == "mzv");
    REQUIRE(captured_result.shaper_freq == Catch::Approx(53.8f).margin(0.1f));
}

TEST_CASE_METHOD(InputShaperTestFixture, "start_resonance_test accepts Y axis",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'Y', [](int, ShaperCalibrationPhase) {}, // progress callback
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

    // Wait for async callback
    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    REQUIRE(captured_result.axis == 'Y');
    REQUIRE(captured_result.is_valid());
}

TEST_CASE_METHOD(InputShaperTestFixture, "start_resonance_test sends correct G-code command for X",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {},
        [&](const InputShaperResult& result) {
            complete_called = true;
            // Verify parsed values from mock response
            REQUIRE(result.shaper_type == "mzv");
            REQUIRE(result.shaper_freq == Catch::Approx(53.8f).margin(0.1f));
        },
        [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

    // Wait for async callback
    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "start_resonance_test flags chart data unavailable when CSV unreadable",
                 "[calibration][input_shaper]") {
    // Simulate Klipper reporting a CSV path whose file the client cannot read
    // (e.g. systemd PrivateTmp isolating HelixScreen from Klipper's /tmp).
    mock_client_.set_shaper_csv_writable(false);

    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {},
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    // Recommendation is still valid — it comes from console output, not the CSV.
    REQUIRE(captured_result.is_valid());
    REQUIRE(captured_result.shaper_type == "mzv");
    // Klipper reported a path, so csv_path is populated...
    REQUIRE_FALSE(captured_result.csv_path.empty());
    // ...but no chart data could be read, and that must be flagged (not silent).
    REQUIRE(captured_result.freq_response.empty());
    REQUIRE(captured_result.chart_data_unavailable);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "start_resonance_test does not flag chart unavailable when CSV is readable",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {},
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    REQUIRE(captured_result.has_freq_data());
    REQUIRE_FALSE(captured_result.chart_data_unavailable);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "start_resonance_test flags the firmware X-overwrite on the Y result",
                 "[calibration][input_shaper]") {
    // Creality's klippy fork overwrites the staged X result with Y's values at
    // the end of a Y-axis run and announces it with a line starting
    // "copy_TestAxis_y_to_x Recommended shaper_type_x = ...". The collector must
    // (a) flag it on the Y result and (b) NOT let the embedded "Recommended
    // shaper_type_x" wording clobber the Y axis's own recommendation.
    InputShaperResult captured_result;

    SECTION("without the marker line the flag stays clear") {
        api_->advanced().start_resonance_test(
            'Y', [](int, ShaperCalibrationPhase) {},
            [&](const InputShaperResult& result) { captured_result = result; },
            [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        for (int i = 0;
             i < 200 && !(captured_result.is_valid() && !captured_result.csv_path.empty()); ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        REQUIRE(captured_result.is_valid());
        REQUIRE_FALSE(captured_result.x_overwritten_by_firmware);
    }

    SECTION("the marker line flags the result and spares the Y recommendation") {
        api_->advanced().start_resonance_test(
            'Y', [](int, ShaperCalibrationPhase) {},
            [&](const InputShaperResult& result) { captured_result = result; },
            [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        // The collector registers its gcode-response handler synchronously, so
        // the marker can be injected before the mock's first transcript tick.
        mock_client_.dispatch_gcode_response(
            "copy_TestAxis_y_to_x Recommended shaper_type_x = ei, shaper_freq_x = 71.4 Hz");

        for (int i = 0;
             i < 200 && !(captured_result.is_valid() && !captured_result.csv_path.empty()); ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        REQUIRE(captured_result.is_valid());
        REQUIRE(captured_result.x_overwritten_by_firmware);
        // The marker's embedded "shaper_type_x = ei" must not have been parsed
        // as this axis's recommendation (the mock's Y run recommends mzv).
        REQUIRE(captured_result.shaper_type == "mzv");
        REQUIRE(captured_result.shaper_freq == Catch::Approx(53.8f).margin(0.1f));
    }
}

// ============================================================================
// set_input_shaper() Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperTestFixture, "set_input_shaper sends command for X axis with mzv",
                 "[calibration][input_shaper]") {
    std::atomic<bool> success_called{false};

    api_->advanced().set_input_shaper(
        'X', "mzv", 36.7, [&]() { success_called = true; },
        [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

    // Wait for async callback
    for (int i = 0; i < 200 && !success_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(success_called);
}

TEST_CASE_METHOD(InputShaperTestFixture, "set_input_shaper sends command for Y axis",
                 "[calibration][input_shaper]") {
    std::atomic<bool> success_called{false};

    api_->advanced().set_input_shaper(
        'Y', "ei", 47.6, [&]() { success_called = true; },
        [&](const MoonrakerError&) { FAIL("Error callback should not be called"); });

    // Wait for async callback
    for (int i = 0; i < 200 && !success_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(success_called);
}

TEST_CASE_METHOD(InputShaperTestFixture, "set_input_shaper accepts all valid shaper types",
                 "[calibration][input_shaper]") {
    std::vector<std::string> shaper_types = {"zv", "mzv", "zvd", "ei", "2hump_ei", "3hump_ei"};

    for (const auto& type : shaper_types) {
        INFO("Testing shaper type: " << type);
        std::atomic<bool> success_called{false};

        api_->advanced().set_input_shaper(
            'X', type, 35.0, [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                FAIL("Error callback should not be called for type: " << type << " - "
                                                                      << err.message);
            });

        // Wait for async callback
        for (int i = 0; i < 200 && !success_called; ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        REQUIRE(success_called);
    }
}

// ============================================================================
// InputShaperResult Parsing Tests
// ============================================================================

TEST_CASE("InputShaperResult default construction", "[calibration]") {
    InputShaperResult result;

    // Default axis is 'X' per struct definition
    REQUIRE(result.axis == 'X');
    REQUIRE(result.shaper_type.empty());
    REQUIRE(result.shaper_freq == 0.0f);
    REQUIRE(result.max_accel == 0.0f);
    REQUIRE(result.smoothing == 0.0f);
    REQUIRE(result.vibrations == 0.0f);
    REQUIRE(result.freq_response.empty());
}

TEST_CASE("InputShaperResult is_valid check", "[calibration]") {
    InputShaperResult result;

    // Empty result is not valid
    REQUIRE_FALSE(result.is_valid());

    // Set valid values
    result.shaper_type = "mzv";
    result.shaper_freq = 36.7f;

    REQUIRE(result.is_valid());
}

// ============================================================================
// Response Parsing Simulation Tests
// ============================================================================

TEST_CASE("InputShaperResult can store calibration data", "[calibration]") {
    // Simulate building a result from parsed G-code responses
    InputShaperResult result;
    result.axis = 'X';
    result.shaper_type = "mzv";
    result.shaper_freq = 36.7f;
    result.max_accel = 5000.0f;
    result.smoothing = 0.140f;
    result.vibrations = 7.2f;

    // Add frequency response data points
    result.freq_response.push_back({10.0f, 0.1f});
    result.freq_response.push_back({20.0f, 0.3f});
    result.freq_response.push_back({36.7f, 1.0f}); // Peak at resonance
    result.freq_response.push_back({50.0f, 0.2f});

    // Verify the result
    REQUIRE(result.axis == 'X');
    REQUIRE(result.is_valid());
    REQUIRE(result.shaper_type == "mzv");
    REQUIRE(result.shaper_freq == Catch::Approx(36.7f));
    REQUIRE(result.max_accel == Catch::Approx(5000.0f));
    REQUIRE(result.vibrations == Catch::Approx(7.2f));
    REQUIRE(result.freq_response.size() == 4);
}

TEST_CASE("InputShaperResult can represent incomplete state", "[calibration]") {
    InputShaperResult result;
    result.axis = 'Y';
    // Leave shaper_type empty to simulate error/incomplete

    REQUIRE_FALSE(result.is_valid());
    REQUIRE(result.shaper_type.empty());
}

// ============================================================================
// Shaper Type Validation Tests
// ============================================================================

TEST_CASE("Valid shaper type strings", "[calibration][validation]") {
    // These are the official Klipper input shaper types
    std::vector<std::string> valid_types = {
        "zv",       // Zero Vibration
        "mzv",      // Modified Zero Vibration
        "zvd",      // ZV + Derivative
        "ei",       // Extra Insensitive
        "2hump_ei", // 2-hump EI
        "3hump_ei"  // 3-hump EI
    };

    // Verify these are recognized as valid types
    for (const auto& type : valid_types) {
        INFO("Checking valid shaper type: " << type);
        // Just verify the strings are what we expect from Klipper
        REQUIRE(type.length() > 0);
        REQUIRE(type.length() <= 10);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(InputShaperTestFixture, "API handles null callbacks gracefully",
                 "[calibration][edge_case][input_shaper]") {
    // Test that calling start_resonance_test with nullptr callbacks doesn't crash
    // Note: The InputShaperCollector handles null callbacks internally
    REQUIRE_NOTHROW(api_->advanced().start_resonance_test('X', nullptr, nullptr, nullptr));

    // Pump LVGL timers to let the timer-based mock dispatch complete
    for (int i = 0; i < 50; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // set_input_shaper requires valid callbacks (by design), so we test with valid ones
    std::atomic<bool> success_called{false};
    REQUIRE_NOTHROW(api_->advanced().set_input_shaper(
        'X', "mzv", 36.7, [&]() { success_called = true; }, nullptr));

    // Wait for async callback
    for (int i = 0; i < 200 && !success_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(success_called);
}

// ============================================================================
// Phase 1: New API Methods and Enhanced Results
// ============================================================================
//
// These tests are written BEFORE implementation (test-first methodology).
// They will FAIL to compile or link until the corresponding types and methods
// are implemented in:
//   - include/calibration_types.h (ShaperOption, InputShaperConfig)
//   - include/moonraker_api.h (measure_axes_noise, get_input_shaper_config)
//   - src/api/moonraker_api_advanced.cpp (NoiseCheckCollector, enhanced collector)
//   - src/api/moonraker_client_mock.cpp (mock implementations)
// ============================================================================

// ----------------------------------------------------------------------------
// New Types Tests
// ----------------------------------------------------------------------------

TEST_CASE("ShaperOption struct", "[input_shaper][types]") {
    SECTION("default construction") {
        ShaperOption option;

        // Default values should be zeroed/empty
        CHECK(option.type.empty());
        CHECK(option.frequency == 0.0f);
        CHECK(option.vibrations == 0.0f);
        CHECK(option.smoothing == 0.0f);
        CHECK(option.max_accel == 0.0f);
    }

    SECTION("can store fitted shaper data") {
        ShaperOption option;
        option.type = "mzv";
        option.frequency = 36.7f;
        option.vibrations = 7.2f;
        option.smoothing = 0.140f;
        option.max_accel = 5000.0f;

        REQUIRE(option.type == "mzv");
        REQUIRE(option.frequency == Catch::Approx(36.7f));
        REQUIRE(option.vibrations == Catch::Approx(7.2f));
        REQUIRE(option.smoothing == Catch::Approx(0.140f));
        REQUIRE(option.max_accel == Catch::Approx(5000.0f));
    }
}

TEST_CASE("InputShaperConfig struct", "[input_shaper][types]") {
    SECTION("default construction") {
        InputShaperConfig config;

        // Default should indicate unconfigured state
        CHECK(config.shaper_type_x.empty());
        CHECK(config.shaper_freq_x == 0.0f);
        CHECK(config.shaper_type_y.empty());
        CHECK(config.shaper_freq_y == 0.0f);
        CHECK(config.damping_ratio_x == 0.0f);
        CHECK(config.damping_ratio_y == 0.0f);
        CHECK_FALSE(config.is_configured);
    }

    SECTION("can store configured shaper settings") {
        InputShaperConfig config;
        config.shaper_type_x = "mzv";
        config.shaper_freq_x = 36.7f;
        config.shaper_type_y = "ei";
        config.shaper_freq_y = 47.6f;
        config.damping_ratio_x = 0.1f;
        config.damping_ratio_y = 0.1f;
        config.is_configured = true;

        REQUIRE(config.is_configured);
        REQUIRE(config.shaper_type_x == "mzv");
        REQUIRE(config.shaper_freq_x == Catch::Approx(36.7f));
        REQUIRE(config.shaper_type_y == "ei");
        REQUIRE(config.shaper_freq_y == Catch::Approx(47.6f));
    }
}

// ----------------------------------------------------------------------------
// Enhanced InputShaperResult Tests (all_shapers vector)
// ----------------------------------------------------------------------------

TEST_CASE("InputShaperResult has all_shapers vector", "[input_shaper][types]") {
    InputShaperResult result;

    // The all_shapers vector should exist and be empty by default
    REQUIRE(result.all_shapers.empty());

    // Should be able to add shaper options
    ShaperOption zv;
    zv.type = "zv";
    zv.frequency = 35.8f;
    zv.vibrations = 22.7f;
    zv.smoothing = 0.100f;

    ShaperOption mzv;
    mzv.type = "mzv";
    mzv.frequency = 36.7f;
    mzv.vibrations = 7.2f;
    mzv.smoothing = 0.140f;

    result.all_shapers.push_back(zv);
    result.all_shapers.push_back(mzv);

    REQUIRE(result.all_shapers.size() == 2);
    REQUIRE(result.all_shapers[0].type == "zv");
    REQUIRE(result.all_shapers[1].type == "mzv");
}

TEST_CASE_METHOD(InputShaperTestFixture, "start_resonance_test returns all shaper alternatives",
                 "[calibration][input_shaper]") {
    // The mock dispatches 3 shaper options: zv, mzv, ei
    // After enhancement, the result should contain ALL fitted shapers in all_shapers
    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {}, // progress callback
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError& err) {
            FAIL("Error callback should not be called: " << err.message);
        });

    // Wait for async callback
    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    REQUIRE(captured_result.is_valid());

    // Mock now outputs 5 fitted shapers with realistic values from AD5M
    REQUIRE(captured_result.all_shapers.size() == 5);

    // Verify the mock data matches expected values
    // Mock outputs: zv@59.0, mzv@53.8, ei@56.2, 2hump_ei@71.8, 3hump_ei@89.6

    auto find_shaper = [&](const std::string& type) -> const ShaperOption* {
        for (const auto& s : captured_result.all_shapers) {
            if (s.type == type)
                return &s;
        }
        return nullptr;
    };

    const ShaperOption* zv = find_shaper("zv");
    REQUIRE(zv != nullptr);
    CHECK(zv->frequency == Catch::Approx(59.0f).margin(0.1f));
    CHECK(zv->vibrations == Catch::Approx(5.2f).margin(0.1f));
    CHECK(zv->smoothing == Catch::Approx(0.045f).margin(0.01f));
    CHECK(zv->max_accel == Catch::Approx(13400.0f).margin(1.0f));

    const ShaperOption* mzv = find_shaper("mzv");
    REQUIRE(mzv != nullptr);
    CHECK(mzv->frequency == Catch::Approx(53.8f).margin(0.1f));
    CHECK(mzv->vibrations == Catch::Approx(1.6f).margin(0.1f));
    CHECK(mzv->smoothing == Catch::Approx(0.130f).margin(0.01f));
    CHECK(mzv->max_accel == Catch::Approx(4000.0f).margin(1.0f));

    const ShaperOption* ei = find_shaper("ei");
    REQUIRE(ei != nullptr);
    CHECK(ei->frequency == Catch::Approx(56.2f).margin(0.1f));
    CHECK(ei->vibrations == Catch::Approx(0.7f).margin(0.1f));
    CHECK(ei->smoothing == Catch::Approx(0.120f).margin(0.01f));
    CHECK(ei->max_accel == Catch::Approx(4600.0f).margin(1.0f));

    const ShaperOption* two_hump = find_shaper("2hump_ei");
    REQUIRE(two_hump != nullptr);
    CHECK(two_hump->frequency == Catch::Approx(71.8f).margin(0.1f));
    CHECK(two_hump->max_accel == Catch::Approx(8800.0f).margin(1.0f));

    const ShaperOption* three_hump = find_shaper("3hump_ei");
    REQUIRE(three_hump != nullptr);
    CHECK(three_hump->frequency == Catch::Approx(89.6f).margin(0.1f));
    CHECK(three_hump->max_accel == Catch::Approx(8800.0f).margin(1.0f));
}

// ----------------------------------------------------------------------------
// measure_axes_noise() Tests
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(InputShaperTestFixture, "measure_axes_noise returns noise level",
                 "[calibration][input_shaper]") {
    // measure_axes_noise() runs MEASURE_AXES_NOISE G-code
    // Klipper output: "Axes noise for xy-axis accelerometer: 12.3 (x), 15.7 (y), 8.2 (z)"
    // Returns max(x, y) as overall noise level

    std::atomic<bool> complete_called{false};
    float captured_noise = -1.0f;

    api_->advanced().measure_axes_noise(
        [&](float noise_level) {
            captured_noise = noise_level;
            complete_called = true;
        },
        [&](const MoonrakerError& err) {
            FAIL("Error callback should not be called: " << err.message);
        });

    // Wait for async callback
    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    // Mock dispatches: x=12.345678, y=15.678901, z=8.234567
    // Collector returns max(x, y) = 15.678901
    CHECK(captured_noise == Catch::Approx(15.678901f).margin(0.01f));
}

TEST_CASE_METHOD(InputShaperTestFixture, "measure_axes_noise handles no accelerometer error",
                 "[calibration][input_shaper]") {
    // When no ADXL is configured, MEASURE_AXES_NOISE should fail
    // This test requires the mock to be configured to simulate missing accelerometer
    // For now, we test the error callback path exists

    // Configure mock to simulate no accelerometer (implementation detail)
    mock_client_.set_accelerometer_available(false);

    std::atomic<bool> error_called{false};
    std::string captured_error;

    api_->advanced().measure_axes_noise(
        [&](float) { FAIL("Success callback should not be called when accelerometer missing"); },
        [&](const MoonrakerError& err) {
            error_called = true;
            captured_error = err.message;
        });

    // Wait for async callback
    for (int i = 0; i < 200 && !error_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(error_called);
    // Error message should mention accelerometer/ADXL
    CHECK((captured_error.find("accelerometer") != std::string::npos ||
           captured_error.find("ADXL") != std::string::npos ||
           captured_error.find("adxl") != std::string::npos));
}

// ----------------------------------------------------------------------------
// get_input_shaper_config() Tests
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(InputShaperTestFixture, "get_input_shaper_config returns current settings",
                 "[calibration][input_shaper]") {
    // get_input_shaper_config() queries the current input shaper configuration
    // from printer state (via Moonraker's printer.objects.query)

    std::atomic<bool> complete_called{false};
    InputShaperConfig captured_config;

    api_->advanced().get_input_shaper_config(
        [&](const InputShaperConfig& config) {
            captured_config = config;
            complete_called = true;
        },
        [&](const MoonrakerError& err) {
            FAIL("Error callback should not be called: " << err.message);
        });

    // Wait for async callback
    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);

    // Mock returns config from configfile.config.input_shaper (string values)
    // Expected: mzv@36.7Hz for X, ei@47.6Hz for Y
    REQUIRE(captured_config.is_configured);
    CHECK(captured_config.shaper_type_x == "mzv");
    CHECK(captured_config.shaper_freq_x == Catch::Approx(36.7f).margin(0.1f));
    CHECK(captured_config.shaper_type_y == "ei");
    CHECK(captured_config.shaper_freq_y == Catch::Approx(47.6f).margin(0.1f));
}

TEST_CASE_METHOD(InputShaperTestFixture, "get_input_shaper_config handles unconfigured shaper",
                 "[calibration][input_shaper]") {
    // When no input shaper is configured, is_configured should be false
    // This requires mock to simulate unconfigured state

    // Configure mock to simulate unconfigured input shaper
    mock_client_.set_input_shaper_configured(false);

    std::atomic<bool> complete_called{false};
    InputShaperConfig captured_config;

    api_->advanced().get_input_shaper_config(
        [&](const InputShaperConfig& config) {
            captured_config = config;
            complete_called = true;
        },
        [&](const MoonrakerError& err) {
            FAIL("Error callback should not be called: " << err.message);
        });

    // Wait for async callback
    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    REQUIRE_FALSE(captured_config.is_configured);
}

// ============================================================================
// Enhanced Collector: New Recommendation Format + Max Accel + CSV Path
// ============================================================================

TEST_CASE_METHOD(InputShaperTestFixture, "collector parses new Klipper recommendation format",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {},
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError& err) {
            FAIL("Error callback should not be called: " << err.message);
        });

    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    // New mock uses "Recommended shaper_type_x = mzv, shaper_freq_x = 53.8 Hz" format
    CHECK(captured_result.shaper_type == "mzv");
    CHECK(captured_result.shaper_freq == Catch::Approx(53.8f).margin(0.1f));
}

TEST_CASE_METHOD(InputShaperTestFixture, "collector parses max_accel per shaper",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {},
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError& err) { FAIL("Error: " << err.message); });

    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    REQUIRE(captured_result.all_shapers.size() == 5);

    // Verify max_accel is parsed for each shaper
    auto find_shaper = [&](const std::string& type) -> const ShaperOption* {
        for (const auto& s : captured_result.all_shapers) {
            if (s.type == type)
                return &s;
        }
        return nullptr;
    };

    const ShaperOption* zv = find_shaper("zv");
    REQUIRE(zv != nullptr);
    CHECK(zv->max_accel == Catch::Approx(13400.0f).margin(1.0f));

    const ShaperOption* mzv = find_shaper("mzv");
    REQUIRE(mzv != nullptr);
    CHECK(mzv->max_accel == Catch::Approx(4000.0f).margin(1.0f));

    const ShaperOption* ei = find_shaper("ei");
    REQUIRE(ei != nullptr);
    CHECK(ei->max_accel == Catch::Approx(4600.0f).margin(1.0f));

    // Recommended shaper's max_accel should be on the result itself
    CHECK(captured_result.max_accel == Catch::Approx(4000.0f).margin(1.0f));
}

TEST_CASE_METHOD(InputShaperTestFixture, "collector captures CSV path",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {},
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError& err) { FAIL("Error: " << err.message); });

    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    CHECK(captured_result.csv_path == "/tmp/calibration_data_x_mock.csv");
}

TEST_CASE_METHOD(InputShaperTestFixture, "collector emits progress callbacks during sweep",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};
    std::vector<std::pair<int, ShaperCalibrationPhase>> reports;

    api_->advanced().start_resonance_test(
        'X',
        [&](int percent, ShaperCalibrationPhase phase) { reports.emplace_back(percent, phase); },
        [&](const InputShaperResult&) { complete_called = true; },
        [&](const MoonrakerError& err) { FAIL("Error: " << err.message); });

    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    // The sweep owns the whole bar now, so the mock's 20 sweep lines each
    // report; the analysis transition and the completion are the only others.
    CHECK(reports.size() > 20);

    // Sweep reports are monotonic and reach the top of the bar.
    std::vector<int> sweep_values;
    for (const auto& [percent, phase] : reports) {
        if (phase == ShaperCalibrationPhase::Sweeping) {
            sweep_values.push_back(percent);
        }
    }
    REQUIRE(sweep_values.size() >= 20);
    for (size_t i = 1; i < sweep_values.size(); ++i) {
        CHECK(sweep_values[i] >= sweep_values[i - 1]);
    }
    CHECK(sweep_values.back() == 100);

    // Should start low (sweep) and end at 100
    CHECK(reports.front().first < 10); // First sweep line sits at the bottom of the bar
    CHECK(reports.back().first == 100);
    CHECK(reports.back().second == ShaperCalibrationPhase::Complete);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "collector returns 5 shaper alternatives with updated mock",
                 "[calibration][input_shaper]") {
    std::atomic<bool> complete_called{false};
    InputShaperResult captured_result;

    api_->advanced().start_resonance_test(
        'Y', [](int, ShaperCalibrationPhase) {},
        [&](const InputShaperResult& result) {
            captured_result = result;
            complete_called = true;
        },
        [&](const MoonrakerError& err) { FAIL("Error: " << err.message); });

    for (int i = 0; i < 200 && !complete_called; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(complete_called);
    REQUIRE(captured_result.axis == 'Y');
    REQUIRE(captured_result.all_shapers.size() == 5);

    // Verify all 5 shaper types from the updated mock
    std::vector<std::string> expected_types = {"zv", "mzv", "ei", "2hump_ei", "3hump_ei"};
    for (size_t i = 0; i < expected_types.size(); ++i) {
        CHECK(captured_result.all_shapers[i].type == expected_types[i]);
        CHECK(captured_result.all_shapers[i].frequency > 0.0f);
        CHECK(captured_result.all_shapers[i].max_accel > 0.0f);
    }

    // Y axis CSV path should use 'y'
    CHECK(captured_result.csv_path == "/tmp/calibration_data_y_mock.csv");
}

// ============================================================================
// Kalico Smooth Shaper Parsing Tests
// ============================================================================

// These test the regex patterns used by InputShaperCollector::parse_shaper_line()
// and parse_recommendation(). The collector is a private class in
// moonraker_advanced_api.cpp, so we test the regexes directly.

TEST_CASE("Kalico smoother regex parses bleeding-edge format", "[input_shaper][kalico]") {
    // Same regex as InputShaperCollector::parse_shaper_line() kalico_regex
    static const std::regex kalico_regex(
        R"(Fitted (?:shaper|smoother) '([\w]+)' frequency = ([\d.]+) Hz \(vibration score = ([\d.]+)%, smoothing ~= ([\d.]+))");
    // Standard Klipper regex
    static const std::regex klipper_regex(
        R"(Fitted shaper '(\w+)' frequency = ([\d.]+) Hz \(vibrations = ([\d.]+)%, smoothing ~= ([\d.]+)\))");

    SECTION("Kalico smoother line is parsed by kalico_regex") {
        std::string line =
            "Fitted smoother 'smooth_mzv' frequency = 42.6 Hz "
            "(vibration score = 1.23%, smoothing ~= 0.085, combined score = 1.234e-02)";

        std::smatch match;
        REQUIRE(std::regex_search(line, match, kalico_regex));
        CHECK(match[1].str() == "smooth_mzv");
        CHECK(match[2].str() == "42.6");
        CHECK(match[3].str() == "1.23");
        CHECK(match[4].str() == "0.085");

        // Should NOT match klipper_regex (uses "vibration score" not "vibrations")
        CHECK_FALSE(std::regex_search(line, match, klipper_regex));
    }

    SECTION("Kalico discrete shaper line is parsed by kalico_regex") {
        std::string line =
            "Fitted shaper 'mzv' frequency = 36.7 Hz "
            "(vibration score = 1.23%, smoothing ~= 0.140, combined score = 2.345e-02)";

        std::smatch match;
        REQUIRE(std::regex_search(line, match, kalico_regex));
        CHECK(match[1].str() == "mzv");
        CHECK(match[2].str() == "36.7");
        CHECK(match[3].str() == "1.23");
        CHECK(match[4].str() == "0.140");

        // Should NOT match klipper_regex either (uses "vibration score")
        CHECK_FALSE(std::regex_search(line, match, klipper_regex));
    }

    SECTION("Standard Klipper line is NOT matched by kalico_regex") {
        std::string line = "Fitted shaper 'mzv' frequency = 36.7 Hz "
                           "(vibrations = 7.2%, smoothing ~= 0.140)";

        std::smatch match;
        CHECK_FALSE(std::regex_search(line, match, kalico_regex));

        // Should match klipper_regex
        REQUIRE(std::regex_search(line, match, klipper_regex));
        CHECK(match[1].str() == "mzv");
        CHECK(match[2].str() == "36.7");
        CHECK(match[3].str() == "7.2");
        CHECK(match[4].str() == "0.140");
    }

    SECTION("All Kalico smooth shaper types parse correctly") {
        std::vector<std::pair<std::string, std::string>> smoother_lines = {
            {"smooth_zv", "Fitted smoother 'smooth_zv' frequency = 60.2 Hz (vibration score = "
                          "4.80%, smoothing ~= 0.040, combined score = 1.0e-02)"},
            {"smooth_mzv", "Fitted smoother 'smooth_mzv' frequency = 54.4 Hz (vibration score = "
                           "1.20%, smoothing ~= 0.085, combined score = 2.0e-02)"},
            {"smooth_ei", "Fitted smoother 'smooth_ei' frequency = 57.0 Hz (vibration score = "
                          "0.50%, smoothing ~= 0.095, combined score = 3.0e-02)"},
            {"smooth_2hump_ei", "Fitted smoother 'smooth_2hump_ei' frequency = 72.4 Hz (vibration "
                                "score = 0.00%, smoothing ~= 0.065, combined score = 4.0e-02)"},
            {"smooth_zvd_ei", "Fitted smoother 'smooth_zvd_ei' frequency = 68.0 Hz (vibration "
                              "score = 0.10%, smoothing ~= 0.070, combined score = 5.0e-02)"},
            {"smooth_si", "Fitted smoother 'smooth_si' frequency = 52.0 Hz (vibration score = "
                          "0.00%, smoothing ~= 0.110, combined score = 6.0e-02)"},
        };

        for (const auto& [expected_type, line] : smoother_lines) {
            INFO("Testing: " << expected_type);
            std::smatch match;
            REQUIRE(std::regex_search(line, match, kalico_regex));
            CHECK(match[1].str() == expected_type);
        }
    }
}

TEST_CASE("Kalico recommendation regex parses smoother format", "[input_shaper][kalico]") {
    // Same regexes as InputShaperCollector::parse_recommendation()
    static const std::regex rec_new(
        R"(Recommended shaper_type_\w+ = (\w+), shaper_freq_\w+ = ([\d.]+) Hz)");
    static const std::regex rec_smoother(
        R"(Recommended smoother_type_\w+ = (\w+), smoother_freq_\w+ = ([\d.]+) Hz)");
    static const std::regex rec_old(R"(Recommended shaper is (\w+) @ ([\d.]+) Hz)");

    SECTION("Kalico smoother recommendation") {
        std::string line = "Recommended smoother_type_x = smooth_mzv, smoother_freq_x = 42.6 Hz";

        std::smatch match;
        CHECK_FALSE(std::regex_search(line, match, rec_new));
        REQUIRE(std::regex_search(line, match, rec_smoother));
        CHECK(match[1].str() == "smooth_mzv");
        CHECK(match[2].str() == "42.6");
    }

    SECTION("Standard Klipper recommendation (new format)") {
        std::string line = "Recommended shaper_type_x = mzv, shaper_freq_x = 53.8 Hz";

        std::smatch match;
        REQUIRE(std::regex_search(line, match, rec_new));
        CHECK(match[1].str() == "mzv");
        CHECK(match[2].str() == "53.8");

        // Should NOT match smoother regex
        CHECK_FALSE(std::regex_search(line, match, rec_smoother));
    }

    SECTION("Legacy Klipper recommendation") {
        std::string line = "Recommended shaper is mzv @ 36.7 Hz";

        std::smatch match;
        CHECK_FALSE(std::regex_search(line, match, rec_new));
        CHECK_FALSE(std::regex_search(line, match, rec_smoother));
        REQUIRE(std::regex_search(line, match, rec_old));
        CHECK(match[1].str() == "mzv");
        CHECK(match[2].str() == "36.7");
    }
}

// ============================================================================
// Sweep range + phase reporting
//
// Regression cover for a Voron running Kalico's default 5-135 Hz sweep: the
// collector assumed the sweep ended at 100 Hz, so progress saturated with a
// quarter of the frequencies still untested and the panel switched its label
// to "Analyzing data..." while the toolhead was still moving. These tests feed
// the collector G-code lines directly (no LVGL pump, so the mock's scripted
// transcript stays out of the way) and assert on the phase it reports.
// ============================================================================

namespace {

/// Drives one resonance run and records every (percent, phase) pair.
struct ProgressRecorder {
    std::vector<std::pair<int, ShaperCalibrationPhase>> samples;

    [[nodiscard]] helix::ShaperProgressCallback callback() {
        return [this](int percent, ShaperCalibrationPhase phase) {
            samples.emplace_back(percent, phase);
        };
    }

    [[nodiscard]] bool empty() const {
        return samples.empty();
    }

    [[nodiscard]] int last_percent() const {
        return samples.back().first;
    }

    [[nodiscard]] ShaperCalibrationPhase last_phase() const {
        return samples.back().second;
    }

    [[nodiscard]] bool any_phase(ShaperCalibrationPhase phase) const {
        return std::any_of(samples.begin(), samples.end(),
                           [phase](const auto& s) { return s.second == phase; });
    }
};

} // namespace

TEST_CASE_METHOD(InputShaperTestFixture,
                 "sweep progress scales to the printer's configured max_freq",
                 "[calibration][input_shaper][sweep_range]") {
    // Kalico's default range, and the one the reporting Voron actually uses.
    mock_client_.set_resonance_sweep_range(5.0, 135.0);

    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    // 100 Hz is where the old hardcoded ceiling sat. On this printer it is
    // barely three quarters of the way through the sweep.
    mock_client_.dispatch_gcode_response("Testing frequency 100 Hz");

    REQUIRE_FALSE(progress.empty());
    CHECK(progress.last_phase() == ShaperCalibrationPhase::Sweeping);
    // (100-5)/(135-5) of the full 0..100 bar is ~73%. Assert the band rather
    // than the exact integer so rounding tweaks don't make this brittle.
    CHECK(progress.last_percent() > 65);
    CHECK(progress.last_percent() < 80);

    // The remaining frequencies must still move the bar, not sit pinned. 120 Hz
    // is chosen to stay clear of the ceiling, so this measures the mapping
    // rather than the end-of-sweep fallback.
    mock_client_.dispatch_gcode_response("Testing frequency 120 Hz");
    CHECK(progress.last_phase() == ShaperCalibrationPhase::Sweeping);
    CHECK(progress.last_percent() > 80);
    CHECK(progress.last_percent() < 95);
}

TEST_CASE_METHOD(InputShaperTestFixture, "sweep reports its end exactly at the configured ceiling",
                 "[calibration][input_shaper][sweep_range]") {
    // A printer that genuinely stops at 60 Hz should reach the top of the
    // bar there — proving the range was read, not defaulted.
    mock_client_.set_resonance_sweep_range(5.0, 60.0);

    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    mock_client_.dispatch_gcode_response("Testing frequency 60 Hz");

    // Two reports come out of that one line: the sweep reaching the top of the
    // bar, then the end-of-sweep fallback handing over to the analysis phase.
    // The analysis percent is a meaningless 0 — the phase is the signal; the
    // UI shows a spinner plus elapsed time instead of a bar position.
    REQUIRE(progress.samples.size() == 2);
    CHECK(progress.samples[0].first == 100);
    CHECK(progress.samples[0].second == ShaperCalibrationPhase::Sweeping);
    CHECK(progress.samples[1].first == 0);
    CHECK(progress.samples[1].second == ShaperCalibrationPhase::Analyzing);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "overshooting sweep lines are dropped rather than flipping the phase back",
                 "[calibration][input_shaper][sweep_range]") {
    mock_client_.set_resonance_sweep_range(5.0, 100.0);

    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    // Walk to the declared ceiling and then past it. Overshooting a range we
    // read from the printer means the config lied about this run; those lines
    // are dropped rather than flipping the phase back.
    for (int hz : {5, 50, 100, 120, 133}) {
        mock_client_.dispatch_gcode_response("Testing frequency " + std::to_string(hz) + " Hz");
    }

    // 5, 50 and 100 Hz sweep; the 100 Hz line is the ceiling so it also emits
    // the handover. The 120 and 133 Hz lines contradict the range we were given
    // and are dropped rather than flipping the phase back — the label must not
    // oscillate between "measuring" and "analyzing".
    REQUIRE(progress.samples.size() == 4);
    CHECK(progress.samples[0] == std::make_pair(0, ShaperCalibrationPhase::Sweeping));
    // 50 Hz lands at (50-5)/(100-5) ~= 47% of the bar; assert the band so
    // rounding tweaks don't make this brittle.
    CHECK(progress.samples[1].first > 40);
    CHECK(progress.samples[1].first < 55);
    CHECK(progress.samples[1].second == ShaperCalibrationPhase::Sweeping);
    CHECK(progress.samples[2] == std::make_pair(100, ShaperCalibrationPhase::Sweeping));
    CHECK(progress.samples[3] == std::make_pair(0, ShaperCalibrationPhase::Analyzing));

    // The phase never runs backwards.
    for (size_t i = 1; i < progress.samples.size(); ++i) {
        CHECK(static_cast<int>(progress.samples[i].second) >=
              static_cast<int>(progress.samples[i - 1].second));
    }
}

TEST_CASE_METHOD(InputShaperTestFixture, "Klipper's calculating line ends the sweep phase",
                 "[calibration][input_shaper][sweep_range]") {
    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    mock_client_.dispatch_gcode_response("Testing frequency 40 Hz");
    REQUIRE(progress.last_phase() == ShaperCalibrationPhase::Sweeping);

    // The exact wording Klipper and Kalico emit. The collector used to look
    // only for "Wait for calculations..", which no released firmware prints.
    mock_client_.dispatch_gcode_response("Calculating the best input shaper parameters for x axis");

    CHECK(progress.last_phase() == ShaperCalibrationPhase::Analyzing);
    // The analysis phase reports no percent; the 0 is purely the phase-change
    // signal (the UI swaps the bar for a spinner on this report).
    CHECK(progress.last_percent() == 0);
}

TEST_CASE_METHOD(InputShaperTestFixture, "legacy wait-for-calculations line still ends the sweep",
                 "[calibration][input_shaper][sweep_range]") {
    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    mock_client_.dispatch_gcode_response("Testing frequency 40 Hz");
    mock_client_.dispatch_gcode_response("Wait for calculations..");

    CHECK(progress.last_phase() == ShaperCalibrationPhase::Analyzing);
    CHECK(progress.last_percent() == 0);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "repeated wait-for-calculations heartbeats emit nothing and read as live",
                 "[calibration][input_shaper][sweep_range]") {
    // Some firmwares (e.g. Creality's K1C build) print "Wait for calculations.."
    // every few seconds through the whole analysis phase. Only the first line
    // may report the phase change; the repeats must neither re-emit progress
    // nor look like a stalled calibration to the activity watchdog.
    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    mock_client_.dispatch_gcode_response("Testing frequency 40 Hz");
    mock_client_.dispatch_gcode_response("Wait for calculations..");
    REQUIRE(progress.last_phase() == ShaperCalibrationPhase::Analyzing);
    REQUIRE(progress.samples.size() == 2); // sweep line + phase change, nothing else

    LogCapture log;
    for (int i = 0; i < 5; ++i) {
        mock_client_.dispatch_gcode_response("Wait for calculations..");
    }

    // No additional progress callbacks for the repeats...
    CHECK(progress.samples.size() == 2);
    // ...and each one still stamps the activity watchdog, so no gap warning.
    CHECK(log.count_containing("gap in SHAPER_CALIBRATE output") == 0);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "a fitted shaper line ends the sweep even without a marker line",
                 "[calibration][input_shaper][sweep_range]") {
    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    mock_client_.dispatch_gcode_response("Testing frequency 40 Hz");
    REQUIRE(progress.last_phase() == ShaperCalibrationPhase::Sweeping);

    mock_client_.dispatch_gcode_response(
        "Fitted shaper 'zv' frequency = 46.4 Hz (vibrations = 30.2%, smoothing ~= 0.077)");

    // The first fit line is itself the phase-change signal when no marker line
    // preceded it; its percent is the meaningless 0 the analysis phase reports.
    REQUIRE(progress.last_phase() == ShaperCalibrationPhase::Analyzing);
    CHECK(progress.last_percent() == 0);
    REQUIRE(progress.samples.size() == 2);

    // Subsequent fits only accumulate data - no percent-bump callbacks each.
    mock_client_.dispatch_gcode_response(
        "Fitted shaper 'mzv' frequency = 53.8 Hz (vibrations = 12.4%, smoothing ~= 0.130)");
    mock_client_.dispatch_gcode_response(
        "Fitted shaper 'ei' frequency = 56.2 Hz (vibrations = 8.1%, smoothing ~= 0.120)");
    CHECK(progress.samples.size() == 2);
}

TEST_CASE_METHOD(InputShaperTestFixture, "completion reports the complete phase at 100 percent",
                 "[calibration][input_shaper][sweep_range]") {
    ProgressRecorder progress;
    std::atomic<bool> complete_called{false};
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [&](const InputShaperResult&) { complete_called = true; },
        [](const MoonrakerError&) {});

    mock_client_.dispatch_gcode_response("Testing frequency 40 Hz");
    mock_client_.dispatch_gcode_response("Calculating the best input shaper parameters for x axis");
    mock_client_.dispatch_gcode_response(
        "Recommended shaper_type_x = mzv, shaper_freq_x = 36.7 Hz");
    mock_client_.dispatch_gcode_response(
        "Shaper calibration data written to /tmp/calibration_data_x_nonexistent.csv file");

    REQUIRE(complete_called);
    CHECK(progress.last_percent() == 100);
    CHECK(progress.last_phase() == ShaperCalibrationPhase::Complete);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "an implausible configured range falls back to the defaults",
                 "[calibration][input_shaper][sweep_range]") {
    // max_freq below min_freq would divide by a negative range.
    mock_client_.set_resonance_sweep_range(120.0, 20.0);

    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    mock_client_.dispatch_gcode_response("Testing frequency 70 Hz");

    REQUIRE_FALSE(progress.empty());
    // Defaults are 5-135 Hz, so 70 Hz lands mid-sweep rather than clamped:
    // (70-5)/(135-5) ~= 50% of the bar.
    CHECK(progress.last_phase() == ShaperCalibrationPhase::Sweeping);
    CHECK(progress.last_percent() > 30);
    CHECK(progress.last_percent() < 70);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "reaching the configured ceiling ends the sweep without a marker line",
                 "[calibration][input_shaper][sweep_range]") {
    mock_client_.set_resonance_sweep_range(5.0, 135.0);

    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    mock_client_.dispatch_gcode_response("Testing frequency 100 Hz");
    REQUIRE(progress.last_phase() == ShaperCalibrationPhase::Sweeping);

    // Klipper stops one step short of max_freq, so 134 Hz is the last line of a
    // 135 Hz sweep. No marker line follows here — the ceiling itself is the
    // signal, which is what makes this independent of firmware wording.
    mock_client_.dispatch_gcode_response("Testing frequency 134 Hz");

    CHECK(progress.last_phase() == ShaperCalibrationPhase::Analyzing);
    CHECK(progress.last_percent() == 0);
}

TEST_CASE_METHOD(InputShaperTestFixture,
                 "the sweep-end fallback stays off when the range was never read",
                 "[calibration][input_shaper][sweep_range]") {
    // An inverted range is rejected, so the collector keeps its 5-135 defaults
    // and must NOT trust them as a ceiling — a printer sweeping past a guessed
    // ceiling would otherwise claim to be analyzing mid-sweep, the original bug.
    mock_client_.set_resonance_sweep_range(120.0, 20.0);

    ProgressRecorder progress;
    api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {}, [](const MoonrakerError&) {});

    for (int hz : {100, 134, 150, 180}) {
        mock_client_.dispatch_gcode_response("Testing frequency " + std::to_string(hz) + " Hz");
    }

    CHECK_FALSE(progress.any_phase(ShaperCalibrationPhase::Analyzing));
    CHECK(progress.last_phase() == ShaperCalibrationPhase::Sweeping);
    // Overshooting the guessed ceiling clamps at the top of the bar while the
    // phase stays Sweeping - it must not run past 100.
    REQUIRE(progress.samples.size() == 4);
    CHECK(progress.last_percent() == 100);
}

TEST_CASE_METHOD(InputShaperTestFixture, "stall diagnostics survive a run with no sweep at all",
                 "[calibration][input_shaper][sweep_range]") {
    // The timeout path calls log_stall_diagnostics() on a collector that may
    // have parsed nothing. Exercise that shape directly: no responses, no
    // frequencies, no fits — it must not read uninitialised state or throw.
    ProgressRecorder progress;
    std::atomic<bool> errored{false};
    REQUIRE_NOTHROW(api_->advanced().start_resonance_test(
        'X', progress.callback(), [](const InputShaperResult&) {},
        [&](const MoonrakerError&) { errored = true; }));

    CHECK(progress.empty());
}
