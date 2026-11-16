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
// v34 then drops external_spool wherever v22 put it (test_config_migration_v34.cpp),
// so these cases pin the fan-out on cooldown_delay_seconds.
//
// The contract that matters is the non-clobber: a printer already holding a
// real value KEEPS it. Fanning the stale root value over live data would be
// strictly worse than leaving the orphan alone.
//
// Driven through the public Config::init() path like the v18/v21 tests, with a
// sandboxed HELIX_CONFIG_DIR so backup-restore search paths stay in the temp dir.

#include "config.h"
#include "test_helpers/config_migration_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

using helix::test::ConfigMigrationFixture;

TEST_CASE_METHOD(ConfigMigrationFixture, "v22 promotes a root value when the printer has none",
                 "[config][migration]") {
    // Losing the value would be the other failure mode: an install that only
    // ever wrote the root key must come out the far side still configured.
    write_and_init(json{{"config_version", 21},
                        {"active_printer_id", "voron"},
                        {"filament", {{"cooldown_delay_seconds", 45}}},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    CHECK(config.get<int>("/printers/voron/filament/cooldown_delay_seconds", 0) == 45);
    CHECK(config.get<int>("/filament/cooldown_delay_seconds", -1) == -1);
}

TEST_CASE_METHOD(ConfigMigrationFixture, "v22 fans the root value out to EVERY printer",
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

TEST_CASE_METHOD(ConfigMigrationFixture, "v22 leaves the root key alone when there is no printer",
                 "[config][migration]") {
    // fan_out_to_printers() refuses to erase a value it has nowhere to put -
    // the lesson from migrate_v19_to_v20's /led, which deleted user settings
    // outright when the fold was skipped.
    write_and_init(json{{"config_version", 21}, {"filament", {{"cooldown_delay_seconds", 75}}}});

    CHECK(config.get<int>("/filament/cooldown_delay_seconds", 0) == 75);
}
