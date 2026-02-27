// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_discovery.cpp
 * @brief TEST-FIRST: Failing tests for PrinterDiscovery class
 *
 * This file contains tests for the NEW PrinterDiscovery class that will
 * consolidate all hardware discovery into a single source of truth:
 * - Hardware lists (heaters, fans, sensors, leds, steppers)
 * - Capability flags (has_qgl, has_probe, etc.)
 * - Macros (from gcode_macro objects)
 * - AMS/MMU detection (AFC, Happy Hare, tool changers)
 *
 * These tests are designed to FAIL until the class is implemented.
 */

#include "../catch_amalgamated.hpp"

// This include will fail until the header is created - that's expected!
#include "printer_discovery.h"

#include <json.hpp> // nlohmann/json from libhv

using json = nlohmann::json;
using namespace helix;

// ============================================================================
// Empty Input Tests
// ============================================================================

TEST_CASE("PrinterDiscovery parses empty objects list", "[printer_discovery]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array());

    // All lists should be empty
    REQUIRE(hw.heaters().empty());
    REQUIRE(hw.fans().empty());
    REQUIRE(hw.sensors().empty());
    REQUIRE(hw.leds().empty());
    REQUIRE(hw.steppers().empty());

    // All capability flags should be false
    REQUIRE_FALSE(hw.has_qgl());
    REQUIRE_FALSE(hw.has_z_tilt());
    REQUIRE_FALSE(hw.has_bed_mesh());
    REQUIRE_FALSE(hw.has_probe());
    REQUIRE_FALSE(hw.has_heater_bed());
    REQUIRE_FALSE(hw.has_mmu());

    // Macro-related
    REQUIRE(hw.macros().empty());
    REQUIRE(hw.nozzle_clean_macro().empty());
}

TEST_CASE("PrinterDiscovery handles malformed input", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Non-array input - object") {
        hw.parse_objects(json::object());
        REQUIRE(hw.heaters().empty());
    }

    SECTION("Non-array input - null") {
        hw.parse_objects(nullptr);
        REQUIRE(hw.heaters().empty());
    }

    SECTION("Array with non-string elements") {
        json objects = {1, "extruder", nullptr, true, "heater_bed"};
        hw.parse_objects(objects);
        REQUIRE(hw.heaters().size() == 2);
    }

    SECTION("Empty string in array") {
        json objects = {"extruder", "", "heater_bed"};
        hw.parse_objects(objects);
        REQUIRE(hw.heaters().size() == 2);
    }
}

// ============================================================================
// Heater Extraction Tests
// ============================================================================

TEST_CASE("PrinterDiscovery parses heaters - extruders and bed", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Single extruder and heater_bed") {
        json objects = {"extruder", "heater_bed"};
        hw.parse_objects(objects);

        REQUIRE(hw.heaters().size() == 2);
        REQUIRE(std::find(hw.heaters().begin(), hw.heaters().end(), "extruder") !=
                hw.heaters().end());
        REQUIRE(std::find(hw.heaters().begin(), hw.heaters().end(), "heater_bed") !=
                hw.heaters().end());
        REQUIRE(hw.has_heater_bed());
    }

    SECTION("Multiple extruders") {
        json objects = {"extruder", "extruder1", "extruder2", "heater_bed"};
        hw.parse_objects(objects);

        REQUIRE(hw.heaters().size() == 4);
        REQUIRE(std::find(hw.heaters().begin(), hw.heaters().end(), "extruder1") !=
                hw.heaters().end());
        REQUIRE(std::find(hw.heaters().begin(), hw.heaters().end(), "extruder2") !=
                hw.heaters().end());
    }

    SECTION("Generic heaters with chamber") {
        json objects = {"extruder", "heater_bed", "heater_generic chamber"};
        hw.parse_objects(objects);

        REQUIRE(hw.heaters().size() == 3);
        REQUIRE(std::find(hw.heaters().begin(), hw.heaters().end(), "heater_generic chamber") !=
                hw.heaters().end());
    }

    SECTION("Excludes extruder_stepper from heaters") {
        json objects = {"extruder", "extruder_stepper filament", "heater_bed"};
        hw.parse_objects(objects);

        // extruder_stepper should NOT be in heaters list
        REQUIRE(hw.heaters().size() == 2);
        REQUIRE(std::find(hw.heaters().begin(), hw.heaters().end(), "extruder_stepper filament") ==
                hw.heaters().end());
    }
}

