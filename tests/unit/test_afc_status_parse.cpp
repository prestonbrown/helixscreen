// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;
using afc::LaneSurface;

TEST_CASE("AFC lane identity: a null spool_id clears, a non-integer says nothing",
          "[afc][status_parse][1195]") {
    for (auto surface : {LaneSurface::Status, LaneSurface::LaneData}) {
        const auto linked = afc::read_lane_identity(json{{"spool_id", 7}}, surface);
        REQUIRE(linked.spool_id);
        CHECK(*linked.spool_id == 7);

        // AFC writes spool_id=None on eject.
        const auto cleared = afc::read_lane_identity(json{{"spool_id", nullptr}}, surface);
        REQUIRE(cleared.spool_id);
        CHECK_FALSE(cleared.spool_id->has_value());

        CHECK_FALSE(afc::read_lane_identity(json{{"spool_id", "x"}}, surface).spool_id);
        CHECK_FALSE(afc::read_lane_identity(json{{"spool_id", 7.5}}, surface).spool_id);
        CHECK_FALSE(afc::read_lane_identity(json::object(), surface).spool_id);
    }
}

TEST_CASE("AFC lane identity reads each surface's own name spelling", "[afc][status_parse][833]") {
    // lane_data uses the shared `name` (#833) with the attribute name as a
    // fallback; get_status says `filament_name`, and `name` there is the lane.
    const json both{{"name", "Ambrosia Pink"}, {"filament_name", "Galaxy Black"}};
    CHECK(afc::read_lane_identity(both, LaneSurface::LaneData).spool_name == "Ambrosia Pink");
    CHECK(afc::read_lane_identity(both, LaneSurface::Status).spool_name == "Galaxy Black");
    CHECK_FALSE(afc::read_lane_identity(json{{"name", "lane1"}}, LaneSurface::Status).spool_name);
    CHECK(afc::read_lane_identity(json{{"filament_name", "X"}}, LaneSurface::LaneData).spool_name ==
          "X");

    // Empty is the eject clear; a non-string says nothing.
    CHECK(afc::read_lane_identity(json{{"name", ""}}, LaneSurface::LaneData).spool_name == "");
    CHECK_FALSE(
        afc::read_lane_identity(json{{"filament_name", nullptr}}, LaneSurface::Status).spool_name);
}

TEST_CASE("AFC lane identity reads colour, material and every vendor spelling",
          "[afc][status_parse]") {
    const auto id = afc::read_lane_identity(
        json{{"color", "#c1c1c1"}, {"material", ""}, {"vendor_name", "Polymaker"}},
        LaneSurface::LaneData);
    REQUIRE(id.color);
    CHECK(id.color->kind == ams::ColorReadingKind::Observed);
    CHECK(id.color->rgb == 0xc1c1c1u);
    CHECK(id.material == "");
    CHECK(id.brand == "Polymaker");

    CHECK(afc::read_lane_identity(json{{"color", ""}}, LaneSurface::Status).color->kind ==
          ams::ColorReadingKind::Cleared);
    CHECK(afc::read_lane_identity(json{{"color", "#zzzzzz"}}, LaneSurface::Status).color->kind ==
          ams::ColorReadingKind::NoReading);
    CHECK(afc::read_lane_identity(json{{"color", "00ff00"}}, LaneSurface::Status).color->rgb ==
          0x00ff00u);

    for (const char* key : {"vendor_name", "spool_vendor", "vendor", "brand"}) {
        CHECK(afc::read_lane_identity(json{{key, "Elegoo"}}, LaneSurface::Status).brand ==
              "Elegoo");
    }
    // An empty vendor is not a clear.
    CHECK_FALSE(afc::read_lane_identity(json{{"spool_vendor", ""}}, LaneSurface::Status).brand);
}

