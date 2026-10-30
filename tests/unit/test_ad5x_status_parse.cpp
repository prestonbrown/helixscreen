// SPDX-License-Identifier: GPL-3.0-or-later

#include "ad5x_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

// GET_ZCOLOR SILENT=1: zmod emits one "// "-prefixed line per LOADED slot plus
// a summary line. Hex is the RIGHTMOST /-segment (a named colour adds one);
// a missing slot number means physically empty; old zmod omits /HEX; very old
// zmod answers with an action:prompt dialog.

TEST_CASE("AD5X GET_ZCOLOR parse reads the summary and each slot line", "[ad5x][status_parse]") {
    auto r = ad5x::parse_zcolor_silent(
        {"// Extruder: None (1) | IFS: True", "// 1: PLA/FFFFFF", "// 2: PETG/2750E0"});
    CHECK_FALSE(r.is_prompt_fallback);
    CHECK_FALSE(r.is_old_format);
    CHECK(r.ifs_active);
    CHECK(r.saw_extruder_summary);
    CHECK(r.current_channel == 1);
    CHECK_FALSE(r.extruder_slot.has_value());
    REQUIRE(r.slots[0]);
    CHECK(r.slots[0]->material == "PLA");
    CHECK(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[1]);
    CHECK(r.slots[1]->material == "PETG");
    CHECK(r.slots[1]->hex == "2750E0");
    CHECK_FALSE(r.slots[2]);
    CHECK_FALSE(r.slots[3]);

    CHECK_FALSE(ad5x::parse_zcolor_silent({"// Extruder: None (0) | IFS: False"}).ifs_active);
}

TEST_CASE("AD5X GET_ZCOLOR parse takes hex from the last segment and the extruder slot 0-based",
          "[ad5x][status_parse]") {
    auto r = ad5x::parse_zcolor_silent(
        {"// Extruder: 1: PLA/FFFFFF | IFS: True", "// 3: PLA/transparent/00000000"});
    CHECK(r.extruder_slot == 0);
    REQUIRE(r.slots[2]);
    CHECK(r.slots[2]->material == "PLA");
    CHECK(r.slots[2]->hex == "00000000");
}

TEST_CASE("AD5X GET_ZCOLOR parse flags slot lines without hex as old format",
          "[ad5x][status_parse]") {
    auto r = ad5x::parse_zcolor_silent({"// 1: PLA", "// 2: PETG"});
    CHECK(r.is_old_format);
    REQUIRE(r.slots[1]);
    CHECK(r.slots[1]->material == "PETG");
    CHECK(r.slots[1]->hex.empty());

    CHECK_FALSE(ad5x::parse_zcolor_silent({"// 1: PLA", "// 2: PETG/00FF00"}).is_old_format);
}

TEST_CASE("AD5X GET_ZCOLOR parse counts only recognised lines as a response",
          "[ad5x][status_parse]") {
    auto r = ad5x::parse_zcolor_silent(
        {"// random gcode echo", "// 99: nonsense", "// 2: PETG/00FF00", "echo: hotend temp 205"});
    CHECK(r.saw_valid_response);
    CHECK_FALSE(r.slots[0]);
    REQUIRE(r.slots[1]);
    CHECK(r.slots[1]->hex == "00FF00");

    CHECK_FALSE(ad5x::parse_zcolor_silent({"echo: random", "// 99: nonsense"}).saw_valid_response);
    CHECK(ad5x::parse_zcolor_silent({"// Extruder: None (0) | IFS: True"}).saw_valid_response);
}

TEST_CASE("AD5X GET_ZCOLOR parse keeps the IFS_STATUS line from a prompt-dialog answer",
          "[ad5x][status_parse]") {
    auto r = ad5x::parse_zcolor_silent(
        {"// action:prompt_begin Select filament",
         R"(// {"State": 4, "Ports": [false, true, true, true], "Chan": 4, "Stall": false})",
         "// action:prompt_show"});
    CHECK(r.is_prompt_fallback);
    CHECK(r.saw_valid_response);
    CHECK_FALSE(r.saw_silent_content);
    CHECK(r.ifs_chan == 4);
    CHECK(r.ifs_state == 4);
    REQUIRE(r.ifs_ports);
    CHECK(*r.ifs_ports == std::array<bool, 4>{false, true, true, true});
}

TEST_CASE("AD5X IFS_STATUS object reads Chan and Ports independently, Ports all or nothing",
          "[ad5x][status_parse]") {
    ad5x::ZColorSilentResult ports_only;
    ad5x::read_ifs_status_object(json{{"Ports", {true, false, false, true}}}, ports_only);
    CHECK_FALSE(ports_only.ifs_chan);
    CHECK(ports_only.ifs_ports);
    CHECK(ports_only.saw_valid_response);

    ad5x::ZColorSilentResult bad_ports;
    ad5x::read_ifs_status_object(json{{"Chan", 2}, {"Ports", {true, 1, false, true}}}, bad_ports);
    CHECK(bad_ports.ifs_chan == 2);
    CHECK_FALSE(bad_ports.ifs_ports);

    ad5x::ZColorSilentResult short_ports;
    ad5x::read_ifs_status_object(json{{"Ports", {true, false}}}, short_ports);
    CHECK_FALSE(short_ports.ifs_ports);
    CHECK_FALSE(short_ports.saw_valid_response);
}

