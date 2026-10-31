// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_medusahc.cpp
 * @brief klipper-toolchanger init states, and the feeder MedusaHC adds.
 *
 * Two defects in status_to_action(), both found reading viesturz/klipper-toolchanger
 * @main klipper/extras/toolchanger.py:
 *
 *   STATUS_UNINITALIZED = 'uninitialized'
 *   STATUS_INITIALIZING = 'initializing'   <- we never mapped this one
 *
 *   def select_tool(self, gcmd, tool, restore_axis):
 *       if self.status == STATUS_UNINITALIZED and self.initialize_on == INIT_FIRST_USE:
 *           self.initialize(self.detected_tool)
 *       if self.status != STATUS_READY:
 *           raise gcmd.error("Cannot select tool, toolchanger status is %s, ...")
 *
 * 1. 'uninitialized' mapped to RESETTING, which is_busy(). Every tool changer
 *    boots uninitialized, so check_preconditions() refused the first tap on
 *    every klipper-toolchanger printer. On the DEFAULT initialize_on: first-use
 *    that refusal is self-inflicted: the tap is what would have triggered
 *    self.initialize(). We blocked the only thing that clears the state.
 *
 * 2. 'initializing' was unmapped and fell through to IDLE — not busy — while
 *    the toolchanger is homing and moving the carriage for real.
 *
 * MedusaHC (Irbis3D/MedusaHC) ships initialize_on: manual, so its cold boot sits
 * in 'uninitialized' until INITIALIZE_TOOLCHANGER, which is what the Reset
 * button already sends. Its feeder is a [servo my_servo] driven by OPEN/CLOSE.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_state.h"
#include "error_event.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::test::ToolChangerHelper;

namespace {

/// A stock upstream MedusaHC object list — see test_medusahc_detection.cpp.
PrinterDiscovery medusahc_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3",
                                  "pin_watch io", "servo my_servo", "extruder"}));
    return hw;
}

PrinterDiscovery plain_toolchanger_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "extruder"}));
    return hw;
}

} // namespace

// An uninitialized changer refuses a tool tap rather than letting it through.
// On initialize_on: manual (what MedusaHC ships) Klipper rejects the select,
// that rejection only logs, and the optimistic SELECTING dispatch stamped is
// never unwound: is_busy() would refuse every later op until a restart, since
// Moonraker republishes only changed fields. The refusal clears with Reset,
// which sends INITIALIZE_TOOLCHANGER. An initializing changer is moving the
// carriage, so it is busy too. The raw status names the state as detail.
TEST_CASE("A toolchanger's status maps to an action, and the unready ones refuse a tap",
          "[ams][toolchanger][coldstart]") {
    struct Row {
        const char* status;
        int tool;
        AmsAction action;
        bool refuses_tap;
    };
    const Row rows[] = {
        {"uninitialized", -1, AmsAction::RESETTING, true},
        {"initializing", -1, AmsAction::RESETTING, true},
        {"ready", 0, AmsAction::IDLE, false},
        {"changing", 0, AmsAction::SELECTING, false},
        {"error", -1, AmsAction::ERROR, false},
    };
    for (const auto& row : rows) {
        INFO(row.status);
        ToolChangerHelper h(4);
        h.feed_status(row.status, row.tool);
        CHECK(h.get_current_action() == row.action);
        if (row.refuses_tap) {
            CHECK(h.get_system_info().operation_detail == row.status);
            CHECK(h.get_system_info().is_busy());
            CHECK_FALSE(h.change_tool(2).success());
            CHECK(h.sent().empty());
        }
    }
}

// ============================================================================
// Feeder capability
// ============================================================================

TEST_CASE("MedusaHC resolves a feeder", "[ams][toolchanger][feeder]") {
    auto hw = medusahc_discovery();
    auto feeder = helix::toolchanger_addon::resolve_feeder(hw);

    REQUIRE(feeder.present);
    CHECK(feeder.provider_name == "MedusaHC");
    CHECK(feeder.open_gcode == "OPEN");
    CHECK(feeder.close_gcode == "CLOSE");
}

TEST_CASE("A plain tool changer resolves no feeder", "[ams][toolchanger][feeder]") {
    auto hw = plain_toolchanger_discovery();
    auto feeder = helix::toolchanger_addon::resolve_feeder(hw);

    CHECK_FALSE(feeder.present);
    CHECK(feeder.open_gcode.empty());
    CHECK(feeder.close_gcode.empty());
}

TEST_CASE("A tool changer with a feeder exposes open and close actions",
          "[ams][toolchanger][feeder]") {
    ToolChangerHelper h(4);
    h.set_feeder(helix::toolchanger_addon::resolve_feeder(medusahc_discovery()));

    auto actions = h.get_device_actions();
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].id == "open_feeder");
    CHECK(actions[1].id == "close_feeder");
    // A BUTTON with no section renders nowhere in the device overlay.
    CHECK(actions[0].type == helix::printer::ActionType::BUTTON);
    CHECK_FALSE(actions[0].section.empty());
    CHECK_FALSE(h.get_device_sections().empty());
}

TEST_CASE("Feeder actions send the provider's gcode", "[ams][toolchanger][feeder]") {
    ToolChangerHelper h(4);
    h.set_feeder(helix::toolchanger_addon::resolve_feeder(medusahc_discovery()));
    h.feed_status("ready", 0);

    REQUIRE(h.execute_device_action("open_feeder").success());
    REQUIRE(h.execute_device_action("close_feeder").success());

    REQUIRE(h.sent().size() == 2);
    CHECK(h.sent()[0] == "OPEN");
    CHECK(h.sent()[1] == "CLOSE");
}

TEST_CASE("Clean nozzle is offered where the printer has a CLEAN macro",
          "[ams][toolchanger][medusahc][clean]") {
    PrinterDiscovery with_clean;
    with_clean.parse_objects(
        json::array({"pin_watch io", "medusahc", "gcode_macro CLEAN", "extruder", "extruder1"}));
    CHECK(toolchanger_addon::clean_nozzle_gcode(with_clean) == "CLEAN");
    // The controller's MHC_CLEAN is a native command nothing can detect; CLEAN
    // is the [gcode_macro] MHC_macros.cfg wraps it in.
    PrinterDiscovery without_clean;
    without_clean.parse_objects(json::array({"pin_watch io", "medusahc", "extruder", "extruder1"}));
    CHECK(toolchanger_addon::clean_nozzle_gcode(without_clean).empty());
    PrinterDiscovery plain_with_clean;
    plain_with_clean.parse_objects(
        json::array({"toolchanger", "tool T0", "tool T1", "gcode_macro CLEAN", "extruder"}));
    CHECK(toolchanger_addon::clean_nozzle_gcode(plain_with_clean).empty());
}

TEST_CASE("Clean nozzle runs only with a tool on the carriage",
          "[ams][toolchanger][medusahc][clean]") {
    const auto find_clean = [](const std::vector<helix::printer::DeviceAction>& actions) {
        const auto it = std::find_if(actions.begin(), actions.end(),
                                     [](const auto& a) { return a.id == "clean_nozzle"; });
        return it == actions.end() ? std::optional<helix::printer::DeviceAction>{}
                                   : std::optional<helix::printer::DeviceAction>{*it};
    };

    ToolChangerHelper none(4);
    CHECK_FALSE(find_clean(none.get_device_actions()).has_value());

    ToolChangerHelper tc(4);
    tc.set_clean_nozzle_gcode("CLEAN");
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", -1}}}});

    auto parked = find_clean(tc.get_device_actions());
    REQUIRE(parked.has_value());
    CHECK(parked->type == helix::printer::ActionType::BUTTON);
    CHECK(parked->moves_machine); // greyed, and refused, while a print runs
    CHECK_FALSE(parked->section.empty());
    CHECK_FALSE(parked->enabled);
    CHECK_FALSE(parked->disable_reason.empty());
    CHECK_FALSE(tc.execute_device_action("clean_nozzle").success());
    CHECK(tc.sent().empty());

    tc.feed(json{{"medusahc", {{"current_tool", 2}}}});
    auto mounted = find_clean(tc.get_device_actions());
    REQUIRE(mounted.has_value());
    CHECK(mounted->enabled);
    REQUIRE(tc.execute_device_action("clean_nozzle").success());
    REQUIRE(tc.sent().size() == 1);
    CHECK(tc.sent().back() == "CLEAN");
    const auto sections = tc.get_device_sections();
    CHECK(std::any_of(sections.begin(), sections.end(),
                      [&](const auto& s) { return s.id == mounted->section; }));
}