TEST_CASE("AFC lane status parse reads the sensor and spool fields a frame carries",
          "[afc][status_parse]") {
    const auto d = afc::parse_lane_status(json{{"prep", true},
                                               {"buffer_status", "Advancing"},
                                               {"dist_hub", 200.0},
                                               {"selector", false},
                                               {"td1_td", ""},
                                               {"multi_color_hexes", {"FF0000", "", "#00FF00"}},
                                               {"bed_temp", 59.6},
                                               {"extruder_temp", nullptr},
                                               {"weight", 931.7},
                                               {"runout_lane", nullptr}});
    CHECK(d.prep == true);
    CHECK_FALSE(d.load);
    CHECK(d.buffer_status == "Advancing");
    CHECK(d.dist_hub == Catch::Approx(200.0f));
    CHECK(d.selector == false);
    CHECK(d.td1_td == -1.0f);
    CHECK(d.multi_color_hexes == "#FF0000,#00FF00");
    REQUIRE(d.bed_temp);
    CHECK(*d.bed_temp == 60);
    REQUIRE(d.extruder_temp);
    CHECK_FALSE(d.extruder_temp->has_value());
    CHECK(d.weight == Catch::Approx(931.7f));
    REQUIRE(d.runout_lane);
    CHECK_FALSE(d.runout_lane->has_value());
    CHECK_FALSE(d.map);
    CHECK(d.presence_reading);

    // A weight-only delta says nothing about presence, a prep key of any type does.
    CHECK_FALSE(afc::parse_lane_status(json{{"weight", 10}}).presence_reading);
    CHECK(afc::parse_lane_status(json{{"load", nullptr}}).presence_reading);
    CHECK(afc::parse_lane_status(json{{"status", "None"}}).presence_reading);
}

TEST_CASE("AFC lane status vocabulary maps to slot status", "[afc][status_parse]") {
    CHECK(afc::lane_slot_status(true, "", false, false) == SlotStatus::LOADED);
    CHECK(afc::lane_slot_status(false, "Tooled", false, false) == SlotStatus::LOADED);
    CHECK(afc::lane_slot_status(false, "Tool Loaded", false, false) == SlotStatus::LOADED);
    // "Loaded" is hub-loaded.
    CHECK(afc::lane_slot_status(false, "Loaded", false, false) == SlotStatus::AVAILABLE);
    CHECK(afc::lane_slot_status(false, "", true, false) == SlotStatus::AVAILABLE);
    CHECK(afc::lane_slot_status(false, "", false, true) == SlotStatus::AVAILABLE);
    CHECK(afc::lane_slot_status(false, "None", false, false) == SlotStatus::EMPTY);
    CHECK(afc::lane_slot_status(false, "Error", false, false) == SlotStatus::EMPTY);
}

TEST_CASE("AFC lane map parse accepts both wire shapes and skips junk entries",
          "[afc][status_parse]") {
    using V = std::vector<int>;
    CHECK(afc::parse_lane_map(json("T3")) == V{3});
    CHECK(afc::parse_lane_map(json{"T16", "bad", "T2"}) == V{16, 2});
    CHECK(afc::parse_lane_map(json("T14,13")).empty());
    CHECK(afc::parse_lane_map(json("T65")).empty());
    CHECK(afc::parse_lane_map(json("T64")) == V{64});
    CHECK(afc::parse_lane_map(json(nullptr)).empty());
    CHECK(afc::parse_lane_map(json("T")).empty());

    const auto d = afc::parse_lane_status(json{{"map", nullptr}, {"current_map", "T11"}});
    REQUIRE(d.map);
    CHECK(d.map->empty());
    CHECK(d.current_map == V{11});
}

