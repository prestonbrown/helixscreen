// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 171 to 208 of test_ams_backend_afc.cpp before it was split, in its order. Mostly
// [classify], [unit][narration], [reconciliation], [mixed], [status], and more

#include "ui_ams_lane_settings.h"
#include "ui_callback_helpers.h"
#include "ui_language_refresh.h"
#include "ui_panel_filament.h"
#include "ui_print_select_detail_view.h"
#include "ui_print_start_controller.h"
#include "ui_test_utils.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/afc_test_fixtures.h"
#include "../test_helpers/filament_accounting_cases.h"
#include "afc_defaults.h"
#include "ams_backend_afc.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_step_model.h"
#include "ams_tool_topology.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "error_event.h"
#include "filament_mapper.h"
#include "filament_op_execute.h"
#include "filament_op_router.h"
#include "filament_slot_override_store.h"
#include "gcode_footer_summary.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/config_dir_guard.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/print_select_detail_view_test_access.h"
#include "test_helpers/print_start_controller_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_cache_dir.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/update_queue_test_access.h"
#include "tools_used_cache.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

using helix::test::AfcBypassOn;
using helix::test::AfcEditedLaneFixture;
using helix::test::AfcFourLanes;
using helix::test::AfcFourRunningLanes;
using helix::test::AfcFourZeroBasedLanes;
using helix::test::AfcOpenDetailFixture;
using helix::test::AfcPrintStartHarness;
using helix::test::discover_afc;
using helix::test::discover_two_units;
using helix::test::feed_lane4_stands_in_for_lane2;
using helix::test::has_action;
using helix::test::kJamLine;
using helix::test::observed_lane1_v110;
using helix::test::set_map_commands;
using helix::test::setup_interleaved_toolchanger;
using helix::test::StaleFaultRig;
using helix::test::tool_of_unknown_color;
using helix::test::trace_contains;
using helix::test::two_buffer_objects;
using helix::test::two_unit_afc_state;
using helix::test::two_unit_objects;

// Multi-unit AFC scenarios (mixed topologies)

TEST_CASE("Mixed BoxTurtle + ViViD units with string format", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;

    // Pre-initialize 12 lanes (non-contiguous: lane1-lane8 + lane13-lane16)
    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3",  "lane4",  "lane5",  "lane6",
                                          "lane7", "lane8", "lane13", "lane14", "lane15", "lane16"};
    helper.set_discovered_lanes(all_lanes, {"Turtle_2"});
    helper.initialize_slots_from_discovery();

    // Feed AFC state with 3 string-format units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "Box_Turtle Turtle_2", "ViViD vivid_1"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane1", "lane2", "lane3", "lane4", "lane5", "lane6", "lane7",
                               "lane8", "lane13", "lane14", "lane15", "lane16"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    // Verify unit_infos_ parsed correctly
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);
    CHECK(unit_infos[0].type == "Box_Turtle");
    CHECK(unit_infos[0].name == "Turtle_1");
    CHECK(unit_infos[0].klipper_key == "AFC_BoxTurtle Turtle_1");
    CHECK(unit_infos[1].type == "Box_Turtle");
    CHECK(unit_infos[1].name == "Turtle_2");
    CHECK(unit_infos[1].klipper_key == "AFC_BoxTurtle Turtle_2");
    CHECK(unit_infos[2].type == "ViViD");
    CHECK(unit_infos[2].name == "vivid_1");
    CHECK(unit_infos[2].klipper_key == "AFC_vivid vivid_1");

    // Feed BoxTurtle 1 unit object (lanes 1-4, parallel topology)
    nlohmann::json bt1_data;
    bt1_data["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    bt1_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt1_data["hubs"] = nlohmann::json::array();
    bt1_data["buffers"] = nlohmann::json::array();

    // Feed BoxTurtle 2 unit object (lanes 5-8, parallel topology)
    nlohmann::json bt2_data;
    bt2_data["lanes"] = nlohmann::json::array({"lane5", "lane6", "lane7", "lane8"});
    bt2_data["extruders"] =
        nlohmann::json::array({"extruder4", "extruder5", "extruder6", "extruder7"});
    bt2_data["hubs"] = nlohmann::json::array();
    bt2_data["buffers"] = nlohmann::json::array();

    // Feed ViViD unit object (lanes 13-16, hub topology)
    nlohmann::json vivid_data;
    vivid_data["lanes"] = nlohmann::json::array({"lane13", "lane14", "lane15", "lane16"});
    vivid_data["extruders"] = nlohmann::json::array({"extruder"});
    vivid_data["hubs"] = nlohmann::json::array({"vivid_hub"});
    vivid_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt1_data;
    params["AFC_BoxTurtle Turtle_2"] = bt2_data;
    params["AFC_vivid vivid_1"] = vivid_data;
    helper.feed_status_update(params);

    // After all 3 units have lanes, reorganization should have happened
    auto& sys_info = helper.get_system_info_mutable();
    REQUIRE(sys_info.units.size() == 3);

    // Check per-unit and total slot counts
    int total_slots = 0;
    for (const auto& unit : sys_info.units) {
        CHECK(unit.slots.size() == 4);
        total_slots += static_cast<int>(unit.slots.size());
    }
    CHECK(total_slots == 12);

    // Verify topologies are correct per unit
    for (const auto& ui : unit_infos) {
        if (ui.type == "Box_Turtle") {
            // Box Turtle: empty hubs + multiple extruders = PARALLEL
            CHECK(ui.topology == PathTopology::PARALLEL);
        }
        if (ui.type == "ViViD") {
            // ViViD: hubs present + single extruder = HUB
            CHECK(ui.topology == PathTopology::HUB);
        }
    }
}

TEST_CASE("Partial unit data waits for all units before reorganization", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed 3 string units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1", "OpenAMS AMS_2"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane0", "lane1", "lane2", "lane3", "lane4", "lane5", "lane6",
                               "lane7", "lane8", "lane9", "lane10", "lane11"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    REQUIRE(helper.get_unit_infos().size() == 3);

    // Feed unit objects for only 2 of 3 units
    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt_data["hubs"] = nlohmann::json::array();
    bt_data["buffers"] = nlohmann::json::array();

    nlohmann::json ams1_data;
    ams1_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
    ams1_data["extruders"] = nlohmann::json::array({"extruder4"});
    ams1_data["hubs"] = nlohmann::json::array({"Hub_1"});
    ams1_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_OpenAMS AMS_1"] = ams1_data;
    helper.feed_status_update(params);

    // With only 2 of 3 units having data, reorganization should NOT happen yet.
    // Premature reorganization drops the last unit's lane data from the stash.
    auto info = helper.get_system_info();
    // Units from initial initialize_slots remain (1 flat unit), not reorganized
    CHECK(info.units.size() == 1);

    // Now feed 3rd unit → reorganization triggers with all units present
    nlohmann::json ams2_data;
    ams2_data["lanes"] = nlohmann::json::array({"lane8", "lane9", "lane10", "lane11"});
    ams2_data["extruders"] = nlohmann::json::array({"extruder5"});
    ams2_data["hubs"] = nlohmann::json::array({"Hub_2"});
    ams2_data["buffers"] = nlohmann::json::array();

    nlohmann::json params2;
    params2["AFC_OpenAMS AMS_2"] = ams2_data;
    helper.feed_status_update(params2);

    // Now all 3 units should be present with all lane data preserved
    info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);
    int total_slots = 0;
    for (const auto& unit : info.units) {
        total_slots += static_cast<int>(unit.slots.size());
    }
    CHECK(total_slots == 12);
}