// ============================================================================
// Fan Extraction Tests
// ============================================================================

TEST_CASE("PrinterDiscovery parses fans - all fan types", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Part cooling fan (canonical 'fan')") {
        json objects = {"fan"};
        hw.parse_objects(objects);

        REQUIRE(hw.fans().size() == 1);
        REQUIRE(hw.fans()[0] == "fan");
    }

    SECTION("Heater fan") {
        json objects = {"heater_fan hotend"};
        hw.parse_objects(objects);

        REQUIRE(hw.fans().size() == 1);
        REQUIRE(hw.fans()[0] == "heater_fan hotend");
    }

    SECTION("Generic fan") {
        json objects = {"fan_generic aux"};
        hw.parse_objects(objects);

        REQUIRE(hw.fans().size() == 1);
        REQUIRE(hw.fans()[0] == "fan_generic aux");
    }

    SECTION("Controller fan") {
        json objects = {"controller_fan electronics"};
        hw.parse_objects(objects);

        REQUIRE(hw.fans().size() == 1);
        REQUIRE(hw.fans()[0] == "controller_fan electronics");
    }

    SECTION("Temperature fan (acts as both sensor and fan)") {
        json objects = {"temperature_fan exhaust"};
        hw.parse_objects(objects);

        REQUIRE(hw.fans().size() == 1);
        REQUIRE(hw.fans()[0] == "temperature_fan exhaust");
        // Should also be in sensors
        REQUIRE(hw.sensors().size() == 1);
        REQUIRE(hw.sensors()[0] == "temperature_fan exhaust");
    }

    SECTION("All fan types together") {
        json objects = {"fan", "heater_fan hotend", "fan_generic aux",
                        "controller_fan electronics"};
        hw.parse_objects(objects);

        REQUIRE(hw.fans().size() == 4);
    }
}

// ============================================================================
// Sensor Extraction Tests
// ============================================================================

TEST_CASE("PrinterDiscovery parses sensors - temperature sensors", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Temperature sensor") {
        json objects = {"temperature_sensor chamber"};
        hw.parse_objects(objects);

        REQUIRE(hw.sensors().size() == 1);
        REQUIRE(hw.sensors()[0] == "temperature_sensor chamber");
    }

    SECTION("Temperature fan (dual-purpose)") {
        json objects = {"temperature_fan exhaust"};
        hw.parse_objects(objects);

        // Should appear in both sensors and fans
        REQUIRE(hw.sensors().size() == 1);
        REQUIRE(hw.sensors()[0] == "temperature_fan exhaust");
        REQUIRE(hw.fans().size() == 1);
    }

    SECTION("Multiple sensors") {
        json objects = {"temperature_sensor chamber", "temperature_sensor raspberry_pi",
                        "temperature_sensor mcu_temp"};
        hw.parse_objects(objects);

        REQUIRE(hw.sensors().size() == 3);
    }
}

// ============================================================================
// LED Extraction Tests
// ============================================================================

TEST_CASE("PrinterDiscovery parses LEDs - neopixel and dotstar", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Neopixel strip") {
        json objects = {"neopixel strip"};
        hw.parse_objects(objects);

        REQUIRE(hw.leds().size() == 1);
        REQUIRE(hw.leds()[0] == "neopixel strip");
    }

    SECTION("Dotstar") {
        json objects = {"dotstar"};
        hw.parse_objects(objects);

        REQUIRE(hw.leds().size() == 1);
        REQUIRE(hw.leds()[0] == "dotstar");
    }

    SECTION("LED indicator") {
        json objects = {"led indicator"};
        hw.parse_objects(objects);

        REQUIRE(hw.leds().size() == 1);
        REQUIRE(hw.leds()[0] == "led indicator");
    }

    SECTION("Multiple LED types") {
        json objects = {"neopixel case_lights", "dotstar toolhead", "led status"};
        hw.parse_objects(objects);

        REQUIRE(hw.leds().size() == 3);
    }
}

