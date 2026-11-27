// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 37 to 66 of test_ams_backend_happy_hare.cpp before it was split, in its order. Mostly
// [clog], [emu], [v4], [v4][edge], [topology], and more

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_accounting_cases.h"
#include "../test_helpers/happy_hare_test_fixtures.h"
#include "../test_helpers/mock_printer.h"
#include "../ui_test_utils.h"
#include "action_prompt_manager.h"
#include "ams_backend_happy_hare.h"
#include "ams_state.h"
#include "ams_types.h"
#include "buffer_reading.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_mapper.h"
#include "hh_defaults.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "spoolman_types.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/config_test_access.h"
#include "test_helpers/device_action_controls.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/happy_hare_fixture.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <any>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ams_action_from_string;
using helix::AmsAction;
using helix::AmsBackendHappyHareTestHelper;
using helix::AmsError;
using helix::AmsResult;
using helix::AmsType;
using helix::BufferMotion;
using helix::EncoderClogInfo;
using helix::path_segment_from_happy_hare_pos;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::spoolman_mode_from_string;
using helix::spoolman_mode_to_string;
using helix::SpoolmanMode;
using helix::SpoolMotorAction;
using helix::StepOperationType;

using helix::test::action_enabled;
using helix::test::connect_with_fixture;
using helix::test::fan_entry;
using helix::test::feed_params;
using helix::test::find_action;
using helix::test::funnel_cleared_copy;
using helix::test::golden_hh_full_frame;
using helix::test::happy_hare_fixture_at;
using helix::test::HappyHareGoldenFixture;
using helix::test::has_recovery;
using helix::test::hh_gates;
using helix::test::HhFourGates;
using helix::test::HhFourGatesOf;
using helix::test::HhFourGatesWithClient;
using helix::test::HhHelper;
using helix::test::HhHelperWithClient;
using helix::test::join_ints;
using helix::test::kHappyHareGolden;
using helix::test::kObjectsQuery;
using helix::test::PrintLifecycleFixture;
using helix::test::recover_req;
using helix::test::ScopedConfig;
using helix::test::TuningRig;
using helix::test::unit_scoped_commands;
using helix::test::v3_fixture;
using helix::test::v3_linear_fixture;
using helix::test::v4_control_rows;

TEST_CASE_METHOD(HhFourGates,
                 "Happy Hare translates espooler and sync feedback into capability fields",
                 "[ams][happy_hare][v4][buffer]") {
    struct Case {
        const char* espooler;
        SpoolMotorAction action;
        const char* sync;
        std::optional<BufferMotion> motion;
    };
    const Case cases[] = {
        {"rewind", SpoolMotorAction::Rewinding, "compressed", BufferMotion::Compressed},
        {"assist", SpoolMotorAction::Assisting, "tension", BufferMotion::Expanded},
        {"wiggle", SpoolMotorAction::Other, "neutral", BufferMotion::Unknown},
        {"", SpoolMotorAction::None, "disabled", std::nullopt},
    };
    for (const auto& c : cases) {
        CAPTURE(c.espooler, c.sync);
        helper.test_parse_mmu_state(
            {{"espooler_active", c.espooler}, {"sync_feedback_state", c.sync}});
        const auto info = helper.get_system_info();
        CHECK(info.spool_motor == c.action);
        CHECK(info.system_buffer_motion == c.motion);
    }
}

TEST_CASE_METHOD(HhHelper, "Happy Hare device sections, in order",
                 "[ams][happy_hare][device_actions]") {
    const auto sections = helper.get_device_sections();

    REQUIRE(sections.size() == 5);
    CHECK(sections[0].id == "setup");
    CHECK(sections[1].id == "speed");
    CHECK(sections[2].id == "toolhead");
    CHECK(sections[2].label == "Toolhead");
    CHECK(sections[3].id == "accessories");
    CHECK(sections[4].id == "maintenance");
}

