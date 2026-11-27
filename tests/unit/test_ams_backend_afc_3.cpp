// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 90 to 120 of test_ams_backend_afc.cpp before it was split, in its order. Mostly [mixed],
// [topology], [backward_compat], [eject], [topology][mixed][dispatch], and more

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

// Mixed Topology — Flat String Units, AFC_lane, Unit Objects, Multi-Extruder

// 2a: Flat string units array

TEST_CASE("AFC backend handles flat string units array", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed AFC state with flat string units (real hardware format)
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"OpenAMS AMS_1", "Box_Turtle Turtle_1", "OpenAMS AMS_2"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane4", "lane5", "lane6", "lane7", "lane8", "lane9", "lane10",
                               "lane11", "lane0", "lane1", "lane2", "lane3"});
    afc_state["extruders"] = nlohmann::json::array(
        {"extruder", "extruder1", "extruder2", "extruder3", "extruder4", "extruder5"});
    helper.feed_afc_state(afc_state);

    // Verify unit_infos_ populated from string parsing
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);

    // Check that type/name were parsed from "Type Name" format
    bool found_openams_1 = false;
    bool found_bt_1 = false;
    bool found_openams_2 = false;
    for (const auto& ui : unit_infos) {
        if (ui.name == "AMS_1" && ui.type == "OpenAMS") {
            found_openams_1 = true;
            REQUIRE(ui.klipper_key == "AFC_OpenAMS AMS_1");
        }
        if (ui.name == "Turtle_1" && ui.type == "Box_Turtle") {
            found_bt_1 = true;
            REQUIRE(ui.klipper_key == "AFC_BoxTurtle Turtle_1");
        }
        if (ui.name == "AMS_2" && ui.type == "OpenAMS") {
            found_openams_2 = true;
            REQUIRE(ui.klipper_key == "AFC_OpenAMS AMS_2");
        }
    }
    REQUIRE(found_openams_1);
    REQUIRE(found_bt_1);
    REQUIRE(found_openams_2);

    // System type is still AFC
    auto info = helper.get_system_info();
    REQUIRE(info.type == AmsType::AFC);
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes, "AFC backend ViViD unit klipper_key uses lowercase",
                 "[ams][afc][vivid]") {
    // ViViD reports as "ViViD vivid_1" but Klipper object is AFC_vivid (lowercase)
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"ViViD vivid_1"});
    afc_state["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].type == "ViViD");
    REQUIRE(unit_infos[0].name == "vivid_1");
    REQUIRE(unit_infos[0].klipper_key == "AFC_vivid vivid_1"); // lowercase, not AFC_ViViD
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes,
                 "AFC backend flat string units: single word name still parses",
                 "[ams][afc][mixed]") {
    // Edge case: unit string with no space should not crash
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"NoSpaceUnit"});
    helper.feed_afc_state(afc_state);

    // Should not crash; unit_infos_ may be empty (no space = can't parse)
    const auto& unit_infos = helper.get_unit_infos();
    // Single word without space has no valid type/name split
    REQUIRE(unit_infos.empty());
}

// 2b: Unit-level object data

TEST_CASE("AFC backend unit-level object populates AfcUnitInfo", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // First, feed flat string units to populate unit_infos_
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1", "OpenAMS AMS_2"});
    helper.feed_afc_state(afc_state);

    // Then feed unit-level object data via status update
    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt_data["hubs"] = nlohmann::json::array();
    bt_data["buffers"] = nlohmann::json::array({"TN", "TN1", "TN2", "TN3"});

    nlohmann::json ams1_data;
    ams1_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
    ams1_data["extruders"] = nlohmann::json::array({"extruder4"});
    ams1_data["hubs"] = nlohmann::json::array({"Hub_1", "Hub_2", "Hub_3", "Hub_4"});
    ams1_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_OpenAMS AMS_1"] = ams1_data;
    helper.feed_status_update(params);

    // Verify unit_infos_ got populated with lane/extruder/hub/buffer data
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);

    // Find Turtle_1 and verify
    for (const auto& ui : unit_infos) {
        if (ui.name == "Turtle_1") {
            REQUIRE(ui.lanes.size() == 4);
            REQUIRE(ui.extruders.size() == 4);
            REQUIRE(ui.hubs.empty());
            REQUIRE(ui.buffers.size() == 4);
            // Box Turtle: empty hubs + multiple extruders → PARALLEL
            REQUIRE(ui.topology == PathTopology::PARALLEL);
        }
        if (ui.name == "AMS_1") {
            REQUIRE(ui.lanes.size() == 4);
            REQUIRE(ui.extruders.size() == 1);
            REQUIRE(ui.hubs.size() == 4);
            REQUIRE(ui.buffers.empty());
            // OpenAMS: hubs present + 1 extruder → HUB
            REQUIRE(ui.topology == PathTopology::HUB);
        }
    }
}