TEST_CASE("The feeder answers the generic feeder questions from its live state",
          "[ams][toolchanger][feeder]") {
    ToolChangerHelper h(4);
    h.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    h.set_feeder(helix::toolchanger_addon::resolve_feeder(medusahc_discovery()));
    h.feed_status("ready", 0);
    REQUIRE(h.supports_feeder_control());

    h.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    CHECK(h.feeder_open());
    h.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    CHECK_FALSE(h.feeder_open());

    REQUIRE(h.set_feeder_open(true).success());
    REQUIRE(h.set_feeder_open(false).success());
    REQUIRE(h.sent().size() == 2);
    CHECK(h.sent()[0] == "OPEN");
    CHECK(h.sent()[1] == "CLOSE");
}

TEST_CASE("A tool changer without a feeder refuses the generic feeder calls",
          "[ams][toolchanger][feeder]") {
    ToolChangerHelper h(4);
    h.feed_status("ready", 0);
    CHECK_FALSE(h.supports_feeder_control());
    CHECK_FALSE(h.feeder_open());
    CHECK_FALSE(h.set_feeder_open(true).success());
    CHECK(h.sent().empty());
}

TEST_CASE("A tool changer without a feeder exposes no device actions",
          "[ams][toolchanger][feeder]") {
    // Every non-MedusaHC klipper-toolchanger printer must be exactly as it was.
    ToolChangerHelper h(4);

    CHECK(h.get_device_actions().empty());
    CHECK(h.get_device_sections().empty());
    CHECK_FALSE(h.execute_device_action("open_feeder").success());
}

TEST_CASE("An unknown device action is refused even with a feeder", "[ams][toolchanger][feeder]") {
    ToolChangerHelper h(4);
    h.set_feeder(helix::toolchanger_addon::resolve_feeder(medusahc_discovery()));

    CHECK_FALSE(h.execute_device_action("purge_everything").success());
    CHECK(h.sent().empty());
}

// ============================================================================
// Dock sensors stamp the slots
//
// refresh_slot_statuses_locked() used to be a two-way split on the carriage
// tool, on the reasoning that "a toolhead is always physically there". With the
// dock sensors that stops being true: a dock reporting empty for a tool that is
// not on the head means the hot end has been taken out of the machine.
// ============================================================================

TEST_CASE("An empty dock stamps its slot EMPTY", "[ams][toolchanger][docks]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", 1},
                   {"sensors", {{"e", 1}, {"t0", 1}, {"t1", 0}, {"t2", 0}, {"t3", 1}}}}}});

    auto info = tc.get_system_info();
    REQUIRE(info.units.size() == 1);
    const auto& slots = info.units[0].slots;
    REQUIRE(slots.size() == 4);
    CHECK(slots[0].status == SlotStatus::AVAILABLE); // docked
    CHECK(slots[1].status == SlotStatus::LOADED);    // on the head
    CHECK(slots[2].status == SlotStatus::EMPTY);     // dock empty, not mounted
    CHECK(slots[3].status == SlotStatus::AVAILABLE);
}

TEST_CASE("The carriage tool stays LOADED even though its dock is empty",
          "[ams][toolchanger][docks]") {
    // The mounted tool's own dock always reads empty — that is where it came
    // from. Stamping it EMPTY would blank the only loaded slot.
    ToolChangerHelper tc(2);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(
        json{{"medusahc", {{"current_tool", 0}, {"sensors", {{"e", 1}, {"t0", 0}, {"t1", 1}}}}}});

    CHECK(tc.get_slot_info(0).status == SlotStatus::LOADED);
    CHECK(tc.get_slot_info(1).status == SlotStatus::AVAILABLE);
}

TEST_CASE("A frame with no dock fields leaves the stamps alone", "[ams][toolchanger][docks]") {
    // Moonraker republishes only changed fields. A later frame that carries just
    // the tool number must not blank the dock knowledge from the frame before.
    ToolChangerHelper tc(3);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc", {{"current_tool", 0}, {"sensors", {{"e", 1}, {"t2", 0}}}}}});
    REQUIRE(tc.get_slot_info(2).status == SlotStatus::EMPTY);

    tc.feed(json{{"medusahc", {{"operation", "idle"}}}});
    CHECK(tc.get_slot_info(2).status == SlotStatus::EMPTY);
}

// ============================================================================
// Which command drives the swap
// ============================================================================

/// Discovery for a changer whose own extra does the swapping: no [toolchanger],
/// tools enumerated off the extruders.
static PrinterDiscovery standalone_medusahc_discovery(int tools) {
    auto objects = json::array({"pin_watch io", "medusahc", "toolhead", "gcode_macro DROP_TOOL"});
    for (int i = 0; i < tools; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
        objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

TEST_CASE("A plain tool changer selects with SELECT_TOOL", "[ams][toolchanger][commands]") {
    ToolChangerHelper tc(4);
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(plain_toolchanger_discovery()));
    tc.feed_status("ready", -1);

    auto err = tc.change_tool(2);
    REQUIRE(err.success());
    REQUIRE_FALSE(tc.sent().empty());
    CHECK(tc.sent().back() == "SELECT_TOOL TOOL=\"tool T2\"");
}

TEST_CASE("A changer without klipper-toolchanger selects with its own macro",
          "[ams][toolchanger][commands]") {
    auto hw = standalone_medusahc_discovery(3);
    ToolChangerHelper tc(3);
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
    // There is no toolchanger object to report "ready"; the extra's own
    // operation field is what settles the action.
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", -1}}}});

    auto err = tc.change_tool(2);
    REQUIRE(err.success());
    REQUIRE_FALSE(tc.sent().empty());
    CHECK(tc.sent().back() == "T2");
}

TEST_CASE("Unmounting uses the machine's own drop command", "[ams][toolchanger][commands]") {
    auto hw = standalone_medusahc_discovery(2);
    ToolChangerHelper tc(2);
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});

    auto err = tc.park_active_tool();
    REQUIRE(err.success());
    REQUIRE_FALSE(tc.sent().empty());
    // DROP_TOOL takes no tool argument: there is only ever one tool on the head.
    CHECK(tc.sent().back() == "DROP_TOOL");
}

// ============================================================================
// Phase vocabulary
// ============================================================================
//
// Irbis3D/MedusaHC-Python-Controller klippy/extras/medusahc.py `operation`:
// idle / changing / dropping / picking. The words name the swap DIRECTION.

TEST_CASE("The operation key names the swap direction", "[ams][toolchanger][medusahc]") {
    auto upstream = toolchanger_addon::read_tool(
        json{{"medusahc", {{"operation", "picking"}, {"current_tool", 1}}}});
    REQUIRE(upstream.has_value());
    CHECK(upstream->operation == "picking");
    CHECK(upstream->phase_names_direction);
}

TEST_CASE("The gripper is read from the controller", "[ams][toolchanger][medusahc]") {
    auto upstream = toolchanger_addon::read_tool(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    REQUIRE(upstream.has_value());
    REQUIRE(upstream->feeder_open.has_value());
    CHECK(*upstream->feeder_open);

    // A machine that never mentions the gripper is not a machine whose gripper
    // is closed: the step bar keys on the difference.
    auto silent = toolchanger_addon::read_tool(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->feeder_open.has_value());
}

TEST_CASE("A swap reported only as 'changing' still reads as busy",
          "[ams][toolchanger][medusahc]") {
    // MHC_SET homes as `changing` before it names a direction, and without
    // klipper-toolchanger there is no [toolchanger] object to set the action.
    auto hw = standalone_medusahc_discovery(3);
    ToolChangerHelper tc(3);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);

    tc.feed(json{{"medusahc", {{"operation", "changing"}, {"current_tool", 0}}}});
    CHECK(tc.get_current_action() != AmsAction::IDLE);

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}}}});
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

// ============================================================================
// Step model
// ============================================================================