TEST_CASE("execute_device_action sends each setting's command and keeps the value",
          "[ams][happy_hare][device_actions][v4][edge]") {
    // `sent` is a fragment of the command (an exact command when `exact`); ""
    // means the action is refused. `kept` is what the settings list shows after:
    // clog detection shows the configfile's mode until the status reports the
    // encoder's, so the value is never kept here.
    using Value = std::variant<std::monostate, double, bool, std::string>;
    struct Row {
        const char* id;
        std::any value;
        const char* sent;
        bool exact;
        Value kept;
    };
    const Row rows[] = {
        {"gear_from_buffer_speed", 200.0, "GEAR_FROM_BUFFER_SPEED=200", false, 200.0},
        {"gear_from_spool_speed", 80.0, "GEAR_FROM_SPOOL_SPEED=80", false, {}},
        {"gear_unload_speed", 90.0, "GEAR_UNLOAD_SPEED=90", false, {}},
        {"selector_speed", 200.0, "SELECTOR_MOVE_SPEED=200", false, {}},
        {"extruder_load_speed", 45.0, "EXTRUDER_LOAD_SPEED=45", false, {}},
        {"extruder_unload_speed", 50.0, "EXTRUDER_UNLOAD_SPEED=50", false, {}},
        {"toolhead_sensor_to_nozzle", 58.5, "TOOLHEAD_SENSOR_TO_NOZZLE=58.5", false, 58.5},
        {"toolhead_extruder_to_nozzle", 72.3, "TOOLHEAD_EXTRUDER_TO_NOZZLE=72.3", false, {}},
        {"toolhead_entry_to_extruder", 0.0, "TOOLHEAD_ENTRY_TO_EXTRUDER=0.0", false, {}},
        {"toolhead_ooze_reduction", 2.5, "TOOLHEAD_OOZE_REDUCTION=2.5", false, {}},
        {"sync_to_extruder", true, "SYNC_TO_EXTRUDER=1", false, true},
        {"sync_to_extruder", false, "SYNC_TO_EXTRUDER=0", false, {}},
        {"clog_detection", std::string("Auto"), "MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=2", true,
         std::string("Off")},
        {"clog_detection",
         std::string("Manual"),
         "MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=1",
         true,
         {}},
        {"clog_detection", std::string("Off"), "MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=0", true, {}},
        {"espooler_mode", std::string("rewind"), "MMU_ESPOOLER OPERATION=rewind", true, {}},
        {"espooler_mode", std::any(), "", false, {}}, // no operation named
        {"test_move", std::any(), "MMU_TEST_MOVE", true, {}},
        {"servo_up", std::any(), "MMU_SERVO POS=up", true, {}},
        {"servo_move", std::any(), "MMU_SERVO POS=move", true, {}},
        {"servo_down", std::any(), "MMU_SERVO POS=down", true, {}},
        {"gear_sync", true, "MMU_SYNC_GEAR_MOTOR SYNC=1", true, {}},
        {"gear_sync", false, "MMU_SYNC_GEAR_MOTOR SYNC=0", true, {}},
        {"servo_buzz", std::any(), "MMU_SERVO", true, {}}, // no BUZZ=1 argument
        {"calibrate_servo", std::any(), "", false, {}},    // not an action
    };
    for (const auto& row : rows) {
        INFO(row.id << (row.value.has_value() ? "" : " (no value)") << " -> " << row.sent);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.set_config_defaults_for_test(); // config-backed actions are offered once it loads
        helper.initialize_test_gates(4);
        helper.set_running(true);

        const AmsError err = helper.execute_device_action(row.id, row.value);
        if (!*row.sent) {
            CHECK_FALSE(err.success());
            continue;
        }
        CHECK(err.success());
        CHECK((row.exact ? helper.has_gcode(row.sent) : helper.has_gcode_containing(row.sent)));
        CHECK_FALSE(helper.has_gcode_containing("BUZZ=1"));
        if (std::holds_alternative<std::monostate>(row.kept))
            continue;
        const auto actions = helper.get_device_actions();
        const auto it = std::find_if(actions.begin(), actions.end(),
                                     [&](const auto& a) { return a.id == row.id; });
        REQUIRE(it != actions.end());
        if (const auto* d = std::get_if<double>(&row.kept))
            CHECK(std::any_cast<double>(it->current_value) == Catch::Approx(*d));
        else if (const auto* b = std::get_if<bool>(&row.kept))
            CHECK(std::any_cast<bool>(it->current_value) == *b);
        else
            CHECK(std::any_cast<std::string>(it->current_value) == std::get<std::string>(row.kept));
    }
}

TEST_CASE_METHOD(HhFourGates,
                 "Happy Hare without drying_state has no dryer and refuses start and stop",
                 "[ams][happy_hare][v4][edge]") {
    CHECK_FALSE(helper.get_dryer_info().supported);
    const AmsError start = helper.start_drying(55.0f, 240);
    CHECK(start.result == AmsResult::NOT_SUPPORTED);
    const AmsError stop = helper.stop_drying();
    CHECK(stop.result == AmsResult::NOT_SUPPORTED);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare reads an object drying_state, defaulting fields it omits",
          "[ams][happy_hare][v4][edge]") {
    struct Row {
        const char* name;
        nlohmann::json state;
        bool active;
        float current;
        float target;
        int remaining;
        int duration;
        int fan;
    };
    const Row rows[] = {
        {"every field",
         {{"active", true},
          {"current_temp", 52.3},
          {"target_temp", 55.0},
          {"remaining_min", 120},
          {"duration_min", 240},
          {"fan_pct", 50}},
         true,
         52.3f,
         55.0f,
         120,
         240,
         50},
        {"some fields", {{"active", false}, {"current_temp", 25.0}}, false, 25.0f, 0.0f, 0, -1, -1},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state({{"drying_state", row.state}});

        const auto dryer = helper.get_dryer_info();
        CHECK(dryer.supported);
        CHECK(dryer.active == row.active);
        CHECK(dryer.current_temp_c == Catch::Approx(row.current));
        CHECK(dryer.target_temp_c == Catch::Approx(row.target));
        CHECK(dryer.remaining_min == row.remaining);
        if (row.duration >= 0)
            CHECK(dryer.duration_min == row.duration);
        if (row.fan >= 0)
            CHECK(dryer.fan_pct == row.fan);
    }
}

