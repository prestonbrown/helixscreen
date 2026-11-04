// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_mount_feed.cpp
 * @brief Loading and unloading through the mounted tool (MedusaHC): the stage
 *        plan, the gcode each stage sends, the operator prompt, the carriage
 *        restore, the printer's own macros, and the print gate.
 */

#include "ui_ams_device_operations_overlay.h"
#include "ui_batch_filament_modal.h"

#include "../busy_guard_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "app_globals.h"
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

namespace {

mf::Request load_request() {
    mf::Request r;
    r.load = true;
    r.heads = {{/*head=*/2, /*temp_c=*/215, /*mount_gcode=*/"SELECT_TOOL TOOL=\"tool T2\""}};
    r.restore_gcode = "SELECT_TOOL TOOL=\"tool T0\"";
    r.prompt_title = "Load filament";
    r.prompt_text = "Insert filament into Tool 3, then tap Continue";
    r.continue_label = "Continue";
    r.cancel_label = "Cancel";
    return r;
}

/// A Load of tools 1 and 3 (0-based) with tool 0 on the carriage at the start.
mf::Request multi_load_request() {
    mf::Request r;
    r.load = true;
    r.heads = {{1, 210, "SELECT_TOOL TOOL=\"tool T1\""}, {3, 220, "SELECT_TOOL TOOL=\"tool T3\""}};
    r.restore_gcode = "SELECT_TOOL TOOL=\"tool T0\"";
    r.prompt_title = "Load filament";
    r.prompt_text = "Insert filament into Tools 2, 4 as far as it goes, then tap Continue";
    r.continue_label = "Continue";
    r.cancel_label = "Cancel";
    return r;
}

std::vector<std::string> gcodes(const std::vector<mf::Stage>& stages) {
    std::vector<std::string> out;
    for (const auto& s : stages) {
        out.push_back(s.gcode);
    }
    return out;
}

/// A MedusaHC-shaped changer whose acks the test hands out one at a time.
class AckingChanger : public ToolChangerHelper {
  public:
    explicit AckingChanger(int tools) : ToolChangerHelper(tools) {
        set_mounts_to_feed(true);
    }

    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        sent_.push_back(gcode);
        acks_.push_back(std::move(on_complete));
        errors_.push_back(std::move(on_error));
        return AmsErrorHelper::success();
    }

    /// Ack the newest stage and let the deferred handlers run.
    void ack() {
        REQUIRE_FALSE(acks_.empty());
        auto cb = acks_.back();
        cb();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The newest stage dropped with the connection, as the request tracker
    /// fails everything in flight when the socket or Klippy goes away.
    void lose() {
        REQUIRE_FALSE(errors_.empty());
        errors_.back()(MoonrakerError::connection_lost("printer.gcode.script"));
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The stage sent @p index-th dropped with the connection, after newer
    /// ones went out.
    void lose_at(size_t index) {
        REQUIRE(index < errors_.size());
        errors_[index](MoonrakerError::connection_lost("printer.gcode.script"));
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    void fail(const std::string& message = "simulated") {
        REQUIRE_FALSE(errors_.empty());
        MoonrakerError err;
        err.message = message;
        errors_.back()(err);
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

  private:
    std::vector<helix::SharedCallback<void()>> acks_;
    std::vector<helix::SharedCallback<void(const MoonrakerError&)>> errors_;
};

} // namespace

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
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Load stopped.");
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
    CHECK(load.steps[1].label == "Mount tool");
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
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "No filament was inserted in time.");
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
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Load all stopped. Completed: Tool 2.");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
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
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "No filament was inserted in time.");
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
    CHECK(tc.action_tracks_step_operation(AmsAction::SELECTING));
    CHECK(tc.action_tracks_step_operation(AmsAction::HEATING));
    CHECK(tc.action_tracks_step_operation(AmsAction::LOADING));
    CHECK(tc.action_tracks_step_operation(AmsAction::PURGING));
    CHECK(tc.action_tracks_step_operation(AmsAction::UNLOADING));
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
    CHECK(model.steps[1].label == "Tool 2 (1/2): Mount tool");
    CHECK(model.steps[3].label == "Tool 2 (1/2): Purge");
    CHECK(model.steps[4].label == "Tool 4 (2/2): Mount tool");
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

/// Puts the purge setting back to its default whatever a case leaves behind:
/// SettingsManager and Config outlive every case in this binary.
struct RestorePurgeSetting {
    ~RestorePurgeSetting() {
        helix::SettingsManager::instance().set_mount_feed_purge_mm(mf::kDefaultPurgeMm);
    }
};

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
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Could not turn the heaters off");
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(std::count(tc.sent_.begin(), tc.sent_.end(), cleanup) == 1);
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

namespace {

/// Irbis3D's controller without klipper-toolchanger: a [medusahc] object and
/// the pin_watch dock sensors.
PrinterDiscovery pin_watch_medusahc_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array(
        {"medusahc", "pin_watch io", "extruder", "extruder1", "extruder2", "extruder3"}));
    return hw;
}

