// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 209 to 254 of test_ams_backend_afc.cpp before it was split, in its order. Mostly
// [status_fields][1149], [dispatch][1183], [extruder_tool_index],
// [status_fields][buffer][multiunit], [toolchanger][empty-carriage], and more

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

TEST_CASE_METHOD(AfcFourLanes, "AFC a zero weight on a lane with no identity reads as unknown",
                 "[ams][afc][weight][1661]") {
    helper.feed_afc_stepper("lane1",
                            {{"material", "PLA"}, {"color", "#FF0000"}, {"weight", 850.0}});
    REQUIRE(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(850.0f));

    SECTION("cleared: no material, bare '#' colour, weight 0") {
        // What clear_values() and Clear Spool leave in AFC. Read as 0 g, the
        // print-start weight gate warns on every print from this lane; unknown
        // (-1) is what it skips.
        helper.feed_afc_stepper("lane1", {{"material", ""}, {"color", "#"}, {"weight", 0.0}});
        CHECK(helper.get_slot_info(0).remaining_weight_g < 0.0f);
    }
    SECTION("a spool metered down to zero keeps its material and stays at zero") {
        helper.feed_afc_stepper("lane1", {{"material", "PLA"}, {"weight", 0.0}});
        CHECK(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(0.0f));
    }
}

TEST_CASE("AFC persist_override records a deliberate pure black, never the no-color sentinel",
          "[ams][afc][override]") {
    const bool black = GENERATE(true, false);
    CAPTURE(black);
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    SlotInfo info;
    info.material = "PLA";
    info.color_rgb = black ? 0x000000 : kAmsDefaultSlotColor;
    helix::test::apply_edit(helper, 0, info);

    const auto& ovr = AfcTestAccess::overrides(helper).at(0);
    CHECK(ovr.color_set == black);
    if (black) {
        CHECK(ovr.color_rgb == 0x000000);
    }
    CHECK(ovr.material == "PLA");
}

TEST_CASE_METHOD(AfcFourLanes, "AFC persist_override wires nozzle/bed temps into the override",
                 "[ams][afc][override]") {
    SlotInfo info;
    info.material = "PETG";
    info.bed_temp = 80;
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    helix::test::apply_edit(helper, 0, info);

    const auto& ovr = AfcTestAccess::overrides(helper).at(0);
    CHECK(ovr.bed_temp == 80);
    CHECK(ovr.nozzle_temp == 240); // midpoint of min/max
}

// Upstream follow-ups: version in status (#807) and lane vendor_name (#808)

TEST_CASE_METHOD(AfcFourZeroBasedLanes,
                 "AFC version is read from the status object when upstream supplies it",
                 "[ams][afc][version]") {
    // AFC is moving the version signal out of the dead afc-install DB namespace
    // and into the status object (AFCProject/AFC-Klipper-Add-On PR #807 adds
    // AFC.version to get_status()). Read it where they are putting it.
    helper.feed_afc_state({{"version", "1.2.1"}});
    CHECK(helper.get_system_info().version == "1.2.1");

    // Absent on every release predating #807 — must not clobber what we already
    // have, or the display flips to empty on the next status tick.
    helper.feed_afc_state({{"current_load", "lane1"}});
    CHECK(helper.get_system_info().version == "1.2.1");

    // Equally, an empty string is not a version.
    helper.feed_afc_state({{"version", ""}});
    CHECK(helper.get_system_info().version == "1.2.1");
}

TEST_CASE("AFC version never gates behaviour", "[ams][afc][version]") {
    // AFC_VERSION is a hand-bumped literal that already drifted from the release
    // tag — it sat at 1.1.37 through the whole v1.2.0 release. Presence of the
    // field is the only trustworthy signal, so a comically old version must not
    // disable anything.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_state({{"version", "0.0.1"}});

    // apply_lane_data only bootstraps an EMPTY slot registry, so these cases seed
    // the slots from discovery first; the payload then names lanes that exist.
    nlohmann::json lanes;
    for (int i = 0; i < 4; ++i) {
        lanes["lane" + std::to_string(i)] = {{"color", "#FF5500"}, {"material", "PLA"}};
    }
    helper.feed_afc_state({{"lanes", lanes}});

    auto slot = helper.get_slot_info(0);
    CHECK(slot.color_rgb == 0xFF5500u);
    CHECK(slot.material == "PLA");
}

TEST_CASE("AFC lane_data's vendor_name is the brand, and an empty one clears it (#808)",
          "[ams][afc][lane_data]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // All four lanes in one payload, matching the slots discovery seeded.
    nlohmann::json lanes;
    lanes["lane0"] = {{"vendor_name", "Polymaker"}};
    lanes["lane1"] = {{"vendor_name", "Polymaker"}};
    lanes["lane2"] = {{"brand", "Prusament"}, {"vendor", "Hatchbox"}}; // not AFC's spellings
    lanes["lane3"] = {{"material", "PLA"}};
    helper.feed_afc_state({{"lanes", lanes}});
    REQUIRE(helper.get_slot_info(0).brand == "Polymaker");
    CHECK(helper.get_slot_info(2).brand.empty());
    CHECK(helper.get_slot_info(3).brand.empty());

    lanes["lane0"] = {{"vendor_name", ""}}; // AFC states no vendor
    lanes["lane1"] = {{"material", "PLA"}}; // a record silent on the vendor
    helper.feed_afc_state({{"lanes", lanes}});
    CHECK(helper.get_slot_info(0).brand.empty());
    CHECK(helper.get_slot_info(1).brand == "Polymaker");
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes,
                 "AFC reads vendor_name from the AFC_stepper status object (#808)",
                 "[ams][afc][vendor]") {
    // Upstream #808 asked for the vendor on BOTH lane_data and get_status(), and
    // jimmyjon711 accepted. The status surface is the one that matters: it is live
    // and present on every AFC version, where lane_data is a DB snapshot that only
    // refreshes when AFC happens to push.
    helper.feed_afc_stepper("lane0", {{"vendor_name", "Polymaker"}});
    CHECK(helper.get_slot_info(0).brand == "Polymaker");

    // Deltas: an update without the key must not clobber what we already have.
    helper.feed_afc_stepper("lane0", {{"material", "PLA"}});
    CHECK(helper.get_slot_info(0).brand == "Polymaker");
}