namespace {

std::vector<std::string> step_labels(const AmsBackend::OperationStepModel& model) {
    std::vector<std::string> out;
    out.reserve(model.steps.size());
    for (const auto& s : model.steps) {
        out.push_back(s.label);
    }
    return out;
}

} // namespace

TEST_CASE("A controller that names the direction gets the four-step bar",
          "[ams][toolchanger][steps]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});

    CHECK(
        step_labels(tc.get_operation_step_model(StepOperationType::LOAD_SWAP)) ==
        std::vector<std::string>{"Release filament", "Dock tool", "Pick up tool", "Grip filament"});
    // Nothing to dock on a fresh mount, nothing to pick up on an unmount.
    CHECK(step_labels(tc.get_operation_step_model(StepOperationType::LOAD_FRESH)) ==
          std::vector<std::string>{"Release filament", "Pick up tool", "Grip filament"});
    CHECK(step_labels(tc.get_operation_step_model(StepOperationType::UNLOAD)) ==
          std::vector<std::string>{"Release filament", "Dock tool", "Grip filament"});
}

TEST_CASE("A tool changer's steps project SELECTING, and docking projects UNLOADING",
          "[ams][toolchanger][coarse]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});

    const auto swap = tc.get_operation_step_model(StepOperationType::LOAD_SWAP);
    REQUIRE(swap.steps.size() == 4);
    CHECK(swap.action_at(0) == AmsAction::SELECTING); // Release filament
    CHECK(swap.action_at(1) == AmsAction::UNLOADING); // Dock tool
    CHECK(swap.action_at(2) == AmsAction::SELECTING); // Pick up tool
    CHECK(swap.action_at(3) == AmsAction::SELECTING); // Grip filament
}

TEST_CASE("A running swap publishes its step's action, and the closing grip reads IDLE",
          "[ams][toolchanger][coarse]") {
    // The park dispatches as UNLOADING, but its first step releases the
    // filament, which projects SELECTING. The grip that closes the swap lands
    // on the frame that ends it, and a finished swap reads IDLE whatever step
    // index it left behind.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    // The active operation is process-wide; later cases here expect the default.
    struct RestoreStepOperation {
        ~RestoreStepOperation() {
            AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
        }
    } restore_step_operation;
    AmsState::instance().set_active_step_operation(StepOperationType::UNLOAD);
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    REQUIRE(tc.park_active_tool().success());

    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    REQUIRE(tc.get_system_info().operation_phase == 0); // Release filament
    CHECK(tc.get_system_info().action == AmsAction::SELECTING);
    CHECK(tc.get_current_action() == AmsAction::SELECTING);

    tc.feed(json{
        {"medusahc", {{"operation", "dropping"}, {"current_tool", 0}, {"feeder_open", true}}}});
    REQUIRE(tc.get_system_info().operation_phase == 1); // Dock tool
    CHECK(tc.get_system_info().action == AmsAction::UNLOADING);

    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", -1}, {"feeder_open", false}}}});
    REQUIRE(tc.get_system_info().operation_phase == 2); // Grip filament
    CHECK(tc.get_system_info().action == AmsAction::IDLE);
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("Irbis3D's opening 'changing' frame lights the swap's first step",
          "[ams][toolchanger][steps][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    struct RestoreStepOperation {
        ~RestoreStepOperation() {
            AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
        }
    } restore_step_operation;
    AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
    // Latch the direction and the gripper, as a swap's own frames would.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    REQUIRE(tc.change_tool(2).success());

    tc.feed(json{{"medusahc", {{"operation", "changing"}, {"current_tool", 0}}}});
    CHECK(tc.get_system_info().operation_phase == 0); // Release filament
    CHECK(tc.get_system_info().action == AmsAction::SELECTING);
    tc.feed(json{{"medusahc", {{"operation", "dropping"}, {"current_tool", 0}}}});
    CHECK(tc.get_system_info().operation_phase == 1); // Dock tool
}

TEST_CASE("MedusaHC's fault dialog quotes the controller's own failure",
          "[ams][toolchanger][medusahc][last_error]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"operation", "picking"}, {"current_tool", 0}}}});
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", -2},
                   {"sensor_error", true},
                   {"last_error", "MHC_SET: sensors did not confirm T1"}}}});
    const auto err = tc.current_error();
    REQUIRE(err.has_value());
    CHECK(err->detail == "Tool change failed: MHC_SET: sensors did not confirm T1");
    // On screen as a fault: no toast on top of the dialog.
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());

    // A -2 the controller did not report keeps the generic words, even with an
    // older failure's message still latched beside a reading the docks agree on.
    ToolChangerHelper unreported(4);
    unreported.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    unreported.feed(json{{"medusahc",
                          {{"operation", "idle"},
                           {"current_tool", 1},
                           {"sensor_error", true},
                           {"last_error", "MHC_SET: sensors did not confirm T1"}}}});
    REQUIRE(unreported.get_system_info().action != AmsAction::ERROR);
    unreported.feed(json{{"medusahc", {{"current_tool", -2}}}});
    const auto generic = unreported.current_error();
    REQUIRE(generic.has_value());
    CHECK(generic->detail == unreported.unload_blocked_reason(1));
}

TEST_CASE("A last_error with no failed operation behind it is a controller error",
          "[ams][toolchanger][medusahc][last_error]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    // What the controller publishes after a failed initialization.
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", 1},
                   {"target_tool", -1},
                   {"sensor_error", false},
                   {"last_error", "Initialization failed; see klippy.log"}}}});
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Tool changer error: Initialization failed; see klippy.log");
    CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == ToastSeverity::ERROR);
    // Not a fault on the carriage: the docks agree.
    CHECK_FALSE(tc.current_error().has_value());

    // Told once.
    ToolChangerTestAccess::clear_feed_last_toast(tc);
    tc.feed(json{{"medusahc", {{"feeder_open", false}}}});
    tc.feed(json{{"medusahc", {{"current_tool", 2}}}});
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());
}

TEST_CASE("A failed swap names the tool it was changing to",
          "[ams][toolchanger][medusahc][last_error]") {
    const std::string failure = "MHC_SET: sensors did not confirm T1";

    SECTION("in the fault dialog") {
        ToolChangerHelper tc(4);
        tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
        tc.feed(json{{"medusahc", {{"operation", "picking"}, {"current_tool", 0}}}});
        tc.feed(json{{"medusahc",
                      {{"operation", "idle"},
                       {"current_tool", -2},
                       {"target_tool", 1},
                       {"sensor_error", true},
                       {"last_error", failure}}}});
        const auto err = tc.current_error();
        REQUIRE(err.has_value());
        CHECK(err->detail == "Tool change to Tool 2 (T1) failed: " + failure);
    }

    SECTION("in the toast when the docks agree again") {
        ToolChangerHelper tc(4);
        tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
        tc.feed(json{{"medusahc",
                      {{"operation", "idle"},
                       {"current_tool", 0},
                       {"target_tool", 3},
                       {"sensor_error", true},
                       {"last_error", failure}}}});
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
              "Tool change to Tool 4 (T3) failed: " + failure);
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == ToastSeverity::WARNING);
    }

    SECTION("a failed park names the tool it was parking") {
        ToolChangerHelper tc(4);
        tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
        tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
        tc.feed(json{{"medusahc", {{"operation", "dropping"}, {"target_tool", 1}}}});
        tc.feed(json{{"medusahc",
                      {{"operation", "idle"},
                       {"current_tool", -2},
                       {"sensor_error", true},
                       {"last_error", "MHC_DROP: sensors did not confirm the drop"}}}});
        const auto err = tc.current_error();
        REQUIRE(err.has_value());
        CHECK(err->detail ==
              "Parking Tool 2 (T1) failed: MHC_DROP: sensors did not confirm the drop");
    }

    SECTION("a swap that fails while dropping the old tool is still a tool change") {
        ToolChangerHelper tc(4);
        tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
        tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});
        tc.feed(json{{"medusahc", {{"operation", "changing"}, {"target_tool", 1}}}});
        tc.feed(json{{"medusahc", {{"operation", "dropping"}}}});
        tc.feed(json{{"medusahc",
                      {{"operation", "idle"},
                       {"current_tool", -2},
                       {"sensor_error", true},
                       {"last_error", failure}}}});
        const auto err = tc.current_error();
        REQUIRE(err.has_value());
        CHECK(err->detail == "Tool change to Tool 2 (T1) failed: " + failure);
    }

    SECTION("a target past the discovered tools is not named") {
        ToolChangerHelper tc(4);
        tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
        tc.feed(json{{"medusahc",
                      {{"operation", "idle"},
                       {"current_tool", 0},
                       {"target_tool", 7},
                       {"sensor_error", true},
                       {"last_error", failure}}}});
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Tool change failed: " + failure);
    }
}

