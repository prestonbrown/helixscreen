// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_hardware_validator.cpp
 * @brief Unit tests for HardwareValidator hardware validation system
 *
 * Tests the HardwareValidator class which validates config expectations
 * against Moonraker hardware discovery:
 * - HardwareSnapshot serialization/comparison
 * - HardwareValidationResult aggregation
 * - Critical hardware detection
 * - Configured vs discovered hardware validation
 * - Optional hardware marking
 */

#include "hardware_validator.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

// ============================================================================
// HardwareSnapshot Tests
// ============================================================================

TEST_CASE("HardwareSnapshot - JSON serialization", "[hardware][validator]") {
    SECTION("Serializes to JSON correctly") {
        HardwareSnapshot snapshot;
        snapshot.timestamp = "2025-01-01T12:00:00Z";
        snapshot.heaters = {"extruder", "heater_bed"};
        snapshot.sensors = {"temperature_sensor chamber"};
        snapshot.fans = {"fan", "heater_fan hotend_fan"};
        snapshot.leds = {"neopixel chamber_light"};
        snapshot.filament_sensors = {"filament_switch_sensor fsensor"};

        json j = snapshot.to_json();

        REQUIRE(j["timestamp"] == "2025-01-01T12:00:00Z");
        REQUIRE(j["heaters"].size() == 2);
        REQUIRE(j["heaters"][0] == "extruder");
        REQUIRE(j["heaters"][1] == "heater_bed");
        REQUIRE(j["sensors"].size() == 1);
        REQUIRE(j["fans"].size() == 2);
        REQUIRE(j["leds"].size() == 1);
        REQUIRE(j["filament_sensors"].size() == 1);
    }

    SECTION("Deserializes from JSON correctly") {
        json j = {{"timestamp", "2025-01-01T12:00:00Z"},
                  {"heaters", {"extruder", "heater_bed"}},
                  {"sensors", {"temperature_sensor chamber"}},
                  {"fans", {"fan"}},
                  {"leds", {"neopixel test"}},
                  {"filament_sensors", {"filament_switch_sensor fs"}}};

        HardwareSnapshot snapshot = HardwareSnapshot::from_json(j);

        REQUIRE(snapshot.timestamp == "2025-01-01T12:00:00Z");
        REQUIRE(snapshot.heaters.size() == 2);
        REQUIRE(snapshot.sensors.size() == 1);
        REQUIRE(snapshot.fans.size() == 1);
        REQUIRE(snapshot.leds.size() == 1);
        REQUIRE(snapshot.filament_sensors.size() == 1);
    }

    SECTION("Handles missing fields gracefully") {
        json j = {{"timestamp", "2025-01-01T12:00:00Z"}, {"heaters", {"extruder"}}};

        HardwareSnapshot snapshot = HardwareSnapshot::from_json(j);

        REQUIRE(snapshot.timestamp == "2025-01-01T12:00:00Z");
        REQUIRE(snapshot.heaters.size() == 1);
        REQUIRE(snapshot.sensors.empty());
        REQUIRE(snapshot.fans.empty());
        REQUIRE(snapshot.leds.empty());
        REQUIRE(snapshot.filament_sensors.empty());
    }

    SECTION("Returns empty snapshot on invalid JSON") {
        json j = "not an object";

        HardwareSnapshot snapshot = HardwareSnapshot::from_json(j);

        REQUIRE(snapshot.is_empty());
    }
}

TEST_CASE("HardwareSnapshot - Comparison", "[hardware][validator]") {
    SECTION("get_removed finds items in old but not in current") {
        HardwareSnapshot old_snapshot;
        old_snapshot.heaters = {"extruder", "heater_bed"};
        old_snapshot.fans = {"fan", "heater_fan hotend_fan"};
        old_snapshot.leds = {"neopixel chamber_light"};

        HardwareSnapshot current;
        current.heaters = {"extruder", "heater_bed"};
        current.fans = {"fan"}; // hotend_fan removed
        current.leds = {};      // LED removed

        auto removed = old_snapshot.get_removed(current);

        REQUIRE(removed.size() == 2);
        REQUIRE(std::find(removed.begin(), removed.end(), "heater_fan hotend_fan") !=
                removed.end());
        REQUIRE(std::find(removed.begin(), removed.end(), "neopixel chamber_light") !=
                removed.end());
    }

    SECTION("get_added finds items in current but not in old") {
        HardwareSnapshot old_snapshot;
        old_snapshot.heaters = {"extruder"};
        old_snapshot.fans = {"fan"};

        HardwareSnapshot current;
        current.heaters = {"extruder", "heater_bed"}; // bed added
        current.fans = {"fan", "controller_fan mcu"}; // controller fan added
        current.leds = {"neopixel strip"};            // LED added

        auto added = old_snapshot.get_added(current);

        REQUIRE(added.size() == 3);
        REQUIRE(std::find(added.begin(), added.end(), "heater_bed") != added.end());
        REQUIRE(std::find(added.begin(), added.end(), "controller_fan mcu") != added.end());
        REQUIRE(std::find(added.begin(), added.end(), "neopixel strip") != added.end());
    }

    SECTION("Returns empty when snapshots are identical") {
        HardwareSnapshot snapshot;
        snapshot.heaters = {"extruder", "heater_bed"};
        snapshot.fans = {"fan"};

        REQUIRE(snapshot.get_removed(snapshot).empty());
        REQUIRE(snapshot.get_added(snapshot).empty());
    }
}

TEST_CASE("HardwareSnapshot - is_empty", "[hardware][validator]") {
    SECTION("Returns true for default-constructed snapshot") {
        HardwareSnapshot snapshot;
        REQUIRE(snapshot.is_empty());
    }

    SECTION("Returns false when any list has items") {
        HardwareSnapshot snapshot;

        snapshot.heaters = {"extruder"};
        REQUIRE_FALSE(snapshot.is_empty());

        snapshot.heaters.clear();
        snapshot.fans = {"fan"};
        REQUIRE_FALSE(snapshot.is_empty());
    }
}

// ============================================================================
// HardwareIssue Factory Tests
// ============================================================================