TEST_CASE("AFC lane_data parse keeps object records and reads only stated presence",
          "[afc][status_parse][lane_data]") {
    const auto records = afc::parse_lane_data(json{
        // AFC's real record shape: metadata only, no presence keys.
        {"lane1", {{"color", "#000000"}, {"material", "ASA"}, {"bed_temp", nullptr}}},
        {"lane2", {{"empty", true}}},
        {"lane3", {{"available", true}}},
        {"lane4", {{"loaded", true}}},
        {"T5", {{"tool_loaded", true}}},
        {"junk", "not a record"},
    });
    REQUIRE(records.size() == 5);
    CHECK(records[0].key == "T5");
    CHECK(records[0].tool == 5);
    CHECK(records[0].status == SlotStatus::LOADED);
    CHECK(records[1].key == "lane1");
    CHECK(records[1].tool == -1);
    CHECK_FALSE(records[1].status);
    CHECK(records[1].identity.material == "ASA");
    CHECK(records[2].status == SlotStatus::EMPTY);
    CHECK(records[3].status == SlotStatus::AVAILABLE);
    CHECK(records[4].status == SlotStatus::AVAILABLE);

    CHECK(afc::lane_data_tool_key("T0") == 0);
    CHECK(afc::lane_data_tool_key("T64") == 64);
    CHECK(afc::lane_data_tool_key("T65") == -1);
    CHECK(afc::lane_data_tool_key("T1a") == -1);
    CHECK(afc::lane_data_tool_key("lane1") == -1);
    CHECK(afc::parse_lane_data(json::array()).empty());
}

TEST_CASE("AFC database reply payload is read from result.value only",
          "[afc][status_parse][database]") {
    const json resp{{"jsonrpc", "2.0"},
                    {"result", {{"namespace", "lane_data"}, {"value", {{"lane1", 1}}}}}};
    CHECK(afc::database_item_value(resp).is_object());
    // A bare payload cannot be told apart from an envelope, so it is not one.
    CHECK_FALSE(afc::database_item_value(json{{"version", "1.0.40"}}).is_object());
    CHECK_FALSE(afc::database_item_value(json{{"result", {{"namespace", "x"}}}}).is_object());
    CHECK(afc::database_item_value(json::array()).is_null());
}

TEST_CASE("AFC state parse reads the lane, toolchange and message fields", "[afc][status_parse]") {
    const auto d = afc::parse_state(json{{"current_lane", ""},
                                         {"current_load", "lane2"},
                                         {"current_toolchange", 1},
                                         {"number_of_toolchanges", 161},
                                         {"message", {{"message", "Jam"}, {"type", "error"}}},
                                         {"next_lane", nullptr},
                                         {"spoolman", false},
                                         {"multiple_tool_mapping", false}});
    // An empty current_lane falls through to current_load.
    CHECK(d.loaded_lane == "lane2");
    CHECK(d.mentions_lane);
    CHECK_FALSE(d.lane_stated_null);
    // AFC's count is 1-based; the index is 0-based.
    CHECK(d.toolchange_index == 0);
    CHECK(d.number_of_toolchanges == 161);
    REQUIRE(d.message);
    CHECK(d.message->text == "Jam");
    CHECK(d.message->type == "error");
    REQUIRE(d.next_lane);
    CHECK_FALSE(d.next_lane->has_value());
    CHECK_FALSE(d.spoolman);
    CHECK(d.multiple_tool_mapping);

    const auto unloaded =
        afc::parse_state(json{{"current_load", nullptr}, {"current_toolchange", 0}});
    CHECK(unloaded.lane_stated_null);
    CHECK(unloaded.loaded_lane.empty());
    CHECK(unloaded.toolchange_index == -1);
    CHECK_FALSE(afc::parse_state(json{{"current_toolchange", 2.0}}).toolchange_index);
}