json pin_watch_frame(int current_tool) {
    return json{{"pin_watch io", {{"current_tool", current_tool}}}};
}

} // namespace

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

namespace {

/// A tool changer on a real MoonrakerAPI, so a stage's send meets the API's
/// busy gate exactly as it does on a printer.
class LiveApiChanger : public AmsBackendToolChanger {
  public:
    LiveApiChanger(IMoonrakerAPI* api, IMoonrakerClient* client)
        : AmsBackendToolChanger(api, client) {
        set_discovered_tools({"T0", "T1", "T2", "T3"});
        set_mounts_to_feed(true);
        running_ = true;
    }
    ~LiveApiChanger() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    [[nodiscard]] bool toolhead_homed() const override {
        return homed;
    }
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }
    /// Whether every stage may skip the home first.
    bool homed = true;
};

} // namespace

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
    CHECK(tc.get_system_info().operation_detail == "sensor error");
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
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == ToastSeverity::SUCCESS);
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
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == ToastSeverity::SUCCESS);
    }
    SECTION("a stopped unload keeps its error toast") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.unload_filament(1).success());
        tc.ack();  // mounted
        tc.fail(); // the heat failed
        tc.ack();  // the cleanup landed
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Unload stopped.");
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == ToastSeverity::ERROR);
    }
}

// ============================================================================
// The dock sensors confirm every swap the run sends
// ============================================================================

namespace {

/// Irbis3D's controller: a [medusahc] object that publishes sensor_error.
PrinterDiscovery irbis_medusahc_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3",
                                  "medusahc", "pin_watch io", "extruder", "extruder1"}));
    return hw;
}

/// A sensed MedusaHC on tool 0, with a Load of tool 1 at its Mount stage.
void start_load_to_mount(AckingChanger& tc) {
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.sent_.back() == mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T1\"");
}

} // namespace

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
    CHECK(tc.get_system_info().operation_detail == "sensor error");
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

TEST_CASE("mount-to-feed: an abort that cannot trust the carriage leaves it alone",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    run.stage_done(); // the prompt is up
    run.operator_reply(mf::Reply::Continue);
    REQUIRE(run.current_stage() != nullptr);
    CHECK(run.current_stage()->swaps); // the mount
    run.abort(/*restore_carriage=*/false);
    CHECK(run.current_gcode() ==
          std::optional<std::string>("SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0"));
    REQUIRE(run.current_stage() != nullptr);
    CHECK_FALSE(run.current_stage()->swaps);

    mf::Run restoring(load_request());
    restoring.stage_done();
    restoring.operator_reply(mf::Reply::Continue);
    restoring.abort();
    REQUIRE(restoring.current_stage() != nullptr);
    CHECK(restoring.current_stage()->swaps); // its cleanup puts tool 0 back
}

TEST_CASE("MedusaHC words a mixed load's prompt for the mounted tool and the docked ones",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({0, 2}).success());
    CHECK(tc.sent_.back().find("action:prompt_text Insert filament into Tool 1 as far as it "
                               "goes, and into the hot end of Tool 3 while it is docked, then "
                               "tap Continue") != std::string::npos);
}