TEST_CASE("Happy Hare drying sends MMU_HEATER with TIMER minutes, per unit on multi-unit",
          "[ams][happy_hare][v4][edge]") {
    // TIMER= is minutes; MMU_HEATER takes no fan setting, so none is ever sent.
    struct Row {
        const char* name;
        std::vector<int> units; // empty = one 4-gate unit
        int unit;
        float temp;
        int minutes;
        int fan;
        const char* start;
        const char* stop;
    };
    const Row rows[] = {
        {"one unit",
         {},
         0,
         55.0f,
         240,
         50,
         "MMU_HEATER DRY=1 TEMP=55 TIMER=240",
         "MMU_HEATER STOP=1"},
        {"no fan given",
         {},
         0,
         45.0f,
         120,
         -1,
         "MMU_HEATER DRY=1 TEMP=45 TIMER=120",
         "MMU_HEATER STOP=1"},
        {"unit 1 owns gates 4-7",
         {4, 4},
         1,
         50.0f,
         120,
         -1,
         "MMU_HEATER DRY=1 TEMP=50 TIMER=120 GATES=4,5,6,7",
         "MMU_HEATER STOP=1 GATES=4,5,6,7"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.test_set_clock([] { return std::time_t{1000}; });
        helper.test_parse_mmu_state({{"drying_state", {{"active", false}}}});
        if (row.units.empty())
            helper.initialize_test_gates(4);
        else
            helper.initialize_test_gates_multi(row.units);

        REQUIRE(helper.start_drying(row.temp, row.minutes, row.fan, row.unit).success());
        CHECK(helper.has_gcode(row.start));
        CHECK_FALSE(helper.has_gcode_containing("FAN="));
        if (row.units.empty())
            CHECK(helper.get_dryer_info().remaining_min == row.minutes);

        helper.clear_captured_gcodes();
        REQUIRE(helper.stop_drying(row.unit).success());
        CHECK(helper.has_gcode(row.stop));
    }
}

TEST_CASE_METHOD(HhHelper, "Happy Hare reads filament_heater + heater_max_temp from config",
                 "[ams][happy_hare][v4]") {
    nlohmann::json settings = {{"mmu_machine", {{"filament_heater", "heater_generic box1_heater"}}},
                               {"mmu", {{"heater_max_temp", 65.0}}}};
    helper.test_apply_heater_config(settings);
    auto d = helper.get_dryer_info();
    REQUIRE(d.supported);
    REQUIRE(d.max_temp_c == Catch::Approx(65.0f));
}

TEST_CASE("Happy Hare unit environment: heater temperature and the sensor's humidity",
          "[ams][happy_hare][v4]") {
    // The heater is named verbatim (no forced heater_generic prefix). An
    // environment_sensor alias ("temperature_sensor box") resolves to whichever
    // humidity chip backs it, including the QIDI Box's aht20_f (#1022). A sensor
    // alone still surfaces: no heater reading has to arrive first.
    using J = nlohmann::json;
    const J box_with_sensor = {{"mmu_machine",
                                {{"filament_heater", "heater_generic box1_heater"},
                                 {"environment_sensor", "temperature_sensor box"}}},
                               {"mmu", {{"heater_max_temp", 65.0}}}};
    const J drying_at_50 = {{"active", true}, {"current_temp", 50.0}};
    struct Row {
        const char* name;
        J settings;      // null = no config query
        J drying_state;  // sent as printer.mmu.drying_state
        J heater_status; // null = none
        J sensor_status; // null = none
        float temp;
        float humidity; // < 0 = no humidity shown
    };
    const Row rows[] = {
        {"a sensor and no heater", J{{"mmu_machine", {{"environment_sensor", "bme280 mmu_box"}}}},
         J::array({"", "", "", ""}), nullptr,
         J{{"bme280 mmu_box", {{"temperature", 24.5}, {"humidity", 38.0}}}}, 24.5f, 38.0f},
        {"a heater outside heater_generic",
         J{{"mmu_machine", {{"filament_heater", "my_heater chamber"}}}}, J::array({"", "", "", ""}),
         J{{"my_heater chamber", {{"temperature", 50.0}}}}, nullptr, 50.0f, -1},
        {"drying_state's own temperature", nullptr, J{{"active", true}, {"current_temp", 48.5}},
         nullptr, nullptr, 48.5f, -1},
        {"humidity from an htu21d chip", box_with_sensor, drying_at_50, nullptr,
         J{{"htu21d box", {{"temperature", 50.0}, {"humidity", 37.5}}}}, 50.0f, 37.5f},
        {"humidity from an aht20_f chip", box_with_sensor, drying_at_50, nullptr,
         J{{"aht20_f box", {{"temperature", 27.67}, {"humidity", 16.0}}}}, -1, 16.0f},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        REQUIRE_FALSE(helper.get_system_info().units[0].environment.has_value());

        if (!row.settings.is_null())
            helper.test_apply_heater_config(row.settings);
        helper.test_parse_mmu_state({{"drying_state", row.drying_state}});
        if (!row.heater_status.is_null())
            helper.test_apply_filament_heater_status(row.heater_status);
        if (!row.sensor_status.is_null())
            CHECK(helper.test_apply_environment_sensor_status(row.sensor_status));

        const auto info = helper.get_system_info();
        REQUIRE(info.units[0].environment.has_value());
        const auto& env = *info.units[0].environment;
        if (row.temp >= 0)
            CHECK(env.temperature_c == Catch::Approx(row.temp));
        CHECK(env.has_humidity == (row.humidity >= 0));
        if (row.humidity >= 0)
            CHECK(env.humidity_pct == Catch::Approx(row.humidity));
    }

    SECTION("a frame naming no humidity chip keeps the last reading") {
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_apply_heater_config(box_with_sensor);
        helper.test_parse_mmu_state({{"drying_state", drying_at_50}});
        REQUIRE(helper.test_apply_environment_sensor_status(
            {{"htu21d box", {{"temperature", 50.0}, {"humidity", 37.5}}}}));
        CHECK_FALSE(helper.test_apply_environment_sensor_status(
            {{"toolhead", {{"position", {0, 0, 0, 0}}}}}));
        CHECK(helper.get_system_info().units[0].environment->humidity_pct == Catch::Approx(37.5f));
    }
}

TEST_CASE_METHOD(HhHelper, "Happy Hare per-gate sensors map to the right unit (multi-MMU)",
                 "[ams][happy_hare][v4]") {
    // Two MMU units of 2 gates each (gates 0-1 = unit 0, gates 2-3 = unit 1).
    helper.initialize_test_units({2, 2});

    // Per-gate (EMU) config: one heater + one env sensor per gate. Comma-string
    // form (as Moonraker often returns getlist values) is parsed too.
    nlohmann::json settings = {
        {"mmu_machine",
         {{"filament_heaters", "heater_generic h0, heater_generic h1, "
                               "heater_generic h2, heater_generic h3"},
          {"environment_sensors",
           nlohmann::json::array({"temperature_sensor s0", "temperature_sensor s1",
                                  "temperature_sensor s2", "temperature_sensor s3"})}}},
        {"mmu", {{"heater_max_temp", 65.0}}}};
    helper.test_apply_heater_config(settings);

    // Live readings: unit 0's first gate (gate 0) heater h0 + sensor s0;
    // unit 1's first gate (gate 2) heater h2 + sensor s2. Distinct values.
    nlohmann::json status = {{"heater_generic h0", {{"temperature", 40.0}}},
                             {"heater_generic h2", {{"temperature", 55.0}}},
                             {"htu21d s0", {{"humidity", 20.0}}},
                             {"htu21d s2", {{"humidity", 80.0}}}};
    REQUIRE(helper.test_apply_filament_heater_status(status));
    REQUIRE(helper.test_apply_environment_sensor_status(status));

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);

    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(40.0f));
    REQUIRE(info.units[0].environment->humidity_pct == Catch::Approx(20.0f));

    REQUIRE(info.units[1].environment.has_value());
    REQUIRE(info.units[1].environment->temperature_c == Catch::Approx(55.0f));
    REQUIRE(info.units[1].environment->humidity_pct == Catch::Approx(80.0f));
}

