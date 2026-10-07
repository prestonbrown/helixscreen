// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/happy_hare_fixture.h"
#include "happy_hare_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

TEST_CASE("Happy Hare status parse leaves every omitted field unset",
          "[happy_hare][status_parse]") {
    const auto d = happy_hare::parse_mmu_status(json::object());
    CHECK_FALSE(d.core.gate);
    CHECK_FALSE(d.core.tool);
    CHECK_FALSE(d.core.filament_loaded);
    CHECK_FALSE(d.core.reason_for_pause);
    CHECK_FALSE(d.core.action);
    CHECK_FALSE(d.core.filament_pos);
    CHECK_FALSE(d.core.bowden_progress);
    CHECK_FALSE(d.core.has_bypass);
    CHECK_FALSE(d.topology.num_units);
    CHECK_FALSE(d.topology.gate_counts);
    CHECK_FALSE(d.topology.ttg_map);
    CHECK_FALSE(d.identity.gate_status);
    CHECK_FALSE(d.identity.color_rgb);
    CHECK_FALSE(d.telemetry.espooler_active);
    CHECK_FALSE(d.telemetry.encoder);
    CHECK_FALSE(d.telemetry.number_of_toolchanges);
    CHECK_FALSE(d.sensors);
    CHECK_FALSE(d.drying);
    CHECK_FALSE(d.endless_spool_enabled);
}

TEST_CASE("Happy Hare status parse core fields", "[happy_hare][status_parse]") {
    const auto c = happy_hare::parse_core(json{{"gate", -2},
                                               {"tool", 3.5},
                                               {"filament", "Unloaded"},
                                               {"action", "Forming Tip"},
                                               {"filament_pos", 4},
                                               {"bowden_progress", 250},
                                               {"has_bypass", false}});
    CHECK(c.gate == -2);
    CHECK_FALSE(c.tool); // a float is not a tool number
    CHECK(c.filament_loaded == false);
    CHECK(c.action == "Forming Tip");
    CHECK(c.filament_pos == 4);
    CHECK(c.bowden_progress == 100);
    CHECK(c.has_bypass == false);

    CHECK(happy_hare::parse_core(json{{"filament", "Loaded"}}).filament_loaded == true);
    CHECK(happy_hare::parse_core(json{{"bowden_progress", -9}}).bowden_progress == -1);
    CHECK_FALSE(happy_hare::parse_core(json{{"has_bypass", "yes"}}).has_bypass);
}

TEST_CASE("Happy Hare status parse topology reads each gate-count spelling",
          "[happy_hare][status_parse]") {
    using happy_hare::parse_topology;

    CHECK(*parse_topology(json{{"num_gates", "6,4"}}).gate_counts == std::vector<int>{6, 4});
    CHECK(*parse_topology(json{{"num_gates", "6,x,0,4"}}).gate_counts == std::vector<int>{6, 4});
    CHECK(*parse_topology(json{{"num_gates", 8}}).gate_counts == std::vector<int>{8});
    CHECK(*parse_topology(json{{"num_gates", json::array({6, 0, 4})}}).gate_counts ==
          std::vector<int>{6, 4});
    CHECK_FALSE(parse_topology(json{{"num_gates", 0}}).gate_counts);
    CHECK_FALSE(parse_topology(json{{"num_gates", "x"}}).gate_counts);

    // unit_gate_counts wins over num_gates when it names any, and keeps a
    // non-positive entry that num_gates would have dropped.
    CHECK(*parse_topology(json{{"num_gates", "6,4"}, {"unit_gate_counts", json::array({2, 0})}})
               .gate_counts == std::vector<int>{2, 0});
    CHECK(*parse_topology(json{{"num_gates", "6,4"}, {"unit_gate_counts", json::array()}})
               .gate_counts == std::vector<int>{6, 4});

    CHECK(parse_topology(json{{"num_units", 0}}).num_units == 1);
    CHECK(parse_topology(json{{"num_units", 3}, {"unit", 2}}).active_unit == 2);

    // An empty ttg_map is a map, not an absent one.
    const auto empty = parse_topology(json{{"ttg_map", json::array()}});
    REQUIRE(empty.ttg_map);
    CHECK(empty.ttg_map->empty());
    CHECK(*parse_topology(json{{"ttg_map", json::array({1, "x", 0})}}).ttg_map ==
          std::vector<int>{1, 0});
}

