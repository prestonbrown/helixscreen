// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 37 to 89 of test_ams_backend_afc.cpp before it was split, in its order. Mostly [recovery],
// [recovery][stale_fault], [recovery][phase4], [tool_mapping], [device_actions], and more

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

TEST_CASE("AFC message sets operation detail", "[ams][afc][message][phase1]") {
    // Real device: AFC global state has "message": {"message": "Loading T1", "type": "info"}
    // We never parse this field today, but it should set operation_detail.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {{"message", {{"message", "Loading T1"}, {"type", "info"}}}};
    helper.feed_afc_state(afc_data);

    // message.message should flow through to operation_detail
    REQUIRE(helper.get_operation_detail().find("Loading T1") != std::string::npos);
}

TEST_CASE("AFC error message emits EVENT_ERROR", "[ams][afc][message][phase1]") {
    // When message.type == "error", we should emit kEventError with the message text
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.install_event_tracker();

    nlohmann::json afc_data = {
        {"message", {{"message", "AFC Error: lane1 failed to load"}, {"type", "error"}}}};
    helper.feed_afc_state(afc_data);

    // error type messages should emit kEventError
    REQUIRE(helper.has_event(AmsBackend::kEventError));
    // Error data should contain the message text
    std::string error_data = helper.get_event_data(AmsBackend::kEventError);
    REQUIRE(error_data.find("lane1 failed to load") != std::string::npos);
}

TEST_CASE("AFC current_load and next_lane tracked", "[ams][afc][state][phase1]") {
    // Real device: AFC global state has "current_load": "lane2", "next_lane": "lane3"
    // These tell us which lane is actively loading and which is queued next.
    // We never parse these fields today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"current_load", "lane2"}, {"next_lane", "lane3"}, {"current_state", "Loading"}};
    helper.feed_afc_state(afc_data);

    // current_load should update current_slot (lane2 = slot 1)
    REQUIRE(helper.get_current_slot() == 1);
    // operation_detail should mention the loading context
    // At minimum, the action should be LOADING from current_state
    REQUIRE(helper.get_action() == AmsAction::LOADING);
}

// Full data parsing tests
//
// These tests verify parsing of extended hub, extruder, stepper, and buffer
// fields from real AFC device data. Tests use fixture structures captured
// from a real Box Turtle at 192.168.1.112.

TEST_CASE("AFC hub bowden length parsed from afc_bowden_length", "[ams][afc][hub][phase2]") {
    // Real device: AFC_hub Turtle_1 has "afc_bowden_length": 1285.0
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Set hub names so the status update routes correctly
    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    helper.feed_afc_hub("Turtle_1", {{"state", false}, {"afc_bowden_length", 1285.0}});

    // bowden_length should be stored and accessible for device actions
    auto actions = helper.get_device_actions();
    bool found_bowden = false;
    for (const auto& action : actions) {
        if (action.id == "bowden_length") {
            found_bowden = true;
            // Value should use the real bowden length, not hardcoded 450
            auto val = std::any_cast<float>(action.current_value);
            REQUIRE(val == Catch::Approx(1285.0f));
            break;
        }
    }
    REQUIRE(found_bowden);
}

// The AFC_stepper fields a real lane publishes land on its lane sensors.
TEST_CASE("AFC stepper fields land on the lane sensors", "[ams][afc][stepper][phase2]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"buffer_status", "Advancing"},
                                      {"filament_status", "Ready"},
                                      {"filament_status_led", "#00ff00"},
                                      {"dist_hub", 200.0}});

    auto sensors = helper.get_lane_sensors(0);
    CHECK(sensors.prep);
    CHECK(sensors.load);
    CHECK(sensors.buffer_status == "Advancing");
    CHECK(sensors.filament_status == "Ready");
    CHECK(sensors.dist_hub == Catch::Approx(200.0f));
}

TEST_CASE("AFC global quiet_mode and led_state follow the AFC object",
          "[ams][afc][global][phase2]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"quiet_mode", false}, {"led_state", true}});
    CHECK_FALSE(helper.get_quiet_mode());
    CHECK(helper.get_led_state());

    helper.feed_afc_state({{"quiet_mode", true}, {"led_state", false}});
    CHECK(helper.get_quiet_mode());
    CHECK_FALSE(helper.get_led_state());
}

TEST_CASE("AFC bowden slider max accommodates real bowden length",
          "[ams][afc][device_actions][phase2]") {
    // The bowden slider max was hardcoded to 1000mm, but real bowden can be 1285mm
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    helper.feed_afc_hub("Turtle_1", {{"state", false}, {"afc_bowden_length", 1285.0}});

    auto actions = helper.get_device_actions();
    for (const auto& action : actions) {
        if (action.id == "bowden_length") {
            // Max should accommodate the real bowden length
            REQUIRE(action.max_value >= 1285.0f);
            break;
        }
    }
}

// Device actions and commands tests
//
// Tests for new maintenance section, LED/mode toggles, and maintenance commands.

TEST_CASE("AFC device sections include maintenance and led",
          "[ams][afc][device_sections][phase3]") {
    AmsBackendAfcTestHelper helper;

    auto sections = helper.get_device_sections();

    bool has_maintenance = false;
    bool has_setup = false;
    for (const auto& section : sections) {
        if (section.id == "maintenance")
            has_maintenance = true;
        if (section.id == "setup")
            has_setup = true;
    }
    REQUIRE(has_maintenance);
    REQUIRE(has_setup);
}

// Device actions that are one AFC command each. The LED toggle sends the
// opposite of the led_state AFC last reported.
TEST_CASE("AFC device actions send their AFC command", "[ams][afc][device_actions][phase3]") {
    struct Row {
        const char* action;
        std::optional<bool> led_state;
        const char* gcode;
        bool per_lane = false; // one command per lane, so only its name is fixed
    };
    const Row rows[] = {
        {"test_lanes", std::nullopt, "AFC_TEST_LANES"},
        {"change_blade", std::nullopt, "AFC_CHANGE_BLADE"},
        {"park", std::nullopt, "AFC_PARK"},
        {"brush", std::nullopt, "AFC_BRUSH"},
        {"reset_motor", std::nullopt, "AFC_RESET_MOTOR_TIME", true},
        {"led_toggle", false, "TURN_ON_AFC_LED"},
        {"led_toggle", true, "TURN_OFF_AFC_LED"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.action << (r.led_state ? (*r.led_state ? " (on)" : " (off)") : "")) {
            AmsBackendAfcTestHelper helper;
            helper.initialize_test_lanes_with_slots(4);
            if (r.led_state) {
                helper.feed_afc_state({{"led_state", *r.led_state}});
            }

            auto result = helper.execute_device_action(r.action);

            CHECK(result.success());
            CHECK(
                (r.per_lane ? helper.has_gcode_starting_with(r.gcode) : helper.has_gcode(r.gcode)));
        }
    }
}

