/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "printer_detector.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Fixtures and Helpers
// ============================================================================

/**
 * @brief Test fixture providing common hardware configurations
 */
class PrinterDetectorFixture {
  protected:
    // Create empty hardware data
    PrinterHardwareData empty_hardware() {
        return PrinterHardwareData{};
    }

    // Create FlashForge AD5M Pro fingerprint (real hardware from user)
    PrinterHardwareData flashforge_ad5m_pro_hardware() {
        return PrinterHardwareData{
            .heaters = {"extruder", "heater_bed"},
            .sensors = {"tvocValue", "weightValue", "temperature_sensor chamber_temp"},
            .fans = {"fan", "fan_generic exhaust_fan"},
            .leds = {"neopixel led_strip"},
            .hostname = "flashforge-ad5m-pro"};
    }

    // Create Voron V2 fingerprint with bed fans and chamber
    PrinterHardwareData voron_v2_hardware() {
        return PrinterHardwareData{.heaters = {"extruder", "heater_bed"},
                                   .sensors = {"temperature_sensor chamber"},
                                   .fans = {"controller_fan", "exhaust_fan", "bed_fans"},
                                   .leds = {"neopixel chamber_leds"},
                                   .hostname = "voron-v2"};
    }

    // Create generic printer without distinctive features
    PrinterHardwareData generic_hardware() {
        return PrinterHardwareData{.heaters = {"extruder", "heater_bed"},
                                   .sensors = {},
                                   .fans = {"fan", "heater_fan hotend_fan"},
                                   .leds = {},
                                   .hostname = "mainsailos"};
    }

    // Create hardware with mixed signals (FlashForge sensor + Voron hostname)
    PrinterHardwareData conflicting_hardware() {
        return PrinterHardwareData{.heaters = {"extruder", "heater_bed"},
                                   .sensors = {"tvocValue"},
                                   .fans = {"bed_fans"},
                                   .leds = {},
                                   .hostname = "voron-v2"};
    }

    // Create Creality K1 fingerprint
    PrinterHardwareData creality_k1_hardware() {
        return PrinterHardwareData{.heaters = {"extruder", "heater_bed"},
                                   .sensors = {},
                                   .fans = {"fan", "chamber_fan"},
                                   .leds = {},
                                   .hostname = "k1-max"};
    }

    // Create Creality Ender 3 fingerprint
    PrinterHardwareData creality_ender3_hardware() {
        return PrinterHardwareData{.heaters = {"extruder", "heater_bed"},
                                   .sensors = {},
                                   .fans = {"fan", "heater_fan hotend_fan"},
                                   .leds = {},
                                   .hostname = "ender3-v2"};
    }
};

// ============================================================================
// Basic Detection Tests
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: Detect FlashForge AD5M Pro by tvocValue sensor",
                 "[printer_detector][sensor_match]") {
    auto hardware = flashforge_ad5m_pro_hardware();
    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FlashForge AD5M Pro");
    // Hostname "ad5m-pro" matches at 96% to differentiate from Adventurer 5M
    REQUIRE(result.confidence == 96);
    // The highest confidence match determines the reason (hostname, not sensor)
    REQUIRE(result.reason.find("ad5m-pro") != std::string::npos);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Detect Voron V2 by bed_fans",
                 "[printer_detector][fan_match]") {
    auto hardware = voron_v2_hardware();
    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    // Fan combo (bed_fans + exhaust) gives 75% confidence
    REQUIRE(result.confidence == 75);
    // Reason should mention fans or Voron enclosed signature
    bool has_voron_reason = (result.reason.find("fan") != std::string::npos ||
                             result.reason.find("Voron") != std::string::npos);
    REQUIRE(has_voron_reason);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Detect by hostname - FlashForge",
                 "[printer_detector][hostname_match]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "flashforge-model"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // Both FlashForge models have "flashforge" hostname match at 80%
    // Adventurer 5M comes first in database, so it wins on tie
    REQUIRE(result.type_name == "FlashForge Adventurer 5M");
    REQUIRE(result.confidence == 80);
    REQUIRE(result.reason.find("Hostname") != std::string::npos);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Detect by hostname - Voron V2",
                 "[printer_detector][hostname_match]") {
    // Use "voron" in hostname to trigger Voron detection
    // "v2" alone is too generic and doesn't match any database entry
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "voron-printer"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    // "voron" hostname match is at 75% in database
    REQUIRE(result.confidence == 75);
    REQUIRE(result.reason.find("voron") != std::string::npos);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Detect by hostname - Creality K1",
                 "[printer_detector][hostname_match]") {
    auto hardware = creality_k1_hardware();
    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // Hostname "k1-max" matches K1 Max specifically at higher confidence
    REQUIRE(result.type_name == "Creality K1 Max");
    REQUIRE(result.confidence == 90);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Detect by hostname - Creality Ender 3",
                 "[printer_detector][hostname_match]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan", "heater_fan hotend_fan"},
        .leds = {},
        .hostname = "ender3-pro" // Avoid "v2" pattern conflict
    };

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality Ender 3");
    // Database has "ender3" hostname match at 85%
    REQUIRE(result.confidence == 85);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Empty hardware returns no detection",
                 "[printer_detector][edge_case]") {
    auto hardware = empty_hardware();
    auto result = PrinterDetector::detect(hardware);

    REQUIRE_FALSE(result.detected());
    REQUIRE(result.type_name.empty());
    REQUIRE(result.confidence == 0);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Generic printer returns no detection",
                 "[printer_detector][edge_case]") {
    auto hardware = generic_hardware();
    auto result = PrinterDetector::detect(hardware);

    REQUIRE_FALSE(result.detected());
    REQUIRE(result.confidence == 0);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: Multiple matches return highest confidence",
                 "[printer_detector][edge_case]") {
    // Conflicting hardware: FlashForge sensor (95%) vs Voron hostname (85%)
    auto hardware = conflicting_hardware();
    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // tvocValue matches Adventurer 5M at 95% (first in database)
    REQUIRE(result.type_name == "FlashForge Adventurer 5M");
    REQUIRE(result.confidence == 95); // Should pick FlashForge (higher confidence)
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: Unknown hostname with no distinctive features",
                 "[printer_detector][edge_case]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "my-custom-printer-123"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE_FALSE(result.detected());
    REQUIRE(result.confidence == 0);
}