TEST_CASE("Non-contiguous lane numbering assigns sequential global indices", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;

    // Lanes 1-8 and 13-16 (gap at 9-12)
    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3",  "lane4",  "lane5",  "lane6",
                                          "lane7", "lane8", "lane13", "lane14", "lane15", "lane16"};
    helper.set_discovered_lanes(all_lanes, {});
    helper.initialize_slots_from_discovery();

    // Verify all 12 lanes are initialized with sequential indices
    REQUIRE(helper.get_slot_count() == 12);

    // The slot registry maps sequential indices 0-11 to the non-contiguous lane names
    CHECK(helper.get_slot_name(0) == "lane1");
    CHECK(helper.get_slot_name(7) == "lane8");
    CHECK(helper.get_slot_name(8) == "lane13"); // Index 8 → lane13 (no gap)
    CHECK(helper.get_slot_name(11) == "lane16");

    // Feed units to trigger reorganization
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "Box_Turtle Turtle_2", "ViViD vivid_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit objects with lane assignments
    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = {
        {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
        {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}},
        {"hubs", nlohmann::json::array()},
        {"buffers", nlohmann::json::array()}};
    params["AFC_BoxTurtle Turtle_2"] = {
        {"lanes", {"lane5", "lane6", "lane7", "lane8"}},
        {"extruders", {"extruder4", "extruder5", "extruder6", "extruder7"}},
        {"hubs", nlohmann::json::array()},
        {"buffers", nlohmann::json::array()}};
    params["AFC_vivid vivid_1"] = {{"lanes", {"lane13", "lane14", "lane15", "lane16"}},
                                   {"extruders", {"extruder"}},
                                   {"hubs", {"vivid_hub"}},
                                   {"buffers", nlohmann::json::array()}};
    helper.feed_status_update(params);

    // After reorganization, global indices should still be sequential 0-11
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);

    // Verify each unit's slots have correct global indices
    int expected_global = 0;
    for (const auto& unit : info.units) {
        for (const auto& slot : unit.slots) {
            CHECK(slot.global_index == expected_global);
            expected_global++;
        }
    }
    CHECK(expected_global == 12);
}

// Tool changer current_slot is not overwritten

TEST_CASE_METHOD(AfcFourZeroBasedLanes,
                 "AFC tool changer reconciliation preserves current_load slot",
                 "[ams][afc][toolchanger]") {
    // In a tool changer, ALL 4 lanes are tool_loaded: true (direct-feed).
    // AFC reports current_load = "lane2" meaning lane2 is active at toolhead.
    // The reconciliation block must NOT overwrite current_slot with lane0.

    // Build a single status update containing BOTH AFC state and lane data
    nlohmann::json params;

    // AFC global state with current_load
    params["AFC"] = {{"current_load", "lane2"}};

    // All 4 lanes report as loaded (tool changer direct-feed)
    for (int i = 0; i < 4; ++i) {
        std::string key = "AFC_stepper lane" + std::to_string(i);
        params[key] = {{"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"},
                       {"material", "PLA"},  {"spool_id", 100 + i}, {"weight", 750}};
    }

    helper.feed_status_update(params);

    // current_slot should be 2 (lane2), not 0 (first loaded lane)
    auto info = helper.get_system_info();
    CHECK(info.current_slot == 2);
    CHECK(info.filament_loaded == true);
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes, "AFC apply_lane_data does not overwrite valid current_slot",
                 "[ams][afc][toolchanger]") {
    // First set current_slot via AFC state (current_load)
    helper.feed_afc_state({{"current_load", "lane2"}});
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Now feed lane data where all lanes are tool_loaded
    nlohmann::json lane_data;
    for (int i = 0; i < 4; ++i) {
        std::string name = "lane" + std::to_string(i);
        lane_data[name] = {{"tool_loaded", true},
                           {"color", "00FF00"},
                           {"material", "PETG"},
                           {"spool_id", 200 + i}};
    }
    helper.feed_afc_state({{"lanes", lane_data}});

    // current_slot should still be 2 (set by current_load), not overwritten
    auto info = helper.get_system_info();
    CHECK(info.current_slot == 2);
}

// 3-unit premature reorganize must not drop the last unit's lanes

TEST_CASE("AFC 3-unit incremental arrival preserves all unit lanes",
          "[ams][afc][mixed][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed AFC state with 3 string-format units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"OpenAMS AMS_1", "OpenAMS AMS_2", "Box_Turtle Turtle_1"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane0", "lane1", "lane2", "lane3", "lane4", "lane5", "lane6",
                               "lane7", "lane8", "lane9", "lane10", "lane11"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    REQUIRE(helper.get_unit_infos().size() == 3);

    // Feed stepper data with color/material for ALL 12 lanes BEFORE unit objects
    for (int i = 0; i < 12; ++i) {
        std::string lane_name = "lane" + std::to_string(i);
        nlohmann::json stepper_data = {
            // Six hex digits per lane: a shorter run is not a colour.
            {"color", "FF0" + std::to_string(1000 + i).substr(1)},
            {"material", "PLA"},
            {"spool_id", 300 + i},
            {"weight", 800},
            {"tool_loaded", (i >= 8)} // Turtle lanes are direct-feed
        };
        helper.feed_afc_stepper(lane_name, stepper_data);
    }

    // Now feed unit objects ONE AT A TIME (simulating incremental status notifications)
    // Unit 1: OpenAMS AMS_1
    {
        nlohmann::json ams1_data;
        ams1_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
        ams1_data["extruders"] = nlohmann::json::array({"extruder0"});
        ams1_data["hubs"] = nlohmann::json::array({"Hub_1"});
        ams1_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_OpenAMS AMS_1"] = ams1_data;
        helper.feed_status_update(params);
    }

    // Unit 2: OpenAMS AMS_2
    {
        nlohmann::json ams2_data;
        ams2_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
        ams2_data["extruders"] = nlohmann::json::array({"extruder1"});
        ams2_data["hubs"] = nlohmann::json::array({"Hub_2"});
        ams2_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_OpenAMS AMS_2"] = ams2_data;
        helper.feed_status_update(params);
    }

    // Unit 3: Box_Turtle Turtle_1
    {
        nlohmann::json bt_data;
        bt_data["lanes"] = nlohmann::json::array({"lane8", "lane9", "lane10", "lane11"});
        bt_data["extruders"] =
            nlohmann::json::array({"extruder", "extruder2", "extruder3", "extruder4"});
        bt_data["hubs"] = nlohmann::json::array();
        bt_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_BoxTurtle Turtle_1"] = bt_data;
        helper.feed_status_update(params);
    }

    // After all 3 units processed, ALL 12 slots should have valid data
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);

    int total_slots = 0;
    for (const auto& unit : info.units) {
        total_slots += static_cast<int>(unit.slots.size());
    }
    REQUIRE(total_slots == 12);

    // Specifically verify that Turtle_1's lanes (8-11) have color/material from stepper data
    // Find Turtle unit - after reorganize, units are sorted by min lane number
    bool found_turtle = false;
    for (const auto& unit : info.units) {
        if (unit.name.find("Turtle") != std::string::npos) {
            found_turtle = true;
            for (const auto& slot : unit.slots) {
                CHECK(slot.material != "");
                CHECK(slot.color_rgb != 0x808080); // Not default gray
            }
        }
    }
    CHECK(found_turtle);
}

