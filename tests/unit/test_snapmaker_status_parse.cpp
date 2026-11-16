// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_status_json.h"
#include "snapmaker_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

TEST_CASE("Snapmaker status parse reads only the extruder fields a frame carries",
          "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_extruder_delta(json{{"temperature", 215.5}});
    CHECK_FALSE(d.state);
    CHECK_FALSE(d.park_pin);
    CHECK_FALSE(d.active_pin);
    CHECK_FALSE(d.activating_move);
    CHECK_FALSE(d.switch_count);
    CHECK_FALSE(d.extruder_offset[0]);

    const auto named = snapmaker::parse_extruder_delta(
        json{{"park_pin", false}, {"extruder_offset", json::array({1.5})}});
    REQUIRE(named.park_pin);
    CHECK(*named.park_pin == false);
    REQUIRE(named.extruder_offset[0]);
    CHECK_FALSE(named.extruder_offset[1]);
}

TEST_CASE("Snapmaker status parse refuses fields of the wrong type", "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_extruder_delta(
        json{{"state", 3}, {"park_pin", "yes"}, {"switch_count", "7"}, {"extruder_offset", "x"}});
    CHECK_FALSE(d.state);
    CHECK_FALSE(d.park_pin);
    CHECK_FALSE(d.switch_count);
    CHECK_FALSE(d.extruder_offset[0]);
}

TEST_CASE("Snapmaker status parse files each extruder object under its tool",
          "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_status(json{{"extruder", json{{"state", "ACTIVE"}}},
                                                {"extruder2", json{{"state", "PARKED"}}},
                                                {"extruder3", "not an object"},
                                                {"toolhead", json{{"extruder", "extruder2"}}}});
    REQUIRE(d.extruders[0]);
    CHECK(*d.extruders[0]->state == "ACTIVE");
    CHECK_FALSE(d.extruders[1]);
    REQUIRE(d.extruders[2]);
    CHECK(*d.extruders[2]->state == "PARKED");
    CHECK_FALSE(d.extruders[3]);
    CHECK(d.toolhead_extruder == "extruder2");

    CHECK_FALSE(snapmaker::parse_status(json::object()).toolhead_extruder);
}

TEST_CASE("read_indexed leaves an entry empty unless the array holds a matching value there",
          "[snapmaker][status_parse]") {
    const json obj{{"flags", json::array({true, 1, false})},
                   {"names", json::array({"a", "b"})},
                   {"nums", json::array({1, 2.5, "x"})},
                   {"scalar", 5}};

    const auto flags = ams::read_indexed<bool, 4>(obj, "flags");
    CHECK(flags[0] == true);
    CHECK_FALSE(flags[1]); // a number is not a bool
    CHECK(flags[2] == false);
    CHECK_FALSE(flags[3]); // shorter than the index

    const auto names = ams::read_indexed<std::string, 4>(obj, "names");
    CHECK(names[1] == "b");
    CHECK_FALSE(names[2]);

    const auto nums = ams::read_indexed<int, 4>(obj, "nums");
    CHECK(nums[0] == 1);
    CHECK(nums[1] == 2);
    CHECK_FALSE(nums[2]);

    for (const char* key : {"scalar", "missing"}) {
        const auto none = ams::read_indexed<int, 4>(obj, key);
        for (const auto& v : none) {
            CHECK_FALSE(v);
        }
    }
}

TEST_CASE("Snapmaker status parse reads filament_detect entries independently",
          "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_status(
        json{{"filament_detect",
              json{{"info", json::array({json{{"MAIN_TYPE", "PLA"}}, "junk", json::object()})},
                   {"state", json::array({1, "no", 0})}}}});
    REQUIRE(d.filament_detect);
    REQUIRE(d.filament_detect->info[0]);
    CHECK(d.filament_detect->info[0]->main_type == "PLA");
    CHECK_FALSE(d.filament_detect->info[1]);
    CHECK(d.filament_detect->info[2]);
    CHECK_FALSE(d.filament_detect->info[3]);
    CHECK(d.filament_detect->state[0] == 1);
    CHECK_FALSE(d.filament_detect->state[1]);
    CHECK(d.filament_detect->state[2] == 0);

    CHECK_FALSE(snapmaker::parse_status(json{{"filament_detect", 3}}).filament_detect);
}