// ============================================================================
// Capability Detection Tests - Leveling
// ============================================================================

TEST_CASE("PrinterDiscovery detects QGL when quad_gantry_level present", "[printer_discovery]") {
    PrinterDiscovery hw;

    json objects = {"extruder", "heater_bed", "quad_gantry_level", "bed_mesh"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_qgl());
    REQUIRE(hw.has_bed_mesh());
    REQUIRE(hw.supports_leveling());
}

TEST_CASE("PrinterDiscovery detects z_tilt", "[printer_discovery]") {
    PrinterDiscovery hw;

    json objects = {"extruder", "heater_bed", "z_tilt"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_z_tilt());
    REQUIRE_FALSE(hw.has_qgl());
    REQUIRE(hw.supports_leveling());
}

// ============================================================================
// Capability Detection Tests - Probes
// ============================================================================

TEST_CASE("PrinterDiscovery detects probe when bltouch present", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("BLTouch probe") {
        json objects = {"extruder", "heater_bed", "bltouch"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_probe());
    }

    SECTION("Standard probe") {
        json objects = {"extruder", "heater_bed", "probe"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_probe());
    }

    SECTION("Eddy current probe") {
        json objects = {"extruder", "heater_bed", "probe_eddy_current btt_eddy"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_probe());
    }
}

// ============================================================================
// Macro Detection Tests
// ============================================================================

TEST_CASE("PrinterDiscovery detects macros and caches common patterns", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Nozzle clean macro - CLEAN_NOZZLE") {
        json objects = {"gcode_macro CLEAN_NOZZLE", "gcode_macro PRINT_START"};
        hw.parse_objects(objects);

        REQUIRE(hw.macros().size() == 2);
        REQUIRE(hw.has_macro("CLEAN_NOZZLE"));
        REQUIRE(hw.nozzle_clean_macro() == "CLEAN_NOZZLE");
    }

    SECTION("Nozzle clean macro - NOZZLE_WIPE variant") {
        json objects = {"gcode_macro NOZZLE_WIPE"};
        hw.parse_objects(objects);

        REQUIRE(hw.nozzle_clean_macro() == "NOZZLE_WIPE");
    }

    SECTION("Purge line macro") {
        json objects = {"gcode_macro PURGE_LINE"};
        hw.parse_objects(objects);

        REQUIRE(hw.purge_line_macro() == "PURGE_LINE");
    }

    SECTION("Heat soak macro") {
        json objects = {"gcode_macro HEAT_SOAK"};
        hw.parse_objects(objects);

        REQUIRE(hw.heat_soak_macro() == "HEAT_SOAK");
    }

    SECTION("Case-insensitive macro lookup") {
        json objects = {"gcode_macro CLEAN_NOZZLE"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_macro("CLEAN_NOZZLE"));
        REQUIRE(hw.has_macro("clean_nozzle"));
        REQUIRE(hw.has_macro("Clean_Nozzle"));
    }
}

// ============================================================================
// AFC/MMU Detection Tests
// ============================================================================

TEST_CASE("PrinterDiscovery detects AFC and extracts lane names", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("AFC detection") {
        json objects = {"AFC", "AFC_stepper lane1", "AFC_stepper lane2", "AFC_stepper lane3",
                        "AFC_stepper lane4"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_mmu());
        REQUIRE(hw.mmu_type() == AmsType::AFC);
    }

    SECTION("AFC lane name extraction") {
        json objects = {"AFC", "AFC_stepper lane1", "AFC_stepper lane2"};
        hw.parse_objects(objects);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 2);
        REQUIRE(std::find(lanes.begin(), lanes.end(), "lane1") != lanes.end());
        REQUIRE(std::find(lanes.begin(), lanes.end(), "lane2") != lanes.end());
    }

    SECTION("AFC hub name extraction") {
        json objects = {"AFC", "AFC_hub Turtle_1", "AFC_stepper lane1"};
        hw.parse_objects(objects);

        auto hubs = hw.afc_hub_names();
        REQUIRE(hubs.size() == 1);
        REQUIRE(hubs[0] == "Turtle_1");
    }
}