// ============================================================================
// A carriage nothing can vouch for is never restored
// ============================================================================

TEST_CASE("MedusaHC: a Mount stage that fails sends no restore swap",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    tc.feed(pin_watch_frame(-2)); // the swap stopped partway
    tc.fail();                    // the machine's T1 reported `!!`
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "SELECT_TOOL TOOL=\"tool T0\"") ==
          tc.sent_.end());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Load stopped. The carriage was not put back: check which tool is mounted.");
}

TEST_CASE("MedusaHC: a Cancel mid-swap waits for the swap, then stops without a restore",
          "[toolchanger][mount_feed][medusahc]") {
    const std::string mount = mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T1\"";
    const std::string toast = "Load stopped. The tool change under way finished, and Tool 1 "
                              "was not put back on the carriage.";
    SECTION("acked, waiting on the frame that confirms it") {
        AckingChanger tc(4);
        start_load_to_mount(tc);
        tc.ack();
        REQUIRE(tc.cancel().success());
        // The run stays busy, with nothing sent, while the carriage still moves.
        CHECK(tc.sent_.back() == mount);
        CHECK(tc.get_current_action() == AmsAction::SELECTING); // still the Mount
        CHECK_FALSE(tc.can_cancel_operation());
        CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());

        tc.feed(pin_watch_frame(1)); // the swap settled
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
        CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "SELECT_TOOL TOOL=\"tool T0\"") ==
              tc.sent_.end());
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) == toast);
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == ToastSeverity::WARNING);
        tc.ack();
        CHECK(tc.get_current_action() == AmsAction::IDLE);
    }
    SECTION("the swap's own ack still to come") {
        AckingChanger tc(4);
        start_load_to_mount(tc);
        REQUIRE(tc.cancel().success());
        CHECK(tc.sent_.back() == mount);
        tc.ack();
        CHECK(tc.sent_.back() == mount);
        tc.feed(pin_watch_frame(1));
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) == toast);
    }
    SECTION("a swap that never settles stops on the sensors' fault") {
        AckingChanger tc(4);
        start_load_to_mount(tc);
        tc.ack();
        REQUIRE(tc.cancel().success());
        ToolChangerTestAccess::feed_swap_check_elapsed(tc);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
              "Load stopped. Dock sensors cannot tell which tool is mounted.");
    }
}

TEST_CASE("MedusaHC: losing the connection mid-Mount sends no restore",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    ToolChangerTestAccess::feed_disconnect_stopping(tc);
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "SELECT_TOOL TOOL=\"tool T0\"") ==
          tc.sent_.end());
}

TEST_CASE("MedusaHC: a Cancel after a confirmed Mount still restores the carriage",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    tc.feed(pin_watch_frame(1));
    tc.ack(); // mounted and confirmed; heating
    REQUIRE(tc.cancel().success());
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                             "SELECT_TOOL TOOL=\"tool T0\"");
    // The carriage goes back, and the operator's own Cancel needs no toast.
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());
}

TEST_CASE("MedusaHC: a dock sensor jostled while the operator inserts is no fault",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    const size_t sent_before = tc.sent_.size();
    tc.feed(pin_watch_frame(-2));
    tc.feed(pin_watch_frame(0));
    CHECK(tc.sent_.size() == sent_before);
    CHECK(tc.can_cancel_operation());
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T1\"");
}

TEST_CASE("MedusaHC: a latch left over from before a restart does not fault a later run",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    // The first full frame after a restart: the flag still latched, T2 seated.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}, {"sensor_error", true}}}});
    REQUIRE(tc.get_system_info().action != AmsAction::ERROR);

    // T2 is already mounted, so the run sends no MHC operation that would
    // clear the flag: nothing but the heat and the retract.
    REQUIRE(tc.unload_filament(2).success());
    tc.ack();
    tc.ack();
    REQUIRE(tc.get_current_action() != AmsAction::UNLOADING);
    CHECK(tc.get_system_info().action != AmsAction::ERROR);
    CHECK(tc.unload_blocked_reason(2).empty());
}

