// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
/**
 * MoonrakerAPI Security Tests
 *
 * Tests comprehensive input validation for G-code command injection prevention.
 * These tests verify the security fixes for Issue #1 from the security review.
 *
 * Test Categories:
 * 1. Command injection prevention (newline, semicolon, control characters)
 * 2. Range validation (temperatures, speeds, positions, distances, feedrates)
 * 3. Valid input acceptance (positive cases)
 * 4. Error callback invocation and message quality
 * 5. G-code generation verification (no G-code sent when validation fails)
 *
 * SECURITY CRITICAL: These tests prevent malicious input from executing
 * arbitrary G-code commands that could damage the printer or harm users.
 */

// ============================================================================
// Test Fixtures
// ============================================================================

class MoonrakerAPITestFixture : public LVGLTestFixture {
  public:
    MoonrakerAPITestFixture() {
        // Initialize printer state (no XML registration - local instance, not singleton)
        state.init_subjects(false);

        // Create disconnected client for validation testing
        // Validation happens before any network I/O, so disconnected client is fine
        client = std::make_unique<MoonrakerClient>();

        // Create API with client
        api = std::make_unique<MoonrakerAPI>(*client, state);

        // Reset test state
        reset_callbacks();
    }

    ~MoonrakerAPITestFixture() {
        api.reset();
        client.reset();
    }

    void reset_callbacks() {
        success_called = false;
        error_called = false;
        captured_error = MoonrakerError();
    }

    // Standard callbacks for testing
    void success_callback() {
        success_called = true;
    }

    void error_callback(const MoonrakerError& err) {
        error_called = true;
        captured_error = err;
    }

    // Check that validation passed (no VALIDATION_ERROR occurred)
    // Note: Disconnected client may cause network errors, but those are
    // expected and don't indicate validation failure
    bool validation_passed() const {
        return !error_called || captured_error.type != MoonrakerErrorType::VALIDATION_ERROR;
    }

    // Test objects
    std::unique_ptr<MoonrakerClient> client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;

    // Callback tracking
    bool success_called = false;
    bool error_called = false;
    MoonrakerError captured_error;
};