// Backwards compatibility: v3 sends nothing new

TEST_CASE_METHOD(HhHelper, "Happy Hare v3 data with no v4 fields works normally",
                 "[ams][happy_hare][v4][compat]") {
    // Pure v3 data — only classic fields
    nlohmann::json mmu_data = {{"gate", 2},
                               {"tool", 2},
                               {"filament", "Loaded"},
                               {"action", "Idle"},
                               {"filament_pos", 8},
                               {"has_bypass", true},
                               {"gate_status", {1, 0, 2, 1}},
                               {"gate_color_rgb", {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00}},
                               {"gate_material", {"PLA", "PETG", "ABS", "TPU"}},
                               {"ttg_map", {0, 1, 2, 3}},
                               {"endless_spool_groups", {0, 0, 1, 1}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.type == AmsType::HAPPY_HARE);
    REQUIRE(info.current_slot == 2);
    REQUIRE(info.current_tool == 2);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.action == AmsAction::IDLE);
    REQUIRE(info.supports_bypass == true);
    REQUIRE(info.total_slots == 4);

    // All v4 fields should be at safe defaults
    REQUIRE(info.spoolman_mode == SpoolmanMode::OFF);
    REQUIRE(info.pending_spool_id == -1);
    REQUIRE(info.espooler_state.empty());
    REQUIRE(info.sync_feedback_state.empty());
    REQUIRE(info.sync_drive == false);
    REQUIRE(info.clog_detection == 0);
    REQUIRE(info.encoder_flow_rate == -1);
    REQUIRE(info.toolchange_purge_volume == 0.0f);

    // Bowden progress not available
    REQUIRE(helper.get_bowden_progress() == -1);

    // Dryer not available
    auto dryer = helper.get_dryer_info();
    REQUIRE_FALSE(dryer.supported);

    // Slot data should be properly parsed
    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_rgb == 0xFF0000);
    REQUIRE(slot0.material == "PLA");
    REQUIRE(slot0.status == SlotStatus::AVAILABLE);

    auto slot2 = helper.get_slot_info(2);
    REQUIRE(slot2.color_rgb == 0x0000FF);
    REQUIRE(slot2.material == "ABS");
    // gate_status=2 maps to FROM_BUFFER, but gate 2 is also the gate Happy Hare
    // reports loaded, and the loaded gate outranks its fill state (#1199). An
    // unloaded from_buffer gate still reads FROM_BUFFER (case below).
    REQUIRE(slot2.status == SlotStatus::LOADED);
}

// has_bypass: the single flag behind the whole bypass UI

// printer.mmu.has_bypass gates the sidebar toggle, the Device Operations bypass
// section and the bypass node on the filament path. Happy Hare defaults it to 0
// for mmu_vendor "Other" (which is what a Qidi Box under HH reports) and on
// type-A selectors ANDs it with the calibrated bypass offset, so it is both
// legitimately false on real hardware and able to turn true later.
TEST_CASE_METHOD(HelixTestFixture, "Happy Hare has_bypass drives supports_bypass",
                 "[ams][happy_hare][bypass]") {
    auto base_mmu = []() {
        return nlohmann::json{{"gate", 0},
                              {"tool", 0},
                              {"filament", "Loaded"},
                              {"action", "Idle"},
                              {"filament_pos", 8},
                              {"gate_status", {1, 0, 2, 1}},
                              {"gate_material", {"PLA", "PETG", "ABS", "TPU"}},
                              {"ttg_map", {0, 1, 2, 3}}};
    };

    SECTION("stays off until the firmware confirms, so the UI never flickers") {
        AmsBackendHappyHareTestHelper helper;
        REQUIRE(helper.get_system_info().supports_bypass == false);

        nlohmann::json mmu = base_mmu();
        mmu["has_bypass"] = false;
        helper.test_parse_mmu_state(mmu);

        REQUIRE(helper.get_system_info().supports_bypass == false);
    }

    SECTION("firmware true turns it on") {
        AmsBackendHappyHareTestHelper helper;

        nlohmann::json mmu = base_mmu();
        mmu["has_bypass"] = true;
        helper.test_parse_mmu_state(mmu);

        REQUIRE(helper.get_system_info().supports_bypass == true);
    }

    SECTION("absent field assumes supported rather than removing the control") {
        AmsBackendHappyHareTestHelper helper;
        helper.test_parse_mmu_state(base_mmu());

        REQUIRE(helper.get_system_info().supports_bypass == true);
    }

    SECTION("turns back on when the selector is calibrated mid-session") {
        AmsBackendHappyHareTestHelper helper;

        nlohmann::json mmu = base_mmu();
        mmu["has_bypass"] = false;
        helper.test_parse_mmu_state(mmu);
        REQUIRE(helper.get_system_info().supports_bypass == false);

        mmu["has_bypass"] = true;
        helper.test_parse_mmu_state(mmu);
        REQUIRE(helper.get_system_info().supports_bypass == true);
    }

    SECTION("a non-boolean value is ignored rather than coerced") {
        AmsBackendHappyHareTestHelper helper;

        nlohmann::json mmu = base_mmu();
        mmu["has_bypass"] = nullptr;
        helper.test_parse_mmu_state(mmu);

        REQUIRE(helper.get_system_info().supports_bypass == true);
    }
}

