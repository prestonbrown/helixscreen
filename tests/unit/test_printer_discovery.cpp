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

    SECTION("AFC_lane takes priority over AFC_stepper when both exist") {
        // Vivid firmware has AFC_stepper for motors (drive/selector) and AFC_lane for lanes.
        // Only AFC_lane should be used as lanes when both types exist.
        json objects = {"AFC",
                        "AFC_stepper Vivid_1_drive",
                        "AFC_stepper Vivid_1_selector",
                        "AFC_lane lane1",
                        "AFC_lane lane2",
                        "AFC_lane lane3",
                        "AFC_lane lane4"};
        hw.parse_objects(objects);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 4); // Only AFC_lane objects, not AFC_stepper motors
        CHECK(lanes[0] == "lane1");
        CHECK(lanes[1] == "lane2");
        CHECK(lanes[2] == "lane3");
        CHECK(lanes[3] == "lane4");
    }

    SECTION("AFC_stepper used as lanes when no AFC_lane objects exist") {
        // Box Turtle firmware only has AFC_stepper for lanes
        json objects = {"AFC", "AFC_stepper lane1", "AFC_stepper lane2", "AFC_stepper lane3",
                        "AFC_stepper lane4"};
        hw.parse_objects(objects);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 4);
        CHECK(lanes[0] == "lane1");
    }

    SECTION("Mixed AFC_stepper lanes merged with AFC_lane (Box Turtle + OpenAMS)") {
        // Box Turtle uses AFC_stepper for lanes, OpenAMS/ACE use AFC_lane.
        // When both exist, stepper names matching "lane\d+" should be merged.
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
        REQUIRE(lanes.size() == 8); // All 8 lanes merged
        // Natural sort: lane0-7
        CHECK(lanes[0] == "lane0");
        CHECK(lanes[3] == "lane3");
        CHECK(lanes[4] == "lane4");
        CHECK(lanes[7] == "lane7");
    }

    SECTION("Vivid motors not merged with AFC_lane objects") {
        // Vivid has AFC_stepper for motors (not lanes) + AFC_lane for actual lanes.
        // Motor names should NOT be merged since they don't match "lane" prefix.
        json objects = {"AFC", "AFC_stepper Vivid_1_drive", "AFC_stepper Vivid_1_selector",
                        "AFC_lane lane1", "AFC_lane lane2"};
        hw.parse_objects(objects);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 2); // Only AFC_lane, not motor steppers
        CHECK(lanes[0] == "lane1");
        CHECK(lanes[1] == "lane2");
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

    SECTION("AFC_vivid detected in afc_unit_object_names") {
        json objects = {"AFC", "AFC_vivid vivid_1"};
        hw.parse_objects(objects);

        auto units = hw.afc_unit_object_names();
        REQUIRE(units.size() == 1);
        REQUIRE(units[0] == "AFC_vivid vivid_1"); // Full Klipper object name (lowercase)
    }

    SECTION("AFC_buffer detected in afc_buffer_names") {
        json objects = {"AFC", "AFC_buffer TN", "AFC_buffer TN1", "AFC_buffer TN2",
                        "AFC_buffer TN3"};
        hw.parse_objects(objects);

        auto buffers = hw.afc_buffer_names();
        REQUIRE(buffers.size() == 4);
        REQUIRE(std::find(buffers.begin(), buffers.end(), "TN") != buffers.end());
    }

    SECTION("AFC_stepper only (Box Turtle compat)") {
        json objects = {"AFC", "AFC_stepper lane1", "AFC_stepper lane2"};
        hw.parse_objects(objects);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 2);
        REQUIRE(lanes[0] == "lane1"); // sorted
        REQUIRE(lanes[1] == "lane2");
    }

    SECTION("Vivid: AFC_stepper motors not counted as lanes") {
        // Exact Klipper objects from a real Vivid setup.
        // AFC_stepper objects are drive/selector motors, not lanes.
        json objects = {"AFC",
                        "AFC_vivid Vivid_1",
                        "AFC_stepper Vivid_1_drive",
                        "AFC_stepper Vivid_1_selector",
                        "AFC_lane lane1",
                        "AFC_lane lane2",
                        "AFC_lane lane3",
                        "AFC_lane lane4",
                        "AFC_hub Vivid_1",
                        "AFC_buffer Vivid_1_buffer",
                        "AFC_extruder extruder",
                        "AFC_led AFC_Indicator_1",
                        "AFC_led AFC_Indicator_2"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_mmu());
        REQUIRE(hw.mmu_type() == AmsType::AFC);

        auto lanes = hw.afc_lane_names();
        REQUIRE(lanes.size() == 4);
        CHECK(lanes[0] == "lane1");
        CHECK(lanes[1] == "lane2");
        CHECK(lanes[2] == "lane3");
        CHECK(lanes[3] == "lane4");

        CHECK(hw.afc_hub_names().size() == 1);
        CHECK(hw.afc_unit_object_names().size() == 1);
        CHECK(hw.afc_unit_object_names()[0] == "AFC_vivid Vivid_1");
    }

    SECTION("Natural sort: lane10 comes after lane9, not before lane2") {
        json objects = {"AFC",
                        "AFC_lane lane0",
                        "AFC_lane lane1",
                        "AFC_lane lane2",
                        "AFC_lane lane3",
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
        // Box Turtle uses AFC_stepper for lanes, OpenAMS uses AFC_lane.
        // When both exist, only AFC_lane objects count as lanes.
        json objects = {"AFC",
                        "AFC_stepper Turtle_1_stepper",
                        "AFC_lane lane0",
                        "AFC_lane lane1",
                        "AFC_lane lane2",
                        "AFC_lane lane3",
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

TEST_CASE("PrinterDiscovery prefers AFC over Snapmaker when both present", "[printer_discovery]") {
    // SnapMaker U1 with aftermarket AFC filament changers — filament_detect
    // (native Snapmaker) is present but AFC should win since it provides
    // real filament management with lanes and hubs.
    PrinterDiscovery hw;

    json objects = {"filament_detect",
                    "toolchanger",
                    "tool T0",
                    "tool T1",
                    "tool T2",
                    "tool T3",
                    "AFC",
                    "AFC_hub Turtle",
                    "AFC_hub Vivid",
                    "AFC_hub EMU",
                    "AFC_hub HTLF Claymore",
                    "AFC_lane lane1",
                    "AFC_lane lane2",
                    "AFC_lane lane3",
                    "AFC_lane lane4",
                    "AFC_lane lane5",
                    "AFC_lane lane6",
                    "AFC_lane lane7",
                    "AFC_lane lane8"};
    hw.parse_objects(objects);

    // AFC should win over Snapmaker
    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == AmsType::AFC);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    REQUIRE(hw.detected_ams_systems()[0].type == AmsType::AFC);

    // Snapmaker and toolchanger flags still set for other queries
    REQUIRE(hw.has_snapmaker());
    REQUIRE(hw.has_tool_changer());

    // AFC discovery data preserved
    REQUIRE(hw.afc_hub_names().size() == 4);
    REQUIRE(hw.afc_lane_names().size() == 8);
    REQUIRE(hw.tool_names().size() == 4);
}

TEST_CASE("PrinterDiscovery prefers AFC over standalone toolchanger", "[printer_discovery]") {
    // Generic toolchanger with AFC — AFC should win
    PrinterDiscovery hw;

    json objects = {"toolchanger",    "tool T0",        "tool T1",        "AFC",
                    "AFC_hub Turtle", "AFC_lane lane1", "AFC_lane lane2", "AFC_lane lane3",
                    "AFC_lane lane4"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == AmsType::AFC);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    REQUIRE(hw.detected_ams_systems()[0].type == AmsType::AFC);
    REQUIRE(hw.has_tool_changer());
}

TEST_CASE("PrinterDiscovery uses Snapmaker backend when no MMU present", "[printer_discovery]") {
    // Native Snapmaker U1 without aftermarket filament changer
    PrinterDiscovery hw;

    json objects = {"filament_detect", "toolchanger", "tool T0", "tool T1", "tool T2", "tool T3"};
    hw.parse_objects(objects);

    REQUIRE(hw.has_snapmaker());
    REQUIRE(hw.mmu_type() == AmsType::SNAPMAKER);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    REQUIRE(hw.detected_ams_systems()[0].type == AmsType::SNAPMAKER);
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

    SECTION("detects lis3dh") {
        json config = {{"lis3dh", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects named lis3dh") {
        json config = {{"lis3dh toolhead", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects icm20948") {
        json config = {{"icm20948", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects named icm20948") {
        json config = {{"icm20948 bed", json::object()}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects Beacon onboard accelerometer via accel_scale") {
        json config = {{"beacon", {{"accel_scale", "16g"}}}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects Beacon onboard accelerometer via accel_axes_map") {
        json config = {{"beacon", {{"accel_axes_map", "x,y,z"}}}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("beacon without accel fields does not set accelerometer flag") {
        json config = {{"beacon", {{"serial", "/dev/serial/by-id/usb-beacon"}}}};
        hw.parse_config_keys(config);
        REQUIRE_FALSE(hw.has_accelerometer());
    }

    SECTION("detects beacon accelerometer via resonance_tester accel_chip") {
        json config = {{"resonance_tester", {{"accel_chip", "beacon"}}}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects beacon accelerometer via resonance_tester accel_chip_x") {
        json config = {{"resonance_tester", {{"accel_chip_x", "beacon"}}}};
        hw.parse_config_keys(config);
        REQUIRE(hw.has_accelerometer());
    }

    SECTION("detects beacon accelerometer via resonance_tester accel_chip_y") {
        json config = {{"resonance_tester", {{"accel_chip_y", "beacon"}}}};
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

    SECTION("Cavity sensor is treated as chamber (Snapmaker U1)") {
        json objects = {"temperature_sensor cavity"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.supports_chamber());
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor cavity");
    }

    SECTION("Cavity heater_generic is treated as chamber") {
        json objects = {"heater_generic cavity"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
        REQUIRE(hw.supports_chamber());
    }

    SECTION("Cavity temperature_fan is treated as chamber") {
        json objects = {"temperature_fan cavity_fan"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
    }

    SECTION("Box sensor is treated as chamber (Elegoo COSMOS)") {
        json objects = {"temperature_sensor box"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.supports_chamber());
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor box");
    }

    SECTION("Enclosure sensor is treated as chamber") {
        json objects = {"temperature_sensor enclosure"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor enclosure");
    }

    SECTION("Enclosure heater_generic is treated as chamber") {
        json objects = {"heater_generic enclosure_heater"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
    }

    SECTION("Box temperature_fan is treated as chamber") {
        json objects = {"temperature_fan box_fan"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
    }

    SECTION("Case-insensitive keyword match") {
        json objects = {"temperature_sensor Box", "temperature_sensor ENCLOSURE_top",
                        "heater_generic Chamber_primary", "temperature_fan Cavity"};
        PrinterDiscovery hw2;
        hw2.parse_objects(objects);

        REQUIRE(hw2.has_chamber_sensor());
        REQUIRE(hw2.has_chamber_heater());
    }
}

// ============================================================================
// Chamber-keyword confidence: stronger keyword wins over weaker one regardless
// of iteration order. Regression coverage for #947 (QIDI Q2).
// ============================================================================

TEST_CASE("PrinterDiscovery chamber-keyword scoring prefers 'chamber' over 'box'",
          "[printer_discovery][chamber][issue947]") {
    PrinterDiscovery hw;

    SECTION("QIDI Q2: chamber heater wins over Qidi-Box filament dryer (chamber first)") {
        // Order matches the Q2's klippy stats: box1_* appears before chamber.
        // Without confidence scoring (last-wins), chamber would win here by
        // luck — this section just nails down that ordering.
        json objects = {"heater_generic box1_heater",
                        "temperature_sensor box1_env",
                        "temperature_sensor box1_heater_temp_a",
                        "temperature_sensor box1_heater_temp_b",
                        "temperature_sensor Box1_STM32",
                        "heater_bed",
                        "heater_generic chamber",
                        "temperature_sensor Chamber_Thermal_Protection_Sensor",
                        "extruder"};
        hw.parse_objects(objects);

        REQUIRE(hw.chamber_heater_name() == "heater_generic chamber");
        REQUIRE(hw.chamber_heater_object_name() == "chamber");
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor Chamber_Thermal_Protection_Sensor");
    }

    SECTION("QIDI Q2: chamber heater still wins when box1_* objects come LAST") {
        // The bug case: if Moonraker returned objects in a different order
        // (e.g. alphabetical with capitals last), the old last-wins discovery
        // would mis-resolve to a box1_* object. Scoring must prevent that.
        json objects = {"heater_generic chamber",
                        "temperature_sensor Chamber_Thermal_Protection_Sensor",
                        "heater_generic box1_heater",
                        "temperature_sensor box1_env",
                        "temperature_sensor box1_heater_temp_a",
                        "temperature_sensor box1_heater_temp_b",
                        "temperature_sensor Box1_STM32"};
        hw.parse_objects(objects);

        REQUIRE(hw.chamber_heater_name() == "heater_generic chamber");
        REQUIRE(hw.chamber_heater_object_name() == "chamber");
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor Chamber_Thermal_Protection_Sensor");
    }

    SECTION("box1_* alone (no chamber) does NOT register as chamber") {
        // Digit-suffixed AMS names must not be treated as the printer chamber.
        json objects = {"heater_generic box1_heater", "temperature_sensor box1_env",
                        "temperature_sensor box1_heater_temp_a", "temperature_sensor Box1_STM32"};
        hw.parse_objects(objects);

        REQUIRE_FALSE(hw.has_chamber_heater());
        REQUIRE_FALSE(hw.has_chamber_sensor());
    }

    SECTION("Elegoo COSMOS literal 'box' still detected (token, not substring)") {
        json objects = {"temperature_sensor box"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor box");
    }

    SECTION("'box_fan' (standalone token) still detected") {
        json objects = {"temperature_fan box_fan"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
        REQUIRE(hw.chamber_heater_name() == "temperature_fan box_fan");
    }

    SECTION("enclosure beats standalone box when both present") {
        json objects = {"temperature_sensor box", "temperature_sensor enclosure"};
        hw.parse_objects(objects);

        // enclosure (conf 90) > standalone box (conf 60)
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor enclosure");
    }

    SECTION("chamber beats enclosure when both present") {
        json objects = {"temperature_sensor enclosure", "temperature_sensor chamber"};
        hw.parse_objects(objects);

        // chamber (conf 100) > enclosure (conf 90)
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor chamber");
    }
}

// ============================================================================
// Air-quality disambiguation: a sensor whose name merely CONTAINS "chamber"
// but is really an air-quality sensor (chamber_tvoc, chamber_co2, ...) must
// not out-rank the real chamber temperature sensor. The compound (-1) and
// air-quality (-40) modifiers push these below a clean "chamber"/"enclosure".
// ============================================================================

TEST_CASE("PrinterDiscovery chamber scoring demotes air-quality sensors",
          "[printer_discovery][chamber]") {
    PrinterDiscovery hw;

    SECTION("clean 'chamber' beats 'chamber_tvoc' listed first (tvoc is air-quality)") {
        // chamber_tvoc appears BEFORE chamber: substring scoring would lock in
        // the tvoc sensor (both 100, first-wins). With the air-quality penalty
        // chamber_tvoc scores 59, so the real chamber (100) wins regardless.
        json objects = {"temperature_sensor chamber_tvoc", "temperature_sensor chamber"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor chamber");
    }

    SECTION("'enclosure' beats 'chamber_tvoc' (air-quality demoted below enclosure)") {
        // chamber_tvoc = 100 - 1 - 40 = 59; enclosure = 90 wins.
        json objects = {"temperature_sensor chamber_tvoc", "temperature_sensor enclosure"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor enclosure");
    }

    SECTION("'chamber_tvoc' alone is still detected (floored, not dropped)") {
        // Only chamber-ish sensor present — must still register (score 59 > 0),
        // not fall through to undetected.
        json objects = {"temperature_sensor chamber_tvoc"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor chamber_tvoc");
    }

    SECTION("clean compound 'chamber_temp' alone is still detected (regression guard)") {
        // A plain compound temperature sensor (no air-quality token) scores 99
        // and must remain a valid chamber sensor.
        json objects = {"temperature_sensor chamber_temp"};
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_sensor());
        REQUIRE(hw.chamber_sensor_name() == "temperature_sensor chamber_temp");
    }
}

// ============================================================================
// Chamber-heater TYPE tiebreak: a settable `heater_generic` must always beat a
// same-keyword `temperature_fan` (a cooling fan), regardless of iteration order.
// Regression for the Creality K2 Plus, which exposes BOTH
// `heater_generic chamber_heater` (the real, settable heater) AND
// `temperature_fan chamber_fan` (a cooling fan with its own target). With a
// keyword-only tiebreak both score 100, so the winner depended on Moonraker's
// iteration order — picking the fan would route "heat the chamber" to
// SET_TEMPERATURE_FAN_TARGET (a cooling threshold) instead of the heater.
// ============================================================================

TEST_CASE("PrinterDiscovery chamber heater prefers heater_generic over temperature_fan",
          "[printer_discovery][chamber][k2]") {
    SECTION("K2 Plus: heater_generic wins when the temperature_fan is listed FIRST") {
        // The failure case: cooling fan iterated before the heater.
        json objects = {"temperature_fan chamber_fan",
                        "heater_fan chamber_fan",
                        "temperature_sensor chamber_temp",
                        "heater_generic chamber_heater",
                        "heater_bed",
                        "extruder"};
        PrinterDiscovery hw;
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
        REQUIRE(hw.chamber_heater_name() == "heater_generic chamber_heater");
        REQUIRE(hw.chamber_heater_object_name() == "chamber_heater");
    }

    SECTION("K2 Plus: heater_generic still wins when listed first (order-independent)") {
        json objects = {"heater_generic chamber_heater", "temperature_fan chamber_fan",
                        "temperature_sensor chamber_temp"};
        PrinterDiscovery hw;
        hw.parse_objects(objects);

        REQUIRE(hw.chamber_heater_name() == "heater_generic chamber_heater");
        REQUIRE(hw.chamber_heater_object_name() == "chamber_heater");
    }

    SECTION("temperature_fan is still selected when it is the ONLY chamber control") {
        // Printers that use a temperature_fan as their active chamber control
        // must keep working — the type tiebreak only demotes the fan when a
        // real heater_generic is also present.
        json objects = {"temperature_fan chamber", "temperature_sensor chamber_aux"};
        PrinterDiscovery hw;
        hw.parse_objects(objects);

        REQUIRE(hw.has_chamber_heater());
        REQUIRE(hw.chamber_heater_name() == "temperature_fan chamber");
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

TEST_CASE("AFC unknown unit types detected generically", "[printer_discovery][afc]") {
    PrinterDiscovery hw;
    json objects = json::array({"AFC", "AFC_stepper lane1", "AFC_NightOwl Owl_1", "AFC_buffer TN"});
    hw.parse_objects(objects);

    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == AmsType::AFC);

    // NightOwl is not a known component prefix, so it appears as a unit object
    auto& unit_names = hw.afc_unit_object_names();
    REQUIRE(unit_names.size() == 1);
    CHECK(unit_names[0] == "AFC_NightOwl Owl_1");

    // Known component types should NOT appear as unit objects
    CHECK(hw.afc_lane_names().size() == 1);   // lane1 from AFC_stepper
    CHECK(hw.afc_buffer_names().size() == 1); // TN from AFC_buffer
}

// ==========================================================================
// AD5X IFS Detection
// ==========================================================================

TEST_CASE("PrinterDiscovery detects AD5X IFS via zmod sensors", "[printer_discovery][ad5x_ifs]") {
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({
        "extruder",
        "heater_bed",
        "filament_switch_sensor _ifs_port_sensor_1",
        "filament_switch_sensor _ifs_port_sensor_2",
        "filament_switch_sensor _ifs_port_sensor_3",
        "filament_switch_sensor _ifs_port_sensor_4",
        "filament_switch_sensor head_switch_sensor",
    }));

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::AD5X_IFS);
    REQUIRE(discovery.detected_ams_systems().size() == 1);
    REQUIRE(discovery.detected_ams_systems()[0].type == AmsType::AD5X_IFS);
}

TEST_CASE("PrinterDiscovery detects native ZMOD IFS via motion sensor",
          "[printer_discovery][ad5x_ifs]") {
    helix::PrinterDiscovery discovery;
    // Native ZMOD IFS (without lessWaste) only exposes a motion sensor and head
    // switch sensor — no per-port switch sensors
    discovery.parse_objects(nlohmann::json::array({
        "extruder",
        "heater_bed",
        "filament_motion_sensor ifs_motion_sensor",
        "filament_switch_sensor head_switch_sensor",
    }));

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::AD5X_IFS);
    REQUIRE(discovery.detected_ams_systems().size() == 1);
    REQUIRE(discovery.detected_ams_systems()[0].type == AmsType::AD5X_IFS);

    // Both sensors must appear in filament_sensor_names (not just head_switch_sensor)
    auto& sensors = discovery.filament_sensor_names();
    REQUIRE(sensors.size() == 2);
    REQUIRE(std::find(sensors.begin(), sensors.end(), "filament_motion_sensor ifs_motion_sensor") !=
            sensors.end());
    REQUIRE(std::find(sensors.begin(), sensors.end(),
                      "filament_switch_sensor head_switch_sensor") != sensors.end());
}

TEST_CASE("PrinterDiscovery does not detect AD5X IFS without zmod sensors",
          "[printer_discovery][ad5x_ifs]") {
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({
        "extruder",
        "heater_bed",
        "filament_switch_sensor runout_sensor",
    }));

    REQUIRE_FALSE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::NONE);
}

TEST_CASE("PrinterDiscovery: Happy Hare takes priority over IFS sensors",
          "[printer_discovery][ad5x_ifs]") {
    helix::PrinterDiscovery discovery;
    // Both MMU and IFS sensors present — MMU wins because it's detected first
    discovery.parse_objects(nlohmann::json::array({
        "mmu",
        "filament_switch_sensor _ifs_port_sensor_1",
    }));

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::HAPPY_HARE);
}

// ==========================================================================
// QIDI Box Detection
// ==========================================================================
// The QIDI Box is a hub-style filament changer for Plus 4 / Q2 / Max 4.
// Klipper loads custom extensions that register objects under these names:
//   - `box_stepper slot<N>` (one per physical slot; 4 per box, 1-4 boxes)
//   - `box_extras` (coordinator: b_endstop / e_endstop / button state)
//   - `box_rfid card_reader_<N>` (2 readers per box)
//   - `heater_generic heater_box<N>` (drying heater per box)
//   - `aht20_f heater_box<N>` (humidity sensor co-located with heater)
//
// Detection key: presence of any `box_stepper slot*` object. That's the
// unambiguous signal — the other names ride along.

TEST_CASE("PrinterDiscovery detects QIDI Box via box_stepper objects",
          "[printer_discovery][qidi_box]") {
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({
        "extruder",
        "heater_bed",
        "box_extras",
        "box_stepper slot0",
        "box_stepper slot1",
        "box_stepper slot2",
        "box_stepper slot3",
        "heater_generic heater_box1",
        "aht20_f heater_box1",
        "box_rfid card_reader_1",
        "box_rfid card_reader_2",
    }));

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::QIDI_BOX);
    REQUIRE(discovery.detected_ams_systems().size() == 1);
    REQUIRE(discovery.detected_ams_systems()[0].type == AmsType::QIDI_BOX);
}

TEST_CASE("PrinterDiscovery counts QIDI Box slots from box_stepper objects",
          "[printer_discovery][qidi_box]") {
    // The backend needs to know how many slots exist to size system_info_
    // correctly. Slot count = number of `box_stepper slot*` objects.

    SECTION("Single box: 4 slots") {
        helix::PrinterDiscovery discovery;
        discovery.parse_objects(nlohmann::json::array({
            "box_stepper slot0",
            "box_stepper slot1",
            "box_stepper slot2",
            "box_stepper slot3",
        }));
        REQUIRE(discovery.qidi_box_slot_count() == 4);
    }

    SECTION("Two boxes: 8 slots") {
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = nlohmann::json::array();
        for (int i = 0; i < 8; ++i) {
            objects.push_back("box_stepper slot" + std::to_string(i));
        }
        discovery.parse_objects(objects);
        REQUIRE(discovery.qidi_box_slot_count() == 8);
    }

    SECTION("Four boxes: 16 slots (max)") {
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = nlohmann::json::array();
        for (int i = 0; i < 16; ++i) {
            objects.push_back("box_stepper slot" + std::to_string(i));
        }
        discovery.parse_objects(objects);
        REQUIRE(discovery.qidi_box_slot_count() == 16);
    }

    SECTION("No QIDI Box: slot count is 0") {
        helix::PrinterDiscovery discovery;
        discovery.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
        REQUIRE(discovery.qidi_box_slot_count() == 0);
    }
}

TEST_CASE("PrinterDiscovery: Happy Hare takes priority over QIDI Box",
          "[printer_discovery][qidi_box]") {
    // Happy Hare is a generic MMU and wins over hardware-specific backends
    // (matches AD5X IFS priority semantics). If both objects appear in
    // printer.objects.list, mmu_type() must be HAPPY_HARE.
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({
        "mmu",
        "box_stepper slot0",
        "box_stepper slot1",
        "box_stepper slot2",
        "box_stepper slot3",
    }));

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == AmsType::HAPPY_HARE);
    // QIDI slot counter must NOT increment when Happy Hare claims the slot.
    REQUIRE(discovery.qidi_box_slot_count() == 0);
}

// ============================================================================
// Build Volume from configfile.settings
// ============================================================================
//
// PrinterDetector reads discovery.build_volume(), and 63 of the 94 printer
// database entries carry a build_volume_range heuristic. The only other writer
// of the field is the safety-limits fetch, which is kicked off from the
// discovery-complete handler — i.e. after detection has already run. These
// tests pin the parse that fills it during discovery instead.

TEST_CASE("PrinterDiscovery: build volume parsed from configfile.settings steppers",
          "[printer_discovery][build_volume]") {
    helix::PrinterDiscovery discovery;

    // Nothing has been parsed yet: the field starts empty, which is exactly the
    // state detection used to see on every in-app run.
    REQUIRE(discovery.build_volume().x_max == 0.0f);
    REQUIRE(discovery.build_volume().y_max == 0.0f);

    json settings = {{"printer", {{"kinematics", "corexy"}}},
                     {"stepper_x", {{"position_min", 0.0}, {"position_max", 350.0}}},
                     {"stepper_y", {{"position_min", 0.0}, {"position_max", 350.0}}},
                     {"stepper_z", {{"position_min", 0.0}, {"position_max", 340.0}}}};

    REQUIRE(discovery.parse_build_volume(settings));
    REQUIRE(discovery.build_volume().x_min == 0.0f);
    REQUIRE(discovery.build_volume().x_max == 350.0f);
    REQUIRE(discovery.build_volume().y_min == 0.0f);
    REQUIRE(discovery.build_volume().y_max == 350.0f);
    REQUIRE(discovery.build_volume().z_max == 340.0f);
}

TEST_CASE("PrinterDiscovery: build volume keeps negative axis minimums",
          "[printer_discovery][build_volume]") {
    // Voron-style configs park X/Y off the bed, so position_min is negative.
    // The span, not the max alone, is what build_volume_range windows compare.
    helix::PrinterDiscovery discovery;
    json settings = {{"stepper_x", {{"position_min", -5.0}, {"position_max", 355.0}}},
                     {"stepper_y", {{"position_min", -2.5}, {"position_max", 357.5}}}};

    REQUIRE(discovery.parse_build_volume(settings));
    REQUIRE(discovery.build_volume().x_min == -5.0f);
    REQUIRE(discovery.build_volume().y_min == -2.5f);
    REQUIRE(discovery.build_volume().x_max == 355.0f);
}

TEST_CASE("PrinterDiscovery: build volume rejects unusable payloads",
          "[printer_discovery][build_volume]") {
    SECTION("Non-object settings is ignored") {
        helix::PrinterDiscovery discovery;
        REQUIRE_FALSE(discovery.parse_build_volume(json("not an object")));
        REQUIRE(discovery.build_volume().x_max == 0.0f);
    }

    SECTION("Settings with no stepper sections is ignored") {
        helix::PrinterDiscovery discovery;
        json settings = {{"printer", {{"kinematics", "cartesian"}}}};
        REQUIRE_FALSE(discovery.parse_build_volume(settings));
        REQUIRE(discovery.build_volume().x_max == 0.0f);
    }

    SECTION("Null position values do not throw and are skipped") {
        helix::PrinterDiscovery discovery;
        json settings = {{"stepper_x", {{"position_min", nullptr}, {"position_max", nullptr}}},
                         {"stepper_y", {{"position_max", nullptr}}}};
        REQUIRE_NOTHROW(discovery.parse_build_volume(settings));
        REQUIRE_FALSE(discovery.parse_build_volume(settings));
        REQUIRE(discovery.build_volume().x_max == 0.0f);
    }

    SECTION("An all-zero extent is not stored") {
        // Scoring a zero-sized volume against every printer's window is worse
        // than having no volume at all, so the setter is left untouched.
        helix::PrinterDiscovery discovery;
        json settings = {{"stepper_x", {{"position_min", 0.0}, {"position_max", 0.0}}},
                         {"stepper_y", {{"position_min", 0.0}, {"position_max", 0.0}}}};
        REQUIRE_FALSE(discovery.parse_build_volume(settings));
        REQUIRE(discovery.build_volume().x_max == 0.0f);
    }

    SECTION("A partial config with only X still stores what it found") {
        helix::PrinterDiscovery discovery;
        json settings = {{"stepper_x", {{"position_min", 0.0}, {"position_max", 180.0}}}};
        REQUIRE(discovery.parse_build_volume(settings));
        REQUIRE(discovery.build_volume().x_max == 180.0f);
        REQUIRE(discovery.build_volume().y_max == 0.0f);
    }
}