TEST_CASE_METHOD(AfcFourZeroBasedLanes, "AFC takes no weight from lane_data on any version (#805)",
                 "[ams][afc][lane_data]") {
    // The AFC_stepper subscription is the sole weight authority — live since v1.1.0.
    helper.feed_afc_stepper("lane0", {{"weight", 750.0}});
    REQUIRE(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(750.0f));

    // Now deliver a lane_data payload the way a v1.2.0 box does: `weight` present
    // but STALE, because cmd_SET_WEIGHT updated the lane object without publishing
    // (AFCProject/AFC-Klipper-Add-On#805, fixed only after v1.2.0 in PR #812). The
    // stale and fixed payloads are byte-identical, so no feature detection can tell
    // them apart — the only safe rule is to never source weight here.
    //
    // `remaining_weight` / `total_weight` are included too: no AFC version ever
    // emits either key, so nothing should read them from lane_data.
    nlohmann::json lanes;
    for (int i = 0; i < 4; ++i) {
        lanes["lane" + std::to_string(i)] = {
            {"weight", 12.0}, {"remaining_weight", 34.0}, {"total_weight", 56.0}};
    }
    helper.feed_afc_state({{"lanes", lanes}});

    CHECK(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(750.0f));
    CHECK(helper.get_slot_info(0).total_weight_g == Catch::Approx(-1.0f));

    // A cleared lane_data record (post-#812 writes weight: 0) must not zero a live
    // reading either.
    for (int i = 0; i < 4; ++i)
        lanes["lane" + std::to_string(i)] = {{"weight", 0}, {"spool_id", nullptr}};
    helper.feed_afc_state({{"lanes", lanes}});
    CHECK(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(750.0f));
}

TEST_CASE("AFC clears the operation detail when its message empties", "[ams][afc][recovery]") {
    // operation_detail outranks the action- and print-state-derived strings in
    // AmsState::recompute_action_detail(), so a value left behind here pins the
    // AMS sidebar status label to a stale error for the rest of the session.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.test_parse_afc_state(nlohmann::json{
        {"error_state", true},
        {"message",
         {{"message", "Hub is already clear while trying to reset 'lane1'"}, {"type", "error"}}}});
    REQUIRE(helper.get_system_info().operation_detail ==
            "Hub is already clear while trying to reset 'lane1'");

    // AFC_CLEAR_MESSAGE lands: the message object empties.
    helper.test_parse_afc_state(nlohmann::json{{"message", {{"message", ""}, {"type", ""}}}});
    REQUIRE(helper.get_system_info().operation_detail.empty());
}

TEST_CASE("AFC lane parses the v1.1.0 fields we used to drop", "[ams][afc][status_fields][1149]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", observed_lane1_v110());

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.filament_status == "Ready");
    REQUIRE(sensors.filament_status_led == "#00cc00");
    REQUIRE(sensors.endstops == "load,hub,tool_start,tool_end,buffer_advance,buffer_trailing");

    // A Box Turtle has no selector, so AFC omits the key entirely. That must
    // read as "no selector on this lane", not "selector clear".
    REQUIRE(sensors.has_selector == false);
    REQUIRE(sensors.selector == false);

    REQUIRE(helper.get_lane_remember_spool("lane1") == std::optional<bool>{false});
    REQUIRE(helper.get_lane_remember_spool("lane4") == std::nullopt);
}

TEST_CASE("AFC lane parses the v1.2.0-only spool fields", "[ams][afc][status_fields][1149]") {
    // Source-derived: AFC_lane.get_status() at v1.2.0 adds filament_name,
    // multi_color_hexes, initial_weight and bed_temp to the observed shape.
    // multi_color_hexes is a list of BARE hexes — AFC splits Spoolman's
    // comma-joined string and re-prefixes only the first into `color`.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json lane = observed_lane1_v110();
    lane["spool_id"] = 42;
    lane["filament_name"] = "Galaxy Black";
    lane["initial_weight"] = 1000.0;
    lane["bed_temp"] = 60.0;
    lane["multi_color_hexes"] = nlohmann::json::array({"D4AF37", "C0C0C0", "B87333"});
    lane["selector"] = true;

    helper.feed_afc_stepper("lane1", lane);

    auto info = helper.get_system_info();
    const auto& slot = info.units[0].slots[0];
    REQUIRE(slot.spool_name == "Galaxy Black");
    REQUIRE(slot.bed_temp == 60);
    REQUIRE(slot.multi_color_hexes == "#D4AF37,#C0C0C0,#B87333");
    REQUIRE(slot.is_multi_color());
    REQUIRE(slot.total_weight_g == Catch::Approx(1000.0f));

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.has_selector == true);
    REQUIRE(sensors.selector == true);
}

TEST_CASE("AFC initial_weight is ignored on a lane with no Spoolman link",
          "[ams][afc][status_fields][1149]") {
    // espooler_values.full_weight is a CONFIGURED unit constant (typically
    // 1000 g) that every lane reports, empty ones included; Spoolman only
    // overwrites it when a spool is linked. Adopting it ungated would render an
    // ejected lane as "0 / 1000 g" instead of unknown.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json lane = observed_lane1_v110(); // spool_id: null
    lane["initial_weight"] = 1000.0;
    lane["weight"] = 0.0;
    helper.feed_afc_stepper("lane1", lane);

    auto info = helper.get_system_info();
    REQUIRE(info.units[0].slots[0].total_weight_g == Catch::Approx(-1.0f));
    REQUIRE(info.units[0].slots[0].get_remaining_percent() == Catch::Approx(-1.0f));
}

TEST_CASE("AFC lane delta omitting the new fields leaves them intact",
          "[ams][afc][status_fields][1149]") {
    // Moonraker forwards only CHANGED keys, so a frame carrying nothing but a
    // sensor flip must not zero everything parsed from the previous frame.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json lane = observed_lane1_v110();
    lane["spool_id"] = 42;
    lane["filament_name"] = "Galaxy Black";
    lane["initial_weight"] = 750.0;
    lane["bed_temp"] = 60.0;
    lane["multi_color_hexes"] = nlohmann::json::array({"D4AF37", "C0C0C0"});
    lane["selector"] = true;
    helper.feed_afc_stepper("lane1", lane);

    // The delta a real buffer switch produces: one key.
    helper.feed_afc_stepper("lane1", nlohmann::json{{"buffer_status", "Trailing"}});

    auto info = helper.get_system_info();
    const auto& slot = info.units[0].slots[0];
    REQUIRE(slot.spool_name == "Galaxy Black");
    REQUIRE(slot.bed_temp == 60);
    REQUIRE(slot.multi_color_hexes == "#D4AF37,#C0C0C0");
    REQUIRE(slot.total_weight_g == Catch::Approx(750.0f));

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.buffer_status == "Trailing");
    REQUIRE(sensors.filament_status_led == "#00cc00");
    REQUIRE(sensors.endstops == "load,hub,tool_start,tool_end,buffer_advance,buffer_trailing");
    REQUIRE(sensors.has_selector == true);
    REQUIRE(sensors.selector == true);
    REQUIRE(helper.get_lane_remember_spool("lane1") == std::optional<bool>{false});
}