TEST_CASE("A controller failure the screen is not showing toasts once",
          "[ams][toolchanger][medusahc][last_error]") {
    const std::string failure = "MHC_SET: sensors did not confirm T1";

    SECTION("the docks agree again after the fault") {
        ToolChangerHelper tc(4);
        tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
        tc.feed(json{{"medusahc", {{"operation", "picking"}, {"current_tool", 0}}}});
        tc.feed(json{{"medusahc",
                      {{"operation", "idle"},
                       {"current_tool", -2},
                       {"sensor_error", true},
                       {"last_error", failure}}}});
        REQUIRE(tc.get_system_info().action == AmsAction::ERROR);
        REQUIRE(ToolChangerTestAccess::feed_last_toast(tc).empty());

        tc.feed(json{{"medusahc", {{"current_tool", -1}}}});
        REQUIRE(tc.get_system_info().action == AmsAction::IDLE);
        CHECK(ToolChangerTestAccess::feed_last_toast(tc).find(failure) != std::string::npos);
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == ToastSeverity::WARNING);

        // Further frames under the same latched failure say nothing more.
        ToolChangerTestAccess::clear_feed_last_toast(tc);
        tc.feed(json{{"medusahc", {{"feeder_open", false}}}});
        tc.feed(json{{"medusahc", {{"current_tool", 1}}}});
        CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());

        // The controller's next operation clears it, and a new failure toasts again.
        tc.feed(json{{"medusahc", {{"sensor_error", false}, {"last_error", ""}}}});
        tc.feed(json{{"medusahc",
                      {{"operation", "idle"},
                       {"current_tool", 1},
                       {"sensor_error", true},
                       {"last_error", failure}}}});
        CHECK(ToolChangerTestAccess::feed_last_toast(tc).find(failure) != std::string::npos);
    }

    SECTION("a failure left over from before this connection") {
        ToolChangerHelper tc(4);
        tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
        tc.feed(json{{"medusahc",
                      {{"operation", "idle"},
                       {"current_tool", 2},
                       {"sensor_error", true},
                       {"last_error", failure}}}});
        REQUIRE(tc.get_system_info().action != AmsAction::ERROR);
        CHECK(ToolChangerTestAccess::feed_last_toast(tc).find(failure) != std::string::npos);
    }
}

TEST_CASE("A changer with no phase source shows no bar at all", "[ams][toolchanger][steps]") {
    // Plain klipper-toolchanger: the only signal is
    // toolchanger.status == "changing", which is not a sequence. The
    // legacy Heat/Feed/Purge fallback describes a filament system, so it is
    // suppressed rather than shown.
    ToolChangerHelper tc(2);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(plain_toolchanger_discovery()));
    tc.feed_status("ready", 0);

    const auto model = tc.get_operation_step_model(StepOperationType::LOAD_SWAP);
    CHECK(model.steps.empty());
    CHECK(model.suppressed);
    CHECK(tc.get_operation_step_index_subject(StepOperationType::LOAD_SWAP) == nullptr);
}

TEST_CASE("A resting changer sits on no step at all", "[ams][toolchanger][steps]") {
    // At rest the machine is idle with the gripper closed, which is the SAME
    // wire state as the closing grip at the end of a swap. Reporting the final
    // step here leaves operation_phase pinned at it, and AmsState publishes the
    // action before the phase, so the next swap's bar observes the stale end
    // index the instant it is built and paints itself complete before the
    // carriage moves.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    CHECK(tc.get_system_info().operation_phase < 0);

    // Nor does opening the gripper by hand from the feeder panel invent a step:
    // that is idle-with-the-gripper-open and no operation is running.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    CHECK(tc.get_system_info().operation_phase < 0);
}

TEST_CASE("The gripper separates the two idle ends of a swap", "[ams][toolchanger][steps]") {
    // Both ends of a running swap read idle. Open is the release that OPENS it,
    // closed is the grip that CLOSES it; a gripper-blind machine gets no step
    // for an idle frame at all. Driven through a real dispatch rather than fed
    // frames cold, because "an operation is running" is half of what separates
    // these states from resting.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    REQUIRE(tc.change_tool(2).success());

    // The gap between dispatching and the machine moving: still idle, gripper
    // still closed. This is NOT the closing grip, and claiming the last step
    // here paints the bar complete for the second before anything happens.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    CHECK(tc.get_system_info().operation_phase < 0);

    // LOAD_SWAP's sequence is Release/Dock/Pick/Grip.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    CHECK(tc.get_system_info().operation_phase == 0); // Release filament

    tc.feed(json{
        {"medusahc", {{"operation", "dropping"}, {"current_tool", 0}, {"feeder_open", true}}}});
    CHECK(tc.get_system_info().operation_phase == 1); // Dock tool

    tc.feed(json{
        {"medusahc", {{"operation", "picking"}, {"current_tool", -1}, {"feeder_open", true}}}});
    CHECK(tc.get_system_info().operation_phase == 2); // Pick up tool

    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}, {"feeder_open", false}}}});
    CHECK(tc.get_system_info().operation_phase == 3); // Grip filament
}

TEST_CASE("A phase-less frame leaves the step alone", "[ams][toolchanger][steps]") {
    // Moonraker republishes only what CHANGED, so a delta naming just the tool
    // carries no phase word. Stomping the step to -1 on one of those would drop
    // the bar back to nothing mid-swap.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    REQUIRE(tc.change_tool(2).success());
    tc.feed(json{
        {"medusahc", {{"operation", "dropping"}, {"current_tool", 0}, {"feeder_open", true}}}});
    REQUIRE(tc.get_system_info().operation_phase == 1);

    tc.feed(json{{"medusahc", {{"current_tool", 0}}}});
    CHECK(tc.get_system_info().operation_phase == 1);
}

TEST_CASE("The step index stays pinned to the model actually on screen",
          "[ams][toolchanger][steps]") {
    // get_operation_step_model() and resolve_step_locked() both derive
    // their sequence from tc_step_sequence(), but the model is captured once
    // (by the sidebar, at operation start) while the index is recomputed on
    // EVERY frame from whatever feeder_state_reported_/direction_reported_
    // read right then. A machine whose early frames carry `operation` without
    // `feeder_open` gets a 2-step model (Dock/Pick); before the fix, the
    // moment a later frame reported feeder_open for the FIRST time, the index
    // computation would silently start resolving against the 4-step
    // Release/Dock/Pick/Grip sequence instead - landing "dropping" on index 1
    // (Pick, on the model actually rendered) instead of 0 (Dock).
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);

    // No feeder_open field at all yet: feeder_state_reported_ stays false.
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});
    REQUIRE(tc.change_tool(2).success());

    // The sidebar captures the model at operation start, before feeder_open
    // has ever been reported on this backend.
    const auto model = tc.get_operation_step_model(StepOperationType::LOAD_SWAP);
    REQUIRE(step_labels(model) == std::vector<std::string>{"Dock tool", "Pick up tool"});

    // The FIRST frame to ever report feeder_open arrives mid-operation.
    tc.feed(json{
        {"medusahc", {{"operation", "dropping"}, {"current_tool", 0}, {"feeder_open", true}}}});
    // Against the 2-step model actually on screen, "dropping" is step 0
    // (Dock) - not step 1, which is only where Dock sits in the 4-step
    // sequence this same frame would build if the model were asked for fresh.
    CHECK(tc.get_system_info().operation_phase == 0);
}

