// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_medusahc_detection.cpp
 * @brief MedusaHC is a capability on top of a tool changer, never its own AMS.
 *
 * Upstream MedusaHC (Irbis3D/MedusaHC) is a klipper-toolchanger printer:
 * printer.cfg includes toolchanger.cfg, which declares [toolchanger] and
 * [tool T0..T3]. AmsBackendToolChanger already drives it. The only thing
 * MedusaHC adds that we cannot see from those objects is its own [pin_watch io]
 * extra, which is NOT a stock Klipper module (absent from all 136 files in
 * Klipper3d/klipper klippy/extras), so the object name is a real signature.
 *
 * So detection here answers "is this tool changer a MedusaHC" and nothing more.
 * It must never touch has_mmu_/mmu_type_: those select which AMS backend gets
 * built, and an unguarded write there silently replaces a working backend. The
 * regression matrix below is the guard — every one of those cases was measured
 * misdetecting when the signal was wired as an MMU type instead of a capability.
 */

#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <json.hpp> // nlohmann/json from libhv

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// Object list a stock upstream MedusaHC reports. Sources, all on
/// Irbis3D/MedusaHC@main: Macros/toolchanger.cfg ([toolchanger] + [tool T0..T3]),
/// Macros/MHC_config.cfg ([pin_watch io], [gcode_macro T0..T3], [servo my_servo],
/// [extruder]..[extruder3]). printer.cfg:20 includes toolchanger.cfg uncommented,
/// so these objects are live on a default install.
json upstream_medusahc_objects() {
    return json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3", "pin_watch io",
                        "servo my_servo", "gcode_macro T0", "gcode_macro T1", "gcode_macro T2",
                        "gcode_macro T3", "extruder", "extruder1", "extruder2", "extruder3",
                        "toolhead", "gcode_move", "configfile"});
}

/// Object list from a MedusaHC fork that folded the dock sensing into its own
/// extra: [medusahc] carries all seven switch pins and there is no pin_watch and
/// no klipper-toolchanger left. Verbatim shape from debug bundle 6QWNVZY5
/// (topi314/MedusaHC on a DuCR10, HelixScreen 0.99.116), trimmed to what
/// detection reads.
json fork_medusahc_objects() {
    return json::array(
        {"medusahc",         "medusahc_calibrate", "gcode_macro T0", "gcode_macro T1",
         "gcode_macro T2",   "gcode_macro T3",     "gcode_macro T4", "gcode_macro T5",
         "gcode_macro OPEN", "gcode_macro CLOSE",  "servo opener",   "extruder",
         "extruder1",        "extruder2",          "extruder3",      "extruder4",
         "extruder5",        "toolhead",           "gcode_move",     "configfile"});
}

} // namespace

// ============================================================================
// The capability itself
// ============================================================================

TEST_CASE("MedusaHC is detected from pin_watch alongside a tool changer",
          "[printer_discovery][medusahc]") {
    PrinterDiscovery hw;
    hw.parse_objects(upstream_medusahc_objects());

    REQUIRE(helix::toolchanger_addon::present(hw));
    REQUIRE(hw.has_tool_changer());
    REQUIRE(hw.tool_names().size() == 4);
}

TEST_CASE("MedusaHC detection leaves AMS selection on the tool changer",
          "[printer_discovery][medusahc]") {
    PrinterDiscovery hw;
    hw.parse_objects(upstream_medusahc_objects());

    // The whole point: AmsBackendToolChanger still gets built. A MedusaHC-typed
    // AMS would mean a backend subscribed to an object this printer does not have.
    REQUIRE(hw.mmu_type() == AmsType::TOOL_CHANGER);
    REQUIRE_FALSE(hw.has_mmu());

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::TOOL_CHANGER);
}

TEST_CASE("pin_watch without a tool changer is not a MedusaHC", "[printer_discovery][medusahc]") {
    // pin_watch.py can be installed on its own. Without [toolchanger] there is
    // no tool changer to attribute it to, and claiming MedusaHC would enable a
    // feeder this machine has no macros for.
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"pin_watch io", "extruder", "toolhead"}));

    REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
    REQUIRE(hw.mmu_type() == AmsType::NONE);
    REQUIRE(hw.detected_ams_systems().empty());
}

TEST_CASE("a plain klipper-toolchanger is not a MedusaHC", "[printer_discovery][medusahc]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "extruder"}));

    REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
    REQUIRE(hw.mmu_type() == AmsType::TOOL_CHANGER);
}

TEST_CASE("pin_watch matching is exact, not a prefix", "[printer_discovery][medusahc]") {
    // A prefix test (rfind(name, 0) == 0) would swallow any future third-party
    // object whose name merely starts with "pin_watch".
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "pin_watchdog", "extruder"}));

    REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
    REQUIRE(hw.mmu_type() == AmsType::TOOL_CHANGER);
}

TEST_CASE("MedusaHC capability resets between parses", "[printer_discovery][medusahc]") {
    PrinterDiscovery hw;
    hw.parse_objects(upstream_medusahc_objects());
    REQUIRE(helix::toolchanger_addon::present(hw));

    hw.parse_objects(json::array({"extruder", "toolhead"}));
    REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
}

// ============================================================================
// The changer that runs without klipper-toolchanger
// ============================================================================