TEST_CASE("Snapmaker status parse reads feed channels left then right, by lane",
          "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_status(
        json{{"filament_feed right",
              json{{"extruder3", json{{"channel_state", "wait_insert"}, {"module_exist", true}}},
                   {"extruder2", json{{"filament_detected", false}}}}},
             {"filament_feed left",
              json{{"extruder1", json{{"channel_state", "load_finish"}, {"channel_error", "ok"}}},
                   {"extruder0",
                    json{{"filament_detected", true}, {"channel_action_state", "load"}}}}}});
    REQUIRE(d.feed_channels.size() == 4);
    CHECK(d.feed_channels[0].lane == 0);
    CHECK(d.feed_channels[1].lane == 1);
    CHECK(d.feed_channels[2].lane == 2);
    CHECK(d.feed_channels[3].lane == 3);

    CHECK(d.feed_channels[0].filament_detected == true);
    CHECK(d.feed_channels[0].channel_action_state == "load");
    CHECK_FALSE(d.feed_channels[0].channel_state);
    CHECK(d.feed_channels[1].channel_state == "load_finish");
    CHECK_FALSE(d.feed_channels[1].filament_detected);
    CHECK(d.feed_channels[2].filament_detected == false);
    CHECK(d.feed_channels[3].module_exist == true);
}

TEST_CASE("Snapmaker status parse reads a null feed field as absent", "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_status(
        json{{"filament_feed left", json{{"extruder0", json{{"filament_detected", nullptr},
                                                            {"channel_state", nullptr},
                                                            {"channel_error", nullptr}}}}}});
    REQUIRE(d.feed_channels.size() == 1);
    CHECK_FALSE(d.feed_channels[0].filament_detected);
    CHECK_FALSE(d.feed_channels[0].channel_state);
    CHECK_FALSE(d.feed_channels[0].channel_error);
}

TEST_CASE("Snapmaker status parse reads the batch macro's doing flag only under its own key",
          "[snapmaker][status_parse]") {
    const json frame{{"gcode_macro AUTO_FEEDING_BATCH", json{{"doing", false}}},
                     {"gcode_macro OTHER", json{{"doing", true}}}};

    CHECK(snapmaker::parse_status(frame, "gcode_macro AUTO_FEEDING_BATCH").batch_doing == false);
    CHECK(snapmaker::parse_status(frame, "gcode_macro OTHER").batch_doing == true);
    CHECK_FALSE(snapmaker::parse_status(frame, "").batch_doing);
    CHECK_FALSE(snapmaker::parse_status(frame, "gcode_macro MISSING").batch_doing);
    CHECK_FALSE(
        snapmaker::parse_status(json{{"gcode_macro M", json{{"doing", 1}}}}, "gcode_macro M")
            .batch_doing);
}