TEST_CASE("An idle frame with the gripper open does not end a running swap",
          "[ams][toolchanger][steps]") {
    // A swap RELEASES the filament before it moves, so its first frame is
    // idle-with-the-gripper-open. Reporting IDLE there tore the step bar down on
    // step 0 and brought the action buttons back mid-swap.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);

    REQUIRE(tc.change_tool(2).success());
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    CHECK(tc.get_current_action() != AmsAction::IDLE);

    // But opening the gripper by hand while genuinely idle must NOT read as busy
    // - that is exactly what the feeder panel's Open button does.
    ToolChangerHelper idle_tc(4);
    idle_tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    idle_tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    idle_tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    CHECK(idle_tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("An aborted swap that ends with the head empty does not latch busy",
          "[ams][toolchanger][steps]") {
    // A real firmware unmount can end with the carriage empty and nothing to
    // re-grip, so the gripper stays open on the swap's LAST idle frame too -
    // the other helpers in this file always close it before ending, so this
    // drives the backend through a real dropping frame first. Before the fix,
    // the hold in apply_tool_sensor_locked() re-armed itself forever:
    // was_mid_operation is partly derived from the action the hold itself
    // sets, so every later idle+open frame satisfied the same condition that
    // created it and the backend never reached IDLE again.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // Settle at idle with tool 0 mounted.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);

    // The unmount releases the filament and confirms it is actually running.
    tc.feed(json{
        {"medusahc", {{"operation", "dropping"}, {"current_tool", 0}, {"feeder_open", true}}}});
    REQUIRE(tc.get_current_action() != AmsAction::IDLE);

    // The head ends up empty, and the gripper never re-closes over nothing.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", -1}, {"feeder_open", true}}}});
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.get_system_info().is_busy());

    // A later swap must still get the ordinary hold on ITS OWN opening frame -
    // the fix must not have removed the hold outright, and the bound must have
    // reset rather than staying latched true from the aborted swap above.
    REQUIRE(tc.change_tool(1).success());
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", -1}, {"feeder_open", true}}}});
    CHECK(tc.get_current_action() != AmsAction::IDLE);
}

TEST_CASE("A fresh dispatch gets its own hold even after a stale confirmation",
          "[ams][toolchanger][steps]") {
    // toolchanger.status can settle back to "ready" in a frame that carries no
    // medusahc data at all - parse_toolchanger_state() sets IDLE straight from
    // that, bypassing the idle/ready branch in apply_tool_sensor_locked() that
    // normally resets operation_confirmed_. Left stale at true from an earlier
    // confirmed swap, a LATER dispatch's own opening idle-with-open frame
    // would read "already confirmed" and skip the hold instead of starting
    // one. begin_dispatch_locked() has to re-arm the bound itself for this
    // reason.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // Confirm a swap is running (operation_confirmed_ -> true)...
    tc.feed(json{
        {"medusahc", {{"operation", "dropping"}, {"current_tool", 0}, {"feeder_open", true}}}});
    REQUIRE(tc.get_current_action() != AmsAction::IDLE);

    // ...then let it settle via the OTHER status source, bypassing the reset.
    tc.feed_status("ready", 0);
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);

    // A brand new dispatch must still get the ordinary hold on its own
    // opening frame, not read the stale confirmation as its own.
    REQUIRE(tc.change_tool(1).success());
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    CHECK(tc.get_current_action() != AmsAction::IDLE);
}

TEST_CASE("A tool changer tracks the sidebar's optimistic heat marker",
          "[ams][toolchanger][steps]") {
    // HEATING is a lie about the MACHINE and the truth about the SIDEBAR, which
    // is why it belongs here and why it keeps looking like a mistake to remove.
    // AmsOperationSidebar::start_operation() stamps HEATING the instant a user
    // starts an operation, before any frame has arrived. If the changer answers
    // "not an operation I follow" to it, update_step_progress() takes its
    // not-active branch on our OWN dispatch: it hides the bar and clears
    // target_load_slot_, after which the rest of a user-initiated swap reads as
    // externally started and the bar restarts from step 0.
    //
    // A changer genuinely never heats, so the honest-looking edit is to drop
    // HEATING from this list. That is the regression this test exists to catch.
    ToolChangerHelper tc(4);

    CHECK(tc.action_tracks_step_operation(AmsAction::HEATING));
    CHECK(tc.action_tracks_step_operation(AmsAction::SELECTING));
    CHECK(tc.action_tracks_step_operation(AmsAction::UNLOADING));

    // Everything else stays untracked: a changer moves hot ends, so the
    // filament vocabulary describes nothing it does.
    CHECK_FALSE(tc.action_tracks_step_operation(AmsAction::IDLE));
    CHECK_FALSE(tc.action_tracks_step_operation(AmsAction::LOADING));
    CHECK_FALSE(tc.action_tracks_step_operation(AmsAction::PURGING));
}

TEST_CASE("A fresh dispatch does not inherit the previous swap's gripper latch",
          "[ams][toolchanger][steps]") {
    // feeder_opened_this_operation_ is what tells the grip that ENDS a swap
    // from the closed gripper that merely precedes one. apply_tool_sensor_locked()
    // clears it when an operation ends through the idle branch - but a swap can
    // settle via toolchanger.status alone and never reach that branch, leaving
    // the latch set. The next dispatch's first closed-gripper frame would then
    // resolve to the FINAL step, painting the bar complete before the carriage
    // has moved: the very failure the latch exists to prevent.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));
    AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);

    // A swap that actually opens the gripper, so the latch is genuinely set.
    tc.feed(json{
        {"medusahc", {{"operation", "dropping"}, {"current_tool", 0}, {"feeder_open", true}}}});
    REQUIRE(tc.get_current_action() != AmsAction::IDLE);

    // Settled through the OTHER status source, which never clears the latch.
    tc.feed_status("ready", 0);
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);

    // New dispatch, then the gap before the machine moves: idle, gripper still
    // closed. There is no step to report here - not the last one.
    REQUIRE(tc.change_tool(1).success());
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    CHECK(tc.get_system_info().operation_phase == -1);
}

// ============================================================================
// Wording and refusals
// ============================================================================

TEST_CASE("Discovery makes a MedusaHC changer load and unload through the carriage",
          "[ams][toolchanger][labels][mount_feed]") {
    // One extruder drive serves every tool, so a load mounts the tool first;
    // to the UI it is simply an automatic Load and Unload.
    ToolChangerHelper tc(2);
    tc.set_discovery(medusahc_discovery());
    const auto ops = tc.head_filament_ops(0);
    CHECK(ops.load == FilamentOpSupport::Automatic);
    CHECK(ops.unload == FilamentOpSupport::Automatic);
    CHECK(tc.carriage_can_be_empty());
    CHECK(tc.load_feeds_filament());

    // A plain klipper-toolchanger keeps filament by hand.
    ToolChangerHelper plain(2);
    CHECK(plain.head_filament_ops(0).load == FilamentOpSupport::Manual);
}

TEST_CASE("A blocked unmount explains itself only when it can", "[ams][toolchanger][labels]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // Nothing mounted needs no explanation: there is visibly nothing to unmount.
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", -1}}}});
    CHECK(tc.unload_blocked_reason(0).empty());

    // -2 is the sensors saying they cannot tell.
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", -2}}}});
    CHECK_FALSE(tc.unload_blocked_reason(0).empty());
}

TEST_CASE("Sensors losing track of a MOUNTED tool withdraws Unmount",
          "[ams][toolchanger][labels]") {
    // The realistic fault, and the one the -1 -> -2 ordering above misses: a
    // tool is mounted and THEN the docks lose track. The sensor-error path holds
    // the last known tool on purpose, so current_slot stays >= 0 right through
    // it. Anything keyed on current_slot < 0 never fires here, and Unmount stays
    // ENABLED against a carriage state nobody can vouch for.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    REQUIRE(tc.can_unload_from_toolhead(1));
    REQUIRE(tc.unload_blocked_reason(1).empty());

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", -2}}}});
    CHECK_FALSE(tc.unload_blocked_reason(1).empty());
    CHECK_FALSE(tc.can_unload_from_toolhead(1));

    // And it clears when the sensors recover.
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    CHECK(tc.unload_blocked_reason(1).empty());
    CHECK(tc.can_unload_from_toolhead(1));
}