// ============================================================================
// Case Sensitivity Tests
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Case-insensitive sensor matching",
                 "[printer_detector][case_sensitivity]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder"},
        .sensors = {"TVOCVALUE", "temperature_sensor chamber"}, // Uppercase
        .fans = {},
        .leds = {},
        .hostname = "test"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FlashForge AD5M Pro");
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Case-insensitive hostname matching",
                 "[printer_detector][case_sensitivity]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder"},
        .sensors = {},
        .fans = {},
        .leds = {},
        .hostname = "FLASHFORGE-AD5M" // Uppercase
    };

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FlashForge AD5M Pro");
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Case-insensitive fan matching",
                 "[printer_detector][case_sensitivity]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {"BED_FANS", "EXHAUST_fan"}, // Mixed case
                                 .leds = {},
                                 .hostname = "test"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
}

// ============================================================================
// Heuristic Type Tests
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: sensor_match heuristic - weightValue",
                 "[printer_detector][heuristics]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {"weightValue"}, // 70% confidence
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FlashForge AD5M Pro");
    REQUIRE(result.confidence == 70);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: fan_match heuristic - single pattern",
                 "[printer_detector][heuristics]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {"bed_fans"}, // 50% confidence alone
                                 .leds = {},
                                 .hostname = "test"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.confidence == 50);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: fan_combo heuristic - multiple patterns required",
                 "[printer_detector][heuristics]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder"},
        .sensors = {},
        .fans = {"bed_fans", "chamber_fan", "exhaust_fan"}, // 70% confidence with combo
        .leds = {},
        .hostname = "test"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.confidence == 70); // fan_combo has higher confidence than single fan_match
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: fan_combo missing one pattern fails",
                 "[printer_detector][heuristics]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder"},
        .sensors = {},
        .fans = {"bed_fans"}, // Has bed_fans but missing chamber/exhaust
        .leds = {},
        .hostname = "generic-test" // No hostname match
    };

    auto result = PrinterDetector::detect(hardware);

    // Should only match single fan_match (50%), not fan_combo (70%)
    REQUIRE(result.detected());
    REQUIRE(result.confidence == 50);
}

// ============================================================================
// Real-World Printer Fingerprints
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Real FlashForge AD5M Pro fingerprint",
                 "[printer_detector][real_world]") {
    // Based on actual hardware discovery from FlashForge AD5M Pro
    PrinterHardwareData hardware{
        .heaters = {"extruder", "extruder1", "heater_bed"},
        .sensors = {"tvocValue", "weightValue", "temperature_sensor chamber_temp",
                    "temperature_sensor mcu_temp"},
        .fans = {"fan", "fan_generic exhaust_fan", "heater_fan hotend_fan"},
        .leds = {"neopixel led_strip"},
        .hostname = "flashforge-ad5m-pro"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FlashForge AD5M Pro");
    REQUIRE(result.confidence == 95); // tvocValue is most distinctive (95%)
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Real Voron 2.4 fingerprint",
                 "[printer_detector][real_world]") {
    // Typical Voron 2.4 configuration
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor chamber", "temperature_sensor raspberry_pi",
                    "temperature_sensor octopus"},
        .fans = {"fan", "heater_fan hotend_fan", "controller_fan octopus_fan",
                 "temperature_fan bed_fans", "fan_generic exhaust_fan"},
        .leds = {"neopixel chamber_leds", "neopixel sb_leds"},
        .hostname = "voron2-4159"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    // Hostname "voron2-4159" matches "voron" pattern (75%) - "v2" pattern requires hyphen/space
    REQUIRE(result.confidence == 75);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Voron 2.4 without v2 in hostname",
                 "[printer_detector][real_world]") {
    // Voron V2 with generic hostname (only hardware detection available)
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor chamber"},
        .fans = {"bed_fans", "exhaust_fan", "controller_fan"},
        .leds = {},
        .hostname = "mainsailos" // Generic hostname
    };

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.confidence == 70); // fan_combo match
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Voron 0.1 by hostname only",
                 "[printer_detector][real_world]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan", "heater_fan hotend_fan"},
                                 .leds = {},
                                 .hostname = "voron-v0-mini"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 0.1");
    REQUIRE(result.confidence == 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Voron Trident by hostname",
                 "[printer_detector][real_world]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "voron-trident-300"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron Trident");
    REQUIRE(result.confidence == 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Voron Switchwire by hostname",
                 "[printer_detector][real_world]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "switchwire-250"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron Switchwire");
    REQUIRE(result.confidence == 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Creality K1 with chamber fan",
                 "[printer_detector][real_world]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan", "chamber_fan"},
                                 .leds = {},
                                 .hostname = "creality-k1-max"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality K1");
    REQUIRE(result.confidence == 80); // Hostname match
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Creality Ender 3 V2",
                 "[printer_detector][real_world]") {
    // NOTE: Hostname must contain "ender3" pattern but avoid "v2" substring
    // which would match Voron 2.4 at higher confidence (85% vs 80%)
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan", "heater_fan hotend_fan"},
        .leds = {},
        .hostname = "my-ender3-printer" // Contains "ender3" without "v2"
    };

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality Ender 3");
    REQUIRE(result.confidence == 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Creality Ender 5 Plus",
                 "[printer_detector][real_world]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "ender5-plus"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality Ender 5");
    REQUIRE(result.confidence == 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Creality CR-10",
                 "[printer_detector][real_world]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "cr-10-s5"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality CR-10");
    REQUIRE(result.confidence == 80);
}