TEST_CASE("MedusaHC: a fault the controller reports at the prompt still stops the run",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"current_tool", 0}, {"sensor_error", false}}}});
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    tc.feed(json{{"medusahc", {{"sensor_error", true}}}});
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
}

TEST_CASE("MedusaHC: a sensor fault left over from before a run does not stop the next one",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    ToolChangerTestAccess::set_feed_sensor_fault(tc);
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    tc.feed(json{{"medusahc", {{"operation", "idle"}}}});
    CHECK(tc.can_cancel_operation());
    CHECK(tc.sent_.back().find("action:prompt_show") != std::string::npos);
}

TEST_CASE_METHOD(BusyGuardFixture,
                 "MedusaHC's run is answered while busy even when each stage homes first",
                 "[toolchanger][mount_feed][medusahc]") {
    // A printer that lost homing to idle_timeout sends G28 ahead of every stage,
    // and the stage itself goes out from the G28's answer. The busy-gate bypass
    // has to survive that hop, or a heater-only cleanup is dropped again.
    LiveApiChanger tc(api.get(), &mock_client);
    tc.homed = false;
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});
    begin_blocking_episode();
    const auto drain = []() {
        for (int i = 0; i < 6; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    };

    REQUIRE(tc.load_filament(2).success()); // already mounted: nothing to restore
    drain();                                // homed, then the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.cancel().success()); // stopped while homing for the heat
    drain();                        // homed, then the heater-only cleanup
    const auto& history = mock_client.gcode_script_history();
    REQUIRE_FALSE(history.empty());
    REQUIRE(history.back() == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0");
    CHECK(std::count(history.begin(), history.end(), "G28") >= 3);
    // The cleanup's answer ended the run.
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC: a frame silent about the carriage leaves a standing sensor fault",
          "[toolchanger][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"current_tool", 1}}}});
    tc.feed(json{{"medusahc", {{"current_tool", -2}}}}); // no swap running: a fault
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);

    // Moonraker republishes only what changed: a docks-only frame is not the
    // sensors agreeing again.
    tc.feed(json{{"medusahc", {{"sensors", {{"t0", 1}, {"t2", 1}}}}}});
    CHECK(tc.get_system_info().action == AmsAction::ERROR);
    CHECK(tc.get_system_info().operation_detail == "sensor error");
    CHECK_FALSE(tc.unload_blocked_reason(1).empty());

    tc.feed(json{{"medusahc", {{"current_tool", 1}}}});
    CHECK(tc.get_system_info().action == AmsAction::IDLE);
    CHECK(tc.get_system_info().operation_detail.empty());
    CHECK(tc.unload_blocked_reason(1).empty());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "MedusaHC reads the saved and clamped purge length when it discovers the printer",
                 "[toolchanger][mount_feed][medusahc][purge]") {
    RestorePurgeSetting restore;
    Config* config = Config::get_instance();
    const std::string key = config->df() + helix::wizard::kMountFeedPurgeMm;
    // What a restart does: SettingsManager rebuilt from the saved config.
    const auto reload = [&](int stored) {
        config->set<int>(key, stored);
        helix::SettingsManager::instance().deinit_subjects();
        helix::SettingsManager::instance().init_subjects();
        return helix::SettingsManager::instance().get_mount_feed_purge_mm();
    };
    CHECK(reload(500) == mf::kMaxPurgeMm);
    CHECK(reload(-5) == 0);
    REQUIRE(reload(120) == 120);

    AckingChanger tc(4);
    tc.set_discovery(irbis_medusahc_discovery());
    REQUIRE(tc.max_heads_per_filament_op() == 4); // set_discovery kept mount-to-feed
    const auto actions = tc.get_device_actions();
    const auto slider = std::find_if(actions.begin(), actions.end(),
                                     [](const auto& a) { return a.id == "purge_length"; });
    REQUIRE(slider != actions.end());
    const auto* value = std::any_cast<float>(&slider->current_value);
    REQUIRE(value != nullptr);
    // The slider shows the value the next load sends: the same member feeds
    // both (the setting case above follows it into the gcode).
    CHECK(*value == 120.0f);
}