// v3+v4 mixed: some v4 fields with v3 base

TEST_CASE_METHOD(HhHelper, "Happy Hare mixed v3/v4 data parses both correctly",
                 "[ams][happy_hare][v4][compat]") {
    nlohmann::json mmu_data = {// v3 fields
                               {"gate", 0},
                               {"tool", 0},
                               {"filament", "Loaded"},
                               {"action", "Idle"},
                               {"filament_pos", 8},
                               {"gate_status", {2, 1, 0, 1}},
                               {"gate_material", {"PLA", "PETG", "", "ABS"}},
                               // v4 fields mixed in
                               {"bowden_progress", 100},
                               {"spoolman_support", "push"},
                               {"gate_name", {"Red Spool", "", "Empty", "Black"}},
                               {"gate_temperature", {210, 230, 0, 250}},
                               {"espooler_active", "assist"},
                               {"clog_detection_enabled", 2}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    // v3 data
    REQUIRE(info.current_slot == 0);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.total_slots == 4);

    // v4 additions
    REQUIRE(helper.get_bowden_progress() == 100);
    REQUIRE(info.spoolman_mode == SpoolmanMode::PUSH);
    REQUIRE(info.espooler_state == "assist");
    REQUIRE(info.clog_detection == 2);

    // Per-slot v4 data
    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_name == "Red Spool");
    REQUIRE(slot0.nozzle_temp_min == 210);
    REQUIRE(slot0.material == "PLA");

    auto slot3 = helper.get_slot_info(3);
    REQUIRE(slot3.color_name == "Black");
    REQUIRE(slot3.nozzle_temp_min == 250);
    REQUIRE(slot3.material == "ABS");
}

TEST_CASE("Happy Hare reports the manages_active_spool the shared case table claims",
          "[ams][happy_hare][spoolman][plugin_filament]") {
    const auto rows = helix::test::ownership_rows_for("Happy Hare");
    REQUIRE(rows.size() >= 3);
    for (const auto& row : rows) {
        INFO(row["name"].get<std::string>());
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state({{"spoolman_support", row["spoolman_mode"]}});
        CHECK(helper.manages_active_spool() == row["manages_active_spool"].get<bool>());
    }

    // Beyond the shared table: OFF before anything is reported, pull mode, and
    // a mode switched back off.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    CHECK_FALSE(helper.manages_active_spool());
    helper.test_parse_mmu_state({{"spoolman_support", "pull"}});
    CHECK(helper.manages_active_spool());
    helper.test_parse_mmu_state({{"spoolman_support", "off"}});
    CHECK_FALSE(helper.manages_active_spool());
}

// Live heater temp/target from Moonraker status

TEST_CASE_METHOD(HhFourGates, "Happy Hare parses live filament_heater temp/target",
                 "[ams][happy_hare][v4]") {
    helper.test_apply_heater_config(
        nlohmann::json{{"mmu_machine", {{"filament_heater", "heater_generic box1_heater"}}},
                       {"mmu", {{"heater_max_temp", 65.0}}}});
    nlohmann::json params = {
        {"heater_generic box1_heater", {{"temperature", 48.5}, {"target", 55.0}}}};
    helper.test_apply_filament_heater_status(params);
    auto d = helper.get_dryer_info();
    REQUIRE(d.current_temp_c == Catch::Approx(48.5f).epsilon(0.01));
    REQUIRE(d.target_temp_c == Catch::Approx(55.0f).epsilon(0.01));
}

TEST_CASE("Happy Hare with a heater reads drying_state as an array or an object",
          "[ams][happy_hare][emu]") {
    // A drying_state array ships on every Happy Hare install, so a heater has to be
    // configured for the dryer to be supported at all; "complete" is not drying.
    struct Row {
        const char* name;
        nlohmann::json state;
        bool active;
    };
    const Row rows[] = {
        {"all empty", {"", "", "", ""}, false},
        {"one gate complete", {"complete", "", "", ""}, false},
        {"one gate active", {"", "active", "", ""}, true},
        {"object format", {{"active", true}, {"current_temp", 55.0}}, true},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_apply_heater_config(
            {{"mmu_machine", {{"filament_heater", "heater_generic MMU_heater"}}}});
        helper.test_parse_mmu_state({{"drying_state", row.state}});
        CHECK(helper.get_dryer_info().supported);
        CHECK(helper.get_dryer_info().active == row.active);
    }
}

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "gate_color hex string fallback",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(3);
    // gate_color_rgb absent, fall back to gate_color hex strings
    nlohmann::json mmu_data = {{"gate_color", {"ffffff", "000000", "042f56"}}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();
    REQUIRE(info.units[0].slots[0].color_rgb == 0xFFFFFF);
    REQUIRE(info.units[0].slots[1].color_rgb == 0x000000);
    REQUIRE(info.units[0].slots[2].color_rgb == 0x042F56);
}