// 4-unit ordering: units sorted by min lane number, not name (#554)

TEST_CASE("AFC 4-unit ordering by lane number not unit name", "[ams][afc][mixed][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(16);
    helper.initialize_slots_from_discovery();

    // Feed AFC state: 4 units in Moonraker JSON order (last unit first)
    // This reproduces #554 where unit discovery order caused wrong display
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array(
        {"Box_Turtle Turtle_2", "Box_Turtle Turtle_1", "ACE ace_1", "OpenAMS AMS_1"});
    afc_state["lanes"] = nlohmann::json::array({});
    for (int i = 0; i < 16; ++i)
        afc_state["lanes"].push_back("lane" + std::to_string(i));
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    // Feed stepper data for all 16 lanes
    for (int i = 0; i < 16; ++i) {
        std::string lane = "lane" + std::to_string(i);
        helper.feed_afc_stepper(
            lane, {{"color", "FF0000"}, {"material", "PLA"}, {"spool_id", i}, {"weight", 800}});
    }

    // Feed unit objects mapping lanes to units (deliberately out of physical order).
    // Key format uses Klipper convention: "AFC_" + type (underscores stripped) + " " + name
    auto feed_unit = [&](const std::string& klipper_key, int first_lane, int count) {
        nlohmann::json data;
        data["lanes"] = nlohmann::json::array();
        for (int i = first_lane; i < first_lane + count; ++i)
            data["lanes"].push_back("lane" + std::to_string(i));
        data["extruders"] = nlohmann::json::array({"extruder"});
        data["hubs"] = nlohmann::json::array();
        data["buffers"] = nlohmann::json::array();
        nlohmann::json params;
        params[klipper_key] = data;
        helper.feed_status_update(params);
    };

    // Unit with lanes 12-15 arrives FIRST (the bug scenario from #554)
    feed_unit("AFC_BoxTurtle Turtle_2", 12, 4);
    feed_unit("AFC_BoxTurtle Turtle_1", 0, 4);
    feed_unit("AFC_ACE ace_1", 8, 4);
    feed_unit("AFC_OpenAMS AMS_1", 4, 4);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 4);

    // Units must be ordered by min lane number, NOT alphabetically:
    // - Turtle_1 (lanes 0-3) first
    // - AMS_1 (lanes 4-7) second
    // - ace_1 (lanes 8-11) third
    // - Turtle_2 (lanes 12-15) fourth
    // With alphabetical sort, "ACE ace_1" would wrongly be first
    CHECK(info.units[0].name.find("Turtle_1") != std::string::npos);
    CHECK(info.units[1].name.find("AMS_1") != std::string::npos);
    CHECK(info.units[2].name.find("ace_1") != std::string::npos);
    CHECK(info.units[3].name.find("Turtle_2") != std::string::npos);

    // All 16 lanes should map correctly by global index
    REQUIRE(info.total_slots == 16);
    int total = 0;
    for (const auto& unit : info.units) {
        total += static_cast<int>(unit.slots.size());
    }
    CHECK(total == 16);
}

// Reactive lane highlighting & current_slot stability (PR #336 + improvements)

