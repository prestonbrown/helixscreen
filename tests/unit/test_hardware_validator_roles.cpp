// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_hardware_validator_roles.cpp
 * @brief TDD tests for registry-driven role validation in HardwareValidator.
 *
 * Production flow: fan and heater roles are resolved+persisted to config (pre-heal)
 * by FanRoleConfig::from_config and the heater heal block in the discovery sequence,
 * BEFORE HardwareValidator::validate() runs. When a pre-heal fires, validate() sees
 * Tier 0 Resolved and stays silent. Only Unresolved roles (no confident substitute)
 * surface as expected_missing.
 *
 * Populate pattern: MoonrakerClientMock::set_fans/set_heaters + client.hardware()
 * Config pattern:   local Config with v3 format via setup_printer_data()
 */

#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "hardware_role_registry.h"
#include "hardware_validator.h"
#include "moonraker_client_mock.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

// ---------------------------------------------------------------------------
// Local test fixture (separate name from helix::HardwareValidatorConfigFixture
// in test_hardware_validator.cpp to avoid ODR collision)
// ---------------------------------------------------------------------------
namespace helix {
class RoleValidatorFixture {
  protected:
    Config config;
    MoonrakerClientMock client;

    void setup_printer_data(const json& printer_data) {
        helix::setup_printer_data(config, printer_data);
    }

    void setup_minimal(const json& fans_node = json::object(),
                       const json& heaters_node = json::object(),
                       const json& optional_list = json::array()) {
        json printer = {{"moonraker_host", "127.0.0.1"},
                        {"moonraker_port", 7125},
                        {"hardware",
                         {{"optional", optional_list},
                          {"expected", json::array()},
                          {"last_snapshot", json::object()}}}};
        if (!fans_node.empty())
            printer["fans"] = fans_node;
        if (!heaters_node.empty())
            printer["heaters"] = heaters_node;
        setup_printer_data(printer);
    }
};
} // namespace helix

// ---------------------------------------------------------------------------
// Pre-heal + validate: stale role healed upstream → validator stays silent
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture,
                 "pre-healed part-fan role is silent in validator (no newly_discovered entry)",
                 "[hwvalidate][roles]") {
    // Stale role: fans/part was set to "output_pin fan0" (now gone).
    // The user silenced "output_pin fan0" as hardware/optional.
    // A canonical "fan" IS available in discovery.
    setup_minimal(
        /*fans_node=*/{{"part", "output_pin fan0"}},
        /*heaters_node=*/json::object(),
        /*optional_list=*/{"output_pin fan0"});

    // Discovery: "output_pin fan0" is absent; "fan" (canonical default) is live.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan", "heater_fan hotend_fan", "fan_generic Aux_Cooling_Fan"});

    // Production pre-heal: FanRoleConfig::from_config calls resolve_role_from_config
    // with persist=true BEFORE validate() runs. Mimic that here.
    std::string healed = helix::resolve_role_from_config(
        HardwareRoleId::PartFan, &config, client.hardware().fans(), /*persist_autoheal=*/true);
    REQUIRE(healed == "fan"); // canonical default was adopted
    REQUIRE(config.get<std::string>(config.df() + "fans/part", "") == "fan");

    HardwareValidator v;
    auto result = v.validate(&config, client.hardware());

    // After the upstream pre-heal the role reads as Resolved (Tier 0) — validator
    // must NOT surface it in newly_discovered or expected_missing.
    for (const auto& issue : result.newly_discovered) {
        bool is_part_fan_issue =
            issue.hardware_type == HardwareType::FAN && issue.hardware_name == "fan";
        REQUIRE_FALSE(is_part_fan_issue);
    }
    for (const auto& issue : result.expected_missing) {
        bool is_stale_part_fan =
            issue.hardware_type == HardwareType::FAN && issue.hardware_name == "output_pin fan0";
        REQUIRE_FALSE(is_stale_part_fan);
    }
}

