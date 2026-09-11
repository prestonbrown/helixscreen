// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v24 -> v25 config migration, which re-keys the prediction
// history from phase ordinals to phase names:
//
//   /print_start_history/entries/*/phases/{"2": 30}  ->  {"HOMING": 30}
//
// An ordinal is a position, not an identity. Inserting a phase renumbers every
// phase after it, and a stored "5" then reads back as whatever now occupies
// slot 5 — the same JSON, silently meaning something else. The ordinals at and
// above the insertion point (QGL, Z_TILT, BED_MESH, CLEANING, PURGING) are the
// ones that shift, so they carry the assertions here.
//
// The mapping is against the ordinals as they were BEFORE SOAKING took slot 4,
// because that is the numbering the stored documents were written under.
//
// Ordering matters: migrate_v10_to_v11 erases "3" and "4" (the heating phases)
// by their pre-SOAKING ordinal. Converting names first would leave those keys
// spelled HEATING_BED/HEATING_NOZZLE, and the erase would miss them.
//
// Driven through the public Config::init() path like the v18/v21/v22 migration
// tests, with a sandboxed HELIX_CONFIG_DIR.

#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class MigrationV25Fixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    void SetUp() {
        // Per-process directory: `make test-run` shards across concurrent
        // helix-tests processes and a fixed path lets two of them clobber each
        // other's settings.json mid-migration.
        temp_dir =
            (fs::temp_directory_path() / ("helix_migration_v25_test_" + std::to_string(::getpid())))
                .string();
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
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

    void write_and_init(const json& contents) {
        std::ofstream f(config_path);
        f << contents.dump(2);
        f.close();
        config.init(config_path);
    }

    json phases_of(size_t entry) {
        return config.get<json>("/print_start_history/entries/" + std::to_string(entry) + "/phases",
                                json::object());
    }

  public:
    MigrationV25Fixture() {
        SetUp();
    }
    ~MigrationV25Fixture() {
        TearDown();
    }
};

/// A history entry keyed the way every shipped build wrote it: pre-SOAKING
/// ordinals, one per phase the run actually spent time in.
json v11_history() {
    return json{{"entries", json::array({
                                json{{"total", 300},
                                     {"timestamp", 1700000000},
                                     {"temp_bucket", 1},
                                     {"phases",
                                      {{"2", 31},   // HOMING
                                       {"5", 62},   // QGL
                                       {"6", 47},   // Z_TILT
                                       {"7", 93},   // BED_MESH
                                       {"8", 21},   // CLEANING
                                       {"9", 16}}}} // PURGING
                            })}};
}

} // namespace

TEST_CASE_METHOD(MigrationV25Fixture, "v25 re-keys stored phase ordinals to phase names",
                 "[config][migration]") {
    write_and_init(json{{"config_version", 24},
                        {"active_printer_id", "voron"},
                        {"print_start_history", v11_history()},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    REQUIRE(config.get<int>("/config_version", 0) == helix::CURRENT_CONFIG_VERSION);

    const json phases = phases_of(0);
    INFO("phases after migration: " << phases.dump());

    // Every duration lands on the phase that measured it. The five below all
    // sit at or past the SOAKING insertion point, so a migration reading the
    // ordinals with the NEW table would shift each one a phase later.
    CHECK(phases.value("HOMING", -1) == 31);
    CHECK(phases.value("QGL", -1) == 62);
    CHECK(phases.value("Z_TILT", -1) == 47);
    CHECK(phases.value("BED_MESH", -1) == 93);
    CHECK(phases.value("CLEANING", -1) == 21);
    CHECK(phases.value("PURGING", -1) == 16);

    // Nothing is left behind under a number.
    CHECK_FALSE(phases.contains("2"));
    CHECK_FALSE(phases.contains("5"));
    CHECK_FALSE(phases.contains("9"));

    // SOAKING is new; a document written before it existed has no soak time.
    CHECK_FALSE(phases.contains("SOAKING"));

    // The rest of the entry is untouched.
    CHECK(config.get<int>("/print_start_history/entries/0/total", 0) == 300);
    CHECK(config.get<int>("/print_start_history/entries/0/temp_bucket", 0) == 1);
}

TEST_CASE_METHOD(MigrationV25Fixture, "v25 runs after the v11 heating-phase strip",
                 "[config][migration]") {
    // A v10 document runs the whole ladder. migrate_v10_to_v11 erases the two
    // heating ordinals; if names were applied first it would find nothing to
    // erase and the heating times would survive as HEATING_BED/HEATING_NOZZLE.
    write_and_init(json{{"config_version", 10},
                        {"active_printer_id", "voron"},
                        {"print_start_history",
                         {{"entries", json::array({
                                          json{{"total", 240},
                                               {"phases",
                                                {{"2", 30},   // HOMING
                                                 {"3", 120},  // HEATING_BED
                                                 {"4", 45},   // HEATING_NOZZLE
                                                 {"7", 60}}}} // BED_MESH
                                      })}}},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    const json phases = phases_of(0);
    INFO("phases after migration: " << phases.dump());

    CHECK(phases.value("HOMING", -1) == 30);
    CHECK(phases.value("BED_MESH", -1) == 60);
    CHECK_FALSE(phases.contains("HEATING_BED"));
    CHECK_FALSE(phases.contains("HEATING_NOZZLE"));
    CHECK_FALSE(phases.contains("3"));
    CHECK_FALSE(phases.contains("4"));
}

TEST_CASE_METHOD(MigrationV25Fixture, "v25 leaves an already-named history alone",
                 "[config][migration]") {
    // A stamp rollback replays the ladder over data already in the new shape.
    // Re-running the conversion must be a no-op rather than a second rename.
    const json named = json{
        {"entries",
         json::array({json{
             {"total", 210},
             {"phases", {{"HOMING", 28}, {"SOAKING", 300}, {"BED_MESH", 88}, {"PURGING", 12}}}}})}};

    write_and_init(json{{"config_version", 24},
                        {"active_printer_id", "voron"},
                        {"print_start_history", named},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    CHECK(phases_of(0) == named["entries"][0]["phases"]);
}

TEST_CASE_METHOD(MigrationV25Fixture, "v25 drops an ordinal no phase ever had",
                 "[config][migration]") {
    // A hand-edited or corrupted document must not name a key after whatever
    // the table happens to hold at that index.
    write_and_init(json{
        {"config_version", 24},
        {"active_printer_id", "voron"},
        {"print_start_history",
         {{"entries", json::array({json{{"total", 90}, {"phases", {{"2", 30}, {"97", 5}}}}})}}},
        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    const json phases = phases_of(0);
    INFO("phases after migration: " << phases.dump());
    CHECK(phases.value("HOMING", -1) == 30);
    CHECK(phases.size() == 1);
}

TEST_CASE_METHOD(MigrationV25Fixture, "v25 tolerates a history with no phases object",
                 "[config][migration]") {
    write_and_init(
        json{{"config_version", 24},
             {"active_printer_id", "voron"},
             {"print_start_history",
              {{"entries", json::array({json{{"total", 90}}, json{{"phases", json::array()}},
                                        json{{"total", 60}, {"phases", {{"2", 30}}}}})}}},
             {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    REQUIRE(config.get<int>("/config_version", 0) == helix::CURRENT_CONFIG_VERSION);
    CHECK(phases_of(2).value("HOMING", -1) == 30);
}