// AFC_TOOLHEAD_CALIBRATION is a Python-registered command, so it never appears in
// the objects list; printer.gcode.help is the only place a client can see it.
TEST_CASE("AFC toolhead calibration action follows printer.gcode.help",
          "[ams][afc][device_actions]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    SECTION("hidden before the help reply arrives") {
        CHECK_FALSE(has_action(helper.get_device_actions(), "toolhead_calibration"));
    }

    SECTION("shown in the toolhead section when AFC lists the command") {
        nlohmann::json reply = {
            {"jsonrpc", "2.0"},
            {"result",
             {{"AFC_CALIBRATION", "open the calibration menu"},
              {"AFC_TOOLHEAD_CALIBRATION", "Open the toolhead variable calibration menu"}}}};
        REQUIRE(AfcTestAccess::apply_gcode_help_response(helper, reply));

        const auto actions = helper.get_device_actions();
        const auto it = std::find_if(actions.begin(), actions.end(),
                                     [](const auto& a) { return a.id == "toolhead_calibration"; });
        REQUIRE(it != actions.end());
        CHECK(it->section == "toolhead");
        CHECK(it->enabled);
    }

    SECTION("hidden when AFC predates the command") {
        nlohmann::json reply = {{"jsonrpc", "2.0"},
                                {"result", {{"AFC_CALIBRATION", "open the calibration menu"}}}};
        REQUIRE(AfcTestAccess::apply_gcode_help_response(helper, reply));
        CHECK_FALSE(has_action(helper.get_device_actions(), "toolhead_calibration"));
    }

    SECTION("a malformed reply changes nothing") {
        CHECK_FALSE(AfcTestAccess::apply_gcode_help_response(
            helper, nlohmann::json{{"result", "AFC_TOOLHEAD_CALIBRATION"}}));
        CHECK_FALSE(has_action(helper.get_device_actions(), "toolhead_calibration"));
    }
}

TEST_CASE("AFC device action toolhead_calibration dispatches gcode", "[ams][afc][device_actions]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    SECTION("refused while the backend does not offer it") {
        // Offered or not, the id is handled; only the list says whether it may run.
        auto result = helper.execute_device_action("toolhead_calibration");
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
        CHECK_FALSE(helper.has_gcode("AFC_TOOLHEAD_CALIBRATION"));
    }

    SECTION("sent once AFC lists the command") {
        nlohmann::json reply = {
            {"jsonrpc", "2.0"},
            {"result", {{"AFC_TOOLHEAD_CALIBRATION", "Open the toolhead calibration menu"}}}};
        REQUIRE(AfcTestAccess::apply_gcode_help_response(helper, reply));
        auto result = helper.execute_device_action("toolhead_calibration");
        REQUIRE(result.success());
        REQUIRE(helper.has_gcode("AFC_TOOLHEAD_CALIBRATION"));
    }
}

TEST_CASE("AFC device action quiet_mode dispatches an explicit ENABLE",
          "[ams][afc][device_actions][quiet_mode][phase3]") {
    // AFC_QUIET_MODE's ENABLE parameter defaults to the CURRENT value —
    // `gcmd.get_int("ENABLE", self._get_quiet_mode(), minval=0, maxval=1)`
    // (AFC.py v1.2.0:934-953, identical at v1.1.0:736-756) — so a bare
    // `AFC_QUIET_MODE` sets quiet mode to whatever it already was and the
    // button does nothing at all.
    //
    // The `show_macros` wrapper supplies no parameter either: _create_options
    // emits `{%set dummy=params.ENABLE|default('0')|int%}` and then
    // `_AFC_QUIET_MODE {rawparams}` (AFC_functions.py:637-644). The `dummy`
    // assignment is discarded and rawparams is empty for a bare call.
    //
    // So the ENABLE VALUE is the whole test. A prefix or substring match would
    // re-admit the exact bug, because `AFC_QUIET_MODE` is a prefix of the
    // correct command.
    SECTION("off -> on") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        REQUIRE_FALSE(helper.get_quiet_mode()); // default

        REQUIRE(helper.execute_device_action("quiet_mode").success());

        REQUIRE(helper.has_gcode("AFC_QUIET_MODE ENABLE=1"));
        // The value must be the INVERSE of the tracked state, not a constant.
        REQUIRE_FALSE(helper.has_gcode("AFC_QUIET_MODE ENABLE=0"));
    }

    SECTION("on -> off") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.feed_afc_state({{"quiet_mode", true}});
        REQUIRE(helper.get_quiet_mode());

        REQUIRE(helper.execute_device_action("quiet_mode").success());

        REQUIRE(helper.has_gcode("AFC_QUIET_MODE ENABLE=0"));
        REQUIRE_FALSE(helper.has_gcode("AFC_QUIET_MODE ENABLE=1"));
    }

    SECTION("never the bare no-op form, whatever the state") {
        // Exact-match, so this fails if the parameter is ever dropped again.
        for (bool quiet : {false, true}) {
            CAPTURE(quiet);
            AmsBackendAfcTestHelper helper;
            helper.initialize_test_lanes_with_slots(4);
            helper.feed_afc_state({{"quiet_mode", quiet}});
            helper.clear_captured_gcodes();

            REQUIRE(helper.execute_device_action("quiet_mode").success());

            REQUIRE(helper.captured_gcodes.size() == 1);
            REQUIRE_FALSE(helper.has_gcode("AFC_QUIET_MODE"));
            REQUIRE(helper.captured_gcodes[0] ==
                    (quiet ? "AFC_QUIET_MODE ENABLE=0" : "AFC_QUIET_MODE ENABLE=1"));
        }
    }
}

// Error recovery tests
//
// Tests for differentiated reset (AFC_RESET vs AFC_HOME), per-lane reset,
// and error message surfacing.

TEST_CASE("AFC recover sends AFC_RESET", "[ams][afc][recovery][phase4]") {
    // recover() uses AFC_RESET
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true); // Bypass precondition for unit test

    auto result = helper.recover();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_RESET"));
    REQUIRE_FALSE(helper.has_gcode("AFC_HOME"));
}

TEST_CASE("AFC reset sends AFC_RESET command", "[ams][afc][recovery]") {
    // reset() sends AFC_RESET — the same gcode as recover(), since AFC only has one reset command.
    // Both operations use AFC_RESET; the distinction is in the UI notification text only.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true); // Bypass precondition for unit test

    auto result = helper.reset();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_RESET"));
}