TEST_CASE("A dock-sensor fault reaches the LIVE half of the loaded signal too",
          "[ams][toolchanger][labels]") {
    // can_unload_from_toolhead() is only the snapshot half. AmsContextMenu ORs
    // it with the live pair — slot_is_actively_loaded() ||
    // slot_has_filament_at_toolhead() — so a withdrawal that lives in the
    // snapshot alone is handed straight back by the live one and the button
    // never greys. The base slot_is_actively_loaded() rule cannot see the fault:
    // it reads current_slot + filament_loaded, and the sensor-error path holds
    // both on purpose.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    REQUIRE(tc.slot_is_actively_loaded(1));

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", -2}}}});

    // The held state the base rule would answer from, asserted so this case
    // fails for the right reason if the hold is ever dropped instead.
    REQUIRE(tc.get_current_slot() == 1);
    REQUIRE(tc.is_filament_loaded());

    CHECK_FALSE(tc.slot_is_actively_loaded(1));
    CHECK_FALSE(tc.slot_has_filament_at_toolhead(1));
    CHECK_FALSE(tc.can_unload_from_toolhead(1));

    // Withdrawn, not blanked: the tool comes back when the sensors do.
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    CHECK(tc.slot_is_actively_loaded(1));
}

// ============================================================================
// Partial status frames
// ============================================================================

TEST_CASE("A partial medusahc frame leaves the mounted tool alone",
          "[ams][toolchanger][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}, {"tool_count", 4}}}});
    REQUIRE(tc.get_current_slot() == 1);
    REQUIRE(tc.slot_is_actively_loaded(1));

    // Moonraker republishes only the fields that CHANGED, so a frame carrying
    // the gripper alone says nothing about the carriage.
    tc.feed(json{{"medusahc", {{"feeder_open", true}}}});

    CHECK(tc.get_current_slot() == 1);
    CHECK(tc.is_filament_loaded());
    CHECK(tc.slot_is_actively_loaded(1));
    CHECK(tc.can_unload_from_toolhead(1));
}

TEST_CASE("A partial medusahc frame does not empty the mounted tool's slot",
          "[ams][toolchanger][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // The mounted tool's own dock reads vacant: that is where the tool came from.
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", 1},
                   {"sensors", {{"e", 1}, {"t0", 1}, {"t1", 0}, {"t2", 1}, {"t3", 1}}}}}});
    REQUIRE(tc.get_slot_info(1).status == SlotStatus::LOADED);

    tc.feed(json{{"medusahc", {{"feeder_open", true}}}});

    CHECK(tc.get_slot_info(1).status == SlotStatus::LOADED);
}

// ============================================================================
// The overloaded -2
// ============================================================================

TEST_CASE("A -2 during a swap is transitional, not a sensor fault",
          "[ams][toolchanger][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    REQUIRE(tc.slot_is_actively_loaded(1));

    // The tool is now between its dock and the head, where no settled switch
    // pattern exists and the firmware answers -2 for want of anything better.
    tc.feed(json{{"medusahc", {{"operation", "changing"}, {"current_tool", 1}}}});
    REQUIRE(tc.get_system_info().is_busy());

    tc.feed(json{{"medusahc", {{"operation", "changing"}, {"current_tool", -2}}}});

    CHECK(tc.get_system_info().action != AmsAction::ERROR);
    CHECK(tc.get_current_slot() == 1);
    CHECK_FALSE(tc.current_error().has_value());
}

TEST_CASE("A published sensor_error is a fault mid-swap", "[ams][toolchanger][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc", {{"operation", "picking"}, {"current_tool", 1}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::SELECTING);

    // Irbis3D publishes the flag itself; that is the firmware's own verdict and
    // outranks the phase.
    tc.feed(json{
        {"medusahc", {{"operation", "picking"}, {"current_tool", 1}, {"sensor_error", true}}}});

    CHECK(tc.get_system_info().action == AmsAction::ERROR);
    CHECK(tc.current_error().has_value());
}

TEST_CASE("A tool put back by hand clears the controller's latched sensor_error",
          "[ams][toolchanger][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc", {{"operation", "picking"}, {"current_tool", 1}}}});
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", -2}, {"sensor_error", true}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);
    REQUIRE_FALSE(tc.unload_blocked_reason(1).empty());

    // A delta carrying none of the carriage holds the fault.
    tc.feed(json{{"medusahc", {{"feeder_open", false}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);

    // The controller republishes current_tool as the docks change, never the
    // flag: it stays true until its next operation begins.
    tc.feed(json{{"medusahc", {{"current_tool", -1}}}});
    CHECK(tc.get_system_info().action == AmsAction::IDLE);
    CHECK(tc.get_current_slot() == -1);
    CHECK(tc.unload_blocked_reason(1).empty());
}

TEST_CASE("A latched sensor_error from before a restart is no fault while the docks agree",
          "[ams][toolchanger][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // The first full frame after a restart: the flag still latched from a
    // failure before it, the docks reading a definite carriage.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}, {"sensor_error", true}}}});
    CHECK(tc.get_system_info().action != AmsAction::ERROR);
    CHECK(tc.get_current_slot() == 2);
    CHECK(tc.unload_blocked_reason(2).empty());
}

TEST_CASE("A -2 at rest is still a fault, and names the docks", "[ams][toolchanger][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", -2}}}});

    auto err = tc.current_error();
    REQUIRE(err.has_value());
    CHECK(err->source == helix::ErrorSource::TOOLCHANGER);
    CHECK_FALSE(err->title.empty());
}

// ============================================================================
// Filament in the tube
// ============================================================================

TEST_CASE("An ejected tool draws no filament in its tube", "[ams][toolchanger][medusahc]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // T0 on the head (its own dock therefore vacant), T3 off the rack entirely.
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", 0},
                   {"sensors", {{"e", 1}, {"t0", 0}, {"t1", 1}, {"t2", 1}, {"t3", 0}}}}}});

    REQUIRE(tc.get_slot_info(3).status == SlotStatus::EMPTY);
    CHECK(tc.get_slot_filament_segment(3) == PathSegment::NONE);

    // A docked tool carries its own hot end and its own strand, so it keeps one.
    CHECK(tc.get_slot_filament_segment(1) == PathSegment::NOZZLE);
    CHECK(tc.get_slot_filament_segment(0) == PathSegment::NOZZLE);
}

// ============================================================================
// The mock's dock-sensor fault
// ============================================================================

namespace {
/// Advance the mock's swap one notification at a time until it rests, and
/// return every operation word the controller published along the way.
std::vector<std::string> run_medusa_to_rest(MoonrakerClientMock& mock) {
    std::vector<std::string> ops;
    for (int i = 0; i < 40; ++i) {
        const std::string op = mock.medusa_status_json()["operation"].get<std::string>();
        if (ops.empty() || ops.back() != op) {
            ops.push_back(op);
        }
        if (op == "idle" && ops.size() > 1) {
            break;
        }
        helix::MoonrakerClientMockTestAccess::advance_medusa(mock);
    }
    return ops;
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "HELIX_MOCK_MEDUSA_SENSOR_ERROR is a pick that failed before connect",
                 "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc");
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    const bool fault = GENERATE(true, false);
    CAPTURE(fault);
    ScopedEnv fault_env("HELIX_MOCK_MEDUSA_SENSOR_ERROR", fault ? "1" : nullptr);

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(mock.is_mock_medusahc());