TEST_CASE("HardwareIssue - Factory methods", "[hardware][validator]") {
    SECTION("critical() creates CRITICAL severity issue") {
        auto issue = HardwareIssue::critical("extruder", HardwareType::HEATER, "Missing extruder");

        REQUIRE(issue.hardware_name == "extruder");
        REQUIRE(issue.hardware_type == HardwareType::HEATER);
        REQUIRE(issue.severity == HardwareIssueSeverity::CRITICAL);
        REQUIRE(issue.message == "Missing extruder");
        REQUIRE_FALSE(issue.is_optional);
    }

    SECTION("warning() creates WARNING severity issue") {
        auto issue =
            HardwareIssue::warning("neopixel test", HardwareType::LED, "LED not found", true);

        REQUIRE(issue.hardware_name == "neopixel test");
        REQUIRE(issue.hardware_type == HardwareType::LED);
        REQUIRE(issue.severity == HardwareIssueSeverity::WARNING);
        REQUIRE(issue.is_optional == true);
    }

    SECTION("info() creates INFO severity issue") {
        auto issue = HardwareIssue::info("filament_switch_sensor fs", HardwareType::FILAMENT_SENSOR,
                                         "New sensor discovered");

        REQUIRE(issue.severity == HardwareIssueSeverity::INFO);
        REQUIRE_FALSE(issue.is_optional);
    }
}

// ============================================================================
// HardwareValidationResult Tests
// ============================================================================

TEST_CASE("HardwareValidationResult - Aggregation", "[hardware][validator]") {
    SECTION("has_issues returns false when empty") {
        HardwareValidationResult result;
        REQUIRE_FALSE(result.has_issues());
    }

    SECTION("has_issues returns true with any issues") {
        HardwareValidationResult result;
        result.newly_discovered.push_back(
            HardwareIssue::info("neopixel test", HardwareType::LED, "New LED"));

        REQUIRE(result.has_issues());
    }

    SECTION("has_critical returns true only for critical issues") {
        HardwareValidationResult result;

        // Add warning - not critical
        result.expected_missing.push_back(
            HardwareIssue::warning("fan", HardwareType::FAN, "Missing"));
        REQUIRE_FALSE(result.has_critical());

        // Add critical - now critical
        result.critical_missing.push_back(
            HardwareIssue::critical("extruder", HardwareType::HEATER, "Missing"));
        REQUIRE(result.has_critical());
    }

    SECTION("total_issue_count sums all categories") {
        HardwareValidationResult result;
        result.critical_missing.push_back(
            HardwareIssue::critical("extruder", HardwareType::HEATER, "Missing"));
        result.expected_missing.push_back(
            HardwareIssue::warning("fan", HardwareType::FAN, "Missing"));
        result.expected_missing.push_back(
            HardwareIssue::warning("led", HardwareType::LED, "Missing"));
        result.newly_discovered.push_back(
            HardwareIssue::info("sensor", HardwareType::SENSOR, "New"));

        REQUIRE(result.total_issue_count() == 4);
    }

    SECTION("max_severity returns highest severity") {
        HardwareValidationResult result;

        // Empty = INFO (default)
        REQUIRE(result.max_severity() == HardwareIssueSeverity::INFO);

        // Add info
        result.newly_discovered.push_back(HardwareIssue::info("led", HardwareType::LED, "New"));
        REQUIRE(result.max_severity() == HardwareIssueSeverity::INFO);

        // Add warning - now WARNING
        result.expected_missing.push_back(
            HardwareIssue::warning("fan", HardwareType::FAN, "Missing"));
        REQUIRE(result.max_severity() == HardwareIssueSeverity::WARNING);

        // Add critical - now CRITICAL
        result.critical_missing.push_back(
            HardwareIssue::critical("extruder", HardwareType::HEATER, "Missing"));
        REQUIRE(result.max_severity() == HardwareIssueSeverity::CRITICAL);
    }
}

// ============================================================================
// HardwareValidator Tests
// ============================================================================

TEST_CASE("HardwareValidator - Critical hardware detection", "[hardware][validator]") {
    MoonrakerClientMock client;

    SECTION("Detects missing extruder as critical") {
        // Mock client with no extruder
        client.set_heaters({"heater_bed"});

        HardwareValidator validator;
        auto result = validator.validate(nullptr, client.hardware());

        REQUIRE(result.has_critical());
        REQUIRE(result.critical_missing.size() == 1);
        REQUIRE(result.critical_missing[0].hardware_name == "extruder");
    }

    SECTION("No critical issue when extruder exists") {
        client.set_heaters({"extruder", "heater_bed"});

        HardwareValidator validator;
        auto result = validator.validate(nullptr, client.hardware());

        REQUIRE_FALSE(result.has_critical());
    }

    SECTION("Detects extruder with numbered variant") {
        client.set_heaters({"extruder0", "heater_bed"});

        HardwareValidator validator;
        auto result = validator.validate(nullptr, client.hardware());

        REQUIRE_FALSE(result.has_critical());
    }
}

TEST_CASE("HardwareValidator - New hardware discovery", "[hardware][validator]") {
    MoonrakerClientMock client;

    SECTION("Suggests LED when discovered but not configured") {
        client.set_heaters({"extruder", "heater_bed"});
        client.set_leds({"neopixel chamber_light"});

        HardwareValidator validator;
        // Pass nullptr for config = no configured LED
        auto result = validator.validate(nullptr, client.hardware());

        // Should suggest the LED
        bool found_led = false;
        for (const auto& issue : result.newly_discovered) {
            if (issue.hardware_type == HardwareType::LED) {
                found_led = true;
                break;
            }
        }
        REQUIRE(found_led);
    }
}

TEST_CASE("HardwareValidator - New fan discovery", "[hardware][validator]") {
    MoonrakerClientMock client;

    SECTION("Flags fan not assigned to any role") {
        client.set_heaters({"extruder", "heater_bed"});
        // "fan" is the default part fan, but "temperature_fan chamber_fan"
        // is an extra fan with no config role assignment
        client.set_fans({"fan", "heater_fan hotend_fan", "temperature_fan chamber_fan"});

        HardwareValidator validator;
        // nullptr config = only "fan" is default part fan role
        auto result = validator.validate(nullptr, client.hardware());

        bool found_chamber_fan = false;
        for (const auto& issue : result.newly_discovered) {
            if (issue.hardware_name == "temperature_fan chamber_fan" &&
                issue.hardware_type == HardwareType::FAN) {
                found_chamber_fan = true;
                break;
            }
        }
        REQUIRE(found_chamber_fan);
    }

    SECTION("Does not flag fans assigned to roles") {
        client.set_heaters({"extruder", "heater_bed"});
        // Only "fan" (default part) and "heater_fan hotend_fan" which is a heater_fan
        client.set_fans({"fan", "heater_fan hotend_fan"});

        HardwareValidator validator;
        auto result = validator.validate(nullptr, client.hardware());

        // heater_fan hotend_fan is not assigned to hotend role (no config),
        // so it should show as newly discovered
        bool found_part_fan = false;
        for (const auto& issue : result.newly_discovered) {
            if (issue.hardware_name == "fan") {
                found_part_fan = true;
            }
        }
        // "fan" is the default part fan role, should NOT be flagged
        REQUIRE_FALSE(found_part_fan);
    }
}