// AFC_LANE_RESET names the lane by its AFC name; a slot outside the discovered
// lanes is refused, and so is any lane while the backend is not running.
TEST_CASE("AFC recover_lane_position sends AFC_LANE_RESET for a valid lane",
          "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    SECTION("not running") {
        auto result = helper.recover_lane_position(0);
        CHECK(result.result == AmsResult::NOT_CONNECTED);
        CHECK(helper.captured_gcodes.empty());
    }

    helper.set_running(true);
    for (const auto& [slot, gcode] :
         {std::pair{0, "AFC_LANE_RESET LANE=lane1"}, std::pair{2, "AFC_LANE_RESET LANE=lane3"}}) {
        DYNAMIC_SECTION("slot " << slot) {
            REQUIRE(helper.recover_lane_position(slot).success());
            CHECK(helper.has_gcode(gcode));
        }
    }
    for (int slot : {99, 4, -1}) {
        DYNAMIC_SECTION("out of range " << slot) {
            auto result = helper.recover_lane_position(slot);
            CHECK(result.result == AmsResult::INVALID_SLOT);
            CHECK(helper.captured_gcodes.empty());
        }
    }
}

TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC set_tool_mapping refuses a lane past the discovered range",
                 "[ams][afc][slot_index]") {
    CHECK(helper.set_tool_mapping(0, 4).result == AmsResult::INVALID_SLOT);
    CHECK(helper.set_tool_mapping(0, -2).result == AmsResult::INVALID_SLOT);
}

TEST_CASE("Z-Mod's lane-picking shim offers no remap and sends no SET_MAP",
          "[ams][afc][tool_mapping]") {
    AmsBackendAfcTestHelper helper;
    discover_afc(helper, /*afclite=*/true);
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    CHECK(helper.get_remap_strategy() == AmsBackend::RemapStrategy::None);
    CHECK_FALSE(helix::printer::can_remap(helper));
    CHECK_FALSE(helix::printer::can_write_mapping_table(helper));

    CHECK(helper.set_tool_mapping(0, 1).result == AmsResult::NOT_SUPPORTED);

    SlotInfo info;
    info.mapped_tool = 2;
    helix::test::apply_edit(helper, 0, info);

    CHECK_FALSE(helper.has_gcode_starting_with("SET_MAP "));
}

TEST_CASE("Z-Mod's lane-picking shim leaves every lane to the print",
          "[ams][afc][tool_mapping][filament_mapping][preflight]") {
    helix::test::RegisteredBackend<AmsBackendAfcTestHelper> reg;
    AmsBackendAfcTestHelper& helper = *reg;
    discover_afc(helper, /*afclite=*/true);
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(helper.firmware_picks_lanes_at_print());
    const auto routing = helper.firmware_default_routing();
    CHECK(routing.chosen_at_print);
    CHECK(routing.head(0) == -1);

    helix::GcodeToolInfo t0{};
    t0.tool_index = 0;
    t0.color_known = false;
    auto& ams = AmsState::instance();
    const auto seed = ams.seed_tool_mappings({t0}, ams.collect_available_slots());
    REQUIRE(seed.size() == 1);
    CHECK(seed[0].is_auto);
    CHECK(seed[0].mapped_slot == -1);
    CHECK_FALSE(helix::file_preflight({t0}, seed, false).has_block());
}

TEST_CASE("AFC without the shim routes by its own lane map", "[ams][afc][tool_mapping]") {
    AmsBackendAfcTestHelper helper;
    discover_afc(helper, /*afclite=*/false);
    CHECK_FALSE(helper.firmware_picks_lanes_at_print());
    CHECK_FALSE(helper.firmware_default_routing().chosen_at_print);
}