TEST_CASE("a MedusaHC with neither pin_watch nor toolchanger is still a changer",
          "[printer_discovery][medusahc]") {
    PrinterDiscovery hw;
    hw.parse_objects(fork_medusahc_objects());

    REQUIRE(helix::toolchanger_addon::present(hw));
    CHECK(hw.has_medusahc());
    // The [toolchanger] object genuinely is not there. The flag says what the
    // object list said, and nothing else may quietly widen it.
    CHECK_FALSE(hw.has_tool_changer());
    CHECK_FALSE(hw.has_pin_watch());
}

TEST_CASE("a changer without tool objects still registers a tool changer AMS",
          "[printer_discovery][medusahc]") {
    PrinterDiscovery hw;
    hw.parse_objects(fork_medusahc_objects());

    REQUIRE(hw.mmu_type() == AmsType::TOOL_CHANGER);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    CHECK(hw.detected_ams_systems()[0].type == AmsType::TOOL_CHANGER);
}

TEST_CASE("tools are enumerated from the extruders when there are no tool objects",
          "[printer_discovery][medusahc]") {
    PrinterDiscovery hw;
    hw.parse_objects(fork_medusahc_objects());

    // One hot end per extruder heater, which is what a hotend changer is. The
    // names are the G-code tool numbers the machine's own T<n> macros use.
    REQUIRE(hw.tool_names().size() == 6);
    CHECK(hw.tool_names()[0] == "T0");
    CHECK(hw.tool_names()[5] == "T5");
}

TEST_CASE("real tool objects are never overwritten by the enumeration",
          "[printer_discovery][medusahc]") {
    PrinterDiscovery hw;
    hw.parse_objects(upstream_medusahc_objects());

    // Stock upstream has [tool T0..T3] AND four extruders. The objects win: a
    // klipper-toolchanger name can be anything, and ASSIGN_TOOL can remap it.
    REQUIRE(hw.tool_names().size() == 4);
    CHECK(hw.tool_names()[0] == "T0");
    CHECK(hw.tool_names()[3] == "T3");
}

TEST_CASE("medusahc_calibrate alone does not make a changer", "[printer_discovery][medusahc]") {
    // Object matching is exact, as it is for pin_watch. A sibling object whose
    // name merely starts with the same characters is not the changer.
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"medusahc_calibrate", "extruder", "toolhead"}));

    CHECK_FALSE(hw.has_medusahc());
    CHECK_FALSE(helix::toolchanger_addon::present(hw));
    CHECK(hw.detected_ams_systems().empty());
}

// ============================================================================
// Regression matrix — none of these may be re-typed by MedusaHC detection
// ============================================================================

TEST_CASE("MedusaHC detection never displaces another AMS system",
          "[printer_discovery][medusahc][regression]") {
    // Every case here was measured resolving to MedusaHC when pin_watch and
    // `T<n>` macros were wired as an MMU type. They are load-bearing: each one
    // is a shipping printer family whose backend would be swapped out.

    SECTION("Snapmaker U1 — filament_detect plus its own T4..T31 macros") {
        // Verbatim shape of the U1 capture in
        // tests/unit/test_printer_discovery_real_hardware.cpp.
        PrinterDiscovery hw;
        hw.parse_objects(
            json::array({"filament_detect", "gcode_macro T4", "gcode_macro T5", "gcode_macro T6",
                         "gcode_macro T31", "extruder", "toolhead"}));

        REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
        REQUIRE(hw.mmu_type() == AmsType::SNAPMAKER);
    }

    SECTION("Happy Hare") {
        PrinterDiscovery hw;
        hw.parse_objects(json::array({"mmu", "gcode_macro T0", "gcode_macro T1", "toolhead"}));

        REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
        REQUIRE(hw.mmu_type() == AmsType::HAPPY_HARE);
    }

    SECTION("AD5X IFS") {
        PrinterDiscovery hw;
        hw.parse_objects(json::array(
            {"filament_motion_sensor ifs_motion_sensor", "gcode_macro T0", "extruder"}));

        REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
        REQUIRE(hw.mmu_type() == AmsType::AD5X_IFS);
    }

    SECTION("QIDI Box") {
        PrinterDiscovery hw;
        hw.parse_objects(
            json::array({"box_stepper slot0", "box_stepper slot1", "gcode_macro T0", "extruder"}));

        REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
        REQUIRE(hw.mmu_type() == AmsType::QIDI_BOX);
    }

    SECTION("plain IDEX with T0/T1 macros and no MMU at all") {
        PrinterDiscovery hw;
        hw.parse_objects(
            json::array({"extruder", "extruder1", "gcode_macro T0", "gcode_macro T1"}));

        REQUIRE_FALSE(helix::toolchanger_addon::present(hw));
        REQUIRE(hw.mmu_type() == AmsType::NONE);
        REQUIRE(hw.detected_ams_systems().empty());
    }

    SECTION("an MMU on a machine that also runs pin_watch keeps its own backend") {
        // The strongest form: the MedusaHC signal is present AND another system
        // owns filament. Filament management must win; MedusaHC is only ever a
        // tool-changer capability.
        PrinterDiscovery hw;
        hw.parse_objects(json::array({"mmu", "pin_watch io", "toolchanger", "tool T0", "tool T1"}));

        REQUIRE(hw.mmu_type() == AmsType::HAPPY_HARE);
        const auto& systems = hw.detected_ams_systems();
        REQUIRE(systems.size() == 1);
        REQUIRE(systems[0].type == AmsType::HAPPY_HARE);
    }
}
