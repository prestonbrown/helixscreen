// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 1 to 36 of test_ams_backend_afc.cpp before it was split, in its order. Mostly
// [persistence], [discovery], [tool_mapping][routing][print_select],
// [tool_mapping][routing][print_start], [endless_spool][phase1], and more

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

// AFC version is informational only
//
// Nothing in AFC writes the afc-install Moonraker namespace (AFC #451), so the
// version string there is absent or frozen: a BoxTurtle whose payload is
// 1.0.32-era reports "1.0.0". Capabilities are feature-detected from the data;
// these tests pin that no behavior keys off the reported version.

TEST_CASE("AFC persistence is independent of the reported version", "[ams][afc][version]") {
    for (const char* version : {"1.0.0", "1.0.19", "unknown", "", "9.9.9"}) {
        AmsBackendAfcTestHelper helper;
        CAPTURE(version);
        helper.set_afc_version(version);
        helper.initialize_test_lanes_with_slots(4);

        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        info.remaining_weight_g = 850;
        info.spoolman_id = 42;
        helix::test::apply_edit(helper, 0, info);

        // "1.0.0" is what a BoxTurtle running v1.1.0-4 reports: nothing writes the
        // afc-install namespace, so a version gate silently drops every save (#644).
        CHECK(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=FF0000"));
        CHECK(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=PLA"));
        CHECK(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=850"));
        CHECK(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
    }
}

// compute_filament_segment_unlocked() - Sensor-to-Segment Mapping Tests

// The furthest point the sensors place the active filament. Toolhead sensors
// beat the hub, the hub beats the lane sensors, and the end sensor alone means
// NOZZLE. Without a current lane (or with one not in the map) every lane is
// scanned in index order and the first lane with any sensor wins, load before
// prep within a lane. The latched loaded_to_hub field is never a position: it
// reads true on every prepped lane forever. Filament flagged loaded or a slot
// selected with no sensor at all reads SPOOL.
TEST_CASE("AFC segment follows the furthest triggered sensor", "[ams][afc][segment]") {
    using H = AmsBackendAfcTestHelper;
    struct Row {
        const char* name;
        std::function<void(H&)> setup;
        PathSegment expected;
        bool with_units = false; // the named-hub rows need the unit layout
    };
    const Row rows[] = {
        {"no sensors", [](H&) {}, PathSegment::NONE},
        {"loaded flag, no sensors", [](H& h) { h.set_filament_loaded(true); }, PathSegment::SPOOL},
        {"current slot, no sensors", [](H& h) { h.set_current_slot(0); }, PathSegment::SPOOL},
        {"prep",
         [](H& h) {
             h.set_current_lane("lane1");
             h.set_lane_prep_sensor(0, true);
         },
         PathSegment::PREP},
        {"prep and load",
         [](H& h) {
             h.set_current_lane("lane1");
             h.set_lane_prep_sensor(0, true);
             h.set_lane_load_sensor(0, true);
         },
         PathSegment::LANE},
        {"hub sensor",
         [](H& h) {
             h.set_current_lane("lane1");
             h.set_lane_loaded_to_hub(0, true);
             h.set_hub_sensor(true);
         },
         PathSegment::OUTPUT},
        {"hub beats every lane sensor",
         [](H& h) {
             h.set_current_lane("lane1");
             h.set_lane_prep_sensor(0, true);
             h.set_lane_load_sensor(0, true);
             h.set_lane_loaded_to_hub(0, true);
             h.set_hub_sensor(true);
         },
         PathSegment::OUTPUT},
        {"tool start beats the hub",
         [](H& h) {
             h.set_hub_sensor(true);
             h.set_tool_start_sensor(true);
         },
         PathSegment::TOOLHEAD},
        {"tool end",
         [](H& h) {
             h.set_hub_sensor(true);
             h.set_tool_start_sensor(true);
             h.set_tool_end_sensor(true);
         },
         PathSegment::NOZZLE},
        {"tool end alone", [](H& h) { h.set_tool_end_sensor(true); }, PathSegment::NOZZLE},
        {"scan finds prep", [](H& h) { h.set_lane_prep_sensor(2, true); }, PathSegment::PREP},
        {"scan finds load", [](H& h) { h.set_lane_load_sensor(1, true); }, PathSegment::LANE},
        {"scan reaches the last lane", [](H& h) { h.set_lane_load_sensor(3, true); },
         PathSegment::LANE},
        {"scan stops at the first lane with a sensor",
         [](H& h) {
             h.set_lane_prep_sensor(0, true);
             h.set_lane_load_sensor(1, true);
         },
         PathSegment::PREP},
        {"scan takes load before prep within a lane",
         [](H& h) {
             h.set_lane_load_sensor(0, true);
             h.set_lane_prep_sensor(0, true);
         },
         PathSegment::LANE},
        {"a current lane not in the map scans",
         [](H& h) {
             h.set_current_lane("nonexistent");
             h.set_lane_prep_sensor(0, true);
         },
         PathSegment::PREP},
        {"latched loaded_to_hub with the hub clear",
         [](H& h) {
             h.set_hub_sensor("Turtle_1", false);
             h.set_lane_loaded_to_hub(0, true);
             h.set_lane_load_sensor(0, true);
             h.set_lane_prep_sensor(0, true);
         },
         PathSegment::LANE, true},
        {"named hub sensor triggered",
         [](H& h) {
             h.set_lane_loaded_to_hub(0, false);
             h.set_hub_sensor("Turtle_1", true);
         },
         PathSegment::OUTPUT, true},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            H helper;
            if (r.with_units) {
                helper.initialize_test_lanes_with_slots(4);
            } else {
                helper.initialize_test_lanes(4);
            }
            r.setup(helper);
            CHECK(helper.test_compute_filament_segment() == r.expected);
        }
    }

    SECTION("no lanes initialized") {
        H helper;
        CHECK(helper.test_compute_filament_segment() == PathSegment::NONE);
    }
}

TEST_CASE("AFC get_slot_filament_segment ignores the latched loaded_to_hub field "
          "for a non-active slot",
          "[ams][afc][segment]") {
    // Same defect as compute_filament_segment_unlocked(), in the per-slot accessor
    // that drives per-lane path rendering on the AMS panel: loaded_to_hub is
    // latched at prep and never updated, so a non-active lane that was ever
    // prepped reads "at hub" forever, regardless of where its filament actually
    // is. There is no per-slot hub sensor for a non-active slot, so load/prep
    // remain the furthest this can honestly report.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_hub_sensor("Turtle_1", false);
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_lane_load_sensor(0, true);

    // Slot 0 is not the active slot (current_slot defaults to -1), so this
    // exercises the non-active-slot branch of get_slot_filament_segment().
    REQUIRE(helper.get_slot_filament_segment(0) == PathSegment::LANE);
    // A lane an extruder holds is at that toolhead, the current lane or not.
    helper.set_extruder_lane_loaded("extruder1", "lane1");
    CHECK(helper.get_slot_filament_segment(0) == PathSegment::NOZZLE);
}

// set_discovered_lanes() - Lane Discovery from PrinterCapabilities Tests

TEST_CASE("AFC set_discovered_lanes: sets lane names correctly", "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};

    helper.set_discovered_lanes(lanes, hubs);

    // After setting lanes and initializing, they should be accessible via registry
    helper.initialize_slots_from_discovery();
    REQUIRE(helper.get_slot_count() == 4);
    REQUIRE(helper.get_slot_name(0) == "lane1");
    REQUIRE(helper.get_slot_name(3) == "lane4");
}

TEST_CASE("AFC set_discovered_lanes: sets hub names correctly", "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    std::vector<std::string> lanes = {"lane1", "lane2"};
    std::vector<std::string> hubs = {"Turtle_1", "Turtle_2"};

    helper.set_discovered_lanes(lanes, hubs);

    REQUIRE(helper.get_hub_names().size() == 2);
    REQUIRE(helper.get_hub_names()[0] == "Turtle_1");
}

TEST_CASE("AFC set_discovered_lanes: empty lanes doesn't overwrite existing",
          "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    // First set some lanes
    std::vector<std::string> lanes = {"lane1", "lane2"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    // Then call with empty lanes - should not overwrite
    std::vector<std::string> empty_lanes;
    std::vector<std::string> new_hubs = {"NewHub"};
    helper.set_discovered_lanes(empty_lanes, new_hubs);

    // Lanes should remain unchanged (check via discovery init)
    helper.initialize_slots_from_discovery();
    REQUIRE(helper.get_slot_count() == 2);
    // But hubs should be updated
    REQUIRE(helper.get_hub_names().size() == 1);
    REQUIRE(helper.get_hub_names()[0] == "NewHub");
}

TEST_CASE("AFC segment: works with discovered lanes", "[ams][afc][discovery][segment]") {
    AmsBackendAfcTestHelper helper;

    // Set lanes via discovery (like PrinterCapabilities would)
    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    // Initialize the lanes (like start() would do)
    helper.initialize_slots_from_discovery();

    // Now test that sensors work correctly
    helper.set_current_lane("lane2");
    helper.set_lane_prep_sensor(1, true);
    helper.set_lane_load_sensor(1, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

TEST_CASE("AFC's lane subscription names every lane field the parser reads",
          "[afc][ams_status_objects]") {
    // After the first snapshot Moonraker sends only the listed fields, so a
    // key the parser reads but the list omits never updates live.
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"AFC", "AFC_stepper lane1"}));
    const nlohmann::json objects = AmsBackendAfc::required_status_objects(hw);
    REQUIRE(objects.contains("AFC_stepper lane1"));
    const nlohmann::json& lane = objects["AFC_stepper lane1"];
    REQUIRE(lane.is_array());
    std::vector<std::string> listed;
    for (const auto& field : lane) {
        listed.push_back(field.get<std::string>());
    }
    for (const char* field : {"bed_temp",
                              "buffer_status",
                              "color",
                              "dist_hub",
                              "endstops",
                              "extruder_temp",
                              "filament_name",
                              "filament_status",
                              "filament_status_led",
                              "initial_weight",
                              "load",
                              "loaded_to_hub",
                              "material",
                              "multi_color_hexes",
                              "prep",
                              "remember_spool",
                              "selector",
                              "spool_id",
                              "spool_vendor",
                              "status",
                              "td1_color",
                              "td1_scan_time",
                              "td1_td",
                              "tool_loaded",
                              "vendor_name",
                              "weight"}) {
        INFO(field);
        CHECK(std::count(listed.begin(), listed.end(), std::string(field)) == 1);
    }
}

// apply_user_edit() persistence
//
// An edit reaches AFC as SET_COLOR / SET_MATERIAL / SET_WEIGHT / SET_SPOOL_ID
// LANE=<name> through the backend's execute_gcode(), which the helper captures.

// One edited field, the write it produces. Colours go out as uppercase hex with
// no '#', and pure black is a deliberate pick that persists while the default
// grey does not (#1597). Materials keep their punctuation and are quoted when
// they hold a space, since Klipper splits extended-command args on whitespace.
// A Spoolman-linked lane sends the type the slicer matches, as its Spoolman
// filament records it, since AFC copies the lane's material into lane_data.
// Weights go out as whole grams; zero or unknown sends nothing. A spool-link
// clear sends an empty SPOOL_ID, and an unlinked lane staying unlinked sends
// nothing.
TEST_CASE("AFC persistence writes each edited field in AFC's format", "[ams][afc][persistence]") {
    struct Row {
        const char* name;
        int slot;
        int linked_spool;
        std::function<void(SlotInfo&)> edit;
        const char* sent;     // must appear, or nullptr
        const char* not_sent; // no command with this prefix, or nullptr
    };
    const Row rows[] = {
        {"colour", 0, 0, [](SlotInfo& i) { i.color_rgb = 0xFF0000; },
         "SET_COLOR LANE=lane1 COLOR=FF0000", nullptr},
        {"colour on another lane", 1, 0, [](SlotInfo& i) { i.color_rgb = 0x00FF00; },
         "SET_COLOR LANE=lane2 COLOR=00FF00", nullptr},
        {"pure black", 0, 0, [](SlotInfo& i) { i.color_rgb = 0x000000; },
         "SET_COLOR LANE=lane1 COLOR=000000", nullptr},
        {"default grey", 0, 0, [](SlotInfo& i) { i.color_rgb = 0x808080; }, nullptr, "SET_COLOR"},
        {"material", 1, 0, [](SlotInfo& i) { i.material = "PLA"; },
         "SET_MATERIAL LANE=lane2 MATERIAL=PLA", nullptr},
        {"material with punctuation", 0, 0, [](SlotInfo& i) { i.material = "PLA+"; },
         "SET_MATERIAL LANE=lane1 MATERIAL=PLA+", nullptr},
        {"multi-word material", 0, 0, [](SlotInfo& i) { i.material = "Silk PLA"; },
         "SET_MATERIAL LANE=lane1 MATERIAL=\"Silk PLA\"", nullptr},
        {"linked lane's material", 0, 0,
         [](SlotInfo& i) {
             i.spoolman_id = 152;
             i.material = "ABS+";
         },
         "SET_MATERIAL LANE=lane1 MATERIAL=ABS", nullptr},
        {"empty material", 0, 0, [](SlotInfo& i) { i.material = ""; }, nullptr, "SET_MATERIAL"},
        {"fractional weight", 0, 0, [](SlotInfo& i) { i.remaining_weight_g = 850.5f; },
         "SET_WEIGHT LANE=lane1 WEIGHT=850", nullptr},
        {"zero weight", 0, 0, [](SlotInfo& i) { i.remaining_weight_g = 0; }, nullptr, "SET_WEIGHT"},
        {"unknown weight", 0, 0, [](SlotInfo& i) { i.remaining_weight_g = -1; }, nullptr,
         "SET_WEIGHT"},
        {"spool link", 0, 0, [](SlotInfo& i) { i.spoolman_id = 42; },
         "SET_SPOOL_ID LANE=lane1 SPOOL_ID=42", nullptr},
        {"spool link cleared", 0, 123, [](SlotInfo& i) { i.spoolman_id = 0; },
         "SET_SPOOL_ID LANE=lane1 SPOOL_ID=", nullptr},
        {"unlinked stays unlinked", 0, 0, [](SlotInfo& i) { i.spoolman_id = 0; }, nullptr,
         "SET_SPOOL_ID"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            AmsBackendAfcTestHelper helper;
            helper.initialize_test_lanes_with_slots(4);
            helper.get_mutable_slot(r.slot)->spoolman_id = r.linked_spool;

            SlotInfo info;
            r.edit(info);
            AmsError err = helix::test::apply_edit(helper, r.slot, info);

            CHECK(err.success());
            if (r.sent) {
                CHECK(helper.has_gcode(r.sent));
            }
            if (r.not_sent) {
                CHECK_FALSE(helper.has_gcode_starting_with(r.not_sent));
            }
        }
    }
}

// On AFC, SET_SPOOL_ID with an empty value is not a narrow unlink: AFC_spool.py's
// set_spoolID() routes an empty/None id into clear_values(), which wipes material,
// color, weight and both temps (and calls clear_lane_data()). Emitting it LAST
// therefore destroys the SET_COLOR / SET_MATERIAL / SET_WEIGHT sent earlier in the
// same save. Observed on the .112 BoxTurtle: a save emitted COLOR/MATERIAL/WEIGHT
// then SPOOL_ID=, and the editor reopened 3s later with material empty.
TEST_CASE("AFC persistence: spool-link clear is emitted before the data writes",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Lane starts linked to a Spoolman spool.
    SlotInfo* existing_slot = helper.get_mutable_slot(0);
    REQUIRE(existing_slot != nullptr);
    existing_slot->spoolman_id = 86;

    // Unlink and set fresh identity in the SAME save — the exact shape of the
    // real-world failure.
    SlotInfo info;
    info.spoolman_id = 0;
    info.material = "PLA";
    info.color_rgb = 0xE53935;
    info.remaining_weight_g = 500.0f;

    helix::test::apply_edit(helper, 0, info);

    const int clear_idx = helper.gcode_index_of("SET_SPOOL_ID LANE=lane1 SPOOL_ID=");
    const int color_idx = helper.gcode_index_of("SET_COLOR LANE=lane1");
    const int material_idx = helper.gcode_index_of("SET_MATERIAL LANE=lane1");
    const int weight_idx = helper.gcode_index_of("SET_WEIGHT LANE=lane1");

    REQUIRE(clear_idx >= 0);
    REQUIRE(color_idx >= 0);
    REQUIRE(material_idx >= 0);
    REQUIRE(weight_idx >= 0);

    // The clear must precede every data write, or AFC wipes what we just set.
    REQUIRE(clear_idx < color_idx);
    REQUIRE(clear_idx < material_idx);
    REQUIRE(clear_idx < weight_idx);
}

// The link branch is destructive in the opposite direction: AFC_spool.py's
// set_spoolID() with a valid id fetches the spool from Spoolman and overwrites
// material, color, weight, both temps, density, diameter and empty_spool_weight
// from that record. Emitting it after our own writes replaces them with
// Spoolman's values, so it must precede them too.
TEST_CASE("AFC persistence: spool-link set is emitted before the data writes",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.spoolman_id = 86;
    info.material = "PLA";
    info.color_rgb = 0xE53935;
    info.remaining_weight_g = 500.0f;

    helix::test::apply_edit(helper, 0, info);

    const int link_idx = helper.gcode_index_of("SET_SPOOL_ID LANE=lane1 SPOOL_ID=86");
    const int color_idx = helper.gcode_index_of("SET_COLOR LANE=lane1");
    const int material_idx = helper.gcode_index_of("SET_MATERIAL LANE=lane1");
    const int weight_idx = helper.gcode_index_of("SET_WEIGHT LANE=lane1");

    REQUIRE(link_idx >= 0);
    REQUIRE(color_idx >= 0);
    REQUIRE(material_idx >= 0);
    REQUIRE(weight_idx >= 0);

    REQUIRE(link_idx < color_idx);
    REQUIRE(link_idx < material_idx);
    REQUIRE(link_idx < weight_idx);
}

TEST_CASE("AFC persistence: SET_MAP fires when an edit changes mapped_tool",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Helper seeds slot 0 → T0 by default. Remap it to T2 through the slot edit path.
    SlotInfo info;
    info.mapped_tool = 2;

    helix::test::apply_edit(helper, 0, info);

    REQUIRE(helper.has_gcode("SET_MAP LANE=lane1 MAP=T2"));
    // so a later T2 resolves to this lane
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);
}

TEST_CASE("AFC persistence: SET_MAP not fired when mapped_tool unchanged",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Default mapping for slot 0 is T0. Setting same value should not emit SET_MAP.
    SlotInfo info;
    info.mapped_tool = 0;
    info.material = "PLA";

    helix::test::apply_edit(helper, 0, info);

    for (const auto& gcode : helper.captured_gcodes) {
        REQUIRE(gcode.rfind("SET_MAP ", 0) != 0);
    }
}

TEST_CASE("AFC persistence: SET_MAP not fired when caller leaves mapped_tool default (-1)",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Spoolman polling and consumption_sink build a SlotInfo from the existing slot,
    // but a misuse (default-constructed SlotInfo) must NOT clobber the live mapping.
    // The guard is `info.mapped_tool >= 0` — so mapped_tool=-1 is a no-op.
    SlotInfo info; // mapped_tool defaults to -1
    info.material = "PLA";

    helix::test::apply_edit(helper, 2, info);

    for (const auto& gcode : helper.captured_gcodes) {
        REQUIRE(gcode.rfind("SET_MAP ", 0) != 0);
    }
    // Live registry mapping for slot 2 must still be T2 (the seeded default).
    REQUIRE(helper.get_slot_mapped_tool(2) == 2);
}

TEST_CASE("AFC persistence: an unsendable material is reported, not swallowed",
          "[ams][afc][persistence]") {
    // A material that genuinely cannot be expressed as a G-code parameter still
    // must not be dropped in silence: the rest of the save goes out, and the
    // caller is told which part did not.
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = "PLA;G28";
    info.remaining_weight_g = 750.0f;

    AmsError err = helix::test::apply_edit(helper, 0, info);

    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_MATERIAL"));
    // The rest of the write is unaffected.
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=750"));
    REQUIRE_FALSE(err.success());
    REQUIRE(err.result == AmsResult::COMMAND_FAILED);
}

TEST_CASE_METHOD(AfcEditedLaneFixture, "AFC weight update without persist writes nothing durable",
                 "[ams][afc][persistence][filament_slot_override][1652]") {
    const nlohmann::json stored_before = client.mock_db_get("lane_data", "lane1");
    REQUIRE_FALSE(stored_before.is_null());
    const float recorded_before = record().remaining_weight_g;

    helper.update_slot_weight(0, 500.0f, -1.0f, /*persist=*/false);

    // The live slot takes the meter's number...
    CHECK(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(500.0f));
    // ...and nothing durable does.
    CHECK(helper.captured_gcodes.empty());
    CHECK(client.mock_db_get("lane_data", "lane1") == stored_before);
    CHECK(record().remaining_weight_g == Catch::Approx(recorded_before));
}

TEST_CASE("an AFC with no Spoolman refuses a spool link before writing it",
          "[ams][afc][persistence][review]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo linked;
    linked.material = "PLA";
    linked.spoolman_id = 7;

    SECTION("Spoolman off: refused, nothing sent") {
        // AFC publishes false for its Spoolman URL when none is configured.
        helper.feed_afc_state({{"spoolman", false}});
        helper.clear_captured_gcodes();
        const AmsError err = helix::test::apply_edit(helper, 0, linked);
        CHECK_FALSE(err.success());
        CHECK(err.user_msg == "AFC has no Spoolman configured");
        CHECK(helper.captured_gcodes.empty());
        CHECK(helper.get_slot_info(0).spoolman_id == 0);
    }
    SECTION("Spoolman on: the link is sent") {
        helper.feed_afc_state({{"spoolman", "http://192.0.2.1:7912"}});
        REQUIRE(helix::test::apply_edit(helper, 0, linked).success());
        CHECK(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=7"));
    }
}

// reset_tool_mappings() Tests

// One command resets every lane's mapping, whatever the lane count. AFC dev/1.3
// (Klipper-Add-On #832) calls the command AFC_RESET_MAPPING (older firmware:
// RESET_AFC_MAPPING) and publishes multiple_tool_mapping alongside the new name. The flag's VALUE
// is the opt-in to virtual tools and defaults false, so key presence is the version signal, never
// the value; without the key the new name is an unknown command.
TEST_CASE("AFC reset_tool_mappings sends one reset under the firmware's name",
          "[ams][afc][tool_mapping][reset]") {
    struct Row {
        const char* name;
        int lanes;
        nlohmann::json state;
        const char* gcode;
    };
    const Row rows[] = {
        {"no status yet", 4, nullptr, "RESET_AFC_MAPPING RUNOUT=no"},
        {"eight lanes", 8, nullptr, "RESET_AFC_MAPPING RUNOUT=no"},
        {"a frame without the flag", 4, {{"led_state", true}}, "RESET_AFC_MAPPING RUNOUT=no"},
        {"the flag reported false",
         4,
         {{"multiple_tool_mapping", false}},
         "AFC_RESET_MAPPING RUNOUT=no"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            AmsBackendAfcTestHelper helper;
            helper.initialize_test_lanes_with_slots(r.lanes);
            if (!r.state.is_null()) {
                helper.feed_afc_state(r.state);
            }

            REQUIRE(helper.reset_tool_mappings().success());
            CHECK(helper.captured_gcodes.size() == 1);
            CHECK(helper.has_gcode(r.gcode));
        }
    }
}

// With AFC's multiple tool mapping on, SET_MAP adds the tool to the target lane
// and leaves that lane's own tools and current_map in place, so the lane keeps
// showing its old tool. A remap is refused with a reason rather than sent.
TEST_CASE("AFC refuses a tool remap while multiple tool mapping is on",
          "[ams][afc][tool_mapping][multiple_mapping]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    SECTION("flag off: the remap is allowed and sent") {
        helper.feed_afc_state({{"multiple_tool_mapping", false}});
        CHECK(helper.can_set_tool_mapping(2, 0).success());
        REQUIRE(helper.set_tool_mapping(2, 0).success());
        CHECK(helper.has_gcode("SET_MAP LANE=lane1 MAP=T2"));

        SlotInfo info;
        info.mapped_tool = 3;
        helix::test::apply_edit(helper, 1, info);
        CHECK(helper.has_gcode("SET_MAP LANE=lane2 MAP=T3"));
        CHECK(helix::ui::lane_settings::tool_row_refusal(helper, 0, 0).empty());
    }

    SECTION("flag on: the remap is refused with a reason and nothing is sent") {
        helper.feed_afc_state({{"multiple_tool_mapping", true}});
        const AmsError err = helper.can_set_tool_mapping(2, 0);
        CHECK(err.result == AmsResult::NOT_SUPPORTED);
        CHECK_FALSE(err.user_msg.empty());
        CHECK_FALSE(helper.set_tool_mapping(2, 0).success());
        // The lane editor grays out the Assigned Tool row and says why.
        CHECK(helix::ui::lane_settings::tool_row_refusal(helper, 0, 0) == err.user_msg);
        CHECK(helix::ui::lane_settings::tool_row_refusal(helper, 0, -1) == err.user_msg);

        SlotInfo info;
        info.mapped_tool = 3;
        helix::test::apply_edit(helper, 1, info);
        for (const auto& gcode : helper.captured_gcodes) {
            INFO(gcode);
            CHECK(gcode.rfind("SET_MAP ", 0) != 0);
        }
    }

    SECTION("the flag turning off again allows the remap") {
        helper.feed_afc_state({{"multiple_tool_mapping", true}});
        helper.feed_afc_state({{"multiple_tool_mapping", false}});
        CHECK(helper.can_set_tool_mapping(2, 0).success());
    }
}

// reset_endless_spool() Tests

TEST_CASE("AFC reset_endless_spool clears all slots", "[ams][afc][endless_spool][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // Set some backups first
    helper.set_endless_spool_config(0, 1);
    helper.set_endless_spool_config(2, 3);

    auto result = helper.reset_endless_spool();

    REQUIRE(result.success());
    // Should have sent 4 SET_RUNOUT commands (one per slot)
    REQUIRE(helper.captured_gcodes.size() == 4);

    // Each should be setting RUNOUT=NONE to disable
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane1 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane2 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane3 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane4 RUNOUT=NONE"));
}

TEST_CASE("AFC reset_endless_spool with no lanes yet refuses instead of silently succeeding",
          "[ams][afc][endless_spool][reset]") {
    AmsBackendAfcTestHelper helper;
    // Don't initialize any lanes or configs — AFC always advertises the mapping as
    // editable, so without a slot-count guard the loop is skipped and the caller is
    // told the wipe succeeded. The UI confirms a destructive warning before calling
    // this, so a silent no-op is worse than a refusal.

    auto result = helper.reset_endless_spool();

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::NOT_SUPPORTED);
    REQUIRE_FALSE(result.user_msg.empty());
    REQUIRE(helper.captured_gcodes.empty());
}

// Critical data sync tests
//
// These tests verify parsing of fields that the real AFC device exposes
// (captured from 192.168.1.112). Tests use fixture data to validate that
// state updates flow through correctly to internal state.

TEST_CASE_METHOD(LVGLTestFixture, "AFC status label follows a language switch with no new frame",
                 "[ams][afc][state][i18n]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.set_narration_phase(-1, "");
    helix::test::RegisteredBackend<AmsBackendAfcTestHelper> afc;
    const auto shown = [&] {
        return std::string(lv_subject_get_string(ams.get_ams_action_detail_subject()));
    };

    std::optional<ScopedLanguage> lang(std::in_place, "de");
    (*afc).feed_afc_state({{"current_state", "Moving"}});
    helix::ui::refresh_display_texts();
    const std::string german = shown();
    CHECK(german == lv_tr("Moving lane"));

    lang.emplace("fr");
    helix::ui::refresh_display_texts();
    CHECK(shown() == lv_tr("Moving lane"));
    CHECK(shown() != german);
}

TEST_CASE("AFC action comes from current_state; a stray AFC.status is ignored",
          "[ams][afc][state][deadfields]") {
    // AFC.get_status() publishes current_state and no "status" key at all
    // (AFC.py v1.2.0:2531-2564; same shape at v1.1.0). "status" exists on
    // AFC_lane and AFC_extruder, which have their own parsers — reading it off
    // the AFC object was a second, contradictory action writer that could never
    // fire on real firmware.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    SECTION("current_state drives the action") {
        helper.feed_afc_state({{"current_state", "Loading"}});
        REQUIRE(helper.get_action() == AmsAction::LOADING);
    }

    SECTION("a status key never overrides current_state") {
        helper.feed_afc_state({{"current_state", "Idle"}, {"status", "Loading"}});
        REQUIRE(helper.get_action() == AmsAction::IDLE);
    }

    SECTION("a status key on its own moves nothing") {
        helper.feed_afc_state({{"current_state", "Idle"}});
        REQUIRE(helper.get_action() == AmsAction::IDLE);

        helper.feed_afc_state({{"status", "Loading"}});
        REQUIRE(helper.get_action() == AmsAction::IDLE);
    }
}