TEST_CASE("AFC lane clears filament identity when firmware clears it",
          "[ams][afc][status_fields][1149]") {
    // clear_values() on eject sets filament_name="", multi_color=[] and
    // bed_temp=None. Those are deliberate clears, not missing fields.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json lane = observed_lane1_v110();
    lane["spool_id"] = 42;
    lane["filament_name"] = "Galaxy Black";
    lane["bed_temp"] = 60.0;
    lane["multi_color_hexes"] = nlohmann::json::array({"D4AF37"});
    helper.feed_afc_stepper("lane1", lane);

    helper.feed_afc_stepper("lane1", nlohmann::json{{"filament_name", ""},
                                                    {"bed_temp", nullptr},
                                                    {"multi_color_hexes", nlohmann::json::array()},
                                                    {"spool_id", nullptr}});

    auto info = helper.get_system_info();
    const auto& slot = info.units[0].slots[0];
    REQUIRE(slot.spool_name.empty());
    REQUIRE(slot.bed_temp == 0);
    REQUIRE(slot.multi_color_hexes.empty());
    REQUIRE_FALSE(slot.is_multi_color());
}

TEST_CASE("AFC top-level next_lane resolves, clears and survives deltas",
          "[ams][afc][status_fields][1149]") {
    // Observed on the live box: "next_lane": "lane4" with no toolchange running.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"next_lane", "lane4"}});
    REQUIRE(helper.get_system_info().next_slot == 3);

    // An unrelated delta must not drop the staging.
    helper.feed_afc_state({{"current_toolchange", 12}});
    REQUIRE(helper.get_system_info().next_slot == 3);

    // AFC naming no lane is a real transition to "none".
    helper.feed_afc_state({{"next_lane", nullptr}});
    REQUIRE(helper.get_system_info().next_slot == -1);
}

TEST_CASE("AFC top-level position_saved, spoolman and maps are read",
          "[ams][afc][status_fields][1149]") {
    // Observed shape (spoolman URL redacted); `maps` is source-derived — it is
    // the one top-level key v1.1.0 does not emit.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"position_saved", true},
                           {"spoolman", "http://spoolman.invalid:7912"},
                           {"maps", nlohmann::json::array({"T0", "T1", "T2", "T3"})}});

    auto info = helper.get_system_info();
    REQUIRE(info.position_saved == true);
    REQUIRE(info.spoolman_url == "http://spoolman.invalid:7912");
    REQUIRE(helper.get_afc_tool_cmds() == std::vector<std::string>{"T0", "T1", "T2", "T3"});

    // A later delta that mentions none of them leaves all three alone.
    helper.feed_afc_state({{"led_state", true}});
    info = helper.get_system_info();
    REQUIRE(info.position_saved == true);
    REQUIRE(info.spoolman_url == "http://spoolman.invalid:7912");
    REQUIRE(helper.get_afc_tool_cmds().size() == 4);

    // AFC publishes `false`, not null, when Spoolman is unconfigured — a
    // non-string must not be coerced into the URL.
    helper.feed_afc_state({{"spoolman", false}, {"position_saved", false}});
    info = helper.get_system_info();
    REQUIRE(info.spoolman_url == "http://spoolman.invalid:7912");
    REQUIRE(info.position_saved == false);
}

TEST_CASE("AFC maps cross-check does not override the per-lane tool mapping",
          "[ams][afc][status_fields][1149]") {
    // maps is diagnostic: a lane claiming a T-command AFC never registered still
    // maps, because maps is absent entirely before v1.2.0 and cannot be trusted
    // as an authority.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"maps", nlohmann::json::array({"T0", "T1"})}});
    helper.feed_afc_stepper("lane3", nlohmann::json{{"map", "T7"}});

    REQUIRE(helper.get_slot_mapped_tool(2) == 7);
}

TEST_CASE("AFC buffer parses the observed v1.1.0 frame", "[ams][afc][status_fields][1149]") {
    // Observed: AFC_buffer Turtle_1 on the live box, buffer disabled.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    // AFC_buffer frames are only dispatched for buffers AFC has named.
    helper.feed_afc_state({{"buffers", {"Turtle_1"}}});

    helper.feed_afc_buffer("Turtle_1",
                           nlohmann::json{{"state", "Advancing"},
                                          {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                          {"enabled", false},
                                          {"rotation_distance", nullptr},
                                          {"fault_detection_enabled", false},
                                          {"error_sensitivity", 0},
                                          {"fault_timer", nullptr},
                                          {"distance_to_fault", nullptr}});

    auto info = helper.get_system_info();
    REQUIRE(info.units[0].buffer_health.has_value());
    const auto& bh = info.units[0].buffer_health.value();
    REQUIRE(bh.state == "Advancing");
    REQUIRE(bh.fault_detection_enabled == false);
    // AFC nulls both while the buffer is idle — that maps to the not-reported
    // sentinel, not to zero.
    REQUIRE(bh.rotation_distance == Catch::Approx(-1.0f));
    REQUIRE(bh.fault_timer == Catch::Approx(-1.0f));
    // v1.1.0 emits no multipliers at all.
    REQUIRE(bh.multiplier == Catch::Approx(-1.0f));
    REQUIRE(bh.active_lane.empty());
}

TEST_CASE("AFC buffer delta omitting lanes still reaches its unit",
          "[ams][afc][status_fields][1149]") {
    // A state-only delta carries no `lanes`, so the routing has to come from
    // what an earlier frame said.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    // AFC_buffer frames are only dispatched for buffers AFC has named.
    helper.feed_afc_state({{"buffers", {"Turtle_1"}}});

    helper.feed_afc_buffer("Turtle_1",
                           nlohmann::json{{"state", "Trailing"},
                                          {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                          {"multiplier", 1.1}});
    helper.feed_afc_buffer("Turtle_1", nlohmann::json{{"state", "Advancing"}});

    auto info = helper.get_system_info();
    REQUIRE(info.units[0].buffer_health.has_value());
    const auto& bh = info.units[0].buffer_health.value();
    REQUIRE(bh.state == "Advancing");
    REQUIRE(bh.multiplier == Catch::Approx(1.1f));
}

TEST_CASE("AFC buffers in the layout-building frame land on their own units",
          "[ams][afc][status_fields][buffer][multiunit]") {
    // Defect (a). The connect frame carries the AFC_buffer objects and the
    // AFC_BoxTurtle objects together. Buffers must be resolved against the units
    // that frame builds, not against the synthetic pre-reorganize unit 0.
    AmsBackendAfcTestHelper helper;
    discover_two_units(helper);

    nlohmann::json params = two_unit_objects();
    const nlohmann::json buffers = two_buffer_objects();
    for (auto it = buffers.begin(); it != buffers.end(); ++it) {
        params[it.key()] = it.value();
    }
    helper.feed_status_update(params);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].buffer_health.has_value());
    REQUIRE(info.units[1].buffer_health.has_value());

    CHECK(info.units[0].buffer_health->state == "Advancing");
    CHECK(info.units[0].buffer_health->error_sensitivity == Catch::Approx(3.0f));
    CHECK(info.units[0].buffer_health->distance_to_fault == Catch::Approx(11.0f));

    CHECK(info.units[1].buffer_health->state == "Trailing");
    CHECK(info.units[1].buffer_health->error_sensitivity == Catch::Approx(7.0f));
    CHECK(info.units[1].buffer_health->distance_to_fault == Catch::Approx(22.0f));
}