TEST_CASE("AFC HTLF mixed topology classification from per-lane hub routing",
          "[ams][afc][topology][mixed]") {
    // HTLF unit has 4 lanes: 2 direct to their own extruders, 2 through hub to shared extruder.
    // Per-lane hub field: "direct" vs hub name determines MIXED topology.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(3);

    // Feed stepper data WITH hub routing BEFORE unit object (stepper parsing happens first)
    helper.feed_afc_stepper("lane0", {{"map", "T0"},
                                      {"extruder", "extruder"},
                                      {"hub", "direct"},
                                      {"status", "Loaded"},
                                      {"color", "FF0000"}});
    helper.feed_afc_stepper("lane1", {{"map", "T2"},
                                      {"extruder", "extruder1"},
                                      {"hub", "direct"},
                                      {"status", "Loaded"},
                                      {"color", "00FF00"}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"},
                                      {"extruder", "extruder2"},
                                      {"hub", "HTLF_1"},
                                      {"status", "Loaded"},
                                      {"color", "0000FF"}});
    helper.feed_afc_stepper("lane3", {{"map", "T3"},
                                      {"extruder", "extruder2"},
                                      {"hub", "HTLF_1"},
                                      {"status", "Loaded"},
                                      {"color", "FFFF00"}});

    // Feed AFC state with unit name
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"HTLF HTLF_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object (triggers topology classification)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder", "extruder1", "extruder2"});
    unit_data["hubs"] = nlohmann::json::array({"HTLF_1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_HTLF HTLF_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is MIXED (not PARALLEL or HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::MIXED);

    // Verify per-lane hub routing data
    REQUIRE(unit_infos[0].lane_is_hub_routed.size() == 4);
    REQUIRE(unit_infos[0].lane_is_hub_routed[0] == false); // lane0 direct
    REQUIRE(unit_infos[0].lane_is_hub_routed[1] == false); // lane1 direct
    REQUIRE(unit_infos[0].lane_is_hub_routed[2] == true);  // lane2 via hub
    REQUIRE(unit_infos[0].lane_is_hub_routed[3] == true);  // lane3 via hub

    // Verify propagation to AmsUnit via get_system_info
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() >= 1);
    bool found = false;
    for (const auto& unit : info.units) {
        if (unit.name == "HTLF HTLF_1") {
            found = true;
            REQUIRE(unit.topology == PathTopology::MIXED);
            REQUIRE(unit.lane_is_hub_routed.size() == 4);
            REQUIRE(unit.lane_is_hub_routed[0] == false);
            REQUIRE(unit.lane_is_hub_routed[1] == false);
            REQUIRE(unit.lane_is_hub_routed[2] == true);
            REQUIRE(unit.lane_is_hub_routed[3] == true);
        }
    }
    REQUIRE(found);

    // lane2 moves to a direct route; lane3 keeps the unit MIXED, so only the
    // per-lane routing changes, and the published unit must follow it.
    helper.feed_afc_stepper("lane2", {{"map", "T1"},
                                      {"extruder", "extruder2"},
                                      {"hub", "direct"},
                                      {"status", "Loaded"},
                                      {"color", "0000FF"}});
    REQUIRE(helper.get_unit_infos()[0].topology == PathTopology::MIXED);
    bool found_after = false;
    for (const auto& unit : helper.get_system_info().units) {
        if (unit.name == "HTLF HTLF_1") {
            found_after = true;
            REQUIRE(unit.lane_is_hub_routed.size() == 4);
            CHECK(unit.lane_is_hub_routed[2] == false);
            CHECK(unit.lane_is_hub_routed[3] == true);
        }
    }
    REQUIRE(found_after);
}