TEST_CASE("PrinterDiscovery detects new AFC object types", "[printer_discovery][afc]") {
    PrinterDiscovery hw;

    SECTION("AFC_lane detected and added to afc_lane_names") {
        json objects = {"AFC", "AFC_lane lane4", "AFC_lane lane5", "AFC_lane lane6",
                        "AFC_lane lane7"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_mmu());
        REQUIRE(hw.mmu_type() == AmsType::AFC);
        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 4);
        // Lane names should be just the suffix: "lane4", "lane5", etc.
        REQUIRE(std::find(lanes.begin(), lanes.end(), "lane4") != lanes.end());
        REQUIRE(std::find(lanes.begin(), lanes.end(), "lane7") != lanes.end());
    }

    SECTION("AFC_lane and AFC_stepper both populate afc_lane_names") {
        json objects = {"AFC",
                        "AFC_stepper lane0",
                        "AFC_stepper lane1",
                        "AFC_stepper lane2",
                        "AFC_stepper lane3",
                        "AFC_lane lane4",
                        "AFC_lane lane5",
                        "AFC_lane lane6",
                        "AFC_lane lane7"};
        hw.parse_objects(objects);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 8); // Both types in same vector
    }

    SECTION("AFC_BoxTurtle detected in afc_unit_object_names") {
        json objects = {"AFC", "AFC_BoxTurtle Turtle_1"};
        hw.parse_objects(objects);

        auto units = hw.afc_unit_object_names();
        REQUIRE(units.size() == 1);
        REQUIRE(units[0] == "AFC_BoxTurtle Turtle_1"); // Full Klipper object name
    }

    SECTION("AFC_OpenAMS detected in afc_unit_object_names") {
        json objects = {"AFC", "AFC_OpenAMS AMS_1", "AFC_OpenAMS AMS_2"};
        hw.parse_objects(objects);

        auto units = hw.afc_unit_object_names();
        REQUIRE(units.size() == 2);
        REQUIRE(units[0] == "AFC_OpenAMS AMS_1");
        REQUIRE(units[1] == "AFC_OpenAMS AMS_2");
    }

    SECTION("AFC_buffer detected in afc_buffer_names") {
        json objects = {"AFC", "AFC_buffer TN", "AFC_buffer TN1", "AFC_buffer TN2",
                        "AFC_buffer TN3"};
        hw.parse_objects(objects);

        auto buffers = hw.afc_buffer_names();
        REQUIRE(buffers.size() == 4);
        REQUIRE(std::find(buffers.begin(), buffers.end(), "TN") != buffers.end());
    }

    SECTION("Existing AFC_stepper detection unchanged") {
        json objects = {"AFC", "AFC_stepper lane1", "AFC_stepper lane2"};
        hw.parse_objects(objects);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 2);
        REQUIRE(lanes[0] == "lane1"); // sorted
        REQUIRE(lanes[1] == "lane2");
    }

    SECTION("Natural sort: lane10 comes after lane9, not before lane2") {
        json objects = {"AFC",
                        "AFC_stepper lane0",
                        "AFC_stepper lane1",
                        "AFC_stepper lane2",
                        "AFC_stepper lane3",
                        "AFC_lane lane4",
                        "AFC_lane lane5",
                        "AFC_lane lane6",
                        "AFC_lane lane7",
                        "AFC_lane lane8",
                        "AFC_lane lane9",
                        "AFC_lane lane10",
                        "AFC_lane lane11"};
        hw.parse_objects(objects);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 12);
        // Verify natural ordering: lane0..lane11 (not alphabetical where lane10 < lane2)
        for (int i = 0; i < 12; ++i) {
            REQUIRE(lanes[i] == "lane" + std::to_string(i));
        }
    }

    SECTION("Natural sort: buffer names with numeric suffixes") {
        json objects = {"AFC", "AFC_buffer TN", "AFC_buffer TN2", "AFC_buffer TN1",
                        "AFC_buffer TN10"};
        hw.parse_objects(objects);

        auto buffers = hw.afc_buffer_names();
        REQUIRE(buffers.size() == 4);
        // "TN" has no trailing digits, then TN1, TN2, TN10
        REQUIRE(buffers[0] == "TN");
        REQUIRE(buffers[1] == "TN1");
        REQUIRE(buffers[2] == "TN2");
        REQUIRE(buffers[3] == "TN10");
    }

    SECTION("Mixed AFC hardware - full J0eB0l setup") {
        json objects = {"AFC",
                        "AFC_stepper lane0",
                        "AFC_stepper lane1",
                        "AFC_stepper lane2",
                        "AFC_stepper lane3",
                        "AFC_lane lane4",
                        "AFC_lane lane5",
                        "AFC_lane lane6",
                        "AFC_lane lane7",
                        "AFC_lane lane8",
                        "AFC_lane lane9",
                        "AFC_lane lane10",
                        "AFC_lane lane11",
                        "AFC_BoxTurtle Turtle_1",
                        "AFC_OpenAMS AMS_1",
                        "AFC_OpenAMS AMS_2",
                        "AFC_hub Hub_1",
                        "AFC_hub Hub_2",
                        "AFC_hub Hub_3",
                        "AFC_hub Hub_4",
                        "AFC_hub Hub_5",
                        "AFC_hub Hub_6",
                        "AFC_hub Hub_7",
                        "AFC_hub Hub_8",
                        "AFC_buffer TN",
                        "AFC_buffer TN1",
                        "AFC_buffer TN2",
                        "AFC_buffer TN3",
                        "AFC_extruder extruder",
                        "AFC_extruder extruder1",
                        "AFC_extruder extruder2",
                        "AFC_extruder extruder3",
                        "AFC_extruder extruder4",
                        "AFC_extruder extruder5"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_mmu());
        REQUIRE(hw.mmu_type() == AmsType::AFC);
        REQUIRE(hw.afc_lane_names().size() == 12);
        REQUIRE(hw.afc_unit_object_names().size() == 3);
        REQUIRE(hw.afc_hub_names().size() == 8);
        REQUIRE(hw.afc_buffer_names().size() == 4);
    }
}