TEST_CASE("Snapmaker status parse reads print_task_config without inventing what it omits",
          "[snapmaker][status_parse]") {
    const auto none = snapmaker::parse_status(json::object());
    CHECK_FALSE(none.print_task_config);

    const auto d = snapmaker::parse_status(
        json{{"print_task_config", json{{"extruder_map_table", json::array({2, 7, "x", -1, 3})},
                                        {"extruders_used", json::array({true, 1, false})},
                                        {"filament_exist", json::array({true, false})},
                                        {"filament_type", json::array({"PLA", 5, "PETG"})},
                                        {"filament_color_rgba", json::array({"112233FF", "FFF"})},
                                        {"auto_replenish_filament", true}}}});
    REQUIRE(d.print_task_config);
    const auto& ptc = *d.print_task_config;

    // Out-of-range and non-integer heads are -1, never clamped to head 0.
    REQUIRE(ptc.extruder_map_table);
    CHECK(*ptc.extruder_map_table == std::vector<int>{2, -1, -1, -1, 3});
    REQUIRE(ptc.extruders_used);
    CHECK(*ptc.extruders_used == std::vector<bool>{true, false, false});

    CHECK(ptc.filament_exist[0] == true);
    CHECK(ptc.filament_exist[1] == false);
    CHECK_FALSE(ptc.filament_exist[2]);
    CHECK(ptc.filament_type[0] == "PLA");
    CHECK_FALSE(ptc.filament_type[1]);
    CHECK(ptc.filament_type[2] == "PETG");
    CHECK_FALSE(ptc.filament_vendor[0]);
    CHECK(ptc.preferences.auto_replenish == true);
    CHECK_FALSE(ptc.preferences.end_led_turn_off);

    CHECK_FALSE(snapmaker::parse_status(json{{"print_task_config", json::object()}})
                    .print_task_config->extruder_map_table);
}

TEST_CASE("Snapmaker rgb_from_rgba_hex reads the colour the lane grammar names",
          "[snapmaker][status_parse]") {
    CHECK(snapmaker::rgb_from_rgba_hex("112233FF") == 0x112233u);
    CHECK(snapmaker::rgb_from_rgba_hex("AABBCC") == 0xAABBCCu);
    CHECK(snapmaker::rgb_from_rgba_hex("#112233FF") == 0x112233u);
    CHECK(snapmaker::rgb_from_rgba_hex("aabbcc") == 0xAABBCCu);

    // A malformed value is no colour at all, where a leading-digits read would
    // have taken 0x112233 out of the first one.
    CHECK_FALSE(snapmaker::rgb_from_rgba_hex("112233ZZ"));
    CHECK_FALSE(snapmaker::rgb_from_rgba_hex("12G456"));
    CHECK_FALSE(snapmaker::rgb_from_rgba_hex(""));
    CHECK_FALSE(snapmaker::rgb_from_rgba_hex("#"));
}

TEST_CASE("Snapmaker status parse reads the fitted nozzle a head reports",
          "[snapmaker][status_parse][nozzle]") {
    const auto d = snapmaker::parse_extruder_delta(
        json{{"nozzle_diameter", 0.6}, {"nozzle_volume_type", "high_flow"}});
    REQUIRE(d.nozzle_diameter);
    CHECK(*d.nozzle_diameter == 0.6);
    REQUIRE(d.nozzle_volume_type);
    CHECK(*d.nozzle_volume_type == "high_flow");

    const auto absent = snapmaker::parse_extruder_delta(json{{"temperature", 200}});
    CHECK_FALSE(absent.nozzle_diameter);
    CHECK_FALSE(absent.nozzle_volume_type);
}

TEST_CASE("Snapmaker status parse ignores a nozzle reading that names no nozzle",
          "[snapmaker][status_parse][nozzle]") {
    for (const json& bad : {json(0.0), json(-0.4), json("0.4"), json(nullptr)}) {
        CHECK_FALSE(
            snapmaker::parse_extruder_delta(json{{"nozzle_diameter", bad}}).nozzle_diameter);
    }
    CHECK_FALSE(
        snapmaker::parse_extruder_delta(json{{"nozzle_volume_type", 1}}).nozzle_volume_type);
}

TEST_CASE("Snapmaker tool state keeps a nozzle field a later frame omits",
          "[snapmaker][status_parse][nozzle]") {
    ExtruderToolState s;
    s.apply(snapmaker::parse_extruder_delta(
        json{{"nozzle_diameter", 0.4}, {"nozzle_volume_type", "standard"}}));
    s.apply(snapmaker::parse_extruder_delta(json{{"nozzle_diameter", 0.2}}));
    REQUIRE(s.nozzle_diameter);
    CHECK(*s.nozzle_diameter == 0.2);
    REQUIRE(s.nozzle_volume_type);
    CHECK(*s.nozzle_volume_type == "standard");
}
