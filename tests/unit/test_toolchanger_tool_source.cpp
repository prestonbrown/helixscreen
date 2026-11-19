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
 * The controller's [medusahc] status carries operation / last_error /
 * sensor_error / sensors.
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

TEST_CASE("The feeder uses the plain OPEN/CLOSE names on every MedusaHC build",
          "[toolchanger][toolsource][feeder]") {
    // The controller's OPEN/CLOSE forward to its MHC_* commands and work on a
    // parked tool, and the stock config has only the plain names.
    for (const bool official : {true, false}) {
        CAPTURE(official);
        auto feeder = toolchanger_addon::resolve_feeder(medusahc_hw(official));
        REQUIRE(feeder.present);
        CHECK(feeder.open_gcode == "OPEN");
        CHECK(feeder.close_gcode == "CLOSE");
    }
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
    CHECK(f.detected_open == "OPEN");
    CHECK(f.detected_close == "CLOSE");
}

TEST_CASE("'auto' and empty both mean the detected default", "[toolchanger][feeder][macro]") {
    auto hw = medusahc_hw(/*official_macros=*/true);

    auto explicit_auto = toolchanger_addon::resolve_feeder(hw, "auto", "auto");
    CHECK(explicit_auto.open_gcode == "OPEN");
    CHECK(explicit_auto.open_choice == "auto");

    // A never-set setting reads back empty on some config paths; it must not
    // blank the gcode and silently break the button.
    auto empty = toolchanger_addon::resolve_feeder(hw, "", "");
    CHECK(empty.open_gcode == "OPEN");
    CHECK(empty.close_gcode == "CLOSE");
    CHECK(empty.open_choice == "auto");
}

TEST_CASE("The picker's first option is the auto sentinel", "[toolchanger][feeder][macro]") {
    auto f = toolchanger_addon::resolve_feeder(medusahc_hw(/*official_macros=*/true));
    REQUIRE_FALSE(f.macro_options.empty());
    CHECK(f.macro_options[0] == std::string(toolchanger_addon::kAutoMacro));
}

// ============================================================================
// Per-dock state
//
// Sergei (Irbis3D, 2026-08-24) on what `sensors` carries: "e is the toolhead
// sensor, t0, t1, t2 ... are the sensors for the corresponding docks. e: 1 means
// a tool is installed on the toolhead. tN: 1 means the corresponding tool is
// seated in its dock." So a dock reading 0 for a tool that is not on the head
// means that hot end is physically absent, which is a state the tool number
// alone cannot express.
// ============================================================================

TEST_CASE("The sensors dict gives per-dock occupancy", "[toolchanger][toolsource][docks]") {
    // Sergei's own example: T3 on the head, everything else docked.
    auto r = read_tool(
        json{{"medusahc",
              {{"operation", "idle"},
               {"current_tool", 3},
               {"tool_count", 6},
               {"sensors",
                {{"e", 1}, {"t5", 1}, {"t4", 1}, {"t3", 0}, {"t2", 1}, {"t1", 1}, {"t0", 1}}}}}});
    REQUIRE(r.has_value());
    REQUIRE(r->docks.size() == 6);
    CHECK(r->docks[0] == true);
    CHECK(r->docks[3] == false); // on the head, so its dock is empty
    CHECK(r->docks[5] == true);
    REQUIRE(r->head_loaded.has_value());
    CHECK(*r->head_loaded);
}

TEST_CASE("An empty toolhead sensor reads as no tool on the head",
          "[toolchanger][toolsource][docks]") {
    auto r = read_tool(
        json{{"medusahc", {{"current_tool", -1}, {"sensors", {{"e", 0}, {"t0", 1}, {"t1", 1}}}}}});
    REQUIRE(r.has_value());
    REQUIRE(r->head_loaded.has_value());
    CHECK_FALSE(*r->head_loaded);
    CHECK(r->docks.size() == 2);
}

TEST_CASE("A dock nobody reported stays unknown, not empty", "[toolchanger][toolsource][docks]") {
    // Moonraker only republishes fields that CHANGED, so a frame carrying two
    // docks says nothing about the third. Defaulting the gap to "empty" would
    // stamp a tool missing on every partial update.
    auto r = read_tool(json{{"medusahc", {{"sensors", {{"t0", 1}, {"t2", 0}}}}}});
    REQUIRE(r.has_value());
    REQUIRE(r->docks.size() == 3);
    CHECK(r->docks[0] == true);
    CHECK_FALSE(r->docks[1].has_value());
    CHECK(r->docks[2] == false);
}

