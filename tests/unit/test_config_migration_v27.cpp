// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v26 -> v27 config migration. Every config carries the
// /input/scroll_throw that config.cpp writes on first load, and no screen sets
// it, so a stored 25 is the old shipped default rather than a user's choice.
// It moves to the platform default (35 on ESP32, 25 elsewhere); any other
// stored value stays.

#include "config.h"
#include "config_migrations.h"
#include "input_settings_manager.h"
#include "test_helpers/config_migration_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class MigrationV27Fixture : public helix::test::ConfigMigrationFixture {
  protected:
    void write_and_init(int version, int scroll_throw) {
        ConfigMigrationFixture::write_and_init(
            {{"config_version", version},
             {"active_printer_id", "voron"},
             {"input", {{"scroll_throw", scroll_throw}}},
             {"printers", {{"voron", {{"moonraker_host", "192.168.1.112"}}}}}});
    }
};

} // namespace

TEST_CASE("v27 scroll_throw decision: only the old default moves", "[config][migration]") {
    using helix::config_detail::migrated_scroll_throw;

    SECTION("a stored 25 takes the ESP32 default") {
        CHECK(migrated_scroll_throw(25, 35) == 35);
    }
    SECTION("any other stored value is the user's") {
        CHECK(migrated_scroll_throw(40, 35) == 40);
        CHECK(migrated_scroll_throw(24, 35) == 24);
        CHECK(migrated_scroll_throw(35, 35) == 35);
    }
    SECTION("where the platform default is 25 nothing changes") {
        CHECK(migrated_scroll_throw(25, 25) == 25);
    }
}

TEST_CASE_METHOD(MigrationV27Fixture, "v27 moves a stored 25 to the platform default",
                 "[config][migration]") {
    write_and_init(26, 25);

    REQUIRE(config.get<int>("/config_version", 0) == helix::kCurrentConfigVersion);
    CHECK(config.get<int>("/input/scroll_throw", -1) == InputSettingsManager::kDefaultScrollThrow);
}

TEST_CASE_METHOD(MigrationV27Fixture, "v27 leaves a user's scroll_throw alone",
                 "[config][migration]") {
    write_and_init(26, 40);

    REQUIRE(config.get<int>("/config_version", 0) == helix::kCurrentConfigVersion);
    CHECK(config.get<int>("/input/scroll_throw", -1) == 40);
}

TEST_CASE("v27 leaves a missing scroll_throw missing", "[config][migration]") {
    // A config without the key reads the platform default already; the
    // migration writes nothing for it.
    json with_input{{"config_version", 26}, {"input", {{"scroll_limit", 10}}}};
    helix::config_detail::run_versioned_migrations(with_input, "");
    CHECK(with_input["config_version"] == helix::kCurrentConfigVersion);
    CHECK_FALSE(with_input["input"].contains("scroll_throw"));

    json without_input{{"config_version", 26}};
    helix::config_detail::run_versioned_migrations(without_input, "");
    CHECK_FALSE(without_input.contains("input"));
}