// ============================================================================
// Confidence Scoring Tests
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: High confidence (≥70) detection",
                 "[printer_detector][confidence]") {
    auto hardware = flashforge_ad5m_pro_hardware();
    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.confidence >= 70); // Should be considered high confidence
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Medium confidence (50-69) detection",
                 "[printer_detector][confidence]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {"bed_fans"}, // 50% confidence
                                 .leds = {},
                                 .hostname = "test"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.confidence >= 50);
    REQUIRE(result.confidence < 70);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Zero confidence (no match)",
                 "[printer_detector][confidence]") {
    auto hardware = generic_hardware();
    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.confidence == 0);
}

// ============================================================================
// Database Loading Tests
// ============================================================================

TEST_CASE("PrinterDetector: Database loads successfully", "[printer_detector][database]") {
    // First detection loads database
    PrinterHardwareData hardware;
    auto result = PrinterDetector::detect(hardware);

    // Should not crash or return error reason about database
    REQUIRE(result.reason.find("Failed to load") == std::string::npos);
    REQUIRE(result.reason.find("Invalid") == std::string::npos);
}

TEST_CASE("PrinterDetector: Subsequent calls use cached database", "[printer_detector][database]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {"tvocValue"},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test"};

    // First call loads database
    auto result1 = PrinterDetector::detect(hardware);
    REQUIRE(result1.detected());

    // Second call should use cached database (no reload)
    auto result2 = PrinterDetector::detect(hardware);
    REQUIRE(result2.detected());
    REQUIRE(result1.type_name == result2.type_name);
    REQUIRE(result1.confidence == result2.confidence);
}

// ============================================================================
// Helper Method Tests
// ============================================================================

TEST_CASE("PrinterDetector: detected() helper returns true for valid match",
          "[printer_detector][helpers]") {
    PrinterDetectionResult result{
        .type_name = "Test Printer", .confidence = 50, .reason = "Test reason"};

    REQUIRE(result.detected());
}

TEST_CASE("PrinterDetector: detected() helper returns false for no match",
          "[printer_detector][helpers]") {
    PrinterDetectionResult result{.type_name = "", .confidence = 0, .reason = "No match"};

    REQUIRE_FALSE(result.detected());
}

// ============================================================================
// Enhanced Detection Tests - Kinematics
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: kinematics_match heuristic - CoreXY",
                 "[printer_detector][kinematics]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test-printer",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    // CoreXY alone matches many printers at low confidence
    // It should detect something with corexy kinematics
    REQUIRE(result.detected());
    REQUIRE(result.confidence >= 30); // Kinematics match has moderate confidence
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: kinematics_match heuristic - Delta",
                 "[printer_detector][kinematics]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test",
                                 .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                 .printer_objects = {"delta_calibrate"},
                                 .kinematics = "delta"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // Delta kinematics combined with delta_calibrate gives high confidence
    REQUIRE(result.confidence >= 90);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: kinematics_match heuristic - CoreXZ (Switchwire)",
                 "[printer_detector][kinematics]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "corexz"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron Switchwire"); // CoreXZ is Switchwire signature
    REQUIRE(result.confidence == 90);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: kinematics_match heuristic - Cartesian",
                 "[printer_detector][kinematics]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "ender3-test", // To help distinguish
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "cartesian"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality Ender 3");
}