TEST_CASE("A frame with no dock fields reports no docks at all",
          "[toolchanger][toolsource][docks]") {
    // Empty, not "all empty" — the caller must be able to tell a frame that said
    // nothing from one that said every dock is vacant.
    auto r = read_tool(json{{"medusahc", {{"current_tool", 0}, {"operation", "idle"}}}});
    REQUIRE(r.has_value());
    CHECK(r->docks.empty());
    CHECK_FALSE(r->head_loaded.has_value());
}

TEST_CASE("The controller's target tool is read, and absent means no news",
          "[toolchanger][toolsource][last_error]") {
    auto r = read_tool(json{{"medusahc", {{"target_tool", 2}}}});
    REQUIRE(r.has_value());
    REQUIRE(r->target_tool.has_value());
    CHECK(*r->target_tool == 2);

    auto silent = read_tool(json{{"medusahc", {{"current_tool", 1}}}});
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->target_tool.has_value());
}

// ============================================================================
// Who owns the swap
// ============================================================================

TEST_CASE("A MedusaHC beside klipper-toolchanger still swaps with its own commands",
          "[toolchanger][toolsource][commands]") {
    // klipper-toolchanger only mirrors the detected tool there: it keeps the
    // machine's own T<n> macros and its UNSELECT_TOOL runs an empty
    // dropoff_gcode. Its INITIALIZE_TOOLCHANGER does exist.
    auto cmds = toolchanger_addon::resolve_tool_commands(medusahc_hw());
    REQUIRE(cmds.present);
    CHECK(cmds.select_prefix == "T");
    CHECK(cmds.unselect == "DROP_TOOL");
    CHECK(cmds.initialize_available);
}

TEST_CASE("A plain klipper-toolchanger keeps its own select commands",
          "[toolchanger][toolsource][commands]") {
    // An absent capability is the backend's signal to keep using
    // SELECT_TOOL/UNSELECT_TOOL.
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "extruder"}));
    auto cmds = toolchanger_addon::resolve_tool_commands(hw);
    CHECK_FALSE(cmds.present);
    CHECK(cmds.select_prefix.empty());
    CHECK(cmds.unselect.empty());
}

TEST_CASE("A toolchanger view whose SELECT_TOOL takes tool numbers gets its own commands",
          "[toolchanger][toolsource][commands]") {
    // [ff_toolchange] publishes toolchanger and tool T<n> as read-only views
    // over its own swap code, which parses T=<n> or TOOL=T<n> only and has no
    // INITIALIZE_TOOLCHANGER or ASSIGN_TOOL.
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "ff_tool 0", "ff_tool 1",
                                  "ff_toolchange", "ff_tool_offset", "extruder", "extruder1"}));

    auto cmds = toolchanger_addon::resolve_tool_commands(hw);
    REQUIRE(cmds.present);
    CHECK(cmds.select_prefix == "SELECT_TOOL T=");
    CHECK(cmds.unselect == "UNSELECT_TOOL");
    CHECK_FALSE(cmds.initialize_available);

    // Its toolchanger.tool_number is already read off the dock sensors, so
    // there is no add-on sensor object to subscribe or prefer over it.
    CHECK_FALSE(toolchanger_addon::resolve_tool_sensor(hw).present);
    CHECK(toolchanger_addon::required_status_objects(hw).empty());
}

TEST_CASE("A changer without klipper-toolchanger names its own commands",
          "[toolchanger][toolsource][commands]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"pin_watch io", "medusahc", "gcode_macro T0", "gcode_macro T1",
                                  "gcode_macro DROP_TOOL", "extruder", "extruder1", "toolhead"}));

    auto cmds = toolchanger_addon::resolve_tool_commands(hw);
    REQUIRE(cmds.present);
    // SELECT_TOOL does not exist on this machine; the T<n> macros the extra
    // registers are the swap.
    CHECK(cmds.select_prefix == "T");
    CHECK(cmds.unselect == "DROP_TOOL");
    CHECK_FALSE(cmds.initialize_available);
}