TEST_CASE("Happy Hare status parse gate arrays keep the frame's length and skip bad entries",
          "[happy_hare][status_parse]") {
    const auto id = happy_hare::parse_gate_identity(json{
        {"gate_status", json::array({1, "x", 2.5, -1})},
        {"gate_color_rgb",
         json::array({0xFF0000, json::array({1.0, 0.5, 0.0}), json::array({1.0, 0.5}), "red"})},
        {"gate_color", json::array({"ff0000", "", "zzz", 5})},
        {"gate_material", json::array({"PLA", 3})},
        {"gate_temperature", json::array({210, 215.7, "hot"})},
        {"gate_spool_id", json::array({12, 0})}});

    REQUIRE(id.gate_status);
    CHECK(id.gate_status->size() == 4);
    CHECK((*id.gate_status)[0] == 1);
    CHECK_FALSE((*id.gate_status)[1]);
    CHECK_FALSE((*id.gate_status)[2]); // a float is not a status
    CHECK((*id.gate_status)[3] == -1);

    REQUIRE(id.color_rgb);
    CHECK((*id.color_rgb)[0] == 0xFF0000u);
    CHECK((*id.color_rgb)[1] == 0xFF8000u); // 0.5 * 255 + 0.5 rounds to 128
    CHECK_FALSE((*id.color_rgb)[2]);
    CHECK_FALSE((*id.color_rgb)[3]);

    REQUIRE(id.color);
    CHECK((*id.color)[0]->kind == ams::ColorReadingKind::Observed);
    CHECK((*id.color)[0]->rgb == 0xFF0000u);
    CHECK((*id.color)[1]->kind == ams::ColorReadingKind::Cleared);
    CHECK((*id.color)[2]->kind == ams::ColorReadingKind::NoReading);
    CHECK_FALSE((*id.color)[3]);

    CHECK((*id.material)[0] == "PLA");
    CHECK_FALSE((*id.material)[1]);
    CHECK((*id.temperature)[1] == 215);
    CHECK_FALSE((*id.temperature)[2]);
    CHECK((*id.spool_id)[1] == 0);
    CHECK_FALSE(id.name);
}

TEST_CASE("Happy Hare status parse telemetry reads nested objects field by field",
          "[happy_hare][status_parse]") {
    const auto t = happy_hare::parse_telemetry(
        json{{"espooler_active", "assist"},
             {"sync_drive", true},
             {"clog_detection_enabled", 2},
             {"encoder", json{{"flow_rate", 97}, {"headroom", 12.5}}},
             {"flowguard", json{{"enabled", true}, {"encoder_mode", 1}}},
             {"leds", json{{"unit0", json{{"exit_effect", "gate_status"}}}}},
             {"num_toolchanges", 3},
             {"slicer_tool_map", json{{"total_toolchanges", nullptr}}},
             {"spoolman_support", "pull"},
             {"pending_spool_id", 7}});
    CHECK(t.espooler_active == "assist");
    CHECK(t.sync_drive == true);
    CHECK(t.clog_detection_enabled == 2);
    REQUIRE(t.encoder);
    CHECK(t.encoder->flow_rate == 97);
    CHECK(t.encoder->headroom == Catch::Approx(12.5f));
    CHECK_FALSE(t.encoder->min_headroom);
    REQUIRE(t.flowguard);
    CHECK(t.flowguard->enabled == true);
    CHECK_FALSE(t.flowguard->active);
    CHECK(t.flowguard->encoder_mode == 1);
    CHECK(t.led_exit_effect == "gate_status");
    CHECK(t.current_toolchange == 2);
    // The slicer object without a total says there is none.
    CHECK(t.number_of_toolchanges == 0);
    CHECK(t.spoolman_mode == SpoolmanMode::PULL);
    CHECK(t.pending_spool_id == 7);

    CHECK(happy_hare::parse_telemetry(json{{"num_toolchanges", 0}}).current_toolchange == -1);
    CHECK_FALSE(happy_hare::parse_telemetry(json{{"slicer_tool_map", 3}}).number_of_toolchanges);
}

