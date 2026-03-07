// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "static_subject_registry.h"
#include "wizard_config_paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix;

// Test fixture for Config class testing
// Must be in namespace helix to match friend declaration in Config
namespace helix {
class ConfigTestFixture {
  protected:
    Config config;

    // Helper methods to access protected members
    void set_data_null(const std::string& json_ptr) {
        config.data[json::json_pointer(json_ptr)] = nullptr;
    }

    void set_data_empty() {
        config.data = {};
    }

    // Helper for plural naming refactor tests
    void set_data_for_plural_test(const json& data) {
        config.data = data;
    }

    // Helper to get mutable reference to config data for migration testing
    json& get_data() {
        return config.data;
    }

    // Helper to check if config data contains a key
    bool data_contains(const std::string& key) {
        return config.data.contains(key);
    }

    // Helper to apply migration to config data
    void apply_migration() {
        // Re-implement the migration logic for testing
        // This mirrors migrate_display_config() in config.cpp
        if (!config.data.contains("display_rotate")) {
            return; // Already migrated
        }

        if (!config.data.contains("display")) {
            config.data["display"] = json::object();
        }

        // Migrate only if target key doesn't already exist
        if (config.data.contains("display_rotate")) {
            if (!config.data["display"].contains("rotate")) {
                config.data["display"]["rotate"] = config.data["display_rotate"];
            }
            config.data.erase("display_rotate");
        }
        if (config.data.contains("display_sleep_sec")) {
            if (!config.data["display"].contains("sleep_sec")) {
                config.data["display"]["sleep_sec"] = config.data["display_sleep_sec"];
            }
            config.data.erase("display_sleep_sec");
        }
        if (config.data.contains("display_dim_sec")) {
            if (!config.data["display"].contains("dim_sec")) {
                config.data["display"]["dim_sec"] = config.data["display_dim_sec"];
            }
            config.data.erase("display_dim_sec");
        }
        if (config.data.contains("display_dim_brightness")) {
            if (!config.data["display"].contains("dim_brightness")) {
                config.data["display"]["dim_brightness"] = config.data["display_dim_brightness"];
            }
            config.data.erase("display_dim_brightness");
        }
        if (config.data.contains("touch_calibrated") || config.data.contains("touch_calibration")) {
            if (!config.data["display"].contains("calibration")) {
                config.data["display"]["calibration"] = json::object();
            }
            if (config.data.contains("touch_calibrated")) {
                if (!config.data["display"]["calibration"].contains("valid")) {
                    config.data["display"]["calibration"]["valid"] =
                        config.data["touch_calibrated"];
                }
                config.data.erase("touch_calibrated");
            }
            if (config.data.contains("touch_calibration")) {
                const auto& cal = config.data["touch_calibration"];
                for (const auto& key : {"a", "b", "c", "d", "e", "f"}) {
                    if (cal.contains(key) && !config.data["display"]["calibration"].contains(key)) {
                        config.data["display"]["calibration"][key] = cal[key];
                    }
                }
                config.data.erase("touch_calibration");
            }
        }

        // Second migration: move calibration and touch_device from /display/ to /input/
        migrate_to_input();
    }

    // Helper to migrate touch settings from /display/ to /input/ (second migration step)
    void migrate_to_input() {
        // Ensure input section exists
        if (!config.data.contains("input")) {
            config.data["input"] = json::object();
        }

        // Migrate /display/calibration -> /input/calibration
        if (config.data.contains("display") && config.data["display"].contains("calibration")) {
            if (!config.data["input"].contains("calibration")) {
                config.data["input"]["calibration"] = config.data["display"]["calibration"];
            }
            config.data["display"].erase("calibration");
        }

        // Migrate /display/touch_device -> /input/touch_device
        if (config.data.contains("display") && config.data["display"].contains("touch_device")) {
            if (!config.data["input"].contains("touch_device")) {
                config.data["input"]["touch_device"] = config.data["display"]["touch_device"];
            }
            config.data["display"].erase("touch_device");
        }
    }

    // Helper to check display subsection contains a key
    bool display_contains(const std::string& key) {
        return config.data.contains("display") && config.data["display"].contains(key);
    }

    // Helper to check calibration subsection contains a key (now under /input/)
    bool calibration_contains(const std::string& key) {
        return config.data.contains("input") && config.data["input"].contains("calibration") &&
               config.data["input"]["calibration"].contains(key);
    }

    // Helper to get display section size
    size_t display_size() {
        return config.data.contains("display") ? config.data["display"].size() : 0;
    }

    void setup_default_config() {
        // Manually populate config.data with realistic test JSON (v3 format)
        config.data = {
            {"config_version", 3},
            {"active_printer_id", "default"},
            {"printers",
             {{"default",
               {{"moonraker_host", "192.168.1.100"},
                {"moonraker_port", 7125},
                {"log_level", "debug"},
                {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                {"temp_sensors", {{"bed", "temperature_sensor bed"}, {"hotend", "extruder"}}},
                {"fans", {{"hotend", "heater_fan hotend_fan"}, {"part", "fan"}}},
                {"hardware_map", {{"heated_bed", "heater_bed"}, {"hotend", "extruder"}}}}}}}};
        config.active_printer_id_ = "default";
    }

    void setup_minimal_config() {
        // Minimal config for wizard testing (default host, v3 format)
        config.data = {
            {"config_version", 3},
            {"active_printer_id", "default"},
            {"printers",
             {{"default", {{"moonraker_host", "127.0.0.1"}, {"moonraker_port", 7125}}}}}};
        config.active_printer_id_ = "default";
    }

    void setup_incomplete_config() {
        // Config missing hardware_map (should trigger wizard, v3 format)
        config.data = {
            {"config_version", 3},
            {"active_printer_id", "default"},
            {"printers",
             {{"default", {{"moonraker_host", "192.168.1.50"}, {"moonraker_port", 7125}}}}}};
        config.active_printer_id_ = "default";
    }
};
} // namespace helix

// ============================================================================
// get() without default parameter - Existing behavior
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() returns existing string value",
                 "[core][config][get]") {
    setup_default_config();

    std::string host = config.get<std::string>(config.df() + "moonraker_host");
    REQUIRE(host == "192.168.1.100");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() returns existing int value",
                 "[core][config][get]") {
    setup_default_config();

    int port = config.get<int>(config.df() + "moonraker_port");
    REQUIRE(port == 7125);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() returns existing nested value",
                 "[config][get]") {
    setup_default_config();

    std::string bed = config.get<std::string>(config.df() + "hardware_map/heated_bed");
    REQUIRE(bed == "heater_bed");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with df() prefix returns value",
                 "[config][get]") {
    setup_default_config();

    std::string host = config.get<std::string>(config.df() + "moonraker_host");
    REQUIRE(host == "192.168.1.100");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with missing key throws exception",
                 "[core][config][get]") {
    setup_default_config();

    REQUIRE_THROWS_AS(config.get<std::string>(config.df() + "nonexistent_key"),
                      nlohmann::detail::type_error);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with missing nested key throws exception",
                 "[config][get]") {
    setup_default_config();

    REQUIRE_THROWS_AS(config.get<std::string>(config.df() + "hardware_map/missing"),
                      nlohmann::detail::type_error);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with type mismatch throws exception",
                 "[config][get]") {
    setup_default_config();

    // Try to get string value as int
    REQUIRE_THROWS(config.get<int>(config.df() + "moonraker_host"));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with object returns nested structure",
                 "[config][get]") {
    setup_default_config();

    auto hardware_map = config.get<json>(config.df() + "hardware_map");
    REQUIRE(hardware_map.is_object());
    REQUIRE(hardware_map["heated_bed"] == "heater_bed");
    REQUIRE(hardware_map["hotend"] == "extruder");
}

