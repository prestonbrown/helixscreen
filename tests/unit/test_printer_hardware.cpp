// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_hardware.cpp
 * @brief Unit tests for PrinterHardware hardware guessing heuristics
 *
 * Tests the PrinterHardware class which encapsulates Klipper naming convention
 * knowledge for guessing hardware assignments:
 * - guess_bed_heater()
 * - guess_hotend_heater()
 * - guess_bed_sensor()
 * - guess_hotend_sensor()
 * - guess_part_cooling_fan()
 * - guess_main_led_strip()
 */

#include "../helix_test_fixture.h"
#include "moonraker_client_mock.h"
#include "printer_hardware.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// guess_bed_heater() Tests
// ============================================================================

TEST_CASE("PrinterHardware::guess_bed_heater", "[printer][guessing]") {
    std::vector<std::string> sensors;
    std::vector<std::string> fans;
    std::vector<std::string> leds;

    SECTION("Exact match: heater_bed (highest priority)") {
        std::vector<std::string> heaters = {"extruder", "heater_bed", "extruder1"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "heater_bed");
    }

    SECTION("Exact match: heated_bed (second priority)") {
        std::vector<std::string> heaters = {"extruder", "heated_bed", "extruder1"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "heated_bed");
    }

    SECTION("Substring match: custom_bed_heater") {
        std::vector<std::string> heaters = {"extruder", "custom_bed_heater", "extruder1"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "custom_bed_heater");
    }

    SECTION("Substring match: bed_chamber") {
        std::vector<std::string> heaters = {"extruder", "bed_chamber"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "bed_chamber");
    }

    SECTION("Priority: heater_bed wins over heated_bed") {
        std::vector<std::string> heaters = {"heated_bed", "heater_bed", "extruder"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "heater_bed");
    }

    SECTION("Priority: heated_bed wins over substring match") {
        std::vector<std::string> heaters = {"extruder", "custom_bed", "heated_bed"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "heated_bed");
    }

    SECTION("Priority: exact match wins when multiple substrings exist") {
        std::vector<std::string> heaters = {"bed_zone1", "bed_zone2", "heater_bed"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "heater_bed");
    }

    SECTION("Multiple substring matches: returns first found") {
        std::vector<std::string> heaters = {"extruder", "bed_zone1", "bed_zone2"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "bed_zone1");
    }

    SECTION("No match: returns empty string") {
        std::vector<std::string> heaters = {"extruder", "extruder1", "chamber_heater"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "");
    }

    SECTION("Empty heaters list: returns empty string") {
        std::vector<std::string> heaters = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "");
    }

    SECTION("Case sensitivity: 'Bed' does not match 'bed'") {
        std::vector<std::string> heaters = {"extruder", "heater_Bed"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "");
    }
}

// ============================================================================
// guess_hotend_heater() Tests
// ============================================================================