TEST_CASE("AFC state parse reads units, system extruders and name lists", "[afc][status_parse]") {
    const auto d = afc::parse_state(
        json{{"units",
              {"Box_Turtle Turtle_1",
               "ViViD V1",
               "nospace",
               {{"name", "AMS_1"}, {"lanes", {"lane5"}}, {"connected", true}}}},
             {"hubs", {"Turtle_1", 3}},
             {"system",
              {{"num_extruders", 2},
               {"extruders",
                {{"extruder1", {{"lane_loaded", nullptr}, {"tool_stn", 42.0}}},
                 {"extruder", {{"lane_loaded", "lane1"}, {"lanes", {"lane1", "lane2"}}}}}}}}});
    REQUIRE(d.units);
    REQUIRE(d.units->refs.size() == 2);
    CHECK(d.units->refs[0].klipper_key == "AFC_BoxTurtle Turtle_1");
    CHECK(d.units->refs[1].klipper_key == "AFC_vivid V1");
    REQUIRE(d.units->entries.size() == 4);
    CHECK_FALSE(d.units->entries[0]);
    REQUIRE(d.units->entries[3]);
    CHECK(d.units->entries[3]->connected == true);
    CHECK(d.units->lane_map.at("AMS_1") == std::vector<std::string>{"lane5"});
    CHECK(d.hubs == std::vector<std::string>{"Turtle_1"});
    CHECK(d.num_extruders == 2);
    REQUIRE(d.system_extruders);
    REQUIRE(d.system_extruders->size() == 2);
    CHECK((*d.system_extruders)[0].name == "extruder");
    CHECK((*d.system_extruders)[0].available_lanes.size() == 2);
    CHECK((*d.system_extruders)[1].lane_loaded.empty());
    CHECK((*d.system_extruders)[1].tool_stn == Catch::Approx(42.0f));
}

TEST_CASE("AFC buffer parse reads an FPS_PSF frame into a pressure reading",
          "[afc][status_parse][buffer]") {
    // Built from AFCFPSBuffer.get_status() in AFC-Klipper-Add-On v1.2.0 (a06f14d):
    // the base AFCBuffer keys plus fps_value, smoothed_fps (both rounded to 3
    // places) and set_point. distance_to_fault is None until a fault is tracked.
    const json frame{{"state", "Neutral"},
                     {"lanes", {"lane1", "lane2"}},
                     {"enabled", true},
                     {"rotation_distance", 22.678},
                     {"active_lane", "lane1"},
                     {"multiplier_high", 1.1},
                     {"multiplier_low", 0.9},
                     {"multiplier", 1.0},
                     {"fault_detection_enabled", false},
                     {"error_sensitivity", 0.0},
                     {"fault_timer", nullptr},
                     {"distance_to_fault", nullptr},
                     {"fps_value", 0.512},
                     {"smoothed_fps", -0.004},
                     {"set_point", 0.5}};
    BufferHealth health;
    afc::apply_buffer_delta(health, afc::parse_buffer(frame));
    CHECK(health.has_fps());
    CHECK(health.fps_reported);
    // Slightly below zero is max tension, still a reading.
    CHECK(health.smoothed_fps == Catch::Approx(-0.004f));
    CHECK(health.fps_value == Catch::Approx(0.512f));
    CHECK(health.fps_set_point == Catch::Approx(0.5f));
    CHECK(health.motion == BufferMotion::Unknown);
    CHECK(health.active_lane == "lane1");
    CHECK(health.distance_to_fault == -1.0f);
}