TEST_CASE("AFC MIXED unit: a direct-fed lane and a hub-routed lane disagree on unload-before-load",
          "[ams][afc][topology][mixed][dispatch]") {
    // needs_unload_before_load() is a PER-LANE question and this is the machine
    // that proves it: one HTLF unit, lanes 0/1 wired straight to their own
    // extruders and lanes 2/3 merged through HTLF_1 into a shared one. Feeding
    // lane 1 disturbs nothing that lane 2 owns; feeding lane 3 must first clear
    // the shared path. A backend-wide answer cannot say both.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(3);

    helper.feed_afc_stepper(
        "lane0",
        {{"map", "T0"}, {"extruder", "extruder"}, {"hub", "direct"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T2"}, {"extruder", "extruder1"}, {"hub", "direct"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"map", "T1"}, {"extruder", "extruder2"}, {"hub", "HTLF_1"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"map", "T3"}, {"extruder", "extruder2"}, {"hub", "HTLF_1"}, {"status", "Loaded"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"HTLF HTLF_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder", "extruder1", "extruder2"});
    unit_data["hubs"] = nlohmann::json::array({"HTLF_1"});
    unit_data["buffers"] = nlohmann::json::array();
    nlohmann::json params;
    params["AFC_HTLF HTLF_1"] = unit_data;
    helper.feed_status_update(params);

    // Seat lane 2 (hub-routed) so the serial rule has something to want cleared.
    helper.set_filament_loaded(true);
    helper.set_current_slot(2);

    AmsSystemInfo info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    const AmsUnit& unit = info.units[0];
    REQUIRE(unit.topology == PathTopology::MIXED);
    REQUIRE(unit.lane_is_hub_routed.size() == 4);
    REQUIRE_FALSE(unit.lane_is_hub_routed[1]); // direct
    REQUIRE(unit.lane_is_hub_routed[3]);       // through HTLF_1

    const int direct_slot = unit.first_slot_global_index + 1;
    const int hub_slot = unit.first_slot_global_index + 3;

    // The whole point: same backend, same snapshot, two different answers.
    CHECK_FALSE(helper.needs_unload_before_load(info, direct_slot));
    CHECK(helper.needs_unload_before_load(info, hub_slot));

    // Guard rail — the backend-wide topology names the shape of the unit, never
    // which lane took which route, so a get_topology()-keyed rule cannot tell the
    // direct lane from the hub-routed one. That is the bug.
    REQUIRE(helper.get_topology() == PathTopology::MIXED);
}

TEST_CASE("AFC uniform HUB unit: an unparsed lane does not masquerade as direct-fed",
          "[ams][afc][topology][mixed][dispatch]") {
    // lane_is_hub_routed stores `false` for lanes whose routing has not arrived
    // yet — Moonraker sorts unit objects before AFC_lane ones, so a frame can
    // carry a unit while some of its lanes have never been parsed (#1229 defect
    // 4). On a uniform unit that placeholder is indistinguishable from "direct",
    // which is why the per-lane vector is consulted ONLY on a MIXED unit. Pin it:
    // every lane of a pure-hub unit still needs the shared path cleared.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // lane3's routing is deliberately never fed.
    for (const char* lane : {"lane0", "lane1", "lane2"}) {
        helper.feed_afc_stepper(
            lane,
            {{"map", "T0"}, {"extruder", "extruder"}, {"hub", "Turtle_1"}, {"status", "Loaded"}});
    }

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder"});
    unit_data["hubs"] = nlohmann::json::array({"Turtle_1"});
    unit_data["buffers"] = nlohmann::json::array();
    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = unit_data;
    helper.feed_status_update(params);

    helper.set_filament_loaded(true);
    helper.set_current_slot(0);

    AmsSystemInfo info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    const AmsUnit& unit = info.units[0];
    REQUIRE(unit.topology == PathTopology::HUB);
    REQUIRE(unit.lane_is_hub_routed.size() == 4);
    REQUIRE_FALSE(unit.lane_is_hub_routed[3]); // the placeholder, not a real "direct"

    CHECK(helper.needs_unload_before_load(info, unit.first_slot_global_index + 3));
}

TEST_CASE("AFC all-hub lanes classified as HUB topology", "[ams][afc][topology]") {
    // When all lanes route through a hub, topology should be HUB (not MIXED)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Feed stepper data: all lanes through hub
    helper.feed_afc_stepper("lane0", {{"hub", "Hub_1"}, {"status", "Loaded"}, {"color", "FF0000"}});
    helper.feed_afc_stepper("lane1", {{"hub", "Hub_1"}, {"status", "Loaded"}, {"color", "00FF00"}});
    helper.feed_afc_stepper("lane2", {{"hub", "Hub_1"}, {"status", "Loaded"}, {"color", "0000FF"}});
    helper.feed_afc_stepper("lane3", {{"hub", "Hub_1"}, {"status", "Loaded"}, {"color", "FFFF00"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder"});
    unit_data["hubs"] = nlohmann::json::array({"Hub_1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_OpenAMS AMS_1"] = unit_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::HUB);
    // All lanes hub-routed
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == true);
    }
}

TEST_CASE("AFC all-direct lanes classified as PARALLEL topology", "[ams][afc][topology]") {
    // When all lanes are direct and multiple extruders exist, topology should be PARALLEL.
    // Box Turtle lanes may report hub:"direct_load" instead of hub:"direct" (#392).
    const std::string hub = GENERATE("direct", "direct_load");
    CAPTURE(hub);
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    const char* const extruders[] = {"extruder", "extruder1", "extruder2", "extruder3"};
    for (int i = 0; i < 4; ++i) {
        helper.feed_afc_stepper("lane" + std::to_string(i),
                                {{"hub", hub}, {"extruder", extruders[i]}, {"status", "Loaded"}});
    }

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array();
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = unit_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == false);
    }
}

TEST_CASE("AFC one-lane direct unit beside hub units is PARALLEL, not HUB",
          "[ams][afc][topology]") {
    // A toolchanger's "Tools" unit with a single lane wired straight to its own
    // extruder: one lane, one extruder, no hub. The overview draws a hub box for
    // every HUB unit, so classifying it HUB puts a hub on a route that has none.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(5);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(2);

    for (const char* lane : {"lane0", "lane1", "lane2", "lane3"}) {
        helper.feed_afc_stepper(
            lane, {{"hub", "Turtle_1"}, {"extruder", "extruder"}, {"status", "Loaded"}});
    }
    helper.feed_afc_stepper("lane4",
                            {{"hub", "direct"}, {"extruder", "extruder1"}, {"status", "Loaded"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1", "Toolchanger Tools"});
    helper.feed_afc_state(afc_state);

    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] = nlohmann::json::array({"extruder"});
    bt_data["hubs"] = nlohmann::json::array({"Turtle_1"});
    bt_data["buffers"] = nlohmann::json::array();

    nlohmann::json tools_data;
    tools_data["lanes"] = nlohmann::json::array({"lane4"});
    tools_data["extruders"] = nlohmann::json::array({"extruder1"});
    tools_data["hubs"] = nlohmann::json::array();
    tools_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_Toolchanger Tools"] = tools_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 2);
    REQUIRE(unit_infos[0].topology == PathTopology::HUB);
    REQUIRE(unit_infos[1].lane_is_hub_routed == std::vector<bool>{false});
    REQUIRE(unit_infos[1].topology == PathTopology::PARALLEL);

    // A later frame routes lane3 straight to its extruder; no unit object follows.
    helper.feed_afc_stepper("lane3", {{"hub", "direct"}});
    REQUIRE(unit_infos[0].lane_is_hub_routed == std::vector<bool>{true, true, true, false});
    REQUIRE(unit_infos[0].topology == PathTopology::MIXED);
    const auto info = helper.get_system_info();
    const auto turtle = std::find_if(info.units.begin(), info.units.end(), [](const AmsUnit& u) {
        return u.name == "Box_Turtle Turtle_1";
    });
    REQUIRE(turtle != info.units.end());
    REQUIRE(turtle->topology == PathTopology::MIXED);
}

TEST_CASE("AFC one-lane direct unit is HUB until the printer reports several extruders",
          "[ams][afc][topology]") {
    // A direct lane on a printer with one extruder shares the nozzle with the hub
    // unit, so loading it must unload the hub lane first.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(5);
    helper.initialize_slots_from_discovery();

    for (const char* lane : {"lane0", "lane1", "lane2", "lane3"}) {
        helper.feed_afc_stepper(
            lane, {{"hub", "Turtle_1"}, {"extruder", "extruder"}, {"status", "Loaded"}});
    }
    helper.feed_afc_stepper("lane4",
                            {{"hub", "direct"}, {"extruder", "extruder"}, {"status", "Loaded"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1", "Toolchanger Tools"});
    helper.feed_afc_state(afc_state);

    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] = nlohmann::json::array({"extruder"});
    bt_data["hubs"] = nlohmann::json::array({"Turtle_1"});
    bt_data["buffers"] = nlohmann::json::array();

    nlohmann::json tools_data;
    tools_data["lanes"] = nlohmann::json::array({"lane4"});
    tools_data["extruders"] = nlohmann::json::array({"extruder"});
    tools_data["hubs"] = nlohmann::json::array();
    tools_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_Toolchanger Tools"] = tools_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 2);
    REQUIRE(unit_infos[0].topology == PathTopology::HUB);
    REQUIRE(unit_infos[1].lane_is_hub_routed == std::vector<bool>{false});
    REQUIRE(unit_infos[1].topology == PathTopology::HUB);

    // The printer's extruder list arrives in a later frame than the unit objects.
    helper.feed_afc_state({{"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}}});
    REQUIRE(helper.get_unit_infos()[1].topology == PathTopology::PARALLEL);
    const auto info = helper.get_system_info();
    const auto tools = std::find_if(info.units.begin(), info.units.end(),
                                    [](const AmsUnit& u) { return u.name == "Toolchanger Tools"; });
    REQUIRE(tools != info.units.end());
    REQUIRE(tools->topology == PathTopology::PARALLEL);
}

TEST_CASE("AFC unit topology: all-direct lanes need an extruder per lane to be PARALLEL",
          "[ams][afc][topology]") {
    using helix::printer::afc_unit_topology;
    // One direct lane on its own extruder of a toolchanger: nothing to merge.
    CHECK(afc_unit_topology({true, false, 1, 1, 0, 4}) == PathTopology::PARALLEL);
    // The same lane on a single-extruder printer shares the nozzle with the other
    // units, so a load must unload first.
    CHECK(afc_unit_topology({true, false, 1, 1, 0, 1}) == PathTopology::HUB);
    // The printer's extruder count has not arrived: stay with the shared answer.
    CHECK(afc_unit_topology({true, false, 1, 1, 0, 0}) == PathTopology::HUB);
    // Four direct lanes on four extruders: a toolchanger unit.
    CHECK(afc_unit_topology({true, false, 4, 4, 0}) == PathTopology::PARALLEL);
    // Four "direct" lanes on one extruder must still merge somewhere.
    CHECK(afc_unit_topology({true, false, 4, 1, 0}) == PathTopology::HUB);
    CHECK(afc_unit_topology({true, true, 4, 2, 1}) == PathTopology::MIXED);
    CHECK(afc_unit_topology({false, true, 4, 1, 1}) == PathTopology::HUB);
    // No routing known yet: the counts decide.
    CHECK(afc_unit_topology({false, false, 4, 1, 1}) == PathTopology::HUB);
    CHECK(afc_unit_topology({false, false, 4, 4, 0}) == PathTopology::PARALLEL);
}

TEST_CASE("AFC system topology follows its units: a toolchanger unit reports PARALLEL",
          "[ams][afc][topology]") {
    // The filament path canvas renders from get_topology(), so the system-level
    // answer decides whether the panel draws one nozzle behind a hub or one per
    // toolhead. A Box Turtle in toolchanger mode feeds four of them.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array();
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = unit_data;
    helper.feed_status_update(params);

    REQUIRE(helper.get_unit_topology(0) == PathTopology::PARALLEL);
    REQUIRE(helper.get_topology() == PathTopology::PARALLEL);
}

TEST_CASE("AFC system topology is MIXED when its units disagree", "[ams][afc][topology]") {
    // One installation may carry both shapes at once: a Box Turtle feeding four
    // toolheads beside an OpenAMS merging four lanes into one. No single unit's
    // answer describes the machine, so the system reports MIXED and the renderer
    // falls to the per-lane routing vector.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

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

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 2);
    REQUIRE(unit_infos[0].topology != unit_infos[1].topology);
    REQUIRE(helper.get_topology() == PathTopology::MIXED);
}

TEST_CASE("AFC unit with hubs AND multiple extruders gets PARALLEL topology",
          "[ams][afc][mixed][toolchanger]") {
    // Toolchanger scenario: HTLF unit has 4 lanes, 3 extruders, 1 hub.
    // Lanes 1,2 go direct to extruder/extruder1, lanes 3,4 go through hub to extruder2.
    // Must be PARALLEL so each unique mapped_tool gets its own physical nozzle.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(3);

    // Feed AFC state with unit name
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"HTLF HTLF_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object: hubs present + 3 extruders (mixed topology)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder", "extruder1", "extruder2"});
    unit_data["hubs"] = nlohmann::json::array({"hub1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_HTLF HTLF_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is PARALLEL (not HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::PARALLEL);

    // Feed stepper map data: lanes 0,1 direct, lanes 2,3 shared via hub
    helper.feed_afc_stepper(
        "lane0",
        {{"map", "T0"}, {"extruder", "extruder"}, {"status", "Loaded"}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T2"}, {"extruder", "extruder1"}, {"status", "Loaded"}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"map", "T1"}, {"extruder", "extruder2"}, {"status", "Loaded"}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"map", "T3"}, {"extruder", "extruder2"}, {"status", "Loaded"}, {"color", "FFFF00"}});

    // Verify tool mappings
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    auto* slot1 = info.get_slot_global(1);
    auto* slot2 = info.get_slot_global(2);
    auto* slot3 = info.get_slot_global(3);
    REQUIRE(slot0->mapped_tool == 0);
    REQUIRE(slot1->mapped_tool == 2);
    REQUIRE(slot2->mapped_tool == 1);
    REQUIRE(slot3->mapped_tool == 3); // Shared extruder2 via hub

    // Verify extruder_name is populated from AFC stepper data
    CHECK(slot0->extruder_name == "extruder");
    CHECK(slot1->extruder_name == "extruder1");
    CHECK(slot2->extruder_name == "extruder2");
    CHECK(slot3->extruder_name == "extruder2");
}

TEST_CASE("AFC hub-routed lanes with per-lane extruders classified as PARALLEL",
          "[ams][afc][topology]") {
    // ACE Pro as lane loader through AFC: 4 lanes, each with its own hub AND its
    // own extruder, all under one unit. Each lane independently feeds a different
    // toolhead through a hub sensor — this is parallel topology, not hub merger.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Feed stepper data: each lane has its own hub and extruder
    helper.feed_afc_stepper("lane0", {{"hub", "ace_hub1"},
                                      {"extruder", "extruder"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"color", "89a84f"}});
    helper.feed_afc_stepper("lane1", {{"hub", "ace_hub2"},
                                      {"extruder", "extruder1"},
                                      {"map", "T1"},
                                      {"status", "Loaded"},
                                      {"color", "23ADE9"}});
    helper.feed_afc_stepper("lane2", {{"hub", "ace_hub3"},
                                      {"extruder", "extruder2"},
                                      {"map", "T2"},
                                      {"status", "Loaded"},
                                      {"color", "F20808"}});
    helper.feed_afc_stepper("lane3", {{"hub", "ace_hub4"},
                                      {"extruder", "extruder3"},
                                      {"map", "T3"},
                                      {"status", "Loaded"},
                                      {"color", "DE4343"}});

    // Feed AFC state with unit
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"ACE Ace_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object: 4 hubs + 4 extruders (parallel with hubs)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array({"ace_hub1", "ace_hub2", "ace_hub3", "ace_hub4"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_ACE Ace_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is PARALLEL (not HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::PARALLEL);

    // All lanes are hub-routed (each through its own hub)
    REQUIRE(unit_infos[0].lane_is_hub_routed.size() == 4);
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == true);
    }

    // Verify tool mappings are 1:1
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    auto* slot1 = info.get_slot_global(1);
    auto* slot2 = info.get_slot_global(2);
    auto* slot3 = info.get_slot_global(3);
    REQUIRE(slot0->mapped_tool == 0);
    REQUIRE(slot1->mapped_tool == 1);
    REQUIRE(slot2->mapped_tool == 2);
    REQUIRE(slot3->mapped_tool == 3);
}

TEST_CASE("AFC hub-routed lanes with shared extruder still classified as HUB",
          "[ams][afc][topology]") {
    // Standard Box Turtle / OpenAMS: 4 lanes through hubs but only 1 extruder.
    // Must remain HUB topology — the new parallel-with-hubs check should NOT
    // trigger when extruder count != lane count.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Feed stepper data: all lanes through per-lane hubs but same extruder
    helper.feed_afc_stepper(
        "lane0",
        {{"hub", "Hub_1"}, {"extruder", "extruder4"}, {"status", "Loaded"}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"hub", "Hub_2"}, {"extruder", "extruder4"}, {"status", "Loaded"}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"hub", "Hub_3"}, {"extruder", "extruder4"}, {"status", "Loaded"}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"hub", "Hub_4"}, {"extruder", "extruder4"}, {"status", "Loaded"}, {"color", "FFFF00"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder4"});
    unit_data["hubs"] = nlohmann::json::array({"Hub_1", "Hub_2", "Hub_3", "Hub_4"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_OpenAMS AMS_1"] = unit_data;
    helper.feed_status_update(params);

    // Must still be HUB — 4 hubs but only 1 extruder
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::HUB);
}

TEST_CASE("AFC backend unit object triggers lane reorganization", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed flat string units
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit-level data for both units
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

    // After both units are parsed, units should be reorganized
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    // Units sorted alphabetically: AMS_1 before Turtle_1
    // (reorganize_slots sorts unit names)
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[1].slot_count == 4);
    REQUIRE(info.total_slots == 8);
}

// 2c: AFC_lane status updates

TEST_CASE("AFC backend handles AFC_lane status updates", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed an AFC_lane update (same schema as AFC_stepper)
    nlohmann::json lane_data;
    lane_data["name"] = "lane4";
    lane_data["unit"] = "AMS_1";
    lane_data["hub"] = "Hub_1";
    lane_data["extruder"] = "extruder4";
    lane_data["buffer"] = nullptr;
    lane_data["prep"] = true;
    lane_data["load"] = true;
    lane_data["tool_loaded"] = false;
    lane_data["loaded_to_hub"] = false;
    lane_data["material"] = "PLA";
    lane_data["spool_id"] = 13;
    lane_data["color"] = "#000000";
    lane_data["weight"] = 295.25;
    lane_data["map"] = "T4";
    lane_data["status"] = "Loaded";
    lane_data["filament_status"] = "Ready";
    lane_data["dist_hub"] = 60;

    // Feed as AFC_lane (not AFC_stepper)
    nlohmann::json params;
    params["AFC_lane lane4"] = lane_data;
    helper.feed_status_update(params);

    // Verify the lane was parsed using apply_lane_status (same JSON schema)
    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(4);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->material == "PLA");
    REQUIRE(slot->mapped_tool == 4);
    REQUIRE(slot->color_rgb == 0x000000);
    // AFC "Loaded" means hub-loaded, tool_loaded=false → AVAILABLE, not LOADED
    REQUIRE(slot->status == SlotStatus::AVAILABLE);
}

TEST_CASE("AFC backend handles mix of AFC_stepper and AFC_lane in same update",
          "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed both AFC_stepper and AFC_lane in same notification
    nlohmann::json stepper_data;
    stepper_data["prep"] = true;
    stepper_data["load"] = true;
    stepper_data["material"] = "PETG";
    stepper_data["color"] = "#FF0000";
    stepper_data["map"] = "T0";
    stepper_data["status"] = "Loaded";

    nlohmann::json lane_data;
    lane_data["prep"] = true;
    lane_data["load"] = true;
    lane_data["material"] = "ABS";
    lane_data["color"] = "#00FF00";
    lane_data["map"] = "T4";
    lane_data["status"] = "Loaded";

    nlohmann::json params;
    params["AFC_stepper lane0"] = stepper_data;
    params["AFC_lane lane4"] = lane_data;
    helper.feed_status_update(params);

    // Both should be parsed
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    REQUIRE(slot0 != nullptr);
    REQUIRE(slot0->material == "PETG");

    auto* slot4 = info.get_slot_global(4);
    REQUIRE(slot4 != nullptr);
    REQUIRE(slot4->material == "ABS");
}

// 2d: Multiple AFC_extruder objects

TEST_CASE("AFC backend handles multiple AFC_extruder objects", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Set extruder names from AFC state
    nlohmann::json afc_state;
    afc_state["extruders"] = nlohmann::json::array(
        {"extruder", "extruder1", "extruder2", "extruder3", "extruder4", "extruder5"});
    helper.feed_afc_state(afc_state);

    // Verify extruder_names_ populated
    const auto& ext_names = helper.get_extruder_names();
    REQUIRE(ext_names.size() == 6);

    // Feed multiple extruder updates
    nlohmann::json params;
    params["AFC_extruder extruder4"] = {
        {"tool_start_status", true}, {"tool_end_status", false}, {"lane_loaded", "lane4"}};
    params["AFC_extruder extruder5"] = {
        {"tool_start_status", false}, {"tool_end_status", false}, {"lane_loaded", nullptr}};
    helper.feed_status_update(params);

    // Verify current slot updated from extruder4's lane_loaded
    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 4);
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes,
                 "AFC backend multi-extruder backward compat: single extruder still works",
                 "[ams][afc][mixed]") {
    // Do NOT set extruder_names_ (empty = backward compat)
    // Feed single AFC_extruder extruder (old format)
    nlohmann::json params;
    params["AFC_extruder extruder"] = {
        {"tool_start_status", true}, {"tool_end_status", true}, {"lane_loaded", "lane0"}};
    helper.feed_status_update(params);

    // Should still work via backward-compat fallback
    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 0);
    REQUIRE(helper.get_tool_start_sensor() == true);
    REQUIRE(helper.get_tool_end_sensor() == true);
}

TEST_CASE("AFC backend stores extruder names from AFC state extruders array", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);

    nlohmann::json afc_state;
    afc_state["extruders"] = nlohmann::json::array({"extruder", "extruder1"});
    helper.feed_afc_state(afc_state);

    const auto& names = helper.get_extruder_names();
    REQUIRE(names.size() == 2);
    REQUIRE(names[0] == "extruder");
    REQUIRE(names[1] == "extruder1");
}

// Backward compatibility

TEST_CASE_METHOD(AfcFourLanes, "AFC backend backward compat: object-format units still works",
                 "[ams][afc][mixed]") {
    // Old format: units as objects with name and lanes
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Box Turtle 1";
    unit_obj["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    unit_obj["connected"] = true;
    afc_state["units"] = nlohmann::json::array({unit_obj});
    helper.feed_afc_state(afc_state);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].name == "Box Turtle 1");
    // unit_infos_ should be empty (object format doesn't populate it)
    REQUIRE(helper.get_unit_infos().empty());
}

TEST_CASE("AFC backend backward compat: mixed string and object units", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Mix of string and object units (shouldn't happen in practice, but be robust)
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Old Turtle";
    unit_obj["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1", unit_obj});
    helper.feed_afc_state(afc_state);

    // String unit creates unit_info, object unit goes through old path
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].name == "AMS_1");
}

// Backward compatibility tests

TEST_CASE("AFC get_unit_topology falls back to get_topology when unit_infos empty",
          "[ams][afc][backward_compat]") {
    // Standard non-mixed AFC: unit_infos_ is empty, so get_unit_topology()
    // should fall back to get_topology() which returns HUB.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // No flat string units fed — unit_infos_ is empty
    REQUIRE(helper.get_unit_infos().empty());

    // get_unit_topology() for any index should fall back to get_topology() = HUB
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(1) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(-1) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(99) == PathTopology::HUB);
}

