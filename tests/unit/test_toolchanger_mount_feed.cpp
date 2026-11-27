// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 1 to 53 of test_toolchanger_mount_feed.cpp before it was split, in its order. Mostly
// [medusahc], [general], [batch], [medusahc][batch], [purge], and more

#include "ui_ams_device_operations_overlay.h"
#include "ui_batch_filament_modal.h"

#include "../busy_guard_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/step_operation_helpers.h"
#include "../test_helpers/toolchanger_mount_feed_fixtures.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "../test_helpers/zero_lane_numbering.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "app_globals.h"
#include "cleanup_hold.h"
#include "config.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "filament_op_slot_resolver.h"
#include "macro_param_cache.h"
#include "mount_feed_sequence.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "toolchanger_addon.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace mf = helix::mount_feed;
using helix::test::ToolChangerHelper;
using json = nlohmann::json;

using helix::test::AckingChanger;
using helix::test::ConnectionRecordingBackend;
using helix::test::gcodes;
using helix::test::irbis_medusahc_discovery;
using helix::test::LiveApiChanger;
using helix::test::load_request;
using helix::test::multi_load_request;
using helix::test::pin_watch_frame;
using helix::test::pin_watch_medusahc_discovery;
using helix::test::PrintableChanger;
using helix::test::RestorePurgeSetting;
using helix::test::ScopedGcodeToolStyle;
using helix::test::start_load_to_mount;
using helix::test::start_printing;

// ============================================================================
// The plan
// ============================================================================

TEST_CASE("mount-to-feed: a load prompts at the dock, then mounts, heats, purges and restores",
          "[toolchanger][mount_feed]") {
    const auto stages = mf::plan(load_request());
    // The strand goes into the docked hot end first: once the tool is on the
    // carriage, the extruder's gears stand between the operator and the nozzle.
    REQUIRE(stages.size() == 5);
    CHECK(stages[0].step == mf::Step::Insert);
    CHECK(stages[0].awaits_operator);
    CHECK(stages[0].gcode.find("action:prompt_begin Load filament") != std::string::npos);
    CHECK(stages[0].gcode.find("MSG=HELIX_MOUNT_FEED_CONTINUE|primary") != std::string::npos);
    CHECK(stages[0].gcode.find("MSG=HELIX_MOUNT_FEED_CANCEL|error") != std::string::npos);
    CHECK(stages[1].step == mf::Step::Mount);
    CHECK(stages[1].gcode == mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T2\"");
    CHECK(stages[2].step == mf::Step::Heat);
    CHECK(stages[2].gcode == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=215\nTEMPERATURE_WAIT "
                             "SENSOR=extruder2 MINIMUM=213");
    CHECK(stages[3].step == mf::Step::Purge);
    CHECK(stages[3].gcode == "M83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 "
                             "F1800\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0");
    CHECK(stages[4].step == mf::Step::Restore);
    CHECK(stages[4].gcode == "SELECT_TOOL TOOL=\"tool T0\"");
}

TEST_CASE("mount-to-feed: the tool already on the carriage is neither mounted nor restored",
          "[toolchanger][mount_feed]") {
    mf::Request r = load_request();
    r.heads[0].mount_gcode.clear();
    r.restore_gcode.clear();
    const auto stages = mf::plan(r);
    REQUIRE(stages.size() == 3);
    CHECK(stages[0].step == mf::Step::Insert);
    CHECK(stages[1].step == mf::Step::Heat);
    CHECK(stages[1].gcode == mf::prompt_end_gcode() +
                                 "\nSET_HEATER_TEMPERATURE HEATER=extruder2 "
                                 "TARGET=215\nTEMPERATURE_WAIT SENSOR=extruder2 MINIMUM=213");
    CHECK(stages[2].step == mf::Step::Purge);
}

TEST_CASE("mount-to-feed: an unload retracts with the built-in moves",
          "[toolchanger][mount_feed]") {
    mf::Request unload = load_request();
    unload.load = false;
    // No feeder release: the retract alone takes the strand fully out.
    CHECK(
        gcodes(mf::plan(unload)) ==
        std::vector<std::string>{
            "SELECT_TOOL TOOL=\"tool T2\"",
            "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=215\nTEMPERATURE_WAIT SENSOR=extruder2 "
            "MINIMUM=213",
            "M83\nG1 E-25 F1800\nG1 E-150 F1200\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0",
            "SELECT_TOOL TOOL=\"tool T0\""});
}

TEST_CASE("mount-to-feed: prompt text cannot break out of its RESPOND line",
          "[toolchanger][mount_feed]") {
    mf::Request r = load_request();
    r.prompt_text = "a \"quoted\" ; comment | pipe";
    const std::string g = mf::prompt_gcode(r);
    CHECK(g.find("a 'quoted' , comment / pipe") != std::string::npos);
}

TEST_CASE("mount-to-feed: only the button's own echo is a reply", "[toolchanger][mount_feed]") {
    CHECK(mf::parse_reply("// HELIX_MOUNT_FEED_CONTINUE") == mf::Reply::Continue);
    CHECK(mf::parse_reply("// HELIX_MOUNT_FEED_CANCEL ") == mf::Reply::Cancel);
    // The prompt's own button lines carry the marker text too.
    CHECK(mf::parse_reply("// action:prompt_footer_button Continue|RESPOND TYPE=command "
                          "MSG=HELIX_MOUNT_FEED_CONTINUE|primary") == mf::Reply::None);
    CHECK(mf::parse_reply("echo: something") == mf::Reply::None);
}

// ============================================================================
// The run
// ============================================================================

TEST_CASE("mount-to-feed: the run waits on the operator and cleans up on Cancel",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    REQUIRE(run.current_step() == mf::Step::Insert);
    CHECK(run.step_index() == 0);
    run.stage_done(); // the prompt is up
    CHECK(run.awaiting_operator());
    CHECK_FALSE(run.current_gcode().has_value());
    run.stage_done(); // a stray ack moves nothing
    CHECK(run.awaiting_operator());

    run.operator_reply(mf::Reply::Cancel);
    CHECK(run.aborted());
    // Nothing has moved or heated yet: closing the prompt is the whole cleanup.
    // A heater-off would zero a target the user set; a restore would swap a
    // carriage nothing touched.
    CHECK(run.current_gcode() == std::optional<std::string>(mf::prompt_end_gcode()));
    run.stage_done();
    CHECK(run.finished());
}

