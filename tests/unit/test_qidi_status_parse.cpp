// SPDX-License-Identifier: GPL-3.0-or-later

#include "qidi_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

TEST_CASE("QIDI save_variables parse reads slot words and tool routes inside the slot count",
          "[qidi][status_parse]") {
    auto v = qidi::parse_save_variables(json{{"box_count", 2},
                                             {"enable_box", 0},
                                             {"value_t1", "slot6"},
                                             {"value_t2", "slot8"},
                                             {"value_t9", "slot0"},
                                             {"slot0", 2},
                                             {"slot5", -1},
                                             {"slot7", "full"}},
                                        4);
    CHECK(v.box_count == 2);
    CHECK(v.enable_box == false);
    CHECK(v.tool_slots == std::vector<std::pair<int, int>>{{1, 6}});
    CHECK(v.slot_state[0] == 2);
    CHECK(v.slot_state[5] == -1);
    CHECK_FALSE(v.slot_state[7]);
    CHECK(v.has_slot_or_action_key);

    auto known = qidi::parse_save_variables(json{{"box_count", 5}, {"slot5", 1}}, 4);
    CHECK_FALSE(known.box_count);
    CHECK_FALSE(known.slot_state[5]); // outside the four slots already known
    CHECK_FALSE(known.has_slot_or_action_key);
}

TEST_CASE("QIDI save_variables parse reads last_load_slot and the tool-change flag",
          "[qidi][status_parse]") {
    CHECK(qidi::parse_save_variables(json{{"last_load_slot", "slot-1"}}, 4).last_load_slot == -1);
    CHECK(qidi::parse_save_variables(json{{"last_load_slot", "slot3"}}, 4).last_load_slot == 3);
    CHECK_FALSE(qidi::parse_save_variables(json{{"last_load_slot", "slot4"}}, 4).last_load_slot);
    CHECK_FALSE(qidi::parse_save_variables(json{{"last_load_slot", 2}}, 4).last_load_slot);

    auto loading = qidi::parse_save_variables(json{{"is_tool_change", 1}}, 4);
    CHECK(loading.tool_change);
    CHECK(loading.has_slot_or_action_key);
    auto odd = qidi::parse_save_variables(json{{"is_tool_change", "yes"}}, 4);
    CHECK_FALSE(odd.tool_change);
    CHECK(odd.has_slot_or_action_key);
}

TEST_CASE("QIDI save_variables parse tells product ids, row ids and colour lists apart",
          "[qidi][status_parse]") {
    auto v = qidi::parse_save_variables(json{{"filament_slot0", " QFA90 "},
                                             {"color_slot0", json::array({"228332"})},
                                             {"vendor_slot0", 0},
                                             {"filament_slot1", " 5 "},
                                             {"color_slot1", 12},
                                             {"vendor_slot1", -3},
                                             {"filament_slot2", "qfa90"},
                                             {"color_slot2", json::array()}},
                                        4);
    CHECK(v.ids[0].filament_code == "QFA90");
    CHECK_FALSE(v.ids[0].filament_id);
    CHECK(v.ids[0].color_list);
    CHECK(v.ids[0].color_rgb == 0x228332u);
    CHECK(v.ids[0].vendor_id == 0);

    CHECK_FALSE(v.ids[1].filament_code);
    CHECK(v.ids[1].filament_id == 5);
    CHECK_FALSE(v.ids[1].color_list);
    CHECK(v.ids[1].color_id == 12);
    CHECK_FALSE(v.ids[1].vendor_id);

    CHECK_FALSE(v.ids[2].filament_code);
    CHECK_FALSE(v.ids[2].filament_id);
    CHECK(v.ids[2].color_list);
    CHECK_FALSE(v.ids[2].color_rgb);
    CHECK(v.ignored_id_keys == std::vector<std::string>{"vendor_slot1", "filament_slot2"});
}