// ============================================================================
// Enhanced Detection Tests - Stepper Count
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: stepper_count heuristic - 4 Z steppers (Voron 2.4)",
                 "[printer_detector][steppers]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1",
                                              "stepper_z2", "stepper_z3"},
                                 .printer_objects = {"quad_gantry_level"},
                                 .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.confidence >= 90); // QGL + 4 Z steppers = very high confidence
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: stepper_count heuristic - 3 Z steppers (Trident)",
                 "[printer_detector][steppers]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {},
        .leds = {},
        .hostname = "test",
        .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1", "stepper_z2"},
        .printer_objects = {"z_tilt"},
        .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron Trident");
    REQUIRE(result.confidence >= 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: stepper_count heuristic - Single Z stepper",
                 "[printer_detector][steppers]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "voron-v0", // Help identify V0
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 0.2");
}

// ============================================================================
// Enhanced Detection Tests - Build Volume
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: build_volume_range heuristic - Small (V0)",
                 "[printer_detector][build_volume]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {},
        .leds = {},
        .hostname = "voron",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 120, .y_min = 0, .y_max = 120, .z_max = 120}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 0.2");
    REQUIRE(result.confidence >= 70);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: build_volume_range heuristic - K1 vs K1 Max",
                 "[printer_detector][build_volume]") {
    // K1 Max has ~300mm build volume
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"chamber_fan"},
        .leds = {},
        .hostname = "creality", // Generic Creality
        .steppers = {},
        .printer_objects = {},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 300, .y_min = 0, .y_max = 300, .z_max = 300}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality K1 Max");
    REQUIRE(result.confidence >= 70);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: build_volume_range heuristic - Large (Ender 5 Max)",
                 "[printer_detector][build_volume]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {},
        .leds = {},
        .hostname = "ender5",
        .steppers = {},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 400, .y_min = 0, .y_max = 400, .z_max = 400}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality Ender 5 Max");
    REQUIRE(result.confidence >= 70);
}

// ============================================================================
// Enhanced Detection Tests - Macro Match
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: macro_match heuristic - KAMP macros",
                 "[printer_detector][macros]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test",
                                 .steppers = {},
                                 .printer_objects = {"gcode_macro ADAPTIVE_BED_MESH",
                                                     "gcode_macro LINE_PURGE",
                                                     "gcode_macro PRINT_START"},
                                 .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "KAMP (Adaptive Meshing)");
    REQUIRE(result.confidence >= 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: macro_match heuristic - Klippain Shake&Tune",
                 "[printer_detector][macros]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test",
                                 .steppers = {},
                                 .printer_objects = {"gcode_macro AXES_SHAPER_CALIBRATION",
                                                     "gcode_macro BELTS_SHAPER_CALIBRATION",
                                                     "gcode_macro PRINT_START"},
                                 .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Klippain Shake&Tune");
    REQUIRE(result.confidence >= 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: macro_match heuristic - Klicky Probe",
                 "[printer_detector][macros]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test",
                                 .steppers = {},
                                 .printer_objects = {"gcode_macro ATTACH_PROBE",
                                                     "gcode_macro DOCK_PROBE",
                                                     "gcode_macro PRINT_START"},
                                 .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Klicky Probe User");
    REQUIRE(result.confidence >= 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: macro_match heuristic - Happy Hare MMU",
                 "[printer_detector][macros]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {},
        .leds = {},
        .hostname = "test",
        .steppers = {},
        .printer_objects = {"mmu", "gcode_macro MMU_CHANGE_TOOL", "gcode_macro _MMU_LOAD"},
        .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "ERCF/Happy Hare MMU");
    REQUIRE(result.confidence >= 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: macro_match heuristic - Case insensitive",
                 "[printer_detector][macros]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test",
                                 .steppers = {},
                                 .printer_objects =
                                     {
                                         "gcode_macro adaptive_bed_mesh", // lowercase
                                         "gcode_macro LINE_purge"         // mixed case
                                     },
                                 .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "KAMP (Adaptive Meshing)");
}

// ============================================================================
// Enhanced Detection Tests - Object Exists
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: object_exists heuristic - quad_gantry_level",
                 "[printer_detector][objects]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "test",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1",
                                              "stepper_z2", "stepper_z3"},
                                 .printer_objects = {"quad_gantry_level"},
                                 .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.confidence >= 95);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: object_exists heuristic - z_tilt",
                 "[printer_detector][objects]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {},
        .leds = {},
        .hostname = "test",
        .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1", "stepper_z2"},
        .printer_objects = {"z_tilt"},
        .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // z_tilt with 3 Z steppers = Trident
    REQUIRE(result.type_name == "Voron Trident");
}

// ============================================================================
// Enhanced Detection Tests - Combined Heuristics
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: Combined heuristics - Full Voron 2.4 fingerprint",
                 "[printer_detector][combined]") {
    // Full Voron 2.4 setup with all data sources
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor chamber"},
        .fans = {"bed_fans", "exhaust_fan", "nevermore"},
        .leds = {"neopixel chamber_leds"},
        .hostname = "voron-2-4",
        .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1", "stepper_z2",
                     "stepper_z3"},
        .printer_objects = {"quad_gantry_level", "neopixel chamber_leds"},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 350, .y_min = 0, .y_max = 350, .z_max = 330}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.confidence >= 95);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: Combined heuristics - Full Creality K1 fingerprint",
                 "[printer_detector][combined]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor chamber_temp"},
        .fans = {"fan", "chamber_fan"},
        .leds = {},
        .hostname = "k1-printer",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {"temperature_fan chamber_fan"},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 220, .y_min = 0, .y_max = 220, .z_max = 250}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality K1");
    REQUIRE(result.confidence >= 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Combined heuristics - Delta printer",
                 "[printer_detector][combined]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "flsun-v400",
        .steppers = {"stepper_a", "stepper_b", "stepper_c"},
        .printer_objects = {"delta_calibrate"},
        .kinematics = "delta",
        .build_volume = {.x_min = -100, .x_max = 100, .y_min = -100, .y_max = 100, .z_max = 400}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FLSUN Delta");
    REQUIRE(result.confidence >= 90);
}

