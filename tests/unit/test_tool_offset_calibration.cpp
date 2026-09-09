// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offset_calibration.cpp
 * @brief Tests for the automatic tool offset calibration module.
 *
 * klipper-toolchanger's CALIBRATE_TOOL_OFFSETS macro does the whole job in one
 * blocking gcode. helix::tool_offset_calibration owns its name and the
 * capability question; the rpc's completion is the run's.
 */

#include "printer_discovery.h"
#include "tool_offset_calibration.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace cal = helix::tool_offset_calibration;

namespace {

PrinterDiscovery printer_with(std::initializer_list<const char*> extra) {
    json objects = json::array({"gcode_move", "toolhead", "extruder"});
    for (const char* o : extra) {
        objects.push_back(o);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// A klipper-toolchanger with the example config's calibration macro.
PrinterDiscovery calibrating_toolchanger() {
    return printer_with({"toolchanger", "tool T0", "tool T1", "tool T2",
                         "gcode_macro CALIBRATE_TOOL_OFFSETS", "tools_calibrate"});
}

} // namespace

// ============================================================================
// Capability
// ============================================================================

TEST_CASE("tool offset calibration: needs a tool changer AND the macro", "[tool_offset_cal]") {
    // A tool changer without the macro has nothing to run; a single-toolhead
    // printer that happens to define the macro has nothing to calibrate.
    CHECK(cal::supported(calibrating_toolchanger()));
    CHECK_FALSE(cal::supported(printer_with({"toolchanger", "tool T0", "tool T1"})));
    CHECK_FALSE(cal::supported(printer_with({"gcode_macro CALIBRATE_TOOL_OFFSETS"})));
    CHECK_FALSE(cal::supported(printer_with({})));
}

TEST_CASE("tool offset calibration: the macro name is case-insensitive on detection",
          "[tool_offset_cal]") {
    // Klipper uppercases the command alias whatever case the section uses.
    PrinterDiscovery hw =
        printer_with({"toolchanger", "tool T0", "tool T1", "gcode_macro Calibrate_Tool_Offsets"});

    CHECK(cal::supported(hw));
    CHECK(cal::calibrate_all_gcode(hw) == "CALIBRATE_TOOL_OFFSETS");
}

TEST_CASE("tool offset calibration: the gcode is the bare macro, or nothing", "[tool_offset_cal]") {
    CHECK(cal::calibrate_all_gcode(calibrating_toolchanger()) == "CALIBRATE_TOOL_OFFSETS");
    CHECK(cal::calibrate_all_gcode(printer_with({"toolchanger", "tool T0"})).empty());
}