TEST_CASE("MedusaHC tells a failed swap stage once, not again when the frame repeats it",
          "[toolchanger][mount_feed][medusahc][last_error]") {
    const std::string failure = "MHC_SET: sensors did not confirm T1";
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"current_tool", 0}, {"sensor_error", false}}}});
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.get_current_action() == AmsAction::SELECTING); // the Mount stage

    // The controller refuses the pick; its command_error reaches us first.
    tc.fail(failure);
    const std::string run_toast = ToolChangerTestAccess::feed_last_toast(tc);
    REQUIRE_FALSE(run_toast.empty());
    for (int i = 0; i < 4 && tc.get_current_action() != AmsAction::IDLE; ++i) {
        tc.ack(); // the cleanup
    }
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);

    // Then the status frame latching the same failure, the pick never having
    // left the dock.
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", 0},
                   {"sensor_error", true},
                   {"last_error", failure}}}});
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == run_toast);
}

// ============================================================================
// The run machinery: the wait's clock, a lost printer, a print, a missing
// [respond], the step bar's labels
// ============================================================================

TEST_CASE("MedusaHC's prompt waits its whole ceiling under a watchdog ticking each second",
          "[toolchanger][mount_feed][medusahc]") {
    using namespace std::chrono_literals;
    AckingChanger tc(4);
    std::chrono::steady_clock::time_point now{};
    ToolChangerTestAccess::set_feed_clock(tc, [&now]() { return now; });
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack(); // the prompt is up; the watchdog starts counting here

    for (int i = 0; i < 30; ++i) {
        now += 1s;
        ToolChangerTestAccess::feed_wait_tick(tc);
    }
    CHECK(tc.get_system_info().is_busy());
    CHECK(tc.sent_.size() == 1);

    // Still waiting one second short of the ceiling.
    const auto ceiling = std::chrono::milliseconds(mf::kPromptTimeoutMs);
    while (now < std::chrono::steady_clock::time_point{} + ceiling - 1s) {
        now += 1s;
        ToolChangerTestAccess::feed_wait_tick(tc);
    }
    REQUIRE(tc.sent_.size() == 1);
    now += 1s;
    ToolChangerTestAccess::feed_wait_tick(tc);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "No filament was inserted in time.");
}

TEST_CASE("MedusaHC ends a run on a Klipper restart and turns its heaters off once ready",
          "[toolchanger][mount_feed][medusahc]") {
    SECTION("mid-heat, the restart reported only by klippy_state") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack(); // the prompt is up
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack(); // mounted; the heat is in flight
        const size_t before = tc.sent_.size();

        tc.on_klipper_restarted();
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(tc.can_cancel_operation());
        REQUIRE(tc.sent_.size() == before + 1);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
              "Load stopped. The connection to Klipper was lost. The carriage was not put back: "
              "check which tool is mounted.");

        // A frame later the run stays over: nothing republishes its action.
        tc.feed_status("ready", 0);
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK(tc.sent_.size() == before + 1);
    }
    SECTION("the stage failed with Klippy's disconnect first") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack();
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack(); // mounted; the heat is in flight
        const size_t before = tc.sent_.size();
        tc.lose();
        CHECK(tc.sent_.size() == before); // nothing goes out to a Klippy that is gone
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        tc.on_klipper_restarted();
        REQUIRE(tc.sent_.size() == before + 1);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    }
    SECTION("at the prompt nothing heated, so nothing is owed") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack(); // the prompt is up
        tc.on_klipper_restarted();
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK(tc.sent_.size() == 1);
    }
}

