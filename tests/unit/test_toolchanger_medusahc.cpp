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

#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_types.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// Real backend with gcode captured. client_ is null, so ensure_homed_then()
/// routes straight to execute_gcode().
class ToolChangerHelper : public AmsBackendToolChanger {
  public:
    explicit ToolChangerHelper(int tool_count) : AmsBackendToolChanger(nullptr, nullptr) {
        std::vector<std::string> names;
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
        running_ = true;
    }

    ~ToolChangerHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        sent_.push_back(gcode);
        (void)on_complete;
        return AmsErrorHelper::success();
    }

    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

    void feed_status(const char* status, int tool_number) {
        feed(json{{"toolchanger", {{"status", status}, {"tool_number", tool_number}}}});
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }

  private:
    std::vector<std::string> sent_;
};

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

// ============================================================================
// Init states
// ============================================================================

TEST_CASE("A cold-boot toolchanger refuses the tool tap", "[ams][toolchanger][coldstart]") {
    ToolChangerHelper h(4);
    h.feed_status("uninitialized", -1);

    // Refusing is the lesser evil, not a clean answer. On the default
    // initialize_on: first-use, select_tool() would have auto-initialized and
    // this tap is what would have cleared the state, so the refusal costs the
    // user a Reset they should not have needed.
    //
    // Letting it through is worse. On initialize_on: manual (what MedusaHC
    // ships) Klipper raises "Cannot select tool, toolchanger status is
    // uninitialized". That rejection reaches execute_gcode()'s error callback,
    // which only logs: on_complete never fires, so the optimistic SELECTING that
    // dispatch_operation() stamped is never unwound, and execute_gcode() returns
    // success() so the `if (!result)` net misses it too. is_busy() would then
    // refuse every later op, and Moonraker only republishes CHANGED fields, so
    // no second 'uninitialized' frame arrives to reset it. A latched SELECTING
    // is unrecoverable without a restart; a refusal clears with Reset, which
    // sends INITIALIZE_TOOLCHANGER.
    CHECK(h.get_system_info().is_busy());
    CHECK(h.get_current_action() == AmsAction::RESETTING);

    auto err = h.change_tool(2);
    CHECK_FALSE(err.success());
    CHECK(h.sent().empty());
}

TEST_CASE("An initializing toolchanger IS busy", "[ams][toolchanger][coldstart]") {
    ToolChangerHelper h(4);
    h.feed_status("initializing", -1);

    // STATUS_INITIALIZING was unmapped and fell through to IDLE, so a tap landed
    // mid-initialization while the carriage was moving.
    CHECK(h.get_system_info().is_busy());
    CHECK(h.get_current_action() == AmsAction::RESETTING);

    auto err = h.change_tool(1);
    CHECK_FALSE(err.success());
    CHECK(h.sent().empty());
}

TEST_CASE("The settled toolchanger states are unchanged", "[ams][toolchanger][coldstart]") {
    // Regression guard: the two fixes above must not disturb the states that
    // already worked.
    ToolChangerHelper h(4);

    h.feed_status("ready", 0);
    CHECK(h.get_current_action() == AmsAction::IDLE);

    h.feed_status("changing", 0);
    CHECK(h.get_current_action() == AmsAction::SELECTING);

    h.feed_status("error", -1);
    CHECK(h.get_current_action() == AmsAction::ERROR);
}

