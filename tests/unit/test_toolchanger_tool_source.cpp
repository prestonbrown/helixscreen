// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_tool_source.cpp
 * @brief Dock sensors, not toolchanger.tool_number, say what is on the head.
 *
 * Irbis3D/MedusaHC ships Macros/toolchanger.cfg with `verify_tool_pickup: False`
 * and `require_tool_present: False`, so klipper-toolchanger never checks what it
 * picked up: `toolchanger.tool_number` is only ever "what SELECT_TOOL last set".
 * The physical answer comes off dock sensors, via pin_watch.py, and the official
 * controller treats it as authoritative (`_current_tool()` returns
 * `pin_watch.current_tool`, and `sensor_error` is `current_tool == -2`).
 *
 * Two [medusahc] schemas exist and object presence cannot separate them, so the
 * reader discriminates on field names and reads the Irbis3D ones first:
 *
 *   Irbis3D MedusaHC-Python-Controller   operation / last_error / sensor_error / sensors
 *   third-party fork                     state     / error      / toolN_docked
 */

#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <algorithm>
#include <json.hpp>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::toolchanger_addon::read_tool;

namespace {

PrinterDiscovery medusahc_hw(bool official_macros = false) {
    PrinterDiscovery hw;
    auto objects = json::array(
        {"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3", "pin_watch io", "extruder"});
    if (official_macros) {
        objects.push_back("gcode_macro MHC_OPEN");
        objects.push_back("gcode_macro MHC_CLOSE");
    }
    hw.parse_objects(objects);
    return hw;
}

} // namespace

// ============================================================================
// Which objects get subscribed
// ============================================================================

TEST_CASE("A MedusaHC subscribes both its sensor objects", "[toolchanger][toolsource]") {
    auto objects = toolchanger_addon::required_status_objects(medusahc_hw());
    REQUIRE(objects.size() == 2);
    CHECK(objects[0] == "medusahc");
    // The real section name, not a guess: [pin_watch io] is what upstream ships,
    // but the name is configurable, so it has to come from discovery.
    CHECK(objects[1] == "pin_watch io");
}

TEST_CASE("A plain tool changer subscribes nothing extra", "[toolchanger][toolsource]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "extruder"}));
    CHECK(toolchanger_addon::required_status_objects(hw).empty());
}

// ============================================================================
// Reading the official Irbis3D schema
// ============================================================================

TEST_CASE("The official medusahc schema is read", "[toolchanger][toolsource]") {
    auto r = read_tool(json{{"medusahc",
                             {{"operation", "picking"},
                              {"current_tool", 2},
                              {"target_tool", 2},
                              {"last_error", ""},
                              {"feeder_open", false},
                              {"sensor_error", false},
                              {"tool_count", 4}}}});
    REQUIRE(r.has_value());
    CHECK(r->current_tool == 2);
    CHECK(r->tool_count == 4);
    CHECK(r->operation == "picking");
    CHECK_FALSE(r->sensor_error);
}

TEST_CASE("A dropping operation is distinguished from picking", "[toolchanger][toolsource]") {
    // klipper-toolchanger collapses both into "changing"; the direction is the
    // whole reason to prefer this source for the action.
    auto r = read_tool(json{{"medusahc", {{"operation", "dropping"}, {"current_tool", -1}}}});
    REQUIRE(r.has_value());
    CHECK(r->operation == "dropping");
    CHECK(r->current_tool == -1);
}

TEST_CASE("sensor_error is not the same as no tool", "[toolchanger][toolsource]") {
    // -2 means the sensors disagree. Treating it as -1 would invite a tool change
    // against an unknown carriage state.
    auto r = read_tool(json{{"medusahc", {{"current_tool", -2}}}});
    REQUIRE(r.has_value());
    CHECK(r->sensor_error);

    auto none = read_tool(json{{"medusahc", {{"current_tool", -1}}}});
    REQUIRE(none.has_value());
    CHECK_FALSE(none->sensor_error);
}

TEST_CASE("An explicit sensor_error flag is honoured", "[toolchanger][toolsource]") {
    auto r = read_tool(json{{"medusahc", {{"sensor_error", true}, {"current_tool", 1}}}});
    REQUIRE(r.has_value());
    CHECK(r->sensor_error);
}

// ============================================================================
// The fork schema is a fallback, never a default
// ============================================================================

TEST_CASE("The fork's 'state' field is read when 'operation' is absent",
          "[toolchanger][toolsource]") {
    auto r = read_tool(json{{"medusahc", {{"state", "changing"}, {"current_tool", 1}}}});
    REQUIRE(r.has_value());
    CHECK(r->operation == "changing");
    CHECK(r->current_tool == 1);
}

TEST_CASE("'operation' wins when a frame somehow carries both", "[toolchanger][toolsource]") {
    // Irbis3D's name is the default; the fork's is only a fallback.
    auto r = read_tool(
        json{{"medusahc", {{"operation", "idle"}, {"state", "changing"}, {"current_tool", 0}}}});
    REQUIRE(r.has_value());
    CHECK(r->operation == "idle");
}

// ============================================================================
// pin_watch alone (Irbis3D stock, no [medusahc] object at all)
// ============================================================================

TEST_CASE("pin_watch alone still gives the mounted tool", "[toolchanger][toolsource]") {
    // Config (a): the 250-star stock repo has no [medusahc]. pin_watch.py's
    // whole get_status() is {"current_tool": int}.
    auto r = read_tool(json{{"pin_watch io", {{"current_tool", 3}}}});
    REQUIRE(r.has_value());
    CHECK(r->current_tool == 3);
    CHECK_FALSE(r->sensor_error);
}