TEST_CASE("HardwareValidator - Session changes", "[hardware][validator]") {
    SECTION("Detects hardware removed since last session") {
        // Create a "previous" snapshot with LED
        HardwareSnapshot previous;
        previous.heaters = {"extruder", "heater_bed"};
        previous.leds = {"neopixel chamber_light"};

        // Current discovery has no LED
        HardwareSnapshot current;
        current.heaters = {"extruder", "heater_bed"};
        current.leds = {};

        auto removed = previous.get_removed(current);

        REQUIRE(removed.size() == 1);
        REQUIRE(removed[0] == "neopixel chamber_light");
    }
}

TEST_CASE("HardwareValidator - Helper functions", "[hardware][validator]") {
    SECTION("hardware_type_to_string returns correct strings") {
        REQUIRE(std::string(hardware_type_to_string(HardwareType::HEATER)) == "heater");
        REQUIRE(std::string(hardware_type_to_string(HardwareType::SENSOR)) == "sensor");
        REQUIRE(std::string(hardware_type_to_string(HardwareType::FAN)) == "fan");
        REQUIRE(std::string(hardware_type_to_string(HardwareType::LED)) == "led");
        REQUIRE(std::string(hardware_type_to_string(HardwareType::FILAMENT_SENSOR)) ==
                "filament_sensor");
        REQUIRE(std::string(hardware_type_to_string(HardwareType::OTHER)) == "hardware");
    }
}

// ============================================================================
// Integration-style Tests
// ============================================================================

TEST_CASE("HardwareValidator - Full validation scenario", "[hardware][validator]") {
    MoonrakerClientMock client;

    SECTION("Healthy printer with all expected hardware") {
        client.set_heaters({"extruder", "heater_bed"});
        client.set_fans({"fan", "heater_fan hotend_fan"});
        client.set_leds({"neopixel chamber_light"});

        HardwareValidator validator;
        auto result = validator.validate(nullptr, client.hardware());

        // No critical issues (extruder present)
        REQUIRE_FALSE(result.has_critical());

        // May have info about new hardware (LED not configured)
        // but no expected_missing since config is null
        REQUIRE(result.expected_missing.empty());
    }

    SECTION("Printer missing extruder reports critical") {
        client.set_heaters({"heater_bed"}); // No extruder!
        client.set_fans({"fan"});

        HardwareValidator validator;
        auto result = validator.validate(nullptr, client.hardware());

        REQUIRE(result.has_critical());
        REQUIRE(result.has_issues());
        REQUIRE(result.max_severity() == HardwareIssueSeverity::CRITICAL);
    }
}

TEST_CASE("HardwareValidator - Snapshot roundtrip", "[hardware][validator]") {
    SECTION("Snapshot survives JSON roundtrip") {
        HardwareSnapshot original;
        original.timestamp = "2025-01-01T12:00:00Z";
        original.heaters = {"extruder", "heater_bed", "heater_generic chamber"};
        original.sensors = {"temperature_sensor raspberry_pi", "temperature_sensor chamber"};
        original.fans = {"fan", "heater_fan hotend_fan", "controller_fan electronics"};
        original.leds = {"neopixel chamber_light", "led status"};
        original.filament_sensors = {"filament_switch_sensor fsensor"};

        // Serialize and deserialize
        json j = original.to_json();
        HardwareSnapshot restored = HardwareSnapshot::from_json(j);

        // Verify all fields match
        REQUIRE(restored.timestamp == original.timestamp);
        REQUIRE(restored.heaters == original.heaters);
        REQUIRE(restored.sensors == original.sensors);
        REQUIRE(restored.fans == original.fans);
        REQUIRE(restored.leds == original.leds);
        REQUIRE(restored.filament_sensors == original.filament_sensors);
    }
}

// ============================================================================
// Config Integration Tests (Optional/Expected Hardware)
// ============================================================================

#include "../test_helpers/config_test_access.h"
#include "config.h"

// Test fixture for Config-dependent HardwareValidator tests
// NOTE: After the plural naming refactor, hardware config moves from
// /hardware/ to /printer/hardware/
namespace helix {
class HardwareValidatorConfigFixture {
  protected:
    Config config;

    // Helper to check if a JSON pointer path exists in config data
    bool config_contains(const std::string& json_ptr) {
        return ConfigTestAccess::data(config).contains(json::json_pointer(json_ptr));
    }

    // Helper to wrap printer data in v3 format with active_printer_id
    void setup_printer_data(const json& printer_data) {
        helix::setup_printer_data(config, printer_data);
    }

    void setup_empty_hardware_config() {
        setup_printer_data({{"moonraker_host", "127.0.0.1"},
                            {"moonraker_port", 7125},
                            {"hardware",
                             {{"optional", json::array()},
                              {"expected", json::array()},
                              {"last_snapshot", json::object()}}}});
    }

    void setup_hardware_with_optional() {
        setup_printer_data({{"moonraker_host", "127.0.0.1"},
                            {"moonraker_port", 7125},
                            {"hardware",
                             {{"optional", {"neopixel chamber_light", "fan exhaust"}},
                              {"expected", json::array()},
                              {"last_snapshot", json::object()}}}});
    }

    void setup_hardware_with_expected() {
        setup_printer_data({{"moonraker_host", "127.0.0.1"},
                            {"moonraker_port", 7125},
                            {"hardware",
                             {{"optional", json::array()},
                              {"expected", {"temperature_sensor chamber", "neopixel status"}},
                              {"last_snapshot", json::object()}}}});
    }

    void setup_config(const json& j) {
        // For tests that pass full config JSON — wrap in v3 format if needed
        if (j.contains("printer") && !j.contains("printers")) {
            ConfigTestAccess::data(config) = {{"config_version", 3},
                                              {"active_printer_id", "default"},
                                              {"printers", {{"default", j["printer"]}}}};
            ConfigTestAccess::active_printer_id(config) = "default";
        } else {
            ConfigTestAccess::data(config) = j;
            if (j.contains("active_printer_id")) {
                ConfigTestAccess::active_printer_id(config) =
                    j["active_printer_id"].get<std::string>();
            }
        }
    }