TEST_CASE("Happy Hare status parse sensors, drying and the endless-spool bit",
          "[happy_hare][status_parse]") {
    const auto d =
        happy_hare::parse_mmu_status(json{{"sensors", json{{"mmu_pre_gate_0", true},
                                                           {"mmu_pre_gate_2", nullptr},
                                                           {"mmu_pre_gate_x", true},
                                                           {"mmu_pre_gate_-1", true},
                                                           {"mmu_gear", true}}},
                                          {"drying_state", json::array({"active", 4, "queued"})},
                                          {"endless_spool", 1}});
    REQUIRE(d.sensors);
    REQUIRE(d.sensors->pre_gate.size() == 2);
    CHECK(d.sensors->pre_gate[0] == std::pair<int, bool>{0, true});
    CHECK(d.sensors->pre_gate[1] == std::pair<int, bool>{2, false});
    CHECK_FALSE(d.sensors->aggregate_pre_gate);

    REQUIRE(d.drying);
    CHECK_FALSE(d.drying->object);
    CHECK(*d.drying->per_gate == std::vector<std::string>{"active", "", "queued"});
    CHECK(d.endless_spool_enabled == true);

    const auto emu =
        happy_hare::parse_mmu_status(json{{"sensors", json{{"mmu_pre_gate", true}}},
                                          {"drying_state", json{{"active", true}, {"fan_pct", 30}}},
                                          {"endless_spool_enabled", nullptr},
                                          {"endless_spool", false}});
    REQUIRE(emu.sensors);
    CHECK(emu.sensors->aggregate_pre_gate == true);
    REQUIRE(emu.drying);
    REQUIRE(emu.drying->object);
    CHECK(emu.drying->object->active == true);
    CHECK(emu.drying->object->fan_pct == 30);
    CHECK_FALSE(emu.drying->object->current_temp_c);
    // The newer key is null, so the older spelling answers.
    CHECK(emu.endless_spool_enabled == false);
}

// ============================================================================
// Happy Hare v4 layout
// ============================================================================

TEST_CASE("Happy Hare layout: a v4 install is recognised from mmu_machine",
          "[happy_hare][status_parse][hh_v4]") {
    const json fx = test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    const auto layout =
        happy_hare::read_machine_layout(fx["configfile_settings"], fx["mmu_machine"]);
    CHECK(layout.version == "4.0.0");
    CHECK(layout.v4);
    CHECK(layout.unit_params_section == "mmu_unit_parameters unit0");
    CHECK(layout.toolhead_section == "mmu_toolhead default");

    SECTION("configfile alone still names the version and the first unit") {
        const auto from_config =
            happy_hare::read_machine_layout(fx["configfile_settings"], json::object());
        CHECK(from_config.v4);
        CHECK(from_config.unit_params_section == "mmu_unit_parameters unit0");
    }
    SECTION("unit and toolhead names are lowercased the way configfile keys are") {
        json settings = fx["configfile_settings"];
        settings["mmu_unit unit0"]["toolhead"] = "Main";
        json live = fx["mmu_machine"];
        live["unit_0"]["name"] = "Unit0";
        const auto mixed = happy_hare::read_machine_layout(settings, live);
        CHECK(mixed.unit_params_section == "mmu_unit_parameters unit0");
        CHECK(mixed.toolhead_section == "mmu_toolhead main");
    }
}

TEST_CASE("Happy Hare layout: v3 publishes per-unit fields but no version",
          "[happy_hare][status_parse][hh_v4]") {
    // v3.4 mmu_machine.get_status(): unit_N objects and num_units, no version.
    const json live = {{"unit_0", {{"name", "ERCF"}, {"selector_type", "LinearSelector"}}},
                       {"num_units", 1}};
    const json settings = {{"mmu", {{"happy_hare_version", 3.42}}}};
    const auto layout = happy_hare::read_machine_layout(settings, live);
    CHECK_FALSE(layout.v4);
    CHECK(layout.version == "3.42"); // v3's own [mmu] happy_hare_version
    CHECK(layout.unit_params_section.empty());
}