TEST_CASE("PrinterDiscovery detects Happy Hare MMU", "[printer_discovery]") {
    PrinterDiscovery hw;

    json objects = {"mmu", "extruder", "heater_bed"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == AmsType::HAPPY_HARE);
}

TEST_CASE("PrinterDiscovery parses Happy Hare mmu_encoder objects", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Single encoder") {
        json objects = {"mmu", "mmu_encoder toolhead"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_mmu());
        REQUIRE(hw.mmu_type() == AmsType::HAPPY_HARE);

        auto encoders = hw.mmu_encoder_names();
        REQUIRE(encoders.size() == 1);
        REQUIRE(encoders[0] == "toolhead");
    }

    SECTION("Multiple encoders") {
        json objects = {"mmu", "mmu_encoder toolhead", "mmu_encoder gate"};
        hw.parse_objects(objects);

        auto encoders = hw.mmu_encoder_names();
        REQUIRE(encoders.size() == 2);
        REQUIRE(std::find(encoders.begin(), encoders.end(), "toolhead") != encoders.end());
        REQUIRE(std::find(encoders.begin(), encoders.end(), "gate") != encoders.end());
    }

    SECTION("Encoder without mmu object still detected") {
        // Edge case: encoder object exists but main mmu object missing
        json objects = {"mmu_encoder toolhead"};
        hw.parse_objects(objects);

        auto encoders = hw.mmu_encoder_names();
        REQUIRE(encoders.size() == 1);
        REQUIRE(encoders[0] == "toolhead");
    }
}