TEST_CASE("AD5X ifs_materials object files string-keyed slots and blanks the ? sentinel",
          "[ad5x][status_parse]") {
    ad5x::ZColorSilentResult r;
    CHECK(ad5x::read_ifs_materials_object(
        json{{"slots",
              {{"2", {{"type", "?"}, {"color", "#F80"}}}, {"9", {{"type", "PLA"}}}, {"x", 1}}}},
        r));
    CHECK(r.saw_silent_content);
    REQUIRE(r.slots[1]);
    CHECK(r.slots[1]->material.empty());
    CHECK(r.slots[1]->hex == "FF8800");
    CHECK_FALSE(r.slots[0]);

    ad5x::ZColorSilentResult none;
    CHECK_FALSE(ad5x::read_ifs_materials_object(json{{"slots", {{"5", json::object()}}}}, none));
    CHECK_FALSE(none.saw_valid_response);
}

TEST_CASE("AD5X module colour normalisation", "[ad5x][status_parse]") {
    CHECK(ad5x::normalize_module_color_hex("#A03CF7") == "A03CF7");
    CHECK(ad5x::normalize_module_color_hex("#F80") == "FF8800");
    CHECK(ad5x::normalize_module_color_hex("ff8800") == "FF8800");
    // Not-a-colour reads as "no reading", never as black.
    CHECK(ad5x::normalize_module_color_hex("").empty());
    CHECK(ad5x::normalize_module_color_hex("#12345").empty());
    CHECK(ad5x::normalize_module_color_hex("#GGGGGG").empty());
}

TEST_CASE("AD5X ifs module object discards loaded_channels with a non-integer entry",
          "[ad5x][status_parse]") {
    auto obj = ad5x::parse_ifs_module_object(
        json{{"loaded_channels", {1, 4, 7}}, {"active_channel", 4}, {"activity", "loading"}});
    REQUIRE(obj.loaded_ports);
    CHECK(*obj.loaded_ports == std::array<bool, 4>{true, false, false, true});
    CHECK(obj.active_channel == 4);
    CHECK(obj.activity == "loading");
    CHECK_FALSE(obj.error);

    CHECK_FALSE(ad5x::parse_ifs_module_object(json{{"loaded_channels", {1, "2"}}}).loaded_ports);
}

TEST_CASE("AD5X sensor frame prefers the namespace that carries a reading",
          "[ad5x][status_parse]") {
    auto f = ad5x::parse_sensor_frame(
        json{{"filament_motion_sensor ifs_motion_sensor", json::object()},
             {"zmod_ifs_motion_sensor ifs_motion_sensor", {{"filament_detected", true}}},
             {"filament_switch_sensor toolhead", {{"filament_detected", false}}},
             {"filament_switch_sensor _ifs_port_sensor_3", {{"filament_detected", true}}},
             {"extruder", {{"temperature", 212.5}}}});
    CHECK(f.motion_detected == true);
    CHECK(f.head_detected == false);
    CHECK(f.port_detected[2] == true);
    CHECK_FALSE(f.port_detected[0]);
    CHECK(f.has_extruder);
    CHECK(f.extruder_temperature == 212.5);
    CHECK_FALSE(f.extruder_target);

    auto empty = ad5x::parse_sensor_frame(json::object());
    CHECK_FALSE(empty.motion_detected);
    CHECK_FALSE(empty.has_extruder);
}

TEST_CASE("AD5X save_variables parse detects the plugin and reads its rows",
          "[ad5x][status_parse]") {
    auto v = ad5x::parse_save_variables(
        json{{"less_waste_tools", {2, 1, "x"}},
             {"less_waste_colors", {"FF0000", "00FF00", "0000FF", "FFFFFF"}},
             {"less_waste_current_tool", 1},
             {"less_waste_external", 1},
             {"ifs_loaded", 9},
             {"ifs_at_hub", 3}},
        "bambufy");
    CHECK(v.prefix == "less_waste");
    CHECK(v.tools[0] == 2);
    CHECK(v.tools[1] == 1);
    CHECK_FALSE(v.tools[2]);
    REQUIRE(v.short_tool_list);
    CHECK(v.short_tool_list->first == "less_waste_colors");
    CHECK(v.short_tool_list->second == 4);
    CHECK(v.current_tool == 1);
    CHECK(v.external == true);
    CHECK(v.module_loaded == 0);
    CHECK(v.module_at_hub == 3);
    CHECK_FALSE(v.custom_types);
    CHECK_FALSE(v.tools_conflict);
}