TEST_CASE("AFC with its own SET_MAP keeps the native remap", "[ams][afc][tool_mapping]") {
    AmsBackendAfcTestHelper helper;
    discover_afc(helper, /*afclite=*/false);
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    CHECK(helper.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    CHECK(helper.set_tool_mapping(0, 1).success());
    CHECK(helper.has_gcode("SET_MAP LANE=lane2 MAP=T0"));
}

TEST_CASE("AFC recover_lane_position on a backend with no lanes offers no span",
          "[ams][afc][recovery][phase4]") {
    // No lanes discovered yet (slot_count() == 0). The suggestion must not
    // claim a lane 1 exists.
    AmsBackendAfcTestHelper helper;
    helper.set_running(true);

    auto result = helper.recover_lane_position(0);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
    REQUIRE(result.suggestion == "Select a valid number");
}

TEST_CASE("AFC lane reset is offered only when that lane's hub sensor is triggered",
          "[ams][afc][recovery]") {
    // Measured on a live BoxTurtle 2026-07-27: loaded_to_hub is latched at prep and
    // never updates, reading true on all four lanes at once while the hub is clear.
    // AFC_hub.state is the only signal that tracks an actual hub transit.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "Turtle_1");

    // Satisfy the other gates so the hub sensor is the only discriminator.
    helper.set_active_load_lane("lane1");
    helper.set_lane_load_sensor(0, true);

    // The latched field says "at hub" on every lane. It must not be believed.
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_hub_sensor("Turtle_1", false);
    REQUIRE_FALSE(helper.can_recover_lane_position(0));

    // Hub sensor triggered → the retract has somewhere to retract from.
    helper.set_hub_sensor("Turtle_1", true);
    REQUIRE(helper.can_recover_lane_position(0));
}

TEST_CASE("AFC lane reset is refused while the toolhead holds filament", "[ams][afc][recovery]") {
    // Upstream's own toolhead guard logs and then falls through — it is missing
    // its `return` (AFCProject/AFC-Klipper-Add-On#803), so cmd_AFC_LANE_RESET
    // retracts the lane while the extruder still grips the filament. Ours is the
    // only check that actually stops that, so each of the three signals it reads
    // has to hold on its own.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "Turtle_1");
    helper.set_hub_sensor("Turtle_1", true);
    helper.set_active_load_lane("lane1");
    helper.set_lane_load_sensor(0, true);

    // Baseline: toolhead genuinely free.
    REQUIRE(helper.can_recover_lane_position(0));

    SECTION("tool_start sensor triggered") {
        helper.set_tool_start_sensor(true);
        REQUIRE_FALSE(helper.can_recover_lane_position(0));
        helper.set_tool_start_sensor(false);
        REQUIRE(helper.can_recover_lane_position(0));
    }

    SECTION("tool_end sensor triggered") {
        helper.set_tool_end_sensor(true);
        REQUIRE_FALSE(helper.can_recover_lane_position(0));
        helper.set_tool_end_sensor(false);
        REQUIRE(helper.can_recover_lane_position(0));
    }

    SECTION("AFC.current_load names a lane") {
        // This is the exact condition upstream's guard tests.
        helper.set_toolhead_lane("lane1");
        REQUIRE_FALSE(helper.can_recover_lane_position(0));
        helper.set_toolhead_lane("");
        REQUIRE(helper.can_recover_lane_position(0));
    }

    SECTION("a lane reports tool_loaded even with AFC.current_load empty") {
        // The desync case: AFC persists per-lane tool_loaded through save_vars,
        // so it survives a restart that leaves AFC.current null.
        helper.set_toolhead_lane("");
        helper.get_mutable_slot(2)->status = SlotStatus::LOADED;
        REQUIRE_FALSE(helper.can_recover_lane_position(0));
        helper.get_mutable_slot(2)->status = SlotStatus::AVAILABLE;
        REQUIRE(helper.can_recover_lane_position(0));
    }
}

TEST_CASE("AFC attribution survives the filament_loaded derivation", "[ams][afc][recovery]") {
    // The bug this pins: apply_afc_state() derives filament_loaded from
    // `loaded_lane`, which PREFERS current_lane (= AFC.current_loading). No
    // shipped AFC build publishes an explicit filament_loaded key, so naming a
    // lane in current_lane sets filament_loaded true as a side effect.
    //
    // A toolhead guard reading filament_loaded is therefore mutually exclusive
    // with attribution: the attributed arm could never fire, and recovery was
    // only ever reachable through the unattributed all-lanes fallback #1182
    // removes. Removing that fallback while the guard still read filament_loaded
    // would have deleted per-lane recovery outright rather than narrowing it.
    //
    // Driven through feed_afc_state() on purpose — with set_active_load_lane()
    // the derivation never runs and this test cannot fail.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "Turtle_1");
    helper.set_hub_sensor("Turtle_1", true);
    helper.set_lane_load_sensor(0, true);

    // A failed TOOL_LOAD: AFC names the lane it was working, the toolhead never
    // received anything, so current_load stays null.
    helper.feed_afc_state({{"current_lane", "lane1"}, {"current_load", nullptr}});

    REQUIRE(helper.get_active_load_lane() == "lane1");
    REQUIRE(helper.get_toolhead_lane().empty());
    REQUIRE(helper.get_system_info().filament_loaded); // the derivation, unchanged
    REQUIRE(helper.can_recover_lane_position(0));      // and recovery still offered
}

TEST_CASE("AFC lane reset is refused for a lane routed direct (no hub)", "[ams][afc][recovery]") {
    // "direct" routing means the lane bypasses the hub entirely, so there is no
    // hub sensor to consult and no hub-retract to perform.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "direct");
    helper.set_hub_sensor("Turtle_1", true);

    // Force the latched field true so this case discriminates: a body that
    // returned it directly would answer true here. Without this the sensor's
    // false default makes both implementations agree, and the test proves nothing.
    helper.set_lane_loaded_to_hub(0, true);

    // Every other gate satisfied, so "direct" routing is the sole reason for the
    // refusal — otherwise this passes for the wrong reason once more gates exist.
    helper.set_active_load_lane("lane1");
    helper.set_lane_load_sensor(0, true);

    REQUIRE_FALSE(helper.can_recover_lane_position(0));
}

TEST_CASE("AFC attributes a triggered hub to the lane AFC names as active",
          "[ams][afc][recovery]") {
    // AFC_hub is one sensor shared by every lane on the unit, so a triggered hub
    // alone would offer recovery on all of them at once (observed on a live
    // BoxTurtle 2026-07-27). AFC.current_lane names the lane it was working.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    for (const char* lane : {"lane1", "lane2", "lane3", "lane4"}) {
        helper.set_lane_hub_routing(lane, "Turtle_1");
    }
    helper.set_hub_sensor("Turtle_1", true);

    // All four lanes are seated identically — the live BoxTurtle reads
    // prep/load/loaded_to_hub true on every lane at once. Attribution must be
    // what separates them, not any per-lane sensor.
    for (int i = 0; i < 4; ++i) {
        helper.set_lane_load_sensor(i, true);
    }

    helper.set_active_load_lane("lane2");

    REQUIRE_FALSE(helper.can_recover_lane_position(0));
    REQUIRE(helper.can_recover_lane_position(1));
    REQUIRE_FALSE(helper.can_recover_lane_position(2));
    REQUIRE_FALSE(helper.can_recover_lane_position(3));
}

TEST_CASE("AFC offers recovery on no lane at all when it names none", "[ams][afc][recovery]") {
    // This inverts the previous all-lanes fallback (prestonbrown/helixscreen#1182).
    //
    // The fallback rested on "a wrong guess costs one harmless refusal from the
    // firmware". That is false. cmd_AFC_LANE_RESET opens with an unconditional
    // move_to_hub(DISTANCE, NEG) — DISTANCE defaults to 50 — before any per-lane
    // state check, so a wrong guess physically retracts a correctly-seated lane
    // past its own load switch. Observed on the live BoxTurtle 2026-07-27: a
    // guess at lane1 left it load=False, which then failed T0 with "LOAD TRIGGER
    // NOT TRIGGERED" and needed a manual forward move to restore.
    //
    // Offering nothing does not strand the user: the sidebar Reset dispatches
    // AFC_RESET, which is AFC's OWN lane picker (cmd_AFC_RESET lists every lane
    // with raw_load_state true and dispatches AFC_LANE_RESET for the chosen
    // one). The firmware's candidate list beats anything we would guess.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    for (const char* lane : {"lane1", "lane2", "lane3", "lane4"}) {
        helper.set_lane_hub_routing(lane, "Turtle_1");
    }
    helper.set_hub_sensor("Turtle_1", true);

    // Every lane fully seated and the hub triggered: the ONLY thing missing is
    // attribution. Without these the lanes would be refused for lacking a load
    // switch and the test would not exercise the attribution gate at all.
    for (int i = 0; i < 4; ++i) {
        helper.set_lane_load_sensor(i, true);
    }
    helper.set_active_load_lane("");

    REQUIRE_FALSE(helper.lane_recovery_is_attributed());
    for (int i = 0; i < 4; ++i) {
        REQUIRE_FALSE(helper.can_recover_lane_position(i));
    }
}

TEST_CASE("AFC lane reset is refused while the lane's own load switch is clear",
          "[ams][afc][recovery]") {
    // prestonbrown/helixscreen#1187. cmd_AFC_LANE_RESET does not guard on the
    // lane's load state up front — it opens with an unconditional
    //     move_to_hub(cur_lane, DISTANCE, MoveDirection.NEG, ...)   # DISTANCE=50
    // and only checks `if not cur_lane.raw_load_state` AFTER that move plus one
    // further short move inside the retract loop. So invoking it on a lane whose
    // filament already sits behind its load switch retracts that lane a further
    // ~50mm toward (and potentially out of) its prep sensor and drive gears
    // before erroring out.
    //
    // That is exactly the state a lane is left in by a previous wrong guess, so
    // without this gate the most natural follow-up interaction — tapping Recover
    // again on the lane that just failed — compounds the damage.
    //
    // It also matches cmd_AFC_RESET's own picker, which lists only lanes with
    // raw_load_state true. AFC.get_status publishes that as `load`.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "Turtle_1");
    helper.set_hub_sensor("Turtle_1", true);
    helper.set_active_load_lane("lane1");

    // Hub occupied, toolhead free, AFC names this exact lane — every other gate
    // passes, so the load switch is the sole discriminator.
    helper.set_lane_load_sensor(0, false);
    REQUIRE_FALSE(helper.can_recover_lane_position(0));

    helper.set_lane_load_sensor(0, true);
    REQUIRE(helper.can_recover_lane_position(0));
}