TEST_CASE("AFC stepper-only updates do not overwrite current_slot set by AFC state",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(10);
    helper.initialize_slots_from_discovery();

    // Step 1: AFC global state sets current_load = "lane9" (slot 9)
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane9"}};
        params["AFC_stepper lane9"] = {{"status", "Tooled"}, {"tool_loaded", true},
                                       {"color", "00AEFF"},  {"material", "ASA"},
                                       {"spool_id", 42},     {"weight", 800}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 9);

    // Step 2: Incremental update with ONLY AFC_stepper for lane0 — must NOT reset to 0
    {
        nlohmann::json params;
        params["AFC_stepper lane0"] = {
            {"prep", true}, {"load", true}, {"status", "Loaded"}, {"tool_loaded", false}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 9);
    CHECK(info.filament_loaded == true);

    // Step 3: Another stepper-only update for a different lane — still preserved
    {
        nlohmann::json params;
        params["AFC_stepper lane3"] = {
            {"prep", false}, {"load", false}, {"status", "None"}, {"tool_loaded", false}};
        helper.feed_status_update(params);
    }
    CHECK(helper.get_system_info().current_slot == 9);
}

TEST_CASE("AFC carriage authority does not invent filament AFC says is absent",
          "[ams][afc][reconciliation][1229]") {
    // Guard rails on the afc_stated_unloaded suppression. The carriage is the
    // single authority on WHICH tool is current (#1229) and that must not be
    // weakened — it may simply not manufacture a loaded slot on a frame where AFC
    // said the toolhead is empty.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);
    // "Tooled", not "Loaded": upstream's AFCLaneState separates the two
    // (AFC_lane.py:55-65) and only TOOLED / TOOL_LOADED / tool_loaded mean the
    // filament reached a melt zone — "Loaded" is loaded to the HUB
    // (AFC_lane.py:873). The sections below assert filament_loaded, which is a
    // toolhead claim, so the lanes have to actually be at their toolheads.
    // All four at once is legitimate here and not a contrived state: a
    // toolchanger has one melt zone PER toolhead, which is why reconcile's own
    // comment notes several lanes are loaded simultaneously on these machines.
    for (int i = 0; i < 4; ++i) {
        helper.feed_afc_stepper(
            "lane" + std::to_string(i),
            {{"map", "T" + std::to_string(i)}, {"status", "Tooled"}, {"tool_loaded", true}});
    }

    SECTION("a frame silent about current_load still lets the carriage elect") {
        // The authority itself is untouched: no AFC key, so nothing suppresses it.
        nlohmann::json params;
        params["toolchanger"] = {{"tool_number", 2}};
        helper.feed_status_update(params);

        auto info = helper.get_system_info();
        CHECK(info.current_slot == 2);
        CHECK(info.current_tool == 2);
    }

    SECTION("the suppression is frame-scoped and cannot latch") {
        // #1229 was a value latching and never moving again. Suppress on one
        // frame, then elect normally on the next.
        {
            nlohmann::json params;
            params["AFC"] = {{"current_load", nullptr}};
            params["toolchanger"] = {{"tool_number", 2}};
            helper.feed_status_update(params);
        }
        REQUIRE(helper.get_system_info().current_slot == -1);

        {
            nlohmann::json params;
            params["toolchanger"] = {{"tool_number", 2}};
            helper.feed_status_update(params);
        }
        auto info = helper.get_system_info();
        CHECK(info.current_slot == 2); // elects again — the flag did not stick
        CHECK(info.filament_loaded == true);
    }

    SECTION("a named current_load is not mistaken for the unloaded signal") {
        // afc_stated_unloaded must be narrower than current_slot_set_by_afc_state,
        // which a named lane also sets. Reusing that flag would suppress here too.
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane1"}};
        params["toolchanger"] = {{"tool_number", 1}};
        helper.feed_status_update(params);

        auto info = helper.get_system_info();
        CHECK(info.current_slot == 1);
        CHECK(info.filament_loaded == true);
    }
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes,
                 "AFC reconciliation updates current_slot when active lane becomes unloaded",
                 "[ams][afc][reconciliation]") {
    // Step 1: Set current_slot = 2 via AFC state
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane2"}};
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Step 2: Tool change — lane2 unloaded, lane3 loaded (AFC state included)
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane3"}};
        params["AFC_stepper lane2"] = {{"status", "Loaded"}, {"tool_loaded", false}};
        params["AFC_stepper lane3"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "00FF00"}, {"material", "PETG"}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 3);
}