TEST_CASE("AFC buffer health survives a later frame that rebuilds the units",
          "[ams][afc][status_fields][buffer][multiunit]") {
    // Defect (b), isolated: the units already exist when the buffers arrive, so
    // the attribution is correct no matter what order the dispatcher uses. What
    // this asserts is that a subsequent unit-object frame — which re-runs
    // reorganize_slots() and default-constructs every AmsUnit — does not blank it.
    AmsBackendAfcTestHelper helper;
    discover_two_units(helper);

    helper.feed_status_update(two_unit_objects());
    helper.feed_status_update(two_buffer_objects());

    auto before = helper.get_system_info();
    REQUIRE(before.units.size() == 2);
    REQUIRE(before.units[0].buffer_health.has_value());
    REQUIRE(before.units[1].buffer_health.has_value());

    // AFC re-sends the unit objects and nothing else. Moonraker forwards only
    // changed keys, so the buffer parser will not run again for as long as the
    // buffers hold still.
    helper.feed_status_update(two_unit_objects());

    auto after = helper.get_system_info();
    REQUIRE(after.units.size() == 2);
    REQUIRE(after.units[0].buffer_health.has_value());
    REQUIRE(after.units[1].buffer_health.has_value());
    CHECK(after.units[0].buffer_health->state == "Advancing");
    CHECK(after.units[0].buffer_health->error_sensitivity == Catch::Approx(3.0f));
    CHECK(after.units[1].buffer_health->state == "Trailing");
    CHECK(after.units[1].buffer_health->error_sensitivity == Catch::Approx(7.0f));
}

TEST_CASE("AFC one buffer's fields do not bleed into another buffer's health",
          "[ams][afc][status_fields][buffer][multiunit]") {
    // The old read-modify-write seeded each buffer's update from the OWNING UNIT's
    // current health, which is only the same thing as "this buffer's last reading"
    // when one buffer owns the unit. In the connect frame, before the layout
    // exists, both buffers resolve to the same unit — so TN2 inherited TN1's value
    // for every field TN2's own frame omitted.
    AmsBackendAfcTestHelper helper;
    discover_two_units(helper);

    nlohmann::json params = two_unit_objects();
    params["AFC_buffer TN1"] = nlohmann::json{{"state", "Advancing"},
                                              {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                              {"error_sensitivity", 3},
                                              {"distance_to_fault", 11.0}};
    // TN2 reports no distance_to_fault at all — fault detection is off on this one.
    params["AFC_buffer TN2"] = nlohmann::json{{"state", "Trailing"},
                                              {"lanes", {"lane5", "lane6", "lane7", "lane8"}},
                                              {"error_sensitivity", 7}};
    helper.feed_status_update(params);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[1].buffer_health.has_value());
    CHECK(info.units[1].buffer_health->state == "Trailing");
    CHECK(info.units[1].buffer_health->error_sensitivity == Catch::Approx(7.0f));
    // The "not reported" sentinel, NOT TN1's 11mm.
    CHECK(info.units[1].buffer_health->distance_to_fault == Catch::Approx(-1.0f));

    REQUIRE(info.units[0].buffer_health.has_value());
    CHECK(info.units[0].buffer_health->error_sensitivity == Catch::Approx(3.0f));
    CHECK(info.units[0].buffer_health->distance_to_fault == Catch::Approx(11.0f));
}

TEST_CASE("AFC full multi-lane v1.2.0 frame lands on every slot",
          "[ams][afc][status_fields][1149]") {
    // Observed four-lane BoxTurtle frame with the v1.2.0-only keys added from
    // AFC_lane.get_status() at tag v1.2.0. Colours, weights, maps, dist_hub and
    // the endstop list are the live values.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"current_lane", nullptr},
                           {"next_lane", "lane4"},
                           {"current_state", "Idle"},
                           {"current_toolchange", 12},
                           {"number_of_toolchanges", 12},
                           {"spoolman", "http://spoolman.invalid:7912"},
                           {"position_saved", false},
                           {"error_state", false},
                           {"bypass_state", false},
                           {"quiet_mode", false},
                           {"maps", nlohmann::json::array({"T0", "T1", "T2", "T3"})},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                           {"extruders", {"extruder"}},
                           {"hubs", {"Turtle_1"}},
                           {"buffers", {"Turtle_1"}},
                           {"led_state", true}});

    struct LaneFixture {
        const char* name;
        const char* map;
        const char* color;
        double weight;
        double dist_hub;
        int spool_id;
        const char* filament_name;
        int bed_temp;
    };
    const LaneFixture lanes[] = {
        {"lane1", "T0", "#E53935", 505.8077510382372, 194.57, 42, "Galaxy Black", 60},
        {"lane2", "T1", "#536DFE", 497.0348845693303, 122.73, 43, "Sky Blue", 62},
        {"lane3", "T2", "#43A047", 812.5, 150.0, 44, "Leaf Green", 60},
        {"lane4", "T3", "#FDD835", 233.25, 175.5, 45, "Sunflower", 65},
    };

    for (const auto& lf : lanes) {
        nlohmann::json lane = observed_lane1_v110();
        lane["name"] = lf.name;
        lane["map"] = lf.map;
        lane["color"] = lf.color;
        lane["weight"] = lf.weight;
        lane["dist_hub"] = lf.dist_hub;
        lane["spool_id"] = lf.spool_id;
        lane["filament_name"] = lf.filament_name;
        lane["bed_temp"] = lf.bed_temp;
        lane["initial_weight"] = 1000.0;
        lane["remember_spool"] = true;
        // AFCLaneState.LOADED — "loaded to hub". There is no "Ready" in the
        // lane `status` vocabulary (AFC_lane.py:65-76); that word belongs to the
        // separate filament_status field.
        lane["status"] = "Loaded";
        helper.feed_afc_stepper(lf.name, lane);
    }

    helper.feed_afc_buffer("Turtle_1",
                           nlohmann::json{{"state", "Advancing"},
                                          {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                          {"enabled", true},
                                          {"rotation_distance", 22.678},
                                          {"active_lane", nullptr},
                                          {"multiplier_high", 1.1},
                                          {"multiplier_low", 0.9},
                                          {"multiplier", 1.0},
                                          {"fault_detection_enabled", true},
                                          {"error_sensitivity", 7},
                                          {"fault_timer", 1.5},
                                          {"distance_to_fault", nullptr}});

    auto info = helper.get_system_info();
    REQUIRE(info.next_slot == 3);
    REQUIRE(info.spoolman_url == "http://spoolman.invalid:7912");
    REQUIRE(info.position_saved == false);
    REQUIRE(helper.get_afc_tool_cmds().size() == 4);

    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slots.size() == 4);
    for (int i = 0; i < 4; ++i) {
        const auto& slot = info.units[0].slots[i];
        INFO("slot " << i);
        REQUIRE(slot.spool_name == lanes[i].filament_name);
        REQUIRE(slot.bed_temp == lanes[i].bed_temp);
        REQUIRE(slot.spoolman_id == lanes[i].spool_id);
        REQUIRE(slot.total_weight_g == Catch::Approx(1000.0f));
        REQUIRE(slot.remaining_weight_g == Catch::Approx(lanes[i].weight));
        REQUIRE(slot.mapped_tool == i);
        REQUIRE(helper.get_lane_remember_spool(lanes[i].name) == std::optional<bool>{true});
        REQUIRE(helper.get_lane_sensors(i).endstops ==
                "load,hub,tool_start,tool_end,buffer_advance,buffer_trailing");
        REQUIRE(helper.get_lane_sensors(i).filament_status_led == "#00cc00");
    }

    REQUIRE(info.units[0].buffer_health.has_value());
    const auto& bh = info.units[0].buffer_health.value();
    REQUIRE(bh.active_lane.empty());
    REQUIRE(bh.multiplier == Catch::Approx(1.0f));
    REQUIRE(bh.fault_timer == Catch::Approx(1.5f));
    REQUIRE(bh.distance_to_fault == Catch::Approx(-1.0f));
}

