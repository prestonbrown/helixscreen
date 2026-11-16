// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v25 -> v26 config migration, which converts the persisted
// completion-alert mode from a JSON boolean to the Off/Notification/Alert int
// AudioSettingsManager has always read and written:
//
//   /completion_alert: true   ->  2 (Alert)
//   /completion_alert: false  ->  0 (Off)
//
// The UI always persists an int via AudioSettingsManager::set_completion_alert_mode(),
// so a boolean here can only be a fresh-config default written as a bool, and an
// explicit stored int is a real user choice the migration leaves untouched.

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

TEST_CASE_METHOD(ConfigMigrationFixture,
                 "v26 maps a stored boolean onto the alert mode and leaves a stored int alone",
                 "[config][migration]") {
    struct Case {
        json stored;
        helix::CompletionAlertMode mode;
    };
    // 1 (Notification) is a choice made through the UI, never the boolean bug.
    const auto c = GENERATE(Case{true, helix::CompletionAlertMode::ALERT},
                            Case{false, helix::CompletionAlertMode::OFF},
                            Case{1, helix::CompletionAlertMode::NOTIFICATION});
    CAPTURE(c.stored.dump());
    write_and_init(json{{"config_version", 25},
                        {"active_printer_id", "voron"},
                        {"completion_alert", c.stored},
                        {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});

    REQUIRE(config.get<int>("/config_version", 0) == helix::kCurrentConfigVersion);
    CHECK(config.get<int>("/completion_alert", -1) == static_cast<int>(c.mode));
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