TEST_CASE("PrinterHardware::guess_hotend_heater", "[printer][guessing]") {
    std::vector<std::string> sensors;
    std::vector<std::string> fans;
    std::vector<std::string> leds;

    SECTION("Exact match: extruder (highest priority)") {
        std::vector<std::string> heaters = {"heater_bed", "extruder", "extruder1"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "extruder");
    }

    SECTION("Exact match: extruder0 (second priority)") {
        std::vector<std::string> heaters = {"heater_bed", "extruder0", "extruder1"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "extruder0");
    }

    SECTION("Substring match: extruder1") {
        std::vector<std::string> heaters = {"heater_bed", "extruder1"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "extruder1");
    }

    SECTION("Substring match: hotend_heater") {
        std::vector<std::string> heaters = {"heater_bed", "hotend_heater"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "hotend_heater");
    }

    SECTION("Substring match: e0_heater") {
        std::vector<std::string> heaters = {"heater_bed", "e0_heater"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "e0_heater");
    }

    SECTION("Priority: extruder wins over extruder0") {
        std::vector<std::string> heaters = {"heater_bed", "extruder0", "extruder"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "extruder");
    }

    SECTION("Priority: extruder0 wins over extruder1") {
        std::vector<std::string> heaters = {"heater_bed", "extruder1", "extruder0"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "extruder0");
    }

    SECTION("Priority: extruder substring wins over hotend") {
        std::vector<std::string> heaters = {"heater_bed", "hotend", "extruder2"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "extruder2");
    }

    SECTION("Priority: hotend wins over e0") {
        std::vector<std::string> heaters = {"heater_bed", "e0", "hotend"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "hotend");
    }

    SECTION("Multiple extruder substring matches: returns first found") {
        std::vector<std::string> heaters = {"heater_bed", "extruder1", "extruder2"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "extruder1");
    }

    SECTION("No match: returns empty string") {
        std::vector<std::string> heaters = {"heater_bed", "chamber_heater"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "");
    }

    SECTION("Empty heaters list: returns empty string") {
        std::vector<std::string> heaters = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "");
    }

    SECTION("Case sensitivity: 'Extruder' does not match 'extruder'") {
        std::vector<std::string> heaters = {"heater_bed", "Extruder"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "");
    }

    SECTION("Edge case: e0 matches as substring in 'e0'") {
        std::vector<std::string> heaters = {"heater_bed", "e0"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "e0");
    }
}

// ============================================================================
// guess_bed_sensor() Tests
// ============================================================================

TEST_CASE("PrinterHardware::guess_bed_sensor", "[printer][guessing]") {
    std::vector<std::string> fans;
    std::vector<std::string> leds;

    SECTION("Heater found: returns heater name (heaters have built-in thermistors)") {
        std::vector<std::string> heaters = {"extruder", "heater_bed"};
        std::vector<std::string> sensors = {"temperature_sensor chamber"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
    }

    SECTION("Heater found: returns heated_bed") {
        std::vector<std::string> heaters = {"extruder", "heated_bed"};
        std::vector<std::string> sensors = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "heated_bed");
    }

    SECTION("No heater, sensor match: temperature_sensor bed_temp") {
        std::vector<std::string> heaters = {"extruder"};
        std::vector<std::string> sensors = {"temperature_sensor chamber",
                                            "temperature_sensor bed_temp"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "temperature_sensor bed_temp");
    }

    SECTION("No heater, sensor substring: bed_thermistor") {
        std::vector<std::string> heaters = {"extruder"};
        std::vector<std::string> sensors = {"chamber", "bed_thermistor"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "bed_thermistor");
    }

    SECTION("Priority: heater wins over sensor with 'bed'") {
        std::vector<std::string> heaters = {"extruder", "heater_bed"};
        std::vector<std::string> sensors = {"temperature_sensor bed_auxiliary"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
    }

    SECTION("Multiple sensors with 'bed': returns first found") {
        std::vector<std::string> heaters = {"extruder"};
        std::vector<std::string> sensors = {"chamber", "bed_sensor1", "bed_sensor2"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "bed_sensor1");
    }

    SECTION("No heater, no sensor match: returns empty string") {
        std::vector<std::string> heaters = {"extruder"};
        std::vector<std::string> sensors = {"temperature_sensor chamber", "temperature_sensor mcu"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "");
    }

    SECTION("Empty heaters and sensors: returns empty string") {
        std::vector<std::string> heaters = {};
        std::vector<std::string> sensors = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "");
    }

    SECTION("Heater substring match: custom_bed_heater returns from heater") {
        std::vector<std::string> heaters = {"extruder", "custom_bed_heater"};
        std::vector<std::string> sensors = {"temperature_sensor bed_aux"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "custom_bed_heater");
    }
}

// ============================================================================
// guess_hotend_sensor() Tests
// ============================================================================