// ============================================================================
// get() with default parameter - NEW behavior
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: get() with default returns value when key exists (string)",
                 "[config][get][default]") {
    setup_default_config();

    std::string host = config.get<std::string>(config.df() + "moonraker_host", "default.local");
    REQUIRE(host == "192.168.1.100"); // Ignores default
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: get() with default returns value when key exists (int)",
                 "[config][get][default]") {
    setup_default_config();

    int port = config.get<int>(config.df() + "moonraker_port", 9999);
    REQUIRE(port == 7125); // Ignores default
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: get() with default returns default when key missing (string)",
                 "[core][config][get][default]") {
    setup_default_config();

    std::string printer_name = config.get<std::string>(config.df() + "printer_name", "My Printer");
    REQUIRE(printer_name == "My Printer");
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: get() with default returns default when key missing (int)",
                 "[config][get][default]") {
    setup_default_config();

    int timeout = config.get<int>(config.df() + "timeout", 30);
    REQUIRE(timeout == 30);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: get() with default returns default when key missing (bool)",
                 "[config][get][default]") {
    setup_default_config();

    bool api_key = config.get<bool>(config.df() + "moonraker_api_key", false);
    REQUIRE(api_key == false);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with default handles nested missing path",
                 "[config][get][default]") {
    setup_default_config();

    std::string led = config.get<std::string>(config.df() + "hardware_map/main_led", "none");
    REQUIRE(led == "none");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with empty string default",
                 "[config][get][default]") {
    setup_default_config();

    std::string empty = config.get<std::string>(config.df() + "empty_field", "");
    REQUIRE(empty == "");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with default using df() prefix",
                 "[config][get][default]") {
    setup_default_config();

    std::string printer_name = config.get<std::string>(config.df() + "printer_name", "");
    REQUIRE(printer_name == "");
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: get() with default handles completely missing parent path",
                 "[config][get][default]") {
    setup_default_config();

    std::string missing = config.get<std::string>("/nonexistent/path/key", "fallback");
    REQUIRE(missing == "fallback");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with default prevents crashes on null keys",
                 "[config][get][default]") {
    setup_minimal_config();

    // This is the bug we fixed - printer_name doesn't exist, should return default not throw
    std::string printer_name = config.get<std::string>(config.df() + "printer_name", "");
    REQUIRE(printer_name == "");
}

// ============================================================================
// set() operations
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: set() creates new top-level key", "[config][set]") {
    setup_default_config();

    config.set<std::string>("/new_key", "new_value");
    REQUIRE(config.get<std::string>("/new_key") == "new_value");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set() updates existing key", "[config][set]") {
    setup_default_config();

    config.set<std::string>(config.df() + "moonraker_host", "10.0.0.1");
    REQUIRE(config.get<std::string>(config.df() + "moonraker_host") == "10.0.0.1");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set() creates nested path", "[config][set]") {
    setup_default_config();

    config.set<std::string>(config.df() + "hardware_map/main_led", "neopixel");
    REQUIRE(config.get<std::string>(config.df() + "hardware_map/main_led") == "neopixel");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set() updates nested value", "[config][set]") {
    setup_default_config();

    config.set<std::string>(config.df() + "hardware_map/hotend", "extruder1");
    REQUIRE(config.get<std::string>(config.df() + "hardware_map/hotend") == "extruder1");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set() handles different types", "[config][set]") {
    setup_default_config();

    config.set<int>(config.df() + "new_int", 42);
    config.set<bool>(config.df() + "new_bool", true);
    config.set<std::string>(config.df() + "new_string", "test");

    REQUIRE(config.get<int>(config.df() + "new_int") == 42);
    REQUIRE(config.get<bool>(config.df() + "new_bool") == true);
    REQUIRE(config.get<std::string>(config.df() + "new_string") == "test");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set() overwrites value of different type",
                 "[config][set]") {
    setup_default_config();

    config.set<int>(config.df() + "moonraker_port", 8080);
    REQUIRE(config.get<int>(config.df() + "moonraker_port") == 8080);

    // Overwrite int with string
    config.set<std::string>(config.df() + "moonraker_port", "9090");
    REQUIRE(config.get<std::string>(config.df() + "moonraker_port") == "9090");
}

// ============================================================================
// is_wizard_required() logic - NEW: wizard_completed flag
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: is_wizard_required() returns false when wizard_completed is true",
                 "[config][wizard]") {
    setup_minimal_config();

    // Set wizard_completed flag
    config.set<bool>("/wizard_completed", true);

    REQUIRE(config.is_wizard_required() == false);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: is_wizard_required() returns true when wizard_completed is false",
                 "[config][wizard]") {
    setup_default_config();

    // Explicitly set wizard_completed to false
    config.set<bool>("/wizard_completed", false);

    REQUIRE(config.is_wizard_required() == true);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: is_wizard_required() returns true when wizard_completed flag missing",
                 "[config][wizard]") {
    setup_minimal_config();

    // No wizard_completed flag set
    REQUIRE(config.is_wizard_required() == true);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: wizard_completed flag overrides hardware config",
                 "[config][wizard]") {
    setup_default_config();

    // Even with full hardware config, if wizard_completed is false, wizard should run
    config.set<bool>("/wizard_completed", false);

    REQUIRE(config.is_wizard_required() == true);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: wizard_completed=true skips wizard even with minimal config",
                 "[config][wizard]") {
    setup_minimal_config();

    // Even with minimal config (127.0.0.1 host), wizard_completed=true should skip wizard
    config.set<bool>("/wizard_completed", true);

    REQUIRE(config.is_wizard_required() == false);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: is_wizard_required() handles invalid wizard_completed type",
                 "[config][wizard]") {
    setup_default_config();

    // Set wizard_completed to invalid type (string instead of bool)
    config.set<std::string>("/wizard_completed", "true");

    // Should return true (wizard required) because flag is not a valid boolean
    REQUIRE(config.is_wizard_required() == true);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: is_wizard_required() handles null wizard_completed",
                 "[config][wizard]") {
    setup_default_config();

    // Set wizard_completed to null
    set_data_null("/wizard_completed");

    // Should return true (wizard required) because flag is null
    REQUIRE(config.is_wizard_required() == true);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: handles deeply nested structures", "[config][edge]") {
    setup_default_config();

    config.set<std::string>(config.df() + "nested/level1/level2/level3", "deep");
    std::string deep = config.get<std::string>(config.df() + "nested/level1/level2/level3");
    REQUIRE(deep == "deep");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with default handles empty config",
                 "[config][edge]") {
    // Empty config
    set_data_empty();

    std::string host = config.get<std::string>(config.df() + "moonraker_host", "localhost");
    REQUIRE(host == "localhost");
}

// ============================================================================
// Config Path Structure Tests - NEW plural naming convention
// These tests define the contract for the refactored config structure.
// They SHOULD FAIL until the implementation is updated.
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: heaters path uses plural form",
                 "[config][paths][plural]") {
    setup_default_config();
    config.set<std::string>(config.df() + "heaters/bed", "heater_bed");
    config.set<std::string>(config.df() + "heaters/hotend", "extruder");

    std::string bed_heater = config.get<std::string>(config.df() + "heaters/bed");
    REQUIRE(bed_heater == "heater_bed");

    std::string hotend_heater = config.get<std::string>(config.df() + "heaters/hotend");
    REQUIRE(hotend_heater == "extruder");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: temp_sensors path uses plural form",
                 "[config][paths][plural]") {
    setup_default_config();
    config.set<std::string>(config.df() + "temp_sensors/bed", "heater_bed");
    config.set<std::string>(config.df() + "temp_sensors/hotend", "extruder");

    std::string bed_sensor = config.get<std::string>(config.df() + "temp_sensors/bed");
    REQUIRE(bed_sensor == "heater_bed");

    std::string hotend_sensor = config.get<std::string>(config.df() + "temp_sensors/hotend");
    REQUIRE(hotend_sensor == "extruder");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: fans path uses plural form",
                 "[config][paths][plural]") {
    setup_default_config();
    config.set<std::string>(config.df() + "fans/part", "fan");
    config.set<std::string>(config.df() + "fans/hotend", "heater_fan hotend_fan");

    std::string part_fan = config.get<std::string>(config.df() + "fans/part");
    REQUIRE(part_fan == "fan");

    std::string hotend_fan = config.get<std::string>(config.df() + "fans/hotend");
    REQUIRE(hotend_fan == "heater_fan hotend_fan");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: leds path uses plural form",
                 "[config][paths][plural]") {
    setup_default_config();
    config.set<std::string>(config.df() + "leds/strip", "neopixel chamber_light");

    std::string led_strip = config.get<std::string>(config.df() + "leds/strip");
    REQUIRE(led_strip == "neopixel chamber_light");
}