// ---------------------------------------------------------------------------
// Unresolved GUIDED role → routed to the wizard, NOT surfaced by the validator
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture,
                 "validator stays silent for unresolved GUIDED part-fan role (wizard owns it)",
                 "[hwvalidate][roles]") {
    // Stale role: fans/part was "output_pin fan0" which no longer exists.
    // No fans at all are discovered, so guess_part_cooling_fan() returns ""
    // (no fallback candidate) → Unresolved. PartFan is a GUIDED role, so the
    // validator must NOT surface it (spec §3.4: guided → reconfig wizard only).
    setup_minimal(
        /*fans_node=*/{{"part", "output_pin fan0"}},
        /*heaters_node=*/json::object(),
        /*optional_list=*/{});

    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({}); // empty: no canonical "fan", no heuristic match

    HardwareValidator v;
    auto result = v.validate(&config, client.hardware());

    // Validator must NOT toast the guided role.
    for (const auto& issue : result.expected_missing)
        REQUIRE_FALSE(
            (issue.hardware_name == "output_pin fan0" && issue.hardware_type == HardwareType::FAN));

    // The collector IS the authority: it routes the unresolved guided role to FanSelect.
    auto steps = helix::unresolved_guided_steps(&config, client.hardware());
    REQUIRE(std::find(steps.begin(), steps.end(), helix::wizard::StepId::FanSelect) != steps.end());
}

// ---------------------------------------------------------------------------
// Resolved: configured role still live → no issue emitted
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture, "validator emits no issue when all roles resolve",
                 "[hwvalidate][roles]") {
    setup_minimal(
        /*fans_node=*/{{"part", "fan"}, {"hotend", "heater_fan hotend_fan"}},
        /*heaters_node=*/{{"bed", "heater_bed"}, {"hotend", "extruder"}},
        /*optional_list=*/{});

    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan", "heater_fan hotend_fan"});

    HardwareValidator v;
    auto result = v.validate(&config, client.hardware());

    // No roles in expected_missing or newly_discovered from the registry loop.
    REQUIRE(result.expected_missing.empty());
    bool any_role_issue = false;
    for (const auto& issue : result.newly_discovered)
        if (issue.hardware_type == HardwareType::FAN || issue.hardware_type == HardwareType::HEATER)
            any_role_issue = true;
    REQUIRE_FALSE(any_role_issue);
}

// ---------------------------------------------------------------------------
// Pre-heal + validate: stale heater role healed upstream → validator stays silent
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture,
                 "pre-healed hotend-heater role is silent in validator (no newly_discovered entry)",
                 "[hwvalidate][roles]") {
    // Configured hotend heater is a non-standard name that's gone; extruder (canonical) is live.
    setup_minimal(
        /*fans_node=*/json::object(),
        /*heaters_node=*/{{"hotend", "heater_generic custom_nozzle"}},
        /*optional_list=*/{"heater_generic custom_nozzle"});

    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan"});

    // Production pre-heal: the heater heal block in the discovery sequence calls
    // resolve_role_from_config with persist=true BEFORE validate() runs.
    std::string healed =
        helix::resolve_role_from_config(HardwareRoleId::HotendHeater, &config,
                                        client.hardware().heaters(), /*persist_autoheal=*/true);
    REQUIRE(healed == "extruder"); // canonical default was adopted
    REQUIRE(config.get<std::string>(config.df() + "heaters/hotend", "") == "extruder");

    HardwareValidator v;
    auto result = v.validate(&config, client.hardware());

    // After the upstream pre-heal the role reads as Resolved (Tier 0) — validator
    // must NOT surface it in newly_discovered or expected_missing.
    for (const auto& issue : result.newly_discovered) {
        bool is_heater_issue =
            issue.hardware_type == HardwareType::HEATER && issue.hardware_name == "extruder";
        REQUIRE_FALSE(is_heater_issue);
    }
    for (const auto& issue : result.expected_missing) {
        bool is_stale_heater = issue.hardware_type == HardwareType::HEATER &&
                               issue.hardware_name == "heater_generic custom_nozzle";
        REQUIRE_FALSE(is_stale_heater);
    }
}

// ---------------------------------------------------------------------------
// load_seen_hardware: a config that predates the seen-fans record
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture,
                 "load_seen_hardware reads the last snapshot of a config with no seen record",
                 "[hwvalidate][roles][2044]") {
    // Saved by a build before hardware/seen_fans and seen_heaters existed: the
    // snapshot is the only record of what this printer reported.
    setup_minimal();
    config.set<json>(config.df() + "hardware/last_snapshot",
                     {{"timestamp", "2026-01-01T00:00:00Z"},
                      {"heaters", {"extruder", "heater_bed"}},
                      {"sensors", json::array()},
                      {"fans", {"fan", "fan_generic chamber"}},
                      {"leds", json::array()},
                      {"filament_sensors", json::array()}});

    const helix::SeenHardware seen = HardwareValidator::load_seen_hardware(&config);

    const std::vector<std::string> fans = {"fan", "fan_generic chamber"};
    const std::vector<std::string> heaters = {"extruder", "heater_bed"};
    CHECK(seen.fans == fans);
    CHECK(seen.heaters == heaters);
}