TEST_CASE("PrinterHardware::guess_hotend_sensor", "[printer][guessing]") {
    std::vector<std::string> fans;
    std::vector<std::string> leds;

    SECTION("Heater found: returns extruder (heaters have built-in thermistors)") {
        std::vector<std::string> heaters = {"heater_bed", "extruder"};
        std::vector<std::string> sensors = {"temperature_sensor chamber"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }

    SECTION("Heater found: returns extruder0") {
        std::vector<std::string> heaters = {"heater_bed", "extruder0"};
        std::vector<std::string> sensors = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "extruder0");
    }

    SECTION("No heater, sensor match: temperature_sensor extruder_aux") {
        std::vector<std::string> heaters = {"heater_bed"};
        std::vector<std::string> sensors = {"temperature_sensor chamber",
                                            "temperature_sensor extruder_aux"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "temperature_sensor extruder_aux");
    }

    SECTION("No heater, sensor priority: extruder wins over hotend") {
        std::vector<std::string> heaters = {"heater_bed"};
        std::vector<std::string> sensors = {"hotend_thermistor", "extruder_aux"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "extruder_aux");
    }

    SECTION("No heater, sensor priority: hotend wins over e0") {
        std::vector<std::string> heaters = {"heater_bed"};
        std::vector<std::string> sensors = {"e0_temp", "hotend_thermistor"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "hotend_thermistor");
    }

    SECTION("No heater, sensor match: e0_thermistor") {
        std::vector<std::string> heaters = {"heater_bed"};
        std::vector<std::string> sensors = {"chamber", "e0_thermistor"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "e0_thermistor");
    }

    SECTION("Priority: heater wins over sensor with 'extruder'") {
        std::vector<std::string> heaters = {"heater_bed", "extruder"};
        std::vector<std::string> sensors = {"temperature_sensor extruder_aux"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }

    SECTION("Multiple extruder sensors: returns first found") {
        std::vector<std::string> heaters = {"heater_bed"};
        std::vector<std::string> sensors = {"chamber", "extruder_sensor1", "extruder_sensor2"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "extruder_sensor1");
    }

    SECTION("No heater, no sensor match: returns empty string") {
        std::vector<std::string> heaters = {"heater_bed"};
        std::vector<std::string> sensors = {"temperature_sensor chamber", "temperature_sensor mcu"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "");
    }

    SECTION("Empty heaters and sensors: returns empty string") {
        std::vector<std::string> heaters = {};
        std::vector<std::string> sensors = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "");
    }

    SECTION("Heater substring match: hotend_heater returns from heater") {
        std::vector<std::string> heaters = {"heater_bed", "hotend_heater"};
        std::vector<std::string> sensors = {"temperature_sensor hotend_aux"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "hotend_heater");
    }

    SECTION("Heater e0 match: e0 returns from heater") {
        std::vector<std::string> heaters = {"heater_bed", "e0"};
        std::vector<std::string> sensors = {"temperature_sensor e0_aux"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_sensor() == "e0");
    }
}

// ============================================================================
// guess_part_cooling_fan() Tests
// ============================================================================