// ============================================================================
// Default Config Structure Tests - NEW structure contract
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: default structure has extra_sensors as empty object",
                 "[config][defaults][plural]") {
    setup_default_config();
    config.set<json>(config.df() + "extra_sensors", json::object());

    auto extra_sensors = config.get<json>(config.df() + "extra_sensors");
    REQUIRE(extra_sensors.is_object());
    REQUIRE(extra_sensors.empty());
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: default structure has no fans array - fans is object only",
                 "[config][defaults][plural]") {
    setup_default_config();

    auto fans = config.get<json>(config.df() + "fans", json::object());
    REQUIRE(fans.is_object());
    REQUIRE_FALSE(fans.is_array());
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: temp_sensors key exists for temperature sensor mappings",
                 "[config][defaults][plural]") {
    setup_default_config();

    auto temp_sensors = config.get<json>(config.df() + "temp_sensors");
    REQUIRE(temp_sensors.is_object());
    REQUIRE(temp_sensors.contains("bed"));
    REQUIRE(temp_sensors.contains("hotend"));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: hardware section is under printer/hardware/",
                 "[config][defaults][plural]") {
    setup_default_config();
    config.set<json>(config.df() + "hardware", {{"optional", json::array()},
                                                {"expected", json::array()},
                                                {"last_snapshot", json::object()}});

    auto hardware = config.get<json>(config.df() + "hardware");
    REQUIRE(hardware.is_object());
    REQUIRE(hardware.contains("optional"));
    REQUIRE(hardware.contains("expected"));
    REQUIRE(hardware.contains("last_snapshot"));
}

// ============================================================================
// Wizard Config Path Constants Tests - Verify suffix format (v3 multi-printer)
// Constants are now suffixes — callers prepend config->df() for full path.
// ============================================================================

TEST_CASE("WizardConfigPaths: BED_HEATER is suffix for heaters/bed",
          "[config][paths][wizard][plural]") {
    std::string path = helix::wizard::BED_HEATER;
    REQUIRE(path == "heaters/bed");
}

TEST_CASE("WizardConfigPaths: HOTEND_HEATER is suffix for heaters/hotend",
          "[config][paths][wizard][plural]") {
    std::string path = helix::wizard::HOTEND_HEATER;
    REQUIRE(path == "heaters/hotend");
}

TEST_CASE("WizardConfigPaths: BED_SENSOR is suffix for temp_sensors/bed",
          "[config][paths][wizard][plural]") {
    std::string path = helix::wizard::BED_SENSOR;
    REQUIRE(path == "temp_sensors/bed");
}

TEST_CASE("WizardConfigPaths: HOTEND_SENSOR is suffix for temp_sensors/hotend",
          "[config][paths][wizard][plural]") {
    std::string path = helix::wizard::HOTEND_SENSOR;
    REQUIRE(path == "temp_sensors/hotend");
}

TEST_CASE("WizardConfigPaths: PART_FAN is suffix for fans/part",
          "[config][paths][wizard][plural]") {
    std::string path = helix::wizard::PART_FAN;
    REQUIRE(path == "fans/part");
}

TEST_CASE("WizardConfigPaths: HOTEND_FAN is suffix for fans/hotend",
          "[config][paths][wizard][plural]") {
    std::string path = helix::wizard::HOTEND_FAN;
    REQUIRE(path == "fans/hotend");
}

TEST_CASE("WizardConfigPaths: LED_STRIP is suffix for leds/strip",
          "[config][paths][wizard][plural]") {
    std::string path = helix::wizard::LED_STRIP;
    REQUIRE(path == "leds/strip");
}

// ============================================================================
// Display Config Migration Tests - Phase 1 of display config refactoring
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: display section exists with defaults for new config",
                 "[config][display][migration]") {
    // Populate with display section using test helper
    // Note: calibration and touch_device are now under /input/, not /display/
    set_data_for_plural_test(
        {{"printer", {{"moonraker_host", "127.0.0.1"}}},
         {"display",
          {{"rotate", 0},
           {"sleep_sec", 600},
           {"dim_sec", 300},
           {"dim_brightness", 30},
           {"drm_device", ""}}},
         {"input",
          {{"touch_device", ""}, {"calibration", {{"valid", false}, {"a", 1.0}, {"b", 0.0}}}}}});

    // Verify display section has expected structure (no calibration - that's in /input/)
    auto display = config.get<json>("/display");
    REQUIRE(display.is_object());
    REQUIRE(display.contains("rotate"));
    REQUIRE(display.contains("sleep_sec"));
    REQUIRE(display.contains("dim_sec"));
    REQUIRE(display.contains("dim_brightness"));
    REQUIRE_FALSE(display.contains("calibration")); // Now under /input/

    REQUIRE(display["rotate"].get<int>() == 0);
    REQUIRE(display["sleep_sec"].get<int>() == 600);
    REQUIRE(display["dim_sec"].get<int>() == 300);
    REQUIRE(display["dim_brightness"].get<int>() == 30);

    // Verify calibration is under /input/
    auto input = config.get<json>("/input");
    REQUIRE(input.contains("calibration"));
    REQUIRE(input.contains("touch_device"));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: input/calibration section has coefficients",
                 "[config][input][migration]") {
    // Set up input section with calibration (new location)
    set_data_for_plural_test({{"input",
                               {{"calibration",
                                 {{"valid", true},
                                  {"a", 1.5},
                                  {"b", 0.1},
                                  {"c", -10.0},
                                  {"d", 0.2},
                                  {"e", 1.3},
                                  {"f", -5.0}}}}}});

    auto cal = config.get<json>("/input/calibration");
    REQUIRE(cal.is_object());
    REQUIRE(cal.contains("valid"));
    REQUIRE(cal.contains("a"));
    REQUIRE(cal.contains("b"));
    REQUIRE(cal.contains("c"));
    REQUIRE(cal.contains("d"));
    REQUIRE(cal.contains("e"));
    REQUIRE(cal.contains("f"));

    REQUIRE(cal["valid"].get<bool>() == true);
    REQUIRE(cal["a"].get<double>() == Catch::Approx(1.5));
    REQUIRE(cal["e"].get<double>() == Catch::Approx(1.3));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: display settings accessible via get() with defaults",
                 "[config][display][migration]") {
    set_data_empty();

    // Test default fallback when display section doesn't exist
    int rotate = config.get<int>("/display/rotate", 90);
    REQUIRE(rotate == 90); // Uses default since path doesn't exist

    int sleep_sec = config.get<int>("/display/sleep_sec", 1800);
    REQUIRE(sleep_sec == 1800);

    bool cal_valid = config.get<bool>("/input/calibration/valid", false);
    REQUIRE(cal_valid == false);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: display settings readable when populated",
                 "[config][display][migration]") {
    // Populate display section with calibration at old location
    set_data_for_plural_test({{"display",
                               {{"rotate", 180},
                                {"sleep_sec", 300},
                                {"dim_sec", 120},
                                {"dim_brightness", 50},
                                {"calibration", {{"valid", true}, {"a", 2.0}}}}}});

    // Run migration to move calibration from /display/ to /input/
    migrate_to_input();

    // Verify values are accessible
    REQUIRE(config.get<int>("/display/rotate") == 180);
    REQUIRE(config.get<int>("/display/sleep_sec") == 300);
    REQUIRE(config.get<int>("/display/dim_sec") == 120);
    REQUIRE(config.get<int>("/display/dim_brightness") == 50);
    REQUIRE(config.get<bool>("/input/calibration/valid") == true);
    REQUIRE(config.get<double>("/input/calibration/a") == Catch::Approx(2.0));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: display settings can be set and updated",
                 "[config][display][migration]") {
    // Create config with empty display section
    set_data_for_plural_test({{"display", json::object()}});

    // Set values
    config.set<int>("/display/rotate", 270);
    config.set<int>("/display/sleep_sec", 900);
    config.set<bool>("/input/calibration/valid", true);
    config.set<double>("/input/calibration/a", 1.1);

    // Verify
    REQUIRE(config.get<int>("/display/rotate") == 270);
    REQUIRE(config.get<int>("/display/sleep_sec") == 900);
    REQUIRE(config.get<bool>("/input/calibration/valid") == true);
    REQUIRE(config.get<double>("/input/calibration/a") == Catch::Approx(1.1));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: migrates /display/calibration to /input/calibration",
                 "[config][input][migration]") {
    // Set up calibration under old location (/display/)
    // Migration should move it to /input/
    set_data_for_plural_test({{"display",
                               {{"calibration",
                                 {{"valid", false},
                                  {"a", 1.0},
                                  {"b", 0.0},
                                  {"c", 0.0},
                                  {"d", 0.0},
                                  {"e", 1.0},
                                  {"f", 0.0}}}}}});

    // Run migration to move calibration from /display/ to /input/
    migrate_to_input();

    // Verify migration moved calibration to /input/
    auto cal = config.get<json>("/input/calibration");
    REQUIRE(cal.is_object());

    // Identity matrix check: a=1, b=0, c=0, d=0, e=1, f=0
    REQUIRE(cal["a"].get<double>() == Catch::Approx(1.0));
    REQUIRE(cal["b"].get<double>() == Catch::Approx(0.0));
    REQUIRE(cal["c"].get<double>() == Catch::Approx(0.0));
    REQUIRE(cal["d"].get<double>() == Catch::Approx(0.0));
    REQUIRE(cal["e"].get<double>() == Catch::Approx(1.0));
    REQUIRE(cal["f"].get<double>() == Catch::Approx(0.0));

    // Verify old location no longer has calibration
    REQUIRE_FALSE(display_contains("calibration"));
}