TEST_CASE("mount-to-feed: a failure turns the rest into heater off and restore",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    run.stage_done(); // the prompt is up
    run.operator_reply(mf::Reply::Continue);
    run.stage_done(); // mounted
    REQUIRE(run.current_step() == mf::Step::Heat);
    run.abort(); // the heat failed
    CHECK(run.current_gcode() ==
          std::optional<std::string>("SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0\n"
                                     "SELECT_TOOL TOOL=\"tool T0\""));
    run.abort(); // a failure inside the cleanup ends the run
    CHECK(run.finished());
}

// ============================================================================
// The backend
// ============================================================================

TEST_CASE("MedusaHC loads through the carriage and puts the old tool back",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.get_current_slot() == 0);
    CHECK(tc.head_filament_ops(2).load == FilamentOpSupport::Automatic);
    CHECK(tc.head_filament_ops(2).unload == FilamentOpSupport::Automatic);

    REQUIRE(tc.load_filament(2).success());
    REQUIRE(tc.sent_.size() == 1);
    CHECK(tc.sent_.back().find("action:prompt_show") != std::string::npos);
    CHECK(tc.get_current_action() == AmsAction::LOADING); // the Insert step
    CHECK(tc.get_system_info().operation_phase == 0);
    tc.ack();
    const size_t before_reply = tc.sent_.size();
    CHECK(tc.get_system_info().is_busy()); // waiting on the operator
    CHECK(tc.can_cancel_operation());

    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.sent_.size() == before_reply + 1);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T2\"");
    CHECK(tc.get_current_action() == AmsAction::SELECTING); // the Mount step
    CHECK(tc.get_system_info().operation_phase == 1);

    // A swap's own frames must not end the load or move its step.
    tc.feed_status("ready", 2);
    CHECK(tc.get_system_info().is_busy());
    CHECK(tc.get_system_info().operation_phase == 1);

    tc.ack();
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=220\nTEMPERATURE_WAIT "
                             "SENSOR=extruder2 MINIMUM=218"); // no material named: the default
    CHECK(tc.get_current_action() == AmsAction::HEATING);
    tc.ack();
    CHECK(tc.sent_.back().find("G1 E50 F300") != std::string::npos);
    CHECK(tc.get_current_action() == AmsAction::PURGING);
    tc.ack();
    CHECK(tc.sent_.back() == "SELECT_TOOL TOOL=\"tool T0\"");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
    // A finished load needs nothing more from the operator.
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());
}

TEST_CASE("MedusaHC unloads by retracting fully, sends no OPEN, and parks when nothing was "
          "mounted",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_feeder(toolchanger_addon::Feeder{true, "MedusaHC", "OPEN", "CLOSE"});
    tc.feed_status("ready", -1);

    REQUIRE(tc.unload_filament(1).success());
    CHECK(tc.sent_.back() == "SELECT_TOOL TOOL=\"tool T1\"");
    tc.ack();
    tc.ack();
    CHECK(tc.sent_.back() ==
          "M83\nG1 E-25 F1800\nG1 E-150 F1200\nSET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    tc.ack();
    CHECK(tc.sent_.back() == "UNSELECT_TOOL");
    CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "OPEN") == tc.sent_.end());
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC beside klipper-toolchanger mounts and parks with its own commands",
          "[toolchanger][mount_feed][medusahc][commands]") {
    // The Mount and Restore stages are the same swap the Tool buttons send.
    PrinterDiscovery hw;
    hw.parse_objects(
        json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3", "pin_watch io",
                     "medusahc", "gcode_macro DROP_TOOL", "extruder"}));
    AckingChanger tc(4);
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
    tc.feed_status("ready", -1);

    REQUIRE(tc.unload_filament(1).success());
    CHECK(tc.sent_.back() == "T1");
    tc.ack();
    tc.ack();
    tc.ack();
    CHECK(tc.sent_.back() == "DROP_TOOL");
    for (const auto& g : tc.sent_) {
        CHECK(g.find("SELECT_TOOL") == std::string::npos);
    }
}

TEST_CASE("MedusaHC never runs the printer's own LOAD_FILAMENT or UNLOAD_FILAMENT",
          "[toolchanger][mount_feed][medusahc][macro]") {
    // Shaped like a real printer's custom macros: both take T=, which is no
    // contract to call into.
    auto& cache = MacroParamCache::instance();
    cache.populate_from_configfile(
        json{{"gcode_macro load_filament",
              {{"gcode", "{% set t = params.T|int %}{% set temp = params.TEMP|int %}M109"}}},
             {"gcode_macro unload_filament", {{"gcode", "{% set t = params.T|int %}M104 S0"}}}},
        std::unordered_set<std::string>{"LOAD_FILAMENT", "UNLOAD_FILAMENT"});
    struct ClearCache {
        ~ClearCache() {
            MacroParamCache::instance().clear();
        }
    } clear_cache;

    const auto never_named = [](const std::vector<std::string>& sent) {
        return std::none_of(sent.begin(), sent.end(), [](const std::string& g) {
            return g.find("LOAD_FILAMENT") != std::string::npos;
        });
    };

    AckingChanger tc(4);
    tc.feed_status("ready", 2);
    REQUIRE(tc.load_filament(2).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // the heat
    CHECK(tc.sent_.back().find("G1 E50 F300") != std::string::npos);
    CHECK(never_named(tc.sent_));

    AckingChanger un(4);
    un.feed_status("ready", 2);
    REQUIRE(un.unload_filament(2).success());
    un.ack(); // the heat
    CHECK(un.sent_.back().find("G1 E-150 F1200") != std::string::npos);
    CHECK(never_named(un.sent_));
}

TEST_CASE("MedusaHC cancels a load waiting on the operator",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack(); // the prompt is up
    REQUIRE(tc.cancel().success());
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC refuses a second op while one runs, and cleans up a failed stage",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(1).success());
    CHECK_FALSE(tc.load_filament(2).success());
    CHECK_FALSE(tc.unload_filament(1).success());

    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack();  // mounted
    tc.fail(); // the heat failed
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                             "SELECT_TOOL TOOL=\"tool T0\"");
    // One tool: the failure toast is about the load, not a batch.
    CHECK_FALSE(ToolChangerTestAccess::feed_last_toast(tc).empty());
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC filament ops are refused while a print is running or paused",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    // The backend's print gate and every surface's button gating read this:
    // a paused print is no time to mount another tool.
    REQUIRE(tc.filament_ops_self_home());
    CHECK(helix::print_blocks_filament_op(PrintState::Printing, tc.filament_ops_self_home()));
    CHECK(helix::print_blocks_filament_op(PrintState::Paused, tc.filament_ops_self_home()));
    CHECK_FALSE(helix::print_blocks_filament_op(PrintState::Idle, tc.filament_ops_self_home()));
    // The backend heats the tool it mounts: the UI must not heat the active one.
    CHECK(tc.supports_auto_heat_on_load());
    CHECK(tc.supports_auto_heat_on_unload());
}