// EMU aggregate sensor format

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU aggregate sensor format",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(4);

    SECTION("aggregate sensors with active gate") {
        nlohmann::json mmu_data = {
            {"gate", 0},
            {"sensors",
             {{"mmu_pre_gate", true}, {"mmu_gear", true}, {"extruder", true}, {"toolhead", true}}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].has_slot_sensors == true);

        // All gates should report having pre-gate sensors
        auto gate0 = get_gate_sensor(0);
        REQUIRE(gate0 != nullptr);
        REQUIRE(gate0->has_pre_gate_sensor == true);
        REQUIRE(gate0->pre_gate_triggered == true);

        // Other gates have sensor hardware but we only know current gate's reading
        auto gate1 = get_gate_sensor(1);
        REQUIRE(gate1 != nullptr);
        REQUIRE(gate1->has_pre_gate_sensor == true);
    }

    SECTION("aggregate sensors with different active gate") {
        nlohmann::json mmu_data = {{"gate", 2},
                                   {"sensors", {{"mmu_pre_gate", false}, {"mmu_gear", true}}}};
        test_parse_mmu_state(mmu_data);

        auto gate2 = get_gate_sensor(2);
        REQUIRE(gate2 != nullptr);
        REQUIRE(gate2->has_pre_gate_sensor == true);
        REQUIRE(gate2->pre_gate_triggered == false);
    }
}

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper,
                 "Happy Hare units report the toolhead sensor the sensors map says is fitted",
                 "[ams][happy_hare][toolhead_sensor]") {
    initialize_test_gates(4);

    SECTION("fitted") {
        test_parse_mmu_state({{"sensors", {{"mmu_pre_gate_0", true}, {"toolhead", false}}}});
        CHECK(get_system_info().units[0].has_toolhead_sensor);
    }
    SECTION("absent") {
        test_parse_mmu_state({{"sensors", {{"mmu_pre_gate_0", true}}}});
        CHECK_FALSE(get_system_info().units[0].has_toolhead_sensor);
    }
    SECTION("disabled (null) on v4") {
        test_parse_mmu_state({{"sensors", {{"mmu_pre_gate_0", true}, {"toolhead", nullptr}}}});
        CHECK_FALSE(get_system_info().units[0].has_toolhead_sensor);
    }
}

// EMU gate_filament_name parsing — EMU sends filament names via
// gate_filament_name instead of gate_name

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU gate_filament_name parsing",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(3);

    SECTION("gate_name takes priority over gate_filament_name") {
        nlohmann::json mmu_data = {{"gate_name", {"Priority Name", "Other", "Third"}},
                                   {"gate_filament_name", {"Fallback", "Fallback", "Fallback"}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].slots[0].color_name == "Priority Name");
    }

    SECTION("gate_filament_name used when gate_name absent") {
        nlohmann::json mmu_data = {{"gate_filament_name", {"Name A", "Name B", "Name C"}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].slots[0].color_name == "Name A");
    }
}

// Full EMU integration test — validates all EMU parsing fixes together
// using real data from an EMU user's Moonraker dump

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "Full EMU status integration",
                 "[ams][happy_hare][emu]") {
    // Real data from EMU user's Moonraker dump (simplified)
    nlohmann::json mmu_data = {
        {"gate", 0},
        {"tool", 0},
        {"filament", "Loaded"},
        {"action", "Idle"},
        {"num_gates", 8},
        {"filament_pos", 10},
        {"has_bypass", true},
        {"gate_status", {2, 2, 2, 2, 2, 2, 1, 1}},
        {"gate_color_rgb",
         {{1.0, 1.0, 1.0},
          {0.0, 0.0, 0.0},
          {0.976, 0.976, 0.4},
          {0.016, 0.184, 0.337},
          {0.553, 0.784, 0.588},
          {0.0, 0.0, 0.0},
          {1.0, 1.0, 1.0},
          {0.0, 0.0, 0.0}}},
        {"gate_material", {"PLA", "PLA", "PLA", "PLA", "PLA", "ABS", "ABS", "ASA CF"}},
        {"gate_filament_name",
         {"Matte White", "Matte Black", "Matte Yellow", "Matte Navy", "Matte Green", "Black",
          "White", "Black"}},
        {"gate_temperature", {230, 230, 230, 230, 230, 260, 260, 265}},
        {"gate_name", nullptr},
        {"ttg_map", {0, 1, 2, 3, 4, 5, 6, 7}},
        {"endless_spool_groups", {0, 1, 2, 3, 4, 5, 6, 7}},
        {"bowden_progress", -1},
        {"encoder", nullptr},
        {"unit_gate_counts", nullptr},
        {"sync_drive", true},
        {"sync_feedback_state", "neutral"},
        {"clog_detection_enabled", 0},
        {"espooler_active", ""},
        {"spoolman_support", "push"},
        {"drying_state", {"", "", "", "", "", "", "", ""}},
        {"sensors",
         {{"mmu_pre_gate", true},
          {"mmu_gear", true},
          {"filament_proportional", false},
          {"extruder", true},
          {"toolhead", true}}}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();

    // Structure
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 8);

    // Current state
    REQUIRE(info.current_slot == 0);
    REQUIRE(info.current_tool == 0);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.action == AmsAction::IDLE);
    REQUIRE(info.supports_bypass == true);

    // Colors (float arrays converted to 0xRRGGBB, each channel rounded)
    REQUIRE(info.units[0].slots[0].color_rgb == 0xFFFFFF); // White
    REQUIRE(info.units[0].slots[1].color_rgb == 0x000000); // Black
    REQUIRE(info.units[0].slots[2].color_rgb == 0xF9F966); // 0.976 -> 249, 0.4 -> 102
    REQUIRE(info.units[0].slots[3].color_rgb == 0x042F56); // Navy

    // Materials
    REQUIRE(info.units[0].slots[0].material == "PLA");
    REQUIRE(info.units[0].slots[5].material == "ABS");
    REQUIRE(info.units[0].slots[7].material == "ASA CF");

    // Filament names (from gate_filament_name since gate_name is null)
    REQUIRE(info.units[0].slots[0].color_name == "Matte White");
    REQUIRE(info.units[0].slots[3].color_name == "Matte Navy");

    // Temperatures
    REQUIRE(info.units[0].slots[0].nozzle_temp_min == 230);
    REQUIRE(info.units[0].slots[5].nozzle_temp_min == 260);

    // Sensors (aggregate format)
    REQUIRE(info.units[0].has_slot_sensors == true);

    // Dryer: a stock EMU is passive, a sealed box and a sensor per lane with no heater.
    // The capture carries a drying_state array because Happy Hare's environment manager
    // is always loaded, which says nothing about whether a heater is fitted. Offering
    // drying controls here would send MMU_HEATER at a box that has none.
    auto dryer = get_dryer_info();
    REQUIRE(dryer.supported == false);
    REQUIRE(dryer.active == false);

    // v4 fields
    REQUIRE(info.sync_drive == true);
    REQUIRE(info.sync_feedback_state == "neutral");
    REQUIRE(info.spoolman_mode == SpoolmanMode::PUSH);
    REQUIRE(info.encoder_flow_rate == -1); // null encoder
}