    // -2 is pin_watch's "the docks cannot tell", distinct from -1, nothing
    // mounted, and [medusahc] republishes pin_watch's reading.
    const json status = mock.medusa_status_json();
    CHECK(status["current_tool"].get<int>() == (fault ? -2 : 0));
    CHECK(mock.pin_watch_status_json()["current_tool"].get<int>() == (fault ? -2 : 0));
    CHECK(status["sensor_error"].get<bool>() == fault);
    CHECK(status["last_error"].get<std::string>().empty() == !fault);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "The MedusaHC mock latches a failed pick while current_tool follows the docks",
                 "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc");
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    ScopedEnv fail_env("HELIX_MOCK_MEDUSA_SWAP_FAIL", "2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(mock.medusa_status_json()["current_tool"].get<int>() == 0);

    REQUIRE(mock.gcode_script("T2") == 0);
    CHECK(run_medusa_to_rest(mock) ==
          std::vector<std::string>{"changing", "dropping", "picking", "idle"});

    json status = mock.medusa_status_json();
    CHECK(status["sensor_error"].get<bool>());
    CHECK(status["last_error"].get<std::string>() == "MHC_SET: sensors did not confirm T2");
    CHECK(status["current_tool"].get<int>() == -2);
    CHECK(mock.pin_watch_status_json()["current_tool"].get<int>() == -2);
    CHECK(status["target_tool"].get<int>() == 2);
    // _fail() runs before the pick's _close().
    CHECK(status["feeder_open"].get<bool>());

    // The screen sees the failure, then the hot end put back by hand.
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(mock.hardware()));
    tc.feed(json{{"medusahc", status}});
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);

    mock.medusa_place_tools_by_hand(-1);
    status = mock.medusa_status_json();
    CHECK(status["current_tool"].get<int>() == -1);
    CHECK(mock.pin_watch_status_json()["current_tool"].get<int>() == -1);
    CHECK(status["sensor_error"].get<bool>()); // only the next operation clears it
    tc.feed(json{{"medusahc", {{"current_tool", status["current_tool"]}}}});
    CHECK(tc.get_system_info().action == AmsAction::IDLE);

    // The next operation's _begin() clears the latch before anything moves.
    REQUIRE(mock.gcode_script("MHC_SET T=1") == 0);
    status = mock.medusa_status_json();
    CHECK_FALSE(status["sensor_error"].get<bool>());
    CHECK(status["last_error"].get<std::string>().empty());
    CHECK(run_medusa_to_rest(mock) == std::vector<std::string>{"changing", "picking", "idle"});
    status = mock.medusa_status_json();
    CHECK(status["current_tool"].get<int>() == 1);
    CHECK_FALSE(status["feeder_open"].get<bool>());
}

TEST_CASE_METHOD(LVGLTestFixture, "The MedusaHC mock refuses to move when the docks cannot tell",
                 "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc");
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    ScopedEnv fault_env("HELIX_MOCK_MEDUSA_SENSOR_ERROR", "1");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    REQUIRE(mock.gcode_script("SET T=1") == 0);
    CHECK(run_medusa_to_rest(mock) == std::vector<std::string>{"changing", "idle"});
    const json status = mock.medusa_status_json();
    CHECK(status["last_error"].get<std::string>() == "MHC_SET: ambiguous sensor state");
    CHECK(status["sensor_error"].get<bool>());
    CHECK(status["current_tool"].get<int>() == -2);

    CHECK(mock.gcode_script("MHC_SET T=4") != 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "HELIX_MOCK_MEDUSA_REDOCK_S docks the stuck hot end by hand",
                 "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc");
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    ScopedEnv fault_env("HELIX_MOCK_MEDUSA_SENSOR_ERROR", "1");
    ScopedEnv redock_env("HELIX_MOCK_MEDUSA_REDOCK_S", "2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    helix::MoonrakerClientMockTestAccess::advance_medusa(mock);
    REQUIRE(mock.medusa_status_json()["current_tool"].get<int>() == -2);
    helix::MoonrakerClientMockTestAccess::advance_medusa(mock);
    const json status = mock.medusa_status_json();
    CHECK(status["current_tool"].get<int>() == -1);
    CHECK(status["sensor_error"].get<bool>());
}

namespace {
/// A printer.gcode.script rpc's answer, as its caller sees it.
struct ScriptReply {
    int successes = 0;
    int errors = 0;
    std::string message;
};

void send_script(MoonrakerClientMock& mock, const std::string& script, ScriptReply& reply) {
    mock.send_jsonrpc(
        "printer.gcode.script", json{{"script", script}},
        [&reply](const json&) { ++reply.successes; },
        [&reply](const MoonrakerError& err) {
            ++reply.errors;
            reply.message = err.message;
        });
}

bool history_has(const MoonrakerClientMock& mock, const std::string& line) {
    const auto& history = mock.gcode_script_history();
    return std::find(history.begin(), history.end(), line) != history.end();
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "A MedusaHC swap answers its gcode script only once it ends",
                 "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc");
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    ScopedEnv fail_env("HELIX_MOCK_MEDUSA_SWAP_FAIL", "2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("a failed pick outside a print is a gcode error, and the script ends there") {
        ScriptReply reply;
        send_script(mock, "T2\nM104 T0 S200", reply);
        ui::UpdateQueue::instance().drain();
        CHECK(reply.successes + reply.errors == 0);

        run_medusa_to_rest(mock);
        ui::UpdateQueue::instance().drain();
        CHECK(reply.successes == 0);
        REQUIRE(reply.errors == 1);
        CHECK(reply.message == "MHC_SET: sensors did not confirm T2");
        CHECK_FALSE(history_has(mock, "M104 T0 S200"));
        CHECK(mock.medusa_status_json()["sensor_error"].get<bool>());
    }
    SECTION("a pick the docks confirm succeeds, then runs the rest of the script") {
        ScriptReply reply;
        send_script(mock, "MHC_SET T=1\nM104 T0 S200", reply);
        CHECK_FALSE(history_has(mock, "M104 T0 S200"));

        run_medusa_to_rest(mock);
        ui::UpdateQueue::instance().drain();
        CHECK(reply.errors == 0);
        CHECK(reply.successes == 1);
        CHECK(history_has(mock, "M104 T0 S200"));
    }
    SECTION("a failed pick during a print pauses it and the script still succeeds") {
        REQUIRE(mock.start_print_internal("3DBenchy.gcode"));
        ScriptReply reply;
        send_script(mock, "T2", reply);
        run_medusa_to_rest(mock);
        ui::UpdateQueue::instance().drain();
        CHECK(reply.errors == 0);
        CHECK(reply.successes == 1);
        CHECK(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PAUSED);
    }
    SECTION("an unmount holds its reply until the hot end is parked") {
        ScriptReply reply;
        send_script(mock, "DROP_TOOL", reply);
        ui::UpdateQueue::instance().drain();
        CHECK(reply.successes == 0);
        run_medusa_to_rest(mock);
        ui::UpdateQueue::instance().drain();
        CHECK(reply.successes == 1);
        CHECK(mock.medusa_status_json()["current_tool"].get<int>() == -1);
    }
    SECTION("a disconnect fails the held reply, and the swap ending answers nothing more") {
        ScriptReply reply;
        send_script(mock, "T1", reply);
        mock.disconnect();
        CHECK(reply.errors == 1);
        CHECK(reply.message == MoonrakerError::connection_lost("printer.gcode.script").message);
        run_medusa_to_rest(mock);
        ui::UpdateQueue::instance().drain();
        CHECK(reply.errors == 1);
        CHECK(reply.successes == 0);
    }
    SECTION("a swap a direct gcode call started does not hold the next script") {
        REQUIRE(mock.gcode_script("SELECT_TOOL T=1") == 0);
        ScriptReply reply;
        send_script(mock, "M104 T0 S200\nM104 T0 S210", reply);
        CHECK(reply.successes == 1);
        CHECK(history_has(mock, "M104 T0 S210"));
    }
    SECTION("a swap sent while another runs is refused at once") {
        ScriptReply first;
        ScriptReply second;
        send_script(mock, "T1", first);
        send_script(mock, "T3", second);
        CHECK(second.errors == 1);
        CHECK(second.message == "MedusaHC is busy: changing");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "Beside klipper-toolchanger, tool_number follows the docks",
                 "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc-ktc");
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    ScopedEnv fail_env("HELIX_MOCK_MEDUSA_SWAP_FAIL", "3");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(mock.sync_medusa_toolchanger_tool() == 0);

    REQUIRE(mock.gcode_script("SELECT_TOOL TOOL=\"tool T2\"") == 0);
    run_medusa_to_rest(mock);
    CHECK(mock.sync_medusa_toolchanger_tool() == 2);
    json response;
    mock.send_jsonrpc("printer.objects.subscribe",
                      json{{"objects", json::object({{"toolchanger", nullptr}})}},
                      [&response](const json& r) { response = r; });
    CHECK(response["result"]["status"]["toolchanger"]["tool_number"].get<int>() == 2);