TEST_CASE("MedusaHC: a dropped connection ends the run and turns its heaters off on return",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // tool 1 mounted, tool 3 preheating; the heat is in flight
    const size_t before = tc.sent_.size();

    tc.lose(); // the socket closed under the stage
    CHECK(tc.sent_.size() == before);
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Load all stopped. The connection to Klipper was lost. The carriage was not put back: "
          "check which tool is mounted.");
    tc.on_connection_lost(); // the connection edge arrives after the failed stage
    CHECK(tc.sent_.size() == before);

    tc.feed_status("ready", 1); // reconnected, but no word yet on a print
    CHECK(tc.sent_.size() == before);
    tc.feed(json{{"print_stats", {{"state", "standby"}}}}); // the reconnect's snapshot
    REQUIRE(tc.sent_.size() == before + 1);
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                             "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    tc.feed_status("ready", 1);
    CHECK(tc.sent_.size() == before + 1);
}

TEST_CASE("MedusaHC: a heaters-off dropped by the connection again stays owed",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    tc.lose();
    tc.feed_status("ready", 0);
    const std::string off = "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0";
    REQUIRE(tc.sent_.back() == off);
    tc.lose(); // that send dropped too
    tc.feed_status("ready", 0);
    CHECK(std::count(tc.sent_.begin(), tc.sent_.end(), off) == 2);
}

namespace {

/// An AckingChanger whose printer the test puts a print on.
struct PrintableChanger : AckingChanger {
    explicit PrintableChanger(int tools) : AckingChanger(tools) {}
    bool printing = false;
    bool print_holds_printer() const override {
        return printing;
    }
};

} // namespace

TEST_CASE("MedusaHC: a print that takes over owns the heaters a lost run owed",
          "[toolchanger][mount_feed][medusahc]") {
    PrintableChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    tc.lose();
    const size_t before = tc.sent_.size();

    tc.printing = true; // the reconnect finds a print running
    tc.feed_status("ready", 0);
    CHECK(tc.sent_.size() == before);

    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Tool 4's nozzle is still heating.");

    tc.printing = false; // the print ended: the debt went with it
    tc.feed_status("ready", 0);
    CHECK(tc.sent_.size() == before);
}

TEST_CASE("MedusaHC: a reconnect sends owed heaters-off only once the print state is known",
          "[toolchanger][mount_feed][medusahc]") {
    const auto lose_heating_run = [](PrintableChanger& tc) {
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack();
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack(); // mounted; heating
        tc.lose();
        tc.on_connection_lost();
    };
    SECTION("the snapshot names a print PrinterState has not applied yet") {
        PrintableChanger tc(4);
        lose_heating_run(tc);
        const size_t before = tc.sent_.size();
        tc.feed_status("ready", 0); // the toolchanger's frame lands first
        CHECK(tc.sent_.size() == before);
        // PrinterState still reads idle (printing == false); the frame says otherwise.
        tc.feed(json{{"print_stats", {{"state", "printing"}}}});
        CHECK(tc.sent_.size() == before);
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Tool 4's nozzle is still heating.");
        tc.feed(json{{"print_stats", {{"state", "complete"}}}});
        CHECK(tc.sent_.size() == before); // the debt went with the print
    }
    SECTION("the snapshot says no print") {
        PrintableChanger tc(4);
        lose_heating_run(tc);
        const size_t before = tc.sent_.size();
        tc.feed(json{{"print_stats", {{"state", "standby"}}}});
        REQUIRE(tc.sent_.size() == before + 1);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    }
}