TEST_CASE("MedusaHC shows the mount-to-feed steps for a load and an unload",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    const auto load = tc.get_operation_step_model(StepOperationType::LOAD_FRESH);
    REQUIRE(load.steps.size() == 5);
    CHECK(load.steps[0].label == "Insert filament");
    CHECK(load.steps[1].label == "Pick up tool");
    CHECK(load.steps[2].label == "Heat nozzle");
    CHECK(load.steps[3].label == "Purge");
    const auto unload = tc.get_operation_step_model(StepOperationType::UNLOAD);
    REQUIRE(unload.steps.size() == 4);
    CHECK(unload.steps[2].label == "Retract filament");
    CHECK(unload.steps[3].label == "Restore tool");
}

// ============================================================================
// The operator wait
// ============================================================================

TEST_CASE("mount-to-feed: the operator wait times out into the cleanup",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    run.stage_done(); // the prompt is up
    REQUIRE(run.awaiting_operator());

    CHECK(run.advance_wait(1000) == mf::Run::WaitResult::Waiting);
    CHECK(run.advance_wait(1000) == mf::Run::WaitResult::Waiting);
    CHECK(run.awaiting_operator());
    // Three minutes is the ceiling; the wait never outlives it.
    CHECK(run.advance_wait(mf::kPromptTimeoutMs) == mf::Run::WaitResult::Expired);
    CHECK(run.aborted());
    CHECK(run.current_gcode() == std::optional<std::string>(mf::prompt_end_gcode()));
    // A wait that is not running is not expired either.
    CHECK(run.advance_wait(1000) == mf::Run::WaitResult::NotWaiting);
}

TEST_CASE("mount-to-feed: a prompt dismissed without an answer reads as a dismissal",
          "[toolchanger][mount_feed]") {
    // The screen (or another client) closed the prompt: the echo of the
    // prompt_end RESPOND is the only trace, and the operator can no longer
    // answer what is gone.
    CHECK(mf::parse_console("// action:prompt_end") == mf::ConsoleSignal::Dismissed);
    CHECK(mf::parse_console("action:prompt_end") == mf::ConsoleSignal::Dismissed);
    CHECK(mf::may_be_prompt_signal("// action:prompt_end"));
    // The run's own stages send prompt_end too, but by then it is not waiting.
    CHECK(mf::parse_console("// HELIX_MOUNT_FEED_CONTINUE") == mf::ConsoleSignal::Continue);
    CHECK(mf::parse_console("// HELIX_MOUNT_FEED_CANCEL") == mf::ConsoleSignal::Cancel);
    CHECK(mf::parse_console("echo: unrelated") == mf::ConsoleSignal::None);
    CHECK_FALSE(mf::may_be_prompt_signal("echo: unrelated"));
}

// ============================================================================
// Several tools in one run
// ============================================================================

