// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_summary.cpp
 * @brief Unit tests for wizard summary step subject initialization
 *
 * Tests that the wizard summary correctly initializes subjects with config
 * values and doesn't suffer from undefined behavior in the subject macros.
 *
 * Bug context: The original code passed the same buffer pointer as both
 * `buffer` and `initial_value` to UI_SUBJECT_INIT_AND_REGISTER_STRING,
 * which caused snprintf(buf, size, "%s", buf) - undefined behavior.
 * On some platforms this corrupted the data, causing blank summary screens.
 */

#include "ui_subject_registry.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Subject Macro Tests
// ============================================================================

TEST_CASE("UI_SUBJECT_INIT_AND_REGISTER_STRING with separate pointers",
          "[wizard][summary][subjects]") {
    // This is the CORRECT usage pattern - source and dest are different
    static lv_subject_t test_subject;
    static char buffer[64];
    const char* initial_value = "Test Value 123";

    UI_SUBJECT_INIT_AND_REGISTER_STRING(test_subject, buffer, initial_value, "test_subject_1");

    // Verify the buffer contains the initial value
    REQUIRE(std::string(buffer) == "Test Value 123");

    // Verify the subject can retrieve the value
    const char* retrieved = lv_subject_get_string(&test_subject);
    REQUIRE(std::string(retrieved) == "Test Value 123");
}

TEST_CASE("UI_SUBJECT_INIT_AND_REGISTER_STRING with c_str() from std::string",
          "[wizard][summary][subjects]") {
    static lv_subject_t test_subject;
    static char buffer[128];
    std::string config_value = "FlashForge Adventurer 5M Pro";

    // Pass the c_str() as initial_value, buffer as destination
    // This is safe because they point to different memory
    UI_SUBJECT_INIT_AND_REGISTER_STRING(test_subject, buffer, config_value.c_str(),
                                        "test_subject_2");

    REQUIRE(std::string(buffer) == "FlashForge Adventurer 5M Pro");
    REQUIRE(std::string(lv_subject_get_string(&test_subject)) == "FlashForge Adventurer 5M Pro");
}

TEST_CASE("UI_SUBJECT_INIT_AND_REGISTER_STRING handles empty string",
          "[wizard][summary][subjects]") {
    static lv_subject_t test_subject;
    static char buffer[64];
    std::string empty_value = "";

    UI_SUBJECT_INIT_AND_REGISTER_STRING(test_subject, buffer, empty_value.c_str(),
                                        "test_subject_3");

    REQUIRE(std::string(buffer) == "");
    REQUIRE(std::string(lv_subject_get_string(&test_subject)) == "");
}

TEST_CASE("UI_SUBJECT_INIT_AND_REGISTER_STRING handles default values",
          "[wizard][summary][subjects]") {
    static lv_subject_t test_subject;
    static char buffer[64];

    // Simulate what happens when config returns a default
    std::string default_value = "Unnamed Printer";

    UI_SUBJECT_INIT_AND_REGISTER_STRING(test_subject, buffer, default_value.c_str(),
                                        "test_subject_4");

    REQUIRE(std::string(buffer) == "Unnamed Printer");
    REQUIRE(std::string(lv_subject_get_string(&test_subject)) == "Unnamed Printer");
}

TEST_CASE("UI_SUBJECT_INIT_AND_REGISTER_STRING handles special characters",
          "[wizard][summary][subjects]") {
    static lv_subject_t test_subject;
    static char buffer[128];

    // Config values might contain special characters
    std::string special_value = "Heater: extruder, Sensor: heater_bed";

    UI_SUBJECT_INIT_AND_REGISTER_STRING(test_subject, buffer, special_value.c_str(),
                                        "test_subject_5");

    REQUIRE(std::string(buffer) == "Heater: extruder, Sensor: heater_bed");
}

TEST_CASE("UI_SUBJECT_INIT_AND_REGISTER_INT works correctly", "[wizard][summary][subjects]") {
    static lv_subject_t test_subject;

    UI_SUBJECT_INIT_AND_REGISTER_INT(test_subject, 42, "test_int_subject");

    REQUIRE(lv_subject_get_int(&test_subject) == 42);
}

TEST_CASE("UI_SUBJECT_INIT_AND_REGISTER_INT visibility flag pattern",
          "[wizard][summary][subjects]") {
    // Test the visibility flag pattern used in wizard summary
    static lv_subject_t visible_subject;
    static lv_subject_t hidden_subject;

    // Part fan visible (has a value)
    std::string part_fan = "fan_generic part_fan";
    int visible = (part_fan != "None") ? 1 : 0;
    UI_SUBJECT_INIT_AND_REGISTER_INT(visible_subject, visible, "test_visible_1");
    REQUIRE(lv_subject_get_int(&visible_subject) == 1);

    // LED strip not visible (set to "None")
    std::string led_strip = "None";
    int hidden = (led_strip != "None") ? 1 : 0;
    UI_SUBJECT_INIT_AND_REGISTER_INT(hidden_subject, hidden, "test_visible_2");
    REQUIRE(lv_subject_get_int(&hidden_subject) == 0);
}

// ============================================================================
// Moonraker Connection String Format Test
// ============================================================================

TEST_CASE("Moonraker connection string formatting", "[wizard][summary]") {
    // Test the pattern used for moonraker_connection subject
    std::string moonraker_host = "192.168.1.100";
    int moonraker_port = 7125;

    std::string moonraker_connection;
    if (moonraker_host != "Not configured") {
        moonraker_connection = moonraker_host + ":" + std::to_string(moonraker_port);
    } else {
        moonraker_connection = "Not configured";
    }

    REQUIRE(moonraker_connection == "192.168.1.100:7125");

    // Test default case
    moonraker_host = "Not configured";
    if (moonraker_host != "Not configured") {
        moonraker_connection = moonraker_host + ":" + std::to_string(moonraker_port);
    } else {
        moonraker_connection = "Not configured";
    }

    REQUIRE(moonraker_connection == "Not configured");
}
