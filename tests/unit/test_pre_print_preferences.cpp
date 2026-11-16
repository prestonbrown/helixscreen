// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pre_print_preferences.cpp
 * @brief Provider table for firmwares that store pre-print settings themselves
 *
 * The rule these all serve: absence is silence. A frame that does not mention a
 * setting is not a report that the setting is off, and an objects list we have
 * not received cannot prove a firmware lacks the store.
 */

#include "pre_print_preferences.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

PrinterDiscovery hardware_with_objects(const std::vector<std::string>& names) {
    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array();
    for (const auto& n : names) {
        objects.push_back(n);
    }
    hw.parse_objects(objects);
    return hw;
}

nlohmann::json prefs_frame(const nlohmann::json& fields) {
    nlohmann::json status = nlohmann::json::object();
    status["print_task_config"] = fields;
    return status;
}

} // namespace

TEST_CASE("pre-print preferences: a self-storing firmware is detected by its store",
          "[preprint][preferences]") {
    PrinterDiscovery u1 = hardware_with_objects({"print_task_config", "machine_state_manager"});
    REQUIRE(helix::preprint_prefs::firmware_persists_options(u1));
    REQUIRE(helix::preprint_prefs::provider_name(u1) == "Snapmaker U1");
    REQUIRE(helix::preprint_prefs::required_status_objects(u1) ==
            std::vector<std::string>{"print_task_config"});
}

TEST_CASE("pre-print preferences: a printer without the store has no provider",
          "[preprint][preferences]") {
    PrinterDiscovery plain = hardware_with_objects({"bed_mesh", "quad_gantry_level"});
    REQUIRE_FALSE(helix::preprint_prefs::firmware_persists_options(plain));
    REQUIRE(helix::preprint_prefs::provider_name(plain).empty());
    REQUIRE(helix::preprint_prefs::required_status_objects(plain).empty());
    REQUIRE(helix::preprint_prefs::read_persisted_defaults(
                plain, prefs_frame({{"auto_bed_leveling", true}}))
                .empty());
}

TEST_CASE("pre-print preferences: an unreported objects list refutes nothing",
          "[preprint][preferences]") {
    // parse_objects() has not run, so the hardware lists are not merely empty -
    // they are unknown. Concluding "no store" here would make every printer
    // look like it persists nothing until discovery completes.
    PrinterDiscovery undiscovered;
    REQUIRE_FALSE(undiscovered.objects_reported());
    REQUIRE_FALSE(helix::preprint_prefs::firmware_persists_options(undiscovered));
    REQUIRE(helix::preprint_prefs::read_persisted_defaults(
                undiscovered, prefs_frame({{"auto_bed_leveling", true}}))
                .empty());
}

TEST_CASE("pre-print preferences: stored settings map onto option ids", "[preprint][preferences]") {
    PrinterDiscovery u1 = hardware_with_objects({"print_task_config"});
    auto got = helix::preprint_prefs::read_persisted_defaults(u1, prefs_frame({
                                                                      {"auto_bed_leveling", true},
                                                                      {"shaper_calibrate", true},
                                                                      {"flow_calibrate", true},
                                                                      {"time_lapse_camera", false},
                                                                  }));
    // The stored shaper setting has no option to seed: input shaping is not a
    // pre-print step.
    REQUIRE(got.size() == 3);
    REQUIRE(got.count("shaper_calibrate") == 0);
    REQUIRE(got.at("bed_mesh") == true);
    REQUIRE(got.at("flow_calibrate") == true);
    REQUIRE(got.at("u1_timelapse") == false);
}

TEST_CASE("pre-print preferences: a field the frame omits is left out, never reported off",
          "[preprint][preferences]") {
    // Moonraker sends deltas. A frame carrying only one setting says nothing
    // about the others, and reporting them false would switch a user's stored
    // preferences off on the next resynthesis.
    PrinterDiscovery u1 = hardware_with_objects({"print_task_config"});
    auto got =
        helix::preprint_prefs::read_persisted_defaults(u1, prefs_frame({{"flow_calibrate", true}}));
    REQUIRE(got.size() == 1);
    REQUIRE(got.at("flow_calibrate") == true);
    REQUIRE(got.count("bed_mesh") == 0);
    REQUIRE(got.count("u1_timelapse") == 0);
}

TEST_CASE("pre-print preferences: integer and boolean spellings both read",
          "[preprint][preferences]") {
    PrinterDiscovery u1 = hardware_with_objects({"print_task_config"});
    auto got = helix::preprint_prefs::read_persisted_defaults(
        u1, prefs_frame({{"auto_bed_leveling", 1}, {"flow_calibrate", 0}}));
    REQUIRE(got.at("bed_mesh") == true);
    REQUIRE(got.at("flow_calibrate") == false);
}

TEST_CASE("pre-print preferences: a frame without the store yields nothing",
          "[preprint][preferences]") {
    PrinterDiscovery u1 = hardware_with_objects({"print_task_config"});
    nlohmann::json unrelated = nlohmann::json::object();
    unrelated["toolhead"] = {{"homed_axes", "xyz"}};
    REQUIRE(helix::preprint_prefs::read_persisted_defaults(u1, unrelated).empty());
}
