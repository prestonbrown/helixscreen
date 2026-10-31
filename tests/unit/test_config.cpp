// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_test_access.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "app_constants.h"
#include "config.h"
#include "config_testing.h"
#include "runtime_config.h"
#include "static_subject_registry.h"
#include "test_helpers/unique_temp_dir.h"
#include "text_io.h"
#include "wizard_config_paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

    // Run the real display migration Config::init() runs (migrate_display_config()
    // followed by the /display/ -> /input/ key moves), through the config_testing
    // seam, so these cases go red when production changes.
    void apply_migration() {
        helix::config_testing::run_display_migrations_for_test(ConfigTestAccess::data(config));
    }

    // Second migration step alone (/display/calibration + /display/touch_device ->
    // /input/). Production has no standalone entry point for it — init() always
    // runs it behind the same call as the display_* migration, and that migration
    // is a no-op on a config with no root-level display_* keys, so calling the
    // shared seam here is exactly what a real startup would do.
    void migrate_to_input() {
        helix::config_testing::run_display_migrations_for_test(ConfigTestAccess::data(config));
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
// get(): reads existing values, throws on missing or mistyped keys
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() returns existing values at any depth",
                 "[core][config][get]") {
    setup_default_config();

    struct Row {
        const char* path;
        const char* expected;
    };
    const Row rows[] = {
        {"moonraker_host", "192.168.1.100"},
        {"hardware_map/heated_bed", "heater_bed"},
    };
    for (const Row& r : rows) {
        INFO("path: " << r.path);
        CHECK(config.get<std::string>(config.df() + r.path) == r.expected);
    }
    CHECK(config.get<int>(config.df() + "moonraker_port") == 7125);

    auto hardware_map = config.get<json>(config.df() + "hardware_map");
    REQUIRE(hardware_map.is_object());
    CHECK(hardware_map["heated_bed"] == "heater_bed");
    CHECK(hardware_map["hotend"] == "extruder");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with missing key throws exception",
                 "[core][config][get]") {
    setup_default_config();

    // out_of_range, not type_error: get() uses at() so a missing key never
    // vivifies a null on the way to failing (#1129).
    for (const char* key : {"nonexistent_key", "hardware_map/missing"}) {
        INFO("key: " << key);
        REQUIRE_THROWS_AS(config.get<std::string>(config.df() + key),
                          nlohmann::detail::out_of_range);
        REQUIRE_FALSE(config.exists(config.df() + key));
    }
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with type mismatch throws exception",
                 "[config][get]") {
    setup_default_config();

    REQUIRE_THROWS(config.get<int>(config.df() + "moonraker_host"));
}

// ============================================================================
// get() with a default: the stored value wins, the default fills any gap
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: get() with default returns value or default",
                 "[core][config][get][default]") {
    struct Row {
        const char* path; // absolute
        const char* fallback;
        const char* expected;
    };

    SECTION("string keys on a full config") {
        setup_default_config();
        const std::string df = config.df();
        const Row rows[] = {
            {"moonraker_host", "default.local", "192.168.1.100"},
            {"printer_name", "My Printer", "My Printer"},
            {"hardware_map/main_led", "none", "none"},
            {"empty_field", "", ""},
        };
        for (const Row& r : rows) {
            INFO("path: " << r.path);
            CHECK(config.get<std::string>(df + r.path, r.fallback) == r.expected);
        }
        CHECK(config.get<std::string>("/nonexistent/path/key", "fallback") == "fallback");
        CHECK(config.get<int>(df + "moonraker_port", 9999) == 7125);
        CHECK(config.get<int>(df + "timeout", 30) == 30);
        CHECK(config.get<bool>(df + "moonraker_api_key", false) == false);
    }

    SECTION("a missing key on a minimal config returns the default, not a throw") {
        setup_minimal_config();
        CHECK(config.get<std::string>(config.df() + "printer_name", "") == "");
    }

    SECTION("an empty config returns the default") {
        set_data_empty();
        CHECK(config.get<std::string>(config.df() + "moonraker_host", "localhost") == "localhost");
    }
}

// ============================================================================
// set() operations
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: set() creates and updates keys at any depth",
                 "[config][set]") {
    setup_default_config();
    const std::string df = config.df();

    struct Row {
        std::string path;
        const char* value;
    };
    const Row rows[] = {
        {"/new_key", "new_value"},                    // new top-level key
        {df + "moonraker_host", "10.0.0.1"},          // existing key
        {df + "hardware_map/main_led", "neopixel"},   // new nested key
        {df + "hardware_map/hotend", "extruder1"},    // existing nested key
        {df + "nested/level1/level2/level3", "deep"}, // several missing parents
    };
    for (const Row& r : rows) {
        INFO("path: " << r.path);
        config.set<std::string>(r.path, r.value);
        CHECK(config.get<std::string>(r.path) == r.value);
    }
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: set() handles different types", "[config][set]") {
    setup_default_config();

    config.set<int>(config.df() + "new_int", 42);
    config.set<bool>(config.df() + "new_bool", true);
    REQUIRE(config.get<int>(config.df() + "new_int") == 42);
    REQUIRE(config.get<bool>(config.df() + "new_bool") == true);

    // A key may change type: int overwritten with a string
    config.set<int>(config.df() + "moonraker_port", 8080);
    REQUIRE(config.get<int>(config.df() + "moonraker_port") == 8080);
    config.set<std::string>(config.df() + "moonraker_port", "9090");
    REQUIRE(config.get<std::string>(config.df() + "moonraker_port") == "9090");
}

// ============================================================================
// is_wizard_required(): only a boolean true wizard_completed skips the wizard
// ============================================================================

TEST_CASE_METHOD(ConfigTestFixture, "Config: is_wizard_required() reads wizard_completed",
                 "[config][wizard]") {
    enum class Flag { True, False, Missing, StringTrue, Null };
    struct Row {
        const char* name;
        bool full_config; // default config with hardware, else minimal 127.0.0.1
        Flag flag;
        bool expected;
    };
    const Row rows[] = {
        {"true on a minimal config skips", false, Flag::True, false},
        {"false on a full hardware config still runs", true, Flag::False, true},
        {"missing runs", false, Flag::Missing, true},
        {"string \"true\" is not a boolean, runs", true, Flag::StringTrue, true},
        {"null runs", true, Flag::Null, true},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            if (r.full_config) {
                setup_default_config();
            } else {
                setup_minimal_config();
            }
            switch (r.flag) {
            case Flag::True:
                config.set<bool>("/wizard_completed", true);
                break;
            case Flag::False:
                config.set<bool>("/wizard_completed", false);
                break;
            case Flag::StringTrue:
                config.set<std::string>("/wizard_completed", "true");
                break;
            case Flag::Null:
                set_data_null("/wizard_completed");
                break;
            case Flag::Missing:
                break;
            }
            CHECK(config.is_wizard_required() == r.expected);
        }
    }
}

// ============================================================================
// Wizard Config Path Constants Tests - Verify suffix format (v3 multi-printer)
// Constants are now suffixes — callers prepend config->df() for full path.
// ============================================================================

// Every per-printer constant is a SUFFIX that callers append to Config::df(),
// which already ends in '/'. A leading slash would build
// "/printers/<id>//heaters/bed" — a different node that reads and writes fine
// and silently diverges from every other consumer. Nothing asserted that; the
// seven cases this replaces copied each literal out of wizard_config_paths.h and
// asserted it equalled itself, which no single-site edit can fail.
TEST_CASE("WizardConfigPaths: per-printer constants are df()-relative suffixes",
          "[config][paths][wizard][plural]") {
    const std::pair<const char*, const char*> suffixes[] = {
        {"PRINTER_NAME", helix::wizard::PRINTER_NAME},
        {"PRINTER_TYPE", helix::wizard::PRINTER_TYPE},
        {"TYPE_MISMATCH_SHOWN_FOR", helix::wizard::TYPE_MISMATCH_SHOWN_FOR},
        {"BED_HEATER", helix::wizard::BED_HEATER},
        {"BED_SENSOR", helix::wizard::BED_SENSOR},
        {"HOTEND_HEATER", helix::wizard::HOTEND_HEATER},
        {"HOTEND_SENSOR", helix::wizard::HOTEND_SENSOR},
        {"HOTEND_FAN", helix::wizard::HOTEND_FAN},
        {"PART_FAN", helix::wizard::PART_FAN},
        {"CHAMBER_FAN", helix::wizard::CHAMBER_FAN},
        {"EXHAUST_FAN", helix::wizard::EXHAUST_FAN},
        {"CHAMBER_SENSOR", helix::wizard::CHAMBER_SENSOR},
        {"CHAMBER_HEATER", helix::wizard::CHAMBER_HEATER},
        {"FEEDER_OPEN_MACRO", helix::wizard::FEEDER_OPEN_MACRO},
        {"FEEDER_CLOSE_MACRO", helix::wizard::FEEDER_CLOSE_MACRO},
        {"LED_STRIP", helix::wizard::LED_STRIP},
        {"LED_SELECTED", helix::wizard::LED_SELECTED},
        {"MOONRAKER_HOST", helix::wizard::MOONRAKER_HOST},
        {"MOONRAKER_PORT", helix::wizard::MOONRAKER_PORT},
        {"PRINTER_IMAGE", helix::PRINTER_IMAGE},
    };

    for (const auto& [name, value] : suffixes) {
        INFO("constant: " << name << " = " << (value ? value : "(null)"));
        REQUIRE(value != nullptr);
        const std::string path(value);
        REQUIRE_FALSE(path.empty());
        REQUIRE(path.front() != '/');
        REQUIRE(path.back() != '/');
    }
}

// The device-level constants are the deliberate exception: absolute paths that
// callers must NOT prefix with df(). Losing the leading slash would make them
// invalid JSON pointers.
TEST_CASE("WizardConfigPaths: device-level constants are absolute paths",
          "[config][paths][wizard][plural]") {
    const std::pair<const char*, const char*> absolute[] = {
        {"WIFI_SSID", helix::wizard::WIFI_SSID},
        {"WIFI_PASSWORD", helix::wizard::WIFI_PASSWORD},
    };

    for (const auto& [name, value] : absolute) {
        INFO("constant: " << name << " = " << (value ? value : "(null)"));
        REQUIRE(value != nullptr);
        const std::string path(value);
        REQUIRE_FALSE(path.empty());
        REQUIRE(path.front() == '/');
    }
}