TEST_CASE("PrinterDiscovery parses Happy Hare mmu_servo objects", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Single servo") {
        json objects = {"mmu", "mmu_servo gate"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_mmu());
        REQUIRE(hw.mmu_type() == AmsType::HAPPY_HARE);

        auto servos = hw.mmu_servo_names();
        REQUIRE(servos.size() == 1);
        REQUIRE(servos[0] == "gate");
    }

    SECTION("Multiple servos") {
        json objects = {"mmu", "mmu_servo gate", "mmu_servo selector"};
        hw.parse_objects(objects);

        auto servos = hw.mmu_servo_names();
        REQUIRE(servos.size() == 2);
        REQUIRE(std::find(servos.begin(), servos.end(), "gate") != servos.end());
        REQUIRE(std::find(servos.begin(), servos.end(), "selector") != servos.end());
    }

    SECTION("Servo without mmu object still detected") {
        // Edge case: servo object exists but main mmu object missing
        json objects = {"mmu_servo gate"};
        hw.parse_objects(objects);

        auto servos = hw.mmu_servo_names();
        REQUIRE(servos.size() == 1);
        REQUIRE(servos[0] == "gate");
    }
}

TEST_CASE("PrinterDiscovery parses full Happy Hare configuration", "[printer_discovery]") {
    PrinterDiscovery hw;

    // Typical Happy Hare setup with multiple encoders and servos
    json objects = {
        "mmu",       "mmu_encoder toolhead", "mmu_encoder gate", "mmu_servo gate", "extruder",
        "heater_bed"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == AmsType::HAPPY_HARE);

    auto encoders = hw.mmu_encoder_names();
    REQUIRE(encoders.size() == 2);

    auto servos = hw.mmu_servo_names();
    REQUIRE(servos.size() == 1);
    REQUIRE(servos[0] == "gate");
}

TEST_CASE("PrinterDiscovery detects tool changer", "[printer_discovery]") {
    PrinterDiscovery hw;

    json objects = {"toolchanger", "tool T0", "tool T1", "tool T2"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_tool_changer());
    REQUIRE(hw.mmu_type() == AmsType::TOOL_CHANGER);

    auto tools = hw.tool_names();
    REQUIRE(tools.size() == 3);
}

// ============================================================================
// Filament Sensor Detection Tests
// ============================================================================

TEST_CASE("PrinterDiscovery detects filament sensors - both types", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Switch sensor") {
        json objects = {"filament_switch_sensor fsensor"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_filament_sensors());
        REQUIRE(hw.filament_sensor_names().size() == 1);
        REQUIRE(hw.filament_sensor_names()[0] == "filament_switch_sensor fsensor");
    }

    SECTION("Motion sensor") {
        json objects = {"filament_motion_sensor encoder"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_filament_sensors());
        REQUIRE(hw.filament_sensor_names().size() == 1);
    }

    SECTION("Both sensor types") {
        json objects = {"filament_switch_sensor runout", "filament_motion_sensor encoder"};
        hw.parse_objects(objects);

        REQUIRE(hw.filament_sensor_names().size() == 2);
    }
}

// ============================================================================
// Stepper Extraction Tests
// ============================================================================

TEST_CASE("PrinterDiscovery parses steppers", "[printer_discovery]") {
    PrinterDiscovery hw;

    json objects = {"stepper_x",  "stepper_y",  "stepper_z",
                    "stepper_z1", "stepper_z2", "stepper_z3"};
    hw.parse_objects(objects);

    REQUIRE(hw.steppers().size() == 6);
    REQUIRE(std::find(hw.steppers().begin(), hw.steppers().end(), "stepper_x") !=
            hw.steppers().end());
    REQUIRE(std::find(hw.steppers().begin(), hw.steppers().end(), "stepper_z3") !=
            hw.steppers().end());
}

// ============================================================================
// Additional Capability Detection Tests
// ============================================================================

// ============================================================================
// Accelerometer Detection Tests
// ============================================================================
// NOTE: Klipper's objects/list ONLY returns objects with get_status() method.
// Accelerometers (adxl345, lis2dw, mpu9250, resonance_tester) intentionally
// don't have get_status() since they're on-demand calibration tools.
// Therefore: accelerometer detection MUST use parse_config_keys(), not parse_objects().

