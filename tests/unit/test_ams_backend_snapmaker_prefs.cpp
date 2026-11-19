// tests/unit/test_ams_backend_snapmaker_prefs.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"
#include "test_helpers/backend_harness.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {
/// An unwrapped status object carrying only print_task_config — the shape the
/// initial query response sends and handle_status() takes.
nlohmann::json frame(const nlohmann::json& ptc_fields) {
    nlohmann::json params = nlohmann::json::object();
    params["print_task_config"] = ptc_fields;
    return params;
}
} // namespace

TEST_CASE("snapmaker backend keeps the preferences it is told about", "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_led_turn_off", true}, {"filament_entangle_sen", "low"}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == true);
    REQUIRE(backend.print_preferences().filament_entangle_sen.value() == "low");
}

TEST_CASE("a later frame that omits a preference does not clear it", "[ams][snapmaker][prefs]") {
    // The delta trap: a frame carrying only filament colours says nothing about
    // the preferences, and replacing rather than merging would switch them off.
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_led_turn_off", true}}));
    backend.fire_status(frame({{"filament_type", {"PLA"}}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == true);
}

TEST_CASE("a later frame that changes a preference wins", "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_led_turn_off", true}}));
    backend.fire_status(frame({{"end_led_turn_off", false}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == false);
}

TEST_CASE("every preference survives a later frame that carries none of them",
          "[ams][snapmaker][prefs]") {
    // One assertion per merge arm: deleting any single arm stops its field
    // surviving here, so no arm can lose coverage quietly.
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"auto_replenish_filament", true},
                               {"replenish_ignore_color", false},
                               {"filament_entangle_detect", true},
                               {"end_led_turn_off", false},
                               {"filament_entangle_sen", "high"},
                               {"end_unload_filament", {true, false, true, false}}}));
    backend.fire_status(frame({{"filament_type", {"PLA"}}}));
    const auto prefs = backend.print_preferences();
    REQUIRE(prefs.auto_replenish.value() == true);
    REQUIRE(prefs.replenish_ignore_color.value() == false);
    REQUIRE(prefs.filament_entangle_detect.value() == true);
    REQUIRE(prefs.end_led_turn_off.value() == false);
    REQUIRE(prefs.filament_entangle_sen.value() == "high");
    REQUIRE(prefs.end_unload_filament == std::vector<bool>{true, false, true, false});
}

TEST_CASE("a backend that has seen no preference field reports an empty set",
          "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"filament_type", {"PLA"}}}));
    REQUIRE(backend.print_preferences().empty());
}

TEST_CASE("one preference field is enough to make the held set non-empty",
          "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"auto_replenish_filament", false}}));
    REQUIRE_FALSE(backend.print_preferences().empty());
}

TEST_CASE("snapmaker backend reports the facts a runout switch to another head needs",
          "[ams][snapmaker][runout_other_head]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    const nlohmann::json one_red = {
        {"nums", 1}, {"alpha", 255}, {"mode", 0}, {"colors", {"FF0000"}}};
    const nlohmann::json two_tone = {
        {"nums", 2}, {"alpha", 128}, {"mode", 1}, {"colors", {"00FF00", "0000FF"}}};
    nlohmann::json status =
        frame({{"auto_replenish_filament", false},
               {"filament_vendor", {"Snapmaker", "Generic", "NONE", "Generic"}},
               {"filament_type", {"PLA", "PLA", "NONE", "PETG"}},
               {"filament_sub_type", {"Basic", "Silk", "NONE", "Basic"}},
               {"filament_color_multi", {one_red, two_tone, one_red, one_red}}});
    status["print_stats"] = {{"state", "paused"}};
    status["toolhead"] = {{"extruder", "extruder"}};
    status["extruder1"] = {{"nozzle_diameter", 0.4}, {"nozzle_volume_type", "high_flow"}};
    // Head 1 finished a feeder load; head 3 has filament at the port, its feeder
    // on automatic and its runout sensor armed; head 2's feeder is manual.
    status["filament_feed left"] = {
        {"extruder1", {{"channel_state", "load_finish"}, {"filament_detected", true}}}};
    status["filament_feed right"] = {{"extruder2",
                                      {{"channel_state", "wait_insert"},
                                       {"filament_detected", true},
                                       {"module_exist", true},
                                       {"disable_auto", true}}},
                                     {"extruder3",
                                      {{"channel_state", "wait_insert"},
                                       {"filament_detected", true},
                                       {"module_exist", true},
                                       {"disable_auto", false}}}};
    status["filament_motion_sensor e2_filament"] = {{"enabled", true}};
    status["filament_motion_sensor e3_filament"] = {{"enabled", true}};
    backend.fire_status(status);

    const auto facts = backend.runout_other_head_facts(0);
    REQUIRE(facts.has_value());
    CHECK(facts->runout_head == 0);
    CHECK(facts->paused);
    CHECK(facts->active_head == 0);
    CHECK(facts->auto_switch_enabled == false);
    REQUIRE(facts->original.has_value());
    CHECK(facts->original->colors == std::vector<std::string>{"FF0000"});
    REQUIRE(facts->heads.size() == 4);
    CHECK(facts->heads[1].nozzle_diameter == 0.4);
    CHECK(facts->heads[1].nozzle_flow == "high_flow");
    CHECK(facts->heads[1].loaded_by_feeder);
    REQUIRE(facts->heads[1].record.has_value());
    CHECK(facts->heads[1].record->subtype == "Silk");
    CHECK(facts->heads[1].record->colors == std::vector<std::string>{"00FF00", "0000FF"});
    CHECK(facts->heads[1].record->alpha == 128);
    CHECK(facts->heads[1].record->multi_mode == 1);
    CHECK_FALSE(facts->heads[2].loaded_by_feeder);
    CHECK_FALSE(facts->heads[2].record.has_value()); // NONE is no filament
    CHECK(facts->heads[3].loaded_by_feeder);
    CHECK(facts->heads[3].feeder_can_supply);
    CHECK_FALSE(facts->heads[1].feeder_can_supply); // finished a load; no port reading
    CHECK_FALSE(backend.runout_other_head_facts(4).has_value());

    // A spool swapped in during the pause changes the live record only: the
    // firmware still compares against what ran out.
    backend.fire_status(frame({{"filament_vendor", {"Generic"}}, {"filament_type", {"PETG"}}}));
    CHECK(backend.runout_other_head_facts(0)->original->type == "PLA");
    nlohmann::json resumed;
    resumed["print_stats"] = {{"state", "printing"}};
    backend.fire_status(resumed);
    CHECK(backend.runout_other_head_facts(0)->original->type == "PETG");
}