// ============================================================================
// Display Config Migration Tests - Phase 1 of display config refactoring
// ============================================================================

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
// Default values: read through the two public paths that build them, each read
// with a fallback no default equals (kAbsentSentinel) so an absent or wrong key
// fails instead of echoing. House style: tests/unit/test_config_migrations.cpp.
// ----------------------------------------------------------------------------

namespace {

/// Fallback for the default-value reads below. No production default equals it,
/// so getting it back means the key was missing rather than matching.
const json kAbsentSentinel = "<<key absent>>";

struct ConfigDefault {
    const char* pointer;
    json expected;
};

/// The defaults get_default_config() builds, keyed by JSON pointer.
/// /display/* comes from get_default_display_config() (src/system/config.cpp:90);
/// /input/* from the literal in get_default_config() (src/system/config.cpp:1364)
/// and the ensure-block that mirrors it (src/system/config.cpp:1886).
/// Deliberately absent: /display/rotate — see the rotate case below.
const std::vector<ConfigDefault>& expected_defaults() {
    static const std::vector<ConfigDefault> table = {
        {"/display/sleep_sec", 1200},
        {"/display/dim_sec", 600},
        {"/display/dim_brightness", 30},
        {"/display/drm_device", ""},
        {"/display/gcode_render_mode", 0},
        {"/display/bed_mesh_render_mode", 0},
        {"/display/gpu_3d_blocked", false},
        {"/display/gpu_blur_blocked", false},
        {"/input/touch_device", ""},
        {"/input/scroll_throw", 25},
        {"/input/scroll_limit", 10},
        {"/input/long_press_time", 500},
        {"/input/calibration/valid", false},
        // Identity matrix: a=1, b=0, c=0, d=0, e=1, f=0
        {"/input/calibration/a", 1.0},
        {"/input/calibration/b", 0.0},
        {"/input/calibration/c", 0.0},
        {"/input/calibration/d", 0.0},
        {"/input/calibration/e", 1.0},
        {"/input/calibration/f", 0.0},
    };
    return table;
}

/// Builds a config the way first boot does: an empty sandbox directory with no
/// settings.json, so Config::init() takes the "Create default config" branch
/// (src/system/config.cpp:1766) and then runs the ensure-defaults blocks
/// (src/system/config.cpp:1872, :1886). HELIX_CONFIG_DIR keeps the
/// backup-restore search paths inside the temp dir so the host config cannot
/// leak in and supply the values under test.
class FreshConfigFixture {
  protected:
    Config config;

    FreshConfigFixture() {
        temp_dir_ = helix::test::unique_temp_dir("helix_fresh_config_test");
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir_.c_str(), 1);

        config.init((std::filesystem::path(temp_dir_) / "settings.json").string());
    }

    ~FreshConfigFixture() {
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

  private:
    std::string temp_dir_;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;
};

} // namespace

TEST_CASE_METHOD(ConfigTestFixture, "Config: reset_to_defaults installs the shipped defaults",
                 "[config][display][defaults]") {
    // reset_to_defaults() is the nearest public seam onto get_default_config(),
    // which embeds get_default_display_config() verbatim. No filesystem involved.
    config.reset_to_defaults();

    for (const auto& d : expected_defaults()) {
        INFO("pointer: " << d.pointer << " expected: " << d.expected.dump());
        REQUIRE(config.get<json>(d.pointer, kAbsentSentinel) == d.expected);
    }
}

TEST_CASE_METHOD(FreshConfigFixture, "Config: a first-boot config carries the shipped defaults",
                 "[config][display][defaults]") {
    // Same table, reached through Config::init() on an empty directory, so this
    // also covers the ensure-display-defaults and ensure-input-defaults blocks
    // that run after the default document is built.
    for (const auto& d : expected_defaults()) {
        INFO("pointer: " << d.pointer << " expected: " << d.expected.dump());
        REQUIRE(config.get<json>(d.pointer, kAbsentSentinel) == d.expected);
    }
}

TEST_CASE_METHOD(FreshConfigFixture, "Config: a first-boot config has no display/rotate",
                 "[config][display][defaults]") {
    // Absence is load-bearing, not an oversight: Application uses
    // exists("/display/rotate") (src/application/application.cpp:1658) to tell
    // "user never chose a rotation" from "user chose 0", and adopts the kernel
    // orientation only in the first case. get_default_display_config() therefore
    // must not seed the key. Readers supply their own 0 fallback
    // (src/application/display_manager.cpp:336).
    REQUIRE_FALSE(config.exists("/display/rotate"));
    REQUIRE(config.get<int>("/display/rotate", -1) == -1);
}

// ----------------------------------------------------------------------------
// Edge Cases
// ----------------------------------------------------------------------------

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
    std::string temp_dir = helix::test::unique_temp_dir("helix_test");
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

TEST_CASE_METHOD(ConfigTestFixture, "Config: language defaults to en and round-trips via /language",
                 "[config][language]") {
    set_data_empty();
    REQUIRE(config.get_language() == "en");

    // Written into an empty (null) document, then read back from a stored value
    config.set_language("fr");
    REQUIRE(get_data()["language"] == "fr");

    get_data()["language"] = "de";
    REQUIRE(config.get_language() == "de");
}

// ============================================================================
// Config Versioning & Migration Tests
// ============================================================================

/// RAII guard: redirect BOTH backup tiers (the $HOME fallback and the
/// /var/lib/helixscreen StateDirectory primary) to a temp dir so neither
/// tarball-detection nor restore finds a real backup, even on a machine with
/// HelixScreen installed. Each instance gets a unique dir to avoid parallel-shard races.
struct BackupGuard {
    std::string temp_dir;
    std::string original_home;
    std::string prev_state_dir;
    std::string prev_fallback_dir;
    bool had_home;

    BackupGuard()
        : temp_dir(helix::test::unique_temp_dir("helix_nobackup")),
          prev_state_dir(app_constants::update::detail::state_dir_ref()),
          prev_fallback_dir(app_constants::update::detail::backup_fallback_dir_ref()),
          had_home(std::getenv("HOME") != nullptr) {
        if (had_home)
            original_home = std::getenv("HOME");
        std::filesystem::create_directories(temp_dir);
        setenv("HOME", temp_dir.c_str(), 1);
        app_constants::update::detail::backup_fallback_dir_ref() =
            app_constants::update::sanitize_home(std::getenv("HOME")) + "/.helixscreen";
        app_constants::update::detail::state_dir_ref() = temp_dir + "/var-lib";
    }
    ~BackupGuard() {
        if (had_home)
            setenv("HOME", original_home.c_str(), 1);
        else
            unsetenv("HOME");
        // Restore the SAVED value, never a freshly-computed $HOME/.helixscreen:
        // the test sandbox points this at a temp dir, and recomputing it here
        // un-sandboxes every later test in the shard onto the real backup.
        app_constants::update::detail::backup_fallback_dir_ref() = prev_fallback_dir;
        app_constants::update::detail::state_dir_ref() = prev_state_dir;
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }
};

/// A settings document written into its own temp directory, removed on scope exit.
struct SettingsFile {
    std::string dir;
    std::string path;