TEST_CASE("AFC get_topology still returns HUB for standard AFC", "[ams][afc][backward_compat]") {
    // get_topology() always returns HUB for the AFC backend
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.get_topology() == PathTopology::HUB);
}

TEST_CASE("AFC backend with only AFC_stepper lanes works correctly (no AFC_lane)",
          "[ams][afc][backward_compat]") {
    // Standard Box Turtle setup: only AFC_stepper objects, no AFC_lane objects.
    // The AFC_lane loop should simply skip when no AFC_lane objects exist.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed only AFC_stepper updates (standard non-mixed Box Turtle)
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", false},
                                      {"material", "PLA"},
                                      {"color", "#FF0000"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"weight", 850}});
    helper.feed_afc_stepper("lane2", {{"prep", true},
                                      {"load", false},
                                      {"material", "PETG"},
                                      {"color", "#00FF00"},
                                      {"map", "T1"},
                                      {"status", "Loaded"}});

    // Verify stepper data parsed correctly
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    REQUIRE(slot0 != nullptr);
    REQUIRE(slot0->material == "PLA");
    REQUIRE(slot0->color_rgb == 0xFF0000);
    REQUIRE(slot0->mapped_tool == 0);
    // AFC "Loaded" with no tool_loaded → AVAILABLE (hub-loaded only)
    REQUIRE(slot0->status == SlotStatus::AVAILABLE);

    auto* slot1 = info.get_slot_global(1);
    REQUIRE(slot1 != nullptr);
    REQUIRE(slot1->material == "PETG");
    REQUIRE(slot1->color_rgb == 0x00FF00);
    REQUIRE(slot1->mapped_tool == 1);

    // Sensors should work via AFC_stepper path
    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.prep == true);
    REQUIRE(sensors.load == true);
    REQUIRE(sensors.loaded_to_hub == false);

    // Topology should still be HUB (standard AFC)
    REQUIRE(helper.get_topology() == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
}