    void setup_hardware_with_snapshot() {
        json snapshot = {
            {"timestamp", "2025-01-01T12:00:00Z"},       {"heaters", {"extruder", "heater_bed"}},
            {"sensors", {"temperature_sensor chamber"}}, {"fans", {"fan", "heater_fan hotend_fan"}},
            {"leds", {"neopixel chamber_light"}},        {"filament_sensors", json::array()}};

        setup_printer_data({{"moonraker_host", "127.0.0.1"},
                            {"moonraker_port", 7125},
                            {"hardware",
                             {{"optional", json::array()},
                              {"expected", json::array()},
                              {"last_snapshot", snapshot}}}});
    }
};
} // namespace helix

// Regression: ForgeX AD5M Pro preset assigns five fan roles (part, hotend,
// chamber, exhaust, aux). validate_new_hardware historically only checked
// part/hotend/chamber/exhaust — the aux fan surfaced as "new hardware" on
// every boot even though the preset legitimately mapped it to a role.
TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - aux fan role is recognized as configured",
                 "[hardware][validator][regression]") {
    setup_printer_data({{"moonraker_host", "127.0.0.1"},
                        {"moonraker_port", 7125},
                        {"fans",
                         {{"part", "fan"},
                          {"hotend", "heater_fan hotend_fan"},
                          {"chamber", "fan_generic chamber_fan"},
                          {"exhaust", "fan_generic external_fan"},
                          {"aux", "fan_generic internal_fan"}}},
                        {"hardware",
                         {{"optional", json::array()},
                          {"expected", json::array()},
                          {"last_snapshot", json::object()}}}});

    MoonrakerClientMock client;
    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan", "heater_fan hotend_fan", "fan_generic chamber_fan",
                     "fan_generic external_fan", "fan_generic internal_fan"});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    // Every fan in the discovery is mapped to a role in config — none should
    // surface as newly discovered.
    for (const auto& issue : result.newly_discovered) {
        INFO("unexpected new fan: " << issue.hardware_name);
        REQUIRE(issue.hardware_type != HardwareType::FAN);
    }
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - is_hardware_optional with empty config",
                 "[hardware][validator][config]") {
    setup_empty_hardware_config();

    REQUIRE_FALSE(HardwareValidator::is_hardware_optional(&config, "neopixel chamber_light"));
    REQUIRE_FALSE(HardwareValidator::is_hardware_optional(&config, "anything"));
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - is_hardware_optional detects optional hardware",
                 "[hardware][validator][config]") {
    setup_hardware_with_optional();

    REQUIRE(HardwareValidator::is_hardware_optional(&config, "neopixel chamber_light"));
    REQUIRE(HardwareValidator::is_hardware_optional(&config, "fan exhaust"));
    REQUIRE_FALSE(HardwareValidator::is_hardware_optional(&config, "not_in_list"));
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - set_hardware_optional adds to list",
                 "[hardware][validator][config]") {
    setup_empty_hardware_config();

    // Initially not optional
    REQUIRE_FALSE(HardwareValidator::is_hardware_optional(&config, "new_hardware"));

    // Mark as optional
    HardwareValidator::set_hardware_optional(&config, "new_hardware", true);

    // Now should be optional
    REQUIRE(HardwareValidator::is_hardware_optional(&config, "new_hardware"));
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - set_hardware_optional removes from list",
                 "[hardware][validator][config]") {
    setup_hardware_with_optional();

    // Initially optional
    REQUIRE(HardwareValidator::is_hardware_optional(&config, "neopixel chamber_light"));

    // Unmark as optional
    HardwareValidator::set_hardware_optional(&config, "neopixel chamber_light", false);

    // Should no longer be optional
    REQUIRE_FALSE(HardwareValidator::is_hardware_optional(&config, "neopixel chamber_light"));
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - set_hardware_optional handles duplicates",
                 "[hardware][validator][config]") {
    setup_empty_hardware_config();

    // Add twice - should only appear once
    HardwareValidator::set_hardware_optional(&config, "test_hw", true);
    HardwareValidator::set_hardware_optional(&config, "test_hw", true);

    // Check the list only has one entry
    // NEW path: /printer/hardware/optional (was /hardware/optional)
    json& optional_list = config.get_json(config.df() + "hardware/optional");
    int count = 0;
    for (const auto& item : optional_list) {
        if (item == "test_hw") {
            count++;
        }
    }
    REQUIRE(count == 1);
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - add_expected_hardware adds to list",
                 "[hardware][validator][config]") {
    setup_empty_hardware_config();

    // Add expected hardware
    HardwareValidator::add_expected_hardware(&config, "temperature_sensor chamber");

    // Verify it's in the list
    // NEW path: /printer/hardware/expected (was /hardware/expected)
    json& expected_list = config.get_json(config.df() + "hardware/expected");
    bool found = false;
    for (const auto& item : expected_list) {
        if (item == "temperature_sensor chamber") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - add_expected_hardware handles duplicates",
                 "[hardware][validator][config]") {
    setup_empty_hardware_config();

    // Add same hardware twice
    HardwareValidator::add_expected_hardware(&config, "neopixel test");
    HardwareValidator::add_expected_hardware(&config, "neopixel test");

    // Check the list only has one entry
    // NEW path: /printer/hardware/expected (was /hardware/expected)
    json& expected_list = config.get_json(config.df() + "hardware/expected");
    int count = 0;
    for (const auto& item : expected_list) {
        if (item == "neopixel test") {
            count++;
        }
    }
    REQUIRE(count == 1);
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - add_expected_hardware ignores empty names",
                 "[hardware][validator][config]") {
    setup_empty_hardware_config();

    // Try to add empty name - should be ignored
    HardwareValidator::add_expected_hardware(&config, "");

    // NEW path: /printer/hardware/expected (was /hardware/expected)
    json& expected_list = config.get_json(config.df() + "hardware/expected");
    REQUIRE(expected_list.empty());
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - handles nullptr config gracefully",
                 "[hardware][validator][config]") {
    // These should not crash with nullptr
    REQUIRE_FALSE(HardwareValidator::is_hardware_optional(nullptr, "anything"));

    // These should be no-ops with nullptr (no crash)
    HardwareValidator::set_hardware_optional(nullptr, "test", true);
    HardwareValidator::add_expected_hardware(nullptr, "test");

    // If we got here without crashing, the test passes
    REQUIRE(true);
}

// ============================================================================
// Hardware Path Structure Tests - NEW /printer/hardware/ paths
// These tests define the contract for the refactored hardware config paths.
// They SHOULD FAIL until the implementation is updated.
// ============================================================================

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - optional hardware uses /printer/hardware/optional path",
                 "[hardware][validator][config][paths][plural]") {
    setup_empty_hardware_config();

    // Mark hardware as optional - should write to /printer/hardware/optional
    HardwareValidator::set_hardware_optional(&config, "test_led", true);

    // Verify the path is /printer/hardware/optional (not /hardware/optional)
    REQUIRE(config_contains(config.df() + "hardware/optional"));
    json& optional_list = config.get_json(config.df() + "hardware/optional");
    REQUIRE(optional_list.is_array());

    bool found = false;
    for (const auto& item : optional_list) {
        if (item == "test_led") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - expected hardware uses /printer/hardware/expected path",
                 "[hardware][validator][config][paths][plural]") {
    setup_empty_hardware_config();

    // Add expected hardware - should write to /printer/hardware/expected
    HardwareValidator::add_expected_hardware(&config, "temperature_sensor test");

    // Verify the path is /printer/hardware/expected (not /hardware/expected)
    REQUIRE(config_contains(config.df() + "hardware/expected"));
    json& expected_list = config.get_json(config.df() + "hardware/expected");
    REQUIRE(expected_list.is_array());

    bool found = false;
    for (const auto& item : expected_list) {
        if (item == "temperature_sensor test") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - last_snapshot uses /printer/hardware/last_snapshot path",
                 "[hardware][validator][config][paths][plural]") {
    setup_hardware_with_snapshot();

    // Verify the path is /printer/hardware/last_snapshot (not /hardware_session/last_snapshot)
    REQUIRE(config_contains(config.df() + "hardware/last_snapshot"));
    json& snapshot = config.get_json(config.df() + "hardware/last_snapshot");
    REQUIRE(snapshot.is_object());
    REQUIRE(snapshot.contains("timestamp"));
    REQUIRE(snapshot.contains("heaters"));
    REQUIRE(snapshot.contains("fans"));
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - hardware section is under /printer/ not root",
                 "[hardware][validator][config][paths][plural]") {
    setup_empty_hardware_config();

    // The hardware section should be under /printer/, not at root level
    REQUIRE(config_contains(config.df() + "hardware"));
    REQUIRE_FALSE(config_contains("/hardware"));

    json& hardware = config.get_json(config.df() + "hardware");
    REQUIRE(hardware.is_object());
    REQUIRE(hardware.contains("optional"));
    REQUIRE(hardware.contains("expected"));
    REQUIRE(hardware.contains("last_snapshot"));
}

// ============================================================================
// MMU/AMS Detection Tests - TEST FIRST (TDD)
// These tests verify that the hardware validator uses hardware().has_mmu()
// instead of searching printer_objects_ for string matches.
// ============================================================================

// Fixture for MMU detection tests - extends HardwareValidatorConfigFixture for Config access
// Must be in namespace helix to match friend declaration in Config (inherited via base)
namespace helix {
class MmuDetectionFixture : public HardwareValidatorConfigFixture {
  protected:
    MoonrakerClientMock client;

    void setup_config_with_expected(const std::vector<std::string>& expected) {
        setup_printer_data({{"moonraker_host", "127.0.0.1"},
                            {"moonraker_port", 7125},
                            {"hardware",
                             {{"optional", json::array()},
                              {"expected", expected},
                              {"last_snapshot", json::object()}}}});
    }

    bool is_missing_in_result(const HardwareValidationResult& result, const std::string& name) {
        for (const auto& issue : result.expected_missing) {
            if (issue.hardware_name == name) {
                return true;
            }
        }
        return false;
    }
};
} // namespace helix

TEST_CASE_METHOD(MmuDetectionFixture,
                 "HardwareValidator - MMU detection uses has_mmu() capability flag",
                 "[hardware][validator][mmu]") {
    SECTION("No warning when has_mmu() is true and MMU is expected") {
        // Setup: printer has MMU capability (Happy Hare)
        client.set_heaters({"extruder", "heater_bed"});
        client.set_additional_objects({"mmu"}); // This sets has_mmu() = true

        // Verify capability flag is set
        REQUIRE(client.hardware().has_mmu());

        // Configure expectation for MMU
        setup_config_with_expected({"mmu"});

        HardwareValidator validator;
        auto result = validator.validate(&config, client.hardware());

        // MMU is present (has_mmu() = true), so no warning should be generated
        REQUIRE_FALSE(is_missing_in_result(result, "mmu"));
    }

    SECTION("Warning when MMU is expected but has_mmu() is false") {
        // Setup: printer does NOT have MMU capability
        client.set_heaters({"extruder", "heater_bed"});
        client.set_mmu_enabled(false); // Disable default MMU

        // Verify capability flag is NOT set
        REQUIRE_FALSE(client.hardware().has_mmu());

        // Configure expectation for MMU (user configured MMU in wizard)
        setup_config_with_expected({"mmu"});

        HardwareValidator validator;
        auto result = validator.validate(&config, client.hardware());

        // MMU is NOT present (has_mmu() = false), so warning SHOULD be generated
        REQUIRE(is_missing_in_result(result, "mmu"));
    }
}

TEST_CASE_METHOD(MmuDetectionFixture,
                 "HardwareValidator - AFC detection uses has_mmu() and mmu_type() capability flags",
                 "[hardware][validator][mmu][afc]") {
    SECTION("No warning when has_mmu() is true with AFC type") {
        // Setup: printer has AFC (Armored Turtle / BoxTurtle)
        client.set_heaters({"extruder", "heater_bed"});
        client.set_mmu_enabled(false);          // Disable default Happy Hare MMU
        client.set_additional_objects({"AFC"}); // This sets has_mmu() = true, mmu_type = AFC

        // Verify capability flags
        REQUIRE(client.hardware().has_mmu());
        REQUIRE(client.hardware().mmu_type() == AmsType::AFC);

        // Configure expectation for AFC
        setup_config_with_expected({"AFC"});

        HardwareValidator validator;
        auto result = validator.validate(&config, client.hardware());

        // AFC is present (has_mmu() = true), so no warning should be generated
        REQUIRE_FALSE(is_missing_in_result(result, "AFC"));
    }

    SECTION("Warning when AFC is expected but has_mmu() is false") {
        // Setup: printer does NOT have AFC capability
        client.set_heaters({"extruder", "heater_bed"});
        client.set_mmu_enabled(false); // Disable default MMU

        // Verify capability flag is NOT set
        REQUIRE_FALSE(client.hardware().has_mmu());

        // Configure expectation for AFC
        setup_config_with_expected({"AFC"});

        HardwareValidator validator;
        auto result = validator.validate(&config, client.hardware());

        // AFC is NOT present (has_mmu() = false), so warning SHOULD be generated
        REQUIRE(is_missing_in_result(result, "AFC"));
    }
}

TEST_CASE_METHOD(
    MmuDetectionFixture,
    "HardwareValidator - tool changer detection uses has_tool_changer() capability flag",
    "[hardware][validator][mmu][toolchanger]") {
    SECTION("No warning when has_tool_changer() is true") {
        // Setup: printer has tool changer
        client.set_heaters({"extruder", "heater_bed"});
        client.set_additional_objects({"toolchanger", "tool T0", "tool T1"});

        // Verify capability flag is set
        REQUIRE(client.hardware().has_tool_changer());

        // Configure expectation for tool changer
        setup_config_with_expected({"toolchanger"});

        HardwareValidator validator;
        auto result = validator.validate(&config, client.hardware());

        // Tool changer is present, so no warning should be generated
        REQUIRE_FALSE(is_missing_in_result(result, "toolchanger"));
    }

    SECTION("Warning when tool changer is expected but has_tool_changer() is false") {
        // Setup: printer does NOT have tool changer capability
        client.set_heaters({"extruder", "heater_bed"});
        client.set_mmu_enabled(false); // Disable default MMU (prevents has_mmu true)

        // Verify capability flag is NOT set
        REQUIRE_FALSE(client.hardware().has_tool_changer());

        // Configure expectation for tool changer
        setup_config_with_expected({"toolchanger"});

        HardwareValidator validator;
        auto result = validator.validate(&config, client.hardware());

        // Tool changer is NOT present, so warning SHOULD be generated
        REQUIRE(is_missing_in_result(result, "toolchanger"));
    }
}

// ============================================================================
// "None" Sentinel Value Tests
//
// The wizard dropdown saves "None" as empty string to config. The hardware
// validator should NOT report missing hardware for empty config values.
// ============================================================================

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - empty LED config does not trigger missing warning",
                 "[hardware][validator][config][none]") {
    MoonrakerClientMock client;
    client.set_heaters({"extruder", "heater_bed"});
    client.set_leds({}); // No LEDs on printer

    // Config with empty LED strip (user selected "None" in wizard)
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"leds", {{"strip", ""}}},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    // Empty string = no LED configured, should NOT warn about missing LED
    bool found_led_missing = false;
    for (const auto& issue : result.expected_missing) {
        if (issue.hardware_type == HardwareType::LED) {
            found_led_missing = true;
        }
    }
    REQUIRE_FALSE(found_led_missing);
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - 'None' string in LED config triggers false positive",
                 "[hardware][validator][config][none]") {
    MoonrakerClientMock client;
    client.set_heaters({"extruder", "heater_bed"});
    client.set_leds({}); // No LEDs on printer

    // Config with literal "None" (the old bug — wizard saved "None" as a string)
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"leds", {{"strip", "None"}}},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    // "None" is a non-empty string, so validator WILL report it as missing
    // This test documents the old bug behavior
    bool found_led_missing = false;
    for (const auto& issue : result.expected_missing) {
        if (issue.hardware_type == HardwareType::LED) {
            found_led_missing = true;
        }
    }
    REQUIRE(found_led_missing);
}

TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - empty fan config does not trigger missing warning",
                 "[hardware][validator][config][none]") {
    MoonrakerClientMock client;
    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan", "heater_fan hotend_fan"});

    // Config with empty chamber and exhaust fans (user selected "None")
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"fans",
                     {{"part", "fan"},
                      {"hotend", "heater_fan hotend_fan"},
                      {"chamber", ""},
                      {"exhaust", ""}}},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    // Empty strings for chamber/exhaust = not configured, no warnings
    bool found_fan_missing = false;
    for (const auto& issue : result.expected_missing) {
        if (issue.hardware_type == HardwareType::FAN && issue.hardware_name.empty()) {
            found_fan_missing = true;
        }
    }
    REQUIRE_FALSE(found_fan_missing);
}

// ============================================================================
// Expected Hardware Suppresses New Discovery Tests
// Validates that hardware saved via "Save" button (added to hardware/expected)
// is not re-reported as "newly discovered" on subsequent app launches.
// ============================================================================

class ExpectedHardwareSuppressFixture : public HardwareValidatorConfigFixture {
  protected:
    MoonrakerClientMock client;

    bool has_newly_discovered(const HardwareValidationResult& result, const std::string& name) {
        for (const auto& issue : result.newly_discovered) {
            if (issue.hardware_name == name) {
                return true;
            }
        }
        return false;
    }

