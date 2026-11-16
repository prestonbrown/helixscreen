// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v33 -> v34 config migration: the bypass spool settings go, since
// the bypass spool is a lane record in Moonraker's database; a printer's block
// waits under filament/legacy_external_spool for the import into that record.

#include "config.h"
#include "config_migrations.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;

TEST_CASE("v34 retires filament/external_spool and parks each printer's for the import",
          "[config][migration][external_spool]") {
    json config = {
        {"config_version", 33},
        {"filament", {{"external_spool", {{"material", "PLA"}}}, {"cooldown_delay_seconds", 5}}},
        {"printers",
         {{"a", {{"filament", {{"external_spool", {{"assigned", true}}}, {"keep", 1}}}}},
          {"b", {{"name", "B"}}}}}};
    helix::config_detail::run_versioned_migrations(config, "");
    REQUIRE(config["config_version"] == helix::kCurrentConfigVersion);
    CHECK_FALSE(config["filament"].contains("external_spool"));
    CHECK(config["filament"]["cooldown_delay_seconds"] == 5);
    CHECK_FALSE(config["printers"]["a"]["filament"].contains("external_spool"));
    CHECK(config["printers"]["a"]["filament"]["legacy_external_spool"] == json{{"assigned", true}});
    CHECK(config["printers"]["a"]["filament"]["keep"] == 1);
    CHECK(config["printers"]["b"]["name"] == "B");
    CHECK_FALSE(config["printers"]["b"].contains("filament"));
}