TEST_CASE("mount-to-feed: a load of several tools asks once, then works tool by tool",
          "[toolchanger][mount_feed][batch]") {
    const auto stages = mf::plan(multi_load_request());
    REQUIRE(stages.size() == 8);
    // One prompt up front, naming every tool.
    CHECK(stages[0].step == mf::Step::Insert);
    CHECK(stages[0].awaits_operator);
    CHECK(stages[0].gcode.find("Insert filament into Tools 2, 4 as far as it goes") !=
          std::string::npos);
    // The stage after the reply closes the prompt, mounts the first tool, and
    // heats the second to its own temperature while the first works.
    CHECK(stages[1].step == mf::Step::Mount);
    CHECK(stages[1].gcode == mf::prompt_end_gcode() +
                                 "\nSELECT_TOOL TOOL=\"tool T1\"\n"
                                 "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=220");
    CHECK(stages[2].gcode == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=210\nTEMPERATURE_WAIT "
                             "SENSOR=extruder1 MINIMUM=208");
    CHECK(stages[3].gcode == "M83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 "
                             "F1800\nSET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    // The last tool mounts with nothing left to preheat, and the carriage
    // goes back to the tool the run found on it.
    CHECK(stages[4].gcode == "SELECT_TOOL TOOL=\"tool T3\"");
    CHECK(stages[5].gcode == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=220\nTEMPERATURE_WAIT "
                             "SENSOR=extruder3 MINIMUM=218");
    CHECK(stages[6].gcode == "M83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 "
                             "F1800\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    CHECK(stages[7].gcode == "SELECT_TOOL TOOL=\"tool T0\"");
}

TEST_CASE("mount-to-feed: an unload of several tools preheats the next and restores at the end",
          "[toolchanger][mount_feed][batch]") {
    mf::Request r = multi_load_request();
    r.load = false;
    const auto stages = mf::plan(r);
    REQUIRE(stages.size() == 7);
    // No prompt in the unload direction.
    CHECK(stages[0].step == mf::Step::Mount);
    CHECK(stages[0].gcode ==
          "SELECT_TOOL TOOL=\"tool T1\"\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=220");
    CHECK(stages[1].gcode == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=210\nTEMPERATURE_WAIT "
                             "SENSOR=extruder1 MINIMUM=208");
    CHECK(stages[2].gcode ==
          "M83\nG1 E-25 F1800\nG1 E-150 F1200\nSET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(stages[3].gcode == "SELECT_TOOL TOOL=\"tool T3\"");
    CHECK(stages[4].gcode == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=220\nTEMPERATURE_WAIT "
                             "SENSOR=extruder3 MINIMUM=218");
    CHECK(stages[5].gcode ==
          "M83\nG1 E-25 F1800\nG1 E-150 F1200\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    CHECK(stages[6].gcode == "SELECT_TOOL TOOL=\"tool T0\"");
}

TEST_CASE("mount-to-feed: a multi-tool run's step index walks only the rows it runs",
          "[toolchanger][mount_feed][batch]") {
    mf::Run run(multi_load_request());
    REQUIRE(run.phase_index() == 0); // the prompt reads as tool 1's insert
    run.stage_done();
    REQUIRE(run.awaiting_operator());
    run.operator_reply(mf::Reply::Continue);
    CHECK(run.phase_index() == 1); // tool 1: mount
    run.stage_done();
    CHECK(run.phase_index() == 2); // tool 1: heat
    run.stage_done();
    CHECK(run.phase_index() == 3); // tool 1: purge
    run.stage_done();
    CHECK(run.phase_index() == 4); // tool 2: mount, with no restore or insert between
    run.stage_done();
    CHECK(run.completed_heads() == std::vector<int>{1}); // tool 1's work is done
    run.stage_done();
    run.stage_done();
    CHECK(run.phase_index() == 7); // restore belongs to the last tool
    // The restore in flight counts the tool it is putting back as complete:
    // its filament work is done.
    CHECK(run.completed_heads() == std::vector<int>({1, 3}));
    run.stage_done();
    CHECK(run.finished());
}

TEST_CASE("mount-to-feed: a multi-tool stop turns every heater in the plan off",
          "[toolchanger][mount_feed][batch]") {
    mf::Run run(multi_load_request());
    run.stage_done();
    run.operator_reply(mf::Reply::Continue);
    run.stage_done();
    run.stage_done();
    run.stage_done(); // tool 1 purged
    CHECK(run.completed_heads() == std::vector<int>{1});
    run.abort(); // tool 2's heat failed
    CHECK(run.aborted());
    CHECK(run.current_gcode() ==
          std::optional<std::string>("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                                     "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                     "SELECT_TOOL TOOL=\"tool T0\""));
}

TEST_CASE("mount-to-feed: a run knows which heaters its acked stages left set",
          "[toolchanger][mount_feed][batch]") {
    mf::Run run(multi_load_request());
    run.stage_done(); // the prompt
    CHECK(run.heads_left_heating().empty());
    run.operator_reply(mf::Reply::Continue);
    run.stage_done(); // tool 1 mounted, tool 3 preheating
    run.stage_done(); // tool 1 heated
    CHECK(run.heads_left_heating() == std::vector<int>{1, 3});
    run.stage_done(); // tool 1 purged, its heater off
    CHECK(run.heads_left_heating() == std::vector<int>{3});
    run.abort();
    run.stage_done(); // the cleanup turned every heater off
    CHECK(run.heads_left_heating().empty());
}

// ============================================================================
// Load all / Unload all on the backend
// ============================================================================

TEST_CASE("MedusaHC loads several tools in one sequenced run, preheating the next",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    CHECK(tc.max_heads_per_filament_op() == 4);

    REQUIRE(tc.load_filament_batch({1, 3}).success());
    REQUIRE(tc.sent_.size() == 1);
    // One prompt naming both tools, before anything mounts.
    CHECK(tc.sent_.back().find("Insert filament into the hot ends of Tools 2, 4 while they are "
                               "docked, as far as it goes, then tap Continue") !=
          std::string::npos);
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    // The prompt closes on the first mount, which also preheats tool 3 (0-based
    // head 3, at its 220C default material temperature).
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() +
                                 "\nSELECT_TOOL TOOL=\"tool T1\"\n"
                                 "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=220");
    tc.ack();
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=220\nTEMPERATURE_WAIT "
                             "SENSOR=extruder1 MINIMUM=218");
    tc.ack();
    CHECK(tc.sent_.back().find("G1 E50 F300") != std::string::npos);
    tc.ack();
    CHECK(tc.sent_.back() == "SELECT_TOOL TOOL=\"tool T3\"");
    tc.ack();
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=220\nTEMPERATURE_WAIT "
                             "SENSOR=extruder3 MINIMUM=218");
    tc.ack();
    CHECK(tc.sent_.back().find("G1 E50 F300") != std::string::npos);
    tc.ack();
    CHECK(tc.sent_.back() == "SELECT_TOOL TOOL=\"tool T0\"");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC times a multi-tool load out at the prompt, before anything moves",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    tc.ack(); // the prompt is up
    REQUIRE(tc.get_system_info().is_busy());

    ToolChangerTestAccess::feed_wait_elapsed(tc, mf::kPromptTimeoutMs);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
    CHECK_FALSE(ToolChangerTestAccess::feed_last_toast(tc).empty());
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC cancels a multi-tool load mid-sequence and unwinds every heater",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted tool 1 (with tool 3 preheating)
    tc.ack(); // heated
    tc.ack(); // purged
    REQUIRE(tc.sent_.back() == "SELECT_TOOL TOOL=\"tool T3\"");
    REQUIRE(tc.cancel().success());
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                             "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                             "SELECT_TOOL TOOL=\"tool T0\"");
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).find("2") != std::string::npos);
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC words a single-load failure differently from a batch stop",
          "[toolchanger][mount_feed][medusahc][batch]") {
    std::string single_failure;
    {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(1).success());
        tc.ack();
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack();  // mounted
        tc.fail(); // the heat failed
        single_failure = ToolChangerTestAccess::feed_last_toast(tc);
    }
    std::string batch_stop;
    {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament_batch({1, 3}).success());
        tc.ack();
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack(); // mounted tool 1
        tc.ack(); // heated
        tc.ack(); // purged
        REQUIRE(tc.cancel().success());
        batch_stop = ToolChangerTestAccess::feed_last_toast(tc);
    }
    CHECK_FALSE(single_failure.empty());
    CHECK_FALSE(batch_stop.empty());
    CHECK(single_failure != batch_stop);
}