    int count_newly_discovered(const HardwareValidationResult& result, HardwareType type) {
        int count = 0;
        for (const auto& issue : result.newly_discovered) {
            if (issue.hardware_type == type) {
                count++;
            }
        }
        return count;
    }
};

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - expected LED not reported as newly discovered",
                 "[hardware][validator][expected]") {
    // Printer has a LED strip, user already saved it via hardware health overlay
    client.set_heaters({"extruder", "heater_bed"});
    client.set_leds({"neopixel case_lights"});

    // Config: LED not in wizard config, but IS in hardware/expected
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array({"neopixel case_lights"})},
                      {"last_snapshot", json::object()}}}}}});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "neopixel case_lights"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - expected filament sensor not reported as newly discovered",
                 "[hardware][validator][expected]") {
    // Printer has filament sensors, user already saved them
    client.set_heaters({"extruder", "heater_bed"});
    client.set_filament_sensors(
        {"filament_switch_sensor tool_start", "filament_switch_sensor tool_end"});

    // Config: sensors not in wizard filament_sensors config, but ARE in hardware/expected
    // Note: mock always includes a default "filament_switch_sensor runout_sensor" via
    // rebuild_hardware(), so we include it in expected too
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array({"filament_switch_sensor tool_start",
                                                "filament_switch_sensor tool_end",
                                                "filament_switch_sensor runout_sensor"})},
                      {"last_snapshot", json::object()}}}}}});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor tool_start"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor tool_end"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor runout_sensor"));
    REQUIRE(count_newly_discovered(result, HardwareType::FILAMENT_SENSOR) == 0);
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - mix of expected and new hardware",
                 "[hardware][validator][expected]") {
    // Printer has multiple sensors, only some are in expected list
    client.set_heaters({"extruder", "heater_bed"});
    client.set_filament_sensors({"filament_switch_sensor tool_start",
                                 "filament_switch_sensor tool_end",
                                 "filament_switch_sensor runout"});

    // Only tool_start is in expected — tool_end and runout should still be reported
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array({"filament_switch_sensor tool_start"})},
                      {"last_snapshot", json::object()}}}}}});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor tool_start"));
    REQUIRE(has_newly_discovered(result, "filament_switch_sensor tool_end"));
    REQUIRE(has_newly_discovered(result, "filament_switch_sensor runout"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - LED still discovered when not in expected",
                 "[hardware][validator][expected]") {
    // Printer has LED, expected list has other hardware but not this LED
    client.set_heaters({"extruder", "heater_bed"});
    client.set_leds({"neopixel case_lights"});

    // Expected has a filament sensor, but NOT the LED
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array({"filament_switch_sensor tool_start"})},
                      {"last_snapshot", json::object()}}}}}});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    // LED should still be reported as new (not in expected, not in wizard config)
    REQUIRE(has_newly_discovered(result, "neopixel case_lights"));
}