// ============================================================================
// LED-Based Detection Tests (AD5M Pro vs AD5M)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: AD5M Pro distinguished by LED chamber light",
                 "[printer_detector][led_match]") {
    // AD5M Pro has LED chamber light - this is the key differentiator from regular AD5M
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {"tvocValue", "temperature_sensor chamber_temp"},
                                 .fans = {"fan", "fan_generic exhaust_fan"},
                                 .leds = {"led_strip"}, // LED chamber light - AD5M Pro exclusive
                                 .hostname = "flashforge-ad5m", // Generic AD5M hostname
                                 .steppers = {},
                                 .printer_objects = {},
                                 .kinematics = "cartesian"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // LED chamber light should distinguish Pro from regular 5M
    REQUIRE(result.type_name == "FlashForge AD5M Pro");
    REQUIRE(result.confidence >= 95); // LED + tvocValue = high confidence
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Regular AD5M without LED",
                 "[printer_detector][led_match]") {
    // Regular Adventurer 5M does NOT have LED chamber light
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {"tvocValue"}, // Has TVOC but no LED
                                 .fans = {"fan"},
                                 .leds = {}, // No LEDs - regular AD5M
                                 .hostname = "flashforge",
                                 .steppers = {},
                                 .printer_objects = {},
                                 .kinematics = "cartesian"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // Without LED, should detect as regular Adventurer 5M
    REQUIRE(result.type_name == "FlashForge Adventurer 5M");
    REQUIRE(result.confidence >= 90);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: AD5M Pro with neopixel LEDs",
                 "[printer_detector][led_match]") {
    // Some AD5M Pro setups use neopixel instead of led_strip
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {"tvocValue"},
                                 .fans = {"fan"},
                                 .leds = {"neopixel chamber_led"}, // Neopixel variant
                                 .hostname = "ad5m",
                                 .steppers = {},
                                 .printer_objects = {},
                                 .kinematics = "cartesian"};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FlashForge AD5M Pro");
    REQUIRE(result.confidence >= 92);
}

// ============================================================================
// Top Printer Fingerprints - Comprehensive Real-World Tests
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Prusa MK3S+ fingerprint",
                 "[printer_detector][real_world][prusa]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor board_temp"},
        .fans = {"fan", "heater_fan hotend_fan"},
        .leds = {},
        .hostname = "prusa-mk3s",
        .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_e"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 250, .y_min = 0, .y_max = 210, .z_max = 210}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Prusa MK3S+");
    REQUIRE(result.confidence >= 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Prusa MINI fingerprint",
                 "[printer_detector][real_world][prusa]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "prusa-mini",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 180, .y_min = 0, .y_max = 180, .z_max = 180}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Prusa MINI");
    REQUIRE(result.confidence >= 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Rat Rig V-Core 3 fingerprint",
                 "[printer_detector][real_world][ratrig]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor chamber"},
        .fans = {"fan", "heater_fan hotend_fan"},
        .leds = {},
        .hostname = "ratrig-vcore3",
        .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1", "stepper_z2"},
        .printer_objects = {"z_tilt"},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 300, .y_min = 0, .y_max = 300, .z_max = 300}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Rat Rig V-Core 3");
    REQUIRE(result.confidence >= 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Anycubic Kobra fingerprint",
                 "[printer_detector][real_world][anycubic]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan", "heater_fan hotend_fan"},
        .leds = {},
        .hostname = "anycubic-kobra",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 220, .y_min = 0, .y_max = 220, .z_max = 250}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Anycubic Kobra");
    REQUIRE(result.confidence >= 75);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Elegoo Neptune fingerprint",
                 "[printer_detector][real_world][elegoo]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "elegoo-neptune3",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 220, .y_min = 0, .y_max = 220, .z_max = 280}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Elegoo Neptune");
    REQUIRE(result.confidence >= 75);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Sovol SV06 fingerprint",
                 "[printer_detector][real_world][sovol]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "sovol-sv06",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 220, .y_min = 0, .y_max = 220, .z_max = 250}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Sovol SV06");
    REQUIRE(result.confidence >= 75);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Artillery Sidewinder fingerprint",
                 "[printer_detector][real_world][artillery]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "artillery-sidewinder",
        .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1"}, // Dual Z
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 300, .y_min = 0, .y_max = 300, .z_max = 400}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Artillery Sidewinder");
    REQUIRE(result.confidence >= 75);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: BIQU B1 fingerprint",
                 "[printer_detector][real_world][biqu]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "biqu-b1",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 235, .y_min = 0, .y_max = 235, .z_max = 270}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "BIQU B1");
    REQUIRE(result.confidence >= 75);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Two Trees Sapphire Pro fingerprint",
                 "[printer_detector][real_world][twotrees]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "sapphire-pro",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 235, .y_min = 0, .y_max = 235, .z_max = 235}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Two Trees Sapphire Pro");
    REQUIRE(result.confidence >= 75);
}