TEST_CASE("QIDI controller status parse files box_filament_info keys by global slot",
          "[qidi][status_parse]") {
    auto d = qidi::parse_controller_status(json{{"box",
                                                 {{"box1_filament_info_2_filament_id", "QFL30"},
                                                  {"box1_filament_info_2_filament_type", "PLA"},
                                                  {"box1_filament_info_2_max_temp", 230.7},
                                                  {"box0_filament_info_0_filament_id", "12"},
                                                  {"box4_filament_info_0_vendor_str", "QIDI"},
                                                  {"boxA_filament_info_0_vendor_str", "QIDI"},
                                                  {"box0_filament_info_1_min_temp", "190"}}}});
    REQUIRE(d.size() == 3);
    CHECK(d.at(6).filament_id == "QFL30");
    CHECK(d.at(6).filament_type == "PLA");
    CHECK(d.at(6).max_temp == 230);
    CHECK_FALSE(d.at(6).vendor);
    CHECK(d.at(0).filament_id == "");
    CHECK_FALSE(d.at(1).min_temp);

    CHECK(qidi::parse_controller_status(json{{"box", 1}}).empty());
}

TEST_CASE("QIDI drying state parse reads each box's end time", "[qidi][status_parse]") {
    CHECK_FALSE(qidi::parse_box_drying_state(json{{"other", 1}}));
    auto d = qidi::parse_box_drying_state(json{{"box_drying_state",
                                                {{"box2", {{"end_time", 2800}}},
                                                 {"box1", {{"dry_state", 0}}},
                                                 {"box9", {{"end_time", 5}}},
                                                 {"box3", 7}}}});
    REQUIRE(d);
    CHECK(*d == std::vector<std::pair<int, std::int64_t>>{{0, 0}, {1, 2800}});
}

TEST_CASE("QIDI config parse reads the dryer ceiling, controller dialect and force_move",
          "[qidi][status_parse]") {
    auto c = qidi::parse_config_settings(
        json{{"box_config box0", {{"target_max_temp_heater_generic", 90.0}}},
             {"multi_color_controller box0", json::object()},
             {"force_move", {{"enable_force_move", true}}}});
    CHECK(c.dryer_max_temp == 90.0f);
    CHECK(c.has_multi_color);
    CHECK(c.force_move);

    auto plain =
        qidi::parse_config_settings(json{{"heater_generic heater_box1", {{"max_temp", 80.0}}},
                                         {"multi_color_controller_x", json::object()},
                                         {"force_move", {{"enable_force_move", "True"}}}});
    CHECK(plain.dryer_max_temp == 80.0f);
    CHECK_FALSE(plain.has_multi_color);
    CHECK_FALSE(plain.force_move);
}

TEST_CASE("QIDI heater parse takes box temperature from the heater and ambient sensor only",
          "[qidi][status_parse]") {
    auto r = qidi::parse_heater_status(
        json{{"temperature_sensor heater_temp_a_box1", {{"temperature", 99.0}, {"humidity", 22.0}}},
             {"heater_generic heater_box1", {{"temperature", 55.0}, {"target", 60.0}}},
             {"aht20_f heater_box2", {{"temperature", 23.0}, {"humidity", 38.5}}},
             {"heater_generic heater_box2", {{"temperature", 21.0}}},
             {"temperature_sensor Chamber_Thermal_Protection_Sensor", {{"temperature", 60.0}}}});
    CHECK(r[0].temp == 55.0f);
    CHECK(r[0].target == 60.0f);
    CHECK(r[0].humidity == 22.0f);
    CHECK(r[1].temp == 23.0f);
    CHECK(r[1].humidity == 38.5f);
    CHECK_FALSE(r[1].target);
    CHECK_FALSE(r[2].temp);
    CHECK_FALSE(r[3].humidity);
}

TEST_CASE("QIDI filas list parse reads fila sections, colordict and vendor_list",
          "[qidi][status_parse]") {
    const auto list = qidi::parse_filas_list(R"INI(
# leading comment
  [fila5]
filament    = PLA Rapido
type=PLA
    min_temp   =   195
   max_temp=215   ; inline tail
box_max_temp = 45
[fila0]
min_temp = 1
[printer]
min_temp = 2
[colordict]
1 = #FAFAFA
2 = 060606
3 =
x = #112233
[vendor_list]
0 = Generic
2 = eSUN # comment
)INI");
    REQUIRE(list.profiles.size() == 1);
    const auto& p = list.profiles.at(5);
    CHECK(p.name == "PLA Rapido");
    CHECK(p.type == "PLA");
    CHECK(p.nozzle_min == 195);
    CHECK(p.nozzle_max == 215);
    CHECK(p.box_min == 0);
    CHECK(p.box_max == 45);
    CHECK(list.colors == std::map<int, std::uint32_t>{{1, 0xFAFAFA}, {2, 0x060606}});
    CHECK(list.vendors == std::map<int, std::string>{{0, "Generic"}, {2, "eSUN"}});
}
