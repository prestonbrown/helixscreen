// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_vrtx.cpp
 * @brief VRTX (Slimohan/VRTX) owns the tool change: a mount goes through its T<n> macro.
 *
 * VRTX wraps klipper-toolchanger's SELECT_TOOL in `T<n>` macros that apply the tool's
 * offsets, standby temperatures and a failed-change guard. klipper-toolchanger still
 * runs underneath, so ASSIGN_TOOL, UNSELECT_TOOL and the tool listing stay as they are.
 */

#include "../test_helpers/toolchanger_test_helper.h"
#include "printer_discovery.h"
#include "tool_offset_calibration.h"
#include "toolchanger_addon.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::test::ToolChangerHelper;

namespace {

PrinterDiscovery vrtx_discovery(int tools, bool with_vrtx = true) {
    json objects = json::array({"toolchanger", "extruder", "heater_bed", "toolhead", "configfile"});
    for (int i = 0; i < tools; ++i) {
        objects.push_back("tool T" + std::to_string(i));
        if (i > 0) {
            objects.push_back("extruder" + std::to_string(i));
        }
        if (with_vrtx) {
            objects.push_back("gcode_macro T" + std::to_string(i));
        }
    }
    if (with_vrtx) {
        objects.push_back("gcode_macro _VRTX_T");
        objects.push_back("gcode_macro VRTX_CALIBRATE");
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

void wire(ToolChangerHelper& tc, const PrinterDiscovery& hw, int tools) {
    tc.set_swap_macro(toolchanger_addon::resolve_swap_macro(hw));
    json numbers = json::array();
    json names = json::array();
    for (int i = 0; i < tools; ++i) {
        numbers.push_back(i);
        names.push_back("tool T" + std::to_string(i));
    }
    tc.feed(json{{"toolchanger",
                  {{"status", "ready"},
                   {"tool_number", 0},
                   {"tool_numbers", numbers},
                   {"tool_names", names}}}});
}

} // namespace

TEST_CASE("VRTX is detected from its _VRTX_T macro beside klipper-toolchanger",
          "[toolchanger][vrtx]") {
    const auto rule = toolchanger_addon::resolve_swap_macro(vrtx_discovery(8));
    CHECK(rule.present);
    CHECK(rule.provider_name == "VRTX");
    CHECK(rule.has_command(7));

    CHECK_FALSE(toolchanger_addon::resolve_swap_macro(vrtx_discovery(8, false)).present);
}

TEST_CASE("VRTX answers only for the tool numbers whose T<n> macro exists", "[toolchanger][vrtx]") {
    json objects =
        json::array({"toolchanger", "tool T0", "tool T1", "gcode_macro _VRTX_T", "gcode_macro T0"});
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    const auto rule = toolchanger_addon::resolve_swap_macro(hw);
    CHECK(rule.has_command(0));
    CHECK_FALSE(rule.has_command(1));
}

TEST_CASE("a VRTX mount sends T<n>, not SELECT_TOOL", "[toolchanger][vrtx]") {
    ToolChangerHelper tc(8);
    wire(tc, vrtx_discovery(8), 8);

    REQUIRE(tc.change_tool(5).success());
    REQUIRE(tc.sent().size() == 1);
    CHECK(tc.sent()[0] == "T5");
}

TEST_CASE("a VRTX mount follows ASSIGN_TOOL: the tapped tool's own number", "[toolchanger][vrtx]") {
    ToolChangerHelper tc(4);
    wire(tc, vrtx_discovery(4), 4);
    // ASSIGN_TOOL TOOL="tool T2" N=0 and the reverse: tool T2 now answers to number 0.
    tc.feed(json{{"toolchanger",
                  {{"tool_numbers", {2, 1, 0, 3}},
                   {"tool_names", {"tool T0", "tool T1", "tool T2", "tool T3"}}}}});

    REQUIRE(tc.change_tool(2).success());
    CHECK(tc.sent().back() == "T0");
}

TEST_CASE("without VRTX a mount still names the tool object", "[toolchanger][vrtx]") {
    ToolChangerHelper tc(4);
    wire(tc, vrtx_discovery(4, false), 4);

    REQUIRE(tc.change_tool(3).success());
    CHECK(tc.sent().back() == "SELECT_TOOL TOOL=\"tool T3\"");
}

TEST_CASE("VRTX parks with klipper-toolchanger's UNSELECT_TOOL", "[toolchanger][vrtx]") {
    ToolChangerHelper tc(4);
    wire(tc, vrtx_discovery(4), 4);
    tc.feed_status("ready", 1);

    REQUIRE(tc.park_active_tool().success());
    CHECK(tc.sent().back() == "UNSELECT_TOOL");
}

TEST_CASE("VRTX_CALIBRATE offers the Tool Offsets panel", "[toolchanger][vrtx]") {
    namespace cal = helix::tool_offset_calibration;
    const auto procedure = cal::resolve_procedure(vrtx_discovery(8));
    CHECK(procedure.kind == cal::ProcedureKind::Panel);
    CHECK(procedure.gcode == "VRTX_CALIBRATE");
    CHECK(cal::supported(vrtx_discovery(8)));
}

TEST_CASE("without VRTX_CALIBRATE or CALIBRATE_TOOL_OFFSETS there is no offsets entry",
          "[toolchanger][vrtx]") {
    namespace cal = helix::tool_offset_calibration;
    CHECK_FALSE(cal::supported(vrtx_discovery(8, false)));
}

TEST_CASE("klipper-toolchanger's own macro wins when both exist", "[toolchanger][vrtx]") {
    namespace cal = helix::tool_offset_calibration;
    json objects = json::array({"toolchanger", "tool T0", "tool T1", "gcode_macro VRTX_CALIBRATE",
                                "gcode_macro CALIBRATE_TOOL_OFFSETS"});
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    CHECK(cal::resolve_procedure(hw).gcode == "CALIBRATE_TOOL_OFFSETS");
}