// ============================================================================
// MCU-Based Detection Tests (Future Feature)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - STM32H723 (BTT Octopus Pro)",
                 "[printer_detector][mcu]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "test",
        .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1", "stepper_z2",
                     "stepper_z3"},
        .printer_objects = {"quad_gantry_level"},
        .kinematics = "corexy",
        .mcu = "stm32h723xx",                          // BTT Octopus Pro MCU
        .mcu_list = {"stm32h723xx", "rp2040", "linux"} // Main + EBB CAN + Linux host
    };

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // STM32H7 + QGL + 4 Z steppers = Voron 2.4 with BTT board
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.confidence >= 95);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: MCU match - STM32F103 (FlashForge stock)",
                 "[printer_detector][mcu]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {"tvocValue"},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "flashforge",
                                 .steppers = {},
                                 .printer_objects = {},
                                 .kinematics = "cartesian",
                                 .mcu = "stm32f103xe", // FlashForge stock MCU
                                 .mcu_list = {"stm32f103xe"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FlashForge Adventurer 5M");
    REQUIRE(result.confidence >= 90);
}

// ============================================================================
// Negative Tests - Ensure No False Positives
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: No false positive on random hostname",
                 "[printer_detector][negative]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "raspberrypi-4b-2022",
                                 .steppers = {},
                                 .printer_objects = {},
                                 .kinematics = "cartesian"};

    auto result = PrinterDetector::detect(hardware);

    // Should NOT detect a specific printer from generic Pi hostname
    REQUIRE_FALSE(result.detected());
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: No false positive on minimal config",
                 "[printer_detector][negative]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder"},
        .sensors = {},
        .fans = {},
        .leds = {},
        .hostname = "localhost",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "" // Unknown kinematics
    };

    auto result = PrinterDetector::detect(hardware);

    // Minimal config should not match any specific printer
    REQUIRE_FALSE(result.detected());
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: No false positive on v2 without Voron features",
                 "[printer_detector][negative]") {
    // "v2" in hostname should NOT match Voron if no other Voron features
    PrinterHardwareData hardware{
        .heaters = {"extruder"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "printer-v2-test", // Contains "v2" but not a Voron
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian" // Not corexy
    };

    auto result = PrinterDetector::detect(hardware);

    // "v2" alone shouldn't trigger Voron detection without corexy/QGL
    if (result.detected()) {
        REQUIRE(result.type_name != "Voron 2.4");
    }
}

// ============================================================================
// MCU-Based Detection Tests - HC32F460 (Anycubic Huada Signature)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - HC32F460 Anycubic Kobra 2",
                 "[printer_detector][mcu][anycubic]") {
    // HC32F460 is a Huada chip almost exclusively used by Anycubic
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "kobra2",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "cartesian",
                                 .mcu = "HC32F460",
                                 .mcu_list = {"HC32F460"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Anycubic Kobra 2");
    // Hostname (85) + MCU (45) - should detect with high confidence
    REQUIRE(result.confidence >= 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: MCU match - HC32F460 Anycubic Kobra 2 Max",
                 "[printer_detector][mcu][anycubic]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "kobra-2-max",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 420, .y_min = 0, .y_max = 420, .z_max = 500},
        .mcu = "HC32F460",
        .mcu_list = {"HC32F460"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Anycubic Kobra 2 Max");
    REQUIRE(result.confidence >= 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - HC32F460 Anycubic Kobra S1",
                 "[printer_detector][mcu][anycubic]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "kobra-s1",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 250, .y_min = 0, .y_max = 250, .z_max = 250},
        .mcu = "HC32F460",
        .mcu_list = {"HC32F460"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Anycubic Kobra S1");
    REQUIRE(result.confidence >= 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: MCU match - HC32F460 Anycubic Kobra S1 Max",
                 "[printer_detector][mcu][anycubic]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "kobra-s1-max",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 400, .y_min = 0, .y_max = 400, .z_max = 450},
        .mcu = "HC32F460",
        .mcu_list = {"HC32F460"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Anycubic Kobra S1 Max");
    REQUIRE(result.confidence >= 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: MCU alone - HC32F460 provides supporting evidence",
                 "[printer_detector][mcu][anycubic]") {
    // MCU alone without hostname should still provide some confidence
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "test-printer", // Generic hostname
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "cartesian",
                                 .mcu = "HC32F460",
                                 .mcu_list = {"HC32F460"}};

    auto result = PrinterDetector::detect(hardware);

    // HC32F460 alone at 45% confidence - should detect as some Anycubic
    REQUIRE(result.detected());
    // Should match one of the Anycubic printers
    bool is_anycubic = result.type_name.find("Anycubic") != std::string::npos ||
                       result.type_name.find("Kobra") != std::string::npos;
    REQUIRE(is_anycubic);
    REQUIRE(result.confidence >= 45);
}

// ============================================================================
// MCU-Based Detection Tests - GD32F303 (FLSUN MKS Robin Nano)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - GD32F303 FLSUN V400",
                 "[printer_detector][mcu][flsun]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "flsun-v400",
                                 .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                 .printer_objects = {"delta_calibrate"},
                                 .kinematics = "delta",
                                 .mcu = "GD32F303",
                                 .mcu_list = {"GD32F303"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FLSUN V400");
    // Delta + hostname + MCU = very high confidence
    REQUIRE(result.confidence >= 90);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - GD32F303 FLSUN Super Racer",
                 "[printer_detector][mcu][flsun]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "flsun-sr",
                                 .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                 .printer_objects = {"delta_calibrate"},
                                 .kinematics = "delta",
                                 .mcu = "GD32F303",
                                 .mcu_list = {"GD32F303"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FLSUN Super Racer");
    REQUIRE(result.confidence >= 85);
}

// ============================================================================
// MCU-Based Detection Tests - STM32H723 (Creality K1 Series)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - STM32H723 Creality K1",
                 "[printer_detector][mcu][creality]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor chamber_temp"},
        .fans = {"fan", "chamber_fan"},
        .leds = {},
        .hostname = "creality-k1",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 220, .y_min = 0, .y_max = 220, .z_max = 250},
        .mcu = "STM32H723",
        .mcu_list = {"STM32H723"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality K1");
    REQUIRE(result.confidence >= 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - STM32H723 Creality K1 Max",
                 "[printer_detector][mcu][creality]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor chamber_temp"},
        .fans = {"fan", "chamber_fan"},
        .leds = {},
        .hostname = "creality-k1-max",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 300, .y_min = 0, .y_max = 300, .z_max = 300},
        .mcu = "STM32H723",
        .mcu_list = {"STM32H723"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality K1 Max");
    REQUIRE(result.confidence >= 90);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - STM32H723 Creality K1C",
                 "[printer_detector][mcu][creality]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {"temperature_sensor chamber_temp"},
                                 .fans = {"fan", "chamber_fan"},
                                 .leds = {},
                                 .hostname = "creality-k1c",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "corexy",
                                 .mcu = "STM32H723",
                                 .mcu_list = {"STM32H723"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Creality K1C");
    REQUIRE(result.confidence >= 85);
}

// ============================================================================
// MCU-Based Detection Tests - STM32F401 (Elegoo Neptune 4)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - STM32F401 Elegoo Neptune 4",
                 "[printer_detector][mcu][elegoo]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "elegoo-neptune4",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "cartesian",
                                 .mcu = "STM32F401",
                                 .mcu_list = {"STM32F401"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Elegoo Neptune 4");
    REQUIRE(result.confidence >= 80);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: MCU match - STM32F401 Elegoo Neptune 4 Pro",
                 "[printer_detector][mcu][elegoo]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "elegoo-neptune4-pro",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "cartesian",
                                 .mcu = "STM32F401",
                                 .mcu_list = {"STM32F401"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Elegoo Neptune 4 Pro");
    REQUIRE(result.confidence >= 85);
}

// ============================================================================
// MCU-Based Detection Tests - STM32F402 (Qidi Plus 4)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - STM32F402 Qidi Plus 4",
                 "[printer_detector][mcu][qidi]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed", "heater_chamber"},
        .sensors = {"temperature_sensor chamber"},
        .fans = {"fan", "chamber_fan"},
        .leds = {},
        .hostname = "qidi-plus4",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 305, .y_min = 0, .y_max = 305, .z_max = 305},
        .mcu = "STM32F402",
        .mcu_list = {"STM32F402"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Qidi Plus 4");
    REQUIRE(result.confidence >= 85);
}

// ============================================================================
// MCU-Based Detection Tests - STM32F103 (Sovol SV08)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match - STM32F103 Sovol SV08",
                 "[printer_detector][mcu][sovol]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "sovol-sv08",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1",
                                              "stepper_z2", "stepper_z3"},
                                 .printer_objects = {"quad_gantry_level"},
                                 .kinematics = "corexy",
                                 .mcu = "STM32F103",
                                 .mcu_list = {"STM32F103"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Sovol SV08");
    // QGL + hostname + MCU = high confidence
    REQUIRE(result.confidence >= 90);
}

// ============================================================================
// Build Volume Detection Tests - Anycubic Series
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: build_volume_range - Kobra S1 (250mm)",
                 "[printer_detector][build_volume][anycubic]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "kobra", // Generic Kobra hostname
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 250, .y_min = 0, .y_max = 250, .z_max = 250},
        .mcu = "HC32F460",
        .mcu_list = {"HC32F460"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // 250mm build volume + HC32F460 + "kobra" hostname should match Kobra S1
    REQUIRE(result.type_name == "Anycubic Kobra S1");
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: build_volume_range - Kobra 2 Max (420mm)",
                 "[printer_detector][build_volume][anycubic]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan"},
        .leds = {},
        .hostname = "anycubic",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 420, .y_min = 0, .y_max = 420, .z_max = 500},
        .mcu = "HC32F460",
        .mcu_list = {"HC32F460"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // Large build volume + HC32F460 should identify as Kobra 2 Max
    REQUIRE(result.type_name == "Anycubic Kobra 2 Max");
}

// ============================================================================
// Case Sensitivity Tests - MCU Matching
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match case insensitive - hc32f460",
                 "[printer_detector][mcu][case_sensitivity]") {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "kobra2",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "cartesian",
                                 .mcu = "hc32f460", // lowercase
                                 .mcu_list = {"hc32f460"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // Should still match Anycubic despite lowercase MCU
    bool is_anycubic = result.type_name.find("Anycubic") != std::string::npos ||
                       result.type_name.find("Kobra") != std::string::npos;
    REQUIRE(is_anycubic);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: MCU match case insensitive - gd32f303",
                 "[printer_detector][mcu][case_sensitivity]") {
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "flsun",
                                 .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                 .printer_objects = {"delta_calibrate"},
                                 .kinematics = "delta",
                                 .mcu = "gd32f303xx", // lowercase with suffix
                                 .mcu_list = {"gd32f303xx"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // Should match FLSUN despite lowercase/suffix
    bool is_flsun = result.type_name.find("FLSUN") != std::string::npos;
    REQUIRE(is_flsun);
}

// ============================================================================
// Combined Heuristics - MCU + Other Evidence
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: Combined - Anycubic Kobra 2 full fingerprint",
                 "[printer_detector][combined][anycubic]") {
    // Full Anycubic Kobra 2 setup with all data sources
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor mcu_temp"},
        .fans = {"fan", "heater_fan hotend_fan"},
        .leds = {},
        .hostname = "anycubic-kobra-2",
        .steppers = {"stepper_x", "stepper_y", "stepper_z"},
        .printer_objects = {},
        .kinematics = "cartesian",
        .build_volume = {.x_min = 0, .x_max = 220, .y_min = 0, .y_max = 220, .z_max = 250},
        .mcu = "HC32F460PETB",
        .mcu_list = {"HC32F460PETB"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Anycubic Kobra 2");
    REQUIRE(result.confidence >= 85);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Combined - FLSUN V400 full fingerprint",
                 "[printer_detector][combined][flsun]") {
    // Full FLSUN V400 setup with all data sources
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {},
        .fans = {"fan", "heater_fan hotend_fan"},
        .leds = {},
        .hostname = "flsun-v400-delta",
        .steppers = {"stepper_a", "stepper_b", "stepper_c"},
        .printer_objects = {"delta_calibrate", "bed_mesh"},
        .kinematics = "delta",
        .build_volume = {.x_min = -150, .x_max = 150, .y_min = -150, .y_max = 150, .z_max = 400},
        .mcu = "GD32F303RET6",
        .mcu_list = {"GD32F303RET6"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "FLSUN V400");
    // Delta + hostname + MCU + objects = very high confidence
    REQUIRE(result.confidence >= 90);
}

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: Combined - Qidi Plus 4 full fingerprint",
                 "[printer_detector][combined][qidi]") {
    // Full Qidi Plus 4 setup
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed", "heater_chamber"},
        .sensors = {"temperature_sensor chamber"},
        .fans = {"fan", "chamber_fan", "auxiliary_fan"},
        .leds = {"neopixel chamber_light"},
        .hostname = "qidi-plus-4",
        .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1"},
        .printer_objects = {"z_tilt"},
        .kinematics = "corexy",
        .build_volume = {.x_min = 0, .x_max = 305, .y_min = 0, .y_max = 305, .z_max = 305},
        .mcu = "STM32F402",
        .mcu_list = {"STM32F402", "rp2040"} // Main + toolhead
    };

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Qidi Plus 4");
    REQUIRE(result.confidence >= 85);
}

// ============================================================================
// Negative Tests - MCU Should Not Cause False Positives
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: MCU alone should not override strong hostname match",
                 "[printer_detector][mcu][negative]") {
    // Voron with Anycubic MCU (user swapped board) - hostname should win
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {"temperature_sensor chamber"},
                                 .fans = {"bed_fans", "exhaust_fan"},
                                 .leds = {"neopixel chamber_leds"},
                                 .hostname = "voron-2-4-350",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z", "stepper_z1",
                                              "stepper_z2", "stepper_z3"},
                                 .printer_objects = {"quad_gantry_level"},
                                 .kinematics = "corexy",
                                 .mcu = "HC32F460", // Anycubic MCU in Voron (unusual)
                                 .mcu_list = {"HC32F460"}};

    auto result = PrinterDetector::detect(hardware);

    REQUIRE(result.detected());
    // Strong Voron evidence (QGL + 4Z + corexy + hostname) should override MCU
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.confidence >= 95);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: Common MCU should not cause false positive",
                 "[printer_detector][mcu][negative]") {
    // STM32F103 is very common, should not trigger detection alone
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "test-printer-123",
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                 .printer_objects = {},
                                 .kinematics = "cartesian",
                                 .mcu = "STM32F103", // Very common, low confidence
                                 .mcu_list = {"STM32F103"}};

    auto result = PrinterDetector::detect(hardware);

    // STM32F103 at 25% confidence alone should NOT trigger detection
    // (unless database has 25% as threshold, which it shouldn't)
    if (result.detected()) {
        // If detected, confidence should be from other sources, not just MCU
        REQUIRE(result.confidence >= 50);
    }
}
