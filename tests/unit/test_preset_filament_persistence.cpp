// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Task 6 (Phase 2 offline filament picker): grows /preset_materials from a
// 4-string array to a 4-object array via a v18->v19 config migration, and
// exercises MaterialSettingsManager::{set,get,clear}_preset_filament so a
// later task can attach branded filament info to each of the 4 quick-material
// preset slots.
//
// Migration tests mirror tests/unit/test_config_migration_v18.cpp exactly:
// the migration function is a static function in config.cpp's anonymous
// namespace, so it's exercised through the public Config::init() path, which
// runs run_versioned_migrations() on an existing on-disk config. A sandboxed
// HELIX_CONFIG_DIR keeps backup-restore search paths inside the temp dir so
// nothing leaks from the host config.

#include "../helix_test_fixture.h"
#include "config.h"
#include "filament_catalog.h"
#include "material_settings_manager.h"

#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

// ============================================================================
// Migration v18 -> v19: /preset_materials strings -> objects
// ============================================================================

namespace {

class MigrationV19Fixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    void SetUp() {
        temp_dir = (fs::temp_directory_path() / "helix_migration_v19_test").string();
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

  public:
    MigrationV19Fixture() {
        SetUp();
    }
    ~MigrationV19Fixture() {
        TearDown();
    }
};

} // namespace

TEST_CASE_METHOD(MigrationV19Fixture,
                 "Config migration v19: preset_materials strings become objects",
                 "[config][migration]") {
    json v18 = {{"config_version", 18},
                {"active_printer_id", "default"},
                {"preset_materials", json::array({"PLA", "PETG", "ABS", "TPU"})}};
    write_and_init(v18);

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    auto& arr = config.get_json("/preset_materials");
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 4);
    for (size_t i = 0; i < arr.size(); ++i) {
        REQUIRE(arr[i].is_object());
    }
    REQUIRE(arr[0]["type"] == "PLA");
    REQUIRE(arr[1]["type"] == "PETG");
    REQUIRE(arr[2]["type"] == "ABS");
    REQUIRE(arr[3]["type"] == "TPU");
}

TEST_CASE_METHOD(MigrationV19Fixture,
                 "Config migration v19: idempotent on already-object preset_materials",
                 "[config][migration]") {
    // Seed at v18 (NOT CURRENT_CONFIG_VERSION) so run_versioned_migrations' `version < 19`
    // gate actually fires and migrate_v18_to_v19 runs against already-object data. This is
    // the only way to exercise its is_string() skip-guard for real: seeding at v19 would
    // never call the migration at all, making the test a no-op that proves nothing.
    json v18 = {{"config_version", 18},
                {"active_printer_id", "default"},
                {"preset_materials", json::array({{{"type", "PLA"}},
                                                  {{"type", "PLA"},
                                                   {"filament_id", "orca_bambu_pla_matte"},
                                                   {"brand", "Bambu Lab"},
                                                   {"name", "PLA Matte"},
                                                   {"nozzle", 220},
                                                   {"bed", 55}},
                                                  {{"type", "ABS"}},
                                                  {{"type", "TPU"}}})}};
    write_and_init(v18);

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    auto& arr = config.get_json("/preset_materials");
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 4);
    for (size_t i = 0; i < arr.size(); ++i) {
        REQUIRE(arr[i].is_object());
    }
    // Branded slot (index 1) must survive untouched -- migrate_v18_to_v19's is_string()
    // guard should skip objects entirely rather than clobbering or re-wrapping them.
    REQUIRE(arr[1]["type"] == "PLA");
    REQUIRE(arr[1]["filament_id"] == "orca_bambu_pla_matte");
    REQUIRE(arr[1]["brand"] == "Bambu Lab");
    REQUIRE(arr[1]["name"] == "PLA Matte");
    REQUIRE(arr[1]["nozzle"] == 220);
    REQUIRE(arr[1]["bed"] == 55);
}

// ============================================================================
// MaterialSettingsManager::{set,get,clear}_preset_filament
// ============================================================================

// ODR NOTE: this class must stay byte-identical to the one defined in
// tests/unit/test_material_settings_manager.cpp — both are `helix::TestAccess`
// linked into the same helix-tests binary, and a mismatched body is an ODR
// violation. If you change one, change both.
namespace helix {
class TestAccess {
  public:
    static void reset(MaterialSettingsManager& mgr) {
        mgr.overrides_.clear();
        mgr.preset_materials_ = default_preset_materials();
        mgr.preset_filaments_ = {};
        mgr.initialized_ = false;
    }
};
} // namespace helix

struct PresetFilamentFixture : HelixTestFixture {
    PresetFilamentFixture() {
        TestAccess::reset(MaterialSettingsManager::instance());
        Config* config = Config::get_instance();
        if (config) {
            config->get_json("/preset_materials") =
                nlohmann::json::array({"PLA", "PETG", "ABS", "TPU"});
        }
    }
    ~PresetFilamentFixture() override {
        TestAccess::reset(MaterialSettingsManager::instance());
    }
};

TEST_CASE_METHOD(PresetFilamentFixture, "preset filament round-trips via config",
                 "[material_settings]") {
    auto& mgr = MaterialSettingsManager::instance();
    mgr.init();
    helix::printer::EffectiveFilament ef;
    ef.id = "orca_test_pla";
    ef.brand = "Bambu Lab";
    ef.name = "PLA Matte";
    ef.type = "PLA";
    ef.nozzle_recommended = 220;
    ef.bed_temp = 55;

    mgr.set_preset_filament(1, ef);
    auto got = mgr.get_preset_filament(1);
    REQUIRE(got.has_value());
    REQUIRE(got->filament_id == "orca_test_pla");
    REQUIRE(got->brand == "Bambu Lab");
    REQUIRE(got->nozzle == 220);
    REQUIRE(got->bed == 55);
    // type kept in lockstep, slot 0 stays generic
    REQUIRE(mgr.get_preset_materials()[1] == "PLA");
    REQUIRE_FALSE(mgr.get_preset_filament(0).has_value());
}

TEST_CASE_METHOD(PresetFilamentFixture, "set_preset_material clears branding for that slot",
                 "[material_settings]") {
    auto& mgr = MaterialSettingsManager::instance();
    mgr.init();
    helix::printer::EffectiveFilament ef;
    ef.id = "x";
    ef.brand = "B";
    ef.name = "N";
    ef.type = "PETG";
    ef.nozzle_recommended = 240;
    ef.bed_temp = 70;
    mgr.set_preset_filament(2, ef);
    REQUIRE(mgr.get_preset_filament(2).has_value());
    mgr.set_preset_material(2, "ABS"); // plain type-swap reverts to generic
    REQUIRE_FALSE(mgr.get_preset_filament(2).has_value());
    REQUIRE(mgr.get_preset_materials()[2] == "ABS");
}

TEST_CASE_METHOD(PresetFilamentFixture, "load tolerates legacy bare-string preset_materials",
                 "[material_settings]") {
    // Defensive: if /preset_materials still holds bare strings (pre-migration or hand-edit),
    // load must treat each string as {type: string}, not crash.
    Config* config = Config::get_instance();
    REQUIRE(config != nullptr);
    config->get_json("/preset_materials") = nlohmann::json::array({"PLA", "PETG", "ABS", "TPU"});
    auto& mgr = MaterialSettingsManager::instance();
    mgr.init();
    REQUIRE(mgr.get_preset_materials()[0] == "PLA");
    REQUIRE(mgr.get_preset_materials()[3] == "TPU");
    for (int i = 0; i < 4; ++i)
        REQUIRE_FALSE(mgr.get_preset_filament(i).has_value());
}