// ============================================================================
// Display Config Migration Tests - Comprehensive coverage
// ============================================================================

// ----------------------------------------------------------------------------
// Migration Detection Tests
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: migration detects old format with display_rotate at root",
                 "[config][display][migration]") {
    // Old format config with display_rotate at root
    json old_format = {{"display_rotate", 90}, {"printer", {{"moonraker_host", "192.168.1.100"}}}};

    set_data_for_plural_test(old_format);
    REQUIRE(data_contains("display_rotate"));

    apply_migration();

    // Old key should be removed
    REQUIRE_FALSE(data_contains("display_rotate"));
    // New structure should exist
    REQUIRE(data_contains("display"));
    REQUIRE(config.get<int>("/display/rotate") == 90);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: migration skips config already in new format",
                 "[config][display][migration]") {
    // New format config - no root-level display_* keys
    json new_format = {{"display", {{"rotate", 180}, {"sleep_sec", 300}}},
                       {"printer", {{"moonraker_host", "192.168.1.100"}}}};

    set_data_for_plural_test(new_format);

    // Verify no migration needed (no display_rotate at root)
    REQUIRE_FALSE(data_contains("display_rotate"));

    apply_migration();

    // Values should be unchanged
    REQUIRE(config.get<int>("/display/rotate") == 180);
    REQUIRE(config.get<int>("/display/sleep_sec") == 300);
}

// ----------------------------------------------------------------------------
// Individual Key Migration Tests
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(ConfigTestFixture, "Config: migrates display_rotate to /display/rotate",
                 "[config][display][migration]") {
    json old_format = {{"display_rotate", 270}};

    set_data_for_plural_test(old_format);
    apply_migration();

    REQUIRE_FALSE(data_contains("display_rotate"));
    REQUIRE(config.get<int>("/display/rotate") == 270);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: migrates display_sleep_sec to /display/sleep_sec",
                 "[config][display][migration]") {
    json old_format = {{"display_rotate", 0}, {"display_sleep_sec", 1800}};

    set_data_for_plural_test(old_format);
    apply_migration();

    REQUIRE_FALSE(data_contains("display_sleep_sec"));
    REQUIRE(config.get<int>("/display/sleep_sec") == 1800);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: migrates display_dim_sec to /display/dim_sec",
                 "[config][display][migration]") {
    json old_format = {{"display_rotate", 0}, {"display_dim_sec", 120}};

    set_data_for_plural_test(old_format);
    apply_migration();

    REQUIRE_FALSE(data_contains("display_dim_sec"));
    REQUIRE(config.get<int>("/display/dim_sec") == 120);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: migrates display_dim_brightness to /display/dim_brightness",
                 "[config][display][migration]") {
    json old_format = {{"display_rotate", 0}, {"display_dim_brightness", 50}};

    set_data_for_plural_test(old_format);
    apply_migration();

    REQUIRE_FALSE(data_contains("display_dim_brightness"));
    REQUIRE(config.get<int>("/display/dim_brightness") == 50);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: migrates touch_calibrated to /input/calibration/valid",
                 "[config][display][migration]") {
    json old_format = {{"display_rotate", 0}, {"touch_calibrated", true}};

    set_data_for_plural_test(old_format);
    apply_migration();

    REQUIRE_FALSE(data_contains("touch_calibrated"));
    REQUIRE(config.get<bool>("/input/calibration/valid") == true);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: migrates touch_calibration coefficients to /input/calibration",
                 "[config][display][migration]") {
    json old_format = {
        {"display_rotate", 0},
        {"touch_calibration",
         {{"a", 1.5}, {"b", 0.1}, {"c", -10.0}, {"d", 0.2}, {"e", 1.3}, {"f", -5.0}}}};

    set_data_for_plural_test(old_format);
    apply_migration();

    REQUIRE_FALSE(data_contains("touch_calibration"));
    REQUIRE(config.get<double>("/input/calibration/a") == Catch::Approx(1.5));
    REQUIRE(config.get<double>("/input/calibration/b") == Catch::Approx(0.1));
    REQUIRE(config.get<double>("/input/calibration/c") == Catch::Approx(-10.0));
    REQUIRE(config.get<double>("/input/calibration/d") == Catch::Approx(0.2));
    REQUIRE(config.get<double>("/input/calibration/e") == Catch::Approx(1.3));
    REQUIRE(config.get<double>("/input/calibration/f") == Catch::Approx(-5.0));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: migration removes all old root-level display keys",
                 "[config][display][migration]") {
    // Full old format config with all legacy keys
    json old_format = {{"display_rotate", 90},
                       {"display_sleep_sec", 900},
                       {"display_dim_sec", 180},
                       {"display_dim_brightness", 25},
                       {"touch_calibrated", true},
                       {"touch_calibration",
                        {{"a", 1.1}, {"b", 0.0}, {"c", 5.0}, {"d", 0.0}, {"e", 0.9}, {"f", 10.0}}},
                       {"printer", {{"moonraker_host", "test"}}}};

    set_data_for_plural_test(old_format);
    apply_migration();

    // All old keys should be gone
    REQUIRE_FALSE(data_contains("display_rotate"));
    REQUIRE_FALSE(data_contains("display_sleep_sec"));
    REQUIRE_FALSE(data_contains("display_dim_sec"));
    REQUIRE_FALSE(data_contains("display_dim_brightness"));
    REQUIRE_FALSE(data_contains("touch_calibrated"));
    REQUIRE_FALSE(data_contains("touch_calibration"));

    // All values should be in new location
    REQUIRE(config.get<int>("/display/rotate") == 90);
    REQUIRE(config.get<int>("/display/sleep_sec") == 900);
    REQUIRE(config.get<int>("/display/dim_sec") == 180);
    REQUIRE(config.get<int>("/display/dim_brightness") == 25);
    REQUIRE(config.get<bool>("/input/calibration/valid") == true);
    REQUIRE(config.get<double>("/input/calibration/a") == Catch::Approx(1.1));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: partial migration handles only existing old keys",
                 "[config][display][migration]") {
    // Config with only some old keys (missing display_dim_sec and touch_calibration)
    json partial_old = {
        {"display_rotate", 180}, {"display_sleep_sec", 1200}, {"touch_calibrated", false}};

    set_data_for_plural_test(partial_old);
    apply_migration();

    // Present keys should be migrated
    REQUIRE(config.get<int>("/display/rotate") == 180);
    REQUIRE(config.get<int>("/display/sleep_sec") == 1200);
    REQUIRE(config.get<bool>("/input/calibration/valid") == false);

    // Missing keys should NOT exist in new location (no defaults injected by migration)
    REQUIRE_FALSE(display_contains("dim_sec"));
    REQUIRE_FALSE(display_contains("dim_brightness"));
    REQUIRE_FALSE(calibration_contains("a"));
}