// ============================================================================
// AMS-managed sensor toast suppression
//
// A fresh Happy Hare or AFC install should not pelt the user with
// "Filament sensor available. Add to config for runout detection?" toasts
// for sensors the AMS backend already owns. The substring filter in
// is_ams_sensor() catches names containing mmu/afc/lane/gate/etc., but
// HH uses conventional names (extruder, toolhead, filament_tension,
// filament_compression) and AFC uses tool_start/tool_end plus
// user-named per-lane/buffer sensors. The validator must consult the
// detected AMS backend in PrinterDiscovery to suppress those too.
// ============================================================================

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - Happy Hare extruder/toolhead sensors auto-suppressed",
                 "[hardware][validator][ams][happy_hare]") {
    // Happy Hare detected — its extruder + toolhead switches and analog
    // tension/compression inputs are managed by the HH backend, not
    // standalone runout detection.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_additional_objects({"mmu"});
    client.set_filament_sensors(
        {"filament_switch_sensor extruder", "filament_switch_sensor toolhead",
         "filament_switch_sensor filament_tension", "filament_switch_sensor filament_compression"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE(client.hardware().has_mmu());
    REQUIRE(client.hardware().mmu_type() == AmsType::HAPPY_HARE);

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor extruder"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor toolhead"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor filament_tension"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor filament_compression"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - extruder sensor still flagged when no MMU detected",
                 "[hardware][validator][ams][happy_hare]") {
    // Sanity check: the HH suppression must be conditional. A plain
    // single-extruder printer with a runout switch named "extruder"
    // should still get the new-hardware toast — otherwise we'd hide
    // a legitimate user sensor.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_mmu_enabled(false); // no MMU
    client.set_filament_sensors({"filament_switch_sensor extruder"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE_FALSE(client.hardware().has_mmu());

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE(has_newly_discovered(result, "filament_switch_sensor extruder"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - AFC tool_start/tool_end auto-suppressed",
                 "[hardware][validator][ams][afc]") {
    // AFC registers tool_start and tool_end at the extruder; without
    // backend-aware suppression they appear as "available" filament
    // sensors on every connect.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_mmu_enabled(false);
    client.set_additional_objects({"AFC"});
    client.set_filament_sensors(
        {"filament_switch_sensor tool_start", "filament_switch_sensor tool_end"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE(client.hardware().has_mmu());
    REQUIRE(client.hardware().mmu_type() == AmsType::AFC);

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor tool_start"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor tool_end"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - AFC per-lane sensors auto-suppressed by lane name",
                 "[hardware][validator][ams][afc]") {
    // AFC names per-lane sensors as "<lane>_prep", "<lane>_load",
    // "<lane>_selector". When a user names a lane "T0" the sensors
    // become "T0_prep" etc. — no substring in is_ams_sensor's static
    // list would match. The discovery-aware suppression consults
    // afc_lane_names() and matches by lane prefix.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_mmu_enabled(false);
    client.set_additional_objects({"AFC", "AFC_lane T0", "AFC_lane T1", "AFC_buffer Turtle1"});
    client.set_filament_sensors(
        {"filament_switch_sensor T0_prep", "filament_switch_sensor T0_load",
         "filament_switch_sensor T0_selector", "filament_switch_sensor T1_prep",
         "filament_switch_sensor Turtle1_expanded", "filament_switch_sensor Turtle1_compressed"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE(client.hardware().mmu_type() == AmsType::AFC);

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor T0_prep"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor T0_load"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor T0_selector"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor T1_prep"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor Turtle1_expanded"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor Turtle1_compressed"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - AFC HTLF home_pin sensor auto-suppressed",
                 "[hardware][validator][ams][afc]") {
    // AFC HTLF units register a "<unit>_home_pin" switch that doesn't
    // carry any AFC-specific substring. When AFC is detected, the
    // _home_pin suffix is the unambiguous signal.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_mmu_enabled(false);
    client.set_additional_objects({"AFC", "AFC_HTLF htlf1"});
    client.set_filament_sensors({"filament_switch_sensor htlf1_home_pin"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE(client.hardware().mmu_type() == AmsType::AFC);

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor htlf1_home_pin"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - AD5X IFS lessWaste port sensors auto-suppressed",
                 "[hardware][validator][ams][ad5x_ifs]") {
    // lessWaste plugin exports per-port HUB sensors as
    // filament_switch_sensor _ifs_port_sensor_{1..4}. The IFS detection
    // pattern in PrinterDiscovery already tags these as IFS-side, but
    // they still land in filament_sensor_names_ and need explicit
    // suppression on the toast path.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_mmu_enabled(false);
    client.set_filament_sensors(
        {"filament_switch_sensor _ifs_port_sensor_1", "filament_switch_sensor _ifs_port_sensor_2",
         "filament_switch_sensor _ifs_port_sensor_3", "filament_switch_sensor _ifs_port_sensor_4",
         "filament_switch_sensor head_switch_sensor"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE(client.hardware().mmu_type() == AmsType::AD5X_IFS);

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor _ifs_port_sensor_1"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor _ifs_port_sensor_2"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor _ifs_port_sensor_3"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor _ifs_port_sensor_4"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor head_switch_sensor"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - AD5X IFS native ZMOD motion sensor auto-suppressed",
                 "[hardware][validator][ams][ad5x_ifs]") {
    // Native ZMOD exposes ifs_motion_sensor (post-hub boolean) plus the
    // toolhead head_switch_sensor.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_mmu_enabled(false);
    client.set_filament_sensors(
        {"filament_motion_sensor ifs_motion_sensor", "filament_switch_sensor head_switch_sensor"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE(client.hardware().mmu_type() == AmsType::AD5X_IFS);

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_motion_sensor ifs_motion_sensor"));
    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor head_switch_sensor"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - CFS toolhead filament_sensor auto-suppressed",
                 "[hardware][validator][ams][cfs]") {
    // K2 CFS exposes a single filament_switch_sensor at the toolhead
    // named just 'filament_sensor'. Generic name, conventional on many
    // printers — only safe to suppress when CFS is the detected backend.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_mmu_enabled(false);
    client.set_additional_objects({"box"}); // CFS detection signal
    client.set_filament_sensors({"filament_switch_sensor filament_sensor"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE(client.hardware().mmu_type() == AmsType::CFS);

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE_FALSE(has_newly_discovered(result, "filament_switch_sensor filament_sensor"));
}

TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - filament_sensor still flagged when no CFS",
                 "[hardware][validator][ams][cfs]") {
    // Sanity: 'filament_sensor' is a conventional name. On a non-CFS
    // printer, a user-installed runout switch with that name must still
    // appear in newly_discovered — otherwise CFS suppression hides
    // legitimate sensors elsewhere.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_mmu_enabled(false);
    client.set_filament_sensors({"filament_switch_sensor filament_sensor"});
    setup_config({{"printer",
                   {{"moonraker_host", "127.0.0.1"},
                    {"moonraker_port", 7125},
                    {"hardware",
                     {{"optional", json::array()},
                      {"expected", json::array()},
                      {"last_snapshot", json::object()}}}}}});

    REQUIRE_FALSE(client.hardware().has_mmu());

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    REQUIRE(has_newly_discovered(result, "filament_switch_sensor filament_sensor"));
}

// The bespoke aux-fan check in validate_configured_hardware() turns any fans/aux
// mapping into an expected-missing warning when discovery does not return that
// object. Feed it the fans block HelixScreen actually ships for the k1 so the
// preset data and the check are held to the same contract.
TEST_CASE_METHOD(HardwareValidatorConfigFixture,
                 "HardwareValidator - shipped k1 preset maps no aux fan to warn about",
                 "[hardware][validator][k1]") {
    std::filesystem::path shipped =
        std::filesystem::current_path() / "assets" / "config" / "presets" / "k1.json";
    INFO("reading " << shipped.string() << " (tests must run from the repo root)");
    REQUIRE(std::filesystem::exists(shipped));
    std::ifstream in(shipped);
    REQUIRE(in.good());
    json preset = json::parse(in);
    REQUIRE(preset["printer"].contains("fans"));
    json fans = preset["printer"]["fans"];

    // A printer whose config comments the aux and exhaust output pins out.
    MoonrakerClientMock client;
    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"output_pin fan0", "heater_fan hotend_fan", "temperature_fan chamber_fan"});

    auto missing_fan_names = [&](const json& fans_block) {
        setup_printer_data({{"moonraker_host", "127.0.0.1"},
                            {"moonraker_port", 7125},
                            {"fans", fans_block},
                            {"hardware",
                             {{"optional", json::array()},
                              {"expected", json::array()},
                              {"last_snapshot", json::object()}}}});
        HardwareValidator validator;
        auto result = validator.validate(&config, client.hardware());
        std::vector<std::string> names;
        for (const auto& issue : result.expected_missing) {
            names.push_back(issue.hardware_name);
        }
        return names;
    };

    // Known positive first: an aux mapping onto an absent object IS reported, so a
    // clean result below means the preset claims nothing, not that the check is dead.
    json with_aux = fans;
    with_aux["aux"] = "output_pin fan1";
    auto reported = missing_fan_names(with_aux);
    REQUIRE(std::find(reported.begin(), reported.end(), "output_pin fan1") != reported.end());

    for (const auto& name : missing_fan_names(fans)) {
        INFO("unexpected missing-hardware warning: " << name);
        REQUIRE(name != "output_pin fan1");
    }
}

// Creality K2 Plus preset regression: the PTC heater's own blower keeps
// Klipper's factory name "heater_fan chamber_fan" on a stock unit, distinct by
// section prefix from the chamber circulation loop "temperature_fan
// chamber_fan" (see the "stock K2 naming" fixture in test_printer_hardware.cpp).
// The preset's hardware/expected list must cover that stock name, or a stock
// rig's real fan reads as newly discovered on every boot.
TEST_CASE_METHOD(ExpectedHardwareSuppressFixture,
                 "HardwareValidator - shipped k2 preset covers the stock chamber heater fan",
                 "[hardware][validator][k2]") {
    std::filesystem::path shipped =
        std::filesystem::current_path() / "assets" / "config" / "presets" / "k2.json";
    INFO("reading " << shipped.string() << " (tests must run from the repo root)");
    REQUIRE(std::filesystem::exists(shipped));
    std::ifstream in(shipped);
    REQUIRE(in.good());
    json preset = json::parse(in);
    REQUIRE(preset["printer"].contains("hardware"));
    REQUIRE(preset["printer"]["hardware"].contains("expected"));
    REQUIRE(preset["printer"].contains("fans"));

    // Object list as reported by Moonraker /printer/objects/list on a stock K2
    // Plus, unmodified from the factory config.
    client.set_heaters({"extruder", "heater_bed", "heater_generic chamber_heater"});
    client.set_fans({"heater_fan hotend_fan", "output_pin fan0", "output_pin fan2",
                     "output_pin fan1", "heater_fan chamber_fan", "temperature_fan chamber_fan",
                     "output_pin extruder_fan"});

    setup_printer_data({{"moonraker_host", "127.0.0.1"},
                        {"moonraker_port", 7125},
                        {"fans", preset["printer"]["fans"]},
                        {"hardware",
                         {{"optional", json::array()},
                          {"expected", preset["printer"]["hardware"]["expected"]},
                          {"last_snapshot", json::object()}}}});

    HardwareValidator validator;
    auto result = validator.validate(&config, client.hardware());

    // hardware/expected is a suppression set for non-AMS entries, not a
    // presence requirement, so a name listed in it is never checked for
    // absence — result.expected_missing has nothing to assert here.
    REQUIRE_FALSE(has_newly_discovered(result, "heater_fan chamber_fan"));
}