    SettingsFile(const char* tag, const json& doc)
        : dir(helix::test::unique_temp_dir(tag)), path(dir + "/test_config.json") {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::ofstream(path) << doc.dump(2);
    }
    ~SettingsFile() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

TEST_CASE_METHOD(
    ConfigTestFixture,
    "Config: a config below the migration floor is kept aside and replaced by defaults",
    "[core][config][migration][versioning]") {
    json v5_config = {{"config_version", 5},
                      {"brightness", 50},
                      {"active_printer_id", "ender3"},
                      {"printers", {{"ender3", {{"moonraker_host", "192.168.1.100"}}}}}};
    SettingsFile file("helix_floor_test", v5_config);

    BackupGuard guard;
    helix::LogCapture log;
    Config test_config;
    test_config.init(file.path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE(test_config.get_active_printer_id() == "default");
    REQUIRE(test_config.get<std::string>(test_config.df() + "moonraker_host") == "192.168.1.100");
    REQUIRE_FALSE(test_config.exists("/printers/ender3"));
    REQUIRE(log.count_containing("config_version 5") == 1);

    auto kept = json::parse(std::ifstream(file.path + ".pre-migration"));
    REQUIRE(kept == v5_config);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: a config at the migration floor is migrated",
                 "[core][config][migration][versioning]") {
    json v9_config = {{"config_version", kMinMigratableConfigVersion},
                      {"active_printer_id", "ender3"},
                      {"printers", {{"ender3", {{"moonraker_host", "192.168.1.100"}}}}}};
    SettingsFile file("helix_floor_v9_test", v9_config);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE(test_config.get_active_printer_id() == "ender3");
    REQUIRE(test_config.get<std::string>(test_config.df() + "moonraker_host") == "192.168.1.100");
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: fresh config gets version stamp and sounds default to false",
                 "[core][config][migration][versioning]") {
    // BackupGuard (below) redirects both backup tiers into a temp dir, so a real
    // /var/lib/helixscreen backup on the host cannot be restored over the fresh
    // config this test is asserting about.

    // Brand new config — no file exists
    std::string temp_dir = helix::test::unique_temp_dir("helix_fresh_test");
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/fresh_config.json";

    // Ensure file doesn't exist
    std::filesystem::remove(temp_path);
    REQUIRE_FALSE(std::filesystem::exists(temp_path));

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    // Fresh config should have current version (skips all migrations)
    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);

    // Fresh config should NOT have sounds_enabled set (it's a user pref, not in base defaults)
    // But if accessed with default, should be false
    REQUIRE(test_config.get<bool>("/sounds_enabled", false) == false);

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: v0 config without sounds_enabled key just gets version stamp",
                 "[config][migration][versioning]") {
    // Edge case: old config that somehow never had sounds_enabled
    json minimal_v0 = {
        {"brightness", 50},
        {"printer", {{"moonraker_host", "192.168.1.100"}, {"moonraker_port", 7125}}}};

    SettingsFile file("helix_nosound_test", minimal_v0);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    // Should get version stamp without errors
    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
    // sounds_enabled should not have been created by migration
    REQUIRE(test_config.get<bool>("/sounds_enabled", false) == false);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: factory reset removes rolling backups at every tier",
                 "[config][reset][1714]") {
    const std::string old_host = "192.168.168.7";
    std::string temp_dir = helix::test::unique_temp_dir("helix_reset_backups");
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/settings.json";

    // Redirects both backup tiers (and HOME) into guard.temp_dir
    BackupGuard guard;

    json old_config = {
        {"config_version", kCurrentConfigVersion},
        {"wizard_completed", true},
        {"printers", {{"default", {{"moonraker_host", old_host}, {"moonraker_port", 7125}}}}}};
    {
        std::ofstream o(temp_path);
        o << old_config.dump(2);
    }

    const std::vector<std::string> config_tiers = {
        app_constants::update::config_backup_primary(),
        app_constants::update::config_backup_fallback(),
        app_constants::update::legacy_config_backup_primary(),
        app_constants::update::legacy_config_backup_fallback(),
    };
    const std::vector<std::string> env_tiers = {
        app_constants::update::env_backup_primary(),
        app_constants::update::env_backup_fallback(),
    };

    // Seed every tier the restore path reads, so the assertions below prove
    // removal rather than absence.
    for (const auto& tier : config_tiers) {
        std::filesystem::create_directories(std::filesystem::path(tier).parent_path());
        std::ofstream o(tier);
        o << old_config.dump(2);
    }
    for (const auto& tier : env_tiers) {
        std::filesystem::create_directories(std::filesystem::path(tier).parent_path());
        std::ofstream o(tier);
        o << "MOONRAKER_HOST=" << old_host << "\n";
    }
    for (const auto& tier : config_tiers)
        REQUIRE(std::filesystem::exists(tier));
    for (const auto& tier : env_tiers)
        REQUIRE(std::filesystem::exists(tier));

    Config config;
    config.init(temp_path);
    REQUIRE(config.get<std::string>(config.df() + "moonraker_host") == old_host);

    // The factory-reset path: reset, persist, then the settings file is gone
    config.reset_to_defaults();
    config.save();

    for (const auto& tier : config_tiers)
        REQUIRE_FALSE(std::filesystem::exists(tier));
    for (const auto& tier : env_tiers)
        REQUIRE_FALSE(std::filesystem::exists(tier));

    // With settings.json missing and no backups left, init() must land on
    // fresh defaults, not the pre-reset config.
    std::filesystem::remove(temp_path);
    Config after;
    after.init(temp_path);
    REQUIRE(after.is_wizard_required());
    REQUIRE(after.get<std::string>(after.df() + "moonraker_host", "") != old_host);
    for (const auto& tier : config_tiers)
        REQUIRE_FALSE(std::filesystem::exists(tier));

    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// Versionless (version 0) documents: the single /printer shape
// ============================================================================

TEST_CASE("Config: every shipped preset loads as a fresh install in the /printers shape",
          "[core][config][migration][preset]") {
    namespace fs = std::filesystem;
    std::string src = __FILE__;
    // __FILE__ is repo-relative or absolute; either way the prefix is the root.
    const std::string presets_dir =
        src.substr(0, src.rfind("tests/unit/")) + "assets/config/presets";

    int checked = 0;
    for (const auto& entry : fs::directory_iterator(presets_dir)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        const std::string name = entry.path().filename().string();
        INFO("preset " << name);
        const json preset = json::parse(std::ifstream(entry.path()));
        REQUIRE_FALSE(preset.contains("config_version"));
        const json& legacy_printer = preset.at("printer");

        std::string temp_dir = helix::test::unique_temp_dir("helix_preset_load");
        fs::create_directories(temp_dir);
        std::string temp_path = temp_dir + "/settings.json";
        fs::copy_file(entry.path(), temp_path);

        BackupGuard guard;
        Config test_config;
        test_config.init(temp_path);
        const json doc = test_config.get<json>("", json());

        REQUIRE(doc.at("config_version") == kCurrentConfigVersion);
        REQUIRE_FALSE(doc.contains("printer"));

        std::string slug = "default";
        for (const char* key : {"printer_name", "name"}) {
            if (legacy_printer.contains(key)) {
                slug = Config::slugify(legacy_printer.at(key).get<std::string>());
                break;
            }
        }
        REQUIRE(test_config.get_active_printer_id() == slug);
        const json& printer = doc.at("printers").at(slug);

        if (legacy_printer.contains("heaters")) {
            CHECK(printer.at("heaters") == legacy_printer.at("heaters"));
        }
        if (preset.contains("preset")) {
            CHECK(printer.at("preset") == preset.at("preset"));
        }

        const json legacy_leds = legacy_printer.value("leds", json::object());
        const std::string strip = legacy_leds.value("strip", "");
        CHECK(printer.at("leds").at("selected") ==
              (strip.empty() ? json::array() : json::array({strip})));
        if (legacy_leds.contains("selected_strips")) {
            CHECK(printer.at("leds").at("selected_strips") == legacy_leds.at("selected_strips"));
        }

        json rotate;
        if (preset.contains("display") && preset.at("display").contains("rotate")) {
            rotate = preset.at("display").at("rotate");
        } else if (preset.contains("display_rotate")) {
            rotate = preset.at("display_rotate");
        }
        if (!rotate.is_null()) {
            CHECK(doc.at("display").at("rotate") == rotate);
        }
        CHECK_FALSE(doc.contains("display_rotate"));
        CHECK(doc.at("printers").at("show_printer_switcher") == false);

        CHECK(test_config.is_wizard_required() != preset.value("wizard_completed", false));

        fs::remove_all(temp_dir);
        ++checked;
    }
    REQUIRE(checked >= 20);
}

TEST_CASE("Config: version-0 document selects its legacy LED strip",
          "[core][config][migration][preset]") {
    std::string temp_dir = helix::test::unique_temp_dir("helix_v0_led_strip");
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/settings.json";
    {
        std::ofstream o(temp_path);
        o << json{{"printer", {{"leds", {{"strip", "neopixel case"}}}}}}.dump(2);
    }

    BackupGuard guard;
    Config test_config;
    test_config.init(temp_path);

    REQUIRE(test_config.get<json>(test_config.df() + "leds/selected") ==
            json::array({"neopixel case"}));
    REQUIRE(test_config.get<std::string>(test_config.df() + "leds/strip") == "neopixel case");

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Config: version-0 migration restructures single printer to multi-printer",
          "[core][config][migration][v4]") {
    // A version-0 document in the single /printer shape the shipped presets use
    json v3_config = {
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

    SettingsFile file("helix_test_v3_to_v4", v3_config);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);

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

    // Panel widgets should have moved under printer entry. This drives the whole
    // migration chain, so the v22 step lifts the legacy flat array
    // into the multi-page shape — the widget set and its order are what the versionless step
    // is responsible for carrying across, not the container.
    auto pw = test_config.get<json>(test_config.df() + "panel_widgets/home", json());
    REQUIRE(pw.is_object());
    REQUIRE(pw.contains("pages"));
    REQUIRE(pw["pages"].size() == 1);
    auto pw_widgets = pw["pages"][0]["widgets"];
    REQUIRE(pw_widgets.is_array());
    REQUIRE(pw_widgets.size() == 1);
    REQUIRE(pw_widgets[0]["id"] == "temp");

    // wizard_completed should be copied to printer entry
    REQUIRE(test_config.get<bool>(test_config.df() + "wizard_completed") == true);

    // Root-level wizard_completed should still exist (backward compat)
    REQUIRE(test_config.get<bool>("/wizard_completed") == true);
}

TEST_CASE("Config: version-0 migration picks the active printer id",
          "[core][config][migration][v4]") {
    struct Row {
        const char* name;
        json doc;
        const char* active_id;
        const char* host;
    };
    const Row rows[] = {
        {"a nameless /printer becomes 'default'",
         {{"printer", {{"moonraker_host", "10.0.0.5"}, {"moonraker_port", 7125}}}},
         "default",
         "10.0.0.5"},
        {"a document already in the /printers shape keeps its id",
         {{"active_printer_id", "ender3"},
          {"printers",
           {{"ender3",
             {{"moonraker_host", "192.168.1.50"},
              {"moonraker_port", 7125},
              {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}}}}}}},
         "ender3",
         "192.168.1.50"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            SettingsFile file("helix_test_v3_to_v4_id", r.doc);
            BackupGuard guard;
            Config test_config;
            test_config.init(file.path);

            CHECK(test_config.get_active_printer_id() == r.active_id);
            CHECK(test_config.get<std::string>(test_config.df() + "moonraker_host") == r.host);
        }
    }
}

TEST_CASE("Config: version-0 migration moves printer_image to per-printer path",
          "[core][config][migration][v4]") {
    // A version-0 document in the single /printer shape, with /display/printer_image
    json v3_config = {
        {"printer",
         {{"name", "My Printer"}, {"moonraker_host", "192.168.1.100"}, {"moonraker_port", 7125}}},
        {"display", {{"printer_image", "shipped:voron-v2"}, {"rotate", 0}}}};

    SettingsFile file("helix_test_v3_to_v4_printer_image", v3_config);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);

    // printer_image should be at per-printer path, not under /display
    REQUIRE(test_config.get<std::string>(test_config.df() + PRINTER_IMAGE) == "shipped:voron-v2");

    // /display/printer_image should no longer exist
    auto display = test_config.get<json>("/display", json::object());
    REQUIRE_FALSE(display.contains("printer_image"));

    // /display should still have other keys intact
    REQUIRE(test_config.get<int>("/display/rotate") == 0);
}

TEST_CASE("Config: version-0 migration sets the printer switcher only for one printer",
          "[core][config][migration][v5]") {
    enum class Switcher { Off, On, Absent };
    struct Row {
        const char* name;
        json printers;
        Switcher expected;
    };
    const Row rows[] = {
        {"a single printer hides the switcher",
         {{"ender3", {{"moonraker_host", "192.168.1.50"}, {"moonraker_port", 7125}}}},
         Switcher::Off},
        {"several printers leave the key unwritten",
         {{"ender3", {{"moonraker_host", "192.168.1.50"}}},
          {"voron", {{"moonraker_host", "192.168.1.51"}}}},
         Switcher::Absent},
        {"an explicit true is kept",
         {{"show_printer_switcher", true}, {"ender3", {{"moonraker_host", "192.168.1.50"}}}},
         Switcher::On},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            SettingsFile file("helix_test_v4_to_v5",
                              {{"active_printer_id", "ender3"}, {"printers", r.printers}});
            BackupGuard guard;
            Config test_config;
            test_config.init(file.path);

            REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
            switch (r.expected) {
            case Switcher::Off:
                CHECK(test_config.get<bool>("/printers/show_printer_switcher") == false);
                break;
            case Switcher::On:
                CHECK(test_config.get<bool>("/printers/show_printer_switcher") == true);
                break;
            case Switcher::Absent:
                CHECK_FALSE(test_config.exists("/printers/show_printer_switcher"));
                break;
            }
        }
    }
}