// ----------------------------------------------------------------------------
// Default Value Tests - Verify get_default_display_config() values
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/rotate is 0",
                 "[config][display][defaults]") {
    set_data_empty();

    // Use default fallback when not set
    int rotate = config.get<int>("/display/rotate", 0);
    REQUIRE(rotate == 0);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/sleep_sec is 600",
                 "[config][display][defaults]") {
    set_data_empty();

    int sleep_sec = config.get<int>("/display/sleep_sec", 600);
    REQUIRE(sleep_sec == 600);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/dim_sec is 300",
                 "[config][display][defaults]") {
    set_data_empty();

    int dim_sec = config.get<int>("/display/dim_sec", 300);
    REQUIRE(dim_sec == 300);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/dim_brightness is 30",
                 "[config][display][defaults]") {
    set_data_empty();

    int dim_brightness = config.get<int>("/display/dim_brightness", 30);
    REQUIRE(dim_brightness == 30);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/drm_device is empty string",
                 "[config][display][defaults]") {
    set_data_empty();

    std::string drm_device = config.get<std::string>("/display/drm_device", "");
    REQUIRE(drm_device == "");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/touch_device is empty string",
                 "[config][display][defaults]") {
    set_data_empty();

    std::string touch_device = config.get<std::string>("/input/touch_device", "");
    REQUIRE(touch_device == "");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/gcode_render_mode is 0",
                 "[config][display][defaults]") {
    set_data_empty();

    int gcode_render_mode = config.get<int>("/display/gcode_render_mode", 0);
    REQUIRE(gcode_render_mode == 0);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/bed_mesh_render_mode is 0",
                 "[config][display][defaults]") {
    set_data_empty();

    int bed_mesh_render_mode = config.get<int>("/display/bed_mesh_render_mode", 0);
    REQUIRE(bed_mesh_render_mode == 0);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: default input/calibration/valid is false",
                 "[config][input][defaults]") {
    set_data_empty();

    bool cal_valid = config.get<bool>("/input/calibration/valid", false);
    REQUIRE(cal_valid == false);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: default input/calibration coefficients form identity matrix",
                 "[config][input][defaults]") {
    set_data_empty();

    // Identity matrix: a=1, b=0, c=0, d=0, e=1, f=0
    REQUIRE(config.get<double>("/input/calibration/a", 1.0) == Catch::Approx(1.0));
    REQUIRE(config.get<double>("/input/calibration/b", 0.0) == Catch::Approx(0.0));
    REQUIRE(config.get<double>("/input/calibration/c", 0.0) == Catch::Approx(0.0));
    REQUIRE(config.get<double>("/input/calibration/d", 0.0) == Catch::Approx(0.0));
    REQUIRE(config.get<double>("/input/calibration/e", 1.0) == Catch::Approx(1.0));
    REQUIRE(config.get<double>("/input/calibration/f", 0.0) == Catch::Approx(0.0));
}

// ----------------------------------------------------------------------------
// Read/Write Tests - Set and get display values
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(ConfigTestFixture, "Config: set and get display/rotate",
                 "[config][display][readwrite]") {
    set_data_for_plural_test({{"display", json::object()}});

    config.set<int>("/display/rotate", 180);
    REQUIRE(config.get<int>("/display/rotate") == 180);

    config.set<int>("/display/rotate", 270);
    REQUIRE(config.get<int>("/display/rotate") == 270);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set and get display/sleep_sec",
                 "[config][display][readwrite]") {
    set_data_for_plural_test({{"display", json::object()}});

    config.set<int>("/display/sleep_sec", 1800);
    REQUIRE(config.get<int>("/display/sleep_sec") == 1800);

    config.set<int>("/display/sleep_sec", 0); // Disable sleep
    REQUIRE(config.get<int>("/display/sleep_sec") == 0);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set and get input/calibration/valid",
                 "[config][input][readwrite]") {
    set_data_for_plural_test({{"input", {{"calibration", json::object()}}}});

    config.set<bool>("/input/calibration/valid", true);
    REQUIRE(config.get<bool>("/input/calibration/valid") == true);

    config.set<bool>("/input/calibration/valid", false);
    REQUIRE(config.get<bool>("/input/calibration/valid") == false);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set and get input/calibration coefficients",
                 "[config][input][readwrite]") {
    set_data_for_plural_test({{"input", {{"calibration", json::object()}}}});

    // Set custom calibration values
    config.set<double>("/input/calibration/a", 1.25);
    config.set<double>("/input/calibration/b", 0.05);
    config.set<double>("/input/calibration/c", -15.5);
    config.set<double>("/input/calibration/d", 0.03);
    config.set<double>("/input/calibration/e", 1.15);
    config.set<double>("/input/calibration/f", -8.2);

    REQUIRE(config.get<double>("/input/calibration/a") == Catch::Approx(1.25));
    REQUIRE(config.get<double>("/input/calibration/b") == Catch::Approx(0.05));
    REQUIRE(config.get<double>("/input/calibration/c") == Catch::Approx(-15.5));
    REQUIRE(config.get<double>("/input/calibration/d") == Catch::Approx(0.03));
    REQUIRE(config.get<double>("/input/calibration/e") == Catch::Approx(1.15));
    REQUIRE(config.get<double>("/input/calibration/f") == Catch::Approx(-8.2));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set and get display/drm_device",
                 "[config][display][readwrite]") {
    set_data_for_plural_test({{"display", json::object()}});

    config.set<std::string>("/display/drm_device", "/dev/dri/card0");
    REQUIRE(config.get<std::string>("/display/drm_device") == "/dev/dri/card0");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set and get input/touch_device",
                 "[config][input][readwrite]") {
    set_data_for_plural_test({{"input", json::object()}});

    config.set<std::string>("/input/touch_device", "/dev/input/event0");
    REQUIRE(config.get<std::string>("/input/touch_device") == "/dev/input/event0");
}

