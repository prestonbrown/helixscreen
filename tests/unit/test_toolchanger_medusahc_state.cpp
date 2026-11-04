// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_medusahc_state.cpp
 * @brief What a MedusaHC changer's status frames mean for the carriage, the
 *        action and the commands the backend is willing to send.
 *
 * Two persona discoveries: Irbis3D's controller beside klipper-toolchanger
 * ([toolchanger] + [tool Tn] + pin_watch + [medusahc]), and a changer with no
 * klipper-toolchanger at all, whose swap is the controller's own T<n>
 * ([pin_watch io] + [medusahc] + T<n> macros). The docks are the physical truth on both;
 * toolchanger.tool_number is only what SELECT_TOOL last set.
 */

#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "moonraker_error.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::test::ToolChangerHelper;

namespace {

/// Irbis3D's controller beside klipper-toolchanger.
PrinterDiscovery irbis_tc_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3",
                                  "pin_watch io", "medusahc", "extruder"}));
    return hw;
}

/// pin_watch and [medusahc] with T<n> macros and no klipper-toolchanger.
PrinterDiscovery standalone_discovery(int tools) {
    auto objects = json::array({"pin_watch io", "medusahc", "toolhead"});
    for (int i = 0; i < tools; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
        objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// A backend wired the way set_discovery() wires @p hw.
void wire(ToolChangerHelper& tc, const PrinterDiscovery& hw) {
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
}

bool sent_contains(const ToolChangerHelper& tc, const std::string& needle) {
    return std::any_of(tc.sent().begin(), tc.sent().end(),
                       [&](const std::string& g) { return g.find(needle) != std::string::npos; });
}

/// Irbis3D + klipper-toolchanger with tool 1 mounted, then the docks lose it.
void latch_sensor_fault(ToolChangerHelper& tc) {
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 1}}},
                 {"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    REQUIRE(tc.get_current_slot() == 1);
    tc.feed(json{{"medusahc", {{"current_tool", -2}}}});
    REQUIRE(tc.get_current_action() == AmsAction::ERROR);
}

} // namespace

// ============================================================================
// The docks outrank toolchanger.tool_number
// ============================================================================

TEST_CASE("A toolchanger-only delta does not override the dock sensors",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(4);
    wire(tc, irbis_tc_discovery());
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", -1}}},
                 {"medusahc", {{"operation", "idle"}, {"current_tool", -1}}}});
    REQUIRE(tc.get_current_slot() == -1);

    // SELECT_TOOL set tool_number, the pickup failed and the docks still say
    // the head is empty, so medusahc.current_tool did not change and is absent
    // from the delta.
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});
    CHECK(tc.get_current_slot() == -1);
    CHECK(tc.get_current_tool() == -1);
    CHECK_FALSE(tc.slot_is_actively_loaded(2));
}

TEST_CASE("Without dock sensors toolchanger.tool_number still names the carriage",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(4);
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});
    CHECK(tc.get_current_slot() == 2);
    CHECK(tc.slot_is_actively_loaded(2));
}

TEST_CASE("A dock-sensor fault holds the last tool beside tool_number -1",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(4);
    wire(tc, irbis_tc_discovery());
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 1}}},
                 {"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    REQUIRE(tc.get_current_slot() == 1);
    tc.feed(json{{"toolchanger", {{"tool_number", -1}}}, {"medusahc", {{"current_tool", -2}}}});
    CHECK(tc.get_current_action() == AmsAction::ERROR);
    CHECK(tc.get_current_slot() == 1);
}

TEST_CASE("A toolchanger 'ready' frame does not clear a dock-sensor fault",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(4);
    wire(tc, irbis_tc_discovery());
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    tc.feed(json{{"medusahc", {{"current_tool", -2}}}});
    REQUIRE(tc.get_current_action() == AmsAction::ERROR);

    tc.feed(json{{"toolchanger", {{"status", "ready"}}}});
    CHECK(tc.get_current_action() == AmsAction::ERROR);
    CHECK(tc.current_error().has_value());

    // The docks agreeing again is what clears it.
    tc.feed(json{{"medusahc", {{"current_tool", 1}}}});
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.current_error().has_value());
}

// ============================================================================
// No swap against an unknown carriage
// ============================================================================

TEST_CASE("A dock-sensor fault refuses park and tool change",
          "[ams][toolchanger][medusahc][state]") {
    SECTION("beside klipper-toolchanger") {
        ToolChangerHelper tc(4);
        wire(tc, irbis_tc_discovery());
        latch_sensor_fault(tc);

        const AmsError park = tc.park_active_tool();
        CHECK_FALSE(park.success());
        CHECK_FALSE(park.user_msg.empty());
        const AmsError change = tc.change_tool(2);
        CHECK_FALSE(change.success());
        CHECK_FALSE(change.user_msg.empty());
        CHECK(tc.sent().empty());
        CHECK(tc.get_current_action() == AmsAction::ERROR);
    }
    SECTION("on the controller's own T<n>") {
        ToolChangerHelper tc(6);
        wire(tc, standalone_discovery(6));
        tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
        tc.feed(json{{"medusahc", {{"current_tool", -2}}}});
        REQUIRE(tc.get_current_action() == AmsAction::ERROR);

        CHECK_FALSE(tc.change_tool(2).success());
        CHECK_FALSE(tc.park_active_tool().success());
        CHECK(tc.sent().empty());
    }
}