TEST_CASE("AFC treats a stale active_load_lane_ as unattributed rather than "
          "matching no lane",
          "[ams][afc][recovery]") {
    // initialize_slots() (re-)runs whenever the lane count changes but never
    // touches active_load_lane_. If a unit re-init or lane rename leaves it
    // naming a lane that no longer exists in the registry, `lane_name ==
    // active_load_lane_` is false for every lane at once. Both the gate and the
    // UI-facing attribution flag must read that the same way — a stale name is
    // NOT attribution, so neither may treat it as one.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    for (const char* lane : {"lane1", "lane2", "lane3", "lane4"}) {
        helper.set_lane_hub_routing(lane, "Turtle_1");
    }
    helper.set_hub_sensor("Turtle_1", true);
    for (int i = 0; i < 4; ++i) {
        helper.set_lane_load_sensor(i, true);
    }

    // Attribute to a real lane first, to prove the stale case is what changes
    // behavior here (not simply an always-empty active_load_lane_).
    //
    // Driven through apply_afc_state rather than poking active_load_lane_ alone:
    // the name only ever reaches us on a frame that also carries current_state,
    // and lane_recovery_is_attributed() now reads the action too, so setting the
    // name by itself models a state AFC cannot produce.
    helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Loading"}});
    REQUIRE(helper.get_active_load_lane() == "lane2");
    REQUIRE(helper.lane_recovery_is_attributed());
    REQUIRE(helper.can_recover_lane_position(1));

    // Re-init drops lane2..lane4 from the registry. active_load_lane_ still
    // says "lane2", but lane2 no longer exists.
    helper.initialize_test_lanes_with_slots(1);
    helper.set_lane_load_sensor(0, true);
    REQUIRE(helper.get_active_load_lane() == "lane2");

    // Both the gate and the UI-facing attribution flag must fall back to
    // unattributed rather than disagree with each other.
    REQUIRE_FALSE(helper.lane_recovery_is_attributed());
    REQUIRE_FALSE(helper.can_recover_lane_position(0));
}

TEST_CASE("AFC: a current_loading latched by a FAILED toolchange is not attribution",
          "[ams][afc][recovery][eject]") {
    // AFC.current_loading is set at the top of TOOL_LOAD (AFC.py v1.2.0:1523) and
    // TOOL_UNLOAD (:1948). It is cleared in exactly two places, set_tool_loaded()
    // and set_tool_unloaded(), and BOTH clears sit under `if normal_toolchange:`
    // (AFC_lane.py:1526, :1545) — the success path. A toolchange that fails
    // therefore pins current_loading to that lane until the next SUCCESSFUL one,
    // which a user with a jammed lane cannot perform.
    //
    // AFC publishes it as `current_lane`, which active_load_lane_ prefers over
    // current_load precisely because attribution wants "the lane being worked".
    // That is right while a toolchange is running and wrong once it has stopped:
    // a latched name is the residue of a failure, not a live diagnosis.
    //
    // It matters because can_recover && recovery_attributed OUTRANKS Eject in
    // decide_unload_mode(). Treating the latch as attribution means the failure
    // that makes a user want Eject is the very thing that takes Eject away,
    // replacing it with Recover — a lane reset that retracts toward the hub and
    // never returns filament to the spool.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    for (const char* lane : {"lane1", "lane2", "lane3", "lane4"}) {
        helper.set_lane_hub_routing(lane, "Turtle_1");
    }
    helper.set_hub_sensor("Turtle_1", true);
    for (int i = 0; i < 4; ++i) {
        helper.set_lane_load_sensor(i, true);
    }

    SECTION("mid-toolchange, the named lane IS the one being worked") {
        helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Loading"}});

        REQUIRE(helper.get_active_load_lane() == "lane2");
        CHECK(helper.lane_recovery_is_attributed());
        CHECK(helper.can_recover_lane_position(1));
    }

    SECTION("an AFC error state is still a live diagnosis") {
        // The lane genuinely needs recovery here and AFC is still saying so, so
        // the attribution stands. Narrowing must not cost the case recovery
        // ranking exists to serve.
        helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Error"}});

        CHECK(helper.lane_recovery_is_attributed());
        CHECK(helper.can_recover_lane_position(1));
    }

    SECTION("back at Idle with the name still latched, it is NOT attribution") {
        // The failure path: TOOL_LOAD set current_loading, failed, and nothing
        // ever cleared it. AFC is idle, so no lane is being worked — the name is
        // stale by construction.
        helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Loading"}});
        REQUIRE(helper.lane_recovery_is_attributed());

        helper.test_parse_afc_state({{"current_state", "Idle"}});

        // The name is still there; AFC never clears it on failure.
        REQUIRE(helper.get_active_load_lane() == "lane2");
        // It no longer outranks Eject...
        CHECK_FALSE(helper.lane_recovery_is_attributed());
        // ...but Recover itself must REMAIN offered. can_recover_lane_position()
        // reads the same name as a safety gate — "only retract a lane AFC itself
        // names", so a blind opening retract cannot land on the wrong one
        // (#1182). A latched name still identifies the right lane, and the lane
        // really may be stranded. Withdrawing the ranking must not withdraw the
        // recovery; that would strand exactly the user it exists for.
        CHECK(helper.can_recover_lane_position(1));
    }

    SECTION("the whole point: Recover stays available but stops displacing Eject") {
        // decide_unload_mode() ranks `can_recover && attributed` above Eject and
        // plain `can_recover` below it. With attribution withdrawn and the gate
        // still open, the jammed lane gets Eject back AND keeps Recover as the
        // last-resort arm — which is the outcome this whole change is for.
        helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Idle"}});

        CHECK_FALSE(helper.lane_recovery_is_attributed());
        CHECK(helper.can_recover_lane_position(1));
    }
}