TEST_CASE("MedusaHC: a heaters-off dropped late keeps what a later run came to owe",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    tc.lose();
    tc.feed_status("ready", 0); // the owed heaters-off goes out
    const size_t first_off = tc.sent_.size() - 1;
    REQUIRE(tc.sent_[first_off] == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");

    // A second run starts and is lost before the first heaters-off answers.
    REQUIRE(tc.load_filament(1).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    tc.lose();
    tc.lose_at(first_off); // the first heaters-off dropped with that connection too

    tc.feed_status("ready", 0);
    const std::string& off = tc.sent_.back();
    CHECK(off.find("HEATER=extruder1 TARGET=0") != std::string::npos);
    CHECK(off.find("HEATER=extruder3 TARGET=0") != std::string::npos);
}

TEST_CASE("MedusaHC: a cleanup that only closes the prompt never claims the heaters failed",
          "[toolchanger][mount_feed][medusahc]") {
    // A printer without [respond] refuses every RESPOND line: the prompt
    // stage fails, and so does the cleanup closing it.
    struct NoRespondChanger : AckingChanger {
        explicit NoRespondChanger(int tools) : AckingChanger(tools) {}
        AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                               helix::SharedCallback<void(const MoonrakerError&)> on_error,
                               bool silent) override {
            const bool refused = gcode.find("RESPOND") != std::string::npos;
            auto error = on_error;
            AmsError sent = AckingChanger::execute_gcode(gcode, std::move(on_complete),
                                                         std::move(on_error), silent);
            if (refused && error) {
                MoonrakerError err;
                err.message = "Unknown command:\"RESPOND\"";
                error(err); // deferred: the test drains it
            }
            return sent;
        }
    };
    NoRespondChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Load stopped.");
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
}

namespace {

/// A print already on the printer the moment a run would send its next stage.
void start_printing(BusyGuardFixture& f) {
    helix::test::set_wire_state(f.state, PrintJobState::PRINTING);
}

} // namespace

TEST_CASE_METHOD(BusyGuardFixture, "MedusaHC: a print starting under a run ends it, unsent",
                 "[toolchanger][mount_feed][medusahc]") {
    LiveApiChanger tc(api.get(), &mock_client);
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});
    const auto drain = []() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    };
    REQUIRE(tc.unload_filament_batch({1, 2, 3}).success());
    const auto& history = mock_client.gcode_script_history();
    REQUIRE_FALSE(history.empty());
    // The first stage, the Mount with the next tool's preheat, went out.
    REQUIRE(history.back() == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=220");
    const size_t sent = history.size();

    start_printing(*this);
    drain(); // the Mount's ack arrives with the print running
    CHECK(history.size() == sent);
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Unload all stopped. A print started. The carriage was not put back: check which "
          "tool is mounted. Tool 3's nozzle is still heating.");
}