TEST_CASE("AD5X save_variables parse uses the known prefix and flags disagreeing tool tables",
          "[ad5x][status_parse]") {
    auto v = ad5x::parse_save_variables(json{{"bambufy_custom_types", {"PLA+", "", 3}},
                                             {"bambufy_current_tool", 2},
                                             {"less_waste_tools", {1, 2}},
                                             {"bambufy_colors", {"FF0000"}}},
                                        "less_waste");
    CHECK(v.prefix == "bambufy");
    REQUIRE(v.custom_types);
    CHECK(*v.custom_types == std::vector<std::string>{"PLA+"});
    CHECK(v.current_tool == 2);
    CHECK_FALSE(v.short_tool_list); // bambufy's 4-entry lists are its own shape

    auto known = ad5x::parse_save_variables(json{{"bambufy_current_tool", 3}}, "bambufy");
    CHECK_FALSE(known.prefix);
    CHECK(known.current_tool == 3);
    CHECK_FALSE(known.module_loaded);

    CHECK(ad5x::parse_save_variables(json{{"bambufy_tools", {4, 2}}, {"less_waste_tools", {2, 1}}},
                                     "")
              .tools_conflict);
}

TEST_CASE("AD5X _IFS_VARS parse accepts bool and int, and an empty dict is no macro",
          "[ad5x][status_parse]") {
    CHECK_FALSE(ad5x::parse_ifs_vars_macro(json::object()));
    auto m = ad5x::parse_ifs_vars_macro(
        json{{"variable_backup", 0}, {"variable_ifs_unlock_after_boot", true}});
    REQUIRE(m);
    CHECK(m->backup == false);
    CHECK(m->unlock_after_boot == true);
    CHECK_FALSE(ad5x::parse_ifs_vars_macro(json{{"variable_backup", "on"}})->backup);
}

TEST_CASE("AD5X filament.json parse resolves missing fields against default, then 1000/1200",
          "[ad5x][status_parse]") {
    auto t = ad5x::parse_filament_json(R"({
        "default": {"filament_tube_length": 900, "filament_ifs_speed": 1100},
        "PETG":    {"filament_tube_length": 650, "filament_ifs_speed": "1000"},
        "PLA":     {"filament_ifs_speed": 800}
    })");
    REQUIRE(t);
    CHECK(t->default_params == std::pair<int, int>{900, 1100});
    CHECK(t->materials.at("PETG") == std::pair<int, int>{650, 1000});
    CHECK(t->materials.at("PLA") == std::pair<int, int>{900, 800});
    CHECK(t->materials.count("default") == 0);

    auto bare = ad5x::parse_filament_json(R"({"ABS": {}})");
    REQUIRE(bare);
    CHECK(bare->default_params == std::pair<int, int>{1000, 1200});
    CHECK(bare->materials.at("ABS") == std::pair<int, int>{1000, 1200});

    CHECK_FALSE(ad5x::parse_filament_json("[1, 2]"));
    CHECK_FALSE(ad5x::parse_filament_json("{not json"));
}

TEST_CASE("AD5X Adventurer5M.json parse normalises each FFMInfo port", "[ad5x][status_parse]") {
    auto f = ad5x::parse_adventurer_json(
        R"({"FFMInfo": {"ffmColor1": "#a0b1c2", "ffmType1": "PLA",
                        "ffmColor2": "", "ffmType2": "?", "ffmType4": "PETG", "channel": 2}})");
    REQUIRE(f);
    CHECK(f->has_ffm);
    REQUIRE(f->ports[0]);
    CHECK(f->ports[0]->hex == "A0B1C2");
    CHECK(f->ports[0]->has_color);
    REQUIRE(f->ports[1]);
    CHECK(f->ports[1]->hex == "808080");
    CHECK_FALSE(f->ports[1]->has_color);
    CHECK(f->ports[1]->type.empty());
    CHECK_FALSE(f->ports[2]);
    REQUIRE(f->ports[3]);
    CHECK(f->ports[3]->type == "PETG");
    CHECK(f->channel == 2);

    auto no_ffm = ad5x::parse_adventurer_json(R"({"other": 1})");
    REQUIRE(no_ffm);
    CHECK_FALSE(no_ffm->has_ffm);
    CHECK_FALSE(ad5x::parse_adventurer_json("{oops"));
}

TEST_CASE("AD5X user.cfg parse collects filament_ names inside [zmod_ifs] only",
          "[ad5x][status_parse][issue_904]") {
    const std::string body = "# global header\n"
                             "[extruder]\n"
                             "filament_IGNORED: 999  ; not in zmod_ifs\n"
                             "[zmod_ifs]\r\n"
                             "filament_PLA+: 220   # inline comment\n"
                             "filament_RPLA: 215\r\n"
                             "other_setting: 42\n";
    CHECK(ad5x::parse_user_cfg_filament_types(body) == std::vector<std::string>{"PLA+", "RPLA"});
    CHECK(ad5x::parse_user_cfg_filament_types("").empty());
    CHECK(ad5x::parse_user_cfg_filament_types("[zmod_ifs]\nallowed_tool_count: 4\n").empty());
}
