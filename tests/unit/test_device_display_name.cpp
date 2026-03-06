// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "device_display_name.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// get_display_name() - Direct Mappings
// ============================================================================

TEST_CASE("get_display_name() direct mappings", "[device_display_name][direct]") {
    SECTION("Part cooling fan") {
        REQUIRE(get_display_name("fan", DeviceType::FAN) == "Part Cooling Fan");
    }

    SECTION("Bed heater") {
        REQUIRE(get_display_name("heater_bed", DeviceType::HEATER) == "Bed Heater");
    }

    SECTION("Hotend heater") {
        REQUIRE(get_display_name("extruder", DeviceType::HEATER) == "Hotend Heater");
    }

    SECTION("Extruder as temp sensor") {
        REQUIRE(get_display_name("extruder", DeviceType::TEMP_SENSOR) == "Hotend Temperature");
    }

    SECTION("Bed as temp sensor") {
        REQUIRE(get_display_name("heater_bed", DeviceType::TEMP_SENSOR) == "Bed Temperature");
    }
}

// ============================================================================
// get_display_name() - Type-Aware Suffixes
// ============================================================================

TEST_CASE("get_display_name() type-aware suffixes", "[device_display_name][suffix]") {
    SECTION("Same name, different types") {
        REQUIRE(get_display_name("chamber", DeviceType::FAN) == "Chamber Fan");
        REQUIRE(get_display_name("chamber", DeviceType::TEMP_SENSOR) == "Chamber Temperature");
        REQUIRE(get_display_name("chamber", DeviceType::LED) == "Chamber LED");
        REQUIRE(get_display_name("chamber", DeviceType::HEATER) == "Chamber Heater");
    }

    SECTION("Filament sensor suffix") {
        REQUIRE(get_display_name("toolhead", DeviceType::FILAMENT_SENSOR) == "Toolhead Sensor");
        REQUIRE(get_display_name("entry", DeviceType::FILAMENT_SENSOR) == "Entry Sensor");
    }

    SECTION("Generic type has no suffix") {
        REQUIRE(get_display_name("chamber", DeviceType::GENERIC) == "Chamber");
        REQUIRE(get_display_name("electronics", DeviceType::GENERIC) == "Electronics");
    }
}

// ============================================================================
// get_display_name() - Prefix Stripping
// ============================================================================

TEST_CASE("get_display_name() prefix stripping", "[device_display_name][prefix]") {
    SECTION("Heater fan prefix") {
        REQUIRE(get_display_name("heater_fan hotend_fan", DeviceType::FAN) == "Hotend Fan");
    }

    SECTION("Controller fan prefix") {
        REQUIRE(get_display_name("controller_fan electronics", DeviceType::FAN) ==
                "Electronics Fan");
    }

    SECTION("Fan generic prefix") {
        REQUIRE(get_display_name("fan_generic nevermore", DeviceType::FAN) == "Nevermore Fan");
    }

    SECTION("Neopixel prefix") {
        REQUIRE(get_display_name("neopixel chamber_led", DeviceType::LED) == "Chamber LED");
    }

    SECTION("LED prefix") {
        REQUIRE(get_display_name("led status", DeviceType::LED) == "Status LED");
    }

    SECTION("Dotstar prefix") {
        REQUIRE(get_display_name("dotstar case_light", DeviceType::LED) == "Case Light");
    }

    SECTION("Filament switch sensor prefix") {
        // "runout" alone doesn't imply sensor, so suffix is added
        REQUIRE(get_display_name("filament_switch_sensor runout", DeviceType::FILAMENT_SENSOR) ==
                "Runout Sensor");
        // "runout_sensor" contains "sensor" so no suffix added
        REQUIRE(get_display_name("filament_switch_sensor runout_sensor",
                                 DeviceType::FILAMENT_SENSOR) == "Runout Sensor");
    }

    SECTION("Filament motion sensor prefix") {
        REQUIRE(get_display_name("filament_motion_sensor encoder", DeviceType::FILAMENT_SENSOR) ==
                "Encoder Sensor");
    }

    SECTION("Temperature sensor prefix") {
        REQUIRE(get_display_name("temperature_sensor chamber", DeviceType::TEMP_SENSOR) ==
                "Chamber Temperature");
    }

    SECTION("Heater generic prefix") {
        REQUIRE(get_display_name("heater_generic chamber", DeviceType::HEATER) == "Chamber Heater");
    }
}

