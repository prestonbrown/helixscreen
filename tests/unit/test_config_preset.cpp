// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_test_access.h"
#include "config.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace helix {
class PresetConfigFixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string saved_config_dir_;
    std::string saved_data_dir_;
    bool had_config_dir_ = false;
    bool had_data_dir_ = false;
    bool did_setup_ = false;
    bool torn_down_ = false;

    // Catch2 invokes the fixture ctor/dtor, NOT SetUp()/TearDown(). Tests call
    // those manually; a dtor-driven TearDown() is the safety net so a test that
    // forgets to call TearDown() still restores HELIX_DATA_DIR/HELIX_CONFIG_DIR
    // (a leak there breaks find_readable() for every later test in the process —
    // 127 printer_detector failures, macro/grid/theme cascades). Idempotent via
    // did_setup_/torn_down_ so an explicit TearDown() + the dtor don't double-run.
    ~PresetConfigFixture() {
        TearDown();
    }

    void SetUp() {
        did_setup_ = true;
        // Per-process sandbox. The name must not be shared: the printer_database
        // symlink below is created inside it, and a copy_file fallback onto an
        // existing symlink writes THROUGH it — into whichever checkout that
        // symlink names. Two suites running from different worktrees at once
        // would otherwise rewrite each other's tracked assets/config copy.
        temp_dir = (fs::temp_directory_path() / ("helix_preset_test." + std::to_string(::getpid())))
                       .string();
        fs::create_directories(temp_dir + "/presets");
        fs::create_directories(temp_dir + "/assets/config/presets");

        // apply_preset_file resolves preset path via helix::find_readable, which
        // checks $HELIX_CONFIG_DIR first then falls back to
        // $HELIX_DATA_DIR/assets/config/. Point both env vars at temp_dir so
        // find_readable lands inside our sandbox.
        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        if (const char* prev = std::getenv("HELIX_DATA_DIR")) {
            saved_data_dir_ = prev;
            had_data_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);
        setenv("HELIX_DATA_DIR", temp_dir.c_str(), 1);

        // Ensure the shipped printer_database.json is reachable from the
        // sandboxed HELIX_DATA_DIR so DB-aware tests (e.g. preset → printer
        // type lookup) continue to work.
        std::error_code ec;
        fs::path real_db = fs::current_path() / "assets" / "config" / "printer_database.json";
        if (fs::exists(real_db)) {
            fs::path linked = fs::path(temp_dir) / "assets" / "config" / "printer_database.json";
            fs::create_symlink(real_db, linked, ec);
            if (ec) {
                fs::copy_file(real_db, linked, fs::copy_options::overwrite_existing, ec);
            }
        }

        // Set config path so apply_preset_file can find presets/ relative to it
        ConfigTestAccess::path(config) = temp_dir + "/settings.json";

        // Set active printer ID so df() returns the correct prefix
        ConfigTestAccess::active_printer_id(config) = "default";

        // Initialize with a v4 multi-printer structure
        ConfigTestAccess::data(config) = {
            {"preset", "ad5m"},
            {"language", "de"},
            {"active_printer_id", "default"},
            {"display", {{"animations_enabled", false}}},
            {"printers",
             {{"default",
               {{"moonraker_host", "127.0.0.1"},
                {"moonraker_port", 7125},
                {"printer_name", "My Printer"},
                {"wizard_completed", false},
                {"fans", {{"hotend", "heater_fan heat_fan"}, {"part", "fan_generic fanM106"}}},
                {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                {"hardware", {{"expected", {"heater_bed", "extruder"}}}}}}}}};
    }

    void TearDown() {
        if (!did_setup_ || torn_down_) {
            return;
        }
        torn_down_ = true;
        // TearDown runs from the destructor, which a failing REQUIRE reaches while
        // unwinding. The throwing overload would terminate the process there and
        // bury the assertion that actually failed.
        std::error_code rm_ec;
        fs::remove_all(temp_dir, rm_ec);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        if (had_data_dir_) {
            setenv("HELIX_DATA_DIR", saved_data_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_DATA_DIR");
        }
    }

    void write_preset(const std::string& name, const nlohmann::json& preset_json) {
        std::string path = temp_dir + "/presets/" + name + ".json";
        std::ofstream f(path);
        f << preset_json.dump(2);
    }

    /// Write the preset only to the read-only seed bundle location.
    /// On a fresh install, the writable dir has no presets/ subtree at all —
    /// install tarballs land them under <install>/assets/config/presets/.
    void write_seed_preset(const std::string& name, const nlohmann::json& preset_json) {
        std::string path = temp_dir + "/assets/config/presets/" + name + ".json";
        std::ofstream f(path);
        f << preset_json.dump(2);
    }

    /// Copy a preset HelixScreen actually ships into the sandbox, so a test can
    /// assert on the shipped data rather than on a hand-written stand-in.
    /// Runs from the repo root, like every other asset-reading test.
    void copy_shipped_preset(const std::string& name) {
        fs::path shipped = fs::current_path() / "assets" / "config" / "presets" / (name + ".json");
        INFO("reading " << shipped.string() << " (tests must run from the repo root)");
        REQUIRE(fs::exists(shipped));
        std::error_code ec;
        fs::copy_file(shipped, fs::path(temp_dir) / "presets" / (name + ".json"),
                      fs::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);
    }

    json& printer_data() {
        return ConfigTestAccess::data(config)["printers"]["default"];
    }

    json& data() {
        return ConfigTestAccess::data(config);
    }
};
} // namespace helix

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file merges hardware keys into active printer",
                 "[config][preset]") {
    SetUp();

    json preset = {{"printer",
                    {{"fans",
                      {{"hotend", "heater_fan heat_fan"},
                       {"part", "fan_generic fanM106"},
                       {"chamber", "fan_generic chamber_fan"},
                       {"exhaust", "fan_generic exhaust_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                     {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                     {"leds", {{"strip", "neopixel led_strip"}}},
                     {"hardware",
                      {{"expected",
                        {"heater_bed", "extruder", "neopixel led_strip", "fan_generic chamber_fan",
                         "fan_generic exhaust_fan"}}}},
                     {"filament_sensors", {{"runout", "filament_switch_sensor runout"}}},
                     {"default_macros", {"START_PRINT", "END_PRINT"}}}}};
    write_preset("ad5m_pro", preset);

    REQUIRE(config.apply_preset_file("ad5m_pro") == true);

    auto& pd = printer_data();
    REQUIRE(pd["fans"].contains("chamber"));
    REQUIRE(pd["fans"].contains("exhaust"));
    REQUIRE(pd["leds"].contains("strip"));
    REQUIRE(pd["hardware"]["expected"].size() == 5);
    REQUIRE(pd.contains("filament_sensors"));
    REQUIRE(pd.contains("default_macros"));

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::apply_preset_file preserves non-hardware settings",
                 "[config][preset]") {
    SetUp();

    json preset = {{"printer",
                    {{"fans", {{"hotend", "heater_fan heat_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}}},
                     {"temp_sensors", {{"bed", "heater_bed"}}}}}};
    write_preset("minimal", preset);

    config.apply_preset_file("minimal");

    REQUIRE(data()["language"] == "de");
    auto& pd = printer_data();
    REQUIRE(pd["moonraker_host"] == "127.0.0.1");
    REQUIRE(pd["moonraker_port"] == 7125);
    REQUIRE(pd["printer_name"] == "My Printer");
    REQUIRE(pd["wizard_completed"] == false);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file post-wizard seeds absent role keys from preset",
                 "[config][preset]") {
    SetUp();

    // Machine provisioned before the ForgeX preset landed: wizard done, generic
    // ad5m fan names in config, no chamber/exhaust/aux/led keys.
    printer_data()["wizard_completed"] = true;
    copy_shipped_preset("ad5m_pro_forgex");

    REQUIRE(config.apply_preset_file("ad5m_pro_forgex") == true);

    auto& pd = printer_data();
    REQUIRE(pd["fans"]["chamber"] == "fan_generic chamber_fan");
    REQUIRE(pd["fans"]["exhaust"] == "fan_generic external_fan");
    REQUIRE(pd["fans"]["aux"] == "fan_generic internal_fan");
    // leds/strip is not seeded post-wizard: a preset LED name the machine does not
    // have would raise "Configured LED strip not found", which outranks the toast
    // this migration exists to silence.
    REQUIRE_FALSE(pd.contains("leds"));

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file post-wizard never overwrites a held role value",
                 "[config][preset]") {
    SetUp();

    printer_data()["wizard_completed"] = true;
    // The user (or a heal) holds a chamber mapping; the preset disagrees.
    printer_data()["fans"]["chamber"] = "fan_generic user_pick";

    json preset = {
        {"printer",
         {{"fans",
           {{"chamber", "fan_generic chamber_fan"}, {"exhaust", "fan_generic external_fan"}}}}}};
    write_preset("ad5m_pro", preset);

    REQUIRE(config.apply_preset_file("ad5m_pro") == true);
    REQUIRE(printer_data()["fans"]["chamber"] == "fan_generic user_pick");
    // Seeding still reaches keys nobody holds.
    REQUIRE(printer_data()["fans"]["exhaust"] == "fan_generic external_fan");

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file post-wizard unions hardware/expected without AMS "
                 "keywords",
                 "[config][preset]") {
    SetUp();

    printer_data()["wizard_completed"] = true;

    json preset = {{"printer",
                    {{"hardware",
                      {{"expected",
                        {"heater_bed", "extruder", "fan_generic chamber_fan", "led chamber_light",
                         "controller_fan driver_fan", "AFC", "mmu", "toolchanger", "ace"}}}}}}};
    write_preset("expected_preset", preset);

    REQUIRE(config.apply_preset_file("expected_preset") == true);

    auto& expected = printer_data()["hardware"]["expected"];
    // Scaffold holds heater_bed + extruder; the union adds the rest, no duplicates.
    REQUIRE(expected.size() == 5);
    for (const auto& entry : expected) {
        std::string name = entry.get<std::string>();
        REQUIRE(name != "AFC");
        REQUIRE(name != "mmu");
        REQUIRE(name != "toolchanger");
        REQUIRE(name != "ace");
    }

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file post-wizard leaves leds/strip alone",
                 "[config][preset]") {
    SetUp();

    printer_data()["wizard_completed"] = true;
    printer_data().erase("leds");

    // The fan role pins that the migration ran at all, so the LED absence below is
    // evidence rather than a vacuous pass on a preset that was never applied.
    json preset = {{"printer",
                    {{"fans", {{"chamber", "fan_generic chamber_fan"}}},
                     {"leds", {{"strip", "led chamber_light"}}}}}};
    write_preset("led_preset", preset);

    REQUIRE(config.apply_preset_file("led_preset") == true);
    REQUIRE(printer_data()["fans"]["chamber"] == "fan_generic chamber_fan");
    REQUIRE_FALSE(printer_data().contains("leds"));

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::apply_preset_file returns false for missing file",
                 "[config][preset]") {
    SetUp();

    REQUIRE(config.apply_preset_file("nonexistent_preset") == false);
    REQUIRE(printer_data()["fans"].size() == 2);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file preserves scaffolded keys not mentioned in preset",
                 "[config][preset]") {
    SetUp();

    // Add leds to initial (scaffolded) config
    printer_data()["leds"] = {{"strip", "neopixel led_strip"}};

    // Write preset WITHOUT leds
    json preset = {{"printer",
                    {{"fans", {{"hotend", "heater_fan heat_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}}},
                     {"temp_sensors", {{"bed", "heater_bed"}}}}}};
    write_preset("no_leds", preset);

    config.apply_preset_file("no_leds");

    // Deep-merge leaves untouched subtrees in place. The wizard_completed guard
    // ensures this only runs pre-wizard, so preserving scaffolded defaults is safe
    // and avoids dropping values like moonraker_host that aren't part of every preset.
    REQUIRE(printer_data().contains("leds"));
    REQUIRE(printer_data()["leds"]["strip"] == "neopixel led_strip");

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file does not overwrite user-entered moonraker host",
                 "[config][preset][regression]") {
    // Regression for the 127.0.0.1 persistence bug: the add-printer wizard's
    // Connection step saves the user's real Moonraker IP BEFORE the
    // Printer-Identify step applies the model preset. Preset "printer" blocks
    // hardcode "moonraker_host":"127.0.0.1", so merge_patch was clobbering the
    // user's entered IP — on next restart HelixScreen connected to localhost.
    // Connection settings are deployment-specific and must survive preset merge,
    // while all non-connection keys must still merge.
    SetUp();

    // Connection wizard step already saved the user's real host/port.
    printer_data()["moonraker_host"] = "192.168.1.67";
    printer_data()["moonraker_port"] = 7125;
    printer_data()["wizard_completed"] = false;

    // Preset carries the offending 127.0.0.1 plus genuine hardware keys.
    json preset = {{"printer",
                    {{"moonraker_host", "127.0.0.1"},
                     {"moonraker_port", 7125},
                     {"fans", {{"chamber", "fan_generic chamber_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}}}}}};
    write_preset("clobber_preset", preset);

    REQUIRE(config.apply_preset_file("clobber_preset") == true);

    auto& pd = printer_data();
    // The user-entered IP MUST survive the preset merge.
    REQUIRE(pd["moonraker_host"] == "192.168.1.67");
    // Non-connection keys still merge in.
    REQUIRE(pd["fans"]["chamber"] == "fan_generic chamber_fan");
    REQUIRE(pd["heaters"]["bed"] == "heater_bed");
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file does not overwrite connection settings, merges rest",
                 "[config][preset]") {
    SetUp();

    // Scaffolded with empty moonraker_host (fresh install before Connection step).
    printer_data()["moonraker_host"] = "";
    printer_data().erase("moonraker_port");

    json preset = {{"printer",
                    {{"moonraker_host", "127.0.0.1"},
                     {"moonraker_port", 7125},
                     {"input",
                      {{"scroll_limit", 10},
                       {"scroll_throw", 25},
                       {"jitter_threshold", 5},
                       {"scroll_guard", true}}},
                     {"heaters", {{"bed", "heater_bed"}}}}}};
    write_preset("network_preset", preset);

    REQUIRE(config.apply_preset_file("network_preset") == true);

    auto& pd = printer_data();
    // Connection settings are NOT seeded from the preset — the Connection wizard
    // step owns them. A scaffolded-empty host stays empty.
    REQUIRE(pd["moonraker_host"] == "");
    REQUIRE_FALSE(pd.contains("moonraker_port"));
    // Everything else still merges.
    REQUIRE(pd["input"]["scroll_limit"] == 10);
    REQUIRE(pd["input"]["scroll_throw"] == 25);
    REQUIRE(pd["input"]["jitter_threshold"] == 5);
    REQUIRE(pd["input"]["scroll_guard"] == true);
    REQUIRE(pd["heaters"]["bed"] == "heater_bed");

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file merges top-level input block into device-level input",
                 "[config][preset]") {
    SetUp();

    // Fresh, pre-wizard. Preset carries a TOP-LEVEL "input" block (device-level,
    // not under "printer"). This seeds touch calibration, which is read from the
    // top-level /input/calibration/* path (touch_calibration_wrapper.cpp).
    json preset = {{"printer", {{"heaters", {{"bed", "heater_bed"}}}}},
                   {"input",
                    {{"calibration",
                      {{"valid", true},
                       {"a", 1.66},
                       {"b", 0.0},
                       {"c", 0.0},
                       {"d", 0.0},
                       {"e", 1.76},
                       {"f", 0.0}}}}}};
    write_preset("input_preset", preset);

    REQUIRE(config.apply_preset_file("input_preset") == true);

    REQUIRE(config.get<bool>("/input/calibration/valid", false) == true);
    REQUIRE(config.get<double>("/input/calibration/a", 0.0) == Catch::Approx(1.66));
    REQUIRE(config.get<double>("/input/calibration/e", 0.0) == Catch::Approx(1.76));

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file sets printer type from database preset lookup",
                 "[config][preset]") {
    SetUp();

    // Ensure no prior type is set on this scaffolded printer
    printer_data().erase("type");

    // Use a preset name that exists in the shipped printer_database.json so the
    // DB lookup can resolve "ad5x" → "FlashForge Adventurer 5X".
    json preset = {{"printer", {{"heaters", {{"bed", "heater_bed"}}}}}};
    write_preset("ad5x", preset);

    REQUIRE(config.apply_preset_file("ad5x") == true);

    REQUIRE(printer_data().contains("type"));
    REQUIRE(printer_data()["type"] == "FlashForge Adventurer 5X");

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file does not overwrite existing printer type",
                 "[config][preset]") {
    SetUp();

    // User/wizard already picked a printer type explicitly
    printer_data()["type"] = "Custom Voron";

    json preset = {{"printer", {{"heaters", {{"bed", "heater_bed"}}}}}};
    write_preset("ad5x", preset);

    REQUIRE(config.apply_preset_file("ad5x") == true);

    // Should preserve the user-selected type even though the preset maps to a DB entry
    REQUIRE(printer_data()["type"] == "Custom Voron");

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file tolerates preset with no database mapping",
                 "[config][preset]") {
    SetUp();

    printer_data().erase("type");

    json preset = {{"printer", {{"heaters", {{"bed", "heater_bed"}}}}}};
    write_preset("not_a_real_preset_xyz", preset);

    REQUIRE(config.apply_preset_file("not_a_real_preset_xyz") == true);

    // No DB match → type remains unset; merge still succeeds
    REQUIRE_FALSE(printer_data().contains("type"));
    REQUIRE(printer_data()["heaters"]["bed"] == "heater_bed");

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file merges display settings from preset",
                 "[config][preset]") {
    SetUp();

    json preset = {{"printer",
                    {{"fans", {{"hotend", "heater_fan heat_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}}},
                     {"temp_sensors", {{"bed", "heater_bed"}}}}},
                   {"display",
                    {{"backlight_enable_ioctl", true},
                     {"hardware_blank", true},
                     {"sleep_backlight_off", true}}}};
    write_preset("display_preset", preset);

    config.apply_preset_file("display_preset");

    REQUIRE(data()["display"]["backlight_enable_ioctl"] == true);
    REQUIRE(data()["display"]["hardware_blank"] == true);
    REQUIRE(data()["display"]["sleep_backlight_off"] == true);
    // Existing display setting should be preserved
    REQUIRE(data()["display"]["animations_enabled"] == false);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file finds preset shipped only in read-only seed bundle",
                 "[config][preset][regression]") {
    // Fresh install: install tarball lands presets under
    // <install>/assets/config/presets/, NOT under the writable user config dir.
    // Without find_readable() fallback, apply_preset_file misses the preset and
    // the wizard runs every hardware step on a known printer (forgex AD5M Pro
    // first-install bug, 2026-04).
    SetUp();

    json preset = {
        {"printer",
         {{"fans",
           {{"hotend", "heater_fan hotend_fan"},
            {"part", "fan"},
            {"chamber", "fan_generic chamber_fan"},
            {"exhaust", "fan_generic external_fan"},
            {"aux", "fan_generic internal_fan"}}},
          {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
          {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
          {"leds", {{"strip", "led chamber_light"}}},
          {"hardware",
           {{"expected",
             {"heater_bed", "extruder", "fan", "heater_fan hotend_fan", "fan_generic internal_fan",
              "fan_generic chamber_fan", "fan_generic external_fan", "led chamber_light",
              "controller_fan driver_fan", "filament_switch_sensor e0_sensor"}}}}}}};

    write_seed_preset("ad5m_pro_forgex", preset);
    REQUIRE_FALSE(fs::exists(temp_dir + "/presets/ad5m_pro_forgex.json"));
    REQUIRE(fs::exists(temp_dir + "/assets/config/presets/ad5m_pro_forgex.json"));

    REQUIRE(config.apply_preset_file("ad5m_pro_forgex") == true);

    auto& pd = printer_data();
    REQUIRE(pd["fans"]["aux"] == "fan_generic internal_fan");
    REQUIRE(pd["fans"]["chamber"] == "fan_generic chamber_fan");
    REQUIRE(pd["leds"]["strip"] == "led chamber_light");
    REQUIRE(pd["hardware"]["expected"].size() == 10);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file prefers writable preset over seed bundle",
                 "[config][preset][regression]") {
    // If a user has a writable override at <config>/presets/X.json, it should
    // win over the shipped seed at <data>/assets/config/presets/X.json. This
    // matches find_readable's user-dir-first ordering.
    SetUp();

    json seed_preset = {
        {"printer", {{"fans", {{"hotend", "from_seed"}}}, {"heaters", {{"bed", "heater_bed"}}}}}};
    json user_preset = {
        {"printer", {{"fans", {{"hotend", "from_user"}}}, {"heaters", {{"bed", "heater_bed"}}}}}};

    write_seed_preset("override_preset", seed_preset);
    write_preset("override_preset", user_preset);

    REQUIRE(config.apply_preset_file("override_preset") == true);
    REQUIRE(printer_data()["fans"]["hotend"] == "from_user");

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::clear_preset removes the marker",
                 "[config][preset]") {
    SetUp();

    auto& cfg = config;
    cfg.set_preset("qidi_q2");
    REQUIRE(cfg.has_preset());
    REQUIRE(cfg.get_preset() == "qidi_q2");

    cfg.clear_preset();
    REQUIRE_FALSE(cfg.has_preset());
    REQUIRE(cfg.get_preset().empty());

    cfg.clear_preset(); // idempotent
    REQUIRE_FALSE(cfg.has_preset());

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::apply_preset_file integrates with detection flow",
                 "[config][preset][integration]") {
    SetUp();

    // Simulate what auto_detect_and_save does: set preset then apply
    write_preset(
        "ad5m_pro",
        {{"preset", "ad5m_pro"},
         {"printer",
          {{"fans",
            {{"chamber", "fan_generic chamber_fan"},
             {"exhaust", "fan_generic external_fan"},
             {"hotend", "heater_fan heat_fan"},
             {"part", "fan_generic fanM106"},
             {"aux", "fan_generic internal_fan"}}},
           {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
           {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
           {"leds", {{"strip", "led chamber_light"}}},
           {"hardware",
            {{"expected",
              {"heater_bed", "extruder", "fan_generic chamber_fan", "led chamber_light"}}}}}}});

    // Simulate detection flow
    config.set_preset("ad5m_pro");
    bool applied = config.apply_preset_file("ad5m_pro");
    REQUIRE(applied == true);

    // Verify preset name was updated
    REQUIRE(config.get_preset() == "ad5m_pro");

    // Verify hardware was merged
    REQUIRE(printer_data()["fans"].contains("chamber"));
    REQUIRE(printer_data().contains("leds"));

    // Verify non-hardware preserved
    REQUIRE(printer_data()["moonraker_host"] == "127.0.0.1");

    TearDown();
}

// ============================================================================
// Device-level display/input seeding is scoped to on-device installs
//
// A preset's top-level "display" and "input" blocks describe the PRINTER'S OWN
// PANEL — rotation, white balance, the touch calibration matrix. They are only
// correct when HelixScreen is the thing driving that panel. A separate host (a
// Pi with its own touchscreen, a desktop) that merely talks to the printer over
// the network must not inherit them.
// ============================================================================

/// TEST-NET-1 (RFC 5737). Guaranteed never assigned to a real interface, so
/// is_moonraker_on_same_host()'s getifaddrs() scan can't accidentally match it
/// on whatever machine CI runs on.
static constexpr const char* REMOTE_TEST_HOST = "192.0.2.10";

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file skips device display/input for a remote printer",
                 "[config][preset][device-scope]") {
    SetUp();

    // HelixScreen is running somewhere else and talking to this printer over the
    // network — e.g. a Pi with its own touchscreen that detected a Centauri Carbon.
    printer_data()["moonraker_host"] = REMOTE_TEST_HOST;

    json preset = {
        {"printer", {{"heaters", {{"bed", "heater_bed"}}}}},
        {"display", {{"rotate", 270}, {"rotation_probed", true}, {"r_gain", 90}}},
        {"input",
         {{"jitter_threshold", 7}, {"calibration", {{"valid", true}, {"a", 1.66}, {"e", 1.76}}}}}};
    write_preset("remote_preset", preset);

    REQUIRE(config.apply_preset_file("remote_preset") == true);

    // The printer's own hardware still merges — that block is about the printer.
    REQUIRE(printer_data()["heaters"]["bed"] == "heater_bed");

    // The host's screen is untouched: no rotation, no probe suppression, no gains.
    REQUIRE_FALSE(data()["display"].contains("rotate"));
    REQUIRE_FALSE(data()["display"].contains("rotation_probed"));
    REQUIRE_FALSE(data()["display"].contains("r_gain"));
    // Keys the host already had survive.
    REQUIRE(data()["display"]["animations_enabled"] == false);

    // Touch calibration from a foreign panel would make this screen unusable.
    REQUIRE(config.get<bool>("/input/calibration/valid", false) == false);
    REQUIRE_FALSE(data().contains("input"));

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file seeds device display/input for a loopback printer",
                 "[config][preset][device-scope]") {
    SetUp();

    // On-device install (K1, CC1, AD5M, ...): Moonraker is on this same box, so the
    // preset's panel settings ARE this panel's settings.
    printer_data()["moonraker_host"] = "127.0.0.1";

    json preset = {{"printer", {{"heaters", {{"bed", "heater_bed"}}}}},
                   {"display", {{"rotate", 270}, {"rotation_probed", true}}},
                   {"input", {{"jitter_threshold", 7}}}};
    write_preset("device_preset", preset);

    REQUIRE(config.apply_preset_file("device_preset") == true);

    REQUIRE(data()["display"]["rotate"] == 270);
    REQUIRE(data()["display"]["rotation_probed"] == true);
    REQUIRE(data()["display"]["animations_enabled"] == false); // preserved
    REQUIRE(config.get<int>("/input/jitter_threshold", 0) == 7);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file seeds device display/input when no host is set yet",
                 "[config][preset][device-scope]") {
    SetUp();

    // The factory tarball bakes a preset straight into settings.json and no preset
    // file carries moonraker_host, so first boot on a K1/K2/U1 can reach the merge
    // with the key entirely absent. That MUST still seed the panel — an unset host
    // means "nobody has told us about a remote printer", not "we are remote".
    printer_data().erase("moonraker_host");

    json preset = {{"printer", {{"heaters", {{"bed", "heater_bed"}}}}},
                   {"display", {{"rotate", 270}, {"rotation_probed", true}}}};
    write_preset("factory_preset", preset);

    REQUIRE(config.apply_preset_file("factory_preset") == true);

    REQUIRE(data()["display"]["rotate"] == 270);
    REQUIRE(data()["display"]["rotation_probed"] == true);

    TearDown();
}

// The k1 preset seeds a printer whose stock config leaves the aux fan and the
// chamber temperature sensor commented out. Naming absent Klipper objects costs
// the owner a hardware warning to hand-silence and a discovery-time miss on
// every pass, so the preset must claim neither. mutate-diff does not mutate
// assets/*.json, which leaves this the only guard over that data.
TEST_CASE_METHOD(PresetConfigFixture, "k1 preset claims no aux fan and no fixed chamber sensor",
                 "[config][preset][k1]") {
    SetUp();

    copy_shipped_preset("k1");
    REQUIRE(config.apply_preset_file("k1") == true);

    auto& pd = printer_data();

    // resolve_aux_fan() picks the aux slot out of discovered fans at runtime and
    // prefers a user-commandable one, so a mapping here only ever adds a claim.
    REQUIRE(pd.contains("fans"));
    REQUIRE_FALSE(pd["fans"].contains("aux"));

    // "auto" hands the chamber sensor to discovery; "" would mean "no chamber".
    REQUIRE(pd["temp_sensors"]["chamber"] == "auto");

    // The exhaust fan is a live role feeding the fan UI and stays mapped.
    REQUIRE(pd["fans"]["exhaust"] == "output_pin fan2");

    // Nothing the preset expects may name an object it no longer maps.
    for (const auto& expected : pd["hardware"]["expected"]) {
        REQUIRE(expected.get<std::string>() != "output_pin fan1");
        REQUIRE(expected.get<std::string>() != "temperature_sensor chamber_temp");
    }

    TearDown();
}

// The creator5 presets ship the four fd_ex head switches with role "none":
// FilamentSensorManager maps only e<N>_filament names to a head, so an fd_ex
// sensor with the runout role counts any empty docked head as a runout and
// pops the guidance modal. Runout stays off until verified on hardware
// (prestonbrown/helixscreen#1714). mutate-diff does not mutate assets/*.json,
// which leaves this the only guard over that data.
TEST_CASE_METHOD(PresetConfigFixture,
                 "creator5 preset ships fd_ex switches without the runout role",
                 "[config][preset][creator5]") {
    SetUp();

    copy_shipped_preset("creator5");
    REQUIRE(config.apply_preset_file("creator5") == true);

    auto& pd = printer_data();

    REQUIRE(pd.contains("filament_sensors"));
    int switches = 0;
    for (const auto& sensor : pd["filament_sensors"]["sensors"]) {
        const std::string klipper_name = sensor.at("klipper_name").get<std::string>();
        if (klipper_name.find("fd_ex") == std::string::npos) {
            continue;
        }
        ++switches;
        INFO(klipper_name << " has role " << sensor.at("role").get<std::string>());
        CHECK(sensor.at("role").get<std::string>() != "runout");
    }
    // The loop must have seen the four head switches, not an empty array.
    REQUIRE(switches == 4);

    TearDown();
}

// The Creator 5 has no chamber heater: the Pro's heated-chamber objects must
// stay out of the non-Pro preset entirely, or a Creator 5 boots expecting
// hardware it does not have and warns about it on every start.
TEST_CASE_METHOD(PresetConfigFixture, "creator5 preset carries no chamber heater",
                 "[config][preset][creator5]") {
    SetUp();

    copy_shipped_preset("creator5");
    REQUIRE(config.apply_preset_file("creator5") == true);

    auto& pd = printer_data();

    CHECK(pd["heaters"].value("chamber", "") == "");
    CHECK(pd["temp_sensors"].value("chamber", "") == "");
    CHECK(pd["fans"].value("chamber", "") == "");
    CHECK(pd["fans"].value("aux", "") == "");
    // The chamber LED is shared hardware and stays; the heater and the fan
    // suite are the Pro's.
    for (const auto& expected : pd["hardware"]["expected"]) {
        const std::string name = expected.get<std::string>();
        INFO(name);
        CHECK(name.find("chamber_heater") == std::string::npos);
        CHECK(name.find("chamber_fan") == std::string::npos);
        CHECK(name.find("chamber_loop_fan") == std::string::npos);
    }
    CHECK(pd["default_macros"].value("cooldown", "").find("chamber_heater") == std::string::npos);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "creator5_pro preset ships fd_ex switches without the runout role",
                 "[config][preset][creator5]") {
    SetUp();

    copy_shipped_preset("creator5_pro");
    REQUIRE(config.apply_preset_file("creator5_pro") == true);

    auto& pd = printer_data();

    REQUIRE(pd.contains("filament_sensors"));
    int switches = 0;
    for (const auto& sensor : pd["filament_sensors"]["sensors"]) {
        const std::string klipper_name = sensor.at("klipper_name").get<std::string>();
        if (klipper_name.find("fd_ex") == std::string::npos) {
            continue;
        }
        ++switches;
        INFO(klipper_name << " has role " << sensor.at("role").get<std::string>());
        CHECK(sensor.at("role").get<std::string>() != "runout");
    }
    // The loop must have seen the four head switches, not an empty array.
    REQUIRE(switches == 4);

    // The Pro's chamber heater is what separates it from the Creator 5; the
    // preset must keep mapping it.
    REQUIRE(pd["heaters"].value("chamber", "") == "heater_generic chamber_heater");

    TearDown();
}

// ============================================================================
// apply_preset_file post-wizard migration 2: default_macros
// ============================================================================

namespace {

constexpr const char* kStaleK2Cooldown = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\n"
                                         "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0\n"
                                         "SET_HEATER_TEMPERATURE HEATER=chamber_heater TARGET=0";

/// The cooldown text the sandboxed copy of k2.json currently ships, so these
/// tests track the shipped preset instead of a second hardcoded copy of it.
std::string current_k2_cooldown_text(const std::string& temp_dir) {
    std::ifstream in(temp_dir + "/presets/k2.json");
    json preset = json::parse(in);
    return preset["printer"]["default_macros"]["cooldown"].get<std::string>();
}

/// A filament_sensors block already at the role the shipped k2 preset wants,
/// so migration 1 finds nothing to do and these tests isolate migration 2.
json matching_k2_filament_sensors() {
    return {{"master_enabled", true},
            {"sensors",
             {{{"enabled", true},
               {"klipper_name", "filament_switch_sensor filament_sensor"},
               {"role", "runout"},
               {"type", "switch"}}}}};
}

} // namespace

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file migrates a stale default_macros.cooldown post-wizard",
                 "[config][preset][macros]") {
    SetUp();

    copy_shipped_preset("k2");
    printer_data()["wizard_completed"] = true;
    printer_data()["filament_sensors"] = matching_k2_filament_sensors();
    printer_data()["default_macros"]["cooldown"] = kStaleK2Cooldown;

    const std::string current_text = current_k2_cooldown_text(temp_dir);
    REQUIRE(current_text != kStaleK2Cooldown);

    REQUIRE(config.apply_preset_file("k2") == true);
    REQUIRE(printer_data()["default_macros"]["cooldown"] == current_text);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file never touches a customized default_macros.cooldown",
                 "[config][preset][macros]") {
    SetUp();

    copy_shipped_preset("k2");
    printer_data()["wizard_completed"] = true;
    printer_data()["filament_sensors"] = matching_k2_filament_sensors();
    printer_data()["default_macros"]["cooldown"] = "MY_CUSTOM_COOLDOWN_MACRO";

    config.apply_preset_file("k2");

    REQUIRE(printer_data()["default_macros"]["cooldown"] == "MY_CUSTOM_COOLDOWN_MACRO");

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file's default_macros migration is idempotent",
                 "[config][preset][macros]") {
    SetUp();

    copy_shipped_preset("k2");
    printer_data()["wizard_completed"] = true;
    printer_data()["filament_sensors"] = matching_k2_filament_sensors();
    printer_data()["default_macros"]["cooldown"] = kStaleK2Cooldown;

    const std::string current_text = current_k2_cooldown_text(temp_dir);

    REQUIRE(config.apply_preset_file("k2") == true);
    REQUIRE(printer_data()["default_macros"]["cooldown"] == current_text);

    // Second run: the stored text now equals the current one, not the stale
    // one, so there is nothing left to migrate.
    REQUIRE(config.apply_preset_file("k2") == false);
    REQUIRE(printer_data()["default_macros"]["cooldown"] == current_text);

    TearDown();
}