TEST_CASE("AFC tool mapping from stepper map field", "[ams][afc][tool_mapping][phase1]") {
    // Real device: AFC_stepper lane1 has "map": "T0", lane2 has "map": "T1", etc.
    // We never parse this field today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed stepper data with map field
    helper.feed_afc_stepper("lane1", {{"map", "T0"}, {"prep", true}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"}, {"prep", true}});
    helper.feed_afc_stepper("lane3", {{"map", "T2"}, {"prep", false}});
    helper.feed_afc_stepper("lane4", {{"map", "T3"}, {"prep", false}});

    // tool_to_slot_map should reflect the mapping from stepper "map" fields
    auto mapping = helper.get_tool_mapping();
    REQUIRE(mapping.size() == 4);
    REQUIRE(mapping[0] == 0); // T0 → lane1 (slot 0)
    REQUIRE(mapping[1] == 1); // T1 → lane2 (slot 1)
    REQUIRE(mapping[2] == 2); // T2 → lane3 (slot 2)
    REQUIRE(mapping[3] == 3); // T3 → lane4 (slot 3)
}

TEST_CASE("AFC lane remap is the routing an unremapped print runs with",
          "[ams][afc][tool_mapping][routing]") {
    // A print using T1 must find lane4, and must not move T1 back onto lane2 at
    // print start.
    AmsBackendAfcTestHelper helper;
    feed_lane4_stands_in_for_lane2(helper);

    const auto live = helper.get_tool_mapping();
    REQUIRE(live.size() == 4);
    REQUIRE(live[1] == 3);

    const helix::FirmwareRouting routing = helper.firmware_default_routing();
    CHECK(routing.head(0) == 0);
    CHECK(routing.head(1) == 3);
    CHECK(routing.head(2) == 2);
    CHECK(routing.head(3) == 1);

    std::vector<helix::AvailableSlot> slots;
    for (int i = 0; i < 4; ++i) {
        const SlotInfo info = helper.get_slot_info(i);
        helix::AvailableSlot as{};
        as.slot_index = i;
        as.backend_index = 0;
        as.color_rgb = info.color_rgb;
        as.is_empty = info.status == SlotStatus::EMPTY || info.status == SlotStatus::UNKNOWN;
        as.current_tool_mapping = info.mapped_tool;
        slots.push_back(as);
    }
    REQUIRE(slots[1].is_empty);
    REQUIRE_FALSE(slots[3].is_empty);

    const std::vector<helix::GcodeToolInfo> tools{tool_of_unknown_color(1)};

    // The positional seed (auto color map off) is what a print runs with when
    // the user picks nothing.
    const auto mappings = helix::FilamentMapper::effective_mappings(tools, slots, false, routing);
    REQUIRE(mappings.size() == 1);
    CHECK(mappings[0].mapped_slot == 3);

    const auto preflight =
        helix::PreflightValidator::validate(tools, slots, mappings, routing, false, false);
    CHECK_FALSE(preflight.has_block());

    // PrintStartController sends set_tool_mapping() (SET_MAP) for every mapping
    // that differs from the live map; the seed must match it so nothing is sent.
    CHECK(mappings[0].mapped_slot == live[1]);
    CHECK(helix::FilamentMapper::identity_filtered_remap(mappings, routing).empty());
}

TEST_CASE("AFC print start leaves a printer-side lane remap alone",
          "[ams][afc][tool_mapping][routing][print_start]") {
    AfcPrintStartHarness h{{tool_of_unknown_color(1)}};
    REQUIRE(h.view.get_filament_mappings().size() == 1);
    CHECK(h.view.get_filament_mappings()[0].mapped_slot == 3);

    SECTION("an unremapped print sends no SET_MAP") {
        CHECK_FALSE(PrintStartControllerTestAccess::apply_remaps(h.controller));
        CHECK(set_map_commands(h.reg->captured_gcodes) == std::vector<std::string>{});
    }

    SECTION("a remap the user picks still sends exactly its SET_MAP") {
        helix::ToolMapping pick;
        pick.tool_index = 1;
        pick.mapped_slot = 2;
        pick.mapped_backend = 0;
        pick.user_picked = true;
        h.view.set_filament_mappings({pick});

        CHECK(PrintStartControllerTestAccess::apply_remaps(h.controller));
        CHECK(set_map_commands(h.reg->captured_gcodes) ==
              std::vector<std::string>{"SET_MAP LANE=lane3 MAP=T1"});
    }

    SECTION("a stale seed the user never picked sends nothing") {
        // The card still holds a seed from before the printer's map moved:
        // T1 on lane2. It was never the user's choice, so it follows the
        // printer, which routes T1 to lane4.
        helix::ToolMapping stale;
        stale.tool_index = 1;
        stale.mapped_slot = 1;
        stale.mapped_backend = 0;
        stale.reason = helix::ToolMapping::MatchReason::FIRMWARE_MAPPING;
        h.view.set_filament_mappings({stale});

        CHECK_FALSE(PrintStartControllerTestAccess::apply_remaps(h.controller));
        CHECK(set_map_commands(h.reg->captured_gcodes) == std::vector<std::string>{});
    }

    SECTION("beside a pick, a stale un-picked row sends nothing") {
        // The early-out sees a pick and goes on to the send loop, so the loop
        // itself has to skip the stale row: T0 seeded onto lane2, which the
        // printer does not route it to.
        helix::ToolMapping stale;
        stale.tool_index = 0;
        stale.mapped_slot = 1;
        stale.mapped_backend = 0;
        stale.reason = helix::ToolMapping::MatchReason::FIRMWARE_MAPPING;
        helix::ToolMapping pick;
        pick.tool_index = 1;
        pick.mapped_slot = 2;
        pick.mapped_backend = 0;
        pick.user_picked = true;
        h.view.set_filament_mappings({stale, pick});

        CHECK(PrintStartControllerTestAccess::apply_remaps(h.controller));
        CHECK(set_map_commands(h.reg->captured_gcodes) ==
              std::vector<std::string>{"SET_MAP LANE=lane3 MAP=T1"});
    }

    SECTION("an auto color match still sends its SET_MAP") {
        helix::ToolMapping matched;
        matched.tool_index = 1;
        matched.mapped_slot = 2;
        matched.mapped_backend = 0;
        matched.reason = helix::ToolMapping::MatchReason::COLOR_MATCH;
        h.view.set_filament_mappings({matched});

        CHECK(PrintStartControllerTestAccess::apply_remaps(h.controller));
        CHECK(set_map_commands(h.reg->captured_gcodes) ==
              std::vector<std::string>{"SET_MAP LANE=lane3 MAP=T1"});
    }
}

TEST_CASE("AFC print start sends no SET_MAP for a free-lane guess",
          "[ams][afc][tool_mapping][routing][print_start]") {
    // T3 is routed to lane2, which is empty, and its file states no colour, so
    // auto color mapping has nothing to match it on (prestonbrown/helixscreen#1862).
    AfcPrintStartHarness h{{tool_of_unknown_color(3)}};
    auto& ams = AmsState::instance();

    SECTION("the seed keeps T3 on its routed lane and preflight sees it empty") {
        h.view.set_filament_mappings(
            ams.seed_tool_mappings(h.tools, ams.collect_available_slots(), true));
        const auto seeded = h.view.get_filament_mappings();
        REQUIRE(seeded.size() == 1);
        CHECK(seeded[0].mapped_slot == 1);
        CHECK(seeded[0].reason == helix::ToolMapping::MatchReason::FIRMWARE_MAPPING);
        CHECK(h.preflight().has_block());

        CHECK_FALSE(PrintStartControllerTestAccess::apply_remaps(h.controller));
        CHECK(set_map_commands(h.reg->captured_gcodes) == std::vector<std::string>{});
    }

    SECTION("a fallback row that reaches print start is not sent") {
        helix::ToolMapping guess;
        guess.tool_index = 3;
        guess.mapped_slot = 0;
        guess.mapped_backend = 0;
        guess.reason = helix::ToolMapping::MatchReason::FREE_LANE_FALLBACK;
        h.view.set_filament_mappings({guess});

        CHECK_FALSE(PrintStartControllerTestAccess::apply_remaps(h.controller));
        CHECK(set_map_commands(h.reg->captured_gcodes) == std::vector<std::string>{});
    }
}

TEST_CASE_METHOD(AfcOpenDetailFixture,
                 "An open print detail follows a printer-side map change it did not pick",
                 "[ams][afc][tool_mapping][routing][print_select]") {
    // The map moving is not a slot change: no colour or presence differs, so
    // only the tool-map version announces it. An open detail view must re-seed
    // the card's un-picked entries on it.
    feed_lanes();
    open({"#FF0000", "#00FF00"});

    auto before = view.get_filament_mappings();
    REQUIRE(before.size() == 2);
    REQUIRE(before[1].mapped_slot == 1);

    // On the printer, lane4 takes over T1 and lane2 takes T3.
    afc().feed_afc_stepper("lane2", {{"map", "T3"}});
    afc().feed_afc_stepper("lane4", {{"map", "T1"}});
    drain();

    const auto after = view.get_filament_mappings();
    REQUIRE(after.size() == 2);
    CHECK(after[0].mapped_slot == 0);
    CHECK(after[1].mapped_slot == 3);
}

TEST_CASE_METHOD(AfcOpenDetailFixture,
                 "A print detail coming back re-seeds for a map that moved while it was away",
                 "[ams][afc][tool_mapping][routing][print_select]") {
    // While another view holds the screen the detail view ignores AMS changes,
    // so activation is the only thing that catches it up.
    feed_lanes();
    open({"#FF0000", "#00FF00"});
    REQUIRE(view.get_filament_mappings()[1].mapped_slot == 1);

    view.on_deactivate(DeactivateReason::NavigateAway);
    afc().feed_afc_stepper("lane2", {{"map", "T3"}});
    afc().feed_afc_stepper("lane4", {{"map", "T1"}});
    drain();
    REQUIRE(view.get_filament_mappings()[1].mapped_slot == 1);

    view.on_activate();
    drain();
    CHECK(view.get_filament_mappings()[1].mapped_slot == 3);
}

TEST_CASE_METHOD(AfcOpenDetailFixture, "A palette re-read for the open file keeps the user's pick",
                 "[ams][afc][tool_mapping][routing][print_select]") {
    feed_lanes();

    SECTION("the headless scan's chip render") {
        open({"#FF0000", "#00FF00"});
        pick_t1_on_lane4();
        PrintSelectDetailViewTestAccess::render_authoritative_chips(view, {0, 1}, true);
        check_t1_pick_kept();
    }

    SECTION("a footer backfill of a palette the metadata lacked") {
        // Materials alone are enough to show the card (OrcaSlicer on a K2 Plus
        // states no colours), so a pick can exist before the colours arrive.
        open({});
        pick_t1_on_lane4();
        helix::gcode::GcodeFooterSummary summary;
        summary.colours = {"#FF0000", "#00FF00"};
        PrintSelectDetailViewTestAccess::apply_footer_summary(view, summary, false);
        check_t1_pick_kept();
    }
}

TEST_CASE_METHOD(AfcOpenDetailFixture,
                 "Flipping auto color mapping re-seeds an open print detail's un-picked rows",
                 "[ams][afc][tool_mapping][routing][print_select][print_start]") {
    // The file is red T0, green T1; lane2 holds red and lane1 green, so auto
    // color mapping crosses the two tools over and the printer's own routing
    // does not. The rows follow whichever the setting says, the moment it flips.
    feed_lanes({"#00FF00", "#FF0000", "#0000FF", "#FFFFFF"});

    SECTION("on to off: the color matches are dropped and nothing is sent") {
        set_auto_color(true);
        open({"#FF0000", "#00FF00"});
        auto matched = view.get_filament_mappings();
        REQUIRE(matched.size() == 2);
        REQUIRE(matched[0].mapped_slot == 1);
        REQUIRE(matched[0].reason == helix::ToolMapping::MatchReason::COLOR_MATCH);

        set_auto_color(false);
        afc().captured_gcodes.clear();
        CHECK_FALSE(PrintStartControllerTestAccess::apply_remaps(*controller));
        CHECK(set_map_commands(afc().captured_gcodes) == std::vector<std::string>{});
    }

    SECTION("off to on: the color matches are sent") {
        set_auto_color(false);
        open({"#FF0000", "#00FF00"});
        REQUIRE(view.get_filament_mappings().size() == 2);
        REQUIRE(view.get_filament_mappings()[0].mapped_slot == 0);

        set_auto_color(true);
        afc().captured_gcodes.clear();
        CHECK(PrintStartControllerTestAccess::apply_remaps(*controller));
        CHECK(set_map_commands(afc().captured_gcodes) ==
              std::vector<std::string>{"SET_MAP LANE=lane2 MAP=T0", "SET_MAP LANE=lane1 MAP=T1"});
    }
}

TEST_CASE("AFC print using a tool no lane is mapped to is blocked, not rerouted",
          "[ams][afc][tool_mapping][routing][print_start]") {
    // lane4 answers to T5, so no lane owns T1. AFC registers no T1 command, so
    // the print cannot run T1 anywhere: the seed leaves it unresolved, preflight
    // blocks on it, and print start does not invent a lane for it.
    AfcPrintStartHarness h{{tool_of_unknown_color(1)}, "T5"};
    REQUIRE(h.reg->get_tool_mapping().size() > 1);
    REQUIRE(h.reg->get_tool_mapping()[1] == -1);
    CHECK(h.reg->firmware_default_routing().head(1) == -1);

    const auto& seed = h.view.get_filament_mappings();
    REQUIRE(seed.size() == 1);
    CHECK(seed[0].is_auto);
    CHECK(seed[0].mapped_slot < 0);
    CHECK(helix::FilamentMapper::find_unresolved_tools(seed) == std::vector<int>{1});

    const auto result = h.preflight();
    REQUIRE(result.checks.size() == 1);
    CHECK(result.checks[0].severity == helix::ToolCheck::Severity::EmptySlot);

    CHECK_FALSE(PrintStartControllerTestAccess::apply_remaps(h.controller));
    CHECK(set_map_commands(h.reg->captured_gcodes) == std::vector<std::string>{});
}

TEST_CASE("AFC tool mapping swap updates correctly", "[ams][afc][tool_mapping][phase1]") {
    // When lanes swap tools (e.g., T0 moves from lane1 to lane3), the mapping
    // should update accordingly
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Initial mapping: T0→lane1, T1→lane2, T2→lane3, T3→lane4
    helper.feed_afc_stepper("lane1", {{"map", "T0"}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"}});
    helper.feed_afc_stepper("lane3", {{"map", "T2"}});
    helper.feed_afc_stepper("lane4", {{"map", "T3"}});

    // Now swap: lane1 gets T2, lane3 gets T0
    helper.feed_afc_stepper("lane1", {{"map", "T2"}});
    helper.feed_afc_stepper("lane3", {{"map", "T0"}});

    // After swap, mapping should reflect new tool assignments
    auto mapping = helper.get_tool_mapping();
    REQUIRE(mapping.size() == 4);
    REQUIRE(mapping[0] == 2); // T0 → lane3 (slot 2)
    REQUIRE(mapping[1] == 1); // T1 → lane2 (slot 1)
    REQUIRE(mapping[2] == 0); // T2 → lane1 (slot 0)
    REQUIRE(mapping[3] == 3); // T3 → lane4 (slot 3)

    // Slot mapped_tool should also be updated
    REQUIRE(helper.get_slot_mapped_tool(0) == 2); // lane1 now maps to T2
    REQUIRE(helper.get_slot_mapped_tool(2) == 0); // lane3 now maps to T0
}

// How a lane's `map` / `current_map` deltas resolve to the one tool SlotRegistry
// holds per lane. Moonraker sends deltas, so an absent key is "unchanged"; a
// PRESENT null, empty list, empty string or unparseable value is authoritative
// and unmaps. AFC virtual tools (#605) send `map` as a list unconditionally, so
// ["T0"] must behave exactly like "T0". current_map names the tool a multi-tool
// lane is actually on and moves on its own while map stays put; it only selects
// among map's tools, survives a map-only delta while still listed, and an empty
// one is no news (single-tool lanes may send it null). Without a usable
// current_map the lowest tool is an arbitrary stable pick: AFC's order is not
// sorted, so the lists are fed unsorted. Junk list entries are skipped, and
// "T14,T13" must not parse as 14. Every tool in map still routes to the lane.
TEST_CASE("AFC lane map and current_map resolve to one tool per lane", "[ams][afc][tool_mapping]") {
    using nlohmann::json;
    const json multi = {{"map", json::array({"T11", "T10"})}, {"current_map", "T11"}};
    struct Row {
        const char* name;
        std::vector<json> frames;
        int tool;
        std::vector<int> routed;   // tools the forward map sends to this lane
        std::vector<int> unrouted; // tools it must not
    };
    const Row rows[] = {
        {"a string maps", {{{"map", "T0"}}, {{"map", "T3"}}}, 3, {3}, {0}},
        {"a present null unmaps",
         {{{"map", "T2"}, {"prep", true}}, {{"map", nullptr}, {"prep", true}}},
         -1,
         {},
         {2}},
        {"a delta without map keeps it",
         {{{"map", "T2"}, {"prep", true}}, {{"weight", 931.7}}},
         2,
         {2},
         {}},
        {"a one-tool list maps like a string", {{{"map", json::array({"T3"})}}}, 3, {3}, {}},
        {"a multi-tool list takes the lowest",
         {{{"map", json::array({"T14", "T13", "T1"})}}},
         1,
         {1},
         {}},
        {"dropping the virtual tools keeps the tool",
         {{{"map", json::array({"T14", "T13", "T1"})}}, {{"map", json::array({"T1"})}}},
         1,
         {1},
         {}},
        {"current_map picks the active tool", {multi}, 11, {11, 10}, {}},
        {"current_map alone retargets", {multi, {{"current_map", "T10"}}}, 10, {10, 11}, {}},
        {"current_map survives a map-only delta",
         {multi, {{"map", json::array({"T11", "T10", "T5"})}}},
         11,
         {11},
         {}},
        {"current_map dropped from map falls back",
         {multi, {{"map", json::array({"T10"})}}},
         10,
         {10},
         {11}},
        {"current_map outside map is ignored",
         {{{"map", json::array({"T11", "T10"})}, {"current_map", "T7"}}},
         10,
         {10},
         {7}},
        {"a null current_map",
         {{{"map", json::array({"T2"})}, {"current_map", nullptr}}},
         2,
         {2},
         {}},
        {"an empty current_map", {{{"map", json::array({"T3"})}, {"current_map", ""}}}, 3, {3}, {}},
        {"a null current_map alone",
         {{{"map", json::array({"T2"})}}, {{"current_map", nullptr}}},
         2,
         {2},
         {}},
        {"unmapping forgets current_map",
         {multi, {{"map", json::array()}}, {{"map", json::array({"T11", "T4"})}}},
         4,
         {4},
         {}},
        {"an empty list unmaps",
         {{{"map", json::array({"T2"})}}, {{"map", json::array()}}},
         -1,
         {},
         {2}},
        {"junk entries are skipped",
         {{{"map", json::array({"T14,T13", "garbage", "T7", 5, nullptr})}}},
         7,
         {7},
         {14}},
        {"a list with nothing usable unmaps",
         {{{"map", json::array({"T1"})}}, {{"map", json::array({"T14,T13", "nope"})}}},
         -1,
         {},
         {1}},
        {"an object-shaped map unmaps",
         {{{"map", "T2"}}, {{"map", json::object({{"T0", "lane1"}})}}},
         -1,
         {},
         {2}},
        {"an empty string unmaps", {{{"map", "T2"}}, {{"map", ""}}}, -1, {}, {2}},
        {"an unparseable string unmaps", {{{"map", "T2"}}, {{"map", "garbage"}}}, -1, {}, {2}},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            AmsBackendAfcTestHelper helper;
            helper.initialize_test_lanes_with_slots(4);
            for (const json& frame : r.frames) {
                helper.feed_afc_stepper("lane1", frame);
            }

            CHECK(helper.get_slot_mapped_tool(0) == r.tool);
            const auto forward = helper.get_tool_mapping();
            const auto lane_of = [&](int t) {
                return t < static_cast<int>(forward.size()) ? forward[t] : -1;
            };
            for (int t : r.routed) {
                INFO("T" << t);
                CHECK(lane_of(t) == 0);
            }
            for (int t : r.unrouted) {
                INFO("T" << t);
                CHECK(lane_of(t) == -1);
            }
        }
    }
}

TEST_CASE("AFC endless spool from runout_lane field", "[ams][afc][endless_spool][phase1]") {
    // Real device: AFC_stepper lane1 has "runout_lane": "lane2"
    // meaning if lane1 runs out, switch to lane2.
    // We never parse this field today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // Feed stepper data with runout_lane
    helper.feed_afc_stepper("lane1", {{"runout_lane", "lane2"}});

    // runout_lane should update endless spool backup config
    auto edges = helper.get_endless_spool_edges();
    REQUIRE(edges.size() == 4);
    REQUIRE(edges[0] == 1); // lane1's backup is lane2 (slot 1)
}

TEST_CASE("AFC endless spool null runout_lane clears backup", "[ams][afc][endless_spool][phase1]") {
    // When runout_lane is null, the backup should be cleared (-1)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // First set a backup
    helper.set_endless_spool_config(0, 1); // lane1 backup = lane2

    // Now feed a null runout_lane
    nlohmann::json stepper_data;
    stepper_data["runout_lane"] = nullptr; // JSON null
    helper.feed_afc_stepper("lane1", stepper_data);

    // null runout_lane should clear the backup
    REQUIRE(helper.get_endless_spool_edges()[0] == -1); // Cleared
}