TEST_CASE("AFC no-op load resolves on the macro ack", "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(2).success());
    REQUIRE(h.sent().size() == 1);
    REQUIRE(h.sent()[0] == "CHANGE_TOOL LANE=lane3");

    // The optimistic set is the transition that makes an operation completable.
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC says "lane3 already loaded", never enters a toolchange and reports
    // nothing at all. The macro acks in 4ms.
    h.ack(0);

    REQUIRE(h.action() == AmsAction::IDLE);
    // Asserting the end value alone would pass vacuously — IDLE is where
    // this started. The busy leg is the load-bearing half.
    REQUIRE(trace_contains(h.trace(), AmsAction::LOADING));
    REQUIRE(h.trace().back() == AmsAction::IDLE);
}

TEST_CASE("AFC names the lane a load is headed for until the load ends",
          "[ams][afc][dispatch][spool_spin]") {
    AfcDispatchAckHelper h;
    REQUIRE(h.load_filament(3).success());
    CHECK(h.get_system_info().pending_target_slot == 3);
    // AFC takes the load over, then the macro acks while it is still loading: the incoming
    // lane stays named, so its spool keeps turning for the whole load.
    h.feed_state("Loading");
    h.ack(0);
    CHECK(h.get_system_info().pending_target_slot == 3);
    h.feed_state("Idle");
    CHECK(h.get_system_info().pending_target_slot == -1);
}

TEST_CASE("AFC no-op load names no lane once the macro ack ends it", "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;
    REQUIRE(h.load_filament(3).success());
    REQUIRE(h.get_system_info().pending_target_slot == 3);

    // AFC never reports a state for this load: the gcode ack is its whole end.
    h.ack(0);

    REQUIRE(h.action() == AmsAction::IDLE);
    CHECK(h.get_system_info().pending_target_slot == -1);
    CHECK(h.get_system_info().path_active_slot() == -1);
}

TEST_CASE("AFC lane change shows the incoming lane through the load half",
          "[ams][afc][dispatch][spool_spin][toolchange]") {
    AfcDispatchAckHelper h;
    h.feed_afc({{"current_state", "Idle"}, {"current_lane", "lane1"}});
    REQUIRE(h.get_system_info().current_slot == 0);

    // CHANGE_TOOL unloads lane1, reports Idle between the halves, then loads lane3.
    auto change_to_lane3 = [&h]() {
        h.feed_afc({{"current_state", "Unloading"}, {"next_lane", "lane3"}});
        h.feed_afc({{"current_state", "Idle"}, {"current_lane", nullptr}});
        CHECK(h.get_system_info().action == AmsAction::UNLOADING);
        CHECK(h.get_system_info().pending_target_slot == 2);
        h.feed_afc({{"current_state", "Loading"}});
        CHECK(h.get_system_info().path_active_slot() == 2);
        h.feed_afc({{"current_state", "Idle"}, {"current_lane", "lane3"}, {"next_lane", nullptr}});
        CHECK(h.get_system_info().pending_target_slot == -1);
        CHECK(h.get_system_info().path_active_slot() == 2);
    };

    SECTION("a load the user started") {
        REQUIRE(h.load_filament(2).success());
        change_to_lane3();
    }
    SECTION("a tool change from the print's gcode") {
        change_to_lane3();
    }
}

TEST_CASE("AFC load that never starts names no incoming lane", "[ams][afc][dispatch]") {
    AfcDispatchAckHelper h;

    SECTION("its homing move fails") {
        h.homed = false;
        h.fail_g28 = true;
        REQUIRE(h.load_filament(2).success());
        REQUIRE(h.sent().size() == 1);
        CHECK(h.action() == AmsAction::IDLE);
    }
    SECTION("Klipper restarts under it") {
        REQUIRE(h.load_filament(2).success());
        REQUIRE(h.get_system_info().pending_target_slot == 2);
        h.on_klipper_restarted();
        CHECK(h.action() == AmsAction::IDLE);
    }
    CHECK(h.get_system_info().pending_target_slot == -1);
    CHECK(h.get_system_info().path_active_slot() == -1);
}

TEST_CASE("AFC dispatch resolves in the queue pass after the macro ack",
          "[ams][afc][dispatch][threading][1761]") {
    AfcLiveDispatchHelper h;

    REQUIRE(h.load_filament(2).success());
    REQUIRE(h.get_current_action() == AmsAction::LOADING);
    CHECK(helix::ui::UpdateQueueTestAccess::passes_until(
              [&] { return h.get_current_action() == AmsAction::IDLE; }) == 1);
}

TEST_CASE("AFC unload and tool change dispatch their own actions", "[ams][afc][dispatch][1183]") {
    SECTION("unload") {
        AfcDispatchAckHelper h;
        h.mark_filament_loaded(true);

        REQUIRE(h.unload_filament(1).success());
        REQUIRE(h.sent()[0] == "TOOL_UNLOAD LANE=lane2");
        REQUIRE(h.action() == AmsAction::UNLOADING);

        h.ack(0);
        REQUIRE(h.action() == AmsAction::IDLE);
        REQUIRE(trace_contains(h.trace(), AmsAction::UNLOADING));
    }

    SECTION("tool change") {
        AfcDispatchAckHelper h;

        REQUIRE(h.change_tool(1).success());
        REQUIRE(h.sent()[0] == "T1");
        // SELECTING, not LOADING: AFC's toolchanger states all map to SELECTING
        // and the operation carries the 300s toolchange budget.
        REQUIRE(h.action() == AmsAction::SELECTING);

        h.ack(0);
        REQUIRE(h.action() == AmsAction::IDLE);
        REQUIRE(trace_contains(h.trace(), AmsAction::SELECTING));
    }
}