TEST_CASE("EncoderClogInfo: clog percentage is the detection length used, clamped", "[ams][clog]") {
    struct PctRow {
        float detection_length;
        float headroom;
        int pct;
    };
    const PctRow pct_rows[] = {
        {0.0f, 5.0f, 0},     // no detection length: nothing to measure
        {12.4f, 12.4f, 0},   // full headroom
        {12.4f, 0.0f, 100},  // no headroom
        {10.0f, 3.0f, 70},   // 7mm of 10 used
        {10.0f, -1.0f, 100}, // negative headroom clamps
    };
    for (const auto& r : pct_rows) {
        INFO("length " << r.detection_length << " headroom " << r.headroom);
        EncoderClogInfo info;
        info.detection_length = r.detection_length;
        info.headroom = r.headroom;
        CHECK(info.get_clog_pct() == r.pct);
    }

    struct WarnRow {
        float desired;
        float min;
        bool warning;
    };
    const WarnRow warn_rows[] = {{5.0f, 3.0f, true}, {5.0f, 6.0f, false}, {0.0f, 0.0f, false}};
    for (const auto& r : warn_rows) {
        INFO("desired " << r.desired << " min " << r.min);
        EncoderClogInfo info;
        info.desired_headroom = r.desired;
        info.min_headroom = r.min;
        CHECK(info.is_warning() == r.warning);
    }
}

// Encoder + Flowguard JSON Parsing Tests

TEST_CASE_METHOD(HhFourGates, "Happy Hare: parse full encoder object into encoder_info",
                 "[ams][happy_hare][clog]") {
    nlohmann::json mmu_data;
    mmu_data["clog_detection_enabled"] = 2;
    mmu_data["encoder"] = {{"flow_rate", 85},
                           {"desired_headroom", 5.0},
                           {"detection_length", 12.4},
                           {"headroom", 8.0},
                           {"min_headroom", 4.2}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.encoder_info.enabled == true);
    REQUIRE(info.encoder_info.flow_rate == 85);
    REQUIRE(info.encoder_info.detection_mode == 2);
    REQUIRE(info.encoder_info.desired_headroom == Catch::Approx(5.0f));
    REQUIRE(info.encoder_info.detection_length == Catch::Approx(12.4f));
    REQUIRE(info.encoder_info.headroom == Catch::Approx(8.0f));
    REQUIRE(info.encoder_info.min_headroom == Catch::Approx(4.2f));
}

TEST_CASE_METHOD(HhFourGates, "Happy Hare: partial encoder object defaults missing fields",
                 "[ams][happy_hare][clog]") {
    nlohmann::json mmu_data;
    mmu_data["encoder"] = {{"flow_rate", 92}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.encoder_info.flow_rate == 92);
    // Not enabled because clog_detection_enabled not set
    REQUIRE(info.encoder_info.enabled == false);
    REQUIRE(info.encoder_info.detection_length == Catch::Approx(0.0f));
    REQUIRE(info.encoder_info.headroom == Catch::Approx(0.0f));
}

TEST_CASE_METHOD(HhFourGates,
                 "Happy Hare: a frame without encoder, flowguard or tangle objects leaves defaults",
                 "[ams][happy_hare][clog]") {
    helper.test_parse_mmu_state(nlohmann::json{{"action", "Idle"}});

    const auto info = helper.get_system_info();
    CHECK_FALSE(info.encoder_info.enabled);
    CHECK(info.encoder_info.flow_rate == -1);
    CHECK_FALSE(info.flowguard_info.enabled);
    CHECK_FALSE(info.flowguard_info.active);
    CHECK(info.flowguard_info.trigger.empty());
    CHECK(info.flowguard_info.reason.empty());
    CHECK_FALSE(info.tangle_prevention.present);
}

TEST_CASE_METHOD(HhFourGates, "Happy Hare: parse flowguard object", "[ams][happy_hare][clog]") {
    nlohmann::json mmu_data;
    mmu_data["flowguard"] = {{"enabled", true}, {"active", true},  {"trigger", "CLOG"},
                             {"level", 0.35},   {"max_clog", 0.8}, {"max_tangle", -0.6}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.flowguard_info.enabled == true);
    REQUIRE(info.flowguard_info.active == true);
    REQUIRE(info.flowguard_info.trigger == "CLOG");
    REQUIRE(info.flowguard_info.level == Catch::Approx(0.35f));
    REQUIRE(info.flowguard_info.max_clog == Catch::Approx(0.8f));
    REQUIRE(info.flowguard_info.max_tangle == Catch::Approx(-0.6f));
}