TEST_CASE("A rejected swap command clears the optimistic action",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(6);
    wire(tc, standalone_discovery(6));
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});

    SECTION("a gcode error ends it") {
        REQUIRE(tc.change_tool(3).success());
        REQUIRE(tc.get_current_action() == AmsAction::SELECTING);
        tc.reject_last_send(MoonrakerErrorType::JSON_RPC_ERROR, "MHC_SET: ambiguous sensor state");
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(tc.get_system_info().is_busy());
        CHECK(tc.change_tool(1).success());
    }
    SECTION("a reply timeout leaves the long swap running") {
        REQUIRE(tc.change_tool(3).success());
        tc.reject_last_send(MoonrakerErrorType::TIMEOUT, "timed out");
        CHECK(tc.get_current_action() == AmsAction::SELECTING);
    }
    SECTION("a fault the frames raised is not overwritten") {
        REQUIRE(tc.change_tool(3).success());
        // The controller's own sensor flag mid-swap: a fault, with the dispatch
        // still armed.
        tc.feed(json{{"medusahc", {{"current_tool", -2}, {"sensor_error", true}}}});
        REQUIRE(tc.get_current_action() == AmsAction::ERROR);
        tc.reject_last_send(MoonrakerErrorType::JSON_RPC_ERROR, "swap failed");
        CHECK(tc.get_current_action() == AmsAction::ERROR);
    }
    SECTION("another client's swap the controller reports keeps its action") {
        REQUIRE(tc.change_tool(3).success());
        // Someone else's T1 got there first: the controller is mid-pick.
        tc.feed(json{{"medusahc", {{"operation", "picking"}}}});
        tc.reject_last_send(MoonrakerErrorType::JSON_RPC_ERROR, "MedusaHC is busy: picking");
        CHECK(tc.get_current_action() == AmsAction::SELECTING);
        CHECK(tc.get_system_info().is_busy());
        // Its own idle frame ends it.
        tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
        CHECK(tc.get_current_action() == AmsAction::IDLE);
    }
}

TEST_CASE("A restart or a lost connection forgets the controller's last phase word",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(6);
    wire(tc, standalone_discovery(6));
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});
    tc.feed(json{{"medusahc", {{"operation", "picking"}}}});
    REQUIRE(ToolChangerTestAccess::last_operation_word(tc) == "picking");
    tc.on_connection_lost();
    CHECK(ToolChangerTestAccess::last_operation_word(tc).empty());

    tc.feed(json{{"medusahc", {{"operation", "dropping"}}}});
    REQUIRE(ToolChangerTestAccess::last_operation_word(tc) == "dropping");
    tc.on_klipper_restarted();
    CHECK(ToolChangerTestAccess::last_operation_word(tc).empty());
}

TEST_CASE("A park this screen dispatched fails as a park, not a tool change",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(6);
    wire(tc, standalone_discovery(6));
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    REQUIRE(tc.park_active_tool().success());
    // The failure is the first frame the controller sends: no `dropping` seen.
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", -2},
                   {"target_tool", 1},
                   {"sensor_error", true},
                   {"last_error", "MHC_DROP: sensors did not confirm the drop"}}}});
    const auto err = tc.current_error();
    REQUIRE(err.has_value());
    CHECK(err->detail == "Parking Tool 2 (T1) failed: MHC_DROP: sensors did not confirm the drop");
}

TEST_CASE("Klipper restart: a swap latched mid-'changing' clears",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(6);
    wire(tc, standalone_discovery(6));
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});
    tc.feed(json{{"medusahc", {{"operation", "changing"}, {"feeder_open", true}}}});
    tc.on_klipper_restarted();
    // The controller's first frame after the restart: idle, gripper closed.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}, {"feeder_open", false}}}});
    CHECK(tc.get_system_info().operation_phase == -1);
}

// ============================================================================
// Recover and reset
// ============================================================================

TEST_CASE("Recover and reset send INITIALIZE_TOOLCHANGER only where it exists",
          "[ams][toolchanger][medusahc][state]") {
    SECTION("no klipper-toolchanger") {
        ToolChangerHelper tc(6);
        wire(tc, standalone_discovery(6));
        tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});
        CHECK_FALSE(tc.reset_available());
        CHECK_FALSE(tc.recover_available());
        CHECK(tc.reset().result == AmsResult::NOT_SUPPORTED);
        CHECK(tc.recover().result == AmsResult::NOT_SUPPORTED);
        CHECK(tc.sent().empty());
    }
    SECTION("beside klipper-toolchanger") {
        ToolChangerHelper tc(4);
        wire(tc, irbis_tc_discovery());
        CHECK(tc.reset_available());
        CHECK(tc.recover_available());
        CHECK(tc.reset().success());
        CHECK(sent_contains(tc, "INITIALIZE_TOOLCHANGER"));
    }
}