TEST_CASE("PrinterDiscovery parse_objects ignores accelerometer names", "[printer_discovery]") {
    // These objects will NEVER appear in Klipper's objects/list response anyway,
    // but we verify parse_objects() doesn't try to detect them
    PrinterDiscovery hw;

    SECTION("adxl345 in objects list does not set accelerometer flag") {
        json objects = {"adxl345", "extruder", "heater_bed"};
        hw.parse_objects(objects);
        REQUIRE_FALSE(hw.has_accelerometer());
    }

    SECTION("resonance_tester in objects list does not set accelerometer flag") {
        json objects = {"resonance_tester", "extruder"};
        hw.parse_objects(objects);
        REQUIRE_FALSE(hw.has_accelerometer());
    }

    SECTION("named adxl345 in objects list does not set accelerometer flag") {
        json objects = {"adxl345 bed", "extruder"};
        hw.parse_objects(objects);
        REQUIRE_FALSE(hw.has_accelerometer());
    }
}

TEST_CASE("PrinterDiscovery detects accelerometers from config keys", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("detects adxl345") {
        json config = {{"adxl345", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects named adxl345") {
        json config = {{"adxl345 bed", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects lis2dw") {
        json config = {{"lis2dw", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects named lis2dw") {
        json config = {{"lis2dw toolhead", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects mpu9250") {
        json config = {{"mpu9250", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects resonance_tester") {
        json config = {{"resonance_tester", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("does not detect unrelated config keys") {
        json config = {{"extruder", json::object()}, {"heater_bed", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE_FALSE(hw.has_accelerometer());
    }

    SECTION("handles non-object input gracefully") {
        hw.parse_config_keys(json::array());
        REQUIRE_FALSE(hw.has_accelerometer());

        hw.parse_config_keys(nullptr);
        REQUIRE_FALSE(hw.has_accelerometer());
    }
}

TEST_CASE("PrinterDiscovery detects LED capability", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Neopixel triggers has_led") {
        json objects = {"neopixel case_lights"};
        hw.parse_objects(objects);
        REQUIRE(hw.has_led());
    }

    SECTION("Output pin with LED in name triggers has_led") {
        json objects = {"output_pin case_led"};
        hw.parse_objects(objects);
        REQUIRE(hw.has_led());
    }

    SECTION("Output pin with LIGHT in name triggers has_led") {
        json objects = {"output_pin chamber_light"};
        hw.parse_objects(objects);
        REQUIRE(hw.has_led());
    }

    SECTION("Output pin without LED/LIGHT keywords does NOT trigger has_led") {
        json objects = {"output_pin part_fan_boost", "output_pin power_relay"};
        hw.parse_objects(objects);
        REQUIRE_FALSE(hw.has_led());
    }
}

TEST_CASE("PrinterDiscovery detects firmware retraction", "[printer_discovery]") {
    PrinterDiscovery hw;

    json objects = {"extruder", "firmware_retraction"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_firmware_retraction());
}

TEST_CASE("PrinterDiscovery detects timelapse plugin", "[printer_discovery]") {
    PrinterDiscovery hw;

    json objects = {"extruder", "timelapse"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_timelapse());
}

TEST_CASE("PrinterDiscovery detects chamber heater and sensor", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("Chamber heater") {
        json objects = {"heater_generic chamber"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
        REQUIRE(hw.supports_chamber());
    }

    SECTION("Chamber sensor") {
        json objects = {"temperature_sensor chamber"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.supports_chamber());
    }

    SECTION("Both chamber heater and sensor") {
        json objects = {"heater_generic chamber", "temperature_sensor chamber_temp"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
        REQUIRE(hw.has_chamber_sensor());
    }
}

// ============================================================================
// Clear/Reset Tests
// ============================================================================

TEST_CASE("PrinterDiscovery clear resets all state", "[printer_discovery]") {
    PrinterDiscovery hw;

    // First populate with data
    json objects = {"extruder", "heater_bed",      "quad_gantry_level", "bed_mesh",
                    "probe",    "neopixel lights", "gcode_macro FOO"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_qgl());
    REQUIRE(hw.heaters().size() == 2);

    // Clear should reset everything
    hw.clear();

    REQUIRE_FALSE(hw.has_qgl());
    REQUIRE_FALSE(hw.has_bed_mesh());
    REQUIRE_FALSE(hw.has_probe());
    REQUIRE(hw.heaters().empty());
    REQUIRE(hw.leds().empty());
    REQUIRE(hw.macros().empty());
}

// ============================================================================
// Real-world Configuration Tests
// ============================================================================

TEST_CASE("PrinterDiscovery handles full Voron 2.4 config", "[printer_discovery]") {
    PrinterDiscovery hw;

    json objects = {"configfile",
                    "mcu",
                    "mcu EBBCan",
                    "stepper_x",
                    "stepper_y",
                    "stepper_z",
                    "stepper_z1",
                    "stepper_z2",
                    "stepper_z3",
                    "extruder",
                    "heater_bed",
                    "heater_generic chamber",
                    "temperature_sensor chamber",
                    "temperature_sensor raspberry_pi",
                    "fan",
                    "heater_fan hotend_fan",
                    "controller_fan controller",
                    "neopixel status_led",
                    "probe",
                    "quad_gantry_level",
                    "bed_mesh",
                    "gcode_macro PRINT_START",
                    "gcode_macro CLEAN_NOZZLE"};
    hw.parse_objects(objects);

    // Hardware lists
    REQUIRE(hw.heaters().size() == 3);  // extruder, heater_bed, heater_generic chamber
    REQUIRE(hw.fans().size() == 3);     // fan, heater_fan, controller_fan
    REQUIRE(hw.sensors().size() == 2);  // temperature_sensor chamber, raspberry_pi
    REQUIRE(hw.leds().size() == 1);     // neopixel status_led
    REQUIRE(hw.steppers().size() == 6); // stepper_x,y,z,z1,z2,z3

    // Capabilities
    REQUIRE(hw.has_qgl());
    REQUIRE(hw.has_bed_mesh());
    REQUIRE(hw.has_probe());
    REQUIRE(hw.has_heater_bed());
    REQUIRE(hw.has_chamber_heater());
    REQUIRE(hw.has_chamber_sensor());
    REQUIRE(hw.has_led());

    // Macros
    REQUIRE(hw.macros().size() == 2);
    REQUIRE(hw.nozzle_clean_macro() == "CLEAN_NOZZLE");
}

// ============================================================================
// screws_tilt_adjust Detection Tests
// ============================================================================
// NOTE: screws_tilt_adjust doesn't implement get_status() in Klipper,
// so it typically won't appear in objects/list. Must detect from configfile.

TEST_CASE("PrinterDiscovery detects screws_tilt_adjust from objects list", "[printer_discovery]") {
    // Belt-and-suspenders: if a future Klipper version adds get_status(), this still works
    PrinterDiscovery hw;

    json objects = {"extruder", "heater_bed", "screws_tilt_adjust"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_screws_tilt());
}

TEST_CASE("PrinterDiscovery detects screws_tilt_adjust from config keys", "[printer_discovery]") {
    PrinterDiscovery hw;

    SECTION("screws_tilt_adjust present in config") {
        json config = {{"screws_tilt_adjust", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_screws_tilt());
    }

    SECTION("unrelated config keys do not trigger screws_tilt") {
        json config = {{"extruder", json::object()}, {"heater_bed", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE_FALSE(hw.has_screws_tilt());
    }
}

TEST_CASE("PrinterDiscovery detects screws_tilt_adjust from config when missing from objects",
          "[printer_discovery]") {
    // Real-world scenario: screws_tilt_adjust in configfile but NOT in objects/list
    PrinterDiscovery hw;

    // Objects list without screws_tilt_adjust (as happens on real printers)
    json objects = {"extruder", "heater_bed", "bed_mesh", "probe"};
    hw.parse_objects(objects);
    REQUIRE_FALSE(hw.has_screws_tilt());

    // Config keys include screws_tilt_adjust (the fallback path)
    json config = {{"screws_tilt_adjust", {{"screw1", "50, 50"}}},
                   {"extruder", json::object()},
                   {"printer", {{"kinematics", "corexy"}}}};
    hw.parse_config_keys(config);
    REQUIRE(hw.has_screws_tilt());
}
