// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_test_access.h"
#include "app_constants.h"
#include "config.h"
#include "config_testing.h"
#include "runtime_config.h"
#include "static_subject_registry.h"
#include "wizard_config_paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

// Test fixture for Config class testing
// Accesses Config internals via ConfigTestAccess
namespace helix {
class ConfigTestFixture {
  protected:
    Config config;

    // Helper methods to access protected members
    void set_data_null(const std::string& json_ptr) {
        ConfigTestAccess::data(config)[json::json_pointer(json_ptr)] = nullptr;
    }

    void set_data_empty() {
        ConfigTestAccess::data(config) = {};
    }

    // Helper for plural naming refactor tests
    void set_data_for_plural_test(const json& data) {
        ConfigTestAccess::data(config) = data;
    }

    // Helper to get mutable reference to config data for migration testing
    json& get_data() {
        return ConfigTestAccess::data(config);
    }

    // Helper to check if config data contains a key
    bool data_contains(const std::string& key) {
        return ConfigTestAccess::data(config).contains(key);
    }

    // Helper to set config file path for save tests
    void set_path(const std::string& p) {
        ConfigTestAccess::path(config) = p;
    }

    // Helper to apply migration to config data
    void apply_migration() {
        // Re-implement the migration logic for testing
        // This mirrors migrate_display_config() in config.cpp
        if (!ConfigTestAccess::data(config).contains("display_rotate")) {
            return; // Already migrated
        }

        if (!ConfigTestAccess::data(config).contains("display")) {
            ConfigTestAccess::data(config)["display"] = json::object();
        }

        // Migrate only if target key doesn't already exist
        if (ConfigTestAccess::data(config).contains("display_rotate")) {
            if (!ConfigTestAccess::data(config)["display"].contains("rotate")) {
                ConfigTestAccess::data(config)["display"]["rotate"] =
                    ConfigTestAccess::data(config)["display_rotate"];
            }
            ConfigTestAccess::data(config).erase("display_rotate");
        }
        if (ConfigTestAccess::data(config).contains("display_sleep_sec")) {
            if (!ConfigTestAccess::data(config)["display"].contains("sleep_sec")) {
                ConfigTestAccess::data(config)["display"]["sleep_sec"] =
                    ConfigTestAccess::data(config)["display_sleep_sec"];
            }
            ConfigTestAccess::data(config).erase("display_sleep_sec");
        }
        if (ConfigTestAccess::data(config).contains("display_dim_sec")) {
            if (!ConfigTestAccess::data(config)["display"].contains("dim_sec")) {
                ConfigTestAccess::data(config)["display"]["dim_sec"] =
                    ConfigTestAccess::data(config)["display_dim_sec"];
            }
            ConfigTestAccess::data(config).erase("display_dim_sec");
        }
        if (ConfigTestAccess::data(config).contains("display_dim_brightness")) {
            if (!ConfigTestAccess::data(config)["display"].contains("dim_brightness")) {
                ConfigTestAccess::data(config)["display"]["dim_brightness"] =
                    ConfigTestAccess::data(config)["display_dim_brightness"];
            }
            ConfigTestAccess::data(config).erase("display_dim_brightness");
        }
        if (ConfigTestAccess::data(config).contains("touch_calibrated") ||
            ConfigTestAccess::data(config).contains("touch_calibration")) {
            if (!ConfigTestAccess::data(config)["display"].contains("calibration")) {
                ConfigTestAccess::data(config)["display"]["calibration"] = json::object();
            }
            if (ConfigTestAccess::data(config).contains("touch_calibrated")) {
                if (!ConfigTestAccess::data(config)["display"]["calibration"].contains("valid")) {
                    ConfigTestAccess::data(config)["display"]["calibration"]["valid"] =
                        ConfigTestAccess::data(config)["touch_calibrated"];
                }
                ConfigTestAccess::data(config).erase("touch_calibrated");
            }
            if (ConfigTestAccess::data(config).contains("touch_calibration")) {
                const auto& cal = ConfigTestAccess::data(config)["touch_calibration"];
                for (const auto& key : {"a", "b", "c", "d", "e", "f"}) {
                    if (cal.contains(key) &&
                        !ConfigTestAccess::data(config)["display"]["calibration"].contains(key)) {
                        ConfigTestAccess::data(config)["display"]["calibration"][key] = cal[key];
                    }
                }
                ConfigTestAccess::data(config).erase("touch_calibration");
            }
        }

        // Second migration: move calibration and touch_device from /display/ to /input/
        migrate_to_input();
    }

    // Helper to migrate touch settings from /display/ to /input/ (second migration step)
    void migrate_to_input() {
        // Ensure input section exists
        if (!ConfigTestAccess::data(config).contains("input")) {
            ConfigTestAccess::data(config)["input"] = json::object();
        }

        // Migrate /display/calibration -> /input/calibration
        if (ConfigTestAccess::data(config).contains("display") &&
            ConfigTestAccess::data(config)["display"].contains("calibration")) {
            if (!ConfigTestAccess::data(config)["input"].contains("calibration")) {
                ConfigTestAccess::data(config)["input"]["calibration"] =
                    ConfigTestAccess::data(config)["display"]["calibration"];
            }
            ConfigTestAccess::data(config)["display"].erase("calibration");
        }

        // Migrate /display/touch_device -> /input/touch_device
        if (ConfigTestAccess::data(config).contains("display") &&
            ConfigTestAccess::data(config)["display"].contains("touch_device")) {
            if (!ConfigTestAccess::data(config)["input"].contains("touch_device")) {
                ConfigTestAccess::data(config)["input"]["touch_device"] =
                    ConfigTestAccess::data(config)["display"]["touch_device"];
            }
            ConfigTestAccess::data(config)["display"].erase("touch_device");
        }
    }

    // Helper to check display subsection contains a key
    bool display_contains(const std::string& key) {
        return ConfigTestAccess::data(config).contains("display") &&
               ConfigTestAccess::data(config)["display"].contains(key);
    }

    // Helper to check calibration subsection contains a key (now under /input/)
    bool calibration_contains(const std::string& key) {
        return ConfigTestAccess::data(config).contains("input") &&
               ConfigTestAccess::data(config)["input"].contains("calibration") &&
               ConfigTestAccess::data(config)["input"]["calibration"].contains(key);
    }

    // Helper to get display section size
    size_t display_size() {
        return ConfigTestAccess::data(config).contains("display")
                   ? ConfigTestAccess::data(config)["display"].size()
                   : 0;
    }