    SECTION("a reading the docks cannot name unselects outside a print") {
        REQUIRE(mock.gcode_script("T3") == 0);
        run_medusa_to_rest(mock);
        REQUIRE(mock.medusa_status_json()["current_tool"].get<int>() == -2);
        CHECK(mock.sync_medusa_toolchanger_tool() == -1);
    }
    SECTION("and is left alone during a print") {
        REQUIRE(mock.start_print_internal("3DBenchy.gcode"));
        REQUIRE(mock.gcode_script("T3") == 0);
        run_medusa_to_rest(mock);
        REQUIRE(mock.medusa_status_json()["current_tool"].get<int>() == -2);
        CHECK(mock.sync_medusa_toolchanger_tool() == 2);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "The T<n>-macro MedusaHC mock carries six tools",
                 "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc-tn");
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    const auto hw = mock.hardware();
    const auto& objs = hw.printer_objects();
    CHECK(std::find(objs.begin(), objs.end(), "gcode_macro T5") != objs.end());
    CHECK(std::find(objs.begin(), objs.end(), "gcode_macro T6") == objs.end());
    const auto& heaters = hw.heaters();
    CHECK(std::find(heaters.begin(), heaters.end(), "extruder5") != heaters.end());
    // Every tool's lane mirrors a spool, the last two included.
    CHECK(mock.mock_db_get("lane_data", "T5")["spoolman_id"].get<int>() == 15);
    CHECK(mock.mock_db_get("lane_data", "T4")["material"].get<std::string>() == "ASA");
    json response;
    mock.send_jsonrpc("printer.objects.subscribe",
                      json{{"objects", json::object({{"extruder5", nullptr}})}},
                      [&response](const json& r) { response = r; });
    CHECK(response["result"]["status"]["extruder5"]["temperature"].get<double>() == 25.0);

    json status = mock.medusa_status_json();
    CHECK(status["tool_count"].get<int>() == 6);
    CHECK(status["sensors"].contains("t5"));

    REQUIRE(mock.gcode_script("T5") == 0);
    run_medusa_to_rest(mock);
    CHECK(mock.medusa_status_json()["current_tool"].get<int>() == 5);
    CHECK(mock.gcode_script("SET T=6") != 0);
    CHECK(mock.get_last_gcode_error() == "Tool T6 is outside configured range T0..T5");
}

namespace {
/// The status frames a connected mock pushes over its first live ticks.
std::vector<json> live_frames(MoonrakerClientMock& mock) {
    std::mutex mu;
    std::vector<json> frames;
    const auto sub = mock.register_notify_update([&](const json& notification) {
        auto params = notification.find("params");
        if (params != notification.end() && params->is_array() && !params->empty()) {
            std::lock_guard<std::mutex> lock(mu);
            frames.push_back((*params)[0]);
        }
    });
    mock.connect("ws://mock/websocket", [] {}, [] {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (MoonrakerClientMockTestAccess::tick_count(mock) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    mock.unsubscribe_notify_update(sub);
    mock.disconnect();
    std::lock_guard<std::mutex> lock(mu);
    return frames;
}

bool any_frame(const std::vector<json>& frames, const std::function<bool(const json&)>& pred) {
    return std::any_of(frames.begin(), frames.end(), pred);
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "The MedusaHC mock's live frames carry its whole machine",
                 "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    SECTION("six hot ends on the T<n>-macro controller") {
        ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc-tn");
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        const auto frames = live_frames(mock);
        CHECK(any_frame(frames, [](const json& f) {
            // The live tick's shape, not the query's (which carries pressure advance).
            return f.contains("extruder5") && f["extruder5"].contains("temperature") &&
                   !f["extruder5"].contains("pressure_advance");
        }));
    }
    SECTION("klipper-toolchanger's tool_number beside the controller") {
        ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc-ktc");
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        const auto frames = live_frames(mock);
        CHECK(any_frame(frames, [](const json& f) {
            return f.contains("toolchanger") && f["toolchanger"].contains("tool_number") &&
                   f["toolchanger"]["tool_number"] == 0 && !f["toolchanger"].contains("status");
        }));
    }
}

TEST_CASE("Each MedusaHC mock variant lists its configuration's objects",
          "[ams][toolchanger][medusahc][mock]") {
    ScopedEnv printer_env("HELIX_MOCK_PRINTER", nullptr);
    const auto has = [](const helix::PrinterDiscovery& hw, const std::string& name) {
        const auto& objs = hw.printer_objects();
        return std::find(objs.begin(), objs.end(), name) != objs.end();
    };

    SECTION("the stock controller: no klipper-toolchanger, four tools") {
        ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc");
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        REQUIRE(mock.mock_medusa_variant() == MoonrakerClientMock::MedusaVariant::CONTROLLER);
        const auto hw = mock.hardware();
        CHECK(has(hw, "pin_watch io"));
        CHECK(has(hw, "medusahc"));
        CHECK_FALSE(has(hw, "toolchanger"));
        CHECK_FALSE(has(hw, "tool T0"));
        CHECK(has(hw, "gcode_macro T3"));
        CHECK_FALSE(has(hw, "gcode_macro T4"));
        // MHC_* are native commands, never a [gcode_macro].
        CHECK_FALSE(has(hw, "gcode_macro MHC_OPEN"));
        for (const char* macro :
             {"gcode_macro OPEN", "gcode_macro CLOSE", "gcode_macro CLEAN", "gcode_macro DROP_TOOL",
              "gcode_macro _TOOL_OFFSET", "gcode_macro _GLOBAL_STATE", "save_variables"}) {
            CAPTURE(macro);
            CHECK(has(hw, macro));
        }
        // A separate project's object, not Irbis3D Core's.
        CHECK_FALSE(has(hw, "medusahc_calibrate"));
        CHECK(hw.tool_names().size() == 4);
        const auto cmds = toolchanger_addon::resolve_tool_commands(hw);
        CHECK(cmds.select_prefix == "T");
        CHECK(cmds.unselect == "DROP_TOOL");
        CHECK_FALSE(cmds.initialize_available);
        CHECK(mock.medusa_status_json()["tool_count"].get<int>() == 4);
    }
    SECTION("controller with klipper-toolchanger") {
        ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc-ktc");
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        REQUIRE(mock.mock_medusa_variant() == MoonrakerClientMock::MedusaVariant::CONTROLLER_KTC);
        const auto hw = mock.hardware();
        CHECK(has(hw, "pin_watch io"));
        CHECK(has(hw, "toolchanger"));
        CHECK(has(hw, "tool T3"));
        CHECK(has(hw, "gcode_macro OPEN"));
        const auto cmds = toolchanger_addon::resolve_tool_commands(hw);
        CHECK(cmds.select_prefix == "T");
        CHECK(cmds.initialize_available);
    }
    SECTION("controller alone, T<n> macros") {
        ScopedEnv ams_env("HELIX_MOCK_AMS", "medusahc-tn");
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        REQUIRE(mock.mock_medusa_variant() == MoonrakerClientMock::MedusaVariant::CONTROLLER_TN);
        const auto hw = mock.hardware();
        CHECK(has(hw, "pin_watch io"));
        CHECK_FALSE(has(hw, "toolchanger"));
        CHECK_FALSE(has(hw, "tool T0"));
        CHECK(has(hw, "gcode_macro T0"));
        CHECK(has(hw, "gcode_macro DROP_TOOL"));
        CHECK(toolchanger_addon::resolve_tool_sensor(hw).present);
        const auto cmds = toolchanger_addon::resolve_tool_commands(hw);
        CHECK(cmds.select_prefix == "T");
        CHECK(cmds.unselect == "DROP_TOOL");
        CHECK(mock.medusa_status_json().contains("operation"));
    }
}

TEST_CASE("Only upstream's dock and head keys are read", "[ams][toolchanger][medusahc]") {
    // The abandoned fork published toolN_docked and head_loaded; upstream's
    // controller has `sensors` with t0, t1, ... only.
    auto reading =
        toolchanger_addon::read_tool(json{{"medusahc",
                                           {{"current_tool", 0},
                                            {"tool1_docked", true},
                                            {"head_loaded", true},
                                            {"sensors", {{"tool2_docked", 1}, {"t3", 1}}}}}});
    REQUIRE(reading.has_value());
    CHECK_FALSE(reading->head_loaded.has_value());
    REQUIRE(reading->docks.size() == 4);
    CHECK_FALSE(reading->docks[1].has_value());
    CHECK_FALSE(reading->docks[2].has_value());
    CHECK(reading->docks[3] == std::optional<bool>(true));
}