TEST_CASE_METHOD(
    AfcFourZeroBasedLanes,
    "AFC reconciliation via stepper-only updates current_slot when no authoritative source",
    "[ams][afc][reconciliation]") {
    // Step 1: Set current_slot via stepper LOADED status only (no AFC state)
    {
        nlohmann::json params;
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Step 2: Stepper-only tool change — should detect and update
    {
        nlohmann::json params;
        params["AFC_stepper lane2"] = {{"status", "Loaded"}, {"tool_loaded", false}};
        params["AFC_stepper lane3"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "00FF00"}, {"material", "PETG"}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 3);
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes, "AFC non-active extruder does not overwrite current_slot",
                 "[ams][afc][reconciliation]") {
    helper.setup_toolchanger(2);

    // Set tool mapping: T0 -> lane0 (slot 0), T1 -> lane1 (slot 1)
    helper.feed_afc_stepper(
        "lane0", {{"map", "T0"}, {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T1"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "00FF00"}});

    // AFC state says current_load = lane0. The tool number is DERIVED from that
    // lane's mapping (lane0 -> slot 0 -> T0); AFC publishes no current_tool key.
    helper.feed_afc_state({{"current_load", "lane0"}});
    REQUIRE(helper.get_system_info().current_slot == 0);
    REQUIRE(helper.get_system_info().current_tool == 0);

    // Non-active extruder1 reports lane_loaded = lane1 — must NOT overwrite current_slot
    helper.feed_afc_extruder(
        "extruder1",
        {{"lane_loaded", "lane1"}, {"tool_start_status", false}, {"tool_end_status", false}});

    auto info2 = helper.get_system_info();
    CHECK(info2.current_slot == 0);
    CHECK(info2.current_tool == 0);
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes, "AFC filament unload (current_load=null) clears state",
                 "[ams][afc][reconciliation]") {
    // Load filament into lane2
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane2"}};
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);
    REQUIRE(helper.get_system_info().filament_loaded == true);

    // AFC reports current_load = null (unloaded)
    helper.feed_afc_state({{"current_load", nullptr}});

    auto info = helper.get_system_info();
    CHECK(info.current_slot == -1);
    CHECK(info.filament_loaded == false);
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes,
                 "AFC tool changer reconciliation derives current_slot from active tool",
                 "[ams][afc][reconciliation]") {
    helper.setup_toolchanger(4);

    // Set up tool mapping: T0->slot0, T1->slot1, T2->slot2, T3->slot3
    helper.feed_afc_stepper(
        "lane0",
        {{"map", "T0"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T1"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"map", "T2"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"map", "T3"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "FFFF00"}});

    // Verify tool mapping is populated
    auto mapping = helper.get_tool_to_slot_map();
    REQUIRE(mapping.size() >= 4);

    // Set up PARALLEL topology via unit object
    {
        nlohmann::json params;
        nlohmann::json bt_data;
        bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
        bt_data["extruders"] =
            nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
        bt_data["hubs"] = nlohmann::json::array();
        bt_data["buffers"] = nlohmann::json::array();
        params["AFC_BoxTurtle Turtle_1"] = bt_data;
        helper.feed_status_update(params);
    }

    // Simulate: Klipper reports active tool = T2, current_load is null (mid
    // tool-swap). The tool number comes from Klipper's own toolchanger object,
    // which is where a mid-swap tool number REALLY comes from — AFC.get_status()
    // publishes no "current_tool" key on any version (AFC.py v1.2.0:2531-2564),
    // so the fixture must not prove this with a field the firmware never sends.
    //
    // Both keys land in one frame, the same way Moonraker batches them:
    // apply_afc_state() runs first and clears the tool on the null current_load,
    // then the toolchanger block re-establishes T2. current_slot stays -1
    // because the null current_load is authoritative "unloaded" and sets
    // current_slot_set_by_afc_state, which the reconciliation block honours.
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", nullptr}};
        params["toolchanger"] = {{"tool_number", 2}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == -1);
    CHECK(info.filament_loaded == false);
    CHECK(info.current_tool == 2);
}

TEST_CASE("AFC jam with toolhead loaded offers Unload not Eject", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_extruder("extruder", {{"tool_start_status", true}, {"lane_loaded", "lane2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error(kJamLine, ctx);

    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
    REQUIRE(e->detail.find("reload filament manually") != std::string::npos); // full text

    auto has = [&](const std::string& label) {
        return std::any_of(e->recovery_actions.begin(), e->recovery_actions.end(),
                           [&](const helix::RecoveryAction& a) { return a.label == label; });
    };
    REQUIRE(has("Resume"));
    REQUIRE(has("Unload")); // toolhead loaded -> Unload (closes R1)
    REQUIRE(has("Recover"));
    REQUIRE_FALSE(has("Eject"));
}

TEST_CASE("AFC jam with empty toolhead offers Eject not Unload", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_extruder("extruder", {{"tool_start_status", false}, {"lane_loaded", "lane2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error(kJamLine, ctx);

    REQUIRE(e.has_value());
    auto has = [&](const std::string& label) {
        return std::any_of(e->recovery_actions.begin(), e->recovery_actions.end(),
                           [&](const helix::RecoveryAction& a) { return a.label == label; });
    };
    REQUIRE(has("Resume"));
    REQUIRE(has("Eject")); // empty toolhead -> Eject
    REQUIRE_FALSE(has("Unload"));
    REQUIRE(has("Recover"));
}

// RecoveryModalPresenter refuses to send a needs_hot_nozzle action into a nozzle
// below min_extrude_temp, so the flag decides whether a tapped recovery runs now
// or after a preheat. Getting it wrong is silent in both directions: false on a
// filament-moving action re-creates the cold-extrude failure, true on a
// state-only action makes a recovery wait on heat it does not need.
TEST_CASE("AFC recovery actions flag only the ones that move filament through the nozzle",
          "[ams][afc][classify]") {
    auto flag_of = [](const std::optional<helix::ErrorEvent>& e, const std::string& label) {
        for (const auto& a : e->recovery_actions) {
            if (a.label == label)
                return a.needs_hot_nozzle ? 1 : 0;
        }
        return -1; // absent
    };

    SECTION("toolhead loaded: Resume and Unload need heat, Recover does not") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.feed_afc_extruder("extruder",
                                 {{"tool_start_status", true}, {"lane_loaded", "lane2"}});
        helix::ClassifyContext ctx;
        ctx.is_paused = true;
        auto e = helper.classify_error(kJamLine, ctx);
        REQUIRE(e.has_value());

        CHECK(flag_of(e, "Resume") == 1);  // resuming the print extrudes
        CHECK(flag_of(e, "Unload") == 1);  // pulls filament back out of the melt zone
        CHECK(flag_of(e, "Recover") == 0); // AFC_RESET re-preps lanes, no nozzle
    }

    SECTION("empty toolhead: Eject is lane-to-spool and needs no heat") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.feed_afc_extruder("extruder",
                                 {{"tool_start_status", false}, {"lane_loaded", "lane2"}});
        helix::ClassifyContext ctx;
        ctx.is_paused = true;
        auto e = helper.classify_error(kJamLine, ctx);
        REQUIRE(e.has_value());

        CHECK(flag_of(e, "Eject") == 0);
        CHECK(flag_of(e, "Resume") == 1);
        CHECK(flag_of(e, "Recover") == 0);
    }
}

TEST_CASE("AFC catch-all: paused + error_state + unknown !! is CRITICAL AFC",
          "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_state({{"error_state", true}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error("!! Some AFC fault we do not recognize", ctx);

    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
    REQUIRE_FALSE(e->recovery_actions.empty()); // std actions attached
}

TEST_CASE("AFC defers when not paused and no error_state", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helix::ClassifyContext ctx; // idle, no error_state fed
    REQUIRE_FALSE(helper.classify_error("!! Some AFC fault", ctx).has_value());
}

TEST_CASE("AFC ignores non-error lines", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    REQUIRE_FALSE(helper.classify_error("// AFC_Brush: Clean Nozzle", ctx).has_value());
    REQUIRE_FALSE(helper.classify_error("ok", ctx).has_value());
}

TEST_CASE("AFC narration folds purge wording into the poop phase", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    // AFC has exactly one purge in a toolchange: the poop macro. "purge" wording
    // comes out of that same macro ("AFC_Poop: Move To Purge Location"), so it
    // must land on `poop` rather than a phase of its own.
    REQUIRE(afc.match_narration_phase("Purge") == std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("Purging old filament") ==
            std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("AFC_Poop: Move To Purge Location") ==
            std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("Loading lane 2 to hub") ==
            std::optional<std::string>("feed"));
}
TEST_CASE("AFC narration recognizes brush/cut/poop/kick (S2)", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    // AFC_BRUSH is the only wipe. It announces itself as "Clean Nozzle" at the
    // default verbose=1 and "Move to Brush." only at verbose>1, so both spellings
    // have to resolve to the same phase or the step never lights on a stock install.
    REQUIRE(afc.match_narration_phase("AFC_Brush: Clean Nozzle") ==
            std::optional<std::string>("brush"));
    REQUIRE(afc.match_narration_phase("Move to Brush") == std::optional<std::string>("brush"));
    REQUIRE(afc.match_narration_phase("Cutting tip") == std::optional<std::string>("cut"));
    REQUIRE(afc.match_narration_phase("Poop") == std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("Kick") == std::optional<std::string>("kick"));
    REQUIRE(afc.match_narration_phase("lane 2 is now loaded in toolhead") ==
            std::optional<std::string>("load"));
}
TEST_CASE("AFC narration recognizes the old-filament unload (#1046)",
          "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    // Both templates end/continue on an `unload` step; without a matcher case the
    // final unload step could never highlight (issue #1046 I-1).
    REQUIRE(afc.match_narration_phase("Retracting filament") ==
            std::optional<std::string>("unload"));
    REQUIRE(afc.match_narration_phase("Retract") == std::optional<std::string>("unload"));
    // AFC's actual console line when it pulls the old filament back to its lane.
    REQUIRE(afc.match_narration_phase("Unloading lane1") == std::optional<std::string>("unload"));
    REQUIRE(afc.match_narration_phase("Lane lane1 unload done") ==
            std::optional<std::string>("unload"));
    // Guard the substring trap: "unloading lane1" contains "loading lane", so a
    // naive ordering resolves the unload to `feed` and the bar jumps forward.
    REQUIRE(afc.match_narration_phase("Unloading lane1") != std::optional<std::string>("feed"));
    // ...while the cut macro's own retract wording stays on `cut`.
    REQUIRE(afc.match_narration_phase("AFC_Cut: Retract Filament for Cut") ==
            std::optional<std::string>("cut"));
}
TEST_CASE("AFC narration ignores unrelated lines", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    REQUIRE_FALSE(afc.match_narration_phase("Klipper state: ready").has_value());
    REQUIRE_FALSE(afc.match_narration_phase("").has_value());
}
TEST_CASE("AFC LOAD_SWAP template mirrors AFC's real CHANGE_TOOL order",
          "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    auto tmpl = afc.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(tmpl.empty());
    auto idx = [&](const std::string& id) {
        for (size_t i = 0; i < tmpl.size(); ++i)
            if (tmpl[i].id == id)
                return static_cast<int>(i);
        return -1;
    };
    // AFC's CHANGE_TOOL is TOOL_UNLOAD(old) then TOOL_LOAD(new):
    //   heat -> cut/form tip -> retract old to lane      (TOOL_UNLOAD)
    //   -> feed new to toolhead -> poop -> wipe -> kick -> wipe (TOOL_LOAD)
    // do_poop_kick_wipe runs *after* load_sequence succeeds, and its wipe runs
    // twice, straddling the kick: AFC.py v1.2.0:1390-1413, and the same sequence
    // inline in TOOL_LOAD at v1.1.0:1417-1440.
    //
    // The template is therefore ordered by FIRST occurrence, which puts brush
    // BEFORE kick. Listing kick first made the published index run 4 -> 6 -> 5
    // on every stock toolchange.
    REQUIRE(idx("heat") == 0);
    REQUIRE(idx("cut") == 1);
    REQUIRE(idx("unload") == 2);
    REQUIRE(idx("feed") == 3);
    REQUIRE(idx("poop") == 4);
    REQUIRE(idx("brush") == 5);
    REQUIRE(idx("kick") == 6);
    REQUIRE(idx("load") == 7);
    // The load-bearing half: brush precedes kick because the FIRST wipe does.
    REQUIRE(idx("brush") < idx("kick"));

    // The purge-to-bucket and the kick happen AFTER the new filament is fed —
    // that is the entire point of the poop, it purges the old colour out through
    // the new filament. Listing them before `feed` was the bug this pins.
    REQUIRE(idx("poop") > idx("feed"));
    REQUIRE(idx("kick") > idx("feed"));

    // AFC has no purge separate from the poop and no nozzle clean separate from
    // the brush. Both phantom steps must stay gone.
    REQUIRE(idx("purge") == -1);
    REQUIRE(idx("clean") == -1);
}

TEST_CASE("AFC LOAD_FRESH and UNLOAD templates match the real sequences",
          "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    auto ids = [&](StepOperationType op) {
        std::vector<std::string> out;
        for (const auto& p : afc.toolchange_phase_template(op))
            out.push_back(p.id);
        return out;
    };
    // A fresh load is TOOL_LOAD alone: no cut, no unload of a previous lane, but
    // do_poop_kick_wipe still runs afterwards — same poop / wipe / kick / wipe
    // body, so the same first-occurrence order as LOAD_SWAP.
    REQUIRE(ids(StepOperationType::LOAD_FRESH) ==
            std::vector<std::string>{"heat", "feed", "poop", "brush", "kick", "load"});
    // TOOL_UNLOAD alone: heat, cut/form tip, then pull back to the lane.
    REQUIRE(ids(StepOperationType::UNLOAD) == std::vector<std::string>{"heat", "cut", "unload"});
}

TEST_CASE("AFC operation_step_model mirrors the narration phase template",
          "[unit][ams][afc][stepmodel]") {
    AmsBackendAfcTestHelper afc;
    auto tmpl = afc.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    auto model = operation_step_model(afc, StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(model.steps.empty());
    REQUIRE(model.steps.size() == tmpl.size());
    for (size_t i = 0; i < tmpl.size(); ++i) {
        CHECK(model.steps[i].label == tmpl[i].label);
        CHECK(model.steps[i].optional == tmpl[i].optional);
        // No step is flagged for a live temperature readout on a narration model.
        CHECK_FALSE(model.steps[i].live_temp);
    }
}

TEST_CASE("AFC operation_step_index_subject is the narration toolchange-step subject",
          "[unit][ams][afc][stepmodel]") {
    AmsBackendAfcTestHelper afc;
    // The GcodeNarrationRouter drives AmsState's toolchange_step subject.
    CHECK(operation_step_index_subject(afc, StepOperationType::LOAD_SWAP) ==
          AmsState::instance().get_toolchange_step_subject());
}

// apply_lane_status must represent AFC's CLEARS, not just its values
//
// AFC clears a lane's identity itself. LANE_UNLOAD ends with
// set_spoolID(lane, None), which (when remember_spool is false, the default)
// runs clear_values(): spool_id=None, material='', color='', weight=0.
//
// Helix could not represent any of that. `spool_id: null` fails
// is_number_integer() and kept the old id; `color: ""` threw inside std::stoul
// and was caught as "keep existing colour"; AFC's SET_COLOR with an empty value
// stores the literal '#', which stripped to "" and threw the same way. Only
// `material: ""` actually cleared. So an ejected lane kept showing the previous
// spool's identity and, worse, kept its Spoolman link — which is what aimed a
// later edit at the wrong spool.
//
// The parser's job is firmware truth, including absence. Retention across an
// eject is a policy decision that belongs one layer up, in the override store.

TEST_CASE_METHOD(AfcFourLanes, "AFC parse: null spool_id clears the Spoolman link",
                 "[ams][afc][status]") {
    helper.feed_afc_stepper("lane1", {{"spool_id", 86}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->spoolman_id == 86);

    // AFC's clear_values() emits spool_id: null
    helper.feed_afc_stepper("lane1", {{"spool_id", nullptr}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->spoolman_id == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AFC an insert on a lane the plugin names no spool for offers the notice",
                 "[ams][afc][status][1710]") {
    // The empty -> loaded edge is an insert, and the spool_id binding is the
    // plugin's only word on what went in: a lane it leaves unnamed asks (the
    // stored record could describe a spool that left), a lane it names never
    // does.
    helix::test::RegisteredBackend<AmsBackendAfcTestHelper> harness;
    AmsBackendAfcTestHelper& helper = *harness;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    SlotInfo info;
    info.material = "PETG";
    info.color_rgb = 0x1188FF;
    helix::test::apply_edit(helper, 0, info);
    REQUIRE(AfcTestAccess::overrides(helper).count(0) == 1);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // An insert is an edge out of an OBSERVED empty, so the lanes start by
    // reporting empty: initialize_slots() writes UNKNOWN, and a lane whose
    // first frame already says Loaded is a baseline sighting, not an insert.
    helper.feed_afc_stepper("lane1", {{"status", "None"}});
    helper.feed_afc_stepper("lane2", {{"status", "None"}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.empty());

    // lane1 (slot 0) goes empty -> present with no spool named.
    helper.feed_afc_stepper("lane1", {{"status", "Loaded"}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::INFO);
    CHECK(helper.get_system_info().get_slot_global(0)->status == SlotStatus::AVAILABLE);

    // lane2 (slot 1) inserts with the plugin naming its spool: no ask.
    helper.feed_afc_stepper("lane2", {{"spool_id", 86}, {"status", "Loaded"}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.size() == 1);

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "AFC a loaded lane's first frame after start raises no notice",
                 "[ams][afc][status][1710]") {
    // initialize_slots() writes SlotStatus::UNKNOWN: until a frame says
    // otherwise, the lane's emptiness has never been observed, so the first
    // presence frame on every loaded lane at boot or reconnect is a baseline,
    // not an insert. Asking "same spool?" once per lane per boot would train
    // the notice away.
    helix::test::RegisteredBackend<AmsBackendAfcTestHelper> harness;
    AmsBackendAfcTestHelper& helper = *harness;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    SlotInfo info;
    info.material = "PETG";
    info.color_rgb = 0x1188FF;
    helix::test::apply_edit(helper, 0, info);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // First frame of a loaded lane: a baseline sighting, no ask.
    helper.feed_afc_stepper("lane1", {{"status", "Loaded"}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // The same lane later empties and inserts: that edge IS observed, and
    // the notice asks.
    helper.feed_afc_stepper("lane1", {{"status", "None"}});
    helper.feed_afc_stepper("lane1", {{"status", "Loaded"}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::INFO);

    helix::ui::set_test_toast_hook(nullptr);
}

// A lane colour after "#E53935" and then each value. Empty and a bare '#' (what
// SET_COLOR LANE=x COLOR= leaves) clear to the default; three digits expand,
// eight drop the alpha byte; anything else is a parse failure that keeps the
// previous colour, not a clear.
TEST_CASE("AFC parse: lane colour forms", "[ams][afc][status]") {
    struct Row {
        const char* color;
        uint32_t expected;
    };
    const Row rows[] = {
        {"", kAmsDefaultSlotColor}, {"#", kAmsDefaultSlotColor}, {"#F00", 0xFF0000},
        {"#800080FF", 0x800080},    {"#zzzzzz", 0xE53935},       {"#FF0000junk", 0xE53935},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION("\"" << r.color << "\"") {
            AmsBackendAfcTestHelper helper;
            helper.initialize_test_lanes(4);
            helper.initialize_slots_from_discovery();
            helper.feed_afc_stepper("lane1", {{"color", "#E53935"}});
            REQUIRE(helper.get_system_info().get_slot_global(0)->color_rgb == 0xE53935);

            helper.feed_afc_stepper("lane1", {{"color", r.color}});
            CHECK(helper.get_system_info().get_slot_global(0)->color_rgb == r.expected);
        }
    }
}

TEST_CASE_METHOD(AfcFourLanes, "AFC parse: absent fields are retained (deltas, not snapshots)",
                 "[ams][afc][status]") {
    helper.feed_afc_stepper("lane1", {{"spool_id", 86}, {"material", "ASA"}, {"color", "#E53935"}});

    // A weight-only delta must not disturb identity.
    helper.feed_afc_stepper("lane1", {{"weight", 500.0}});

    // get_system_info() returns by value; hold the snapshot in a named local.
    // Binding the pointer to the temporary let it die at the end of the
    // statement, so every check below read freed memory (ASAN: heap-use-after-free).
    const AmsSystemInfo info = helper.get_system_info();
    const auto* slot = info.get_slot_global(0);
    REQUIRE(slot->spoolman_id == 86);
    REQUIRE(slot->material == "ASA");
    REQUIRE(slot->color_rgb == 0xE53935);
    REQUIRE(slot->remaining_weight_g == Catch::Approx(500.0));
}

// can_recover_lane_position — AFC_LANE_RESET has real preconditions
//
// Lane-position recovery has no static capability gate, so the "Reset" menu
// entry was once offered on every AFC lane regardless of state. But AFC_LANE_RESET means
// "retract filament from the bowden back to the hub" (AFC_functions.py), and it
// refuses unless the lane's filament is actually at the hub:
//     if not CUR_HUB.state: AFC_error("Hub is already clear while trying to
//                                      reset '<lane>'"); return
// It also refuses while the toolhead is loaded. Offering it on an ejected lane
// produced exactly that error, which then LATCHED in printer.AFC.message and
// kept re-firing error toasts for the rest of the session (seen on the .112
// BoxTurtle). Reported upstream as AFCProject/AFC-Klipper-Add-On#803.

TEST_CASE_METHOD(AfcFourLanes, "AFC can_recover_lane_position requires filament at the hub",
                 "[ams][afc][reset]") {
    helper.set_discovered_lanes({}, {"Turtle_1"}); // so AFC_hub status updates route
    helper.feed_afc_stepper("lane1", {{"hub", "Turtle_1"}, {"load", true}});

    // Attribution comes from AFC.current_lane, which get_status() populates from
    // AFC.current_loading — set at the top of TOOL_LOAD/TOOL_UNLOAD and cleared
    // only by set_loaded()/set_unloaded() on success. A toolchange that strands
    // filament past the hub therefore leaves this naming the guilty lane, which
    // is what makes the attributed case reachable with the toolhead free.
    // Driving it through feed_afc_state() (rather than the setter) proves
    // apply_afc_state() actually wires the field.
    helper.feed_afc_state({{"current_lane", "lane1"}});

    // loaded_to_hub is latched at prep and never updated (see the [recovery]
    // cases above), so this now drives the real signal: AFC_hub.state.
    SECTION("hub sensor triggered, toolhead free, lane attributed -> reset is possible") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        CHECK(helper.can_recover_lane_position(0));
    }

    SECTION("hub sensor clear -> reset refused") {
        helper.feed_afc_hub("Turtle_1", {{"state", false}});
        CHECK_FALSE(helper.can_recover_lane_position(0));
    }

    SECTION("toolhead loaded -> reset refused even with hub sensor triggered") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        helper.feed_afc_state({{"current_load", "lane1"}});
        CHECK_FALSE(helper.can_recover_lane_position(0));
    }

    SECTION("AFC names no lane -> reset refused despite a triggered hub") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        helper.feed_afc_state({{"current_lane", nullptr}});
        CHECK_FALSE(helper.can_recover_lane_position(0));
    }

    SECTION("lane's own load switch clear -> reset refused") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        helper.feed_afc_stepper("lane1", {{"load", false}});
        CHECK_FALSE(helper.can_recover_lane_position(0));
    }

    SECTION("out-of-range slot is never resettable") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        CHECK_FALSE(helper.can_recover_lane_position(99));
        CHECK_FALSE(helper.can_recover_lane_position(-1));
    }
}

// Override store — user identity survives AFC's own clears
//
// apply_lane_status now honours AFC's clears (spool_id null, empty colour), so
// firmware truth genuinely clears on eject. Retention lives one layer up: the
// override store re-supplies the identity the user attached. Without it, the
// parser change alone would LOSE metadata on eject, which is the opposite of
// what the maintainer asked for (a lane keeps its spool until told otherwise,
// because pulling a spool for maintenance and putting the same one back is the
// common case).
//
// AFC firmware also structurally cannot hold brand / spool_name /
// total_weight_g / colour name / filament+vendor ids — verified against a live
// lane payload and its lane_data record — so those only ever live here.

// A fresh lane given a weight states a total too. AFC holds the remaining
// weight on its lane's meter and has no command for a total, so the total lands
// only in HelixScreen's own record: the edit settles once AFC's meter and that
// record both state what was saved.
TEST_CASE("AFC edit on a fresh unlinked lane settles once AFC echoes what it holds",
          "[ams][afc][override][afc-edit-echo]") {
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    MockPrinter printer;
    helix::test::RegisteredBackend<helix::test::WiredBackend<AmsBackendAfc>> afc_reg(&printer.api);
    auto& afc = *afc_reg;
    afc.set_discovered_lanes({"lane1", "lane2"}, {});
    REQUIRE(afc.start().success());
    helix::ui::UpdateQueue::instance().drain_until_empty();
    afc.fire_status({{"AFC_stepper lane1", {{"status", "Loaded"}, {"weight", 0.0}}}});

    const SlotInfo original = afc.get_slot_info(0);
    SlotInfo edit = original;
    edit.material = "PLA";
    edit.color_rgb = 0xFFFFFF;
    edit.remaining_weight_g = 1000.0f;
    edit.total_weight_g = 1000.0f;
    REQUIRE(afc.commit_user_edit(0, original, edit).success());
    helix::ui::UpdateQueue::instance().drain_until_empty();

    // A weight AFC reports that is not the edit's leaves the edit waiting.
    afc.fire_status(
        {{"AFC_stepper lane1", {{"material", "PLA"}, {"color", "#FFFFFF"}, {"weight", 990.0}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    {
        const auto waiting = helix::ams::lane_sources(afc_reg.lane(0)).pending;
        REQUIRE(waiting.has_value());
        CHECK(waiting->remaining_weight_g.has_value());
    }

    afc.fire_status(
        {{"AFC_stepper lane1", {{"material", "PLA"}, {"color", "#FFFFFF"}, {"weight", 1000.0}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();

    const auto pending = helix::ams::lane_sources(afc_reg.lane(0)).pending;
    CHECK_FALSE((pending && pending->remaining_weight_g.has_value()));
    CHECK_FALSE((pending && pending->total_weight_g.has_value()));
    CHECK_FALSE(pending.has_value());
    CHECK(afc.get_slot_info(0).total_weight_g == Catch::Approx(1000.0f));
    const nlohmann::json record =
        printer.client.mock_db_get(AmsBackendAfc::kOverrideNamespace, "lane1");
    REQUIRE(record.contains("total_weight_g"));
    CHECK(record["total_weight_g"].get<double>() == Catch::Approx(1000.0));
}

TEST_CASE("AFC override survives an eject that clears firmware fields", "[ams][afc][override]") {
    helix::test::RegisteredBackend<AmsBackendAfcTestHelper> helper_reg;
    AmsBackendAfcTestHelper& helper = *helper_reg;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // User attaches identity to lane 1, through both halves of a commit: the
    // backend write and the declaration that stands behind it.
    SlotInfo info = helper.get_slot_info(0);
    info.brand = "Likesilk";
    info.spool_name = "Black ASA";
    info.spoolman_id = 86;
    info.spoolman_filament_id = 55;
    info.material = "ASA";
    info.color_rgb = 0x1A1A1A;
    info.total_weight_g = 1000.0f;
    helix::test::edit_slot_as_user(helper, 0, info);
    REQUIRE(helper.get_slot_info(0).spoolman_filament_id == 55);

    // AFC ejects the lane: clear_values() nulls spool_id and empties
    // colour/material, and apply_lane_status now represents that faithfully.
    helper.feed_afc_stepper(
        "lane1", {{"spool_id", nullptr}, {"material", ""}, {"color", ""}, {"status", "None"}});

    const SlotInfo after = helper.get_slot_info(0);

    // Identity the user attached is re-supplied by the override layer.
    CHECK(after.brand == "Likesilk");
    CHECK(after.spool_name == "Black ASA");
    CHECK(after.spoolman_id == 86);
    CHECK(after.spoolman_filament_id == 55);
    CHECK(after.total_weight_g == Catch::Approx(1000.0f));
}

TEST_CASE_METHOD(AfcFourLanes, "AFC clear_slot_override drops the retained identity",
                 "[ams][afc][override][1625]") {
    SlotInfo info;
    info.brand = "Likesilk";
    info.spoolman_id = 86;
    info.material = "ASA";
    helix::test::apply_edit(helper, 0, info);
    helper.set_lane_spoolman_link(0, 86, 77, 3);
    REQUIRE(helper.get_slot_info(0).spoolman_filament_id == 77);

    helper.clear_slot_override(0);
    helper.feed_afc_stepper("lane1", {{"spool_id", nullptr}, {"material", ""}});

    const SlotInfo after = helper.get_slot_info(0);
    CHECK(after.brand.empty());
    CHECK(after.spoolman_id == 0);
    // All three handles die together: a surviving filament handle would keep
    // naming a Spoolman record the cleared slot is no longer linked to.
    CHECK(after.spoolman_filament_id == 0);
    CHECK(after.spoolman_vendor_id == 0);
}

TEST_CASE_METHOD(AfcFourLanes,
                 "AFC clear_slot_override empties an unlinked lane's firmware identity",
                 "[ams][afc][override][1661]") {
    // No Spoolman link, so no SET_SPOOL_ID reaches AFC and its clear_values()
    // never runs: what SET_MATERIAL / SET_COLOR / SET_WEIGHT stored survives
    // unless the clear names each field.
    helper.captured_gcodes.clear();
    helper.clear_slot_override(0);

    for (const char* gcode : {"SET_MATERIAL LANE=lane1 MATERIAL=", "SET_COLOR LANE=lane1 COLOR=",
                              "SET_WEIGHT LANE=lane1 WEIGHT=0"}) {
        CAPTURE(gcode);
        CHECK(std::find(helper.captured_gcodes.begin(), helper.captured_gcodes.end(),
                        std::string(gcode)) != helper.captured_gcodes.end());
    }
}