TEST_CASE("AFC active_load_lane_ is populated from a current_load delta and "
          "cleared when it goes null",
          "[ams][afc][recovery]") {
    // A setter-only test would pass even if apply_afc_state() never assigned
    // active_load_lane_ at all — this drives it through the real status-update
    // path (feed_afc_state -> apply_afc_state) to prove the parse actually runs.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    REQUIRE(helper.get_active_load_lane().empty());

    helper.feed_afc_state({{"current_load", "lane2"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});
    REQUIRE(helper.get_active_load_lane() == "lane2");

    helper.feed_afc_state({{"current_load", nullptr},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});
    REQUIRE(helper.get_active_load_lane().empty());
}

TEST_CASE("AFC active_load_lane_ survives a delta that omits both lane keys",
          "[ams][afc][recovery]") {
    // apply_afc_state() is fed notify_status_update DELTAS, not snapshots — an
    // absent key means "unchanged," never "clear." AFC.current_toolchange,
    // message, and current_state each trigger a parse independently of
    // current_lane/current_load. An unconditional assignment would wipe the
    // attribution on the very next such delta, re-exposing Recover on every
    // lane on the hub within one update — the original bug, back immediately.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"current_lane", "lane1"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});
    REQUIRE(helper.get_active_load_lane() == "lane1");

    // A delta that mentions neither current_lane nor current_load — only a
    // toolchange counter changed. Attribution must be untouched.
    helper.feed_afc_state({{"current_toolchange", 3}});
    REQUIRE(helper.get_active_load_lane() == "lane1");

    // Same for a message-only delta.
    helper.feed_afc_state({{"message", {{"message", "some notice"}, {"type", "info"}}}});
    REQUIRE(helper.get_active_load_lane() == "lane1");
}

TEST_CASE("AFC error message surfaces in EVENT_ERROR data", "[ams][afc][recovery][phase4]") {
    // Verify that AFC error messages contain useful text in the event data
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.install_event_tracker();

    nlohmann::json afc_data = {
        {"message", {{"message", "Lane 1 failed: filament jam detected"}, {"type", "error"}}}};
    helper.feed_afc_state(afc_data);

    REQUIRE(helper.has_event(AmsBackend::kEventError));
    std::string error_data = helper.get_event_data(AmsBackend::kEventError);
    REQUIRE(error_data.find("filament jam detected") != std::string::npos);
}

TEST_CASE("AFC clear_fault sends RESET_FAILURE and AFC_CLEAR_MESSAGE", "[ams][afc][recovery]") {
    // Measured 2026-07-27: AFC_RESET leaves printer.AFC.message untouched. Only
    // AFC_CLEAR_MESSAGE pops it, and RESET_FAILURE clears the failure flag.
    // Both must fire, and unlike cancel() this must work from IDLE — that is
    // exactly the state a queued message outlives its operation in.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.clear_fault(0);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("RESET_FAILURE"));
    REQUIRE(helper.has_gcode("AFC_CLEAR_MESSAGE"));
}

TEST_CASE("AFC clear_fault is scope-independent of the slot argument", "[ams][afc][recovery]") {
    // AFC has no per-lane fault clear; both commands are system-scoped. Passing a
    // slot must neither fail nor change the gcode emitted.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    REQUIRE(helper.clear_fault(-1).success());
    REQUIRE(helper.has_gcode("AFC_CLEAR_MESSAGE"));
}

TEST_CASE("AFC clears the fault after a successful reset of the faulted lane",
          "[ams][afc][recovery][stale_fault]") {
    StaleFaultRig rig;

    REQUIRE(rig.helper.recover_lane_position(2).success());
    REQUIRE(rig.helper.has_gcode("AFC_LANE_RESET LANE=lane3"));
    REQUIRE(rig.console_watched());

    rig.console("// lane3 reset to hub, take necessary action");
    REQUIRE_FALSE(rig.helper.has_gcode("RESET_FAILURE"));
    REQUIRE(rig.helper.pending_gcode_complete);
    rig.helper.pending_gcode_complete();

    REQUIRE(rig.helper.has_gcode("RESET_FAILURE"));
    REQUIRE(rig.helper.has_gcode("AFC_CLEAR_MESSAGE"));
    REQUIRE_FALSE(rig.console_watched());
}

TEST_CASE("AFC keeps the fault after a failed lane reset", "[ams][afc][recovery][stale_fault]") {
    StaleFaultRig rig;
    REQUIRE(rig.helper.recover_lane_position(2).success());

    SECTION("failure line") {
        rig.console("!! 'lane3' failed to reset to hub");
    }
    SECTION("wrong-lane failure line") {
        rig.console("!! 'lane3' failed to reset to hub, load switch became false during reset");
    }
    SECTION("no verdict at all") {}
    SECTION("another lane's success") {
        rig.console("// lane1 reset to hub, take necessary action");
    }

    rig.helper.pending_gcode_complete();
    REQUIRE_FALSE(rig.helper.has_gcode("RESET_FAILURE"));
    REQUIRE_FALSE(rig.console_watched());
}

TEST_CASE("AFC leaves a fault on another lane after a lane reset succeeds",
          "[ams][afc][recovery][stale_fault]") {
    StaleFaultRig rig;
    rig.helper.feed_afc_state({{"current_lane", "lane1"}});

    REQUIRE(rig.helper.recover_lane_position(2).success());
    rig.console("// lane3 reset to hub, take necessary action");
    rig.helper.pending_gcode_complete();

    REQUIRE_FALSE(rig.helper.has_gcode("RESET_FAILURE"));
}

TEST_CASE("AFC sends no clear when the fault cleared during the reset",
          "[ams][afc][recovery][stale_fault]") {
    StaleFaultRig rig;
    REQUIRE(rig.helper.recover_lane_position(2).success());
    rig.console("// lane3 reset to hub, take necessary action");
    rig.helper.feed_afc_state({{"current_state", "Idle"}, {"error_state", false}});
    rig.helper.pending_gcode_complete();

    REQUIRE_FALSE(rig.helper.has_gcode("RESET_FAILURE"));
}

TEST_CASE("AFC offers a fault clear on the faulted lane only while nothing is in flight",
          "[ams][afc][recovery][stale_fault]") {
    StaleFaultRig rig;
    auto& h = rig.helper;

    REQUIRE(h.has_clearable_fault(2));
    REQUIRE_FALSE(h.has_clearable_fault(0));
    REQUIRE_FALSE(h.has_clearable_fault(99));

    SECTION("a lane AFC marks Error is clearable too") {
        h.feed_afc_stepper("lane2", {{"status", "Error"}});
        REQUIRE(h.has_clearable_fault(1));
    }

    SECTION("not while the lane reset is in flight") {
        REQUIRE(h.recover_lane_position(2).success());
        REQUIRE_FALSE(h.has_clearable_fault(2));

        SECTION("back once it completes") {
            h.pending_gcode_complete();
        }
        SECTION("back once it errors") {
            MoonrakerError err;
            err.type = MoonrakerErrorType::TIMEOUT;
            h.pending_gcode_error(err);
            REQUIRE_FALSE(rig.console_watched());
        }
        REQUIRE(h.has_clearable_fault(2));
    }

    SECTION("not once AFC leaves its error state") {
        h.feed_afc_state({{"current_state", "Idle"}, {"error_state", false}});
        REQUIRE_FALSE(h.has_clearable_fault(2));
    }
}

TEST_CASE("AFC keeps Clear Error on a lane still marked Error after error_state clears",
          "[ams][afc][recovery][stale_fault]") {
    // RESET_FAILURE clears error_state but never the lane's own status, so the
    // lane keeps its dot. The action has to stay with it.
    StaleFaultRig rig;
    rig.helper.feed_afc_stepper("lane3", {{"status", "Error"}});
    rig.helper.feed_afc_state({{"current_state", "Idle"}, {"error_state", false}});

    REQUIRE(rig.helper.get_slot_info(2).error.has_value());
    REQUIRE(rig.helper.has_clearable_fault(2));
}

TEST_CASE("AFC still marks a lane that failed outside an error state",
          "[ams][afc][recovery][stale_fault]") {
    // A load failure outside a print calls handle_lane_failure(pause=False):
    // the lane goes to Error and error_state never rises. That is a live fault,
    // which is why error_state alone cannot retire a lane's Error status.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_state({{"current_state", "Idle"}, {"error_state", false}});
    helper.feed_afc_stepper("lane2", {{"status", "Error"}});

    REQUIRE(helper.get_slot_info(1).error.has_value());
    REQUIRE(helper.has_clearable_fault(1));
}

TEST_CASE("AFC places a lane's fault at the sensor its later sentence names",
          "[ams][afc][fault][sensor]") {
    // The lane's Error status can land a frame before handle_lane_failure's sentence.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_stepper("lane2", {{"status", "Error"}});
    helper.feed_afc_stepper("lane3", {{"status", "Error"}});
    REQUIRE(helper.get_slot_info(1).error.has_value());
    CHECK(helper.get_slot_info(1).error->sensor == LaneSensor::NONE);

    helper.feed_afc_state(
        {{"message",
          {{"message", "lane2 Current lane not loaded, LOAD TRIGGER NOT TRIGGERED"},
           {"type", "error"}}}});
    REQUIRE(helper.get_slot_info(1).error.has_value());
    CHECK(helper.get_slot_info(1).error->sensor == LaneSensor::LOAD);
    // Another faulted lane takes no sensor from lane2's sentence.
    REQUIRE(helper.get_slot_info(2).error.has_value());
    CHECK(helper.get_slot_info(2).error->sensor == LaneSensor::NONE);
}

TEST_CASE("AFC's prep runout lights the lane's prep sensor", "[ams][afc][fault][sensor][prep]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_stepper("lane2", {{"status", "Error"}});
    helper.feed_afc_state(
        {{"message",
          {{"message", "Runout triggered for lane lane2 and runout lane is not setup to switch "
                       "to another lane.\nPlease manually load next spool into toolhead and then "
                       "hit resume to continue."},
           {"type", "error"}}}});
    REQUIRE(helper.get_slot_info(1).error.has_value());
    CHECK(helper.get_slot_info(1).error->sensor == LaneSensor::PREP);
}

TEST_CASE("AFC clearing a fault drops the lane's stale Error marking",
          "[ams][afc][recovery][stale_fault]") {
    StaleFaultRig rig;
    auto& h = rig.helper;
    h.feed_afc_stepper("lane3", {{"status", "Error"}});
    REQUIRE(h.get_slot_info(2).error.has_value());

    REQUIRE(h.clear_fault(2).success());
    REQUIRE(h.has_gcode("RESET_FAILURE"));

    SECTION("a refused RESET_FAILURE keeps the marking") {
        REQUIRE(h.get_slot_info(2).error.has_value());
        REQUIRE(h.has_clearable_fault(2));
    }
    SECTION("an accepted one retires it until the lane fails again") {
        // AFC answers RESET_FAILURE with error_state false and no lane status delta.
        h.accept_notify_gcode();
        h.feed_afc_state({{"current_state", "Idle"}, {"error_state", false}});
        h.feed_afc_stepper("lane3", {{"prep", true}});

        REQUIRE_FALSE(h.get_slot_info(2).error.has_value());
        REQUIRE_FALSE(h.has_clearable_fault(2));

        SECTION("the lane failing again outside a print brings it back") {
            // handle_lane_failure(pause=False): status stays Error (no delta) and
            // error_state never rises. Only the new message says so.
            h.feed_afc_state({{"message", {{"message", ""}, {"type", ""}}}});
            h.feed_afc_state(
                {{"message",
                  {{"message", "lane3 Current lane not loaded, LOAD TRIGGER NOT TRIGGERED"},
                   {"type", "error"}}}});
            REQUIRE(h.get_slot_info(2).error.has_value());
            REQUIRE(h.has_clearable_fault(2));
            CHECK(h.get_slot_info(2).error->sensor == LaneSensor::LOAD);
        }
        SECTION("a new error naming no lane brings every acknowledged lane back") {
            h.feed_afc_state(
                {{"message", {{"message", "AFC could not home the selector"}, {"type", "error"}}}});
            REQUIRE(h.get_slot_info(2).error.has_value());
        }
        SECTION("a new error naming another lane leaves this one cleared") {
            h.feed_afc_state(
                {{"message",
                  {{"message", "lane1 Current lane not loaded, LOAD TRIGGER NOT TRIGGERED"},
                   {"type", "error"}}}});
            REQUIRE_FALSE(h.get_slot_info(2).error.has_value());
        }

        SECTION("a new AFC error brings the marking back") {
            h.feed_afc_state({{"current_state", "Error"}, {"error_state", true}});
            REQUIRE(h.get_slot_info(2).error.has_value());
        }
        SECTION("the lane failing again after moving on brings it back") {
            h.feed_afc_stepper("lane3", {{"status", "Tooled"}});
            h.feed_afc_stepper("lane3", {{"status", "Error"}});
            REQUIRE(h.get_slot_info(2).error.has_value());
        }
    }
}

TEST_CASE("AFC retires the stale marking once Moonraker accepts RESET_FAILURE",
          "[ams][afc][recovery][stale_fault]") {
    // The production execute path, over a client that accepts every script.
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    helix::test::WiredBackend<AmsBackendAfc> afc;
    afc.wire().respond_to("printer.gcode.script", "ok");
    afc.set_discovered_lanes({"lane1", "lane2", "lane3"}, {});
    REQUIRE(afc.start().success());
    helix::ui::UpdateQueue::instance().drain();
    afc.fire_status({{"AFC", {{"current_state", "Error"}, {"error_state", true}}},
                     {"AFC_stepper lane3", {{"status", "Error"}}}});
    REQUIRE(afc.get_slot_info(2).error.has_value());

    int changed = 0;
    const size_t scripts = afc.wire().fake_client().rpc_count("printer.gcode.script");
    afc.set_event_callback([&changed](const std::string& event, const std::string&) {
        changed += event == helix::AmsBackend::EVENT_STATE_CHANGED;
    });
    REQUIRE(afc.clear_fault(2).success());
    REQUIRE(afc.wire().fake_client().rpc_count("printer.gcode.script") > scripts);
    changed = 0;
    helix::ui::UpdateQueue::instance().drain(); // the acceptance retires the marking
    CHECK(changed == 1);
    afc.set_event_callback(nullptr);
    afc.fire_status({{"AFC", {{"current_state", "Idle"}, {"error_state", false}}}});
    CHECK_FALSE(afc.get_slot_info(2).error.has_value());
}

TEST_CASE("AFC drops the console watch when the backend stops mid-reset",
          "[ams][afc][recovery][stale_fault]") {
    StaleFaultRig rig;
    REQUIRE(rig.helper.recover_lane_position(2).success());
    REQUIRE(rig.console_watched());

    rig.helper.stop();
    REQUIRE_FALSE(rig.console_watched());
}

TEST_CASE("AFC drains the message queue until it empties", "[ams][afc][recovery]") {
    // printer.AFC.message is a FIFO head; one AFC_CLEAR_MESSAGE pops one entry.
    // A second queued error surfaces only after the first is popped, so the
    // backend must keep clearing while deltas still carry a message.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.clear_fault(0);
    const int after_first = helper.gcode_count("AFC_CLEAR_MESSAGE");
    REQUIRE(after_first == 1);

    // Delta still carries a message: the next queue entry surfaced. Drain again.
    helper.test_parse_afc_state(nlohmann::json{
        {"message",
         {{"message", "Hub is already clear while trying to reset 'lane2'"}, {"type", "error"}}}});
    helper.test_maybe_drain_message_queue();
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 2);

    // Queue now empty: stop. No further clears.
    helper.test_parse_afc_state(nlohmann::json{{"message", {{"message", ""}, {"type", ""}}}});
    helper.test_maybe_drain_message_queue();
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 2);
}