TEST_CASE_METHOD(HhFourGates, "Happy Hare: v4 flowguard reason and tangle_prevention parse",
                 "[ams][happy_hare][clog]") {
    nlohmann::json mmu_data;
    mmu_data["flowguard"] = {{"enabled", true},
                             {"active", true},
                             {"trigger", "TANGLE"},
                             {"reason", "no movement"},
                             {"level", -1.0}};
    mmu_data["tangle_prevention"] = {{"enabled", true},
                                     {"active", true},
                                     {"boosted", true},
                                     {"threshold", 0.6},
                                     {"release", 0.3}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    CHECK(info.flowguard_info.reason == "no movement");
    CHECK(info.tangle_prevention.present);
    CHECK(info.tangle_prevention.enabled);
    CHECK(info.tangle_prevention.active);
    CHECK(info.tangle_prevention.boosted);
    CHECK(info.tangle_prevention.threshold == Catch::Approx(0.6f));
    CHECK(info.tangle_prevention.release == Catch::Approx(0.3f));

    // Un-trip clears the reason; a later trip without one cannot show it.
    helper.test_parse_mmu_state(nlohmann::json{{"flowguard", {{"trigger", ""}}}});
    CHECK(helper.get_system_info().flowguard_info.reason.empty());
    helper.test_parse_mmu_state(nlohmann::json{{"flowguard", {{"trigger", "CLOG"}}}});
    CHECK(helper.get_system_info().flowguard_info.reason.empty());

    // A unit without a buffer publishes null: the state clears.
    helper.test_parse_mmu_state(nlohmann::json{{"tangle_prevention", nullptr}});
    CHECK_FALSE(helper.get_system_info().tangle_prevention.present);
}

TEST_CASE_METHOD(HhFourGates, "Happy Hare: parse sync_feedback_flow_rate",
                 "[ams][happy_hare][clog]") {
    nlohmann::json mmu_data;
    mmu_data["sync_feedback_flow_rate"] = 92.5;
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.sync_feedback_flow_rate == Catch::Approx(92.5f));
}

TEST_CASE("Happy Hare: VirtualSelector is a HUB, every other selector is LINEAR",
          "[ams][happy_hare][topology]") {
    const std::pair<const char*, PathTopology> rows[] = {
        {"", PathTopology::LINEAR}, // not reported yet
        {"LinearSelector", PathTopology::LINEAR},
        {"RotarySelector", PathTopology::LINEAR},
        {"ServoSelector", PathTopology::LINEAR},
        {"VirtualSelector", PathTopology::HUB},
    };
    for (const auto& [selector, topology] : rows) {
        INFO("selector_type \"" << selector << "\"");
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        if (*selector)
            helper.set_selector_type(selector);
        CHECK(helper.get_topology() == topology);
        // An index with no unit falls back to the machine's topology.
        helper.initialize_test_gates(4);
        CHECK(helper.get_unit_topology(99) == topology);
    }
}

TEST_CASE("Happy Hare: a VirtualSelector makes every unit a HUB with no encoder",
          "[ams][happy_hare][topology]") {
    SECTION("from the status frame, on one unit or two") {
        for (const int units : {1, 2}) {
            INFO("units=" << units);
            helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
            AmsBackendHappyHareTestHelper& helper = *helper_reg;
            helper.set_selector_type("VirtualSelector");
            nlohmann::json mmu_data;
            if (units > 1)
                mmu_data["num_units"] = units;
            mmu_data["gate_status"] = std::vector<int>(4 * units, 1);
            mmu_data["gate_material"] = std::vector<std::string>(4 * units, "PLA");
            mmu_data["gate"] = 0;
            mmu_data["tool"] = 0;
            mmu_data["filament"] = "Unloaded";
            mmu_data["action"] = "Idle";
            helper.test_parse_mmu_state(mmu_data);

            auto info = helper.get_system_info();
            REQUIRE(info.units.size() == static_cast<size_t>(units));
            for (const auto& unit : info.units) {
                CHECK(unit.topology == PathTopology::HUB);
                CHECK_FALSE(unit.has_encoder);
            }
        }
    }
    SECTION("a selector type arriving after the slots updates them") {
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        REQUIRE(helper.get_system_info().units[0].topology == PathTopology::LINEAR);
        REQUIRE(helper.get_system_info().units[0].has_encoder);

        helper.set_selector_type("VirtualSelector");
        helper.apply_selector_type_update();

        auto info = helper.get_system_info();
        CHECK(info.units[0].topology == PathTopology::HUB);
        CHECK_FALSE(info.units[0].has_encoder);
    }
}

// Sync feedback bias parsing

TEST_CASE_METHOD(HhFourGates, "Happy Hare parses sync_feedback_bias fields",
                 "[ams][happy_hare][v4][sync_feedback]") {
    SECTION("positive bias (compression)") {
        nlohmann::json mmu_data = {{"sync_feedback_bias_modelled", 0.75f},
                                   {"sync_feedback_bias_raw", 0.82f}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(0.75f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(0.82f));
    }

    SECTION("negative bias (tension)") {
        nlohmann::json mmu_data = {{"sync_feedback_bias_modelled", -0.5f},
                                   {"sync_feedback_bias_raw", -0.33f}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(-0.5f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(-0.33f));
    }

    SECTION("missing fields remain at sentinel") {
        nlohmann::json mmu_data = {{"gate_status", {1, 1, 1, 1}}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(-2.0f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(-2.0f));
    }
}