// ============================================================================
// v13 → v14: fold legacy telemetry_config.json into settings.json
// ============================================================================

TEST_CASE("Config: v13→v14 folds legacy telemetry_config.json into settings",
          "[core][config][migration][v14]") {
    struct Row {
        const char* name;
        json settings_value; // null = key absent from settings.json
        const char* legacy;
        bool expected;
    };
    const Row rows[] = {
        {"enabled legacy state is imported", nullptr, R"({"enabled": true})", true},
        {"disabled legacy state is imported", nullptr, R"({"enabled": false})", false},
        // settings.json is authoritative: the legacy file never overrides it
        {"an existing /telemetry_enabled wins", true, R"({"enabled": false})", true},
        {"no legacy file leaves the key unwritten", nullptr, nullptr, false},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            json v13 = {{"config_version", 13},
                        {"active_printer_id", "default"},
                        {"printers", {{"default", {{"moonraker_host", "127.0.0.1"}}}}}};
            if (!r.settings_value.is_null()) {
                v13["telemetry_enabled"] = r.settings_value;
            }
            SettingsFile file("helix_test_v13_to_v14", v13);
            const std::string legacy_path = file.dir + "/telemetry_config.json";
            if (r.legacy) {
                std::ofstream(legacy_path) << r.legacy;
            }

            Config test_config;
            test_config.init(file.path);

            REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
            if (!r.legacy) {
                CHECK_FALSE(test_config.exists("/telemetry_enabled"));
                continue;
            }
            REQUIRE(test_config.exists("/telemetry_enabled"));
            CHECK(test_config.get<bool>("/telemetry_enabled") == r.expected);
            // The legacy file is removed either way so it can never re-poison a startup.
            CHECK_FALSE(std::filesystem::exists(legacy_path));
        }
    }
}

// v22→v23: the Macro Button widget's per-instance run gate flipped polarity and
// name (skip_param_prompt -> require_confirmation). The rewrite must be lossless
// in both directions, must not invent a value for widgets that never carried the
// key, and must clear the legacy key so it cannot be read back later.
TEST_CASE("Config: v22→v23 rewrites macro widget skip_param_prompt",
          "[core][config][migration][v23]") {
    json v22_config = {{"config_version", 22},
                       {"active_printer_id", "default"},
                       {"printers",
                        {{"default",
                          {{"panel_widgets",
                            {{"home",
                              {{"pages",
                                {{{"id", "main"},
                                  {"widgets",
                                   {// legacy true -> confirmation off
                                    {{"id", "favorite_macro:1"},
                                     {"enabled", true},
                                     {"config", {{"macro", "PURGE"}, {"skip_param_prompt", true}}}},
                                    // legacy false -> confirmation on
                                    {{"id", "favorite_macro:2"},
                                     {"enabled", true},
                                     {"config", {{"macro", "G28"}, {"skip_param_prompt", false}}}},
                                    // never carried the key -> left alone
                                    {{"id", "favorite_macro:3"},
                                     {"enabled", true},
                                     {"config", {{"macro", "LOAD_FILAMENT"}}}},
                                    // an unrelated widget's config must not be touched
                                    {{"id", "camera"},
                                     {"enabled", true},
                                     {"config", {{"url", "rtsp://x"}}}}}}}}}}}}}}}}}};
    SettingsFile file("helix_test_v22_to_v23", v22_config);

    Config test_config;
    test_config.init(file.path);

    const std::string base = "/printers/default/panel_widgets/home/pages/0/widgets/";

    REQUIRE(test_config.get<bool>(base + "0/config/require_confirmation") == false);
    REQUIRE_FALSE(test_config.exists(base + "0/config/skip_param_prompt"));

    REQUIRE(test_config.get<bool>(base + "1/config/require_confirmation") == true);
    REQUIRE_FALSE(test_config.exists(base + "1/config/skip_param_prompt"));

    // No key before, no key after — the default lives in the struct, not the file.
    REQUIRE_FALSE(test_config.exists(base + "2/config/require_confirmation"));
    REQUIRE(test_config.get<std::string>(base + "2/config/macro") == "LOAD_FILAMENT");

    REQUIRE(test_config.get<std::string>(base + "3/config/url") == "rtsp://x");

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
}

// A config already stamped at v23 must survive untouched — re-running the
// migration over a rewritten document would be a no-op only by luck otherwise.
TEST_CASE("Config: v22→v23 leaves an already-migrated config alone",
          "[core][config][migration][v23]") {
    json v23_config = {
        {"config_version", kCurrentConfigVersion},
        {"active_printer_id", "default"},
        {"printers",
         {{"default",
           {{"panel_widgets",
             {{"home",
               {{"pages",
                 {{{"id", "main"},
                   {"widgets",
                    {{{"id", "favorite_macro:1"},
                      {"enabled", true},
                      {"config",
                       {{"macro", "PURGE"}, {"require_confirmation", false}}}}}}}}}}}}}}}}}};
    SettingsFile file("helix_test_v23_idempotent", v23_config);

    Config test_config;
    test_config.init(file.path);

    REQUIRE(test_config.get<bool>("/printers/default/panel_widgets/home/pages/0/widgets/0/config/"
                                  "require_confirmation") == false);
    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
}

// v14->v15: an AD5X still carrying the wizard-era display overrides
// (hardware_blank=0 AND sleep_backlight_off=false) is put back on its preset.
// Detection needs both stale values and an AD5X, found either by the top-level
// preset or by /printers/<id>/type. See FLASHFORGE_AD5X_SUPPORT.md.
TEST_CASE_METHOD(ConfigTestFixture, "Config: v14→v15 restores the AD5X sleep preset",
                 "[core][config][migration][versioning]") {
    const json stale = {
        {"hardware_blank", 0}, {"sleep_backlight_off", false}, {"backlight_enable_ioctl", false}};
    struct Row {
        const char* name;
        json doc;
        int hardware_blank;
        bool sleep_backlight_off;
    };
    const Row rows[] = {
        {"AD5X preset after a wizard override",
         {{"preset", "ad5x"}, {"display", stale}, {"printer", {{"moonraker_host", "127.0.0.1"}}}},
         1,
         true},
        {"AD5X found by printer type in a multi-printer config",
         {{"display", stale},
          {"printers",
           {{"my-ad5x", {{"type", "FlashForge Adventurer 5X"}, {"moonraker_host", "127.0.0.1"}}}}}},
         1,
         true},
        // The user's own workaround: only sleep_backlight_off was changed
        {"one stale value alone is kept",
         {{"preset", "ad5x"},
          {"display", {{"hardware_blank", 1}, {"sleep_backlight_off", false}}},
          {"printer", {{"moonraker_host", "127.0.0.1"}}}},
         1,
         false},
        // CC1 legitimately ships these values
        {"a non-AD5X printer is left alone",
         {{"preset", "cc1"}, {"display", stale}, {"printer", {{"moonraker_host", "127.0.0.1"}}}},
         0,
         false},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            json doc = r.doc;
            doc["config_version"] = 14;
            SettingsFile file("helix_migration_v15", doc);
            BackupGuard guard;
            Config test_config;
            test_config.init(file.path);

            REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
            CHECK(test_config.get<int>("/display/hardware_blank") == r.hardware_blank);
            CHECK(test_config.get<bool>("/display/sleep_backlight_off") == r.sleep_backlight_off);
        }
    }
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