// ============================================================================
// Swap commands and tool mapping beside klipper-toolchanger
// ============================================================================

TEST_CASE("Beside klipper-toolchanger a MedusaHC mounts with T<n> and parks with its own drop",
          "[ams][toolchanger][medusahc][commands]") {
    // klipper-toolchanger's SELECT_TOOL/UNSELECT_TOOL do not move this
    // hardware; the controller's own T<n> macros do.
    ToolChangerHelper tc(4);
    wire(tc, irbis_tc_discovery());
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}},
                 {"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});

    REQUIRE(tc.change_tool(2).success());
    CHECK(tc.sent().back() == "T2");

    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}}}});
    REQUIRE(tc.park_active_tool().success());
    // No DROP_TOOL macro in this config: the controller's pair it wraps.
    CHECK(tc.sent().back() == "MHC_DROP\nMHC_CLOSE");
    CHECK_FALSE(sent_contains(tc, "SELECT_TOOL"));
}

TEST_CASE("A MedusaHC maps tools by rewriting the file, with or without klipper-toolchanger",
          "[ams][toolchanger][medusahc][tool_map]") {
    // Its T<n> macros name the physical tool, and ASSIGN_TOOL never changes
    // them, so a Native remap would print on the firmware's own mapping.
    ToolChangerHelper beside(4);
    wire(beside, irbis_tc_discovery());
    CHECK(beside.get_remap_strategy() == AmsBackend::RemapStrategy::GcodeRewrite);
    CHECK_FALSE(beside.set_tool_mapping(/*tool_number=*/1, /*slot_index=*/3).success());
    CHECK_FALSE(sent_contains(beside, "ASSIGN_TOOL"));

    ToolChangerHelper standalone(4);
    wire(standalone, standalone_discovery(4));
    CHECK(standalone.get_remap_strategy() == AmsBackend::RemapStrategy::GcodeRewrite);
}

// ============================================================================
// Physical tools, not G-code numbers
// ============================================================================

TEST_CASE("A dock reading is a physical tool, not klipper-toolchanger's tool number",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(4);
    wire(tc, irbis_tc_discovery());
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}},
                 {"medusahc", {{"operation", "idle"}, {"current_tool", -1}}}});

    // Physical tool 2 (dock t2) goes onto the head, whatever tool_number says.
    tc.feed(json{{"medusahc",
                  {{"current_tool", 2},
                   {"sensors", {{"e", 1}, {"t0", 1}, {"t1", 1}, {"t2", 0}, {"t3", 1}}}}}});
    CHECK(tc.get_current_slot() == 2);
    CHECK(tc.get_slot_info(2).status == SlotStatus::LOADED);
    CHECK(tc.get_slot_info(0).status == SlotStatus::AVAILABLE);
}

TEST_CASE("An out-of-range dock reading never becomes the current slot",
          "[ams][toolchanger][medusahc][state]") {
    SECTION("beside klipper-toolchanger") {
        ToolChangerHelper tc(4);
        wire(tc, irbis_tc_discovery());
        tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
        REQUIRE(tc.get_current_slot() == 1);
        tc.feed(json{{"medusahc", {{"current_tool", 5}, {"tool_count", 6}}}});
        CHECK(tc.get_current_slot() == 1);
        for (int i = 0; i < 4; ++i) {
            CHECK((i == 1) == (tc.get_slot_info(i).status == SlotStatus::LOADED));
        }
    }
    SECTION("on the controller's own T<n>") {
        ToolChangerHelper tc(6);
        wire(tc, standalone_discovery(6));
        tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 7}}}});
        CHECK(tc.get_current_slot() < tc.get_system_info().total_slots);
        CHECK(tc.get_current_tool() < tc.get_system_info().total_slots);
    }
}

// ============================================================================
// Remapping on a changer with no ASSIGN_TOOL
// ============================================================================

TEST_CASE("A refused remap on a T<n> changer leaves the table alone",
          "[ams][toolchanger][medusahc][state]") {
    ToolChangerHelper tc(6);
    wire(tc, standalone_discovery(6));
    tc.feed(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 0}}}});

    SECTION("set_tool_mapping") {
        REQUIRE_FALSE(tc.set_tool_mapping(0, 2).success());
    }
    SECTION("a slot edit that moves the tool number") {
        SlotInfo edit = tc.get_slot_info(2);
        edit.mapped_tool = 0;
        edit.material = "PETG";
        const AmsError err = helix::test::apply_edit(tc, 2, edit);
        CHECK_FALSE(err.success());
        // The filament half of the edit still lands.
        CHECK(tc.get_slot_info(2).material == "PETG");
    }
    const auto map = tc.get_tool_mapping();
    CHECK(map[0] == 0);
    CHECK(map[2] == 2);
    CHECK(tc.get_slot_info(0).mapped_tool == 0);
    CHECK(tc.get_slot_info(2).mapped_tool == 2);
    CHECK_FALSE(sent_contains(tc, "ASSIGN_TOOL"));
}