// ============================================================================
// get_display_name() - Redundant Suffix Avoidance
// ============================================================================

TEST_CASE("get_display_name() avoids redundant suffixes", "[device_display_name][redundant]") {
    SECTION("Already has 'fan' in name") {
        REQUIRE(get_display_name("exhaust_fan", DeviceType::FAN) == "Exhaust Fan");
        REQUIRE(get_display_name("hotend_fan", DeviceType::FAN) == "Hotend Fan");
        // NOT "Exhaust Fan Fan" or "Hotend Fan Fan"
    }

    SECTION("Already has 'cooling' in name") {
        REQUIRE(get_display_name("part_cooling", DeviceType::FAN) == "Part Cooling");
    }

    SECTION("Already has 'led' in name") {
        REQUIRE(get_display_name("led_strip", DeviceType::LED) == "LED Strip");
        REQUIRE(get_display_name("status_led", DeviceType::LED) == "Status LED");
        // NOT "LED Strip LED" or "Status LED LED"
    }

    SECTION("Already has 'light' in name") {
        REQUIRE(get_display_name("case_light", DeviceType::LED) == "Case Light");
        REQUIRE(get_display_name("chamber_lights", DeviceType::LED) == "Chamber Lights");
    }

    SECTION("Already has 'sensor' in name") {
        REQUIRE(get_display_name("runout_sensor", DeviceType::FILAMENT_SENSOR) == "Runout Sensor");
        // NOT "Runout Sensor Sensor"
    }

    SECTION("Already has 'runout' in name") {
        // Note: "runout" alone doesn't contain "sensor", so suffix IS added
        // If you want "Filament Runout" without suffix, name it "filament_runout_sensor"
        REQUIRE(get_display_name("filament_runout", DeviceType::FILAMENT_SENSOR) ==
                "Filament Runout Sensor");
        // "filament_runout_sensor" contains "sensor" so no suffix
        REQUIRE(get_display_name("filament_runout_sensor", DeviceType::FILAMENT_SENSOR) ==
                "Filament Runout Sensor");
    }

    SECTION("Already has 'heater' in name") {
        REQUIRE(get_display_name("bed_heater", DeviceType::HEATER) == "Bed Heater");
        // NOT "Bed Heater Heater"
    }

    SECTION("Already has 'temp' or 'temperature' in name") {
        REQUIRE(get_display_name("chamber_temp", DeviceType::TEMP_SENSOR) == "Chamber Temperature");
        REQUIRE(get_display_name("ambient_temperature", DeviceType::TEMP_SENSOR) ==
                "Ambient Temperature");
    }
}

// ============================================================================
// get_display_name() - Special Word Handling
// ============================================================================

TEST_CASE("get_display_name() special word handling", "[device_display_name][special]") {
    SECTION("LED uppercase") {
        REQUIRE(get_display_name("led_strip", DeviceType::LED) == "LED Strip");
        REQUIRE(get_display_name("case_led", DeviceType::LED) == "Case LED");
    }

    SECTION("PSU uppercase") {
        REQUIRE(get_display_name("psu_control", DeviceType::POWER_DEVICE) == "PSU Control");
        REQUIRE(get_display_name("printer_psu", DeviceType::POWER_DEVICE) == "Printer PSU");
    }

    SECTION("USB uppercase") {
        REQUIRE(get_display_name("usb_hub", DeviceType::POWER_DEVICE) == "USB Hub");
    }

    SECTION("GPIO uppercase") {
        REQUIRE(get_display_name("gpio_relay", DeviceType::POWER_DEVICE) == "GPIO Relay");
    }

    SECTION("AC/DC uppercase") {
        REQUIRE(get_display_name("ac_inlet", DeviceType::POWER_DEVICE) == "AC Inlet");
        REQUIRE(get_display_name("dc_output", DeviceType::POWER_DEVICE) == "DC Output");
    }

    SECTION("AMS/AFC/ERCF/MMU uppercase") {
        REQUIRE(get_display_name("ams_hub", DeviceType::GENERIC) == "AMS Hub");
        REQUIRE(get_display_name("afc_unit", DeviceType::GENERIC) == "AFC Unit");
        REQUIRE(get_display_name("ercf_gear", DeviceType::GENERIC) == "ERCF Gear");
        REQUIRE(get_display_name("mmu_selector", DeviceType::GENERIC) == "MMU Selector");
    }

    SECTION("MCU/CPU uppercase") {
        REQUIRE(get_display_name("mcu_temp", DeviceType::TEMP_SENSOR) == "MCU Temperature");
        REQUIRE(get_display_name("cpu_temp", DeviceType::TEMP_SENSOR) == "CPU Temperature");
    }

    SECTION("Abbreviation expansions") {
        REQUIRE(get_display_name("aux_relay", DeviceType::POWER_DEVICE) == "Auxiliary Relay");
        REQUIRE(get_display_name("enc_heater", DeviceType::HEATER) == "Enclosure Heater");
        REQUIRE(get_display_name("cam_light", DeviceType::LED) == "Camera Light");
    }
}

