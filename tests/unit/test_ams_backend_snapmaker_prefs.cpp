// tests/unit/test_ams_backend_snapmaker_prefs.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "../test_helpers/scoped_config_write_counter.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"
#include "config.h"
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
const nlohmann::json kOrange = {{"nums", 1}, {"alpha", 255}, {"mode", 0}, {"colors", {"FF8000"}}};
const nlohmann::json kWhite = {{"nums", 1}, {"alpha", 255}, {"mode", 0}, {"colors", {"FFFFFF"}}};

/// A print running on head 0 (Generic ASA Basic, orange), with head 3 holding
/// the same filament loaded through its feeder.
nlohmann::json printing_on_head0() {
    nlohmann::json printing = frame({{"auto_replenish_filament", true},
                                     {"filament_vendor", {"Generic", "Generic", "NONE", "Generic"}},
                                     {"filament_type", {"ASA", "PLA", "NONE", "ASA"}},
                                     {"filament_sub_type", {"Basic", "Basic", "NONE", "Basic"}},
                                     {"filament_exist", {true, true, false, true}},
                                     {"filament_color_multi", {kOrange, kWhite, kWhite, kOrange}}});
    printing["print_stats"] = {{"state", "printing"}};
    printing["toolhead"] = {{"extruder", "extruder"}};
    printing["extruder"] = {{"nozzle_diameter", 0.4}, {"nozzle_volume_type", "standard"}};
    printing["extruder3"] = {{"nozzle_diameter", 0.4}, {"nozzle_volume_type", "standard"}};
    printing["filament_feed right"] = {{"extruder3",
                                        {{"channel_state", "load_finish"},
                                         {"filament_detected", true},
                                         {"module_exist", true},
                                         {"disable_auto", false}}}};
    printing["filament_motion_sensor e3_filament"] = {{"enabled", true}};
    return printing;
}

/// Head 0's record as the firmware leaves it at a runout (NONE, filament_exist
/// false).
nlohmann::json cleared_head0() {
    return {{"filament_vendor", {"NONE"}},
            {"filament_type", {"NONE"}},
            {"filament_sub_type", {"NONE"}},
            {"filament_exist", {false}},
            {"filament_color_multi", {kWhite}}};
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

TEST_CASE("snapmaker backend keeps the record a runout head printed after the firmware clears it",
          "[ams][snapmaker][runout_other_head]") {
    // The firmware clears the runout head's record (NONE, filament_exist
    // false) before or with the frame that reports the pause.
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(printing_on_head0());
    const nlohmann::json cleared = cleared_head0();
    nlohmann::json paused;
    SECTION("cleared in the frame before the pause") {
        backend.fire_status(frame(cleared));
        paused["print_stats"] = {{"state", "paused"}};
    }
    SECTION("cleared in the frame that reports the pause") {
        paused = frame(cleared);
        paused["print_stats"] = {{"state", "paused"}};
    }
    backend.fire_status(paused);

    const auto facts = backend.runout_other_head_facts(0);
    REQUIRE(facts.has_value());
    CHECK_FALSE(facts->heads[0].record.has_value());
    REQUIRE(facts->original.has_value());
    CHECK(facts->original->vendor == "Generic");
    CHECK(facts->original->type == "ASA");
    CHECK(facts->original->subtype == "Basic");
    CHECK(facts->original->colors == std::vector<std::string>{"FF8000"});
    const auto verdicts = helix::runout_other_head::assess(*facts);
    REQUIRE(verdicts.size() == 4);
    CHECK(verdicts[3].eligible());
    CHECK_FALSE(verdicts[3].color_differs);

    // A record written during the pause does not replace what ran out.
    backend.fire_status(frame({{"filament_vendor", {"Generic"}},
                               {"filament_type", {"PETG"}},
                               {"filament_sub_type", {"Basic"}},
                               {"filament_exist", {true}},
                               {"filament_color_multi", {kWhite}}}));
    CHECK(backend.runout_other_head_facts(0)->original->type == "ASA");

    // The end of the print forgets it.
    nlohmann::json cancelled = frame(cleared);
    cancelled["print_stats"] = {{"state", "cancelled"}};
    backend.fire_status(cancelled);
    CHECK_FALSE(backend.runout_other_head_facts(0)->original.has_value());
    nlohmann::json next_pause;
    next_pause["print_stats"] = {{"state", "paused"}};
    backend.fire_status(next_pause);
    CHECK_FALSE(backend.runout_other_head_facts(0)->original.has_value());
}

TEST_CASE("a restart during a runout pause recovers the record the runout head printed",
          "[ams][snapmaker][runout_other_head][running_records]") {
    HelixTestFixture fixture;
    ScopedConfigWriteCounter store;
    helix::Config& config = *helix::Config::get_instance();
    const std::string key = config.df() + "snapmaker/running_records";
    config.set<nlohmann::json>(key, nullptr);

    {
        helix::test::StartedBackend<AmsBackendSnapmaker> before_restart;
        nlohmann::json printing = printing_on_head0();
        printing["print_stats"] = {
            {"state", "printing"}, {"filename", "cube.gcode"}, {"total_duration", 100.0}};
        before_restart.fire_status(printing);
        nlohmann::json paused = frame(cleared_head0());
        paused["print_stats"] = {{"state", "paused"}, {"total_duration", 130.0}};
        before_restart.fire_status(paused);
        REQUIRE(before_restart.runout_other_head_facts(0)->original.has_value());
    }

    // The new process starts into the pause with the record already cleared.
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    nlohmann::json first = printing_on_head0();
    first.update(frame(cleared_head0()), /*merge_objects=*/true);

    SECTION("the same print") {
        first["print_stats"] = {
            {"state", "paused"}, {"filename", "cube.gcode"}, {"total_duration", 160.0}};
        backend.fire_status(first);
        const auto facts = backend.runout_other_head_facts(0);
        REQUIRE(facts.has_value());
        CHECK_FALSE(facts->heads[0].record.has_value());
        REQUIRE(facts->original.has_value());
        CHECK(facts->original->type == "ASA");
        CHECK(facts->original->colors == std::vector<std::string>{"FF8000"});

        // The end of the print forgets it, in the store too.
        nlohmann::json cancelled;
        cancelled["print_stats"] = {{"state", "cancelled"}};
        backend.fire_status(cancelled);
        const nlohmann::json* kept = config.try_get_json(key);
        CHECK((kept == nullptr || kept->is_null()));
    }
    SECTION("another file") {
        first["print_stats"] = {
            {"state", "paused"}, {"filename", "vase.gcode"}, {"total_duration", 160.0}};
        backend.fire_status(first);
        CHECK_FALSE(backend.runout_other_head_facts(0)->original.has_value());
    }
    SECTION("the same file printed again") {
        first["print_stats"] = {
            {"state", "paused"}, {"filename", "cube.gcode"}, {"total_duration", 40.0}};
        backend.fire_status(first);
        CHECK_FALSE(backend.runout_other_head_facts(0)->original.has_value());
    }
    config.set<nlohmann::json>(key, nullptr);
}