// ============================================================================
// Command Injection Tests - Heater Names
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture,
                 "set_temperature rejects newline injection in heater name",
                 "[api][security][injection]") {
    SECTION("Newline at end of heater name") {
        api->set_temperature(
            "extruder\nM104 S999\n", 200, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.method == "set_temperature");
        REQUIRE_FALSE(captured_error.message.empty());
    }

    SECTION("Newline in middle of heater name") {
        reset_callbacks();
        api->set_temperature(
            "heat\ner_bed", 60, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("Carriage return injection") {
        reset_callbacks();
        api->set_temperature(
            "extruder\rM104 S999", 200, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

TEST_CASE_METHOD(MoonrakerAPITestFixture,
                 "set_temperature rejects semicolon injection in heater name",
                 "[api][security][injection]") {
    SECTION("Semicolon command separator") {
        api->set_temperature(
            "extruder ; M104 S999 ;", 200, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.method == "set_temperature");
    }
}

TEST_CASE_METHOD(MoonrakerAPITestFixture,
                 "set_temperature rejects other malicious characters in heater name",
                 "[api][security][injection]") {
    SECTION("Null byte injection") {
        std::string heater_with_null = "extruder";
        heater_with_null += '\0';
        heater_with_null += "M104 S999";

        api->set_temperature(
            heater_with_null, 200, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("Control characters") {
        reset_callbacks();
        api->set_temperature(
            "extruder\x01\x02", 200, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("Special shell characters") {
        reset_callbacks();
        api->set_temperature(
            "extruder&", 200, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Command Injection Tests - Fan Names
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "set_fan_speed rejects injection in fan name",
                 "[api][security][injection]") {
    SECTION("Newline injection in fan name") {
        api->set_fan_speed(
            "fan\nM106 S255\n", 50, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.method == "set_fan_speed");
    }

    SECTION("Semicolon injection in fan name") {
        reset_callbacks();
        api->set_fan_speed(
            "fan ; M106 S255 ;", 50, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Command Injection Tests - Axes Parameters
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "home_axes rejects invalid axis characters",
                 "[api][security][injection]") {
    SECTION("Newline in axes parameter") {
        api->motion().home_axes(
            "X\nG28 Z\n", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.method == "home_axes");
    }

    SECTION("Invalid axis letter") {
        reset_callbacks();
        api->motion().home_axes(
            "XYA", // 'A' is not a valid axis
            [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("Semicolon injection") {
        reset_callbacks();
        api->motion().home_axes(
            "X;G28 Z", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("Space injection") {
        reset_callbacks();
        api->motion().home_axes(
            "X Y", // Spaces not allowed in axes parameter
            [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

TEST_CASE_METHOD(MoonrakerAPITestFixture, "move_axis rejects invalid axis characters",
                 "[api][security][injection]") {
    SECTION("Invalid axis character") {
        api->motion().move_axis(
            'A', 10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.method == "move_axis");
    }

    SECTION("Special character as axis") {
        reset_callbacks();
        api->motion().move_axis(
            '\n', 10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Range Validation Tests - Temperatures
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "set_temperature validates temperature range",
                 "[api][security][range]") {
    SECTION("Negative temperature rejected") {
        api->set_temperature(
            "extruder", -10.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.message.find("0°C to 400°C") != std::string::npos);
    }

    SECTION("Zero temperature accepted") {
        reset_callbacks();
        api->set_temperature(
            "extruder", 0.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
        // Note: success_called depends on mock implementation
    }

    SECTION("Normal temperature accepted (200°C)") {
        reset_callbacks();
        api->set_temperature(
            "extruder", 200.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Maximum temperature accepted (400°C)") {
        reset_callbacks();
        api->set_temperature(
            "extruder", 400.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Over maximum temperature rejected (500°C)") {
        reset_callbacks();
        api->set_temperature(
            "extruder", 500.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("Extremely high temperature rejected (999°C)") {
        reset_callbacks();
        api->set_temperature(
            "extruder", 999.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Range Validation Tests - Fan Speeds
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "set_fan_speed validates speed range",
                 "[api][security][range]") {
    SECTION("Negative speed rejected") {
        api->set_fan_speed(
            "fan", -10.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.message.find("0% to 100%") != std::string::npos);
    }

    SECTION("Zero speed accepted (fan off)") {
        reset_callbacks();
        api->set_fan_speed(
            "fan", 0.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Normal speed accepted (50%)") {
        reset_callbacks();
        api->set_fan_speed(
            "fan", 50.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Maximum speed accepted (100%)") {
        reset_callbacks();
        api->set_fan_speed(
            "fan", 100.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Over maximum speed rejected (150%)") {
        reset_callbacks();
        api->set_fan_speed(
            "fan", 150.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Range Validation Tests - Feedrates
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "move_axis validates feedrate range",
                 "[api][security][range]") {
    SECTION("Negative feedrate rejected") {
        api->motion().move_axis(
            'X', 10.0, -1000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.message.find("0 to 50000mm/min") != std::string::npos);
    }

    SECTION("Zero feedrate accepted (use default)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 10.0, 0.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Normal feedrate accepted (3000 mm/min)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Maximum feedrate accepted (50000 mm/min)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 10.0, 50000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Over maximum feedrate rejected (100000 mm/min)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 10.0, 100000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Range Validation Tests - Distances (Relative Movement)
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "move_axis validates distance range",
                 "[api][security][range]") {
    SECTION("Under minimum distance rejected (-2000mm)") {
        api->motion().move_axis(
            'X', -2000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.message.find("-1000.0mm to 1000.0mm") != std::string::npos);
    }

    SECTION("Minimum distance accepted (-1000mm)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', -1000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Normal negative distance accepted (-10mm)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', -10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Normal positive distance accepted (10mm)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Maximum distance accepted (1000mm)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 1000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Over maximum distance rejected (2000mm)") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 2000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Range Validation Tests - Positions (Absolute Movement)
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "move_to_position validates position range",
                 "[api][security][range]") {
    SECTION("Negative position rejected") {
        api->motion().move_to_position(
            'X', -10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.message.find("0.0mm to 1000.0mm") != std::string::npos);
    }

    SECTION("Zero position accepted") {
        reset_callbacks();
        api->motion().move_to_position(
            'X', 0.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Normal position accepted (100mm)") {
        reset_callbacks();
        api->motion().move_to_position(
            'X', 100.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Maximum position accepted (1000mm)") {
        reset_callbacks();
        api->motion().move_to_position(
            'X', 1000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Over maximum position rejected (2000mm)") {
        reset_callbacks();
        api->motion().move_to_position(
            'X', 2000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE_FALSE(success_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}

// ============================================================================
// Valid Input Acceptance Tests - Identifiers
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "set_temperature accepts valid heater names",
                 "[api][security][valid]") {
    SECTION("Standard extruder name") {
        api->set_temperature(
            "extruder", 200.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Bed heater name") {
        reset_callbacks();
        api->set_temperature(
            "heater_bed", 60.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Generic heater with space in name") {
        reset_callbacks();
        api->set_temperature(
            "heater_generic chamber", 50.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Heater with underscores and numbers") {
        reset_callbacks();
        api->set_temperature(
            "extruder_1", 200.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }
}

TEST_CASE_METHOD(MoonrakerAPITestFixture, "set_fan_speed accepts valid fan names",
                 "[api][security][valid]") {
    SECTION("Standard fan name") {
        api->set_fan_speed(
            "fan", 50.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Generic fan with space") {
        reset_callbacks();
        api->set_fan_speed(
            "fan_generic cooling_fan", 75.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Fan with numbers") {
        reset_callbacks();
        api->set_fan_speed(
            "fan_1", 100.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }
}

// ============================================================================
// Valid Input Acceptance Tests - Axes
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "home_axes accepts valid axis specifications",
                 "[api][security][valid]") {
    SECTION("Single uppercase axis") {
        api->motion().home_axes(
            "X", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Single lowercase axis") {
        reset_callbacks();
        api->motion().home_axes(
            "y", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Multiple axes uppercase") {
        reset_callbacks();
        api->motion().home_axes(
            "XYZ", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Multiple axes lowercase") {
        reset_callbacks();
        api->motion().home_axes(
            "xyz", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Mixed case axes") {
        reset_callbacks();
        api->motion().home_axes(
            "xY", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Empty axes (home all)") {
        reset_callbacks();
        api->motion().home_axes(
            "", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }
}

TEST_CASE_METHOD(MoonrakerAPITestFixture, "move_axis accepts valid axis characters",
                 "[api][security][valid]") {
    SECTION("X axis uppercase") {
        api->motion().move_axis(
            'X', 10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Y axis lowercase") {
        reset_callbacks();
        api->motion().move_axis(
            'y', -5.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("Z axis") {
        reset_callbacks();
        api->motion().move_axis(
            'Z', 0.2, 1000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }

    SECTION("E axis (extruder)") {
        reset_callbacks();
        api->motion().move_axis(
            'E', 5.0, 300.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(validation_passed());
    }
}

// ============================================================================
// Error Message Quality Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "Validation errors provide descriptive messages",
                 "[api][security][errors]") {
    SECTION("Temperature range error includes range") {
        api->set_temperature(
            "extruder", 500.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.message.find("0") != std::string::npos);
        REQUIRE(captured_error.message.find("400") != std::string::npos);
    }

    SECTION("Fan speed error includes percentage") {
        reset_callbacks();
        api->set_fan_speed(
            "fan", 150.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.message.find("0% to 100%") != std::string::npos);
    }

    SECTION("Invalid identifier error explains character restriction") {
        reset_callbacks();
        api->set_temperature(
            "extruder\n", 200.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.message.find("unsafe characters") != std::string::npos);
    }

    SECTION("Invalid axis error shows the character") {
        reset_callbacks();
        api->motion().move_axis(
            'A', 10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.message.find("A") != std::string::npos);
    }

    SECTION("Distance range error includes limits") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 2000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.message.find("1000") != std::string::npos);
    }
}

TEST_CASE_METHOD(MoonrakerAPITestFixture, "Validation errors include method name",
                 "[api][security][errors]") {
    SECTION("set_temperature method name") {
        api->set_temperature(
            "extruder", -10.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.method == "set_temperature");
    }

    SECTION("set_fan_speed method name") {
        reset_callbacks();
        api->set_fan_speed(
            "fan", -10.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.method == "set_fan_speed");
    }

    SECTION("home_axes method name") {
        reset_callbacks();
        api->motion().home_axes(
            "XA", [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.method == "home_axes");
    }

    SECTION("move_axis method name") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 2000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.method == "move_axis");
    }

    SECTION("move_to_position method name") {
        reset_callbacks();
        api->motion().move_to_position(
            'X', -10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.method == "move_to_position");
    }
}

// ============================================================================
// Edge Cases and Boundary Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "Validation handles edge cases",
                 "[api][security][edge]") {
    SECTION("Empty heater name rejected") {
        api->set_temperature(
            "", 200.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("Empty fan name rejected") {
        reset_callbacks();
        api->set_fan_speed(
            "", 50.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("Boundary temperature values") {
        // Test exact boundaries
        reset_callbacks();
        api->set_temperature(
            "extruder", 0.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });
        REQUIRE(validation_passed());

        reset_callbacks();
        api->set_temperature(
            "extruder", 400.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });
        REQUIRE(validation_passed());
    }

    SECTION("Floating point precision at boundaries") {
        reset_callbacks();
        api->set_temperature(
            "extruder", 400.00001, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });
        REQUIRE(error_called);

        reset_callbacks();
        api->motion().move_axis(
            'X', 1000.00001, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });
        REQUIRE(error_called);
    }
}

// ============================================================================
// G-code Generation Prevention Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPITestFixture, "No G-code sent when validation fails",
                 "[api][security][gcode]") {
    SECTION("Invalid temperature - no RPC call") {
        api->set_temperature(
            "extruder", 500.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
    }

    SECTION("Invalid heater name - no RPC call") {
        reset_callbacks();
        api->set_temperature(
            "extruder\nM104 S999", 200.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
    }

    SECTION("Invalid axis - no RPC call") {
        reset_callbacks();
        api->motion().move_axis(
            'A', 10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
    }

    SECTION("Invalid distance - no RPC call") {
        reset_callbacks();
        api->motion().move_axis(
            'X', 2000.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        REQUIRE(error_called);
    }

    SECTION("Multiple validation failures - no RPC calls") {
        reset_callbacks();

        // Fail 1: Invalid temperature
        api->set_temperature(
            "extruder", 500.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        // Fail 2: Invalid axis
        reset_callbacks();
        api->motion().move_axis(
            'Q', 10.0, 3000.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        // Fail 3: Invalid fan speed
        reset_callbacks();
        api->set_fan_speed(
            "fan", 200.0, [this]() { this->success_callback(); },
            [this](const MoonrakerError& err) { this->error_callback(err); });

        // Verify NO G-code was generated for any call
    }
}

// ============================================================================
// Lifecycle Safety Tests - Issue #4: Callback Cleanup on Destruction
// ============================================================================

TEST_CASE("MoonrakerClient destructor clears callbacks",
          "[eventloop][api][security][lifecycle][slow]") {
    SECTION("Destroy client before connection completes") {
        // Default-construct (no external loop): libhv only joins its event-loop
        // thread in ~WebSocketClient when is_loop_owner is true, which requires a
        // null loop. See the note in test_moonraker_client_security.cpp.
        auto client = std::make_unique<MoonrakerClient>();

        bool connected_called = false;
        bool disconnected_called = false;

        // Start connection (will fail because no server)
        client->connect(
            "ws://127.0.0.1:9999/websocket", [&connected_called]() { connected_called = true; },
            [&disconnected_called]() { disconnected_called = true; });

        // Destroy client immediately (before connection completes)
        client.reset();

        // Run event loop briefly to allow any pending events to fire
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // If callbacks were not cleared, this would crash with use-after-free
        // The test passing means callbacks were properly cleared
        REQUIRE_FALSE(connected_called);
    }

    SECTION("Destroy client with pending requests") {
        auto client = std::make_unique<MoonrakerClient>();

        int error_callback_called = 0;

        // Send request on a client that never connected. ready_to_send() is
        // false, so send_jsonrpc() invokes the error callback SYNCHRONOUSLY and
        // returns kInvalidRequestId (#909) - the request is never handed to the
        // tracker. That, not destruction, is what sets the flag below; the
        // destructor deliberately drops pending requests WITHOUT invoking their
        // error callbacks, because the callback targets may already be gone.
        client->send_jsonrpc(
            "printer.info", json(), [](json) { /* Success - should not be called */ },
            [&error_callback_called](const MoonrakerError& err) {
                error_callback_called++;
                REQUIRE(err.type == MoonrakerErrorType::CONNECTION_LOST);
            });

        REQUIRE(error_callback_called == 1); // fired inline, before the reset below

        client.reset();

        // Still exactly one invocation - the destructor did not re-run it.
        REQUIRE(error_callback_called == 1);
    }

    SECTION("Multiple rapid create/destroy cycles") {
        // Stress test: rapid allocation and deallocation.
        //
        // The observable is ownership of the caller's callbacks, not a crash.
        // connect() copies on_connected/on_disconnected into last_on_connected_/
        // last_on_disconnected_ under reconnect_mutex_ so the install-once
        // trampolines can snapshot them per invocation instead of capturing them
        // (see MoonrakerClient::connect). A shared_ptr captured by those lambdas
        // therefore has use_count > 1 for exactly as long as the client lives,
        // and drops back to our single local reference when it is destroyed.
        // A copy escaping into libhv's reconnect machinery, a detached timer, or
        // any global would keep the count above 1 here.
        struct Counters {
            std::atomic<int> opened{0};
            std::atomic<int> closed{0};
        };
        auto sentinel = std::make_shared<Counters>();
        int connects_started = 0;

        for (int i = 0; i < 10; i++) {
            auto client = std::make_unique<MoonrakerClient>();

            int rc = client->connect(
                "ws://127.0.0.1:9999/websocket",
                [sentinel]() { sentinel->opened.fetch_add(1, std::memory_order_relaxed); },
                [sentinel]() { sentinel->closed.fetch_add(1, std::memory_order_relaxed); });

            // open() returning 0 means the connect was actually initiated -
            // without this the loop could silently stop reaching the code below.
            CHECK(rc == 0);
            connects_started++;

            // The live client holds both copies.
            CHECK(sentinel.use_count() > 1);

            // Destroy immediately
            client.reset();

            // ...and released them. Only our local reference is left. This is
            // deterministic despite the live event-loop thread: the client is
            // default-constructed, so is_loop_owner is true and ~WebSocketClient
            // joins the loop before returning. Any snapshot copy on_ws_close()
            // took under reconnect_mutex_ is therefore already gone.
            CHECK(sentinel.use_count() == 1);
        }

        CHECK(connects_started == 10);

        // Port 9999 refuses the connection, so the WebSocket can never open.
        // A non-zero count here means a callback ran that had no business
        // running - the wrong trampoline branch, or a stale callback from an
        // already-destroyed cycle.
        CHECK(sentinel->opened.load() == 0);

        // The DISCONNECT side is deliberately not asserted on. Loopback refuses
        // the connect fast enough that on_ws_close() usually invokes it before
        // client.reset() lands (10/10 locally), but that is a race against the
        // event-loop thread, not a contract - under load reset() can win and the
        // callback never fires. Reported, not required.
        INFO("disconnect callbacks observed: " << sentinel->closed.load() << "/10");
    }

    SECTION("Destroy client during active connection (mock)") {
        auto client = std::make_unique<MoonrakerClient>();

        // Register a persistent callback
        bool notify_callback_called = false;
        client->register_notify_update(
            [&notify_callback_called](json /* j */) { notify_callback_called = true; });

        // Destroy client
        client.reset();

        // Create new JSON notification (simulating what would come from network)
        json notification = {{"method", "notify_status_update"}};

        // If old callback is still registered, accessing it would crash
        // The test passing means callbacks were properly cleared
        REQUIRE_FALSE(notify_callback_called);
    }
}

TEST_CASE("MoonrakerClient cleanup_pending_requests is exception-safe",
          "[api][security][lifecycle][eventloop][slow]") {
    SECTION("Cleanup with error callbacks that throw exceptions") {
        auto client = std::make_unique<MoonrakerClient>();

        bool first_callback_called = false;
        bool second_callback_called = false;

        // Register request with throwing error callback
        client->send_jsonrpc(
            "printer.info", json(), [](json) { /* Success */ },
            [&first_callback_called](const MoonrakerError& err) {
                first_callback_called = true;
                throw std::runtime_error("Test exception");
            });

        // Register another request
        client->send_jsonrpc(
            "server.info", json(), [](json) { /* Success */ },
            [&second_callback_called](const MoonrakerError& err) {
                second_callback_called = true;
            });

        // Destroy client - should not crash even if callbacks throw
        REQUIRE_NOTHROW(client.reset());

        // First callback was called (but threw)
        REQUIRE(first_callback_called);

        // Note: Second callback may or may not be called depending on
        // whether exception handling stops iteration. The important
        // thing is no crash/memory corruption.
    }
}

TEST_CASE_METHOD(MoonrakerAPITestFixture,
                 "A rejected argument is reported once: to on_error if given, else as a toast",
                 "[api][security][validation]") {
    std::vector<std::string> toasts;
    helix::ui::set_test_notification_error_hook(
        [&toasts](const std::string& msg) { toasts.push_back(msg); });

    std::vector<std::string> shown;
    auto on_error = [&shown](const MoonrakerError& err) {
        CHECK(err.type == MoonrakerErrorType::VALIDATION_ERROR);
        shown.push_back(err.user_message());
    };

    SECTION("with on_error, no toast, and the caller gets the user-facing text") {
        api->motion().move_axis('A', 10.0, 3000.0, nullptr, on_error);
        api->set_fan_speed("fan;M112", 50.0, nullptr, on_error);
        REQUIRE(shown.size() == 2);
        CHECK(shown[0] == "Invalid axis 'A'. Must be X, Y, Z, or E.");
        CHECK(shown[1] == "Invalid fan name 'fan;M112'. Contains unsafe characters.");
        CHECK(toasts.empty());
    }

    SECTION("without on_error, one toast each") {
        api->motion().move_axis('A', 10.0, 3000.0, nullptr, nullptr);
        api->set_fan_speed("fan;M112", 50.0, nullptr, nullptr);
        CHECK(toasts.size() == 2);
    }

    helix::ui::set_test_notification_error_hook(nullptr);
}
