// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v21 -> v22 config migration, which re-scopes the two filament
// settings that were still stored at the config root while being READ
// per-printer:
//
//   /filament/external_spool         -> /printers/<id>/filament/external_spool
//   /filament/cooldown_delay_seconds -> /printers/<id>/filament/cooldown_delay_seconds
//
// Both are read as `Config::df() + "filament/..."`, so a root value was never
// consulted by anything. On a real K2 Plus (config_version 21) the root held a
// red PETG spool from months earlier while the live ASA-GF sat correctly in the
// printer's own node - two answers to the same question, one of them silently
// dead.
//
// The contract that matters is the non-clobber: a printer already holding a
// real value KEEPS it. Fanning the stale root value over live data would be
// strictly worse than leaving the orphan alone.
//
// Driven through the public Config::init() path like the v18/v21 tests, with a
// sandboxed HELIX_CONFIG_DIR so backup-restore search paths stay in the temp dir.

#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class MigrationV22Fixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    void SetUp() {
        temp_dir = (fs::temp_directory_path() / "helix_migration_v22_test").string();
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

  public:
    MigrationV22Fixture() {
        SetUp();
    }
    ~MigrationV22Fixture() {
        TearDown();
    }
};

} // namespace

TEST_CASE_METHOD(MigrationV22Fixture, "v22 keeps a printer's live external spool over the root one",
                 "[config][migration]") {
    // The exact shape found on the K2 Plus: a stale red PETG at the root, the
    // real ASA-GF in the printer node.
    write_and_init(
        json{{"config_version", 21},
             {"active_printer_id", "k2"},
             {"filament", {{"external_spool", {{"color_rgb", 16711680}, {"material", "PETG"}}}}},
             {"printers",
              {{"k2",
                {{"moonraker_host", "192.168.30.196"},
                 {"filament",
                  {{"external_spool",
                    {{"assigned", true}, {"material", "ASA-GF"}, {"spoolman_id", 145}}}}}}}}}});

    CHECK(config.get<int>("/config_version", 0) == 22);
    // The live value survives untouched...
    CHECK(config.get<std::string>("/printers/k2/filament/external_spool/material", "") == "ASA-GF");
    CHECK(config.get<int>("/printers/k2/filament/external_spool/spoolman_id", 0) == 145);
    // ...and the orphan is gone, rather than overwriting it.
    CHECK(config.get<std::string>("/filament/external_spool/material", "GONE") == "GONE");
}

TEST_CASE_METHOD(MigrationV22Fixture, "v22 promotes a root spool when the printer has none",
                 "[config][migration]") {
    // Losing the value would be the other failure mode: an install that only
    // ever wrote the root key must come out the far side still configured.
    write_and_init(json{{"config_version", 21},
                        {"active_printer_id", "voron"},
                        {"filament",
                         {{"external_spool", {{"assigned", true}, {"material", "PETG"}}},
                          {"cooldown_delay_seconds", 45}}},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    CHECK(config.get<std::string>("/printers/voron/filament/external_spool/material", "") ==
          "PETG");
    CHECK(config.get<int>("/printers/voron/filament/cooldown_delay_seconds", 0) == 45);
    CHECK(config.get<std::string>("/filament/external_spool/material", "GONE") == "GONE");
}

TEST_CASE_METHOD(MigrationV22Fixture, "v22 fans the root value out to EVERY printer",
                 "[config][migration]") {
    // Same rule migrate_v20_to_v21 established: an install-wide value becomes
    // every machine's value, so behaviour after the upgrade is unchanged.
    write_and_init(json{{"config_version", 21},
                        {"active_printer_id", "a"},
                        {"filament", {{"cooldown_delay_seconds", 30}}},
                        {"printers",
                         {{"show_printer_switcher", true}, // mixed-map sibling, must be skipped
                          {"a", {{"moonraker_host", "10.0.0.1"}}},
                          {"b", {{"moonraker_host", "10.0.0.2"}}}}}});

    CHECK(config.get<int>("/printers/a/filament/cooldown_delay_seconds", 0) == 30);
    CHECK(config.get<int>("/printers/b/filament/cooldown_delay_seconds", 0) == 30);
    // The non-printer sibling is untouched.
    CHECK(config.get<bool>("/printers/show_printer_switcher", false));
}

TEST_CASE_METHOD(MigrationV22Fixture, "v22 leaves the root key alone when there is no printer",
                 "[config][migration]") {
    // fan_out_to_printers() refuses to erase a value it has nowhere to put -
    // the lesson from migrate_v19_to_v20's /led, which deleted user settings
    // outright when the fold was skipped.
    write_and_init(
        json{{"config_version", 21}, {"filament", {{"external_spool", {{"material", "PLA"}}}}}});

    CHECK(config.get<std::string>("/filament/external_spool/material", "") == "PLA");
}