    void setup_default_config() {
        // Manually populate ConfigTestAccess::data(config) with realistic test JSON (v3 format)
        ConfigTestAccess::data(config) = {
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
        ConfigTestAccess::active_printer_id(config) = "default";
    }

    void setup_minimal_config() {
        // Minimal config for wizard testing (default host, v3 format)
        ConfigTestAccess::data(config) = {
            {"config_version", 3},
            {"active_printer_id", "default"},
            {"printers",
             {{"default", {{"moonraker_host", "127.0.0.1"}, {"moonraker_port", 7125}}}}}};
        ConfigTestAccess::active_printer_id(config) = "default";
    }

    void setup_incomplete_config() {
        // Config missing hardware_map (should trigger wizard, v3 format)
        ConfigTestAccess::data(config) = {
            {"config_version", 3},
            {"active_printer_id", "default"},
            {"printers",
             {{"default", {{"moonraker_host", "192.168.1.50"}, {"moonraker_port", 7125}}}}}};
        ConfigTestAccess::active_printer_id(config) = "default";
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

    // out_of_range, not type_error: get() uses at() so a missing key never
    // vivifies a null on the way to failing (#1129).
    REQUIRE_THROWS_AS(config.get<std::string>(config.df() + "nonexistent_key"),
                      nlohmann::detail::out_of_range);
    REQUIRE_FALSE(config.exists(config.df() + "nonexistent_key"));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with missing nested key throws exception",
                 "[config][get]") {
    setup_default_config();

    REQUIRE_THROWS_AS(config.get<std::string>(config.df() + "hardware_map/missing"),
                      nlohmann::detail::out_of_range);
    // The failed lookup must not have created the missing leaf (#1129).
    REQUIRE_FALSE(config.exists(config.df() + "hardware_map/missing"));
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

TEST_CASE_METHOD(ConfigTestFixture, "Config: default display/gpu_blur_blocked is false",
                 "[config][display][defaults]") {
    set_data_empty();

    bool gpu_blur_blocked = config.get<bool>("/display/gpu_blur_blocked", false);
    REQUIRE(gpu_blur_blocked == false);
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

// RAII guard to save and restore the global backup file that init() overwrites
/// RAII guard: redirect BOTH backup tiers (the $HOME fallback and the
/// /var/lib/helixscreen StateDirectory primary) to a temp dir so neither
/// tarball-detection nor restore finds a real backup. Redirecting the primary
/// too is what lets these tests run on a machine with HelixScreen installed —
/// they used to SKIP whenever /var/lib/helixscreen/settings.json.backup existed.
/// Each instance gets a unique dir to avoid parallel-shard races.
struct BackupGuard {
    std::string temp_dir;
    std::string original_home;
    std::string prev_state_dir;
    std::string prev_fallback_dir;
    bool had_home;

    BackupGuard()
        : temp_dir(std::filesystem::temp_directory_path().string() + "/helix_nobackup_" +
                   std::to_string(getpid()) + "_" + std::to_string(rand())),
          prev_state_dir(AppConstants::Update::detail::state_dir_ref()),
          prev_fallback_dir(AppConstants::Update::detail::backup_fallback_dir_ref()),
          had_home(std::getenv("HOME") != nullptr) {
        if (had_home)
            original_home = std::getenv("HOME");
        std::filesystem::create_directories(temp_dir);
        setenv("HOME", temp_dir.c_str(), 1);
        AppConstants::Update::detail::backup_fallback_dir_ref() =
            AppConstants::Update::sanitize_home(std::getenv("HOME")) + "/.helixscreen";
        AppConstants::Update::detail::state_dir_ref() = temp_dir + "/var-lib";
    }
    ~BackupGuard() {
        if (had_home)
            setenv("HOME", original_home.c_str(), 1);
        else
            unsetenv("HOME");
        // Restore the SAVED value, never a freshly-computed $HOME/.helixscreen:
        // the test sandbox points this at a temp dir, and recomputing it here
        // un-sandboxes every later test in the shard onto the real backup.
        AppConstants::Update::detail::backup_fallback_dir_ref() = prev_fallback_dir;
        AppConstants::Update::detail::state_dir_ref() = prev_state_dir;
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }
};

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
    BackupGuard guard;
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

    BackupGuard guard;
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
    // BackupGuard (below) redirects both backup tiers into a temp dir, so a real
    // /var/lib/helixscreen backup on the host cannot be restored over the fresh
    // config this test is asserting about.

    // Brand new config — no file exists
    std::string temp_dir = std::filesystem::temp_directory_path().string() + "/helix_fresh_test_" +
                           std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/fresh_config.json";

    // Ensure file doesn't exist
    std::filesystem::remove(temp_path);
    REQUIRE_FALSE(std::filesystem::exists(temp_path));

    BackupGuard guard;
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

    BackupGuard guard;
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
         {{"name", "My Printer"}, {"moonraker_host", "192.168.1.100"}, {"moonraker_port", 7125}}},
        {"display", {{"printer_image", "shipped:voron-v2"}, {"rotate", 0}}}};

    {
        std::ofstream o(temp_path);
        o << v3_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    // Should have migrated to v4
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    // printer_image should be at per-printer path, not under /display
    REQUIRE(test_config.get<std::string>(test_config.df() + PRINTER_IMAGE) == "shipped:voron-v2");

    // /display/printer_image should no longer exist
    auto display = test_config.get<json>("/display", json::object());
    REQUIRE_FALSE(display.contains("printer_image"));

    // /display should still have other keys intact
    REQUIRE(test_config.get<int>("/display/rotate") == 0);

    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// v4→v5 migration: show_printer_switcher default for single-printer configs
// ============================================================================

TEST_CASE("Config: v4→v5 migration disables printer switcher for single-printer config",
          "[core][config][migration][v5]") {
    std::string temp_dir = "/tmp/helix_test_v4_to_v5_single";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v4_config = {
        {"config_version", 4},
        {"active_printer_id", "ender3"},
        {"printers", {{"ender3", {{"moonraker_host", "192.168.1.50"}, {"moonraker_port", 7125}}}}}};

    {
        std::ofstream o(temp_path);
        o << v4_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE(test_config.get<bool>("/printers/show_printer_switcher") == false);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v4→v5 migration skips when multiple printers configured",
          "[core][config][migration][v5]") {
    std::string temp_dir = "/tmp/helix_test_v4_to_v5_multi";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v4_config = {{"config_version", 4},
                      {"active_printer_id", "ender3"},
                      {"printers",
                       {{"ender3", {{"moonraker_host", "192.168.1.50"}}},
                        {"voron", {{"moonraker_host", "192.168.1.51"}}}}}};

    {
        std::ofstream o(temp_path);
        o << v4_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    // Migration should NOT have written the key for multi-printer configs
    REQUIRE_FALSE(test_config.exists("/printers/show_printer_switcher"));

    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// v13 → v14: fold legacy telemetry_config.json into settings.json
// ============================================================================

TEST_CASE("Config: v13→v14 imports legacy telemetry_config.json when settings lacks key",
          "[core][config][migration][v14]") {
    std::string temp_dir = "/tmp/helix_test_v13_to_v14_import";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string settings_path = temp_dir + "/test_config.json";
    std::string legacy_path = temp_dir + "/telemetry_config.json";

    json v13_config = {{"config_version", 13},
                       {"active_printer_id", "default"},
                       {"printers", {{"default", {{"moonraker_host", "127.0.0.1"}}}}}};
    {
        std::ofstream o(settings_path);
        o << v13_config.dump(2);
    }
    {
        std::ofstream o(legacy_path);
        o << R"({"enabled": true})";
    }

    Config test_config;
    test_config.init(settings_path);

    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE(test_config.exists("/telemetry_enabled"));
    REQUIRE(test_config.get<bool>("/telemetry_enabled") == true);
    // Legacy file must be removed so it can never re-poison a future startup.
    REQUIRE_FALSE(std::filesystem::exists(legacy_path));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v13→v14 preserves disabled legacy state", "[core][config][migration][v14]") {
    std::string temp_dir = "/tmp/helix_test_v13_to_v14_disabled";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string settings_path = temp_dir + "/test_config.json";
    std::string legacy_path = temp_dir + "/telemetry_config.json";

    json v13_config = {{"config_version", 13}, {"active_printer_id", "default"}};
    {
        std::ofstream o(settings_path);
        o << v13_config.dump(2);
    }
    {
        std::ofstream o(legacy_path);
        o << R"({"enabled": false})";
    }

    Config test_config;
    test_config.init(settings_path);

    REQUIRE(test_config.get<bool>("/telemetry_enabled") == false);
    REQUIRE_FALSE(std::filesystem::exists(legacy_path));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v13→v14 does NOT overwrite existing /telemetry_enabled",
          "[core][config][migration][v14]") {
    // Regression guard for the original bug: settings.json had the key set;
    // legacy telemetry_config.json should never silently override it.
    std::string temp_dir = "/tmp/helix_test_v13_to_v14_no_overwrite";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string settings_path = temp_dir + "/test_config.json";
    std::string legacy_path = temp_dir + "/telemetry_config.json";

    json v13_config = {
        {"config_version", 13},
        {"active_printer_id", "default"},
        {"telemetry_enabled", true}, // authoritative
    };
    {
        std::ofstream o(settings_path);
        o << v13_config.dump(2);
    }
    {
        std::ofstream o(legacy_path);
        o << R"({"enabled": false})"; // would silently disable if we honored this
    }

    Config test_config;
    test_config.init(settings_path);

    REQUIRE(test_config.get<bool>("/telemetry_enabled") == true);
    // File still gets removed — it's dead weight either way.
    REQUIRE_FALSE(std::filesystem::exists(legacy_path));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v13→v14 is a no-op when no legacy file exists",
          "[core][config][migration][v14]") {
    std::string temp_dir = "/tmp/helix_test_v13_to_v14_noop";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string settings_path = temp_dir + "/test_config.json";

    json v13_config = {{"config_version", 13}, {"active_printer_id", "default"}};
    {
        std::ofstream o(settings_path);
        o << v13_config.dump(2);
    }

    Config test_config;
    test_config.init(settings_path);

    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE_FALSE(test_config.exists("/telemetry_enabled"));

    std::filesystem::remove_all(temp_dir);
}

// v14→v15: AD5X with stale wizard-era display overrides must be restored to
// preset values. Post-wizard users had hardware_blank=0, sleep_backlight_off=false
// which caused sleep-mode RGB color artifacts. See FLASHFORGE_AD5X_SUPPORT.md.
TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: v14→v15 restores AD5X sleep preset after wizard override",
                 "[core][config][migration][versioning]") {
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_migration_v15_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    // Simulate an AD5X user at v14 whose identify wizard ran AFTER the v12→v13
    // migration and wrote the stale (pre-#431) display config values.
    json v14_config = {{"config_version", 14},
                       {"preset", "ad5x"},
                       {"display",
                        {{"hardware_blank", 0},
                         {"sleep_backlight_off", false},
                         {"backlight_enable_ioctl", false}}},
                       {"printer", {{"moonraker_host", "127.0.0.1"}}}};
    {
        std::ofstream o(temp_path);
        o << v14_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/display/hardware_blank") == 1);
    REQUIRE(test_config.get<bool>("/display/sleep_backlight_off") == true);
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    std::filesystem::remove_all(temp_dir);
}

// Multi-printer variant: the printer type lives under /printers/<id>/type
// instead of the top-level preset key. Migration must detect both shapes.
TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: v14→v15 restores AD5X sleep preset (multi-printer config)",
                 "[core][config][migration][versioning]") {
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_migration_v15_mp_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v14_config = {
        {"config_version", 14},
        {"display",
         {{"hardware_blank", 0},
          {"sleep_backlight_off", false},
          {"backlight_enable_ioctl", false}}},
        {"printers",
         {{"my-ad5x", {{"type", "FlashForge Adventurer 5X"}, {"moonraker_host", "127.0.0.1"}}}}}};
    {
        std::ofstream o(temp_path);
        o << v14_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/display/hardware_blank") == 1);
    REQUIRE(test_config.get<bool>("/display/sleep_backlight_off") == true);

    std::filesystem::remove_all(temp_dir);
}

// Workaround-2 case: user intentionally set sleep_backlight_off=false but left
// hardware_blank at preset default (1). Migration must NOT overwrite their
// chosen workaround — detection requires BOTH stale values.
TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: v14→v15 preserves user workaround (sleep_backlight_off=false only)",
                 "[core][config][migration][versioning]") {
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_migration_v15_workaround_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v14_config = {{"config_version", 14},
                       {"preset", "ad5x"},
                       {"display",
                        {{"hardware_blank", 1},            // preset default
                         {"sleep_backlight_off", false}}}, // user's workaround
                       {"printer", {{"moonraker_host", "127.0.0.1"}}}};
    {
        std::ofstream o(temp_path);
        o << v14_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/display/hardware_blank") == 1);
    REQUIRE(test_config.get<bool>("/display/sleep_backlight_off") == false);

    std::filesystem::remove_all(temp_dir);
}

// Non-AD5X printers must not be touched by the migration.
TEST_CASE_METHOD(ConfigTestFixture, "Config: v14→v15 does not affect non-AD5X printers",
                 "[core][config][migration][versioning]") {
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_migration_v15_cc1_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    // CC1 preset legitimately has hardware_blank=0, sleep_backlight_off=false.
    // Migration must leave it alone.
    json v14_config = {{"config_version", 14},
                       {"preset", "cc1"},
                       {"display",
                        {{"hardware_blank", 0},
                         {"sleep_backlight_off", false},
                         {"backlight_enable_ioctl", false}}},
                       {"printer", {{"moonraker_host", "127.0.0.1"}}}};
    {
        std::ofstream o(temp_path);
        o << v14_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/display/hardware_blank") == 0);
    REQUIRE(test_config.get<bool>("/display/sleep_backlight_off") == false);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v4→v5 migration preserves explicit show_printer_switcher setting",
          "[core][config][migration][v5]") {
    std::string temp_dir = "/tmp/helix_test_v4_to_v5_explicit";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v4_config = {
        {"config_version", 4},
        {"active_printer_id", "ender3"},
        {"printers",
         {{"show_printer_switcher", true}, {"ender3", {{"moonraker_host", "192.168.1.50"}}}}}};

    {
        std::ofstream o(temp_path);
        o << v4_config.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    // Should NOT override the explicit true setting
    REQUIRE(test_config.get<bool>("/printers/show_printer_switcher") == true);

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

TEST_CASE_METHOD(ConfigTestFixture, "Config: remove_printer prevents removing last printer",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers", {{"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
    config.set_active_printer("voron");

    config.remove_printer("voron");

    // Should still have the printer — cannot remove the last one
    REQUIRE(config.get_printer_ids().size() == 1);
    REQUIRE(config.get_active_printer_id() == "voron");
}

// ---------------------------------------------------------------------------
// archive_printer(): the non-destructive removal used by automatic recovery.
// Phase 11b stale-printer recovery used to call remove_printer() directly, which
// erased a fully configured printer — host, heaters, macros, sensors — with no
// way back. archive_printer() must keep a verbatim copy under /removed_printers.
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: archive_printer preserves the full printer node under /removed_printers",
                 "[core][config][multi-printer]") {
    const json voron = {
        {"moonraker_host", "192.168.1.10"},
        {"moonraker_port", 7125},
        {"wizard_completed", false},
        {"printer_type", "voron-2-4"},
        {"nozzle_diameter", 0.4},
        {"favorite_macros", {"HOME_ALL", "PURGE_LINE"}},
        {"heaters", {{"extruder", {{"max_temp", 300}}}, {"bed", {{"max_temp", 120}}}}}};

    set_data_for_plural_test(
        {{"active_printer_id", "voron"},
         {"printers", {{"voron", voron}, {"ender3", {{"moonraker_host", "192.168.1.20"}}}}}});
    config.set_active_printer("voron");

    config.archive_printer("voron");

    // Gone from the active list...
    auto ids = config.get_printer_ids();
    REQUIRE(ids.size() == 1);
    REQUIRE(std::find(ids.begin(), ids.end(), "voron") == ids.end());

    // ...but preserved under /removed_printers, plus the archive-order stamp
    // the retention cap sorts on.
    REQUIRE(data_contains("removed_printers"));
    REQUIRE(get_data()["removed_printers"].contains("voron"));

    json archived = get_data()["removed_printers"]["voron"];
    REQUIRE(archived.contains(Config::ARCHIVED_AT_KEY));
    REQUIRE(archived[Config::ARCHIVED_AT_KEY].is_number_integer());
    archived.erase(Config::ARCHIVED_AT_KEY);
    REQUIRE(archived == voron);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: archive_printer auto-switches the active printer",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"ender3", {{"moonraker_host", "192.168.1.20"}}}}}});
    config.set_active_printer("voron");

    config.archive_printer("voron");

    // Same contract as remove_printer(): archiving the active printer must leave
    // a valid active selection behind, or df() routes at a missing node.
    REQUIRE(config.get_active_printer_id() == "ender3");
    REQUIRE(config.get<std::string>("/active_printer_id", "") == "ender3");
    REQUIRE(config.df() == "/printers/ender3/");
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: archive_printer archives nothing when it is the last printer",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers", {{"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
    config.set_active_printer("voron");

    config.archive_printer("voron");

    // remove_printer() declines to remove the last printer, so the archive must
    // not happen either — otherwise the config grows a phantom duplicate that
    // recovery UI would offer to "restore" over a printer that never left.
    REQUIRE(config.get_printer_ids().size() == 1);
    REQUIRE(config.get_active_printer_id() == "voron");
    REQUIRE(config.get<std::string>("/printers/voron/moonraker_host", "") == "192.168.1.10");
    REQUIRE_FALSE(data_contains("removed_printers"));
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: archive_printer accumulates instead of clobbering",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"ender3", {{"moonraker_host", "192.168.1.20"}}},
                                {"prusa", {{"moonraker_host", "192.168.1.30"}}}}}});
    config.set_active_printer("voron");

    config.archive_printer("voron");
    config.archive_printer("ender3");

    REQUIRE(config.get_printer_ids().size() == 1);
    REQUIRE(get_data()["removed_printers"].size() == 2);
    REQUIRE(get_data()["removed_printers"]["voron"]["moonraker_host"] == "192.168.1.10");
    REQUIRE(get_data()["removed_printers"]["ender3"]["moonraker_host"] == "192.168.1.20");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: archive_printer on an unknown id is a no-op",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"ender3", {{"moonraker_host", "192.168.1.20"}}}}}});
    config.set_active_printer("voron");

    config.archive_printer("does-not-exist");

    REQUIRE(config.get_printer_ids().size() == 2);
    REQUIRE(config.get_active_printer_id() == "voron");
    REQUIRE_FALSE(data_contains("removed_printers"));
}

// ---------------------------------------------------------------------------
// The /printers map is MIXED. Alongside the printer objects it holds plain
// settings keys: show_printer_switcher (a bool, seeded by get_default_config())
// and _show_printer_switcher_comment (a string, shipped in
// config/settings.json.template). EVERY real config has at least the bool, so
// code that treats the map as printers-only misbehaves on every device while
// passing tests built from a hand-written printers-only map — which is exactly
// how the cases below went missing.
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: remove_printer counts printers, not map entries, for the last-printer "
                 "guard",
                 "[core][config][multi-printer][1210]") {
    set_data_for_plural_test(
        {{"active_printer_id", "voron"},
         {"printers",
          {{"show_printer_switcher", false}, {"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
    REQUIRE(config.set_active_printer("voron"));

    config.remove_printer("voron");

    // printers.size() is 2 here, but only one entry is a printer: refusing must
    // still happen, or the user's only printer is erased.
    REQUIRE(config.get_printer_ids().size() == 1);
    REQUIRE(config.get<std::string>("/printers/voron/moonraker_host", "") == "192.168.1.10");
    REQUIRE(config.get_active_printer_id() == "voron");
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: remove_printer last-printer guard ignores the comment string key",
                 "[core][config][multi-printer][1210]") {
    set_data_for_plural_test(
        {{"active_printer_id", "voron"},
         {"printers",
          {{"_show_printer_switcher_comment", "Show the printer switcher in the header."},
           {"show_printer_switcher", true},
           {"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
    REQUIRE(config.set_active_printer("voron"));

    config.remove_printer("voron");

    REQUIRE(config.get_printer_ids().size() == 1);
    REQUIRE(config.get<std::string>("/printers/voron/moonraker_host", "") == "192.168.1.10");
    // The settings keys themselves are untouched.
    REQUIRE(get_data()["printers"]["show_printer_switcher"].get<bool>() == true);
    REQUIRE(get_data()["printers"]["_show_printer_switcher_comment"].is_string());
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: remove_printer refuses to erase a non-printer key",
                 "[core][config][multi-printer][1210]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"show_printer_switcher", true},
                                {"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"zortrax", {{"moonraker_host", "192.168.1.30"}}}}}});
    REQUIRE(config.set_active_printer("voron"));

    config.remove_printer("show_printer_switcher");

    // A mistyped/stale id that happens to name a settings key must not delete
    // the setting.
    REQUIRE(get_data()["printers"].contains("show_printer_switcher"));
    REQUIRE(get_data()["printers"]["show_printer_switcher"].get<bool>() == true);
    REQUIRE(config.get_printer_ids().size() == 2);
    REQUIRE(config.get_active_printer_id() == "voron");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: remove_printer auto-switch skips non-printer keys",
                 "[core][config][multi-printer][1210]") {
    // nlohmann orders object keys lexicographically, so "show_printer_switcher"
    // is the FIRST entry when every printer id sorts after it — the exact shape
    // that made an unfiltered begin() hand back the bool.
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"show_printer_switcher", false},
                                {"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"zortrax", {{"moonraker_host", "192.168.1.30"}}}}}});
    REQUIRE(config.set_active_printer("voron"));

    config.remove_printer("voron");

    REQUIRE(config.get_active_printer_id() == "zortrax");
    REQUIRE(config.get<std::string>("/active_printer_id", "") == "zortrax");
    REQUIRE(config.df() == "/printers/zortrax/");

    // The consequence of getting this wrong: df() addressed a boolean and the
    // next write through it threw json::type_error.
    config.set<std::string>(config.df() + "moonraker_host", "10.0.0.9");
    REQUIRE(config.get<std::string>("/printers/zortrax/moonraker_host", "") == "10.0.0.9");
    REQUIRE(get_data()["printers"]["show_printer_switcher"].is_boolean());
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: remove_printer auto-switch skips the comment string key",
                 "[core][config][multi-printer][1210]") {
    // "_..." sorts ahead of everything, so the first map entry is a string.
    set_data_for_plural_test(
        {{"active_printer_id", "voron"},
         {"printers",
          {{"_show_printer_switcher_comment", "Show the printer switcher in the header."},
           {"voron", {{"moonraker_host", "192.168.1.10"}}},
           {"zortrax", {{"moonraker_host", "192.168.1.30"}}}}}});
    REQUIRE(config.set_active_printer("voron"));

    config.remove_printer("voron");

    REQUIRE(config.get_active_printer_id() == "zortrax");
    REQUIRE(config.df() == "/printers/zortrax/");
    REQUIRE(get_data()["printers"]["_show_printer_switcher_comment"].is_string());
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: archive_printer auto-switch skips non-printer keys",
                 "[core][config][multi-printer][1210]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"show_printer_switcher", false},
                                {"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"zortrax", {{"moonraker_host", "192.168.1.30"}}}}}});
    REQUIRE(config.set_active_printer("voron"));

    config.archive_printer("voron");

    // archive_printer() delegates the switch to remove_printer(), so it inherits
    // the same contract.
    REQUIRE(config.get_active_printer_id() == "zortrax");
    REQUIRE(config.df() == "/printers/zortrax/");
    REQUIRE(get_data()["removed_printers"].contains("voron"));
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: reset_to_defaults re-points the active printer at the fresh defaults",
                 "[core][config][multi-printer][1210]") {
    set_data_for_plural_test(
        {{"active_printer_id", "voron"},
         {"printers",
          {{"show_printer_switcher", false}, {"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
    REQUIRE(config.set_active_printer("voron"));

    config.reset_to_defaults();

    // The defaults ship one printer keyed "default". A stale active id leaves
    // df() addressing a node that no longer exists — harmless until something
    // writes through it, which vivifies the ghost.
    REQUIRE(config.get_active_printer_id() == "default");
    REQUIRE(config.df() == "/printers/default/");
    REQUIRE(get_data()["printers"].contains("default"));

    config.set<int>(config.df() + "moonraker_port", 7126);
    REQUIRE(get_data()["printers"]["default"]["moonraker_port"].get<int>() == 7126);
    REQUIRE_FALSE(get_data()["printers"].contains("voron"));
}

// ---------------------------------------------------------------------------
// /removed_printers retention. Nothing reads the archive back yet, so an
// unbounded one is pure growth in settings.json and in every rolling backup of
// it.
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: archive_printer keeps only the most recently archived printers",
                 "[core][config][multi-printer][1210]") {
    const size_t total = Config::MAX_ARCHIVED_PRINTERS + 3;

    json printers = {{"show_printer_switcher", false},
                     // Never archived — keeps remove_printer()'s last-printer
                     // guard from firing partway through.
                     {"keeper", {{"moonraker_host", "10.0.0.1"}}}};
    std::vector<std::string> ids;
    for (size_t i = 0; i < total; i++) {
        std::string id = "printer-" + std::to_string(i);
        printers[id] = json{{"moonraker_host", "192.168.1." + std::to_string(i)}};
        ids.push_back(id);
    }
    set_data_for_plural_test({{"active_printer_id", "keeper"}, {"printers", printers}});
    REQUIRE(config.set_active_printer("keeper"));

    // Archive HIGHEST id first, so archive order is the reverse of key order:
    // a prune that sorted on the key rather than the stamp keeps the wrong set.
    for (size_t i = total; i-- > 0;) {
        config.archive_printer(ids[i]);
    }

    const json& archive = get_data()["removed_printers"];
    REQUIRE(archive.size() == Config::MAX_ARCHIVED_PRINTERS);

    for (size_t i = 0; i < total; i++) {
        INFO("printer index " << i);
        // printer-0 was archived last, so the survivors are the LOW indices.
        REQUIRE(archive.contains(ids[i]) == (i < Config::MAX_ARCHIVED_PRINTERS));
    }

    // Stamps are strictly ordered by archive order even though every archive
    // happened within the same wall-clock second.
    int64_t newer = 0;
    for (size_t i = 0; i < Config::MAX_ARCHIVED_PRINTERS; i++) {
        INFO("printer index " << i);
        REQUIRE(archive.at(ids[i]).contains(Config::ARCHIVED_AT_KEY));
        const int64_t stamp = archive.at(ids[i]).at(Config::ARCHIVED_AT_KEY).get<int64_t>();
        if (i > 0) {
            REQUIRE(stamp < newer);
        }
        newer = stamp;
    }
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: archive_printer prunes unstamped and malformed entries first",
                 "[core][config][multi-printer][1210]") {
    set_data_for_plural_test({{"active_printer_id", "zortrax"},
                              {"printers",
                               {{"show_printer_switcher", false},
                                {"voron", {{"moonraker_host", "192.168.1.10"}}},
                                {"zortrax", {{"moonraker_host", "192.168.1.30"}}}}}});
    REQUIRE(config.set_active_printer("zortrax"));

    // An archive written before entries carried a stamp, plus one whose stamp
    // is garbage. Neither may crash the prune, and both must sort as older than
    // anything stamped.
    json legacy = json::object();
    for (size_t i = 0; i < Config::MAX_ARCHIVED_PRINTERS; i++) {
        legacy["legacy-" + std::to_string(i)] = {{"moonraker_host", "172.16.0.1"}};
    }
    legacy["legacy-2"][Config::ARCHIVED_AT_KEY] = "not-a-number";
    legacy["legacy-3"][Config::ARCHIVED_AT_KEY] = nullptr;
    get_data()["removed_printers"] = legacy;

    config.archive_printer("voron");

    const json& archive = get_data()["removed_printers"];
    REQUIRE(archive.size() == Config::MAX_ARCHIVED_PRINTERS);
    // The stamped newcomer survives; the unstamped entry whose key sorts first
    // is the one dropped.
    REQUIRE(archive.contains("voron"));
    REQUIRE_FALSE(archive.contains("legacy-0"));
    for (size_t i = 1; i < Config::MAX_ARCHIVED_PRINTERS; i++) {
        INFO("legacy index " << i);
        REQUIRE(archive.contains("legacy-" + std::to_string(i)));
    }
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

TEST_CASE("StaticSubjectRegistry supports deinit/re-init cycles", "[.][core][registry]") {
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

TEST_CASE("StaticSubjectRegistry deinit_all runs callbacks in LIFO order", "[.][core][registry]") {
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
    // Per-printer storage: the marker lives under the active printer, not at the
    // config root. See test_config_preset_per_printer.cpp for the multi-printer
    // isolation and the legacy root-key lift.
    setup_printer_data(config, {{"preset", "ad5m"}});
    REQUIRE(config.has_preset() == true);
    REQUIRE(config.get_preset() == "ad5m");
}

TEST_CASE_METHOD(ConfigTestFixture, "has_preset returns false for empty preset string",
                 "[config][preset]") {
    setup_printer_data(config, {{"preset", ""}});
    REQUIRE(config.has_preset() == false);
    REQUIRE(config.get_preset().empty());
}

TEST_CASE_METHOD(ConfigTestFixture, "has_preset ignores a stray root-level preset key",
                 "[config][preset]") {
    // A root-level marker belongs to no printer in particular. init() lifts it
    // (see lift_root_preset); reading it here would let one printer's preset leak
    // into a printer that has none of its own — the #1162 bug.
    setup_printer_data(config, json::object());
    get_data()["preset"] = "ad5m";
    REQUIRE(config.has_preset() == false);
    REQUIRE(config.get_preset().empty());
}

// --- Config::save() symlink preservation tests ---

TEST_CASE_METHOD(ConfigTestFixture, "save preserves symlink when config path is a symlink",
                 "[config][save][symlink]") {
    namespace fs = std::filesystem;

    // Create a temp directory structure: real file lives in "target/", symlink in "link/"
    auto tmp = fs::temp_directory_path() / ("helix_test_symlink_" + std::to_string(getpid()));
    fs::create_directories(tmp / "target");
    fs::create_directories(tmp / "link");

    auto real_file = tmp / "target" / "helixconfig.json";
    auto symlink_path = tmp / "link" / "helixconfig.json";

    // Write initial content to real file
    {
        std::ofstream o(real_file);
        o << R"({"initial": true})";
    }

    // Create symlink pointing to real file
    fs::create_symlink(real_file, symlink_path);
    REQUIRE(fs::is_symlink(symlink_path));

    // Set up config to save through the symlink
    set_path(symlink_path.string());
    get_data() = {{"saved", true}, {"value", 42}};

    REQUIRE(config.save());

    // Symlink must still exist and point to the same target
    REQUIRE(fs::is_symlink(symlink_path));
    REQUIRE(fs::read_symlink(symlink_path) == real_file);

    // Real file must contain the new data
    std::ifstream in(real_file);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("\"saved\": true") != std::string::npos);
    REQUIRE(content.find("\"value\": 42") != std::string::npos);

    // Cleanup
    fs::remove_all(tmp);
}

TEST_CASE_METHOD(ConfigTestFixture, "save works normally with regular file (no symlink)",
                 "[config][save]") {
    namespace fs = std::filesystem;

    auto tmp = fs::temp_directory_path() / ("helix_test_regular_" + std::to_string(getpid()));
    fs::create_directories(tmp);

    auto file_path = tmp / "helixconfig.json";

    // Write initial content
    {
        std::ofstream o(file_path);
        o << R"({"initial": true})";
    }

    set_path(file_path.string());
    get_data() = {{"regular_save", true}};

    REQUIRE(config.save());

    // File should be a regular file (not a symlink)
    REQUIRE(!fs::is_symlink(file_path));

    std::ifstream in(file_path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("\"regular_save\": true") != std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("Config::init migrates helixconfig.json to settings.json", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "test_config_migration";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "config");

    std::string old_path = (tmp / "config" / "helixconfig.json").string();
    std::string new_path = (tmp / "config" / "settings.json").string();
    {
        std::ofstream f(old_path);
        f << R"({"config_version": 8, "printers": [{"name": "test", "moonraker_address": "127.0.0.1"}]})";
    }

    REQUIRE(std::filesystem::exists(old_path));
    REQUIRE_FALSE(std::filesystem::exists(new_path));

    auto config = Config::get_instance();
    config->init(new_path);

    REQUIRE_FALSE(std::filesystem::exists(old_path));
    REQUIRE(std::filesystem::exists(new_path));

    std::filesystem::remove_all(tmp);
}

// ============================================================================
// RAII guard for HOME — moved here so corruption tests can also use it.
// ============================================================================

namespace {

/// RAII guard for HOME environment variable — ensures cleanup even if assertions throw.
/// Prevents environ poisoning that creates junk single-char directories in CWD.
///
/// Also redirects the primary (StateDirectory) backup tier under the same temp
/// home. The primary tier outranks the $HOME fallback in the search order, so
/// without this a real /var/lib/helixscreen/settings.json.backup on the host
/// would be restored instead of the fixture's backup — which is why these tests
/// used to SKIP on any machine that had ever installed HelixScreen.
struct HomeGuard {
    std::string original;
    std::string prev_state_dir;
    std::string prev_fallback_dir;
    bool had_home;
    explicit HomeGuard(const std::string& new_home)
        : prev_state_dir(AppConstants::Update::detail::state_dir_ref()),
          prev_fallback_dir(AppConstants::Update::detail::backup_fallback_dir_ref()),
          had_home(std::getenv("HOME") != nullptr) {
        if (had_home)
            original = std::getenv("HOME");
        setenv("HOME", new_home.c_str(), 1);
        AppConstants::Update::detail::backup_fallback_dir_ref() =
            AppConstants::Update::sanitize_home(std::getenv("HOME")) + "/.helixscreen";
        AppConstants::Update::detail::state_dir_ref() = new_home + "/var-lib";
    }
    ~HomeGuard() {
        if (had_home)
            setenv("HOME", original.c_str(), 1);
        else
            unsetenv("HOME");
        // Restore the SAVED value — see the note in ~BackupGuard().
        AppConstants::Update::detail::backup_fallback_dir_ref() = prev_fallback_dir;
        AppConstants::Update::detail::state_dir_ref() = prev_state_dir;
    }
};

} // namespace

// ============================================================================
// Corrupt Config Recovery Tests
// ============================================================================

TEST_CASE("Config::init() recovers from corrupt config by restoring backup",
          "[core][config][corruption]") {
    // HomeGuard (below) redirects both backup tiers into temp_dir, so a real
    // /var/lib/helixscreen backup on the host cannot outrank this test's backup.
    std::string temp_dir = "/tmp/helix_test_corrupt_backup_" + std::to_string(getpid());
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/settings.json";

    // Write corrupt JSON
    {
        std::ofstream o(temp_path);
        o << "{{{{ not valid json !!!!";
    }

    // RAII guard: restore HOME even if REQUIRE throws
    HomeGuard home_guard(temp_dir);

    // Write a valid backup at the fallback backup location
    std::string backup_dir = temp_dir + "/.helixscreen";
    std::filesystem::create_directories(backup_dir);
    std::string backup_path = backup_dir + "/settings.json.backup";
    json backup_data = {{"config_version", CURRENT_CONFIG_VERSION},
                        {"active_printer_id", "voron"},
                        {"brightness", 75},
                        {"printers", {{"voron", {{"moonraker_host", "10.0.0.5"}}}}}};
    {
        std::ofstream o(backup_path);
        o << backup_data.dump(2);
    }

    Config test_config;
    test_config.init(temp_path);

    // Should have restored from backup, not fallen back to defaults
    REQUIRE(test_config.get<std::string>("/printers/voron/moonraker_host") == "10.0.0.5");
    REQUIRE(test_config.get<int>("/brightness") == 75);

    // Corrupt file should be preserved for diagnosis
    REQUIRE(std::filesystem::exists(temp_path + ".corrupt"));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config::init() falls back to defaults when corrupt and no backup exists",
          "[core][config][corruption]") {
    std::string temp_dir = "/tmp/helix_test_corrupt_nobackup_" + std::to_string(getpid());
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/settings.json";

    // Write corrupt JSON
    {
        std::ofstream o(temp_path);
        o << "not json at all";
    }

    // RAII guard: redirect both backup tiers into temp_dir and restore them (and
    // HOME) even if REQUIRE throws. Redirecting the primary tier keeps the test
    // independent of a real /var/lib/helixscreen backup on the host.
    HomeGuard home_guard(temp_dir);

    Config test_config;
    test_config.init(temp_path);

    // Should have fallen back to defaults (127.0.0.1)
    REQUIRE(test_config.get<std::string>("/printers/default/moonraker_host") == "127.0.0.1");

    // Corrupt file should still be preserved
    REQUIRE(std::filesystem::exists(temp_path + ".corrupt"));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config::init() falls back to defaults when both config and backup are corrupt",
          "[core][config][corruption]") {
    std::string temp_dir = "/tmp/helix_test_both_corrupt_" + std::to_string(getpid());
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/settings.json";

    // Write corrupt config
    {
        std::ofstream o(temp_path);
        o << "{truncated";
    }

    // RAII guard: redirect both backup tiers into temp_dir and restore them (and
    // HOME) even if REQUIRE throws. Redirecting the primary tier keeps the test
    // independent of a real /var/lib/helixscreen backup on the host.
    HomeGuard home_guard(temp_dir);

    std::string backup_dir = temp_dir + "/.helixscreen";
    std::filesystem::create_directories(backup_dir);
    {
        std::ofstream o(backup_dir + "/settings.json.backup");
        o << "also not valid json";
    }

    Config test_config;
    test_config.init(temp_path);

    // Both corrupt — should fall back to defaults
    REQUIRE(test_config.get<std::string>("/printers/default/moonraker_host") == "127.0.0.1");

    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// Tarball Default Detection Tests (Moonraker web update config clobber)
//
// HomeGuard redirects both backup tiers into the temp dir, so these run on
// developer machines with HelixScreen installed as well as on clean CI boxes.
// ============================================================================

namespace {

/// RAII temp directory + backup-tier redirect for tarball detection tests.
struct TarballTestEnv {
    std::filesystem::path dir;
    std::string config_path;
    std::string backup_dir;
    HomeGuard home;

    explicit TarballTestEnv(const std::string& name)
        : dir("/tmp/helix_test_" + name + "_" + std::to_string(getpid())),
          config_path((dir / "settings.json").string()),
          backup_dir((dir / ".helixscreen").string()), home(dir.string()) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }

    ~TarballTestEnv() {
        std::filesystem::remove_all(dir);
    }

    void write_config(const json& j) {
        std::ofstream o(config_path);
        o << j.dump(2);
    }

    void write_backup(const json& j) {
        std::filesystem::create_directories(backup_dir);
        std::ofstream o(backup_dir + "/settings.json.backup");
        o << j.dump(2);
    }

    void write_backup_raw(const std::string& content) {
        std::filesystem::create_directories(backup_dir);
        std::ofstream o(backup_dir + "/settings.json.backup");
        o << content;
    }

    /// The installer's "this packaged config is here on purpose" marker,
    /// written beside settings.json.
    std::filesystem::path fresh_marker() const {
        return dir / AppConstants::Update::FRESH_INSTALL_MARKER;
    }

    void write_fresh_marker() {
        std::ofstream o(fresh_marker());
    }
};

} // namespace

TEST_CASE("Config::init() restores backup when tarball default replaces user config",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("tarball_default");

    env.write_config({{"preset", "ad5x"},
                      {"wizard_completed", false},
                      {"display", {{"rotate", 0}}},
                      {"printer", {{"moonraker_host", "127.0.0.1"}}}});

    env.write_backup({{"config_version", CURRENT_CONFIG_VERSION},
                      {"active_printer_id", "my-ad5x"},
                      {"wizard_completed", true},
                      {"brightness", 80},
                      {"printers",
                       {{"my-ad5x",
                         {{"moonraker_host", "192.168.1.50"},
                          {"wizard_completed", true},
                          {"heaters", {{"bed", "heater_bed"}}}}}}}});

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.get<std::string>("/printers/my-ad5x/moonraker_host") == "192.168.1.50");
    REQUIRE(test_config.get<int>("/brightness") == 80);
    REQUIRE_FALSE(test_config.is_wizard_required());

    // Restored config should be persisted to disk
    auto on_disk = json::parse(std::ifstream(env.config_path));
    REQUIRE(on_disk.value("config_version", 0) > 0);
    REQUIRE(on_disk.contains("printers"));
}

TEST_CASE("Config::init() keeps tarball default when no backup exists (fresh install)",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("tarball_fresh");

    env.write_config({{"preset", "ad5m"},
                      {"wizard_completed", false},
                      {"printer", {{"moonraker_host", "127.0.0.1"}}}});

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.is_wizard_required());
}

TEST_CASE("Config::init() keeps tarball default when backup is also a tarball default",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("tarball_both_default");

    json tarball_default = {{"preset", "ad5x"},
                            {"wizard_completed", false},
                            {"printer", {{"moonraker_host", "127.0.0.1"}}}};
    env.write_config(tarball_default);
    env.write_backup(tarball_default);

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.is_wizard_required());
}

TEST_CASE("Config::init() keeps a shipped preset when the installer marked a fresh install",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("tarball_fresh_marker");

    // assets/config/presets/k1.json in shape: a platform preset carrying the
    // display rotation the panel physically needs.
    env.write_config({{"preset", "k1"},
                      {"wizard_completed", false},
                      {"display", {{"rotate", 270}, {"rotation_probed", true}}},
                      {"printer", {{"heaters", {{"bed", "heater_bed"}}}}}});

    // A stale rolling backup left behind by an earlier install. Without the
    // marker this would replace the preset wholesale.
    env.write_backup({{"config_version", CURRENT_CONFIG_VERSION},
                      {"active_printer_id", "other"},
                      {"wizard_completed", true},
                      {"brightness", 80},
                      {"display", {{"rotate", 0}}},
                      {"printers", {{"other", {{"moonraker_host", "192.168.1.50"}}}}}});

    env.write_fresh_marker();

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.get<int>("/display/rotate", -1) == 270);
    REQUIRE(test_config.get<int>("/brightness", -1) == -1);
    REQUIRE(test_config.is_wizard_required());
}

TEST_CASE("Config::init() consumes the fresh-install marker", "[core][config][moonraker-update]") {
    TarballTestEnv env("tarball_marker_consumed");

    env.write_config({{"preset", "k1"},
                      {"wizard_completed", false},
                      {"display", {{"rotate", 270}}},
                      {"printer", {{"heaters", {{"bed", "heater_bed"}}}}}});
    env.write_backup({{"config_version", CURRENT_CONFIG_VERSION},
                      {"active_printer_id", "other"},
                      {"brightness", 80},
                      {"printers", {{"other", {{"moonraker_host", "192.168.1.50"}}}}}});
    env.write_fresh_marker();
    REQUIRE(std::filesystem::exists(env.fresh_marker()));

    Config test_config;
    test_config.init(env.config_path);

    // One-shot: the marker only ever answers for the config it shipped beside,
    // so a later unversioned document must not inherit its verdict.
    REQUIRE_FALSE(std::filesystem::exists(env.fresh_marker()));
}

TEST_CASE("Config::init() keeps a shipped preset when the marker cannot be deleted",
          "[core][config][moonraker-update]") {
    // root bypasses the permission bits this case depends on, so the marker cannot be deleted
    // and the assertion below would be asserting the opposite of the point.
    if (::geteuid() == 0) {
        SKIP("Test requires non-root euid - root bypasses permission bits");
    }
    TarballTestEnv env("tarball_marker_readonly");

    env.write_config({{"preset", "k1"},
                      {"wizard_completed", false},
                      {"display", {{"rotate", 270}}},
                      {"printer", {{"heaters", {{"bed", "heater_bed"}}}}}});
    env.write_backup({{"config_version", CURRENT_CONFIG_VERSION},
                      {"active_printer_id", "other"},
                      {"brightness", 80},
                      {"printers", {{"other", {{"moonraker_host", "192.168.1.50"}}}}}});
    env.write_fresh_marker();

    // Read-only storage: the marker survives because unlink needs write on the
    // directory. Restore the mode however the test exits so the fixture can
    // still clean up.
    struct ModeGuard {
        std::filesystem::path p;
        ~ModeGuard() {
            std::error_code ec;
            std::filesystem::permissions(p, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::add, ec);
        }
    } guard{env.dir};
    std::filesystem::permissions(env.dir, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::remove);

    Config test_config;
    REQUIRE_NOTHROW(test_config.init(env.config_path));

    // The delete really did fail - otherwise this test proves nothing about
    // the read-only path.
    REQUIRE(std::filesystem::exists(env.fresh_marker()));

    // fs::remove takes an error_code, so a failed delete is not fatal. The
    // config also cannot be stamped on read-only storage, so the marker keeps
    // answering for the same undeletable document on every later boot - which
    // is the stable outcome, not a stale verdict about a different one.
    REQUIRE(test_config.get<int>("/display/rotate", -1) == 270);
    REQUIRE(test_config.get<int>("/brightness", -1) == -1);
}

TEST_CASE("Config::init() discards a shipped preset with no fresh-install marker",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("tarball_no_marker_clobber");

    // Byte-for-byte what a fresh install ships - but Moonraker's type:web
    // rmtree() + re-extract puts the identical file here after destroying the
    // user's config, and takes the installer's marker with it.
    env.write_config({{"preset", "k1"},
                      {"wizard_completed", false},
                      {"display", {{"rotate", 270}, {"rotation_probed", true}}},
                      {"printer", {{"heaters", {{"bed", "heater_bed"}}}}}});

    env.write_backup({{"config_version", CURRENT_CONFIG_VERSION},
                      {"active_printer_id", "my-k1"},
                      {"wizard_completed", true},
                      {"brightness", 80},
                      {"display", {{"rotate", 90}}},
                      {"printers", {{"my-k1", {{"moonraker_host", "192.168.1.50"}}}}}});

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.get<std::string>("/printers/my-k1/moonraker_host") == "192.168.1.50");
    REQUIRE(test_config.get<int>("/brightness", -1) == 80);
    REQUIRE(test_config.get<int>("/display/rotate", -1) == 90);
    REQUIRE_FALSE(test_config.is_wizard_required());
}

TEST_CASE("Config::init() keeps tarball default when backup is corrupt",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("tarball_corrupt_backup");

    env.write_config({{"preset", "ad5m"},
                      {"wizard_completed", false},
                      {"printer", {{"moonraker_host", "127.0.0.1"}}}});
    env.write_backup_raw("{{{{ not valid json");

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.is_wizard_required());
}

// ============================================================================
// v10→v11 migration: PID heat rates to shared thermal path + strip heating phases
// ============================================================================

TEST_CASE("Config: v10→v11 migration moves heat rates and strips heating phases",
          "[core][config][migration][v11]") {
    std::string temp_dir = "/tmp/helix_test_v10_to_v11_" + std::to_string(rand());
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v10_config = {
        {"config_version", 10},
        {"calibration",
         {{"pid_history",
           {{"extruder", {{"heat_rate", 1.25}, {"oscillation_duration", 3.5}}},
            {"heater_bed", {{"heat_rate", 0.42}, {"oscillation_duration", 8.0}}}}}}},
        {"print_start_history",
         {{"entries",
           json::array({{{"phases", {{"0", 5.0}, {"1", 10.0}, {"3", 30.0}, {"4", 20.0}}}},
                        {{"phases", {{"0", 6.0}, {"3", 25.0}, {"4", 15.0}}}},
                        {{"no_phases", true}}})}}},
        {"printer", {{"moonraker_host", "127.0.0.1"}, {"moonraker_port", 7125}}}};

    {
        std::ofstream o(temp_path);
        o << v10_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    // Heat rates should be at new location
    REQUIRE(test_config.get<double>("/thermal/rates/extruder/heat_rate") == Catch::Approx(1.25));
    REQUIRE(test_config.get<double>("/thermal/rates/heater_bed/heat_rate") == Catch::Approx(0.42));

    // Old heat_rate keys should be gone
    REQUIRE_FALSE(test_config.exists("/calibration/pid_history/extruder/heat_rate"));
    REQUIRE_FALSE(test_config.exists("/calibration/pid_history/heater_bed/heat_rate"));

    // Oscillation duration should be preserved
    REQUIRE(test_config.get<double>("/calibration/pid_history/extruder/oscillation_duration") ==
            Catch::Approx(3.5));
    REQUIRE(test_config.get<double>("/calibration/pid_history/heater_bed/oscillation_duration") ==
            Catch::Approx(8.0));

    // Heating phase durations (keys "3" and "4") should be stripped
    auto entries = test_config.get<json>("/print_start_history/entries");
    REQUIRE(entries.size() == 3);

    // First entry: had phases 0, 1, 3, 4 → should keep 0, 1
    REQUIRE(entries[0]["phases"].contains("0"));
    REQUIRE(entries[0]["phases"].contains("1"));
    REQUIRE_FALSE(entries[0]["phases"].contains("3"));
    REQUIRE_FALSE(entries[0]["phases"].contains("4"));

    // Second entry: had phases 0, 3, 4 → should keep 0
    REQUIRE(entries[1]["phases"].contains("0"));
    REQUIRE_FALSE(entries[1]["phases"].contains("3"));
    REQUIRE_FALSE(entries[1]["phases"].contains("4"));

    // Third entry: no phases key → unchanged
    REQUIRE(entries[2].contains("no_phases"));
    REQUIRE_FALSE(entries[2].contains("phases"));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v10→v11 migration is idempotent", "[core][config][migration][v11]") {
    std::string temp_dir = "/tmp/helix_test_v10_to_v11_idempotent_" + std::to_string(rand());
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    // Config already at v11 with thermal rates in the new location
    json v11_config = {
        {"config_version", 10},
        {"thermal", {{"rates", {{"extruder", {{"heat_rate", 1.25}}}}}}},
        {"calibration",
         {{"pid_history", {{"extruder", {{"heat_rate", 0.99}, {"oscillation_duration", 3.5}}}}}}},
        {"printer", {{"moonraker_host", "127.0.0.1"}, {"moonraker_port", 7125}}}};

    {
        std::ofstream o(temp_path);
        o << v11_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    // Should keep existing value at new path (not overwrite with old)
    REQUIRE(test_config.get<double>("/thermal/rates/extruder/heat_rate") == Catch::Approx(1.25));

    // Old heat_rate should still be erased
    REQUIRE_FALSE(test_config.exists("/calibration/pid_history/extruder/heat_rate"));

    // Oscillation duration preserved
    REQUIRE(test_config.get<double>("/calibration/pid_history/extruder/oscillation_duration") ==
            Catch::Approx(3.5));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v10→v11 migration handles missing calibration data gracefully",
          "[core][config][migration][v11]") {
    std::string temp_dir = "/tmp/helix_test_v10_to_v11_empty_" + std::to_string(rand());
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    // Minimal v10 config with no calibration or print history
    json v10_config = {{"config_version", 10},
                       {"printer", {{"moonraker_host", "127.0.0.1"}, {"moonraker_port", 7125}}}};

    {
        std::ofstream o(temp_path);
        o << v10_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    // No crash, no thermal section created
    REQUIRE_FALSE(test_config.exists("/thermal"));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v11→v12 migration consolidates chamber keys for all printers",
          "[core][config][migration][v12]") {
    std::string temp_dir = "/tmp/helix_test_v11_to_v12_" + std::to_string(rand());
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v11_config = {
        {"config_version", 11},
        {"active_printer_id", "k1c"},
        {"printers",
         {{"k1c",
           {{"printer",
             {{"chamber_sensor", "temperature_sensor chamber_temp"},
              {"chamber_heater", "temperature_fan chamber_fan"}}}}},
          {"voron", {{"printer", {{"chamber_sensor", "temperature_sensor chamber"}}}}},
          {"bare", {{"heaters", {{"bed", "heater_bed"}}}}}}}};

    {
        std::ofstream o(temp_path);
        o << v11_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    // k1c: both keys moved to canonical paths
    REQUIRE(test_config.get<std::string>("/printers/k1c/temp_sensors/chamber") ==
            "temperature_sensor chamber_temp");
    REQUIRE(test_config.get<std::string>("/printers/k1c/heaters/chamber") ==
            "temperature_fan chamber_fan");
    REQUIRE_FALSE(test_config.exists("/printers/k1c/printer/chamber_sensor"));
    REQUIRE_FALSE(test_config.exists("/printers/k1c/printer/chamber_heater"));
    // Empty /printer subkey removed after migration
    REQUIRE_FALSE(test_config.exists("/printers/k1c/printer"));

    // voron: sensor-only migration still works
    REQUIRE(test_config.get<std::string>("/printers/voron/temp_sensors/chamber") ==
            "temperature_sensor chamber");
    REQUIRE_FALSE(test_config.exists("/printers/voron/printer/chamber_sensor"));
    REQUIRE_FALSE(test_config.exists("/printers/voron/printer"));

    // bare: no chamber keys to migrate, untouched
    REQUIRE(test_config.get<std::string>("/printers/bare/heaters/bed") == "heater_bed");
    REQUIRE_FALSE(test_config.exists("/printers/bare/temp_sensors/chamber"));

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: v11→v12 migration does not overwrite canonical keys already set",
          "[core][config][migration][v12]") {
    std::string temp_dir = "/tmp/helix_test_v11_to_v12_existing_" + std::to_string(rand());
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    json v11_config = {{"config_version", 11},
                       {"printers",
                        {{"k1c",
                          {{"temp_sensors", {{"chamber", "temperature_sensor new_chamber"}}},
                           {"printer", {{"chamber_sensor", "temperature_sensor old_chamber"}}}}}}}};

    {
        std::ofstream o(temp_path);
        o << v11_config.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    // Canonical value preserved; legacy value discarded (migrate_config_keys behavior)
    REQUIRE(test_config.get<std::string>("/printers/k1c/temp_sensors/chamber") ==
            "temperature_sensor new_chamber");
    REQUIRE_FALSE(test_config.exists("/printers/k1c/printer/chamber_sensor"));

    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// v15→v16 migration: screensaver off on BASIC/EMBEDDED tier with Flying Toasters
// ============================================================================

TEST_CASE("Config: v15→v16 migration disables Flying Toasters on constrained tiers",
          "[config][migration][v16]") {
    struct TierGuard {
        ~TierGuard() {
            helix::config_testing::set_forced_tier_for_migration(std::nullopt);
        }
    };
    TierGuard _tier_guard; // Ensures override is reset even if a SECTION fails

    SECTION("BASIC tier with Flying Toasters: type flipped to 0 and notice queued") {
        helix::config_testing::set_forced_tier_for_migration(helix::PlatformTier::BASIC);

        std::string temp_dir = "/tmp/helix_test_v15_to_v16_basic_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v15_config = {{"config_version", 15}, {"display", {{"screensaver_type", 1}}}};
        {
            std::ofstream o(temp_path);
            o << v15_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
        REQUIRE(test_config.get<int>("/display/screensaver_type", 1) == 0);
        REQUIRE(test_config.get<bool>("/display/screensaver_migration_notice_pending", false) ==
                true);

        std::filesystem::remove_all(temp_dir);
    }

    SECTION("EMBEDDED tier with Flying Toasters: type flipped to 0 and notice queued") {
        helix::config_testing::set_forced_tier_for_migration(helix::PlatformTier::EMBEDDED);

        std::string temp_dir = "/tmp/helix_test_v15_to_v16_embedded_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v15_config = {{"config_version", 15}, {"display", {{"screensaver_type", 1}}}};
        {
            std::ofstream o(temp_path);
            o << v15_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
        REQUIRE(test_config.get<int>("/display/screensaver_type", 1) == 0);
        REQUIRE(test_config.get<bool>("/display/screensaver_migration_notice_pending", false) ==
                true);

        std::filesystem::remove_all(temp_dir);
    }

    SECTION("STANDARD tier with Flying Toasters: setting untouched, no notice flag") {
        helix::config_testing::set_forced_tier_for_migration(helix::PlatformTier::STANDARD);

        std::string temp_dir = "/tmp/helix_test_v15_to_v16_standard_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v15_config = {{"config_version", 15}, {"display", {{"screensaver_type", 1}}}};
        {
            std::ofstream o(temp_path);
            o << v15_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        REQUIRE(test_config.get<int>("/display/screensaver_type", 0) == 1);
        REQUIRE_FALSE(test_config.exists("/display/screensaver_migration_notice_pending"));

        std::filesystem::remove_all(temp_dir);
    }

    SECTION("BASIC tier with Starfield (type=2): untouched, no notice flag") {
        helix::config_testing::set_forced_tier_for_migration(helix::PlatformTier::BASIC);

        std::string temp_dir = "/tmp/helix_test_v15_to_v16_starfield_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v15_config = {{"config_version", 15}, {"display", {{"screensaver_type", 2}}}};
        {
            std::ofstream o(temp_path);
            o << v15_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        REQUIRE(test_config.get<int>("/display/screensaver_type", 0) == 2);
        REQUIRE_FALSE(test_config.exists("/display/screensaver_migration_notice_pending"));

        std::filesystem::remove_all(temp_dir);
    }

    SECTION("BASIC tier with Pipes 3D (type=3): untouched, no notice flag") {
        helix::config_testing::set_forced_tier_for_migration(helix::PlatformTier::BASIC);

        std::string temp_dir = "/tmp/helix_test_v15_to_v16_pipes_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v15_config = {{"config_version", 15}, {"display", {{"screensaver_type", 3}}}};
        {
            std::ofstream o(temp_path);
            o << v15_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        REQUIRE(test_config.get<int>("/display/screensaver_type", 0) == 3);
        REQUIRE_FALSE(test_config.exists("/display/screensaver_migration_notice_pending"));

        std::filesystem::remove_all(temp_dir);
    }

    SECTION("BASIC tier with screensaver already Off (type=0): no-op, no notice flag") {
        helix::config_testing::set_forced_tier_for_migration(helix::PlatformTier::BASIC);

        std::string temp_dir = "/tmp/helix_test_v15_to_v16_off_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v15_config = {{"config_version", 15}, {"display", {{"screensaver_type", 0}}}};
        {
            std::ofstream o(temp_path);
            o << v15_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        REQUIRE(test_config.get<int>("/display/screensaver_type", 1) == 0);
        REQUIRE_FALSE(test_config.exists("/display/screensaver_migration_notice_pending"));

        std::filesystem::remove_all(temp_dir);
    }

    SECTION("Already-v16 config: migration does not run, type stays as set") {
        helix::config_testing::set_forced_tier_for_migration(helix::PlatformTier::BASIC);

        std::string temp_dir = "/tmp/helix_test_v15_to_v16_already_v16_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v16_config = {{"config_version", 16}, {"display", {{"screensaver_type", 1}}}};
        {
            std::ofstream o(temp_path);
            o << v16_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        // Migration gate (version < 16) must not fire for already-v16 config
        REQUIRE(test_config.get<int>("/display/screensaver_type", 0) == 1);
        REQUIRE_FALSE(test_config.exists("/display/screensaver_migration_notice_pending"));

        std::filesystem::remove_all(temp_dir);
    }
}

TEST_CASE("Config: v16→v17 migration renames retired Voron printer_image IDs",
          "[core][config][migration][v17]") {
    SECTION("voron-24r2 and voron-0-2 rewritten; unrelated values left alone") {
        std::string temp_dir = "/tmp/helix_test_v16_to_v17_rename_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v16_config = {{"config_version", 16},
                           {"active_printer_id", "voron24"},
                           {"printers",
                            {{"voron24", {{"printer_image", "shipped:voron-24r2"}}},
                             {"voron0", {{"printer_image", "shipped:voron-0-2"}}},
                             {"sw", {{"printer_image", "shipped:voron-switchwire"}}},
                             {"custom", {{"printer_image", "custom:my-printer"}}},
                             {"auto", {{"printer_image", ""}}},
                             {"none", {{"heaters", {{"bed", "heater_bed"}}}}}}}};

        {
            std::ofstream o(temp_path);
            o << v16_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

        REQUIRE(test_config.get<std::string>("/printers/voron24/printer_image") ==
                "shipped:voron-v2");
        REQUIRE(test_config.get<std::string>("/printers/voron0/printer_image") ==
                "shipped:voron-v0");
        REQUIRE(test_config.get<std::string>("/printers/sw/printer_image") ==
                "shipped:voron-switchwire");
        REQUIRE(test_config.get<std::string>("/printers/custom/printer_image") ==
                "custom:my-printer");
        REQUIRE(test_config.get<std::string>("/printers/auto/printer_image", "missing") == "");
        REQUIRE_FALSE(test_config.exists("/printers/none/printer_image"));

        std::filesystem::remove_all(temp_dir);
    }

    SECTION("Already-v17 config: migration does not run") {
        std::string temp_dir = "/tmp/helix_test_v16_to_v17_already_" + std::to_string(rand());
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/test_config.json";

        json v17_config = {{"config_version", 17},
                           {"active_printer_id", "voron24"},
                           {"printers", {{"voron24", {{"printer_image", "shipped:voron-24r2"}}}}}};

        {
            std::ofstream o(temp_path);
            o << v17_config.dump(2);
        }

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);

        // Already-v17: stale value is preserved verbatim, migration gate (version<17) skipped.
        REQUIRE(test_config.get<std::string>("/printers/voron24/printer_image") ==
                "shipped:voron-24r2");

        std::filesystem::remove_all(temp_dir);
    }
}

// ============================================================================
// Test-Mode Backup Isolation Tests (#1105)
//
// `--test` declares its own config (config/settings-test.json) but the backup
// cascade searched PRODUCTION locations (/var/lib/helixscreen/, $HOME/.helixscreen/).
// A missing test config was therefore silently overwritten with a stale real
// config, and saving in test mode clobbered the real user's rolling backup with
// test data. Test mode must never read or write production backups.
// ============================================================================

namespace {

/// RAII: force RuntimeConfig::test_mode and restore the previous value.
struct ConfigTestModeGuard {
    RuntimeConfig* rc;
    bool prev;
    explicit ConfigTestModeGuard(bool enabled) : rc(get_runtime_config()), prev(rc->test_mode) {
        rc->test_mode = enabled;
    }
    ~ConfigTestModeGuard() {
        rc->test_mode = prev;
    }
};

/// RAII: redirect BOTH backup tiers (primary StateDirectory + $HOME fallback)
/// into a temp dir, so a real /var/lib/helixscreen backup on the dev box cannot
/// influence the test, and the test cannot touch the real one.
struct BackupSandbox {
    std::filesystem::path dir;
    std::string prev_state_dir;
    std::string prev_fallback_dir;
    std::string original_home;
    bool had_home;

    explicit BackupSandbox(const std::string& name)
        : dir("/tmp/helix_test_" + name + "_" + std::to_string(getpid()) + "_" +
              std::to_string(rand())),
          prev_state_dir(AppConstants::Update::detail::state_dir_ref()),
          prev_fallback_dir(AppConstants::Update::detail::backup_fallback_dir_ref()),
          had_home(std::getenv("HOME") != nullptr) {
        if (had_home)
            original_home = std::getenv("HOME");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        setenv("HOME", dir.string().c_str(), 1);
        AppConstants::Update::detail::state_dir_ref() = (dir / "var-lib").string();
        AppConstants::Update::detail::backup_fallback_dir_ref() = (dir / ".helixscreen").string();
    }

    ~BackupSandbox() {
        if (had_home)
            setenv("HOME", original_home.c_str(), 1);
        else
            unsetenv("HOME");
        AppConstants::Update::detail::state_dir_ref() = prev_state_dir;
        AppConstants::Update::detail::backup_fallback_dir_ref() = prev_fallback_dir;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string config_path() const {
        return (dir / "settings-test.json").string();
    }

    /// Write a backup at the primary (StateDirectory) tier.
    void write_primary_backup(const json& j) {
        std::filesystem::create_directories(AppConstants::Update::state_dir());
        std::ofstream o(AppConstants::Update::config_backup_primary());
        o << j.dump(2);
    }

    bool primary_backup_exists() const {
        return std::filesystem::exists(AppConstants::Update::config_backup_primary());
    }

    json read_primary_backup() const {
        return json::parse(std::fstream(AppConstants::Update::config_backup_primary()));
    }
};

/// A recognizable "production" backup — marker values that must never leak into
/// a test-mode config.
json production_backup_json() {
    return json{{"config_version", CURRENT_CONFIG_VERSION},
                {"active_printer_id", "real-production-printer"},
                {"brightness", 42},
                {"wizard_completed", true},
                {"printers", {{"real-production-printer", {{"moonraker_host", "10.9.9.9"}}}}}};
}

} // namespace

TEST_CASE("Config::init() in test mode does not restore a missing config from a production backup",
          "[core][config][test-mode][backup-isolation]") {
    BackupSandbox env("testmode_no_restore");
    env.write_primary_backup(production_backup_json());
    REQUIRE(env.primary_backup_exists()); // precondition: a backup IS available

    ConfigTestModeGuard test_mode(true);

    // Config file is absent — exactly the `--test` first-run case.
    REQUIRE_FALSE(std::filesystem::exists(env.config_path()));

    Config test_config;
    test_config.init(env.config_path());

    // Must fall back to normal defaults, NOT the production backup.
    REQUIRE(test_config.get<std::string>("/active_printer_id", "") != "real-production-printer");
    REQUIRE(test_config.get<int>("/brightness", 0) != 42);
    REQUIRE(test_config.get<std::string>("/printers/real-production-printer/moonraker_host", "") ==
            "");
}

TEST_CASE("Config::init() outside test mode still restores a missing config from backup",
          "[core][config][backup-isolation]") {
    // Guards the isolation from over-reaching: production recovery must survive.
    BackupSandbox env("prodmode_restores");
    env.write_primary_backup(production_backup_json());

    ConfigTestModeGuard test_mode(false);

    REQUIRE_FALSE(std::filesystem::exists(env.config_path()));

    Config test_config;
    test_config.init(env.config_path());

    REQUIRE(test_config.get<std::string>("/active_printer_id", "") == "real-production-printer");
    REQUIRE(test_config.get<int>("/brightness", 0) == 42);
}

TEST_CASE("Config::init() in test mode does not adopt a backup over a tarball-default config",
          "[core][config][test-mode][backup-isolation]") {
    // The second /var/lib-consulting path: a loaded config with config_version==0
    // is treated as a tarball default and replaced with backup contents.
    BackupSandbox env("testmode_no_tarball_adopt");
    env.write_primary_backup(production_backup_json());

    // A config_version==0 config — would trip tarball detection outside test mode.
    {
        std::ofstream o(env.config_path());
        o << json{{"preset", "ad5x"},
                  {"wizard_completed", false},
                  {"printers", {{"default", {{"moonraker_host", "127.0.0.1"}}}}}}
                 .dump(2);
    }

    ConfigTestModeGuard test_mode(true);

    Config test_config;
    test_config.init(env.config_path());

    // Test config must stay put — no production data adopted.
    REQUIRE(test_config.get<std::string>("/active_printer_id", "") != "real-production-printer");
    REQUIRE(test_config.get<int>("/brightness", 0) != 42);
}

TEST_CASE("Config::init() in test mode does not write a production rolling backup",
          "[core][config][test-mode][backup-isolation]") {
    // The write-side mirror of the same hazard: test-mode saves must not clobber
    // the real user's rolling backup with test data.
    BackupSandbox env("testmode_no_backup_write");

    // A complete, wizard-completed config so init()'s rolling-backup write is reached.
    {
        std::ofstream o(env.config_path());
        o << json{{"config_version", CURRENT_CONFIG_VERSION},
                  {"active_printer_id", "test-printer"},
                  {"wizard_completed", true},
                  {"brightness", 7},
                  {"printers", {{"test-printer", {{"moonraker_host", "127.0.0.1"}}}}}}
                 .dump(2);
    }

    ConfigTestModeGuard test_mode(true);

    Config test_config;
    test_config.init(env.config_path());
    test_config.set("/brightness", 9);
    test_config.save();

    REQUIRE_FALSE(env.primary_backup_exists());
    REQUIRE_FALSE(std::filesystem::exists(AppConstants::Update::config_backup_fallback()));
}

TEST_CASE("Config::save() outside test mode still writes a rolling backup",
          "[core][config][backup-isolation]") {
    // Guards the write-side isolation from over-reaching.
    BackupSandbox env("prodmode_writes_backup");

    {
        std::ofstream o(env.config_path());
        o << json{{"config_version", CURRENT_CONFIG_VERSION},
                  {"active_printer_id", "test-printer"},
                  {"wizard_completed", true},
                  {"brightness", 7},
                  {"printers", {{"test-printer", {{"moonraker_host", "127.0.0.1"}}}}}}
                 .dump(2);
    }

    ConfigTestModeGuard test_mode(false);

    Config test_config;
    test_config.init(env.config_path());
    test_config.set("/brightness", 9);
    test_config.save();

    REQUIRE(env.primary_backup_exists());
    REQUIRE(env.read_primary_backup().value("brightness", 0) == 9);
}

// ============================================================================
// save() must not refresh the rolling backup while the wizard is incomplete.
//
// The single-slot rolling backup is the only recovery copy of the user's real
// config. A wizard-incomplete config holds preset defaults, not user data, so
// letting save() copy it over the backup destroys the recovery copy. init()
// already carried this guard on the startup backup; save() did not, so the
// first setting touched during setup wiped it.
// ============================================================================

namespace {

/// A config whose only meaningful variable is whether setup ever completed.
/// config_version is CURRENT so tarball detection (which triggers on
/// config_version == 0) cannot adopt the backup over it.
json wizard_state_config_json(bool wizard_completed) {
    return json{
        {"config_version", CURRENT_CONFIG_VERSION},
        {"active_printer_id", "fresh"},
        {"wizard_completed", wizard_completed},
        {"brightness", 7},
        {"printers",
         {{"fresh", {{"moonraker_host", "127.0.0.1"}, {"wizard_completed", wizard_completed}}}}}};
}

/// RAII: neutralize a HELIX_CONFIG_DIR leaked by another test in the shard,
/// which would silently redirect Config::path away from the sandbox.
struct ConfigDirEnvGuard {
    std::string prev;
    bool had;
    ConfigDirEnvGuard() : had(std::getenv("HELIX_CONFIG_DIR") != nullptr) {
        if (had) {
            prev = std::getenv("HELIX_CONFIG_DIR");
            unsetenv("HELIX_CONFIG_DIR");
        }
    }
    ~ConfigDirEnvGuard() {
        if (had)
            setenv("HELIX_CONFIG_DIR", prev.c_str(), 1);
    }
};

} // namespace

TEST_CASE("Config::save() does not overwrite the rolling backup while the wizard is incomplete",
          "[core][config][backup-isolation][wizard]") {
    BackupSandbox env("wizard_incomplete_no_backup_write");
    ConfigDirEnvGuard config_dir_guard;

    // A good recovery copy from a fully configured install already exists.
    env.write_primary_backup(production_backup_json());
    REQUIRE(env.read_primary_backup().value("brightness", 0) == 42);

    {
        std::ofstream o(env.config_path());
        o << wizard_state_config_json(false).dump(2);
    }

    ConfigTestModeGuard test_mode(false);

    Config test_config;
    test_config.init(env.config_path());
    REQUIRE(test_config.is_wizard_required()); // precondition the guard keys off

    test_config.set("/brightness", 9);
    REQUIRE(test_config.save());

    // The backup must still be the user's real config, untouched.
    REQUIRE(env.primary_backup_exists());
    REQUIRE(env.read_primary_backup().value("brightness", 0) == 42);
    REQUIRE(env.read_primary_backup().value("active_printer_id", "") == "real-production-printer");
    // ...and the setup-in-progress config must not have leaked into the fallback tier either.
    REQUIRE_FALSE(std::filesystem::exists(AppConstants::Update::config_backup_fallback()));
}

TEST_CASE("Config::save() does refresh the rolling backup once the wizard is complete",
          "[core][config][backup-isolation][wizard]") {
    // Paired with the test above and differing in exactly one bit, so the guard
    // cannot be "fixed" by disabling rolling backups altogether.
    BackupSandbox env("wizard_complete_backup_write");
    ConfigDirEnvGuard config_dir_guard;

    env.write_primary_backup(production_backup_json());

    {
        std::ofstream o(env.config_path());
        o << wizard_state_config_json(true).dump(2);
    }

    ConfigTestModeGuard test_mode(false);

    Config test_config;
    test_config.init(env.config_path());
    REQUIRE_FALSE(test_config.is_wizard_required());

    test_config.set("/brightness", 9);
    REQUIRE(test_config.save());

    REQUIRE(env.primary_backup_exists());
    REQUIRE(env.read_primary_backup().value("brightness", 0) == 9);
    REQUIRE(env.read_primary_backup().value("active_printer_id", "") == "fresh");
}

TEST_CASE("Config::resolve_path applies HELIX_CONFIG_DIR without losing the filename",
          "[core][config]") {
    ConfigDirEnvGuard config_dir_guard; // start from a known-unset env

    // ConfigDirEnvGuard only restores a value that already existed, so it
    // cannot clean up what this test sets on a shard where the var was unset.
    // Without this the override leaks into every later test in the shard.
    struct UnsetOnExit {
        ~UnsetOnExit() {
            unsetenv("HELIX_CONFIG_DIR");
        }
    } unset_on_exit;

    SECTION("unset env returns the caller's path verbatim") {
        REQUIRE(Config::resolve_path("config/settings.json") == "config/settings.json");
        REQUIRE(Config::resolve_path("config/settings-test.json") == "config/settings-test.json");
    }

    SECTION("empty env is treated as unset") {
        setenv("HELIX_CONFIG_DIR", "", 1);
        REQUIRE(Config::resolve_path("config/settings.json") == "config/settings.json");
    }

    SECTION("override swaps the directory and keeps the filename") {
        setenv("HELIX_CONFIG_DIR", "/tmp/helix-resolve-path", 1);
        // The test/prod distinction rides on the filename, so it must survive.
        REQUIRE(Config::resolve_path("config/settings.json") ==
                "/tmp/helix-resolve-path/settings.json");
        REQUIRE(Config::resolve_path("config/settings-test.json") ==
                "/tmp/helix-resolve-path/settings-test.json");
    }

    SECTION("a trailing slash on the override does not double the separator") {
        setenv("HELIX_CONFIG_DIR", "/tmp/helix-resolve-path/", 1);
        REQUIRE(Config::resolve_path("config/settings.json") ==
                "/tmp/helix-resolve-path/settings.json");
    }

    SECTION("resolve_path agrees with the path init() actually persists to") {
        // The banner and init() must never disagree about where settings live.
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "helix-resolve-path-init";
        std::filesystem::remove_all(dir);
        setenv("HELIX_CONFIG_DIR", dir.string().c_str(), 1);

        Config cfg;
        cfg.init("config/settings-test.json");
        cfg.set("/brightness", 5);
        REQUIRE(cfg.save());

        std::string expected = Config::resolve_path("config/settings-test.json");
        REQUIRE(expected == (dir / "settings-test.json").string());
        REQUIRE(std::filesystem::exists(expected));

        std::filesystem::remove_all(dir);
    }
}

// ============================================================================
// init() must persist migrations through save()'s atomic temp-file + rename
// path, not a bare truncate-in-place ofstream.
//
// Content alone cannot tell the two apart — both end up writing the same JSON.
// What distinguishes them is HOW the bytes land: save() writes a sibling .tmp,
// fsyncs it, and rename()s it over the config, so settings.json is a brand new
// inode and the old one keeps its complete pre-migration contents until the
// instant it is replaced. `std::ofstream o(path)` truncates the live file and
// streams into it, so the same inode passes through a zero-length state — which
// is exactly what a power cut during first-boot migration used to freeze in
// place. The inode/hard-link assertions below are the ones that discriminate;
// the content assertions state what the user actually cares about.
// ============================================================================

TEST_CASE("Config::init() persists migrations by atomic replace, not truncate-in-place",
          "[core][config][migration][atomic-save]") {
    BackupSandbox env("migration_atomic_persist");
    ConfigDirEnvGuard config_dir_guard;
    // Test mode keeps the production backup tiers out of this entirely — the
    // assertion here is about the config file, not about backups.
    ConfigTestModeGuard test_mode(true);

    namespace fs = std::filesystem;
    const fs::path cfg = env.dir / "settings.json";

    // Root-level display_rotate is the pre-/display/ layout: loading it sets
    // config_modified, which is what drives the persist-migrations branch.
    {
        std::ofstream o(cfg);
        o << json{{"config_version", CURRENT_CONFIG_VERSION},
                  {"active_printer_id", "voron"},
                  {"wizard_completed", true},
                  {"display_rotate", 90},
                  {"brightness", 55},
                  {"printers", {{"voron", {{"moonraker_host", "192.168.1.10"}}}}}}
                 .dump(2);
    }

    // A second name for the pre-migration inode. Whatever init() does to the
    // settings.json name, this one keeps pointing at the original file.
    const fs::path witness = env.dir / "witness.json";
    fs::create_hard_link(cfg, witness);

    struct stat before {};
    REQUIRE(::stat(cfg.c_str(), &before) == 0);

    Config test_config;
    test_config.init(cfg.string());

    // In memory the migration ran...
    REQUIRE(test_config.get<int>("/display/rotate", -1) == 90);

    // ...and it reached the disk, whole and parseable.
    json on_disk;
    REQUIRE_NOTHROW(on_disk = json::parse(std::ifstream(cfg)));
    REQUIRE(on_disk.contains("display"));
    REQUIRE(on_disk["display"].value("rotate", -1) == 90);
    REQUIRE_FALSE(on_disk.contains("display_rotate"));
    // Unrelated settings survived the rewrite — a partial write would lose these.
    REQUIRE(on_disk.value("brightness", 0) == 55);
    REQUIRE(on_disk.value("config_version", 0) == CURRENT_CONFIG_VERSION);
    REQUIRE(on_disk["printers"]["voron"].value("moonraker_host", "") == "192.168.1.10");

    // settings.json is a DIFFERENT file than the one we started with: rename()
    // moved a fully-written temp file into place. A truncate-in-place write
    // would have kept the same inode.
    struct stat after {};
    REQUIRE(::stat(cfg.c_str(), &after) == 0);
    REQUIRE(before.st_ino != after.st_ino);

    // The original inode was never edited — it still holds the complete,
    // un-migrated config. That is the property that makes a power cut here
    // survivable: at no point is there a half-written settings.json.
    json witness_json;
    REQUIRE_NOTHROW(witness_json = json::parse(std::ifstream(witness)));
    REQUIRE(witness_json.value("display_rotate", -1) == 90);
    REQUIRE(witness_json.value("brightness", 0) == 55);

    // The atomic path must not leave its scratch file behind.
    REQUIRE_FALSE(fs::exists(cfg.string() + ".tmp"));
}

TEST_CASE("Config::init() loads a read-only settings.json instead of condemning it as corrupt",
          "[core][config][read-only][data-loss]") {
    if (::geteuid() == 0) {
        SKIP("running as root: mode 0444 does not block open(O_WRONLY)");
    }

    BackupSandbox env("read_only_config_load");
    ConfigDirEnvGuard config_dir_guard;
    ConfigTestModeGuard test_mode(true);

    namespace fs = std::filesystem;
    const fs::path cfg = env.dir / "settings.json";

    // A complete, already-migrated config: nothing here should trigger a rewrite,
    // so the only thing under test is whether init() can READ an unwritable file.
    {
        std::ofstream o(cfg);
        o << json{{"config_version", CURRENT_CONFIG_VERSION},
                  {"active_printer_id", "voron"},
                  {"wizard_completed", true},
                  {"brightness", 55},
                  {"printers", {{"voron", {{"moonraker_host", "192.168.1.10"}}}}}}
                 .dump(2);
    }
    fs::permissions(cfg, fs::perms::owner_read);

    Config test_config;
    test_config.init(cfg.string());

    // The user's settings are intact. Reading with std::fstream (default openmode
    // in|out) fails to open a 0444 file, yields an empty parse, and sends init()
    // down the corrupt-file path — which renames settings.json to .corrupt and
    // resets to factory defaults. That silently destroys the config of any user
    // whose settings.json ends up root-owned or read-only.
    REQUIRE(test_config.get<int>("/brightness", -1) == 55);
    REQUIRE(test_config.get<std::string>("/printers/voron/moonraker_host", "") == "192.168.1.10");
    REQUIRE_FALSE(test_config.is_wizard_required());

    // ...and it was not condemned.
    fs::permissions(cfg, fs::perms::owner_all);
    REQUIRE_FALSE(fs::exists(cfg.string() + ".corrupt"));
    REQUIRE(fs::exists(cfg));
}