TEST_CASE("medusahc wins over pin_watch when both are in the frame", "[toolchanger][toolsource]") {
    // [medusahc] reads pin_watch itself and adds operation + sensor_error, so it
    // is strictly the better source.
    auto r = read_tool(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}}},
                            {"pin_watch io", {{"current_tool", 3}}}});
    REQUIRE(r.has_value());
    CHECK(r->current_tool == 2);
}

// ============================================================================
// No news is not "cleared"
// ============================================================================

TEST_CASE("A frame with none of our fields reads as no news", "[toolchanger][toolsource]") {
    // Moonraker only republishes CHANGED fields. Returning a default reading here
    // would clear the mounted tool on every unrelated status update.
    CHECK_FALSE(read_tool(json{{"toolchanger", {{"status", "ready"}}}}).has_value());
    CHECK_FALSE(read_tool(json{{"medusahc", json::object()}}).has_value());
    CHECK_FALSE(read_tool(json::object()).has_value());
    CHECK_FALSE(read_tool(json::array()).has_value());
}

// ============================================================================
// Feeder macro selection
// ============================================================================

TEST_CASE("The feeder prefers the controller's native command",
          "[toolchanger][toolsource][feeder]") {
    // The official build registers MHC_OPEN/MHC_CLOSE and ships legacy OPEN/CLOSE
    // aliases forwarding to them, so both work there. Prefer the native one.
    auto official = toolchanger_addon::resolve_feeder(medusahc_hw(/*official_macros=*/true));
    REQUIRE(official.present);
    CHECK(official.open_gcode == "MHC_OPEN");
    CHECK(official.close_gcode == "MHC_CLOSE");

    // Stock config has only the plain macros.
    auto stock = toolchanger_addon::resolve_feeder(medusahc_hw(/*official_macros=*/false));
    REQUIRE(stock.present);
    CHECK(stock.open_gcode == "OPEN");
    CHECK(stock.close_gcode == "CLOSE");
}

// ============================================================================
// Feeder macro override
// ============================================================================

TEST_CASE("The macro picker offers only plausible macros", "[toolchanger][feeder][macro]") {
    PrinterDiscovery hw;
    hw.parse_objects(
        json::array({"toolchanger", "tool T0", "pin_watch io", "extruder", "gcode_macro MHC_OPEN",
                     "gcode_macro MHC_CLOSE", "gcode_macro OPEN", "gcode_macro CLOSE",
                     // Noise: a printer has hundreds of these.
                     "gcode_macro PRINT_START", "gcode_macro BED_MESH_CALIBRATE", "gcode_macro G32",
                     "gcode_macro LOAD_FILAMENT"}));

    auto candidates = toolchanger_addon::feeder_macro_candidates(hw);
    CHECK(std::find(candidates.begin(), candidates.end(), "MHC_OPEN") != candidates.end());
    CHECK(std::find(candidates.begin(), candidates.end(), "CLOSE") != candidates.end());
    CHECK(std::find(candidates.begin(), candidates.end(), "PRINT_START") == candidates.end());
    CHECK(std::find(candidates.begin(), candidates.end(), "G32") == candidates.end());
}

TEST_CASE("A plain tool changer offers no macro picker", "[toolchanger][feeder][macro]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "gcode_macro OPEN"}));
    CHECK(toolchanger_addon::feeder_macro_candidates(hw).empty());
    CHECK(toolchanger_addon::resolve_feeder(hw).macro_options.empty());
}

TEST_CASE("An override replaces the detected macro", "[toolchanger][feeder][macro]") {
    auto hw = medusahc_hw(/*official_macros=*/true);
    auto f = toolchanger_addon::resolve_feeder(hw, "CUSTOM_RELEASE", "CUSTOM_GRIP");

    CHECK(f.open_gcode == "CUSTOM_RELEASE");
    CHECK(f.close_gcode == "CUSTOM_GRIP");
    CHECK(f.open_choice == "CUSTOM_RELEASE");
    // The detected default is kept so picking "auto" again restores it.
    CHECK(f.detected_open == "MHC_OPEN");
    CHECK(f.detected_close == "MHC_CLOSE");
}

TEST_CASE("'auto' and empty both mean the detected default", "[toolchanger][feeder][macro]") {
    auto hw = medusahc_hw(/*official_macros=*/true);

    auto explicit_auto = toolchanger_addon::resolve_feeder(hw, "auto", "auto");
    CHECK(explicit_auto.open_gcode == "MHC_OPEN");
    CHECK(explicit_auto.open_choice == "auto");

    // A never-set setting reads back empty on some config paths; it must not
    // blank the gcode and silently break the button.
    auto empty = toolchanger_addon::resolve_feeder(hw, "", "");
    CHECK(empty.open_gcode == "MHC_OPEN");
    CHECK(empty.close_gcode == "MHC_CLOSE");
    CHECK(empty.open_choice == "auto");
}

TEST_CASE("The picker's first option is the auto sentinel", "[toolchanger][feeder][macro]") {
    auto f = toolchanger_addon::resolve_feeder(medusahc_hw(/*official_macros=*/true));
    REQUIRE_FALSE(f.macro_options.empty());
    CHECK(f.macro_options[0] == std::string(toolchanger_addon::kAutoMacro));
}