TEST_CASE("A MedusaHC without a DROP_TOOL macro parks with the controller's own commands",
          "[toolchanger][toolsource][commands]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"pin_watch io", "medusahc", "gcode_macro T0", "gcode_macro T1",
                                  "extruder", "extruder1", "toolhead"}));

    auto cmds = toolchanger_addon::resolve_tool_commands(hw);
    REQUIRE(cmds.present);
    CHECK(cmds.unselect == "MHC_DROP\nMHC_CLOSE");
}

TEST_CASE("A printer with no changer module at all swaps with plain T<n>",
          "[toolchanger][toolsource][commands]") {
    // Two hot ends and nothing that calls itself a changer. SELECT_TOOL exists
    // only where klipper-toolchanger does, so naming it here would send a
    // command this printer does not have; T<n> is what Klipper maps to
    // ACTIVATE_EXTRUDER.
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "extruder1", "toolhead"}));

    auto cmds = toolchanger_addon::resolve_tool_commands(hw);
    REQUIRE(cmds.present);
    CHECK(cmds.select_prefix == "T");
    // Only the provider table names an unmount, and no provider claims this
    // machine: its tool can only be swapped for another.
    CHECK(cmds.unselect.empty());
}

// ============================================================================
// Which -2 readings are faults
// ============================================================================

TEST_CASE("A swap in flight makes a derived -2 transitional, not a fault",
          "[toolchanger][toolsource]") {
    using helix::toolchanger_addon::sensor_error_is_fault;

    // Derived from -2: the controller published no flag of its own.
    auto derived = read_tool(json{{"medusahc", {{"operation", "changing"}, {"current_tool", -2}}}});
    REQUIRE(derived.has_value());
    REQUIRE(derived->sensor_error);
    REQUIRE_FALSE(derived->sensor_error_reported);

    CHECK_FALSE(sensor_error_is_fault(*derived, /*swap_in_flight=*/true));
    CHECK(sensor_error_is_fault(*derived, /*swap_in_flight=*/false));
}

TEST_CASE("A published sensor_error is a fault even mid-swap", "[toolchanger][toolsource]") {
    using helix::toolchanger_addon::sensor_error_is_fault;

    auto flagged = read_tool(json{{"medusahc", {{"sensor_error", true}, {"current_tool", 1}}}});
    REQUIRE(flagged.has_value());
    REQUIRE(flagged->sensor_error_reported);
    CHECK(sensor_error_is_fault(*flagged, /*swap_in_flight=*/true));

    // At rest without a carriage reading, nothing contradicts the flag.
    auto bare = read_tool(json{{"medusahc", {{"sensor_error", true}}}});
    REQUIRE(bare.has_value());
    CHECK(sensor_error_is_fault(*bare, /*swap_in_flight=*/false));
}

TEST_CASE("A published sensor_error at rest beside a definite carriage is a past failure",
          "[toolchanger][toolsource]") {
    using helix::toolchanger_addon::sensor_error_is_fault;

    for (int tool : {-1, 0, 2}) {
        CAPTURE(tool);
        auto r = read_tool(json{{"medusahc", {{"sensor_error", true}, {"current_tool", tool}}}});
        REQUIRE(r.has_value());
        REQUIRE(r->sensor_error_reported);
        CHECK_FALSE(sensor_error_is_fault(*r, /*swap_in_flight=*/false));
    }
    auto unknown = read_tool(json{{"medusahc", {{"sensor_error", true}, {"current_tool", -2}}}});
    REQUIRE(unknown.has_value());
    CHECK(sensor_error_is_fault(*unknown, /*swap_in_flight=*/false));
}

TEST_CASE("A clean reading is never a fault", "[toolchanger][toolsource]") {
    using helix::toolchanger_addon::sensor_error_is_fault;

    auto clean = read_tool(json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    REQUIRE(clean.has_value());
    REQUIRE_FALSE(clean->sensor_error);

    CHECK_FALSE(sensor_error_is_fault(*clean, /*swap_in_flight=*/true));
    CHECK_FALSE(sensor_error_is_fault(*clean, /*swap_in_flight=*/false));
}
