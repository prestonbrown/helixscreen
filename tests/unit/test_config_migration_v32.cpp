// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v31 -> v32 config migration: an AD5X's runout_sensor entry, which no
// Klipper object answers to, gives way to head_switch_sensor.

#include "config.h"
#include "config_migrations.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;

namespace {

constexpr const char* kStale = "filament_switch_sensor runout_sensor";
constexpr const char* kHead = "filament_switch_sensor head_switch_sensor";

json sensor(const char* klipper_name, const char* role, bool enabled = true) {
    return {
        {"klipper_name", klipper_name}, {"role", role}, {"type", "switch"}, {"enabled", enabled}};
}

json printer_with(json printer_fields, json sensors) {
    printer_fields["filament_sensors"] = {{"master_enabled", true}, {"sensors", sensors}};
    return printer_fields;
}

json migrate(json config) {
    config["config_version"] = 31;
    helix::config_detail::run_versioned_migrations(config, "");
    REQUIRE(config["config_version"] == helix::kCurrentConfigVersion);
    return config;
}

const json& sensors_of(const json& config, const char* printer_id) {
    return config["printers"][printer_id]["filament_sensors"]["sensors"];
}

} // namespace

TEST_CASE("v32 drops an AD5X's runout_sensor beside head_switch_sensor",
          "[config][migration][ad5x]") {
    json config = migrate(
        {{"active_printer_id", "a"},
         {"printers",
          {{"a", printer_with({{"type", "FlashForge Adventurer 5X"}},
                              json::array({sensor(kStale, "runout"), sensor(kHead, "none")}))}}}});
    const json& sensors = sensors_of(config, "a");
    REQUIRE(sensors.size() == 1);
    CHECK(sensors[0]["klipper_name"] == kHead);
    CHECK(sensors[0]["role"] == "none");
}

TEST_CASE("v32 renames a lone runout_sensor and keeps the user's choices",
          "[config][migration][ad5x]") {
    json config =
        migrate({{"active_printer_id", "a"},
                 {"printers",
                  {{"a", printer_with({{"preset", "ad5x"}},
                                      json::array({sensor(kStale, "runout", false)}))}}}});
    const json& sensors = sensors_of(config, "a");
    REQUIRE(sensors.size() == 1);
    CHECK(sensors[0]["klipper_name"] == kHead);
    CHECK(sensors[0]["role"] == "runout");
    CHECK(sensors[0]["enabled"] == false);
}

TEST_CASE("v32 leaves runout_sensor on a printer that is not an AD5X",
          "[config][migration][ad5x]") {
    json config = migrate(
        {{"active_printer_id", "a"},
         {"printers",
          {{"a", printer_with({{"type", "Voron 2.4"}},
                              json::array({sensor(kStale, "runout"), sensor(kHead, "none")}))}}}});
    const json& sensors = sensors_of(config, "a");
    REQUIRE(sensors.size() == 2);
    CHECK(sensors[0]["klipper_name"] == kStale);
}

TEST_CASE("v32 reads a root ad5x preset as the active printer's only",
          "[config][migration][ad5x]") {
    json config =
        migrate({{"preset", "ad5x"},
                 {"active_printer_id", "b"},
                 {"printers",
                  {{"a", printer_with(json::object(), json::array({sensor(kStale, "runout")}))},
                   {"b", printer_with(json::object(), json::array({sensor(kStale, "runout")}))}}}});
    CHECK(sensors_of(config, "a")[0]["klipper_name"] == kStale);
    CHECK(sensors_of(config, "b")[0]["klipper_name"] == kHead);
}