TEST_CASE("AFC standard single-unit system unchanged by mixed topology code",
          "[ams][afc][backward_compat]") {
    // Verify that feeding a standard single-unit AFC state (object format)
    // does not populate unit_infos_ and preserves the old unit structure
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed old-format AFC state with object-style unit
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Box Turtle 1";
    unit_obj["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    unit_obj["connected"] = true;
    afc_state["units"] = nlohmann::json::array({unit_obj});
    afc_state["current_state"] = "Idle";
    helper.feed_afc_state(afc_state);

    // unit_infos_ should remain empty (object format does not populate it)
    REQUIRE(helper.get_unit_infos().empty());

    // Standard unit structure should still be correct
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].name == "Box Turtle 1");
    REQUIRE(info.units[0].slot_count == 4);

    // Topology falls back to HUB
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
    REQUIRE(helper.get_action() == AmsAction::IDLE);
}

// eject_lane() Tests

// LANE_UNLOAD names the lane by its AFC name, for any lane including one other
// than the loaded slot; a slot outside the lanes is refused, and so is any lane
// while the backend is not running.
TEST_CASE("AFC eject_lane sends LANE_UNLOAD for a valid lane", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    SECTION("not running") {
        CHECK(helper.eject_lane(0).result == AmsResult::NOT_CONNECTED);
        CHECK(helper.captured_gcodes.empty());
    }

    helper.set_running(true);
    SECTION("first lane") {
        REQUIRE(helper.eject_lane(0).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
    }
    SECTION("another lane while slot 0 is loaded") {
        helper.set_filament_loaded(true);
        helper.set_current_slot(0);
        REQUIRE(helper.eject_lane(2).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane3"));
    }
    SECTION("out of range") {
        CHECK(helper.eject_lane(99).result == AmsResult::INVALID_SLOT);
        CHECK(helper.captured_gcodes.empty());
    }
}

// eject_lane() sends LANE_UNLOAD unconditionally
//
// AFC's own cmd_LANE_UNLOAD decides whether to refuse, and AFC's maintainer asked
// us not to gate their macros (prestonbrown/helixscreen#1258); there is no
// reliable AFC version to mirror its rules by either. Every case here is a state
// AFC may refuse, keyed on lane_loaded, hub routing or is_standalone, and every
// one must dispatch.

TEST_CASE("AFC eject_lane dispatches regardless of upstream refusal conditions",
          "[ams][afc][eject]") {
    SECTION("lane seated at its own extruder") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.set_extruder_lane_loaded("extruder", "lane1");

        REQUIRE(helper.eject_lane(0).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
    }

    SECTION("lane routed direct rather than through a hub") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.set_lane_hub_routing("lane1", "direct");

        REQUIRE(helper.eject_lane(0).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
    }

    SECTION("standalone extruder holding nothing") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.report_extruder_standalone("extruder", true);
        helper.set_extruder_lane_loaded("extruder", "");

        REQUIRE(helper.eject_lane(0).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
    }

    SECTION("aggregate state says this slot is the loaded one") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.set_filament_loaded(true);
        helper.set_current_slot(1);

        REQUIRE(helper.eject_lane(1).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane2"));
    }
}