TEST_CASE("MedusaHC counts a dismissed prompt as a Cancel", "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_console_signal(tc, mf::ConsoleSignal::Dismissed);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC times a single-tool load out when nobody inserts",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack(); // the prompt is up
    // A wait inside the ceiling keeps waiting.
    ToolChangerTestAccess::feed_wait_elapsed(tc, mf::kPromptTimeoutMs - 1000);
    CHECK(tc.get_system_info().is_busy());
    CHECK(tc.sent_.size() == 1);
    ToolChangerTestAccess::feed_wait_elapsed(tc, 1000);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
    // The toast claims nothing about heaters or the carriage: neither was touched.
    CHECK_FALSE(ToolChangerTestAccess::feed_last_toast(tc).empty());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).find("eater") == std::string::npos);
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC sequences a full selection itself even with all-macros defined",
          "[toolchanger][mount_feed][medusahc][batch][macro]") {
    auto& cache = MacroParamCache::instance();
    cache.populate_from_configfile(
        json{{"gcode_macro load_all_filament", {{"gcode", "{% if params.TEMP %}M109{% endif %}"}}},
             {"gcode_macro unload_all_filament", {{"gcode", "M104 S0"}}}},
        std::unordered_set<std::string>{"LOAD_ALL_FILAMENT", "UNLOAD_ALL_FILAMENT"});
    struct ClearCache {
        ~ClearCache() {
            MacroParamCache::instance().clear();
        }
    } clear_cache;

    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({0, 1, 2, 3}).success());
    CHECK(tc.sent_.back().find("action:prompt_begin") != std::string::npos);
    CHECK(tc.sent_.back().find("Tools 2, 3, 4") != std::string::npos);

    // A fresh changer: this fixture drops the two-arg ack, so the action above
    // never resolves and would refuse a second op.
    AckingChanger un(4);
    un.feed_status("ready", -1);
    REQUIRE(un.unload_filament_batch({0, 1, 2, 3}).success());
    CHECK(un.sent_.back().rfind("SELECT_TOOL TOOL=\"tool T0\"", 0) == 0);
    for (const auto* sent : {&tc.sent_, &un.sent_}) {
        CHECK(std::none_of(sent->begin(), sent->end(), [](const std::string& g) {
            return g.find("_ALL_FILAMENT") != std::string::npos;
        }));
    }
}

TEST_CASE("MedusaHC's step bar tracks every filament-op action",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    for (AmsAction action : {AmsAction::SELECTING, AmsAction::HEATING, AmsAction::LOADING,
                             AmsAction::PURGING, AmsAction::UNLOADING}) {
        CHECK(tc.action_tracks_step_operation(action));
    }
    CHECK_FALSE(tc.action_tracks_step_operation(AmsAction::IDLE));
    CHECK_FALSE(tc.action_tracks_step_operation(AmsAction::ERROR));
}

TEST_CASE("MedusaHC's step model spans every tool of a multi-tool run",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    const auto model = tc.get_operation_step_model(StepOperationType::LOAD_FRESH);
    // One prompt covers every strand and one restore ends the run, so only the
    // first tool has an insert row and only the last has a restore row.
    REQUIRE(model.steps.size() == 8);
    CHECK(model.steps[0].label == "Tool 2 (1/2): Insert filament");
    CHECK(model.steps[1].label == "Tool 2 (1/2): Pick up tool");
    CHECK(model.steps[3].label == "Tool 2 (1/2): Purge");
    CHECK(model.steps[4].label == "Tool 4 (2/2): Pick up tool");
    CHECK(model.steps[7].label == "Tool 4 (2/2): Restore tool");
    for (const auto& step : model.steps) {
        CHECK(step.label.find("Tool 4 (2/2): Insert") == std::string::npos);
        CHECK(step.label.find("Tool 2 (1/2): Restore") == std::string::npos);
    }
    // The first tool may already be on the carriage, and the carriage may need
    // no putting back.
    CHECK_FALSE(model.steps[0].optional);
    CHECK(model.steps[1].optional);
    CHECK_FALSE(model.steps[4].optional);
    CHECK(model.steps[7].optional);
    // Every heat row shows its own live temperature.
    CHECK(model.steps[2].live_temp);
    CHECK(model.steps[5].live_temp);
    // The phase the run publishes lands inside this model.
    tc.ack(); // prompt up -> phase 0
    CHECK(tc.get_system_info().operation_phase == 0);
}

// ============================================================================
// Review fixes
// ============================================================================

TEST_CASE("mount-to-feed: an abort before the first mount never touches the previous tool's "
          "heater",
          "[toolchanger][mount_feed]") {
    // The run is stopped while the carriage still carries the tool that was on
    // it, whose heater may hold a target its user set. The cleanup turns the
    // RUN's heaters off by name; a blanket M104 S0 would zero that target.
    mf::Run run(load_request());
    run.stage_done(); // the prompt is up
    run.operator_reply(mf::Reply::Continue);
    REQUIRE(run.current_step() == mf::Step::Mount);
    run.abort();
    const std::string cleanup = run.current_gcode().value_or("");
    CHECK(cleanup ==
          "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0\nSELECT_TOOL TOOL=\"tool T0\"");
    CHECK(cleanup.find("M104") == std::string::npos);
}