namespace {

class ConnectionRecordingBackend : public AmsBackendMock {
  public:
    ConnectionRecordingBackend() : AmsBackendMock(4) {}
    int lost = 0;
    void on_connection_lost() override {
        ++lost;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Every backend hears the connection drop, once per drop",
                 "[toolchanger][mount_feed][ams]") {
    get_printer_state().init_subjects(false);
    AmsState::instance().init_subjects(true);
    const auto set_connection = [](ConnectionState state) {
        get_printer_state().set_printer_connection_state(static_cast<int>(state), "");
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };
    set_connection(ConnectionState::CONNECTED);
    auto owned = std::make_unique<ConnectionRecordingBackend>();
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    set_connection(ConnectionState::RECONNECTING);
    CHECK(backend->lost == 1);
    set_connection(ConnectionState::DISCONNECTED); // still down: no second drop
    CHECK(backend->lost == 1);
    set_connection(ConnectionState::CONNECTED);
    CHECK(backend->lost == 1);
    set_connection(ConnectionState::RECONNECTING);
    CHECK(backend->lost == 2);

    AmsState::instance().set_backend(nullptr);
    set_connection(ConnectionState::CONNECTED);
}

namespace {

class BusyBackend : public AmsBackendMock {
  public:
    BusyBackend() : AmsBackendMock(4) {}
    AmsAction action = AmsAction::SELECTING;
    std::optional<StepOperationType> running;
    AmsAction get_current_action() const override {
        return action;
    }
    std::optional<StepOperationType> running_step_operation() const override {
        return running;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "A print start waits only for a filament operation this screen runs",
                 "[toolchanger][mount_feed][ams]") {
    AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<BusyBackend>();
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    // Another client's swap: the printer queues a print behind it itself.
    CHECK_FALSE(AmsState::instance().any_operation_running_here());
    backend->running = StepOperationType::LOAD_FRESH;
    backend->action = AmsAction::HEATING;
    CHECK(AmsState::instance().any_operation_running_here());

    AmsState::instance().set_backend(nullptr);
}

TEST_CASE("MedusaHC offers Cancel exactly while cancel() can act",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    CHECK_FALSE(tc.can_cancel_operation());
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    CHECK(tc.can_cancel_operation());
    REQUIRE(tc.cancel().success());
    // The cleanup is in flight: a second Cancel has nothing left to stop.
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK_FALSE(tc.cancel().success());
}

TEST_CASE_METHOD(LVGLTestFixture, "Abort says Aborting before the backend reports its own result",
                 "[toolchanger][mount_feed][ams]") {
    struct ReportingBackend : BusyBackend {
        std::vector<std::string>* infos = nullptr;
        size_t infos_at_cancel = 0;
        AmsError cancel() override {
            infos_at_cancel = infos->size();
            return AmsErrorHelper::success();
        }
    };
    std::vector<std::string> infos;
    helix::ui::set_test_notification_info_hook(
        [&infos](const std::string& m) { infos.push_back(m); });
    AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<ReportingBackend>();
    owned->infos = &infos;
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    helix::ui::abort_ams_operation();
    CHECK(backend->infos_at_cancel == 1);
    REQUIRE(infos.size() == 1);
    CHECK(infos.front() == "Aborting...");

    AmsState::instance().set_backend(nullptr);
    helix::ui::set_test_notification_info_hook(nullptr);
}

TEST_CASE("MedusaHC remembers which docked hot ends a completed load fed",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    CHECK_FALSE(tc.head_has_filament(2).has_value()); // nothing senses it
    REQUIRE(tc.load_filament(2).success());
    tc.ack(); // the prompt
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack();                                         // mounted
    tc.ack();                                         // heated
    tc.ack();                                         // purged
    CHECK_FALSE(tc.head_has_filament(2).has_value()); // not done until restored
    tc.ack();                                         // tool 0 back on the carriage
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);
    CHECK(tc.head_has_filament(2) == std::optional<bool>(true));
    CHECK_FALSE(tc.head_has_filament(1).has_value());
    // The Unload picker reads the docked tool as loaded, not ready to load.
    CHECK(helix::ui::BatchFilamentModal::row_at_toolhead(tc, 2, 2));
    CHECK_FALSE(helix::ui::BatchFilamentModal::row_at_toolhead(tc, 1, 1));

    REQUIRE(tc.unload_filament(2).success());
    tc.ack(); // mounted
    tc.ack(); // heated
    tc.ack(); // retracted
    tc.ack(); // restored
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.head_has_filament(2).has_value());
}

TEST_CASE("MedusaHC: a new run turns an earlier run's owed heaters off before its own",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack();  // mounted; heating
    tc.lose(); // owed, and no frame has arrived since
    const size_t before = tc.sent_.size();
    REQUIRE(tc.load_filament(1).success());
    REQUIRE(tc.sent_.size() == before + 2);
    CHECK(tc.sent_[before] == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    CHECK(tc.sent_[before + 1].find("action:prompt_begin") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "A backend hears the connection drop after PrinterState's subjects are rebuilt",
                 "[toolchanger][mount_feed][ams]") {
    get_printer_state().init_subjects(false);
    AmsState::instance().init_subjects(true);
    // A soft restart rebuilds PrinterState's subjects; AmsState re-inits on top.
    get_printer_state().deinit_subjects();
    get_printer_state().init_subjects(false);
    AmsState::instance().init_subjects(true);

    const auto set_connection = [](ConnectionState state) {
        get_printer_state().set_printer_connection_state(static_cast<int>(state), "");
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };
    set_connection(ConnectionState::CONNECTED);
    auto owned = std::make_unique<ConnectionRecordingBackend>();
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));
    set_connection(ConnectionState::RECONNECTING);
    CHECK(backend->lost == 1);

    AmsState::instance().set_backend(nullptr);
    set_connection(ConnectionState::CONNECTED);
}