TEST_CASE("PrinterHardware::guess_part_cooling_fan", "[printer][guessing]") {
    std::vector<std::string> heaters;
    std::vector<std::string> sensors;
    std::vector<std::string> leds;

    SECTION("Exact match: 'fan' is canonical Klipper part cooling fan") {
        std::vector<std::string> fans = {"heater_fan hotend_fan", "fan", "fan_generic bed_fans"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_part_cooling_fan() == "fan");
    }

    SECTION("Priority: 'fan' wins over 'part' substring") {
        std::vector<std::string> fans = {"part_cooling_fan", "fan", "controller_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_part_cooling_fan() == "fan");
    }

    SECTION("Substring match: 'part' when no exact 'fan'") {
        std::vector<std::string> fans = {"heater_fan hotend_fan", "part_cooling_fan",
                                         "controller_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_part_cooling_fan() == "part_cooling_fan");
    }

    SECTION("Fallback: first fan when no 'fan' or 'part' match") {
        std::vector<std::string> fans = {"heater_fan hotend_fan", "controller_fan",
                                         "nevermore_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_part_cooling_fan() == "heater_fan hotend_fan");
    }

    SECTION("Empty fans list: returns empty string") {
        std::vector<std::string> fans = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_part_cooling_fan() == "");
    }

    SECTION("Case sensitivity: 'Fan' does not match exact 'fan'") {
        std::vector<std::string> fans = {"Fan", "controller_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // 'Fan' contains 'an' not 'fan', so fallback to first
        REQUIRE(hw.guess_part_cooling_fan() == "Fan");
    }

    SECTION("Fan with space: 'fan ' should not exact match 'fan'") {
        std::vector<std::string> fans = {"fan ", "controller_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // "fan " != "fan" exactly, but it does contain "fan" substring
        // Actually "fan " has trailing space so won't be exact match
        // Will fall back to first since "fan " != "fan" exactly
        REQUIRE(hw.guess_part_cooling_fan() == "fan ");
    }
}

// ============================================================================
// guess_chamber_fan() Tests
// ============================================================================

// ----------------------------------------------------------------------------
// Creality K2 Plus rig: two Klipper objects historically shared the bare name
// "chamber_fan" - [heater_fan chamber_fan] (the PTC heater's own blower) and
// [temperature_fan chamber_fan] (the chamber exhaust). The PTC blower was
// renamed to chamber_heater_fan on the test rig to remove that collision.
// These cases pin guess_chamber_fan() against the real object list on both the
// stock and renamed configs, so the rename cannot silently change detection.
// ----------------------------------------------------------------------------

TEST_CASE("PrinterHardware::guess_chamber_fan - Creality K2 Plus object list",
          "[printer][guessing]") {
    std::vector<std::string> heaters;
    std::vector<std::string> sensors;
    std::vector<std::string> leds;

    // Order as reported by Moonraker /printer/objects/list, filtered to fans.
    SECTION("stock K2 naming") {
        std::vector<std::string> fans = {"heater_fan hotend_fan",  "output_pin fan0",
                                         "output_pin fan2",        "output_pin fan1",
                                         "heater_fan chamber_fan", "temperature_fan chamber_fan",
                                         "output_pin extruder_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Priority 1 wants a bare "chamber_fan"; every K2 object is section-prefixed,
        // so it falls through to the first entry containing "chamber".
        REQUIRE(hw.guess_chamber_fan() == "heater_fan chamber_fan");
    }

    SECTION("after renaming the PTC blower to chamber_heater_fan") {
        std::vector<std::string> fans = {"heater_fan hotend_fan",
                                         "output_pin fan0",
                                         "output_pin fan2",
                                         "output_pin fan1",
                                         "heater_fan chamber_heater_fan",
                                         "temperature_fan chamber_fan",
                                         "output_pin extruder_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Unchanged: the renamed object still contains "chamber" and is still first.
        REQUIRE(hw.guess_chamber_fan() == "heater_fan chamber_heater_fan");
    }
}

TEST_CASE("PrinterHardware::guess_chamber_fan", "[printer][guessing]") {
    std::vector<std::string> heaters;
    std::vector<std::string> sensors;
    std::vector<std::string> leds;

    SECTION("Exact match: 'chamber_fan' (highest priority)") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "chamber_fan",
                                         "nevermore"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "chamber_fan");
    }

    SECTION("Substring match: 'chamber' in name") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan",
                                         "fan_generic chamber_circ"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "fan_generic chamber_circ");
    }

    SECTION("Priority: 'chamber' wins over 'nevermore'") {
        std::vector<std::string> fans = {"fan", "nevermore_filter", "chamber_circulation"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "chamber_circulation");
    }

    SECTION("Substring match: 'nevermore' filter") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "nevermore_filter"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "nevermore_filter");
    }

    SECTION("Priority: 'nevermore' wins over 'bed_fans'") {
        std::vector<std::string> fans = {"fan", "bed_fans", "nevermore"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "nevermore");
    }

    SECTION("Substring match: 'bed_fans' (BTT Pi naming)") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "bed_fans"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "bed_fans");
    }

    SECTION("Priority: 'bed_fans' wins over 'filter'") {
        std::vector<std::string> fans = {"fan", "air_filter", "bed_fans"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "bed_fans");
    }

    SECTION("Substring match: 'filter' for air filtration") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "carbon_filter_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "carbon_filter_fan");
    }

    SECTION("No match: returns empty string (optional hardware)") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "controller_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "");
    }

    SECTION("Empty fans list: returns empty string") {
        std::vector<std::string> fans = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "");
    }

    SECTION("Case sensitivity: 'Chamber' does not match 'chamber'") {
        std::vector<std::string> fans = {"fan", "Chamber_Fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_chamber_fan() == "");
    }

    SECTION("Multiple substring matches: returns first in priority order") {
        std::vector<std::string> fans = {"filter_fan", "nevermore", "chamber_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Exact "chamber_fan" should win
        REQUIRE(hw.guess_chamber_fan() == "chamber_fan");
    }
}

// ============================================================================
// guess_exhaust_fan() Tests
// ============================================================================

TEST_CASE("PrinterHardware::guess_exhaust_fan", "[printer][guessing]") {
    std::vector<std::string> heaters;
    std::vector<std::string> sensors;
    std::vector<std::string> leds;

    SECTION("Exact match: 'exhaust_fan' (highest priority)") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "exhaust_fan", "vent_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "exhaust_fan");
    }

    SECTION("Substring match: 'exhaust' in name") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "fan_generic exhaust"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "fan_generic exhaust");
    }

    SECTION("Priority: 'exhaust' wins over 'vent'") {
        std::vector<std::string> fans = {"fan", "vent_fan", "exhaust_blower"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "exhaust_blower");
    }

    SECTION("Substring match: 'vent' for ventilation") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "vent_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "vent_fan");
    }

    SECTION("Substring match: 'vent' in longer name") {
        std::vector<std::string> fans = {"fan", "enclosure_ventilation"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "enclosure_ventilation");
    }

    SECTION("Substring match: 'external' for outside venting") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "external_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "external_fan");
    }

    SECTION("Priority: 'exhaust' wins over 'external'") {
        std::vector<std::string> fans = {"fan", "external_fan", "exhaust_blower"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "exhaust_blower");
    }

    SECTION("Priority: 'external' wins over 'vent'") {
        std::vector<std::string> fans = {"fan", "vent_fan", "external_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "external_fan");
    }

    SECTION("No match: returns empty string (optional hardware)") {
        std::vector<std::string> fans = {"fan", "heater_fan hotend_fan", "controller_fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "");
    }

    SECTION("Empty fans list: returns empty string") {
        std::vector<std::string> fans = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "");
    }

    SECTION("Case sensitivity: 'Exhaust' does not match 'exhaust'") {
        std::vector<std::string> fans = {"fan", "Exhaust_Fan"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "");
    }

    SECTION("Multiple fans with exhaust: returns first found") {
        std::vector<std::string> fans = {"exhaust_main", "exhaust_secondary"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_exhaust_fan() == "exhaust_main");
    }
}

