// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Versioned config migrations. Each migration is a static function in an anonymous
// namespace, so they are tested through the public Config::init() path, which runs
// run_versioned_migrations() on an existing on-disk config. A sandboxed
// HELIX_CONFIG_DIR keeps backup-restore search paths inside the temp dir so nothing
// leaks from the host config.

#include "config.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/unique_temp_dir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class MigrationFixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

  public:
    MigrationFixture() {
        temp_dir = helix::test::unique_temp_dir("helix_migration_test");
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);
        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);
        config_path = temp_dir + "/settings.json";
    }

    ~MigrationFixture() {
        fs::remove_all(temp_dir);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        config.clear_path();
    }

    // Write a settings.json with the given JSON contents, then init Config from it
    // so the real versioned migrations run.
    void write_and_init(const json& contents) {
        std::ofstream f(config_path);
        f << contents.dump(2);
        f.close();
        config.init(config_path);
    }
};

json stale_display() {
    return {{"sleep_backlight_off", false}, {"hardware_blank", 0}};
}

} // namespace

// ============================================================================
// v12 -> v13 and v14 -> v15: the stale AD5X display pair
//
// Both repair sleep_backlight_off=false with hardware_blank=0, starting one version
// below the migration under test. Starting from v12 runs v13 and then v15, and v15
// would repair a config v13 left alone, so the config value alone cannot say which
// migration did the repair: each repair logs its own tag, and the case asserts on that.
// ============================================================================

TEST_CASE_METHOD(MigrationFixture, "both AD5X sleep migrations repair the same stale pair",
                 "[config][migration]") {
    // The version below each migration. From 12 the v13 repair lands first and v15 finds
    // nothing left to do; from 14 only v15 runs.
    const int from_version = GENERATE(12, 14);
    CAPTURE(from_version);
    const std::string repairing_tag =
        from_version == 12 ? "Migration v13: restored" : "Migration v15: restored";
    const std::string idle_tag =
        from_version == 12 ? "Migration v15: restored" : "Migration v13: restored";
    helix::TextLogCapture log;

    SECTION("preset ad5x") {
        write_and_init(
            {{"config_version", from_version}, {"preset", "ad5x"}, {"display", stale_display()}});
        CHECK(config.get<bool>("/display/sleep_backlight_off", false) == true);
        CHECK(config.get<int>("/display/hardware_blank", -1) == 1);
        CHECK(log.contains(repairing_tag));
        CHECK_FALSE(log.contains(idle_tag));
    }

    SECTION("saved printer of type FlashForge Adventurer 5X") {
        write_and_init({{"config_version", from_version},
                        {"printers", {{"p1", {{"type", "FlashForge Adventurer 5X"}}}}},
                        {"display", stale_display()}});
        CHECK(config.get<bool>("/display/sleep_backlight_off", false) == true);
        CHECK(config.get<int>("/display/hardware_blank", -1) == 1);
        CHECK(log.contains(repairing_tag));
        CHECK_FALSE(log.contains(idle_tag));
    }

    SECTION("preset ad5m is untouched") {
        write_and_init(
            {{"config_version", from_version}, {"preset", "ad5m"}, {"display", stale_display()}});
        CHECK(config.get<bool>("/display/sleep_backlight_off", true) == false);
        CHECK(config.get<int>("/display/hardware_blank", -1) == 0);
    }

    SECTION("an AD5X config already repaired is untouched") {
        write_and_init({{"config_version", from_version},
                        {"preset", "ad5x"},
                        {"display", {{"sleep_backlight_off", true}, {"hardware_blank", 2}}}});
        CHECK(config.get<bool>("/display/sleep_backlight_off", false) == true);
        CHECK(config.get<int>("/display/hardware_blank", -1) == 2);
    }
}

// ============================================================================
// v17 -> v18: touch-calibration recheck
// ============================================================================

