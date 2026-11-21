// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v34 -> v35 config migration: a display sleep still at the old
// 20-minute default moves to the one-hour default; any other choice stays.

#include "app_constants.h"
#include "config.h"
#include "config_migrations.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;

namespace {

json migrate(json display) {
    json config{{"config_version", 34}, {"display", std::move(display)}};
    helix::config_detail::run_versioned_migrations(config, "");
    REQUIRE(config["config_version"] == helix::kCurrentConfigVersion);
    return config["display"];
}

} // namespace

TEST_CASE("v35 moves a display sleep at the old 20-minute default to an hour",
          "[config][migration][display_sleep]") {
    CHECK(migrate({{"sleep_sec", 1200}})["sleep_sec"] ==
          helix::app_constants::display::kDefaultSleepSec);
    CHECK(helix::app_constants::display::kDefaultSleepSec == 3600);
}

TEST_CASE("v35 keeps any other display sleep choice", "[config][migration][display_sleep]") {
    CHECK(migrate({{"sleep_sec", 0}})["sleep_sec"] == 0);
    CHECK(migrate({{"sleep_sec", 600}})["sleep_sec"] == 600);
    CHECK(migrate({{"sleep_sec", 1800}})["sleep_sec"] == 1800);
    CHECK_FALSE(migrate(json::object()).contains("sleep_sec"));
}