// A printer added by address has no name or type until it connects and is identified.
TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: a printer's display name is its name, then its type, then its host",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test({{"active_printer_id", "voron"},
                              {"printers",
                               {{"voron", {{"printer_name", "Voron 2.4"}, {"type", "Voron"}}},
                                {"printer-2", {{"moonraker_host", "10.0.0.2"}}},
                                {"printer-3", {{"moonraker_host", "10.0.0.3"}, {"type", "AD5M"}}},
                                {"printer-4", nlohmann::json::object()}}}});
    CHECK(config.get_printer_display_name("voron", "-") == "Voron 2.4");
    CHECK(config.get_printer_display_name("printer-3", "-") == "AD5M");
    CHECK(config.get_printer_display_name("printer-2", "-") == "10.0.0.2");
    CHECK(config.get_printer_display_name("printer-4", "-") == "-");

    REQUIRE(config.set_active_printer("voron"));
    CHECK(config.get_active_printer_name() == "Voron 2.4");
    REQUIRE(config.set_active_printer("printer-2"));
    CHECK(config.get_active_printer_name() == "10.0.0.2");
    REQUIRE(config.set_active_printer("printer-4"));
    CHECK(config.get_active_printer_name() == "printer-4");
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: find_printer_by_host matches host and port",
                 "[core][config][multi-printer]") {
    set_data_for_plural_test(
        {{"active_printer_id", "voron"},
         {"printers",
          {{"show_printer_switcher", true},
           {"voron", {{"moonraker_host", "192.168.1.10"}}},
           {"ender3", {{"moonraker_host", "192.168.1.20"}, {"moonraker_port", 7126}}}}}});

    CHECK(config.find_printer_by_host("192.168.1.10", 7125) == "voron"); // port defaults to 7125
    CHECK(config.find_printer_by_host("192.168.1.20", 7126) == "ender3");
    CHECK(config.find_printer_by_host("192.168.1.20", 7125).empty());
    CHECK(config.find_printer_by_host("192.168.1.30", 7125).empty());
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: next_printer_id picks an id no printer uses",
                 "[core][config][multi-printer]") {
    SECTION("numbers from the printer count") {
        set_data_for_plural_test({{"active_printer_id", "voron"},
                                  {"printers", {{"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
        REQUIRE(config.next_printer_id() == "printer-2");
    }

    SECTION("skips an id left in use after a delete") {
        // Two printers, so the count suggests printer-3, which is taken.
        set_data_for_plural_test({{"active_printer_id", "printer-3"},
                                  {"printers",
                                   {{"printer-3", {{"moonraker_host", "10.0.0.3"}}},
                                    {"printer-4", {{"moonraker_host", "10.0.0.4"}}}}}});
        REQUIRE(config.next_printer_id() == "printer-5");
    }

    SECTION("plain settings under /printers do not count as printers") {
        set_data_for_plural_test(
            {{"active_printer_id", "voron"},
             {"printers",
              {{"show_printer_switcher", true}, {"voron", {{"moonraker_host", "192.168.1.10"}}}}}});
        REQUIRE(config.next_printer_id() == "printer-2");
    }

    SECTION("the returned id is free to add") {
        set_data_for_plural_test({{"active_printer_id", "printer-2"},
                                  {"printers", {{"printer-2", {{"moonraker_host", "10.0.0.2"}}}}}});
        const std::string id = config.next_printer_id();
        REQUIRE(id != "printer-2");
        config.add_printer(id, {{"moonraker_host", "10.0.0.9"}});
        REQUIRE(config.get_printer_ids().size() == 2);
    }
}

namespace {

/// Checks what a remove/archive left behind in @p printers, a /printers map that
/// started as @p before: the printer count, the active selection df() routes at, each
/// surviving printer's host and every non-printer settings key, unchanged.
void check_printers_after(Config& config, const json& printers, const json& before,
                          size_t expected_count, const std::string& expected_active) {
    CHECK(config.get_printer_ids().size() == expected_count);
    CHECK(config.get_active_printer_id() == expected_active);
    CHECK(config.get<std::string>("/active_printer_id", "") == expected_active);
    CHECK(config.df() == "/printers/" + expected_active + "/");
    for (const auto& id : config.get_printer_ids()) {
        INFO("printer: " << id);
        CHECK(printers[id]["moonraker_host"] == before[id]["moonraker_host"]);
    }
    for (const auto& [key, value] : before.items()) {
        if (!value.is_object()) {
            INFO("settings key: " << key);
            REQUIRE(printers.contains(key));
            CHECK(printers[key] == value);
        }
    }
}

const json kVoron = {{"moonraker_host", "192.168.1.10"}};
const json kEnder3 = {{"moonraker_host", "192.168.1.20"}};
const json kZortrax = {{"moonraker_host", "192.168.1.30"}};
const char* const kComment = "Show the printer switcher in the header.";

} // namespace

// The /printers map is MIXED. Alongside the printer objects it holds plain
// settings keys: show_printer_switcher (a bool, seeded by get_default_config())
// and _show_printer_switcher_comment (a string, shipped in
// config/settings.json.template). Every real config has at least the bool, so
// the rows below carry them wherever a printers-only map would hide a bug
// (prestonbrown/helixscreen#1210). nlohmann orders keys lexicographically, so a
// "_..." or "show_..." key is the FIRST entry when every printer id sorts after it.
TEST_CASE_METHOD(ConfigTestFixture, "Config: remove_printer keeps a valid active printer",
                 "[core][config][multi-printer][1210]") {
    struct Row {
        const char* name;
        json printers;
        const char* remove;
        size_t expected_count;
        const char* expected_active;
    };
    const Row rows[] = {
        {"removing the active printer selects the remaining one",
         {{"voron", kVoron}, {"ender3", kEnder3}},
         "voron",
         1,
         "ender3"},
        {"removing another printer keeps the active one",
         {{"voron", kVoron}, {"ender3", kEnder3}},
         "ender3",
         1,
         "voron"},
        {"the last printer is never removed", {{"voron", kVoron}}, "voron", 1, "voron"},
        {"the last-printer guard counts printers, not map entries",
         {{"show_printer_switcher", false}, {"voron", kVoron}},
         "voron",
         1,
         "voron"},
        {"the last-printer guard ignores the comment string key",
         {{"_show_printer_switcher_comment", kComment},
          {"show_printer_switcher", true},
          {"voron", kVoron}},
         "voron",
         1,
         "voron"},
        {"an id naming a settings key erases nothing",
         {{"show_printer_switcher", true}, {"voron", kVoron}, {"zortrax", kZortrax}},
         "show_printer_switcher",
         2,
         "voron"},
        {"the auto-switch skips the comment string key",
         {{"_show_printer_switcher_comment", kComment}, {"voron", kVoron}, {"zortrax", kZortrax}},
         "voron",
         1,
         "zortrax"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            set_data_for_plural_test({{"active_printer_id", "voron"}, {"printers", r.printers}});
            REQUIRE(config.set_active_printer("voron"));

            config.remove_printer(r.remove);

            check_printers_after(config, get_data()["printers"], r.printers, r.expected_count,
                                 r.expected_active);
        }
    }
}

// ---------------------------------------------------------------------------
// archive_printer(): the non-destructive removal stale-printer recovery uses.
// remove_printer() erases a fully configured printer (host, heaters, macros,
// sensors) with no way back; archive_printer() keeps a verbatim copy under
// /removed_printers.
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
    REQUIRE(archived.contains(Config::kArchivedAtKey));
    REQUIRE(archived[Config::kArchivedAtKey].is_number_integer());
    archived.erase(Config::kArchivedAtKey);
    REQUIRE(archived == voron);
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: archive_printer keeps a valid active printer",
                 "[core][config][multi-printer][1210]") {
    struct Row {
        const char* name;
        json printers;
        const char* archive;
        size_t expected_count;
        const char* expected_active;
        bool archived; // false: /removed_printers is never created
    };
    const Row rows[] = {
        // remove_printer()'s contract: archiving the active printer leaves a valid selection
        {"archiving the active printer selects the remaining one",
         {{"voron", kVoron}, {"ender3", kEnder3}},
         "voron",
         1,
         "ender3",
         true},
        // No phantom duplicate for recovery UI to "restore" over a printer that never left
        {"the last printer is not archived", {{"voron", kVoron}}, "voron", 1, "voron", false},
        {"an unknown id is a no-op",
         {{"voron", kVoron}, {"ender3", kEnder3}},
         "does-not-exist",
         2,
         "voron",
         false},
        {"the auto-switch skips non-printer keys",
         {{"show_printer_switcher", false}, {"voron", kVoron}, {"zortrax", kZortrax}},
         "voron",
         1,
         "zortrax",
         true},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            set_data_for_plural_test({{"active_printer_id", "voron"}, {"printers", r.printers}});
            REQUIRE(config.set_active_printer("voron"));

            config.archive_printer(r.archive);

            check_printers_after(config, get_data()["printers"], r.printers, r.expected_count,
                                 r.expected_active);
            if (r.archived) {
                CHECK(get_data()["removed_printers"].contains(r.archive));
            } else {
                CHECK_FALSE(data_contains("removed_printers"));
            }
        }
    }
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
    const size_t total = Config::kMaxArchivedPrinters + 3;

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
    REQUIRE(archive.size() == Config::kMaxArchivedPrinters);

    for (size_t i = 0; i < total; i++) {
        INFO("printer index " << i);
        // printer-0 was archived last, so the survivors are the LOW indices.
        REQUIRE(archive.contains(ids[i]) == (i < Config::kMaxArchivedPrinters));
    }

    // Stamps are strictly ordered by archive order even though every archive
    // happened within the same wall-clock second.
    int64_t newer = 0;
    for (size_t i = 0; i < Config::kMaxArchivedPrinters; i++) {
        INFO("printer index " << i);
        REQUIRE(archive.at(ids[i]).contains(Config::kArchivedAtKey));
        const int64_t stamp = archive.at(ids[i]).at(Config::kArchivedAtKey).get<int64_t>();
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
    for (size_t i = 0; i < Config::kMaxArchivedPrinters; i++) {
        legacy["legacy-" + std::to_string(i)] = {{"moonraker_host", "172.16.0.1"}};
    }
    legacy["legacy-2"][Config::kArchivedAtKey] = "not-a-number";
    legacy["legacy-3"][Config::kArchivedAtKey] = nullptr;
    get_data()["removed_printers"] = legacy;

    config.archive_printer("voron");

    const json& archive = get_data()["removed_printers"];
    REQUIRE(archive.size() == Config::kMaxArchivedPrinters);
    // The stamped newcomer survives; the unstamped entry whose key sorts first
    // is the one dropped.
    REQUIRE(archive.contains("voron"));
    REQUIRE_FALSE(archive.contains("legacy-0"));
    for (size_t i = 1; i < Config::kMaxArchivedPrinters; i++) {
        INFO("legacy index " << i);
        REQUIRE(archive.contains("legacy-" + std::to_string(i)));
    }
}

TEST_CASE_METHOD(ConfigTestFixture, "Config: is_wizard_required prefers the per-printer flag",
                 "[core][config][multi-printer]") {
    SECTION("a per-printer true beats a root false") {
        set_data_for_plural_test(
            {{"active_printer_id", "voron"},
             {"wizard_completed", false},
             {"printers",
              {{"voron", {{"wizard_completed", true}, {"moonraker_host", "10.0.0.1"}}}}}});
    }
    SECTION("no per-printer flag falls back to the root true") {
        set_data_for_plural_test({{"active_printer_id", "voron"},
                                  {"wizard_completed", true},
                                  {"printers", {{"voron", {{"moonraker_host", "10.0.0.1"}}}}}});
    }
    config.set_active_printer("voron");
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

// The marker lives under the active printer, not at the config root
// (test_config_preset_per_printer.cpp covers multi-printer isolation and the
// legacy root-key lift). A root-level key belongs to no printer: reading it would
// leak one printer's preset into another (#1162).
TEST_CASE_METHOD(ConfigTestFixture, "has_preset reads only the active printer's marker",
                 "[config][preset]") {
    SECTION("an empty config has none") {
        get_data() = json::object();
        CHECK_FALSE(config.has_preset());
        CHECK(config.get_preset().empty());
    }
    SECTION("a printer preset is found") {
        setup_printer_data(config, {{"preset", "ad5m"}});
        CHECK(config.has_preset());
        CHECK(config.get_preset() == "ad5m");
    }
    SECTION("an empty preset string is none") {
        setup_printer_data(config, {{"preset", ""}});
        CHECK_FALSE(config.has_preset());
        CHECK(config.get_preset().empty());
    }
    SECTION("a stray root-level preset key is ignored") {
        setup_printer_data(config, json::object());
        get_data()["preset"] = "ad5m";
        CHECK_FALSE(config.has_preset());
        CHECK(config.get_preset().empty());
    }
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

// ============================================================================
// RAII guard for HOME and both backup tiers
// ============================================================================

namespace {

/// RAII guard for HOME environment variable — ensures cleanup even if assertions throw.
/// Prevents environ poisoning that creates junk single-char directories in CWD.
///
/// Also redirects the primary (StateDirectory) backup tier under the same temp
/// home. The primary tier outranks the $HOME fallback in the search order, so
/// without this a real /var/lib/helixscreen/settings.json.backup on the host
/// would be restored instead of the fixture's backup.
struct HomeGuard {
    std::string original;
    std::string prev_state_dir;
    std::string prev_fallback_dir;
    bool had_home;
    explicit HomeGuard(const std::string& new_home)
        : prev_state_dir(app_constants::update::detail::state_dir_ref()),
          prev_fallback_dir(app_constants::update::detail::backup_fallback_dir_ref()),
          had_home(std::getenv("HOME") != nullptr) {
        if (had_home)
            original = std::getenv("HOME");
        setenv("HOME", new_home.c_str(), 1);
        app_constants::update::detail::backup_fallback_dir_ref() =
            app_constants::update::sanitize_home(std::getenv("HOME")) + "/.helixscreen";
        app_constants::update::detail::state_dir_ref() = new_home + "/var-lib";
    }
    ~HomeGuard() {
        if (had_home)
            setenv("HOME", original.c_str(), 1);
        else
            unsetenv("HOME");
        // Restore the SAVED value — see the note in ~BackupGuard().
        app_constants::update::detail::backup_fallback_dir_ref() = prev_fallback_dir;
        app_constants::update::detail::state_dir_ref() = prev_state_dir;
    }
};

} // namespace

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
        : dir(helix::test::unique_temp_dir("helix_test_" + name)),
          config_path((dir / "settings.json").string()),
          backup_dir((dir / ".helixscreen").string()), home(dir.string()) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }

    ~TarballTestEnv() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void write_config(const json& j) {
        std::ofstream o(config_path);
        o << j.dump(2);
    }

    void write_config_raw(const std::string& content) {
        std::ofstream o(config_path);
        o << content;
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
        return dir / app_constants::update::kFreshInstallMarker;
    }

    void write_fresh_marker() {
        std::ofstream o(fresh_marker());
    }
};

} // namespace

// An unreadable settings.json is set aside as .corrupt and replaced by the
// rolling backup, or by the defaults when the backup is missing or unreadable.
// A zero-byte file is what a power cut mid-write leaves behind.
TEST_CASE("Config::init() recovers an unreadable config from the backup or the defaults",
          "[core][config][corruption][integration][error_path]") {
    auto backup_for = [](const char* host, int brightness) {
        return json{{"config_version", kCurrentConfigVersion},
                    {"active_printer_id", "voron"},
                    {"brightness", brightness},
                    {"printers", {{"voron", {{"moonraker_host", host}}}}}}
            .dump(2);
    };
    struct Row {
        const char* name;
        const char* config;
        std::string backup; // empty: no backup file
        const char* host_ptr;
        const char* host;
        int brightness; // -1: not checked
    };
    const Row rows[] = {
        {"corrupt config, valid backup", "{{{{ not valid json !!!!", backup_for("10.0.0.5", 75),
         "/printers/voron/moonraker_host", "10.0.0.5", 75},
        {"corrupt config, no backup", "not json at all", "", "/printers/default/moonraker_host",
         "127.0.0.1", -1},
        {"corrupt config, corrupt backup", "{truncated", "also not valid json",
         "/printers/default/moonraker_host", "127.0.0.1", -1},
        {"zero-byte config, valid backup", "", backup_for("10.0.0.7", 61),
         "/printers/voron/moonraker_host", "10.0.0.7", 61},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            TarballTestEnv env("corrupt_recovery");
            env.write_config_raw(r.config);
            if (!r.backup.empty()) {
                env.write_backup_raw(r.backup);
            }

            Config test_config;
            test_config.init(env.config_path);

            CHECK(test_config.get<std::string>(r.host_ptr) == r.host);
            if (r.brightness >= 0) {
                CHECK(test_config.get<int>("/brightness") == r.brightness);
            }
            if (*r.config != '\0') {
                // Kept for diagnosis
                CHECK(std::filesystem::exists(env.config_path + ".corrupt"));
            }
        }
    }
}

TEST_CASE("Config::init() restores backup when tarball default replaces user config",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("tarball_default");

    env.write_config({{"preset", "ad5x"},
                      {"wizard_completed", false},
                      {"display", {{"rotate", 0}}},
                      {"printer", {{"moonraker_host", "127.0.0.1"}}}});

    env.write_backup({{"config_version", kCurrentConfigVersion},
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

// A packaged default with nothing better to restore stays, and the wizard runs.
TEST_CASE("Config::init() keeps a tarball default when no usable backup exists",
          "[core][config][moonraker-update]") {
    const json tarball_default = {{"preset", "ad5x"},
                                  {"wizard_completed", false},
                                  {"printer", {{"moonraker_host", "127.0.0.1"}}}};
    struct Row {
        const char* name;
        std::string backup; // empty: no backup file
    };
    const Row rows[] = {
        {"no backup (fresh install)", ""},
        {"the backup is also a tarball default", tarball_default.dump(2)},
        {"the backup is corrupt", "{{{{ not valid json"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            TarballTestEnv env("tarball_kept");
            env.write_config(tarball_default);
            if (!r.backup.empty()) {
                env.write_backup_raw(r.backup);
            }

            Config test_config;
            test_config.init(env.config_path);

            CHECK(test_config.is_wizard_required());
        }
    }
}

// A versionless document can name no printer at all: the installer creates
// one holding only the keys it seeds. It must boot with the default printer
// and its defaults, not with an empty active printer id.
TEST_CASE("Config::init() gives a versionless config with no printer the default printer",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("versionless_no_printer");
    bool device_blocks = false;

    SECTION("the installer's update-channel seed") {
        env.write_config({{"update", {{"channel", 1}}}});
    }
    SECTION("the device blocks with an explicit version 0") {
        env.write_config({{"config_version", 0},
                          {"update", {{"channel", 1}}},
                          {"input", {{"calibration", {{"valid", true}, {"a", 2.0}}}}},
                          {"display", {{"rotate", 180}}}});
        device_blocks = true;
    }
    SECTION("the per-printer seed's device blocks") {
        env.write_config({{"update", {{"channel", 1}}},
                          {"input", {{"calibration", {{"valid", true}, {"a", 2.0}}}}},
                          {"display", {{"rotate", 180}}}});
        device_blocks = true;
    }

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.get_active_printer_id() == "default");
    CHECK(test_config.get<std::string>("/printers/default/moonraker_host") == "127.0.0.1");
    CHECK(test_config.get<int>("/printers/default/moonraker_port") == 7125);
    CHECK(test_config.get<std::string>("/printers/default/heaters/bed") == "heater_bed");
    CHECK(test_config.get<int>("/update/channel") == 1);
    // Everything else a fresh install starts with, under the seeded keys.
    CHECK(test_config.get<std::string>("/log_path") == "/tmp/helixscreen.log");
    CHECK(test_config.get<int>("/gcode_viewer/tube_sides") == 4);
    if (device_blocks) {
        CHECK(test_config.get<bool>("/input/calibration/valid"));
        CHECK(test_config.get<double>("/input/calibration/a") == 2.0);
        CHECK(test_config.get<int>("/display/rotate") == 180);
        // A calibration the installer just seeded is not one to re-check.
        CHECK_FALSE(test_config.get<bool>("/input/calibration/recheck_pending", false));
    }

    auto on_disk = json::parse(std::ifstream(env.config_path));
    CHECK(on_disk.value("active_printer_id", "") == "default");
    CHECK(on_disk.value("config_version", 0) == kCurrentConfigVersion);
}

// Moonraker's web updater can leave the installer's minimal file where the
// user's config was. The rolling backup is the real config and must win over
// the fresh-install path for printerless documents.
TEST_CASE("Config::init() restores the backup over a printerless versionless config",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("seed_doc_backup_wins");
    env.write_config({{"update", {{"channel", 1}}}});
    env.write_backup({{"config_version", kCurrentConfigVersion},
                      {"active_printer_id", "my-pi"},
                      {"update", {{"channel", 0}}},
                      {"printers", {{"my-pi", {{"moonraker_host", "192.168.1.50"}}}}}});

    Config test_config;
    test_config.init(env.config_path);

    CHECK(test_config.get_active_printer_id() == "my-pi");
    CHECK(test_config.get<std::string>("/printers/my-pi/moonraker_host") == "192.168.1.50");
    CHECK(test_config.get<int>("/update/channel") == 0);
    CHECK_FALSE(test_config.exists("/printers/default"));
}

// Legacy display keys in a printerless document move to their current home
// before the defaults are laid under it, or the defaults would shadow them.
TEST_CASE("Config::init() migrates legacy display keys in a printerless versionless config",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("seed_doc_legacy_display");
    env.write_config({{"update", {{"channel", 1}}},
                      {"display", {{"calibration", {{"valid", true}, {"a", 2.0}}}}}});

    Config test_config;
    test_config.init(env.config_path);

    CHECK(test_config.get<bool>("/input/calibration/valid"));
    CHECK(test_config.get<double>("/input/calibration/a") == 2.0);
    CHECK_FALSE(test_config.exists("/display/calibration"));
    CHECK_FALSE(test_config.get<bool>("/input/calibration/recheck_pending", false));
}

// The fresh-install path is for documents that hold no printer at all. A real
// config, versioned or a shipped preset's versionless /printer, keeps its own
// shape and runs the migration chain.
TEST_CASE("Config::init() leaves a config with a printer off the fresh-install path",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("versionless_with_printer");

    SECTION("a versionless preset with a /printer") {
        env.write_config({{"input", {{"calibration", {{"valid", true}, {"a", 2.0}}}}},
                          {"printer", {{"moonraker_host", "192.168.1.50"}}}});
        Config test_config;
        test_config.init(env.config_path);

        CHECK(test_config.get<std::string>("/printers/default/moonraker_host") == "192.168.1.50");
        CHECK_FALSE(test_config.get<bool>("/input/calibration/recheck_pending", false));
        CHECK_FALSE(test_config.exists("/log_path"));
    }
    SECTION("a versioned config with a printer") {
        env.write_config({{"config_version", kCurrentConfigVersion},
                          {"active_printer_id", "voron"},
                          {"printers", {{"voron", {{"moonraker_host", "192.168.1.60"}}}}}});
        Config test_config;
        test_config.init(env.config_path);

        CHECK(test_config.get_active_printer_id() == "voron");
        CHECK(test_config.get<std::string>("/printers/voron/moonraker_host") == "192.168.1.60");
        CHECK_FALSE(test_config.exists("/log_path"));
        CHECK_FALSE(test_config.exists("/printers/default"));
    }
}

// A config written by a newer build may keep its printers in a shape this build
// does not know. It must come through untouched, with no default printer added.
TEST_CASE("Config::init() adds no default printer to a config from a newer build",
          "[core][config][moonraker-update]") {
    TarballTestEnv env("newer_build_printers");
    const json printers = {{"show_printer_switcher", true},
                           {"list", json::array({{{"id", "voron"}}})}};
    env.write_config({{"config_version", kCurrentConfigVersion + 1}, {"printers", printers}});

    Config test_config;
    test_config.init(env.config_path);

    auto on_disk = json::parse(std::ifstream(env.config_path));
    CHECK(on_disk.value("config_version", 0) == kCurrentConfigVersion + 1);
    CHECK(on_disk["printers"] == printers);
    CHECK_FALSE(on_disk.contains("active_printer_id"));
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
    env.write_backup({{"config_version", kCurrentConfigVersion},
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
    env.write_backup({{"config_version", kCurrentConfigVersion},
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

    env.write_backup({{"config_version", kCurrentConfigVersion},
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

namespace {

/// A below-floor document of a remote-UI install: its printer lives on another host.
json below_floor_backup() {
    return {{"config_version", 5},
            {"wizard_completed", true},
            {"active_printer_id", "voron"},
            {"printers",
             {{"voron",
               {{"moonraker_host", "192.168.1.77"},
                {"moonraker_port", 7126},
                {"wizard_completed", true}}}}}};
}

// TEST_MIRROR_OK: read_json_file() parses a fixture file; it shares only its name with the filament
// catalog loader
json read_json_file(const std::string& file) {
    return json::parse(helix::text_io::read_file(file).value_or(""), nullptr, false);
}

} // namespace

TEST_CASE("Config::init() keeps a below-floor backup it restores",
          "[core][config][moonraker-update][migration]") {
    TarballTestEnv env("floor_backup");
    SECTION("over a tarball default") {
        env.write_config({{"preset", "ad5m"},
                          {"wizard_completed", false},
                          {"printer", {{"moonraker_host", "127.0.0.1"}}}});
    }
    SECTION("over a corrupt config") {
        env.write_config_raw("{{{{ not valid json");
    }
    env.write_backup(below_floor_backup());

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE(test_config.get_active_printer_id() == "default");
    REQUIRE(read_json_file(env.config_path + ".pre-migration") == below_floor_backup());
    REQUIRE(read_json_file(env.backup_dir + "/settings.json.backup") == below_floor_backup());
}

TEST_CASE("Config::init() carries the Moonraker connection across the migration floor",
          "[core][config][migration]") {
    TarballTestEnv env("floor_connection");

    SECTION("printers map") {
        env.write_config(below_floor_backup());
    }
    SECTION("single /printer object") {
        env.write_config({{"config_version", 3},
                          {"printer",
                           {{"moonraker_host", "192.168.1.77"},
                            {"moonraker_port", 7126},
                            {"moonraker_api_key", "abc123"}}}});
    }

    Config test_config;
    test_config.init(env.config_path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE(test_config.get<std::string>(test_config.df() + "moonraker_host") == "192.168.1.77");
    REQUIRE(test_config.get<int>(test_config.df() + "moonraker_port") == 7126);
    REQUIRE(test_config.is_wizard_required());
}

TEST_CASE("Config::init() carries the update channel across the migration floor",
          "[core][config][migration]") {
    // The installer seeds the channel into a legacy backup, which is always
    // below the floor; restored over a versionless settings.json, it is what
    // decides the channel the app boots on.
    TarballTestEnv env("floor_update_channel");
    env.write_config({{"wizard_completed", false}, {"printer", {{"moonraker_host", "127.0.0.1"}}}});
    json legacy = below_floor_backup();
    legacy["config_version"] = 8;

    SECTION("a channel in the legacy backup survives") {
        legacy["update"] = {{"channel", 1}};
        std::filesystem::create_directories(env.backup_dir);
        std::ofstream(env.backup_dir + "/helixconfig.json.backup") << legacy.dump(2);

        Config test_config;
        test_config.init(env.config_path);

        REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
        REQUIRE(test_config.get<int>("/update/channel", -1) == 1);
    }
    SECTION("no channel in the legacy backup leaves the default") {
        std::filesystem::create_directories(env.backup_dir);
        std::ofstream(env.backup_dir + "/helixconfig.json.backup") << legacy.dump(2);

        Config test_config;
        test_config.init(env.config_path);

        REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
        REQUIRE(test_config.get<std::string>(test_config.df() + "moonraker_host") ==
                "192.168.1.77");
        REQUIRE_FALSE(test_config.exists("/update/channel"));
    }
}

// ============================================================================
// v10→v11 migration: PID heat rates to shared thermal path + strip heating phases
// ============================================================================

TEST_CASE("Config: v10→v11 migration moves heat rates and strips heating phases",
          "[core][config][migration][v11]") {
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

    SettingsFile file("helix_test_v10_to_v11", v10_config);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);

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

    // Heating phase durations (keys "3" and "4") should be stripped. The later
    // v24->v25 migration names whatever survives, so the surviving keys are
    // checked by name.
    auto entries = test_config.get<json>("/print_start_history/entries");
    REQUIRE(entries.size() == 3);

    // First entry: had phases 0, 1, 3, 4 → should keep 0, 1
    REQUIRE(entries[0]["phases"].contains("IDLE"));
    REQUIRE(entries[0]["phases"].contains("INITIALIZING"));
    REQUIRE_FALSE(entries[0]["phases"].contains("HEATING_BED"));
    REQUIRE_FALSE(entries[0]["phases"].contains("HEATING_NOZZLE"));

    // Second entry: had phases 0, 3, 4 → should keep 0
    REQUIRE(entries[1]["phases"].contains("IDLE"));
    REQUIRE_FALSE(entries[1]["phases"].contains("HEATING_BED"));
    REQUIRE_FALSE(entries[1]["phases"].contains("HEATING_NOZZLE"));

    // Third entry: no phases key → unchanged
    REQUIRE(entries[2].contains("no_phases"));
    REQUIRE_FALSE(entries[2].contains("phases"));
}

TEST_CASE("Config: v10→v11 migration is idempotent", "[core][config][migration][v11]") {
    // Config already at v11 with thermal rates in the new location
    json v11_config = {
        {"config_version", 10},
        {"thermal", {{"rates", {{"extruder", {{"heat_rate", 1.25}}}}}}},
        {"calibration",
         {{"pid_history", {{"extruder", {{"heat_rate", 0.99}, {"oscillation_duration", 3.5}}}}}}},
        {"printer", {{"moonraker_host", "127.0.0.1"}, {"moonraker_port", 7125}}}};

    SettingsFile file("helix_test_v10_to_v11_idempotent", v11_config);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    // Should keep existing value at new path (not overwrite with old)
    REQUIRE(test_config.get<double>("/thermal/rates/extruder/heat_rate") == Catch::Approx(1.25));

    // Old heat_rate should still be erased
    REQUIRE_FALSE(test_config.exists("/calibration/pid_history/extruder/heat_rate"));

    // Oscillation duration preserved
    REQUIRE(test_config.get<double>("/calibration/pid_history/extruder/oscillation_duration") ==
            Catch::Approx(3.5));
}

TEST_CASE("Config: v10→v11 migration handles missing calibration data gracefully",
          "[core][config][migration][v11]") {
    // Minimal v10 config with no calibration or print history
    json v10_config = {{"config_version", 10},
                       {"printer", {{"moonraker_host", "127.0.0.1"}, {"moonraker_port", 7125}}}};

    SettingsFile file("helix_test_v10_to_v11_empty", v10_config);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
    // No crash, no thermal section created
    REQUIRE_FALSE(test_config.exists("/thermal"));
}

TEST_CASE("Config: v11→v12 migration consolidates chamber keys for all printers",
          "[core][config][migration][v12]") {
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

    SettingsFile file("helix_test_v11_to_v12", v11_config);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);

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
}

TEST_CASE("Config: v11→v12 migration does not overwrite canonical keys already set",
          "[core][config][migration][v12]") {
    json v11_config = {{"config_version", 11},
                       {"printers",
                        {{"k1c",
                          {{"temp_sensors", {{"chamber", "temperature_sensor new_chamber"}}},
                           {"printer", {{"chamber_sensor", "temperature_sensor old_chamber"}}}}}}}};

    SettingsFile file("helix_test_v11_to_v12_existing", v11_config);

    BackupGuard guard;
    Config test_config;
    test_config.init(file.path);

    // Canonical value preserved; legacy value discarded (migrate_config_keys behavior)
    REQUIRE(test_config.get<std::string>("/printers/k1c/temp_sensors/chamber") ==
            "temperature_sensor new_chamber");
    REQUIRE_FALSE(test_config.exists("/printers/k1c/printer/chamber_sensor"));
}

// ============================================================================
// v15→v16 migration: screensaver off on BASIC/EMBEDDED tier with Flying Toasters
// ============================================================================

// A pre-v16 config carrying Flying Toasters (type 1) on a BASIC or EMBEDDED tier
// is switched off and queues a one-time notice; any other tier or screensaver,
// or a config already at v16, is left as it is.
TEST_CASE("Config: v15→v16 migration disables Flying Toasters on constrained tiers",
          "[config][migration][v16]") {
    struct TierGuard {
        ~TierGuard() {
            helix::config_testing::set_forced_tier_for_migration(std::nullopt);
        }
    };
    TierGuard tier_guard; // Resets the override even if a row fails

    struct Row {
        const char* name;
        helix::PlatformTier tier;
        int version;
        int type;
        const char* expected;
        bool notice;
    };
    const Row rows[] = {
        {"BASIC tier, Flying Toasters", helix::PlatformTier::BASIC, 15, 1, "off", true},
        {"EMBEDDED tier, Flying Toasters", helix::PlatformTier::EMBEDDED, 15, 1, "off", true},
        {"STANDARD tier, Flying Toasters", helix::PlatformTier::STANDARD, 15, 1, "toasters", false},
        {"BASIC tier, Starfield", helix::PlatformTier::BASIC, 15, 2, "starfield", false},
        {"BASIC tier, Pipes 3D", helix::PlatformTier::BASIC, 15, 3, "pipes", false},
        {"BASIC tier, already off", helix::PlatformTier::BASIC, 15, 0, "off", false},
        {"BASIC tier, already v16", helix::PlatformTier::BASIC, 16, 1, "toasters", false},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            helix::config_testing::set_forced_tier_for_migration(r.tier);
            SettingsFile file(
                "helix_test_v15_to_v16",
                {{"config_version", r.version}, {"display", {{"screensaver_type", r.type}}}});
            BackupGuard guard;
            Config test_config;
            test_config.init(file.path);

            REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);
            CHECK(test_config.get<std::string>("/display/screensaver_type", "") == r.expected);
            if (r.notice) {
                CHECK(
                    test_config.get<bool>("/display/screensaver_migration_notice_pending", false));
            } else {
                CHECK_FALSE(test_config.exists("/display/screensaver_migration_notice_pending"));
            }
        }
    }
}

TEST_CASE("Config: v16→v17 migration renames retired Voron printer_image IDs",
          "[core][config][migration][v17]") {
    SECTION("voron-24r2 and voron-0-2 rewritten; unrelated values left alone") {
        json v16_config = {{"config_version", 16},
                           {"active_printer_id", "voron24"},
                           {"printers",
                            {{"voron24", {{"printer_image", "shipped:voron-24r2"}}},
                             {"voron0", {{"printer_image", "shipped:voron-0-2"}}},
                             {"sw", {{"printer_image", "shipped:voron-switchwire"}}},
                             {"custom", {{"printer_image", "custom:my-printer"}}},
                             {"auto", {{"printer_image", ""}}},
                             {"none", {{"heaters", {{"bed", "heater_bed"}}}}}}}};

        SettingsFile file("helix_test_v16_to_v17_rename", v16_config);

        BackupGuard guard;
        Config test_config;
        test_config.init(file.path);

        REQUIRE(test_config.get<int>("/config_version") == kCurrentConfigVersion);

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
    }

    SECTION("Already-v17 config: migration does not run") {
        SettingsFile file("helix_test_v16_to_v17_already",
                          {{"config_version", 17},
                           {"active_printer_id", "voron24"},
                           {"printers", {{"voron24", {{"printer_image", "shipped:voron-24r2"}}}}}});

        BackupGuard guard;
        Config test_config;
        test_config.init(file.path);

        // The stale value is kept verbatim: the version<17 gate is skipped.
        REQUIRE(test_config.get<std::string>("/printers/voron24/printer_image") ==
                "shipped:voron-24r2");
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

/// RAII: force RuntimeConfig::test_mode; the whole config is restored on exit.
struct ConfigTestModeGuard {
    ScopedRuntimeConfig scoped_config;
    explicit ConfigTestModeGuard(bool enabled) {
        get_runtime_config()->test_mode = enabled;
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
        : dir(helix::test::unique_temp_dir("helix_test_" + name)),
          prev_state_dir(app_constants::update::detail::state_dir_ref()),
          prev_fallback_dir(app_constants::update::detail::backup_fallback_dir_ref()),
          had_home(std::getenv("HOME") != nullptr) {
        if (had_home)
            original_home = std::getenv("HOME");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        setenv("HOME", dir.string().c_str(), 1);
        app_constants::update::detail::state_dir_ref() = (dir / "var-lib").string();
        app_constants::update::detail::backup_fallback_dir_ref() = (dir / ".helixscreen").string();
    }

    ~BackupSandbox() {
        if (had_home)
            setenv("HOME", original_home.c_str(), 1);
        else
            unsetenv("HOME");
        app_constants::update::detail::state_dir_ref() = prev_state_dir;
        app_constants::update::detail::backup_fallback_dir_ref() = prev_fallback_dir;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string config_path() const {
        return (dir / "settings-test.json").string();
    }

    /// Write a backup at the primary (StateDirectory) tier.
    void write_primary_backup(const json& j) {
        std::filesystem::create_directories(app_constants::update::state_dir());
        std::ofstream o(app_constants::update::config_backup_primary());
        o << j.dump(2);
    }

    bool primary_backup_exists() const {
        return std::filesystem::exists(app_constants::update::config_backup_primary());
    }

    json read_primary_backup() const {
        return json::parse(std::fstream(app_constants::update::config_backup_primary()));
    }
};

/// A recognizable "production" backup — marker values that must never leak into
/// a test-mode config.
json production_backup_json() {
    return json{{"config_version", kCurrentConfigVersion},
                {"active_printer_id", "real-production-printer"},
                {"brightness", 42},
                {"wizard_completed", true},
                {"printers", {{"real-production-printer", {{"moonraker_host", "10.9.9.9"}}}}}};
}

} // namespace

// Test mode reads no production backup: neither to replace a missing config nor
// to adopt one over a versionless (tarball-default) config. Production keeps both.
TEST_CASE("Config::init() adopts a production backup only outside test mode",
          "[core][config][test-mode][backup-isolation]") {
    struct Row {
        const char* name;
        bool test_mode;
        bool tarball_config; // false: no settings file at all
        bool adopted;
    };
    const Row rows[] = {
        {"test mode, missing config", true, false, false},
        {"production, missing config", false, false, true},
        {"test mode, tarball-default config", true, true, false},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            BackupSandbox env("backup_adopt");
            env.write_primary_backup(production_backup_json());
            REQUIRE(env.primary_backup_exists()); // a backup IS available
            if (r.tarball_config) {
                // config_version absent: tarball detection would fire outside test mode
                std::ofstream(env.config_path())
                    << json{{"preset", "ad5x"},
                            {"wizard_completed", false},
                            {"printers", {{"default", {{"moonraker_host", "127.0.0.1"}}}}}}
                           .dump(2);
            } else {
                REQUIRE_FALSE(std::filesystem::exists(env.config_path()));
            }
            ConfigTestModeGuard test_mode(r.test_mode);

            Config test_config;
            test_config.init(env.config_path());

            CHECK((test_config.get<std::string>("/active_printer_id", "") ==
                   "real-production-printer") == r.adopted);
            CHECK((test_config.get<int>("/brightness", 0) == 42) == r.adopted);
            if (!r.adopted) {
                CHECK(test_config.get<std::string>(
                          "/printers/real-production-printer/moonraker_host", "") == "");
            }
        }
    }
}

// ============================================================================
// Rolling-backup writes from save()
// ============================================================================

namespace {

/// A config whose only meaningful variable is whether setup ever completed.
/// config_version is CURRENT so tarball detection (which triggers on
/// config_version == 0) cannot adopt the backup over it.
json wizard_state_config_json(bool wizard_completed) {
    return json{
        {"config_version", kCurrentConfigVersion},
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

// The single-slot rolling backup is the only recovery copy of the user's real
// config. save() refreshes it only outside test mode and only once setup is
// complete: test data and a wizard-incomplete config (preset defaults, not user
// data) must never replace it, and the setup-in-progress config must not reach
// the fallback tier either. The wizard rows differ in exactly one bit, so the
// guard cannot be satisfied by disabling rolling backups altogether.
TEST_CASE("Config::save() refreshes the rolling backup only for a finished production config",
          "[core][config][test-mode][backup-isolation][wizard]") {
    struct Row {
        const char* name;
        bool test_mode;
        bool wizard_completed;
        bool prior_backup;
        int backup_brightness; // 0: no primary backup afterwards
        const char* backup_printer;
    };
    const Row rows[] = {
        {"test mode writes no backup", true, true, false, 0, nullptr},
        {"production writes one", false, true, false, 9, "fresh"},
        {"an incomplete wizard keeps the old backup", false, false, true, 42,
         "real-production-printer"},
        {"a completed wizard refreshes the old backup", false, true, true, 9, "fresh"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            BackupSandbox env("rolling_backup_write");
            ConfigDirEnvGuard config_dir_guard;
            if (r.prior_backup) {
                env.write_primary_backup(production_backup_json());
                REQUIRE(env.read_primary_backup().value("brightness", 0) == 42);
            }
            std::ofstream(env.config_path())
                << wizard_state_config_json(r.wizard_completed).dump(2);
            ConfigTestModeGuard test_mode(r.test_mode);

            Config test_config;
            test_config.init(env.config_path());
            REQUIRE(test_config.is_wizard_required() == !r.wizard_completed);

            test_config.set("/brightness", 9);
            REQUIRE(test_config.save());

            if (r.backup_brightness == 0) {
                CHECK_FALSE(env.primary_backup_exists());
            } else {
                REQUIRE(env.primary_backup_exists());
                CHECK(env.read_primary_backup().value("brightness", 0) == r.backup_brightness);
                CHECK(env.read_primary_backup().value("active_printer_id", "") == r.backup_printer);
            }
            if (r.test_mode || !r.wizard_completed) {
                CHECK_FALSE(
                    std::filesystem::exists(app_constants::update::config_backup_fallback()));
            }
        }
    }
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
            std::filesystem::path(helix::test::unique_temp_dir("helix-resolve-path-init"));
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
// is exactly what a power cut during first-boot migration would freeze in
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
        o << json{{"config_version", kCurrentConfigVersion},
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
    REQUIRE(on_disk.value("config_version", 0) == kCurrentConfigVersion);
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
    REQUIRE(helix::test::staging_files_beside(cfg.string()) == 0);
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
        o << json{{"config_version", kCurrentConfigVersion},
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

// ============================================================================
// Invalid UTF-8 must not cost the user their settings (#1493).
//
// nlohmann's default error handler is error_handler_t::strict: dump() throws
// json::type_error.316 on a string holding a byte no UTF-8 decoder accepts.
// The strings that reach settings.json are the ones nothing validates — SSIDs,
// printer and tool names, file names — so the document has to stay writable
// with the offending bytes replaced, not be dropped whole.
// ============================================================================

TEST_CASE("Config::save() persists a document holding invalid UTF-8",
          "[core][config][save][utf8]") {
    BackupSandbox env("save_invalid_utf8");
    ConfigDirEnvGuard config_dir_guard;
    ConfigTestModeGuard test_mode(true);

    const std::string cfg = env.config_path();

    Config test_config;
    test_config.init(cfg);
    test_config.set<int>("/brightness", 42);
    // 0xFF is not a legal UTF-8 lead byte in any position.
    test_config.set<std::string>("/printers/default/printer_name",
                                 std::string("Voron \xff\xfe 2.4"));

    REQUIRE(test_config.save());

    std::ifstream in(cfg);
    REQUIRE(in.good());
    json on_disk = json::parse(in);

    // Every other setting the user changed is on disk.
    CHECK(on_disk["brightness"] == 42);
    // The offending string is stored with the bad bytes replaced, and nothing
    // that could be preserved was thrown away.
    REQUIRE(on_disk["printers"]["default"].contains("printer_name"));
    const std::string name = on_disk["printers"]["default"]["printer_name"].get<std::string>();
    CHECK(name.rfind("Voron ", 0) == 0);
    CHECK(name.find(" 2.4") != std::string::npos);
    CHECK(name.find('\xff') == std::string::npos);
}