TEST_CASE_METHOD(MigrationFixture,
                 "Config migration v18: sets recheck_pending=true from a v17 config",
                 "[config][migration]") {
    json v17 = {{"config_version", 17},
                {"active_printer_id", "default"},
                {"input", {{"calibration", {{"valid", true}, {"a", 1.0}, {"e", 1.0}}}}}};
    write_and_init(v17);

    REQUIRE(config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE(config.get<bool>("/input/calibration/recheck_pending", false) == true);
    // Existing calibration data is preserved (not clobbered).
    REQUIRE(config.get<bool>("/input/calibration/valid", false) == true);
}

TEST_CASE_METHOD(MigrationFixture,
                 "Config migration v18: does not throw when input section is absent",
                 "[config][migration]") {
    json v17 = {{"config_version", 17}, {"active_printer_id", "default"}};
    REQUIRE_NOTHROW(write_and_init(v17));

    REQUIRE(config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE(config.get<bool>("/input/calibration/recheck_pending", false) == true);
}

TEST_CASE_METHOD(MigrationFixture,
                 "Config migration v18: does not throw when calibration is absent",
                 "[config][migration]") {
    json v17 = {{"config_version", 17},
                {"active_printer_id", "default"},
                {"input", {{"touch_device", "/dev/input/event0"}}}};
    REQUIRE_NOTHROW(write_and_init(v17));

    REQUIRE(config.get<bool>("/input/calibration/recheck_pending", false) == true);
    // Sibling input data preserved.
    REQUIRE(config.get<std::string>("/input/touch_device", "") == "/dev/input/event0");
}

TEST_CASE_METHOD(MigrationFixture,
                 "Config migration v18: is idempotent — already-v18 config is untouched",
                 "[config][migration]") {
    // A config already at the current version must not have the flag flipped on.
    json v18 = {{"config_version", kCurrentConfigVersion},
                {"active_printer_id", "default"},
                {"input", {{"calibration", {{"valid", true}, {"recheck_pending", false}}}}}};
    write_and_init(v18);

    REQUIRE(config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE(config.get<bool>("/input/calibration/recheck_pending", true) == false);
}

TEST_CASE_METHOD(MigrationFixture,
                 "Config migration v18: a seed with a printer node keeps its calibration",
                 "[config][migration]") {
    // seed_from_moonraker_detection's C path: a preset's input/display blocks
    // plus a moonraker_host under printers/default, and no config_version.
    json seed = {{"input", {{"calibration", {{"valid", true}, {"a", 1.66}, {"e", 1.76}}}}},
                 {"display", {{"rotate", 180}}},
                 {"active_printer_id", "default"},
                 {"printers", {{"default", {{"moonraker_host", "127.0.0.1"}}}}}};
    write_and_init(seed);

    REQUIRE(config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE_FALSE(config.get<bool>("/input/calibration/recheck_pending", false));
    REQUIRE(config.get<bool>("/input/calibration/valid", false));
}

TEST_CASE_METHOD(MigrationFixture, "Config migration v18: a packaged preset keeps its calibration",
                 "[config][migration]") {
    // A preset shipped as the release tarball's settings.json, on a fresh
    // install with no rolling backup to restore.
    json preset = {{"preset", "artillery-m1-pro"},
                   {"wizard_completed", false},
                   {"input", {{"calibration", {{"valid", true}, {"a", 1.64}, {"e", 1.83}}}}},
                   {"printer", {{"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}}}}};
    write_and_init(preset);

    REQUIRE(config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE_FALSE(config.get<bool>("/input/calibration/recheck_pending", false));
    REQUIRE(config.get<bool>("/input/calibration/valid", false));
}

TEST_CASE_METHOD(MigrationFixture, "Config migration v18: a full preset seed keeps its calibration",
                 "[config][migration]") {
    // seed_full_preset_for_printer's B path: the device blocks, the preset's
    // printer block under printers/default, and the root preset marker.
    json seed = {
        {"input", {{"calibration", {{"valid", true}, {"a", 1.66}, {"e", 1.76}}}}},
        {"display", {{"rotate", 180}}},
        {"active_printer_id", "default"},
        {"preset", "sovol_sv06_ace"},
        {"printers",
         {{"default", {{"wizard_completed", false}, {"heaters", {{"bed", "heater_bed"}}}}}}}};
    write_and_init(seed);

    REQUIRE(config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE_FALSE(config.get<bool>("/input/calibration/recheck_pending", false));
    REQUIRE(config.get<bool>("/input/calibration/valid", false));
}

TEST_CASE_METHOD(MigrationFixture,
                 "Config migration v18: a versionless user config with a finished wizard is "
                 "rechecked",
                 "[config][migration]") {
    // v0.9.10 and earlier wrote no config_version: a single /printer, the
    // wizard flag at the root, and the wizard's affine under /display.
    json user = {{"wizard_completed", true},
                 {"printer", {{"moonraker_host", "192.168.1.50"}}},
                 {"display", {{"calibration", {{"valid", true}, {"a", 1.5}, {"e", 1.7}}}}}};
    write_and_init(user);

    REQUIRE(config.get<int>("/config_version") == kCurrentConfigVersion);
    REQUIRE(config.get<bool>("/input/calibration/valid", false));
    REQUIRE(config.get<bool>("/input/calibration/recheck_pending", false));
}