TEST_CASE("AFC macro ack does not truncate a toolchange AFC took over",
          "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(0).success());
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC entered the toolchange for real and is driving its own state machine.
    h.feed_state("ToolSwap");
    REQUIRE(h.action() == AmsAction::SELECTING);

    // The macro's ack lands while AFC is still cutting. Forcing IDLE here would
    // end the operation in the UI 60 seconds early.
    h.ack(0);
    REQUIRE(h.action() == AmsAction::SELECTING);

    h.feed_state("Cutting");
    REQUIRE(h.action() == AmsAction::CUTTING);

    // AFC's own terminating frame is what resolves it.
    h.feed_state("Idle");
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC Idle re-echo between dispatch and ack still ends the operation",
          "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(2).success());
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC re-echoes the state it was already in. That is NOT AFC taking over —
    // it is the no-op — but the frame itself already produced the LOADING ->
    // IDLE transition the UI needs, so the later ack has nothing left to do.
    h.feed_state("Idle");
    REQUIRE(h.action() == AmsAction::IDLE);
    REQUIRE(trace_contains(h.trace(), AmsAction::LOADING));

    h.ack(0);
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC second dispatch invalidates the first dispatch's pending ack",
          "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(0).success());
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC re-echoes Idle, so the action is no longer busy and the next operation
    // passes check_preconditions() — but the first dispatch's ack is still out
    // there, unfired.
    h.feed_state("Idle");
    REQUIRE(h.action() == AmsAction::IDLE);

    h.mark_filament_loaded(true);
    REQUIRE(h.unload_filament(1).success());
    REQUIRE(h.action() == AmsAction::UNLOADING);

    // The stale ack must not resolve the unload that is now in flight.
    h.ack(0);
    REQUIRE(h.action() == AmsAction::UNLOADING);

    // The current dispatch's own ack still works.
    h.ack(1);
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC optimistic dispatch stamps the stuck-action clock",
          "[ams][afc][dispatch][timeout][1183]") {
    AfcDispatchAckHelper h;

    // Age the clock before dispatching. Without its own stamp the new action
    // would inherit this elapsed time and blow the 180s LOADING budget on its
    // very first poll.
    h.age_action(std::chrono::seconds(1000));
    REQUIRE(h.load_filament(0).success());

    h.age_action(std::chrono::seconds(170));
    (void)h.get_system_info(); // the sidebar's stall-watchdog poll
    REQUIRE(h.action() == AmsAction::LOADING);

    h.age_action(std::chrono::seconds(200));
    (void)h.get_system_info();
    REQUIRE(h.action() == AmsAction::ERROR);
    // A timeout on an action AFC never acknowledged must not latch: the latch
    // keys on last_raw_state_, which here is still AFC's idle token, and would
    // re-force ERROR on every subsequent Idle frame.
    REQUIRE_FALSE(h.latched());
    h.feed_state("Idle");
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC stuck-action timeout still fires for an op AFC took over",
          "[ams][afc][dispatch][timeout][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(0).success());
    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC reports the same busy state forever and no ack ever arrives.
    h.age_action(std::chrono::seconds(200));
    h.feed_state("Loading");

    REQUIRE(h.action() == AmsAction::ERROR);
    // AFC owns this action, so the latch DOES arm — otherwise the next frame's
    // "Loading" would map straight back to LOADING and flap.
    REQUIRE(h.latched());
    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::ERROR);
}

// Bypass: AFC answers for itself once the sidebar stops chaining (#1229 defect 6)
//
// allows_implicit_chaining() == false means the sidebar sends exactly one
// command and lets AFC refuse. That only helps if AFC actually refuses:
// execute_gcode() is fire-and-forget (returns success before Klipper answers,
// silent=true), so without the precondition in enable_bypass() the pass-through
// would report success and change nothing — a silent lie in place of an
// unrequested unload. These pin the refusal that makes the policy safe.
// The policy half is covered in test_ams_bypass_no_chaining.cpp.

TEST_CASE("AFC enable_bypass refuses while filament is at the toolhead",
          "[ams][afc][bypass][1229]") {
    AmsBackendAfcTestHelper helper;
    helper.set_running(true);
    helper.initialize_test_lanes(4);
    helper.set_supports_bypass(true);
    helper.set_current_slot(1);
    helper.set_filament_loaded(true);
    helper.clear_captured_gcodes();

    AmsError err = helper.enable_bypass();

    REQUIRE(err.result == AmsResult::WRONG_STATE);
    REQUIRE_FALSE(err.user_msg.empty());

    // Nothing may be sent at all — a SET_FILAMENT_SENSOR that Klipper later
    // rejected would still have returned SUCCESS to the caller.
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_FILAMENT_SENSOR"));
}

TEST_CASE("AFC enable_bypass sends the sensor enable when nothing is loaded",
          "[ams][afc][bypass][1229]") {
    AmsBackendAfcTestHelper helper;
    helper.set_running(true);
    helper.initialize_test_lanes(4);
    helper.set_supports_bypass(true);
    helper.set_current_slot(-1);
    helper.set_filament_loaded(false);
    helper.clear_captured_gcodes();

    AmsError err = helper.enable_bypass();

    REQUIRE(err.result == AmsResult::SUCCESS);
    // The AFC ctor defaults has_hardware_bypass_sensor to true; discovery
    // downgrades it to "virtual_bypass" only when Klipper publishes that object.
    REQUIRE(helper.has_gcode("SET_FILAMENT_SENSOR SENSOR=bypass ENABLE=1"));
}

// Tool number comes from th_extruder_name, not the AFC_extruder section name
//
// AFC derives a toolhead's tool index from `th_extruder_name`
// (AFC_extruder.py:222-223 = `config.get("extruder_name", <section name>)`,
// consumed at AFC_Toolchanger.py:231-232, both v1.2.0). That value is published
// in NO get_status() anywhere — configfile.settings is the only surface that
// carries it. Deriving it from the SECTION name fails on anything that is not
// `extruder<N>`, and must not fall back to mapping every toolhead to T0.