TEST_CASE("AFC drains a session's worth of accumulated messages", "[ams][afc][recovery]") {
    // AFC's message_queue only ever grows on its own: one entry per
    // AFC_logger.error()/.warning() call, and neither reset_failure() nor
    // AFC_RESUME pops anything. A session therefore accumulates warnings and
    // resolved errors ahead of the actionable one — the reported case was depth
    // 4 behind a per-print-start SET_AFC_TOOLCHANGES deprecation warning
    // (#1186). The stopping condition must be an empty queue, not a fixed
    // count: whatever a short budget truncates survives to become the next
    // session's stale error.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    // Entry 0 is the head clear_fault() pops; the rest surface one at a time.
    const std::vector<std::string> queued = {
        "SET_AFC_TOOLCHANGES is deprecated",
        "lane1 Current lane not loaded, LOAD TRIGGER NOT TRIGGERED\n"
        "||==>--||----||-----||\nTRG   LOAD   HUB   TOOL",
        "'lane1' failed to reset to hub, load switch became false during reset",
        "'lane2' failed to reset to hub, load switch became false during reset",
        "lane3 filament failed to trigger pre extruder gear toolhead sensor",
    };

    helper.clear_fault(0);
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 1); // popped queued[0]

    // Each pop exposes the next head. Four deltas, four more clears.
    for (size_t i = 1; i < queued.size(); ++i) {
        helper.test_parse_afc_state(
            nlohmann::json{{"message", {{"message", queued[i]}, {"type", "error"}}}});
        helper.test_maybe_drain_message_queue();
    }
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == static_cast<int>(queued.size()));

    // Queue empty: stop, and stay stopped.
    helper.test_parse_afc_state(nlohmann::json{{"message", {{"message", ""}, {"type", ""}}}});
    helper.test_maybe_drain_message_queue();
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == static_cast<int>(queued.size()));
}