// ============================================================================
// get_display_name() - Macro Handling
// ============================================================================

TEST_CASE("get_display_name() macro handling", "[device_display_name][macro]") {
    SECTION("Basic macro") {
        REQUIRE(get_display_name("LOAD_FILAMENT", DeviceType::MACRO) == "LOAD FILAMENT");
    }

    SECTION("Macro with leading underscore") {
        REQUIRE(get_display_name("_HEAT_NOZZLE", DeviceType::MACRO) == "HEAT NOZZLE");
    }

    SECTION("Macro with HELIX_ prefix") {
        REQUIRE(get_display_name("HELIX_LOAD_FILAMENT", DeviceType::MACRO) == "LOAD FILAMENT");
    }

    SECTION("Lowercase macro") {
        REQUIRE(get_display_name("home_all", DeviceType::MACRO) == "Home All");
    }
}

// ============================================================================
// extract_device_suffix()
// ============================================================================

TEST_CASE("extract_device_suffix()", "[device_display_name][suffix]") {
    SECTION("With recognized prefix") {
        REQUIRE(extract_device_suffix("heater_fan hotend") == "hotend");
        REQUIRE(extract_device_suffix("neopixel chamber") == "chamber");
        REQUIRE(extract_device_suffix("filament_switch_sensor runout") == "runout");
    }

    SECTION("Without space (no prefix)") {
        REQUIRE(extract_device_suffix("fan") == "fan");
        REQUIRE(extract_device_suffix("extruder") == "extruder");
    }

    SECTION("With unrecognized prefix") {
        // Unknown prefixes should return the full name
        REQUIRE(extract_device_suffix("unknown_prefix something") == "unknown_prefix something");
    }

    SECTION("Empty string") {
        REQUIRE(extract_device_suffix("") == "");
    }
}

// ============================================================================
// prettify_name()
// ============================================================================

