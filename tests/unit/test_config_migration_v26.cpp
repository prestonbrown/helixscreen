// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v25 -> v26 config migration, which converts the persisted
// completion-alert mode from a JSON boolean to the Off/Notification/Alert int
// AudioSettingsManager has always read and written:
//
//   /completion_alert: true   ->  2 (Alert)
//   /completion_alert: false  ->  0 (Off)
//
// A boolean here can only be the old fresh-config default (get_default_config()
// used to write `completion_alert = true`) — the UI always persists an int via
// AudioSettingsManager::set_completion_alert_mode() — so an explicit stored int
// is left untouched: it is a real user choice, not the bug this migration fixes.

#include "audio_settings_manager.h"
#include "config.h"
#include "test_helpers/config_migration_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

using helix::test::ConfigMigrationFixture;

TEST_CASE_METHOD(ConfigMigrationFixture, "v26 fresh config defaults completion_alert to Alert",
                 "[config][migration]") {
    // reset_to_defaults() is the nearest public seam onto get_default_config()
    // with include_user_prefs=true — the fresh-config builder this migration
    // guards against ever having written the wrong type for.
    config.reset_to_defaults();

    CHECK(config.get<int>("/completion_alert", -1) ==
          static_cast<int>(helix::CompletionAlertMode::ALERT));
}

TEST_CASE_METHOD(ConfigMigrationFixture, "v26 migrates a stored true to Alert",
                 "[config][migration]") {
    write_and_init(json{{"config_version", 25},
                        {"active_printer_id", "voron"},
                        {"completion_alert", true},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    REQUIRE(config.get<int>("/config_version", 0) == helix::kCurrentConfigVersion);
    CHECK(config.get<int>("/completion_alert", -1) ==
          static_cast<int>(helix::CompletionAlertMode::ALERT));
}

TEST_CASE_METHOD(ConfigMigrationFixture, "v26 migrates a stored false to Off",
                 "[config][migration]") {
    write_and_init(json{{"config_version", 25},
                        {"active_printer_id", "voron"},
                        {"completion_alert", false},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    REQUIRE(config.get<int>("/config_version", 0) == helix::kCurrentConfigVersion);
    CHECK(config.get<int>("/completion_alert", -1) ==
          static_cast<int>(helix::CompletionAlertMode::OFF));
}

TEST_CASE_METHOD(ConfigMigrationFixture, "v26 leaves an explicit stored int alone",
                 "[config][migration]") {
    // 1 (Notification) is a real user choice made through the UI, which always
    // persists an int. The migration must not treat it as the boolean bug.
    write_and_init(json{{"config_version", 25},
                        {"active_printer_id", "voron"},
                        {"completion_alert", 1},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    REQUIRE(config.get<int>("/config_version", 0) == helix::kCurrentConfigVersion);
    CHECK(config.get<int>("/completion_alert", -1) ==
          static_cast<int>(helix::CompletionAlertMode::NOTIFICATION));
}

TEST_CASE_METHOD(ConfigMigrationFixture, "v26 leaves an absent completion_alert absent",
                 "[config][migration]") {
    // A config that never stored the key reads AudioSettingsManager's default,
    // Alert. Writing anything here, a null included, would replace that default.
    write_and_init(json{{"config_version", 25},
                        {"active_printer_id", "voron"},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    REQUIRE(config.get<int>("/config_version", 0) == helix::kCurrentConfigVersion);
    CHECK_FALSE(config.exists("/completion_alert"));
}