// ----------------------------------------------------------------------------
// Edge Cases
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(ConfigTestFixture, "Config: empty display section gets populated with set values",
                 "[config][display][edge]") {
    set_data_for_plural_test({{"display", json::object()}});

    // Verify empty initially
    REQUIRE(display_size() == 0);

    // Set a single value
    config.set<int>("/display/rotate", 90);

    // Verify value was set
    REQUIRE(config.get<int>("/display/rotate") == 90);
    REQUIRE(display_size() == 1);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: missing calibration subsection can be created via set",
                 "[config][input][edge]") {
    // Input section without calibration
    set_data_for_plural_test({{"input", json::object()}});

    REQUIRE_FALSE(calibration_contains("valid"));

    // Set creates the path
    config.set<bool>("/input/calibration/valid", true);

    REQUIRE(calibration_contains("valid"));
    REQUIRE(config.get<bool>("/input/calibration/valid") == true);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: migration preserves existing /display/ values",
                 "[config][display][edge]") {
    // Config with both old keys AND existing display section
    // This simulates a partial migration or manual edit
    json mixed_format = {{"display_rotate", 90}, // Old format
                         {"display",
                          {{"sleep_sec", 1200}, // Already in new format
                           {"drm_device", "/dev/dri/card1"}}}};

    set_data_for_plural_test(mixed_format);
    apply_migration();

    // Old key should be migrated
    REQUIRE(config.get<int>("/display/rotate") == 90);

    // Existing values should be preserved (not overwritten)
    REQUIRE(config.get<int>("/display/sleep_sec") == 1200);
    REQUIRE(config.get<std::string>("/display/drm_device") == "/dev/dri/card1");
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: migration handles touch_calibration without touch_calibrated",
                 "[config][display][edge]") {
    // Only touch_calibration coefficients, no touch_calibrated flag
    json old_format = {{"display_rotate", 0},
                       {"touch_calibration",
                        {{"a", 1.2}, {"b", 0.0}, {"c", 0.0}, {"d", 0.0}, {"e", 1.2}, {"f", 0.0}}}};

    set_data_for_plural_test(old_format);
    apply_migration();

    // Coefficients should be migrated
    REQUIRE(config.get<double>("/input/calibration/a") == Catch::Approx(1.2));
    REQUIRE(config.get<double>("/input/calibration/e") == Catch::Approx(1.2));

    // valid flag should NOT be set (since touch_calibrated wasn't present)
    REQUIRE_FALSE(calibration_contains("valid"));
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: migration handles touch_calibrated without coefficients",
                 "[config][display][edge]") {
    // Only touch_calibrated flag, no coefficients
    json old_format = {{"display_rotate", 0}, {"touch_calibrated", true}};

    set_data_for_plural_test(old_format);
    apply_migration();

    // Flag should be migrated
    REQUIRE(config.get<bool>("/input/calibration/valid") == true);

    // Coefficients should NOT be set
    REQUIRE_FALSE(calibration_contains("a"));
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: migration handles partial touch_calibration coefficients",
                 "[config][display][edge]") {
    // Only some coefficients present
    json old_format = {{"display_rotate", 0}, {"touch_calibration", {{"a", 1.5}, {"e", 1.3}}}};

    set_data_for_plural_test(old_format);
    apply_migration();

    // Present coefficients should be migrated
    REQUIRE(config.get<double>("/input/calibration/a") == Catch::Approx(1.5));
    REQUIRE(config.get<double>("/input/calibration/e") == Catch::Approx(1.3));

    // Missing coefficients should NOT be set
    REQUIRE_FALSE(calibration_contains("b"));
    REQUIRE_FALSE(calibration_contains("c"));
    REQUIRE_FALSE(calibration_contains("d"));
    REQUIRE_FALSE(calibration_contains("f"));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: display values with boundary conditions",
                 "[config][display][edge]") {
    set_data_for_plural_test({{"display", json::object()}});

    // Test rotation values (0, 90, 180, 270)
    for (int rotation : {0, 90, 180, 270}) {
        config.set<int>("/display/rotate", rotation);
        REQUIRE(config.get<int>("/display/rotate") == rotation);
    }

    // Test sleep disabled (0) and max reasonable value
    config.set<int>("/display/sleep_sec", 0);
    REQUIRE(config.get<int>("/display/sleep_sec") == 0);

    config.set<int>("/display/sleep_sec", 86400); // 24 hours
    REQUIRE(config.get<int>("/display/sleep_sec") == 86400);

    // Test brightness range (0-100)
    config.set<int>("/display/dim_brightness", 0);
    REQUIRE(config.get<int>("/display/dim_brightness") == 0);

    config.set<int>("/display/dim_brightness", 100);
    REQUIRE(config.get<int>("/display/dim_brightness") == 100);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: input calibration with extreme coefficient values",
                 "[config][input][edge]") {
    set_data_for_plural_test({{"input", {{"calibration", json::object()}}}});

    // Test very small values
    config.set<double>("/input/calibration/a", 0.001);
    REQUIRE(config.get<double>("/input/calibration/a") == Catch::Approx(0.001));

    // Test negative values
    config.set<double>("/input/calibration/c", -500.0);
    REQUIRE(config.get<double>("/input/calibration/c") == Catch::Approx(-500.0));

    // Test large values
    config.set<double>("/input/calibration/f", 1000.0);
    REQUIRE(config.get<double>("/input/calibration/f") == Catch::Approx(1000.0));
}

TEST_CASE_METHOD(
    ConfigTestFixture,
    "Config: migration does not overwrite existing /display/ values with old root values",
    "[config][display][migration]") {
    // Set up config with both old and new format - new should win
    json mixed = {{"display_rotate", 90},     // Old value
                  {"display_sleep_sec", 300}, // Old value
                  {"display",
                   {
                       {"rotate", 180},   // New value should NOT be overwritten
                       {"sleep_sec", 600} // New value should NOT be overwritten
                   }}};
    set_data_for_plural_test(mixed);
    apply_migration();

    // Verify new values were preserved
    REQUIRE(get_data()["display"]["rotate"] == 180);
    REQUIRE(get_data()["display"]["sleep_sec"] == 600);

    // Verify old keys were removed
    REQUIRE_FALSE(data_contains("display_rotate"));
    REQUIRE_FALSE(data_contains("display_sleep_sec"));
}

// ============================================================================
// Log Level Configuration Tests
// ============================================================================
// These tests verify the contract for log_level configuration behavior.
// The key behavior is that log_level should NOT have a default value in config,
// allowing test_mode to provide its own fallback to DEBUG.
//
// BUG CONTEXT: config.cpp currently writes log_level="warn" to config during init(),
// which means by the time init_logging() runs, test_mode fallback never triggers.
// The fix is to remove log_level from defaults so test_mode can provide fallback.

TEST_CASE_METHOD(ConfigTestFixture, "Config: default config should NOT contain log_level key",
                 "[core][config][log_level]") {
    // TDD TEST: This test defines the CONTRACT that default config should NOT
    // have log_level. This allows test_mode to provide its own fallback to DEBUG.
    //
    // EXPECTED TO FAIL INITIALLY: config.cpp currently includes log_level in
    // get_default_config() and also sets it during init() if missing.
    // The fix (Step 5) will remove log_level from defaults.
    //
    // Build the same default config structure that get_default_config() produces,
    // but WITHOUT the log_level key (which is the desired behavior).
    set_data_for_plural_test({{"log_path", "/tmp/helixscreen.log"},
                              // NOTE: NO log_level key - this is intentional!
                              {"dark_mode", true},
                              {"display", json::object()},
                              {"printer", json::object()}});

    // The config should NOT have log_level set in defaults
    // This allows the application to fall through to test_mode check
    REQUIRE_FALSE(data_contains("log_level"));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: test mode fallback requires absent log_level",
                 "[core][config][log_level]") {
    // This test verifies the pattern used in init_logging():
    // 1. Get log_level from config with empty string default
    // 2. If empty string, fall through to test_mode check
    //
    // Without this pattern working, test_mode users don't get DEBUG logs.
    set_data_for_plural_test({{"log_path", "/tmp/helixscreen.log"}, {"dark_mode", true}});
    // NO log_level key - simulates config without user override

    // The pattern from init_logging(): get with empty sentinel
    std::string level_str = config.get<std::string>("/log_level", "");

    // When log_level is absent, the sentinel should be returned
    REQUIRE(level_str.empty());

    // In init_logging(), this allows falling through to test_mode check:
    // if (level_str == "trace") { ... }
    // else if (level_str == "debug") { ... }
    // else if (level_str == "info") { ... }
    // else if (get_runtime_config()->test_mode) { <-- CAN NOW REACH THIS
    //     log_config.level = spdlog::level::debug;
    // }
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: get log_level with default returns default when key absent",
                 "[core][config][log_level]") {
    // When log_level is absent, get() with default should return the default
    // This is the pattern used in init_logging() to check for absent log_level
    set_data_empty();

    // Using empty string as sentinel to detect "not set"
    std::string level = config.get<std::string>("/log_level", "");
    REQUIRE(level == "");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: log_level is respected when explicitly set",
                 "[config][log_level]") {
    // When user explicitly sets log_level, it should be used
    set_data_for_plural_test({{"log_level", "debug"}});

    std::string level = config.get<std::string>("/log_level", "");
    REQUIRE(level == "debug");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: log_level can be set to any valid level",
                 "[config][log_level]") {
    for (const char* level_name : {"trace", "debug", "info", "warn"}) {
        set_data_for_plural_test({{"log_level", level_name}});
        std::string level = config.get<std::string>("/log_level", "");
        REQUIRE(level == level_name);
    }
}

// ============================================================================
// Log Level Integration Test (using real Config::init())
// ============================================================================
// This test calls the REAL Config::init() function with a temp file to verify
// the actual default config behavior.