TEST_CASE("prettify_name() snake_case conversion", "[device_display_name][prettify]") {
    SECTION("Basic conversion") {
        REQUIRE(prettify_name("hotend_fan") == "Hotend Fan");
        REQUIRE(prettify_name("chamber_led") == "Chamber LED");
    }

    SECTION("Single word") {
        REQUIRE(prettify_name("chamber") == "Chamber");
        REQUIRE(prettify_name("nevermore") == "Nevermore");
    }

    SECTION("Multiple underscores") {
        REQUIRE(prettify_name("part_cooling_fan") == "Part Cooling Fan");
        REQUIRE(prettify_name("print_chamber_exhaust") == "Print Chamber Exhaust");
    }

    SECTION("With hyphens") {
        REQUIRE(prettify_name("case-light") == "Case Light");
        REQUIRE(prettify_name("led-strip") == "LED Strip");
    }

    SECTION("Mixed case input normalized") {
        REQUIRE(prettify_name("HoTeNd_FaN") == "Hotend Fan");
        REQUIRE(prettify_name("LED_STRIP") == "LED STRIP");
    }

    SECTION("Leading underscore stripped") {
        REQUIRE(prettify_name("_hidden_macro") == "Hidden Macro");
    }

    SECTION("HELIX_ prefix stripped") {
        REQUIRE(prettify_name("HELIX_LOAD_FILAMENT") == "LOAD FILAMENT");
    }

    SECTION("Special words replaced") {
        REQUIRE(prettify_name("psu_led_strip") == "PSU LED Strip");
        REQUIRE(prettify_name("aux_gpio_relay") == "Auxiliary GPIO Relay");
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("get_display_name() edge cases", "[device_display_name][edge]") {
    SECTION("Empty string") {
        REQUIRE(get_display_name("", DeviceType::FAN) == "");
        REQUIRE(get_display_name("", DeviceType::LED) == "");
    }

    SECTION("Prefix only (no suffix after space)") {
        // When raw name is just the type prefix (e.g., "neopixel" with no instance name),
        // and it's also in skip words, we don't add a suffix since the type is obvious
        REQUIRE(get_display_name("neopixel", DeviceType::LED) == "Neopixel");
    }

    SECTION("Single character") {
        REQUIRE(get_display_name("x", DeviceType::FAN) == "X Fan");
    }

    SECTION("Numbers in name") {
        // "fan_1" contains "fan" so no suffix added
        REQUIRE(get_display_name("fan_1", DeviceType::FAN) == "Fan 1");
        REQUIRE(get_display_name("led_strip_2", DeviceType::LED) == "LED Strip 2");
    }
}

// ============================================================================
// Real-World Examples (from user requirements)
// ============================================================================

TEST_CASE("get_display_name() real-world examples", "[device_display_name][examples]") {
    SECTION("User examples from requirements") {
        // heater "heater_bed" --> "Bed Heater" (direct mapping)
        REQUIRE(get_display_name("heater_bed", DeviceType::HEATER) == "Bed Heater");

        // fan_generic "chamber" --> "Chamber Fan" (Klipper: "fan_generic chamber")
        REQUIRE(get_display_name("fan_generic chamber", DeviceType::FAN) == "Chamber Fan");

        // led "chamber_led" -> "Chamber LED"
        REQUIRE(get_display_name("chamber_led", DeviceType::LED) == "Chamber LED");

        // "chamber_light" -> "Chamber Light"
        REQUIRE(get_display_name("chamber_light", DeviceType::LED) == "Chamber Light");

        // filament_switch_sensor "runout_sensor" -> "Runout Sensor"
        REQUIRE(get_display_name("filament_switch_sensor runout_sensor",
                                 DeviceType::FILAMENT_SENSOR) == "Runout Sensor");

        // temp sensor "extruder" -> "Hotend Temperature" (via direct mapping)
        REQUIRE(get_display_name("extruder", DeviceType::TEMP_SENSOR) == "Hotend Temperature");

        // temp sensor "heater_bed" -> "Bed Temperature"
        REQUIRE(get_display_name("heater_bed", DeviceType::TEMP_SENSOR) == "Bed Temperature");
    }

    SECTION("Common Klipper configurations") {
        // Voron-style fans
        REQUIRE(get_display_name("heater_fan hotend_fan", DeviceType::FAN) == "Hotend Fan");
        REQUIRE(get_display_name("controller_fan controller_fan", DeviceType::FAN) ==
                "Controller Fan");
        REQUIRE(get_display_name("fan_generic nevermore", DeviceType::FAN) == "Nevermore Fan");
        // "bed_fans" contains "fans" (plural) which is in skip words, so no suffix
        REQUIRE(get_display_name("fan_generic bed_fans", DeviceType::FAN) == "Bed Fans");

        // Voron-style LEDs - "sb_leds" contains "leds" so no suffix
        REQUIRE(get_display_name("neopixel sb_leds", DeviceType::LED) == "Sb Leds");
        REQUIRE(get_display_name("neopixel caselight", DeviceType::LED) == "Caselight LED");

        // Temperature sensors
        REQUIRE(get_display_name("temperature_sensor chamber", DeviceType::TEMP_SENSOR) ==
                "Chamber Temperature");
        REQUIRE(get_display_name("temperature_sensor raspberry_pi", DeviceType::TEMP_SENSOR) ==
                "Raspberry Pi Temperature");
    }
}