TEST_CASE("mount-to-feed: the next tool's preheat never lowers a hotter target",
          "[toolchanger][mount_feed][batch]") {
    // Tool 3 idles at 230C (its user set it, or a job just used it), above its
    // 220C load temperature. Sending 220 would PULL IT DOWN.
    mf::Request r = multi_load_request();
    r.heads[1].heater_target_c = 230;
    const auto stages = mf::plan(r);
    REQUIRE(stages.size() == 8);
    CHECK(stages[1].gcode == mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T1\"");
    CHECK(stages[1].gcode.find("SET_HEATER_TEMPERATURE HEATER=extruder3") == std::string::npos);

    // Already at its load temperature: nothing to send either.
    r.heads[1].heater_target_c = 220;
    CHECK(mf::plan(r)[1].gcode.find("SET_HEATER_TEMPERATURE HEATER=extruder3") ==
          std::string::npos);

    // A cooler idle tool and a cold one both go straight to the full temperature.
    for (int idle : {200, 0}) {
        CAPTURE(idle);
        r.heads[1].heater_target_c = idle;
        CHECK(mf::plan(r)[1].gcode.find("SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=220") !=
              std::string::npos);
    }
}

TEST_CASE("mount-to-feed: a load purges the requested length past the nozzle",
          "[toolchanger][mount_feed][purge]") {
    mf::Request r = load_request();
    // The default purge: 5mm into the gears, 50mm to the nozzle, 50mm through it.
    CHECK(mf::plan(r)[3].gcode == "M83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 "
                                  "F1800\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0");
    r.purge_mm = 80;
    CHECK(mf::plan(r)[3].gcode.find("G1 E50 F300\nG1 E80 F300\nG1 E-10 F1800") !=
          std::string::npos);
    // None: the strand stops at the nozzle.
    const std::string no_purge = "M83\nG1 E5 F60\nG1 E50 F300\nG1 E-10 "
                                 "F1800\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0";
    r.purge_mm = 0;
    CHECK(mf::plan(r)[3].gcode == no_purge);
    // Out of range is clamped, never extruded as asked.
    r.purge_mm = 5000;
    CHECK(mf::plan(r)[3].gcode.find("G1 E200 F300") != std::string::npos);
    r.purge_mm = -20;
    CHECK(mf::plan(r)[3].gcode == no_purge);

    // An unload never purges.
    r.load = false;
    r.purge_mm = 80;
    CHECK(mf::plan(r)[2].gcode.find("E80") == std::string::npos);
}

TEST_CASE("MedusaHC offers the load purge as a setting, and the next load uses it",
          "[toolchanger][mount_feed][medusahc][purge]") {
    RestorePurgeSetting restore;
    AckingChanger tc(4);
    tc.feed_status("ready", 2);

    const auto sections = tc.get_device_sections();
    CHECK(std::any_of(sections.begin(), sections.end(),
                      [](const auto& s) { return s.id == "purge"; }));
    const auto actions = tc.get_device_actions();
    const auto slider = std::find_if(actions.begin(), actions.end(),
                                     [](const auto& a) { return a.id == "purge_length"; });
    REQUIRE(slider != actions.end());
    CHECK(slider->max_value == static_cast<float>(mf::kMaxPurgeMm));

    REQUIRE(tc.execute_device_action("purge_length", std::any(75.0f)).success());
    REQUIRE(tc.load_filament(2).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // the heat
    CHECK(tc.sent_.back().find("G1 E50 F300\nG1 E75 F300") != std::string::npos);

    // The slider reads back what was set.
    const auto after = tc.get_device_actions();
    const auto reread = std::find_if(after.begin(), after.end(),
                                     [](const auto& a) { return a.id == "purge_length"; });
    REQUIRE(reread != after.end());
    const auto* value = std::any_cast<float>(&reread->current_value);
    REQUIRE(value != nullptr);
    CHECK(*value == 75.0f);
    CHECK(helix::SettingsManager::instance().get_mount_feed_purge_mm() == 75);
}

TEST_CASE("A tool changer that does not mount to feed has no purge setting",
          "[toolchanger][purge]") {
    ToolChangerHelper tc(4);
    const auto sections = tc.get_device_sections();
    CHECK(std::none_of(sections.begin(), sections.end(),
                       [](const auto& s) { return s.id == "purge"; }));
    CHECK_FALSE(tc.execute_device_action("purge_length", std::any(75.0f)).success());
}

TEST_CASE("MedusaHC unwinds a live run when the connection is lost or restored",
          "[toolchanger][mount_feed][medusahc]") {
    // The connection drops at the prompt: the run ends, and since nothing
    // heated, nothing is owed once the printer is back.
    {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack(); // the prompt is up
        REQUIRE(tc.get_system_info().is_busy());

        tc.on_connection_lost();
        CHECK(tc.sent_.size() == 1);
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(tc.can_cancel_operation());
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
              "Load stopped. The connection to Klipper was lost.");
        tc.feed_status("ready", 0); // reconnected
        CHECK(tc.sent_.size() == 1);
    }
    // And the stopping path, mid-purge rather than mid-prompt: no prompt_end,
    // every heater in the plan off, carriage back.
    {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack(); // the prompt is up
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack(); // mounted
        ToolChangerTestAccess::feed_disconnect_stopping(tc);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                 "SELECT_TOOL TOOL=\"tool T0\"");
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(tc.can_cancel_operation());
    }
}

TEST_CASE("MedusaHC ends the run and says so when the cleanup itself cannot be sent",
          "[toolchanger][mount_feed][medusahc]") {
    // A feed stage's send fails synchronously only when the connection cannot
    // carry it; the stock helper's 4-arg override always succeeds, so the
    // cleanup's dispatch fails through a local flag.
    struct FailingCleanupChanger : AckingChanger {
        bool fail_sends = false;
        explicit FailingCleanupChanger(int tools) : AckingChanger(tools) {}
        AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                               helix::SharedCallback<void(const MoonrakerError&)> on_error,
                               bool silent) override {
            if (fail_sends) {
                sent_.push_back(gcode);
                return AmsErrorHelper::command_failed(gcode, "simulated send failure");
            }
            return AckingChanger::execute_gcode(gcode, std::move(on_complete), std::move(on_error),
                                                silent);
        }
    };
    FailingCleanupChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; the heat stage is in flight
    tc.fail_sends = true;
    tc.fail(); // the heat failed, and the cleanup cannot leave either

    // The cleanup was attempted, once - and the run is over, not stuck busy
    // waiting for an ack that cannot arrive.
    const std::string cleanup = "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                "SELECT_TOOL TOOL=\"tool T0\"";
    REQUIRE(tc.sent_.back() == cleanup);
    CHECK_FALSE(ToolChangerTestAccess::feed_last_toast(tc).empty());
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(std::count(tc.sent_.begin(), tc.sent_.end(), cleanup) == 1);
}