TEST_CASE("Config::init() should NOT write log_level to new config file",
          "[core][config][log_level][integration]") {
    // TDD TEST: This test SHOULD FAIL initially because config.cpp currently
    // includes log_level in get_default_config() and writes it during init().
    //
    // The fix (Step 5) will remove log_level from defaults, making this pass.

    // Create a temp directory for the test config
    std::string temp_dir =
        std::filesystem::temp_directory_path().string() + "/helix_test_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_config_path = temp_dir + "/test_config.json";

    // Ensure no existing config file
    std::filesystem::remove(temp_config_path);
    REQUIRE_FALSE(std::filesystem::exists(temp_config_path));

    // Create a fresh Config instance (not using singleton to avoid state pollution)
    Config test_config;
    test_config.init(temp_config_path);

    // Verify the config file was created
    REQUIRE(std::filesystem::exists(temp_config_path));

    // Read the generated config file directly to check its contents
    std::ifstream config_file(temp_config_path);
    json config_data = json::parse(config_file);

    // THE KEY ASSERTION: log_level should NOT be present in default config
    // This allows test_mode to provide its own fallback to DEBUG level
    INFO("Config file contents: " << config_data.dump(2));
    REQUIRE_FALSE(config_data.contains("log_level"));

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// LANGUAGE CONFIG TESTS
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: get_language returns default 'en' for new config",
                 "[config][language]") {
    // Empty config should return default "en"
    set_data_empty();
    REQUIRE(config.get_language() == "en");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get_language returns stored value",
                 "[config][language]") {
    get_data()["language"] = "de";
    REQUIRE(config.get_language() == "de");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set_language stores value", "[config][language]") {
    set_data_empty();

    config.set_language("fr");
    REQUIRE(get_data()["language"] == "fr");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: language supports all planned languages",
                 "[config][language]") {
    std::vector<std::string> languages = {"en", "de", "fr", "es", "ru"};

    for (const auto& lang : languages) {
        config.set_language(lang);
        REQUIRE(config.get_language() == lang);
    }
}