TEST_CASE("Happy Hare layout: each tunable is read from its v4 section",
          "[happy_hare][status_parse][hh_v4]") {
    const json fx = test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    const json& settings = fx["configfile_settings"];
    const auto layout = happy_hare::read_machine_layout(settings, fx["mmu_machine"]);
    const auto number = [&](const char* key) {
        return happy_hare::read_config_number(happy_hare::find_config_param(settings, layout, key));
    };

    const json* macro = happy_hare::find_config_param(settings, layout, "form_tip_macro");
    REQUIRE(macro);
    CHECK(*macro == "_MMU_CUT_TIP_NOSKEW");
    CHECK(number("extruder_load_speed") == 12.0f);
    CHECK(number("extruder_unload_speed") == 12.0f);
    CHECK(number("gear_from_spool_speed") == 80.0f);
    CHECK(number("gear_from_buffer_speed") == 150.0f);
    CHECK(number("gear_unload_speed") == 120.0f);
    CHECK(number("sync_to_extruder") == 1.0f);
    CHECK(number("heater_max_temp") == 65.0f);
    CHECK(number("toolhead_extruder_to_nozzle") == 87.0f);
    CHECK(number("toolhead_sensor_to_nozzle") == 1.0f);
    CHECK(number("toolhead_entry_to_extruder") == 6.0f);
    CHECK(number("toolhead_ooze_reduction") == 0.0f);
    // A VirtualSelector has no selector speed. The clog mode is the encoder mode.
    CHECK_FALSE(number("selector_move_speed"));
    CHECK(number("clog_detection") == 2.0f);
}

TEST_CASE("Happy Hare layout: v3 reads every tunable from [mmu]",
          "[happy_hare][status_parse][hh_v4]") {
    const json settings = {{"mmu",
                            {{"form_tip_macro", "_MMU_FORM_TIP"},
                             {"gear_from_spool_speed", "60"},
                             {"toolhead_ooze_reduction", 2.5},
                             {"enable_clog_detection", 2}}},
                           {"mmu_parameters", {{"gear_from_spool_speed", 99}}}};
    const auto layout = happy_hare::read_machine_layout(settings, json::object());
    REQUIRE_FALSE(layout.v4);
    CHECK(*happy_hare::find_config_param(settings, layout, "form_tip_macro") == "_MMU_FORM_TIP");
    CHECK(happy_hare::read_config_number(
              happy_hare::find_config_param(settings, layout, "gear_from_spool_speed")) == 60.0f);
    CHECK(happy_hare::read_config_number(
              happy_hare::find_config_param(settings, layout, "toolhead_ooze_reduction")) == 2.5f);
    CHECK(happy_hare::read_config_number(
              happy_hare::find_config_param(settings, layout, "clog_detection")) == 2.0f);
}