TEST_CASE("The raw toolchanger status survives as operation detail",
          "[ams][toolchanger][coldstart]") {
    // IDLE alone cannot tell the user why a tap was rejected by Klipper. The
    // status string is what names the state.
    ToolChangerHelper h(4);
    h.feed_status("uninitialized", -1);
    CHECK(h.get_system_info().operation_detail == "uninitialized");
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
/// no pin_watch, tools enumerated off the extruders.
static PrinterDiscovery standalone_medusahc_discovery(int tools) {
    auto objects = json::array({"medusahc", "toolhead"});
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
    CHECK(tc.sent().back() == "SELECT_TOOL T=2");
}

TEST_CASE("A changer without klipper-toolchanger selects with its own macro",
          "[ams][toolchanger][commands]") {
    auto hw = standalone_medusahc_discovery(3);
    ToolChangerHelper tc(3);
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
    // There is no toolchanger object to report "ready"; the extra's own state
    // field is what settles the action.
    tc.feed(json{{"medusahc", {{"state", "ready"}, {"current_tool", -1}}}});

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
    tc.feed(json{{"medusahc", {{"state", "ready"}, {"current_tool", 1}}}});

    auto err = tc.unload_filament(1);
    REQUIRE(err.success());
    REQUIRE_FALSE(tc.sent().empty());
    // DROP_TOOL takes no tool argument: there is only ever one tool on the head.
    CHECK(tc.sent().back() == "DROP_TOOL");
}

// ============================================================================
// Phase vocabulary: `state` is not another spelling of `operation`
// ============================================================================
//
// Read from both controllers' sources rather than inferred from each other:
//   Irbis3D/MedusaHC-Python-Controller  klippy/extras/medusahc.py  `operation`
//     -> idle / dropping / picking
//   topi314/MedusaHC                    scripts/medusahc.py        `state`
//     -> uninitialized / ready / changing / error
// Only the first names the swap DIRECTION. Treating them as interchangeable is
// what left `changing` matching no branch at all.

TEST_CASE("Only the operation key names the swap direction", "[ams][toolchanger][medusahc]") {
    auto upstream = toolchanger_addon::read_tool(
        json{{"medusahc", {{"operation", "picking"}, {"current_tool", 1}}}});
    REQUIRE(upstream.has_value());
    CHECK(upstream->operation == "picking");
    CHECK(upstream->phase_names_direction);

    auto fork = toolchanger_addon::read_tool(
        json{{"medusahc", {{"state", "changing"}, {"current_tool", 1}}}});
    REQUIRE(fork.has_value());
    CHECK(fork->operation == "changing");
    CHECK_FALSE(fork->phase_names_direction);
}

TEST_CASE("The gripper is read from both schemas", "[ams][toolchanger][medusahc]") {
    auto upstream = toolchanger_addon::read_tool(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", true}}}});
    REQUIRE(upstream.has_value());
    REQUIRE(upstream->feeder_open.has_value());
    CHECK(*upstream->feeder_open);

    // medusahc.py:370 publishes feeder_open too - the fork is NOT gripper-blind.
    auto fork = toolchanger_addon::read_tool(
        json{{"medusahc", {{"state", "ready"}, {"current_tool", 0}, {"feeder_open", false}}}});
    REQUIRE(fork.has_value());
    REQUIRE(fork->feeder_open.has_value());
    CHECK_FALSE(*fork->feeder_open);

    // A machine that never mentions the gripper is not a machine whose gripper
    // is closed: the step bar keys on the difference.
    auto silent =
        toolchanger_addon::read_tool(json{{"medusahc", {{"state", "ready"}, {"current_tool", 0}}}});
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->feeder_open.has_value());
}

TEST_CASE("A swap reported only as 'changing' still reads as busy",
          "[ams][toolchanger][medusahc]") {
    // Regression: `changing` matched none of picking/dropping/idle/ready, and on
    // this fork there is no [toolchanger] object to set the action either, so a
    // swap ran with nothing but our own optimistic dispatch driving the UI.
    auto hw = standalone_medusahc_discovery(3);
    ToolChangerHelper tc(3);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));

    tc.feed(json{{"medusahc", {{"state", "ready"}, {"current_tool", 0}}}});
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);

    tc.feed(json{{"medusahc", {{"state", "changing"}, {"current_tool", 0}}}});
    CHECK(tc.get_current_action() != AmsAction::IDLE);

    tc.feed(json{{"medusahc", {{"state", "ready"}, {"current_tool", 2}}}});
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

TEST_CASE("A controller with only 'changing' gets one middle step", "[ams][toolchanger][steps]") {
    ToolChangerHelper tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(standalone_medusahc_discovery(4)));
    tc.feed(json{{"medusahc", {{"state", "ready"}, {"current_tool", 0}, {"feeder_open", false}}}});

    // The gripper still brackets it; the middle cannot be split.
    CHECK(step_labels(tc.get_operation_step_model(StepOperationType::LOAD_SWAP)) ==
          std::vector<std::string>{"Release filament", "Change tool", "Grip filament"});
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
    // get_operation_step_model() and step_index_for_phase_locked() both derive
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

// ============================================================================
// Wording and refusals
// ============================================================================

TEST_CASE("A changer's load mounts a tool rather than moving filament",
          "[ams][toolchanger][labels]") {
    // Drives the Mount/Unmount wording. Asked as a capability so the Snapmaker
    // U1 - a tool changer whose load really does feed filament - keeps the
    // filament wording.
    ToolChangerHelper tc(2);
    CHECK(tc.load_mounts_tool());
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