TEST_CASE("AFC buffer delta overlays what it carries and clears what goes null",
          "[afc][status_parse][buffer][1149]") {
    BufferHealth health;
    afc::apply_buffer_delta(health, afc::parse_buffer(json{{"state", "Trailing"},
                                                           {"active_lane", "lane2"},
                                                           {"rotation_distance", 22.678},
                                                           {"multiplier", 1},
                                                           {"multiplier_low", 0.9},
                                                           {"error_sensitivity", 7},
                                                           {"fault_timer", 1.5},
                                                           {"distance_to_fault", 25.5}}));
    CHECK(health.motion == BufferMotion::Trailing);
    CHECK(health.multiplier == Catch::Approx(1.0f));
    CHECK_FALSE(health.has_fps());

    // A state-only delta leaves the rest standing.
    afc::apply_buffer_delta(health, afc::parse_buffer(json{{"state", "Advancing"}}));
    CHECK(health.motion == BufferMotion::Advancing);
    CHECK(health.active_lane == "lane2");
    CHECK(health.multiplier_low == Catch::Approx(0.9f));
    CHECK(health.error_sensitivity == Catch::Approx(7.0f));
    CHECK(health.distance_to_fault == Catch::Approx(25.5f));

    // AFC nulls these while the buffer is idle: the not-reported sentinel.
    afc::apply_buffer_delta(health, afc::parse_buffer(json{{"active_lane", nullptr},
                                                           {"rotation_distance", nullptr},
                                                           {"fault_timer", nullptr}}));
    CHECK(health.active_lane.empty());
    CHECK(health.rotation_distance == -1.0f);
    CHECK(health.fault_timer == -1.0f);
}

TEST_CASE("AFC hub and extruder parse keep silence apart from a clear", "[afc][status_parse]") {
    const auto hub = afc::parse_hub(json{{"state", "x"}, {"afc_bowden_length", 1285.0}});
    CHECK_FALSE(hub.state);
    CHECK(hub.bowden_length == Catch::Approx(1285.0f));

    const auto ext = afc::parse_extruder_object(
        json{{"tool_start_status", true}, {"lane_loaded", nullptr}, {"status", "ToolDock"}});
    CHECK(ext.tool_start == true);
    CHECK_FALSE(ext.tool_end);
    CHECK_FALSE(ext.on_shuttle);
    REQUIRE(ext.lane_loaded);
    CHECK_FALSE(ext.lane_loaded->has_value());
    CHECK(ext.status == "ToolDock");
    CHECK_FALSE(afc::parse_extruder_object(json{{"lane_loaded", 3}}).lane_loaded);
}

TEST_CASE("AFC unit topology follows per-lane hub routing", "[afc][status_parse][topology]") {
    const std::vector<std::string> lanes{"lane1", "lane2"};
    using R = std::unordered_map<std::string, std::string>;
    CHECK(afc::derive_unit_topology(lanes, 1, true, R{{"lane1", "Turtle_1"}, {"lane2", "Turtle_1"}})
              .topology == PathTopology::HUB);
    CHECK(afc::derive_unit_topology(lanes, 1, true, R{{"lane1", "HTLF_1"}, {"lane2", "direct"}})
              .topology == PathTopology::MIXED);
    CHECK(
        afc::derive_unit_topology(lanes, 2, false, R{{"lane1", "direct"}, {"lane2", "direct_load"}})
            .topology == PathTopology::PARALLEL);
    CHECK(afc::derive_unit_topology(lanes, 2, true, R{{"lane1", "h1"}, {"lane2", "h2"}}).topology ==
          PathTopology::PARALLEL);

    // An unparsed lane is neither direct nor hub-routed.
    const auto partial = afc::derive_unit_topology(lanes, 1, true, R{{"lane1", "Turtle_1"}});
    CHECK(partial.topology == PathTopology::HUB);
    CHECK(partial.lane_is_hub_routed == std::vector<bool>{true, false});
}

TEST_CASE("AFC OpenAMS environment parse reads only OpenAMS humidity chips",
          "[afc][status_parse]") {
    const auto readings = afc::parse_oams_environment(
        json{{"aht3x AMS_1", {{"temperature", 24.5}, {"humidity", 38.0}}},
             {"temperature_oams AMS_2", {{"temperature", 25.0}}},
             {"extruder", {{"temperature", 210.0}}}});
    REQUIRE(readings.size() == 2);
    CHECK(readings[0].unit == "ams_1");
    CHECK(readings[0].humidity_pct == Catch::Approx(38.0f));
    CHECK(readings[1].unit == "ams_2");
    CHECK_FALSE(readings[1].humidity_pct);
}