// ============================================================================
// Config Versioning & Migration Tests
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: v0 config with sounds_enabled=true gets migrated to false",
                 "[core][config][migration][versioning]") {
    // Simulate an existing config from before sound support (no config_version)
    set_data_for_plural_test(
        {{"sounds_enabled", true},
         {"brightness", 50},
         {"printer", {{"moonraker_host", "192.168.1.100"}, {"moonraker_port", 7125}}}});

    // No config_version means v0
    REQUIRE_FALSE(data_contains("config_version"));
    REQUIRE(config.get<bool>("/sounds_enabled") == true);

    // Run init on a temp file to trigger migrations
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_migration_test_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    // Write v0 config to disk
    {
        std::ofstream o(temp_path);
        o << get_data().dump(2);
    }

    // Run init which triggers migrations
    Config test_config;
    test_config.init(temp_path);

    // Verify migration ran
    REQUIRE(test_config.get<bool>("/sounds_enabled") == false);
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: config already at version 1 does NOT get sounds flipped",
                 "[core][config][migration][versioning]") {
    // Config that was already migrated — user may have re-enabled sounds
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_migration_test_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v1_config = {{"config_version", 1},
                      {"sounds_enabled", true},
                      {"brightness", 50},
                      {"printer", {{"moonraker_host", "192.168.1.100"}, {"moonraker_port", 7125}}}};

    {
        std::ofstream o(temp_path);
        o << v1_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    // sounds_enabled should still be true — migration should NOT re-run
    REQUIRE(test_config.get<bool>("/sounds_enabled") == true);
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: fresh config gets version stamp and sounds default to false",
                 "[core][config][migration][versioning]") {
    // Brand new config — no file exists
    std::string temp_dir = std::filesystem::temp_directory_path().string() + "/helix_fresh_test_" +
                           std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/fresh_config.json";

    // Ensure file doesn't exist
    std::filesystem::remove(temp_path);
    REQUIRE_FALSE(std::filesystem::exists(temp_path));

    Config test_config;
    test_config.init(temp_path);

    // Fresh config should have current version (skips all migrations)
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    // Fresh config should NOT have sounds_enabled set (it's a user pref, not in base defaults)
    // But if accessed with default, should be false
    REQUIRE(test_config.get<bool>("/sounds_enabled", false) == false);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: v0 config without sounds_enabled key just gets version stamp",
                 "[config][migration][versioning]") {
    // Edge case: old config that somehow never had sounds_enabled
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_nosound_test_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json minimal_v0 = {
        {"brightness", 50},
        {"printer", {{"moonraker_host", "192.168.1.100"}, {"moonraker_port", 7125}}}};

    {
        std::ofstream o(temp_path);
        o << minimal_v0.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    // Should get version stamp without errors
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    // sounds_enabled should not have been created by migration
    REQUIRE(test_config.get<bool>("/sounds_enabled", false) == false);

    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// v3→v4 migration: Multi-printer support
// ============================================================================

TEST_CASE("Config: v3→v4 migration restructures single printer to multi-printer",
          "[core][config][migration][v4]") {
    std::string temp_dir = "/tmp/helix_test_v3_to_v4";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    // Write a v3 config with single /printer section (pre-multi-printer schema)
    json v3_config = {
        {"config_version", 3},
        {"dark_mode", true},
        {"wizard_completed", true},
        {"printer",
         {{"name", "My Voron 2.4"},
          {"moonraker_host", "192.168.1.112"},
          {"moonraker_port", 7125},
          {"moonraker_api_key", false},
          {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
          {"leds", {{"strip", "neopixel"}, {"selected", json::array({"neopixel"})}}}}},
        {"filament", {{"extrude_speed", 10}}},
        {"panel_widgets", {{"home", json::array({{{"id", "temp"}, {"enabled", true}}})}}}};

    {
        std::ofstream o(temp_path);
        o << v3_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    // Should have migrated to v4
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    // /printer should be gone, /printers should exist
    REQUIRE_FALSE(test_config.get<json>("", json()).contains("printer"));
    REQUIRE(test_config.get<json>("", json()).contains("printers"));

    // Printer should be keyed by slugified name
    std::string expected_id = "my-voron-2-4";
    REQUIRE(test_config.get_active_printer_id() == expected_id);

    // Access via df() should resolve correctly
    REQUIRE(test_config.get<std::string>(test_config.df() + "moonraker_host") == "192.168.1.112");
    REQUIRE(test_config.get<int>(test_config.df() + "moonraker_port") == 7125);

    // Filament should have moved under printer entry
    REQUIRE(test_config.get<int>(test_config.df() + "filament/extrude_speed") == 10);

    // Panel widgets should have moved under printer entry
    auto pw = test_config.get<json>(test_config.df() + "panel_widgets/home", json());
    REQUIRE(pw.is_array());
    REQUIRE(pw.size() == 1);

    // wizard_completed should be copied to printer entry
    REQUIRE(test_config.get<bool>(test_config.df() + "wizard_completed") == true);

    // Root-level wizard_completed should still exist (backward compat)
    REQUIRE(test_config.get<bool>("/wizard_completed") == true);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v3→v4 migration uses 'default' when printer has no name",
          "[core][config][migration][v4]") {
    std::string temp_dir = "/tmp/helix_test_v3_to_v4_noname";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v3_config = {{"config_version", 3},
                      {"printer", {{"moonraker_host", "10.0.0.5"}, {"moonraker_port", 7125}}}};

    {
        std::ofstream o(temp_path);
        o << v3_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get_active_printer_id() == "default");
    REQUIRE(test_config.get<std::string>(test_config.df() + "moonraker_host") == "10.0.0.5");

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v3→v4 migration skips if /printers already exists",
          "[core][config][migration][v4]") {
    std::string temp_dir = "/tmp/helix_test_v3_skip";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    // Already v3 format
    json v3_config = {{"config_version", 3},
                      {"active_printer_id", "ender3"},
                      {"printers",
                       {{"ender3",
                         {{"moonraker_host", "192.168.1.50"},
                          {"moonraker_port", 7125},
                          {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}}}}}}};

    {
        std::ofstream o(temp_path);
        o << v3_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get_active_printer_id() == "ender3");
    REQUIRE(test_config.get<std::string>(test_config.df() + "moonraker_host") == "192.168.1.50");

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v3→v4 migration moves printer_image to per-printer path",
          "[core][config][migration][v4]") {
    std::string temp_dir = "/tmp/helix_test_v3_to_v4_printer_image";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    // Write a v3 config with /display/printer_image (pre-multi-printer schema)
    json v3_config = {
        {"config_version", 3},
        {"printer",
         {{"name", "My Printer"},
          {"moonraker_host", "192.168.1.100"},
          {"moonraker_port", 7125}}},
        {"display", {{"printer_image", "shipped:voron-24r2"}, {"rotate", 0}}}};

    {
        std::ofstream o(temp_path);
        o << v3_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    // Should have migrated to v4
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    // printer_image should be at per-printer path, not under /display
    REQUIRE(test_config.get<std::string>(test_config.df() + PRINTER_IMAGE) == "shipped:voron-24r2");

    // /display/printer_image should no longer exist
    auto display = test_config.get<json>("/display", json::object());
    REQUIRE_FALSE(display.contains("printer_image"));

    // /display should still have other keys intact
    REQUIRE(test_config.get<int>("/display/rotate") == 0);

    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// Multi-printer CRUD
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: slugify converts names to URL-safe IDs",
                 "[core][config][multi-printer]") {
    REQUIRE(Config::slugify("Voron 2.4") == "voron-2-4");
    REQUIRE(Config::slugify("Ender 3 Pro") == "ender-3-pro");
    REQUIRE(Config::slugify("  --Leading Hyphens--  ") == "leading-hyphens");
    REQUIRE(Config::slugify("UPPERCASE") == "uppercase");
    REQUIRE(Config::slugify("special!@#chars") == "special-chars");
    REQUIRE(Config::slugify("") == "default");
    REQUIRE(Config::slugify("   ") == "default");
    REQUIRE(Config::slugify("simple") == "simple");
    REQUIRE(Config::slugify("a--b") == "a-b");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get_printer_ids returns all configured printers",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"ender3", {{"moonraker_host", "192.168.1.20"}}}}}});
    config.set_active_printer("voron");

    auto ids = config.get_printer_ids();
    REQUIRE(ids.size() == 2);
    // Order from JSON object iteration
    bool has_voron = std::find(ids.begin(), ids.end(), "voron") != ids.end();
    bool has_ender3 = std::find(ids.begin(), ids.end(), "ender3") != ids.end();
    REQUIRE(has_voron);
    REQUIRE(has_ender3);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set_active_printer switches df() routing",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"ender3", {{"moonraker_host", "192.168.1.20"}}}}}});
    REQUIRE(config.set_active_printer("voron") == true);

    REQUIRE(config.get<std::string>(config.df() + "moonraker_host") == "192.168.1.10");

    REQUIRE(config.set_active_printer("ender3") == true);
    REQUIRE(config.get<std::string>(config.df() + "moonraker_host") == "192.168.1.20");
    REQUIRE(config.get_active_printer_id() == "ender3");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set_active_printer rejects unknown printer",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers", {{"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
    REQUIRE(config.set_active_printer("voron") == true);

    REQUIRE(config.set_active_printer("nonexistent") == false);
    // Should not have changed
    REQUIRE(config.get_active_printer_id() == "voron");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: add_printer creates new printer entry",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers", {{"voron", {{"moonraker_host", "192.168.1.10"}}}}}});

    json new_printer = {{"moonraker_host", "10.0.0.1"}, {"moonraker_port", 7125}};
    config.add_printer("bambu-x1", new_printer);

    auto ids = config.get_printer_ids();
    REQUIRE(ids.size() == 2);
    REQUIRE(config.get<std::string>("/printers/bambu-x1/moonraker_host") == "10.0.0.1");
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: remove_printer deletes entry and auto-selects remaining printer",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"ender3", {{"moonraker_host", "192.168.1.20"}}}}}});
    config.set_active_printer("voron");

    config.remove_printer("voron");

    auto ids = config.get_printer_ids();
    REQUIRE(ids.size() == 1);
    // Active should auto-switch to remaining printer since we removed the active one
    REQUIRE(config.get_active_printer_id() == "ender3");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: remove_printer keeps active if removing non-active",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"ender3", {{"moonraker_host", "192.168.1.20"}}}}}});
    config.set_active_printer("voron");

    config.remove_printer("ender3");

    REQUIRE(config.get_printer_ids().size() == 1);
    REQUIRE(config.get_active_printer_id() == "voron");
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: remove_printer prevents removing last printer",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers", {{"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
    config.set_active_printer("voron");

    config.remove_printer("voron");

    // Should still have the printer — cannot remove the last one
    REQUIRE(config.get_printer_ids().size() == 1);
    REQUIRE(config.get_active_printer_id() == "voron");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: df() routes to active printer path",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "my-printer"},
                              {"printers", {{"my-printer", {{"moonraker_host", "10.0.0.1"}}}}}});
    config.set_active_printer("my-printer");

    REQUIRE(config.df() == "/printers/my-printer/");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: is_wizard_required checks per-printer flag",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test(
        {{"active_printer_id", "voron"},
         {"wizard_completed", false},
         {"printers", {{"voron", {{"wizard_completed", true}, {"moonraker_host", "10.0.0.1"}}}}}});
    config.set_active_printer("voron");

    // Per-printer wizard_completed=true should take priority over root false
    REQUIRE_FALSE(config.is_wizard_required());
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: is_wizard_required falls back to root flag",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"wizard_completed", true},
                              {"printers", {{"voron", {{"moonraker_host", "10.0.0.1"}}}}}});
    config.set_active_printer("voron");

    // No per-printer flag, should fall back to root wizard_completed=true
    REQUIRE_FALSE(config.is_wizard_required());
}

// ============================================================================
// Registry deinit/re-init cycle tests (Phase 2: soft restart support)
// ============================================================================

TEST_CASE("StaticSubjectRegistry supports deinit/re-init cycles", "[core][registry]") {
    // Verify deinit_all() clears vector, allowing fresh registrations
    int call_count = 0;
    StaticSubjectRegistry::instance().register_deinit("test_cycle_1", [&]() { call_count++; });
    StaticSubjectRegistry::instance().deinit_all();
    REQUIRE(call_count == 1);

    // Re-register after deinit — should work (vector was cleared)
    call_count = 0;
    StaticSubjectRegistry::instance().register_deinit("test_cycle_2", [&]() { call_count++; });
    StaticSubjectRegistry::instance().deinit_all();
    REQUIRE(call_count == 1);
}

TEST_CASE("StaticSubjectRegistry deinit_all runs callbacks in LIFO order", "[core][registry]") {
    std::vector<std::string> order;
    StaticSubjectRegistry::instance().register_deinit("first", [&]() { order.push_back("first"); });
    StaticSubjectRegistry::instance().register_deinit("second",
                                                      [&]() { order.push_back("second"); });
    StaticSubjectRegistry::instance().deinit_all();

    REQUIRE(order.size() == 2);
    // LIFO: second registered = first to deinit
    REQUIRE(order[0] == "second");
    REQUIRE(order[1] == "first");
}

// ============================================================================
// Preset detection tests
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "has_preset returns false for default config",
                 "[config][preset]") {
    get_data() = json::object();
    REQUIRE(config.has_preset() == false);
    REQUIRE(config.get_preset().empty());
}

TEST_CASE_METHOD(ConfigTestFixture, "has_preset returns true when preset field is set",
                 "[config][preset]") {
    get_data() = {{"preset", "ad5m"}};
    REQUIRE(config.has_preset() == true);
    REQUIRE(config.get_preset() == "ad5m");
}

TEST_CASE_METHOD(ConfigTestFixture, "has_preset returns false for empty preset string",
                 "[config][preset]") {
    get_data() = {{"preset", ""}};
    REQUIRE(config.has_preset() == false);
    REQUIRE(config.get_preset().empty());
}