TEST_CASE("AFC extruder tool index prefers configfile extruder_name",
          "[ams][afc][extruder_tool_index]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);

    SECTION("section name that already carries the index") {
        REQUIRE(helper.tool_index_for("extruder") == 0);
        REQUIRE(helper.tool_index_for("extruder1") == 1);
        REQUIRE(helper.tool_index_for("extruder12") == 12);
    }

    SECTION("configfile resolves a section name that does not") {
        helper.seed_extruder_klipper_names({{"e0", "extruder"}, {"e1", "extruder1"}});
        REQUIRE(helper.tool_index_for("e0") == 0);
        REQUIRE(helper.tool_index_for("e1") == 1);
    }

    SECTION("configfile wins over a misleading section name") {
        // `[AFC_extruder extruder1]` with `extruder_name: extruder3` is legal:
        // AFC indexes on the option, so T3 is the right answer, not T1.
        helper.seed_extruder_klipper_names({{"extruder1", "extruder3"}});
        REQUIRE(helper.tool_index_for("extruder1") == 3);
    }

    SECTION("lookup is case-insensitive — Klipper lowercases section headers") {
        helper.seed_extruder_klipper_names({{"t1", "extruder1"}});
        REQUIRE(helper.tool_index_for("T1") == 1);
    }

    SECTION("unresolvable is -1, never a silent T0") {
        // `e1` must not become T0 (as a substr(8) parse would make it), nor
        // e2, e3 or any other toolhead.
        REQUIRE(helper.tool_index_for("e1") == -1);
        REQUIRE(helper.tool_index_for("e2") == -1);
        REQUIRE(helper.tool_index_for("left_head") == -1);
    }

    SECTION("a nonsense extruder_name falls back to the section name") {
        helper.seed_extruder_klipper_names({{"extruder2", "not_a_tool"}});
        REQUIRE(helper.tool_index_for("extruder2") == 2);
    }
}

TEST_CASE("AFC v1.1.0 resolves tool indices with no configfile answer at all",
          "[ams][afc][extruder_tool_index]") {
    // v1.1.0 has no `extruder_name` option on [AFC_extruder] (th_extruder_name
    // was introduced with the toolchanger in v1.2.0), so Klipper never
    // access-tracks the option and configfile.settings carries no value for it.
    // The section-name fallback is then the ONLY path, and it has to work
    // unaided — this is the majority of installed AFC hardware.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    SECTION("plain extruder<N> section names resolve without configfile") {
        REQUIRE(helper.tool_index_for("extruder") == 0);
        REQUIRE(helper.tool_index_for("extruder1") == 1);
        REQUIRE(helper.tool_index_for("extruder2") == 2);
    }

    SECTION("and lane attribution works end to end") {
        helper.feed_afc_state({{"extruders", {"extruder", "extruder1"}}});
        // T1 is the tool on the carriage; extruder1 is T1, so its lane wins and
        // extruder (T0) is ignored — the ordering-independence the old
        // substr(8)/catch path destroyed by mapping both to T0.
        helper.feed_status_update(nlohmann::json{{"toolchanger", {{"tool_number", 1}}}});
        REQUIRE(helper.get_system_info().current_tool == 1);

        nlohmann::json frame;
        frame["AFC_extruder extruder"] = {{"lane_loaded", "lane1"}};
        frame["AFC_extruder extruder1"] = {{"lane_loaded", "lane3"}};
        helper.feed_status_update(frame);

        REQUIRE(helper.get_system_info().current_slot == 2); // lane3, not lane1
    }
}

TEST_CASE_METHOD(AfcFourLanes, "AFC unresolvable extruder makes no lane attribution claim",
                 "[ams][afc][extruder_tool_index]") {
    // Two toolheads named so the section name carries no index. If both
    // resolved to T0 each would in turn claim to be the active tool, and
    // current_slot would track whichever AFC_extruder frame arrived last.
    // AFC.extruders is what routes an "AFC_extruder <name>" frame to a parser.
    helper.feed_afc_state({{"extruders", {"e0", "e1"}}});
    REQUIRE(helper.get_extruder_names() == std::vector<std::string>{"e0", "e1"});

    helper.feed_afc_extruder("e0", {{"lane_loaded", "lane1"}});
    helper.feed_afc_extruder("e1", {{"lane_loaded", "lane3"}});

    REQUIRE(helper.get_system_info().current_slot == -1);

    // Once configfile resolves the names, attribution works again: e1 is T1,
    // and with current_tool unset the first resolvable claim is honoured.
    helper.seed_extruder_klipper_names({{"e0", "extruder"}, {"e1", "extruder1"}});
    helper.feed_afc_extruder("e1", {{"lane_loaded", "lane3"}});
    REQUIRE(helper.get_system_info().current_slot == 2); // lane3
}

// A persisted override IS a user edit. The lock flags are how any consumer of
// the record tells one from a firmware reading (#965, #1649).
TEST_CASE("AFC configfile topology parses to a small result", "[ams][afc][configfile]") {
    using Topo = AmsBackendAfc::ConfigfileTopology;

    SECTION("extruder names are read, case-insensitively") {
        nlohmann::json response = {{"result",
                                    {{"status",
                                      {{"configfile",
                                        {{"settings",
                                          {{"AFC_extruder T0", {{"extruder_name", "extruder"}}},
                                           {"afc_extruder t1", {{"extruder_name", "extruder1"}}},
                                           {"afc_extruder t2", {{"pin_tool_start", "PA1"}}},
                                           {"afc_toolchanger tc", {{"x", 1}}},
                                           {"stepper_x", {{"step_pin", "PB1"}}}}}}}}}}}};
        const Topo topo = AmsBackendAfc::parse_configfile_topology(response);
        REQUIRE(topo.answered);
        REQUIRE(topo.extruder_names.size() == 2);
        CHECK(topo.extruder_names.at("t0") == "extruder");
        CHECK(topo.extruder_names.at("t1") == "extruder1");
    }

    SECTION("empty settings is still an answer") {
        nlohmann::json response = {
            {"result", {{"status", {{"configfile", {{"settings", nlohmann::json::object()}}}}}}}};
        const Topo topo = AmsBackendAfc::parse_configfile_topology(response);
        CHECK(topo.answered);
        CHECK(topo.extruder_names.empty());
    }

    SECTION("a response without settings is not an answer") {
        CHECK_FALSE(AmsBackendAfc::parse_configfile_topology(nlohmann::json::object()).answered);
        nlohmann::json no_settings = {{"result", {{"status", {{"configfile", {{"config", 1}}}}}}}};
        CHECK_FALSE(AmsBackendAfc::parse_configfile_topology(no_settings).answered);
        nlohmann::json not_object = {
            {"result", {{"status", {{"configfile", {{"settings", "oops"}}}}}}}};
        CHECK_FALSE(AmsBackendAfc::parse_configfile_topology(not_object).answered);
    }

    SECTION("a non-string extruder_name is skipped") {
        nlohmann::json response = {
            {"result",
             {{"status",
               {{"configfile", {{"settings", {{"afc_extruder t0", {{"extruder_name", 5}}}}}}}}}}}};
        CHECK(AmsBackendAfc::parse_configfile_topology(response).extruder_names.empty());
    }
}