TEST_CASE("Happy Hare layout: each version's parameter names",
          "[happy_hare][status_parse][hh_v4]") {
    using happy_hare::param_name;
    auto layout = [](double version) {
        happy_hare::MachineLayout l;
        l.version_number = version;
        l.v4 = version >= 4;
        return l;
    };

    SECTION("v2.7.3 to v3.4.1: encoder clog detection, no gear unload speed before 3.10") {
        for (const double v : {2.73, 3.01, 3.40}) {
            CAPTURE(v);
            CHECK(param_name("clog_detection", layout(v)) == "enable_clog_detection");
            CHECK(param_name("detection_length", layout(v)) == "mmu_calibration_clog_length");
            CHECK(param_name("gear_from_spool_speed", layout(v)) == "gear_from_spool_speed");
        }
        CHECK(param_name("gear_unload_speed", layout(3.01)).empty());
        CHECK(param_name("gear_unload_speed", layout(3.10)) == "gear_unload_speed");
    }
    SECTION("v3.4.2: FlowGuard encoder mode") {
        CHECK(param_name("clog_detection", layout(3.42)) == "flowguard_encoder_mode");
        CHECK(param_name("detection_length", layout(3.42)) == "flowguard_encoder_max_motion");
        CHECK(param_name("gear_from_buffer_speed", layout(3.42)) == "gear_from_buffer_speed");
    }
    SECTION("v4") {
        CHECK(param_name("gear_from_spool_speed", layout(4.0)) == "gear_load_speed");
        CHECK(param_name("gear_from_buffer_speed", layout(4.0)) ==
              "gear_from_filament_buffer_speed");
        CHECK(param_name("gear_unload_speed", layout(4.0)) == "gear_unload_speed");
        CHECK(param_name("clog_detection", layout(4.0)) == "flowguard_encoder_mode");
        CHECK(param_name("detection_length", layout(4.0)) == "flowguard_encoder_max_motion");
    }
    SECTION("an unknown version is refused nothing") {
        CHECK(param_name("gear_unload_speed", layout(0)) == "gear_unload_speed");
        CHECK(param_name("clog_detection", layout(0)) == "enable_clog_detection");
    }
}

TEST_CASE("Happy Hare layout: the version number comes from either source",
          "[happy_hare][status_parse][hh_v4]") {
    CHECK(happy_hare::read_machine_layout({{"mmu", {{"happy_hare_version", 3.42}}}}, json::object())
              .version_number == 3.42);
    CHECK(happy_hare::read_machine_layout(json::object(), {{"happy_hare_version", "4.0.0"}})
              .version_number == 4.0);
}

TEST_CASE("Happy Hare entry sensor objects are read from sibling keys",
          "[happy_hare][status_parse][hh_v4]") {
    const json params = {
        {"mmu", {{"gate", 1}}},
        {"filament_switch_sensor mmu_entry_6", {{"filament_detected", true}, {"enabled", true}}},
        {"filament_switch_sensor mmu_entry_7", {{"enabled", false}}},
        {"filament_switch_sensor mmu_entry_8", {{"filament_detected", nullptr}}},
        {"filament_switch_sensor mmu_entry_x", {{"filament_detected", true}}},
        {"filament_switch_sensor mmu_entry_9", nullptr},
        {"filament_switch_sensor runout", {{"filament_detected", true}}}};
    const auto r = happy_hare::parse_entry_sensor_objects(params);
    REQUIRE(r.size() == 2);
    CHECK(r[0].gate == 6);
    CHECK(r[0].detected == true);
    CHECK(r[0].enabled == true);
    CHECK(r[1].gate == 7);
    CHECK_FALSE(r[1].detected);
    CHECK(r[1].enabled == false);
    CHECK(happy_hare::parse_entry_sensor_objects(json::array()).empty());
}

TEST_CASE("Happy Hare sensors dict reads both per-gate spellings",
          "[happy_hare][status_parse][hh_v4]") {
    const auto d = happy_hare::parse_mmu_status(
        json{{"sensors", {{"mmu_pre_gate_0", true}, {"mmu_entry_2", true}, {"mmu_entry", false}}}});
    REQUIRE(d.sensors);
    REQUIRE(d.sensors->pre_gate.size() == 2);
    CHECK_FALSE(d.sensors->aggregate_pre_gate);
}

