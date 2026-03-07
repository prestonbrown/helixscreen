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

#include "config.h"

// Test fixture for Config-dependent HardwareValidator tests
// NOTE: After the plural naming refactor, hardware config moves from
// /hardware/ to /printer/hardware/
// Must be in namespace helix to match friend declaration in Config
namespace helix {
class HardwareValidatorConfigFixture {
  protected:
    Config config;

    // Helper to check if a JSON pointer path exists in config data
    bool config_contains(const std::string& json_ptr) {
        return config.data.contains(json::json_pointer(json_ptr));
    }

    // Helper to wrap printer data in v3 format with active_printer_id
    void setup_printer_data(const json& printer_data) {
        config.data = {{"config_version", 3},
                       {"active_printer_id", "default"},
                       {"printers", {{"default", printer_data}}}};
        config.active_printer_id_ = "default";
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
            config.data = {{"config_version", 3},
                           {"active_printer_id", "default"},
                           {"printers", {{"default", j["printer"]}}}};
            config.active_printer_id_ = "default";
        } else {
            config.data = j;
            if (j.contains("active_printer_id")) {
                config.active_printer_id_ = j["active_printer_id"].get<std::string>();
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