TEST_CASE("a mount-to-feed run holds its heaters once it has started",
          "[toolchanger][mount_feed][sigterm]") {
    using helix::CleanupHold;
    auto tc = std::make_unique<AckingChanger>(4);
    tc->feed_status("ready", 0);
    REQUIRE_FALSE(CleanupHold::any_held_signal_safe());
    REQUIRE(tc->load_filament(3).success());
    tc->ack(); // the prompt is up: nothing has heated
    CHECK_FALSE(CleanupHold::any_held_signal_safe());
    ToolChangerTestAccess::feed_operator_reply(*tc, mf::Reply::Continue);
    tc->ack(); // mounted; the heat stage is in flight
    REQUIRE(CleanupHold::any_held_signal_safe());

    SECTION("teardown sends the off, and moves nothing") {
        tc->before_disconnect();
        CHECK(tc->sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
        CHECK(tc->get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(CleanupHold::any_held_signal_safe());
    }
    SECTION("the link drops: nothing can be sent") {
        tc->lose();
        CHECK_FALSE(CleanupHold::any_held_signal_safe());
    }
    SECTION("destroyed") {
        tc.reset();
        CHECK_FALSE(CleanupHold::any_held_signal_safe());
    }
}

TEST_CASE("MedusaHC stops under a live run without deadlocking, and still sends the cleanup",
          "[toolchanger][mount_feed][medusahc]") {
    // Heap-allocated so a deadlocked stop() can be abandoned: the stuck thread
    // holds the backend's mutex for good, so destroying it would hang the run.
    auto* tc = new AckingChanger(4);
    tc->feed_status("ready", 0);
    REQUIRE(tc->load_filament(3).success());
    tc->ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(*tc, mf::Reply::Continue);
    tc->ack(); // mounted; the heat stage is in flight
    REQUIRE(tc->is_running());

    auto stopped = std::make_shared<std::promise<void>>();
    std::future<void> done = stopped->get_future();
    std::thread stopper([tc, stopped]() {
        tc->stop();
        stopped->set_value();
    });
    if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        stopper.detach();
        FAIL("stop() never returned under a live mount-to-feed run");
    }
    stopper.join();

    CHECK_FALSE(tc->is_running());
    CHECK(tc->sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                              "SELECT_TOOL TOOL=\"tool T0\"");
    CHECK(tc->get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc->can_cancel_operation());
    delete tc;
}

TEST_CASE("MedusaHC: the dock sensors' mid-swap -2 under a mount-to-feed run is no fault",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    REQUIRE(tc.unload_blocked_reason(0).empty());

    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.get_current_action() == AmsAction::SELECTING); // the Mount stage

    // Between the head sensor and the dock sensor changing, pin_watch reports
    // -2 for a moment. That is the swap the run itself asked for.
    tc.feed(pin_watch_frame(-2));
    CHECK(tc.unload_blocked_reason(1).empty());
    CHECK(tc.get_current_action() == AmsAction::SELECTING);
    tc.feed(pin_watch_frame(1));

    tc.ack();
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=220\nTEMPERATURE_WAIT "
                             "SENSOR=extruder1 MINIMUM=218");
    CHECK(tc.get_current_action() == AmsAction::HEATING);

    // Outside a swap the same reading still means the sensors lost the tool.
    tc.feed(pin_watch_frame(-2));
    CHECK_FALSE(tc.unload_blocked_reason(1).empty());
}

TEST_CASE_METHOD(BusyGuardFixture, "MedusaHC's run is answered even while the printer reads busy",
                 "[toolchanger][mount_feed][medusahc]") {
    // A swap leaves idle_timeout reading "Printing" for a while. A script made
    // only of heater commands is "discretionary" to the API's busy gate, which
    // then sends it fire-and-forget and drops its answer. The run's cleanup is
    // such a script when there is no carriage to restore: dropped, the run
    // would sit in its cleanup forever.
    LiveApiChanger tc(api.get(), &mock_client);
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});
    begin_blocking_episode();
    const auto drain = []() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    };

    REQUIRE(tc.load_filament(2).success()); // already mounted: nothing to restore
    drain();                                // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.cancel().success()); // stopped while heating
    const auto& history = mock_client.gcode_script_history();
    REQUIRE_FALSE(history.empty());
    REQUIRE(history.back() == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0");

    drain();
    // The cleanup's answer ended the run.
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(BusyGuardFixture,
                 "A backend send outside a mount-to-feed run keeps the API's busy gate",
                 "[toolchanger][mount_feed][medusahc]") {
    // The gate is the API's rule for every other caller (#1108); only the
    // mount-to-feed run steps around it.
    LiveApiChanger tc(api.get(), &mock_client);
    begin_blocking_episode();
    bool answered = false;
    REQUIRE(tc.ensure_homed_then(
                  "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0",
                  [&answered]() { answered = true; },
                  [&answered](const MoonrakerError&) { answered = true; })
                .success());
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    REQUIRE(mock_client.gcode_script_history().back() ==
            "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0");
    CHECK_FALSE(answered);
}

TEST_CASE("MedusaHC: the controller's phase word never relabels a live mount-to-feed run",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.feed(pin_watch_frame(1));
    tc.ack(); // mounted and confirmed; heating
    REQUIRE(tc.get_current_action() == AmsAction::HEATING);

    // The controller's own phase word says it is idle: true of the changer,
    // not of the run, and the run's status line must not read it.
    tc.feed(json{{"medusahc", {{"operation", "idle"}}}});
    CHECK(tc.get_current_action() == AmsAction::HEATING);
    CHECK(tc.get_system_info().operation_detail.empty());
}

TEST_CASE("MedusaHC: the dock sensors losing the tool mid-run stop it without a restore",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.feed(pin_watch_frame(1));
    tc.ack(); // mounted and confirmed; heating
    REQUIRE(tc.get_current_action() == AmsAction::HEATING);

    // No swap is running, so -2 means the sensors lost the tool: the carriage
    // is unknown and no swap may be sent against it.
    tc.feed(pin_watch_frame(-2));
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Load stopped. Dock sensors cannot tell which tool is mounted.");
    tc.ack();
    CHECK_FALSE(tc.can_cancel_operation());
    // The fault stays on show once the run is over.
    CHECK(tc.get_system_info().action == AmsAction::ERROR);
    CHECK(tc.get_system_info().operation_detail == "Sensor Error");
    CHECK_FALSE(tc.unload_blocked_reason(1).empty());
}

TEST_CASE("MedusaHC's load prompt asks for the strand at the docked hot end",
          "[toolchanger][mount_feed][medusahc]") {
    {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(2).success());
        CHECK(tc.sent_.back().find("action:prompt_text Insert filament into the hot end of Tool "
                                   "3 while it is docked, then tap Continue") != std::string::npos);
    }
    // A tool already on the carriage is not docked, and the prompt says so.
    {
        AckingChanger tc(4);
        tc.feed_status("ready", 2);
        REQUIRE(tc.load_filament(2).success());
        CHECK(tc.sent_.back().find("action:prompt_text Insert filament into Tool 3, then tap "
                                   "Continue") != std::string::npos);
    }
}