// ============================================================================
// guess_main_led_strip() Tests
// ============================================================================

TEST_CASE("PrinterHardware::guess_main_led_strip", "[printer][guessing]") {
    std::vector<std::string> heaters;
    std::vector<std::string> sensors;
    std::vector<std::string> fans;

    SECTION("Priority 1: 'case' substring wins") {
        std::vector<std::string> leds = {"Turtle_Corner_Indicators", "case_lights",
                                         "neopixel sb_leds"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "case_lights");
    }

    SECTION("Priority 2: 'chamber' when no 'case'") {
        std::vector<std::string> leds = {"Turtle_Corner_Indicators", "chamber_leds",
                                         "neopixel sb_leds"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "chamber_leds");
    }

    SECTION("Priority 3: 'light' when no 'case' or 'chamber'") {
        std::vector<std::string> leds = {"Turtle_Corner_Indicators", "led_strip_lights",
                                         "neopixel sb_leds"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "led_strip_lights");
    }

    SECTION("Priority 4: avoid indicators, select generic LED") {
        std::vector<std::string> leds = {"Turtle_Corner_Indicators", "neopixel my_strip"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "neopixel my_strip");
    }

    SECTION("Priority 4: avoid 'status' in name") {
        std::vector<std::string> leds = {"status_leds", "neopixel main_strip"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "neopixel main_strip");
    }

    SECTION("Priority 4: sb_leds excluded as toolhead LED, fallback to first") {
        std::vector<std::string> leds = {"Turtle_Corner_Indicators", "neopixel sb_leds"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Both excluded from Priority 4, fallback returns first available
        REQUIRE(hw.guess_main_led_strip() == "Turtle_Corner_Indicators");
    }

    SECTION("Priority 4: sb_led (singular) excluded as toolhead LED") {
        std::vector<std::string> leds = {"neopixel sb_led", "neopixel my_strip"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "neopixel my_strip");
    }

    SECTION("Priority 4: logo LED excluded as toolhead LED") {
        std::vector<std::string> leds = {"neopixel logo_led", "neopixel my_strip"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "neopixel my_strip");
    }

    SECTION("Priority 4: nozzle LED excluded as toolhead LED") {
        std::vector<std::string> leds = {"neopixel nozzle_led", "neopixel my_strip"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "neopixel my_strip");
    }

    SECTION("Priority 4: toolhead status LED excluded") {
        std::vector<std::string> leds = {"neopixel toolhead_leds", "neopixel my_strip"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "neopixel my_strip");
    }

    SECTION("Priority 3: toolhead_light matched by 'light' keyword before exclusion") {
        std::vector<std::string> leds = {"neopixel toolhead_light"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // "toolhead_light" contains "light" -> matched at Priority 3
        REQUIRE(hw.guess_main_led_strip() == "neopixel toolhead_light");
    }

    SECTION("No room lighting: all LEDs are status/toolhead, fallback to first") {
        std::vector<std::string> leds = {"status_indicator", "corner_indicators"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "status_indicator");
    }

    SECTION("No room lighting: only sb_leds exists, fallback to sb_leds") {
        std::vector<std::string> leds = {"neopixel sb_leds"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Better to control toolhead LEDs than show a broken button
        REQUIRE(hw.guess_main_led_strip() == "neopixel sb_leds");
    }

    SECTION("Empty LEDs list: returns empty string") {
        std::vector<std::string> leds = {};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_main_led_strip() == "");
    }

    SECTION("Case sensitivity test: 'Case' vs 'case'") {
        std::vector<std::string> leds = {"Case_Lights", "neopixel other"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // 'Case' does not contain 'case' (case sensitive)
        // So it won't match priority 1, falls through to Priority 4 generic
        REQUIRE(hw.guess_main_led_strip() == "Case_Lights");
    }

    SECTION("Single status LED: fallback to it") {
        std::vector<std::string> leds = {"status_indicator"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Better to control something than show a broken button
        REQUIRE(hw.guess_main_led_strip() == "status_indicator");
    }

    SECTION("Fallback: priority still prefers case/chamber over sb_leds") {
        std::vector<std::string> leds = {"neopixel sb_leds", "neopixel case_lights"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // case_lights matches Priority 1 ("case"), so it wins over fallback
        REQUIRE(hw.guess_main_led_strip() == "neopixel case_lights");
    }

    SECTION("Fallback: sb_leds selected when only toolhead LEDs exist") {
        std::vector<std::string> leds = {"neopixel sb_leds", "neopixel logo_led"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Both are toolhead LEDs, fallback returns first
        REQUIRE(hw.guess_main_led_strip() == "neopixel sb_leds");
    }
}

// ============================================================================
// Real-world Mock Data Tests
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "PrinterHardware with MoonrakerClientMock data",
                 "[printer][guessing][mock]") {
    SECTION("VORON_24 mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.discover_printer([]() {}); // Populate hardware

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }

    SECTION("VORON_TRIDENT mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_TRIDENT);
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }

    SECTION("CREALITY_K1 mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K1);
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }

    SECTION("FLASHFORGE_AD5M mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_AD5M);
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }

    SECTION("GENERIC_COREXY mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }

    SECTION("GENERIC_BEDSLINGER mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::GENERIC_BEDSLINGER);
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }

    SECTION("MULTI_EXTRUDER mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::MULTI_EXTRUDER);
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder"); // Base extruder prioritized
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
        REQUIRE(hw.guess_hotend_sensor() == "extruder");
    }
}

// ============================================================================
// Edge Cases and Complex Scenarios
// ============================================================================

TEST_CASE("PrinterHardware edge cases", "[printer][guessing][edge]") {
    std::vector<std::string> sensors;
    std::vector<std::string> fans;
    std::vector<std::string> leds;

    SECTION("Bed heater with unusual name: 'bed' only") {
        std::vector<std::string> heaters = {"extruder", "bed"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "bed");
    }

    SECTION("Hotend heater with unusual name: 'hotend' only") {
        std::vector<std::string> heaters = {"heater_bed", "hotend"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "hotend");
    }

    SECTION("Names containing but not matching: 'extruder_bed' for bed") {
        std::vector<std::string> heaters = {"extruder", "extruder_bed"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "extruder_bed");
    }

    SECTION("Names containing but not matching: 'bed_extruder' for hotend") {
        std::vector<std::string> heaters = {"heater_bed", "bed_extruder"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "bed_extruder");
    }

    SECTION("Multiple priority levels: all types present for bed") {
        std::vector<std::string> heaters = {"bed_custom", "heated_bed", "heater_bed", "extruder"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "heater_bed");
    }

    SECTION("Multiple priority levels: all types present for hotend") {
        std::vector<std::string> heaters = {"e0_custom", "hotend",   "extruder1",
                                            "extruder0", "extruder", "heater_bed"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_hotend_heater() == "extruder");
    }

    SECTION("Sensor-only configuration: no heaters, sensors present") {
        std::vector<std::string> heaters = {};
        std::vector<std::string> sensors_local = {"bed_sensor", "extruder_sensor"};
        PrinterHardware hw(heaters, sensors_local, fans, leds);
        REQUIRE(hw.guess_bed_sensor() == "bed_sensor");
        REQUIRE(hw.guess_hotend_sensor() == "extruder_sensor");
    }

    SECTION("Mixed heater/sensor names: heater_bed_sensor") {
        std::vector<std::string> heaters = {"extruder", "heater_bed_sensor"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Should match as bed heater (contains 'bed')
        REQUIRE(hw.guess_bed_heater() == "heater_bed_sensor");
    }

    SECTION("Numeric variants: extruder10 vs extruder1") {
        std::vector<std::string> heaters = {"heater_bed", "extruder10", "extruder1"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // extruder10 appears first in iteration order
        REQUIRE(hw.guess_hotend_heater() == "extruder10");
    }

    SECTION("Empty string in hardware list") {
        std::vector<std::string> heaters = {"", "heater_bed", "extruder"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
    }

    SECTION("Very long hardware name") {
        std::string long_name = "heater_bed_with_very_long_descriptive_name_for_testing_purposes";
        std::vector<std::string> heaters = {"extruder", long_name};
        PrinterHardware hw(heaters, sensors, fans, leds);
        REQUIRE(hw.guess_bed_heater() == long_name);
    }

    SECTION("Unicode/special characters: should still match substring") {
        std::vector<std::string> heaters = {"extruder", "heater_bed_™"};
        PrinterHardware hw(heaters, sensors, fans, leds);
        // Should still match as substring contains 'bed'
        REQUIRE(hw.guess_bed_heater() == "heater_bed_™");
    }
}
