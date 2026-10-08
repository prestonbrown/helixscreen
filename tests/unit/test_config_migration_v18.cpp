// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v17 -> v18 config migration (post-#943 touch-calibration recheck).
// The migration is a static function in an anonymous namespace, so it is tested
// through the public Config::init() path which runs run_versioned_migrations() on
// an existing on-disk config. A sandboxed HELIX_CONFIG_DIR keeps backup-restore
// search paths inside the temp dir so nothing leaks from the host config.

#include "app_constants.h"
#include "config.h"

#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class MigrationV18Fixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    void SetUp() {
        temp_dir = (fs::temp_directory_path() / "helix_migration_v18_test").string();
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        // Sandbox backup-restore search paths into the temp dir.
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);

        config_path = temp_dir + "/settings.json";
    }

    void TearDown() {
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

    // The installer's marker for a packaged config it kept: init() then never
    // swaps a versionless document for a rolling backup another case wrote.
    void mark_fresh_install() {
        std::ofstream(fs::path(temp_dir) / AppConstants::Update::FRESH_INSTALL_MARKER);
    }

  public:
    MigrationV18Fixture() {
        SetUp();
    }
    ~MigrationV18Fixture() {
        TearDown();
    }
};

} // namespace

TEST_CASE_METHOD(MigrationV18Fixture,
                 "Config migration v18: sets recheck_pending=true from a v17 config",
                 "[config][migration]") {
    json v17 = {{"config_version", 17},
                {"active_printer_id", "default"},
                {"input", {{"calibration", {{"valid", true}, {"a", 1.0}, {"e", 1.0}}}}}};
    write_and_init(v17);

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE(config.get<bool>("/input/calibration/recheck_pending", false) == true);
    // Existing calibration data is preserved (not clobbered).
    REQUIRE(config.get<bool>("/input/calibration/valid", false) == true);
}

TEST_CASE_METHOD(MigrationV18Fixture,
                 "Config migration v18: does not throw when input section is absent",
                 "[config][migration]") {
    json v17 = {{"config_version", 17}, {"active_printer_id", "default"}};
    REQUIRE_NOTHROW(write_and_init(v17));

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE(config.get<bool>("/input/calibration/recheck_pending", false) == true);
}

TEST_CASE_METHOD(MigrationV18Fixture,
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

TEST_CASE_METHOD(MigrationV18Fixture,
                 "Config migration v18: is idempotent — already-v18 config is untouched",
                 "[config][migration]") {
    // A config already at the current version must not have the flag flipped on.
    json v18 = {{"config_version", CURRENT_CONFIG_VERSION},
                {"active_printer_id", "default"},
                {"input", {{"calibration", {{"valid", true}, {"recheck_pending", false}}}}}};
    write_and_init(v18);

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE(config.get<bool>("/input/calibration/recheck_pending", true) == false);
}

TEST_CASE_METHOD(MigrationV18Fixture,
                 "Config migration v18: a seed with a printer node keeps its calibration",
                 "[config][migration]") {
    // seed_from_moonraker_detection's C path: a preset's input/display blocks
    // plus a moonraker_host under printers/default, and no config_version.
    json seed = {{"input", {{"calibration", {{"valid", true}, {"a", 1.66}, {"e", 1.76}}}}},
                 {"display", {{"rotate", 180}}},
                 {"active_printer_id", "default"},
                 {"printers", {{"default", {{"moonraker_host", "127.0.0.1"}}}}}};
    mark_fresh_install();
    write_and_init(seed);

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE_FALSE(config.get<bool>("/input/calibration/recheck_pending", false));
    REQUIRE(config.get<bool>("/input/calibration/valid", false));
}

TEST_CASE_METHOD(MigrationV18Fixture,
                 "Config migration v18: a packaged preset keeps its calibration",
                 "[config][migration]") {
    // A preset shipped as the release tarball's settings.json, on a fresh
    // install with no rolling backup to restore.
    json preset = {{"preset", "artillery-m1-pro"},
                   {"wizard_completed", false},
                   {"input", {{"calibration", {{"valid", true}, {"a", 1.64}, {"e", 1.83}}}}},
                   {"printer", {{"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}}}}};
    mark_fresh_install();
    write_and_init(preset);

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE_FALSE(config.get<bool>("/input/calibration/recheck_pending", false));
    REQUIRE(config.get<bool>("/input/calibration/valid", false));
}