TEST_CASE("MedusaHC tells the operator to pull the strand once an unload completes",
          "[toolchanger][mount_feed][medusahc]") {
    SECTION("one tool") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.unload_filament(1).success());
        for (int stage = 0; stage < 4; ++stage) { // mount, heat, retract, restore
            REQUIRE(ToolChangerTestAccess::feed_last_toast(tc).empty());
            tc.ack();
        }
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
              "Unload complete. Pull the filament out of Tool 2.");
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) ==
              helix::NoticeSeverity::SUCCESS);
    }
    SECTION("several tools, one toast naming them all") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.unload_filament_batch({1, 3}).success());
        for (int stage = 0; stage < 7; ++stage) {
            REQUIRE(ToolChangerTestAccess::feed_last_toast(tc).empty());
            tc.ack();
        }
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
              "Unload complete. Pull the filament out of Tool 2, Tool 4.");
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) ==
              helix::NoticeSeverity::SUCCESS);
    }
    SECTION("a stopped unload keeps its error toast") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.unload_filament(1).success());
        tc.ack();  // mounted
        tc.fail(); // the heat failed
        tc.ack();  // the cleanup landed
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(ToolChangerTestAccess::feed_last_toast(tc).empty());
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == helix::NoticeSeverity::ERROR);
    }
}

TEST_CASE("MedusaHC: a swap the sensors confirm moves the run on",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    // The ack can beat the frame that reports the finished swap: the run waits
    // for the frame rather than judging the reading it already had.
    tc.ack();
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T1\"");
    tc.feed(pin_watch_frame(1));
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=220\n"
                             "TEMPERATURE_WAIT SENSOR=extruder1 MINIMUM=218");
}

TEST_CASE("MedusaHC: a mount the sensors still cannot place stops the run without a restore",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    tc.feed(pin_watch_frame(-2)); // mid-swap, and it never settles
    tc.ack();
    REQUIRE(tc.sent_.back() == mf::prompt_end_gcode() +
                                   "\nSELECT_TOOL TOOL=\"tool T1\""); // still waiting on the frame
    ToolChangerTestAccess::feed_swap_check_elapsed(tc);
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "SELECT_TOOL TOOL=\"tool T0\"") ==
          tc.sent_.end());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Load stopped. Dock sensors cannot tell which tool is mounted.");
    tc.ack();
    CHECK(tc.get_system_info().action == AmsAction::ERROR);
    CHECK_FALSE(tc.unload_blocked_reason(1).empty());
}

TEST_CASE("MedusaHC: a mount that seated the wrong tool stops the run without a restore",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    tc.feed(pin_watch_frame(2));
    tc.ack();
    ToolChangerTestAccess::feed_swap_check_elapsed(tc);
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Load stopped. Dock sensors cannot tell which tool is mounted.");
    tc.ack();
    CHECK_FALSE(tc.can_cancel_operation());
}

TEST_CASE("MedusaHC: a restore the sensors confirm completes the run",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    tc.feed(pin_watch_frame(1));
    tc.ack(); // mounted
    tc.ack(); // heated
    tc.ack(); // purged
    REQUIRE(tc.sent_.back() == "SELECT_TOOL TOOL=\"tool T0\"");
    tc.feed(pin_watch_frame(1)); // the old tool, still on: not restored yet
    tc.ack();
    CHECK(tc.can_cancel_operation());
    tc.feed(pin_watch_frame(0));
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());
}

TEST_CASE("MedusaHC: a fault the controller reports during a swap still stops the run",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"current_tool", 0}, {"sensor_error", false}}}});
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.get_current_action() == AmsAction::SELECTING); // the Mount stage

    // The flag is the controller's own word: a fault even mid-swap.
    tc.feed(json{{"medusahc", {{"sensor_error", true}}}});
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    tc.ack();
    CHECK(tc.get_system_info().action == AmsAction::ERROR);
    CHECK(tc.get_system_info().operation_detail == "Sensor Error");
}

TEST_CASE("MedusaHC: a cleanup that sends no swap is no cover for a -2",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(1));
    REQUIRE(tc.load_filament(1).success()); // already mounted: nothing to restore
    tc.ack();                               // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.cancel().success());
    REQUIRE(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");

    tc.feed(pin_watch_frame(-2));
    CHECK_FALSE(tc.unload_blocked_reason(1).empty());
}