TEST_CASE("AFC message drain is bounded", "[ams][afc][recovery]") {
    // A fault that re-enqueues as fast as we pop must not spin forever.
    // kMessageDrainMaxClears is the runaway guard, not the expected exit — the
    // preceding test covers the normal drain-until-empty path.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.clear_fault(0);
    for (int i = 0; i < 50; ++i) {
        helper.test_parse_afc_state(
            nlohmann::json{{"message", {{"message", "still failing"}, {"type", "error"}}}});
        helper.test_maybe_drain_message_queue();
    }

    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") ==
            AmsBackendAfcTestHelper::kMessageDrainMaxClears);
}

TEST_CASE("AFC clear_fault discards queued lane ejects", "[ams][afc][recovery]") {
    // cancel() flushes pending_eject_lanes_ before its IDLE check. clear_fault()
    // replaced cancel() at the error-modal dismiss site, so it must flush too —
    // otherwise dismissing an error leaves a LANE_UNLOAD chain queued to run.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.test_queue_pending_eject("lane2");
    helper.test_queue_pending_eject("lane3");
    REQUIRE(helper.test_pending_eject_count() == 2);

    helper.clear_fault(0);

    REQUIRE(helper.test_pending_eject_count() == 0);
}

TEST_CASE("AFC drain does not fire without a preceding clear_fault", "[ams][afc][recovery]") {
    // Messages arrive constantly in normal operation. Only an explicit
    // clear_fault() arms the drain; otherwise the UI just displays them.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.test_parse_afc_state(
        nlohmann::json{{"message", {{"message", "Lane 1 loaded"}, {"type", ""}}}});
    helper.test_maybe_drain_message_queue();

    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 0);
}

TEST_CASE("AFC drain arm expires so a stale budget cannot eat a later unrelated error",
          "[ams][afc][recovery]") {
    // Pressing Reset when AFC.message is already empty leaves message_drain_budget_
    // armed. printer.AFC.message is a delta field, so if the queue was already
    // empty at clear_fault() time, no later delta will ever carry a `message` key
    // at all — the empty-message disarm in apply_afc_state() never fires, and
    // without a wall-clock bound the budget would sit armed until a genuinely new,
    // unrelated error rolls in and gets silently acknowledged. The deadline is what
    // actually bounds the arm's lifetime.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.clear_fault(0);
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 1);

    // Simulate the window having passed with no intervening deltas.
    helper.set_message_drain_deadline_offset(std::chrono::seconds(-1));

    // A brand-new, unrelated error arrives. It must surface to the user, not be
    // silently popped by the stale residual budget.
    helper.test_parse_afc_state(
        nlohmann::json{{"message", {{"message", "filament jam detected"}, {"type", "error"}}}});
    helper.test_maybe_drain_message_queue();

    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 1);
}