TEST_CASE("AFC units report a toolhead sensor only when an AFC_extruder pin wires one",
          "[ams][afc][configfile][toolhead_sensor]") {
    auto settings = [](nlohmann::json extruder) {
        return nlohmann::json{
            {"result",
             {{"status",
               {{"configfile", {{"settings", {{"afc_extruder extruder", extruder}}}}}}}}}};
    };
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    int state_events = 0;
    helper.set_event_callback([&](const std::string& event, const std::string&) {
        state_events += event == AmsBackend::EVENT_STATE_CHANGED;
    });
    // Unknown until configfile answers.
    REQUIRE(helper.get_system_info().units[0].has_toolhead_sensor);

    SECTION("pin_tool_start wired") {
        AfcTestAccess::apply_configfile_topology(
            helper, AmsBackendAfc::parse_configfile_topology(
                        settings({{"pin_tool_start", "Turtle_1:PA1"}, {"pin_tool_end", ""}})));
        CHECK(helper.get_system_info().units[0].has_toolhead_sensor);
    }
    SECTION("only pin_tool_end wired") {
        AfcTestAccess::apply_configfile_topology(
            helper, AmsBackendAfc::parse_configfile_topology(settings({{"pin_tool_end", "PB2"}})));
        CHECK(helper.get_system_info().units[0].has_toolhead_sensor);
    }
    SECTION("tool_start borrowed from the buffer, no tool_end") {
        AfcTestAccess::apply_configfile_topology(
            helper,
            AmsBackendAfc::parse_configfile_topology(settings({{"pin_tool_start", "buffer"}})));
        CHECK_FALSE(helper.get_system_info().units[0].has_toolhead_sensor);
        // The flip repaints at once.
        CHECK(state_events == 1);
    }
    SECTION("a fitted answer changes nothing and fires nothing") {
        AfcTestAccess::apply_configfile_topology(
            helper, AmsBackendAfc::parse_configfile_topology(settings({{"pin_tool_end", "PB2"}})));
        CHECK(state_events == 0);
    }
    SECTION("an unanswered or extruder-less configfile keeps the default") {
        AfcTestAccess::apply_configfile_topology(
            helper, AmsBackendAfc::parse_configfile_topology(nlohmann::json::object()));
        CHECK(helper.get_system_info().units[0].has_toolhead_sensor);
        AfcTestAccess::apply_configfile_topology(
            helper,
            AmsBackendAfc::parse_configfile_topology(
                {{"result", {{"status", {{"configfile", {{"settings", {{"stepper_x", {}}}}}}}}}}}));
        CHECK(helper.get_system_info().units[0].has_toolhead_sensor);
        CHECK(state_events == 0);
    }
    SECTION("no pins at all") {
        AfcTestAccess::apply_configfile_topology(
            helper,
            AmsBackendAfc::parse_configfile_topology(settings({{"extruder_name", "extruder"}})));
        CHECK_FALSE(helper.get_system_info().units[0].has_toolhead_sensor);
    }
}

// Empty carriage: an AFC toolchanger with every head parked mounts no tool

TEST_CASE("AFC toolchanger with every head parked mounts no tool",
          "[ams][afc][toolchanger][empty-carriage]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.feed_afc_state({{"extruders", {"extruder", "extruder1"}}});

    helper.feed_status_update(
        nlohmann::json{{"toolchanger", {{"tool_number", 1}, {"status", "ready"}}}});
    REQUIRE(helper.get_system_info().current_tool == 1);
    CHECK(helper.carriage_can_be_empty());
    {
        auto topo = helix::build_ams_topology(&helper, 0);
        REQUIRE(topo.has_value());
        CHECK(topo->active_tool == 1);
    }

    helper.feed_status_update(
        nlohmann::json{{"toolchanger", {{"tool_number", -1}, {"status", "ready"}}}});
    REQUIRE(helper.get_system_info().current_tool == -1);

    // AFC.current_load can keep naming the lane of a docked head. Its mapped
    // tool is not mounted: the carriage holds nothing.
    helper.feed_afc_state({{"current_load", "lane2"}});
    CHECK(helper.get_system_info().current_tool == -1);
    CHECK(helper.get_system_info().current_slot == -1);

    auto topo = helix::build_ams_topology(&helper, 0);
    REQUIRE(topo.has_value());
    CHECK(topo->allows_empty_carriage);
    CHECK(topo->active_tool == -1);
}

TEST_CASE("AFC on_shuttle reporting an empty carriage mounts no tool",
          "[ams][afc][toolchanger][empty-carriage]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.feed_afc_state({{"extruders", {"extruder", "extruder1"}}});

    nlohmann::json frame;
    frame["AFC"] = {{"current_load", "lane2"}};
    frame["AFC_extruder extruder"] = {{"on_shuttle", false}};
    frame["AFC_extruder extruder1"] = {{"on_shuttle", false}};
    helper.feed_status_update(frame);

    CHECK(helper.carriage_can_be_empty());
    CHECK(helper.get_system_info().current_tool == -1);
}

TEST_CASE("Single-toolhead AFC keeps a carriage that cannot be empty",
          "[ams][afc][empty-carriage]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);

    helper.feed_afc_state({{"current_load", "lane2"}});
    CHECK_FALSE(helper.carriage_can_be_empty());
    CHECK(helper.get_system_info().current_tool == 1);

    helper.feed_afc_state({{"current_load", nullptr}});
    CHECK_FALSE(helper.carriage_can_be_empty());
    auto topo = helix::build_ams_topology(&helper, 0);
    REQUIRE(topo.has_value());
    CHECK_FALSE(topo->allows_empty_carriage);
}

TEST_CASE("AFC toolchanger: only the carriage's own fault marks the docked heads",
          "[ams][afc][toolchanger][empty-carriage]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.feed_afc_state({{"extruders", {"extruder", "extruder1"}}});
    helper.feed_status_update(
        nlohmann::json{{"toolchanger", {{"tool_number", 0}, {"status", "ready"}}}});
    REQUIRE(helper.carriage_can_be_empty());

    // A lane fault: AFC reports Error, the changer stays ready.
    helper.feed_afc_state({{"current_state", "Error"}});
    REQUIRE(helper.get_system_info().action == AmsAction::ERROR);
    CHECK_FALSE(helper.fault_concerns_carriage());

    // klipper-toolchanger itself reports the fault.
    helper.feed_status_update(nlohmann::json{{"toolchanger", {{"status", "error"}}}});
    CHECK(helper.fault_concerns_carriage());

    helper.feed_status_update(nlohmann::json{{"toolchanger", {{"status", "ready"}}}});
    CHECK_FALSE(helper.fault_concerns_carriage());
}