TEST_CASE("Happy Hare v4 null telemetry reads as no data", "[happy_hare][status_parse][hh_v4]") {
    const json fx = test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    const auto d = happy_hare::parse_mmu_status(fx["mmu_status"]);
    CHECK_FALSE(d.telemetry.sync_feedback_state);
    CHECK_FALSE(d.telemetry.sync_feedback_bias);
    CHECK_FALSE(d.telemetry.sync_feedback_bias_raw);
    CHECK_FALSE(d.telemetry.sync_feedback_flow_rate);
    CHECK_FALSE(d.telemetry.encoder);
    CHECK_FALSE(d.telemetry.flowguard);
    CHECK(d.telemetry.clog_detection_enabled == std::nullopt); // false is not an integer
    CHECK(d.core.has_bypass == true);
    REQUIRE(d.telemetry.espooler);
    CHECK(d.telemetry.espooler->size() == 4);

    // Nulls inside the objects and lists v4 sends once a unit has them.
    const auto inner = happy_hare::parse_telemetry(json{
        {"encoder", {{"flow_rate", nullptr}, {"headroom", 12.5}, {"detection_length", nullptr}}},
        {"flowguard",
         {{"enabled", nullptr},
          {"active", true},
          {"trigger", nullptr},
          {"level", nullptr},
          {"encoder_mode", nullptr}}},
        {"espooler", json::array({"assist", nullptr, 3, "rewind"})},
        {"espooler_active", nullptr}});
    REQUIRE(inner.encoder);
    CHECK_FALSE(inner.encoder->flow_rate);
    CHECK(inner.encoder->headroom == 12.5f);
    CHECK_FALSE(inner.encoder->detection_length);
    REQUIRE(inner.flowguard);
    CHECK_FALSE(inner.flowguard->enabled);
    CHECK(inner.flowguard->active == true);
    CHECK_FALSE(inner.flowguard->trigger);
    CHECK_FALSE(inner.flowguard->level);
    CHECK_FALSE(inner.flowguard->encoder_mode);
    CHECK(*inner.espooler == std::vector<std::string>{"assist", "", "", "rewind"});
    CHECK_FALSE(inner.espooler_active);
    CHECK_FALSE(happy_hare::parse_telemetry(json{{"espooler", nullptr}}).espooler);
}

TEST_CASE("Happy Hare units: one entry per mmu_machine unit, configfile as a fallback",
          "[happy_hare][status_parse][hh_multi_unit]") {
    const json fx = test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    const auto units = happy_hare::read_machine_units(fx["configfile_settings"], fx["mmu_machine"]);
    REQUIRE(units.size() == 2);
    CHECK(units[0].selector_type == "LinearServoSelector");
    CHECK(units[0].first_gate == 0);
    CHECK(units[0].num_gates == 6);
    CHECK(units[1].display_name == "Box Turtle");
    CHECK(units[1].first_gate == 6);
    CHECK(units[1].filament_heaters.size() == 4);

    const json v3_settings = {
        {"mmu_machine", {{"selector_type", "VirtualSelector"}, {"filament_heaters", "a, b ,c"}}}};
    const auto v3 = happy_hare::read_machine_units(v3_settings, json::object());
    REQUIRE(v3.size() == 1);
    CHECK(v3[0].selector_type == "VirtualSelector");
    CHECK(v3[0].filament_heaters == std::vector<std::string>{"a", "b", "c"});
    CHECK(v3[0].first_gate == -1);
}

TEST_CASE("Happy Hare units: heaters collapse to one name only when every unit shares it",
          "[happy_hare][status_parse][hh_multi_unit]") {
    using happy_hare::collect_unit_objects;
    using happy_hare::MachineUnit;
    using happy_hare::UnitObjectKind;
    MachineUnit a;
    a.num_gates = 2;
    a.filament_heater = "h";
    MachineUnit b = a;
    b.num_gates = 3;

    auto same = collect_unit_objects({a, b}, UnitObjectKind::Heater);
    CHECK(same.shared == "h");
    CHECK(same.per_gate.empty());

    b.filament_heater = "g";
    auto differ = collect_unit_objects({a, b}, UnitObjectKind::Heater);
    CHECK(differ.shared.empty());
    CHECK(differ.per_gate == std::vector<std::string>{"h", "h", "g", "g", "g"});

    b.filament_heater.clear();
    b.environment_sensors = {"s0", "s1", "s2"};
    auto none = collect_unit_objects({a, b}, UnitObjectKind::EnvironmentSensor);
    CHECK(none.per_gate == std::vector<std::string>{"", "", "s0", "s1", "s2"});
    a.filament_heater.clear();
    CHECK(collect_unit_objects({a, b}, UnitObjectKind::Heater).per_gate.empty());
}
