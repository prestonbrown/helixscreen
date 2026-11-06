// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_stepper_resonance_calibration.cpp
 * @brief The stepper resonance ("Vertical Artifact Compensation") procedure
 *        table: which routine a printer gets, and the gcode it sends.
 */

#include "printer_discovery.h"
#include "stepper_resonance_calibration.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace cal = helix::stepper_resonance_cal;

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

constexpr const char* kTester = "stepper_resonance_tester";
constexpr const char* kFactoryMacro = "gcode_macro STEPPER_RESONANCE_FACTORY_CALIBRATE";
constexpr const char* kWrapperMacro = "gcode_macro CALIBRATE_VFA";
constexpr const char* kCompensationMacro = "gcode_macro VFA_COMPENSATION";

} // namespace

TEST_CASE("stepper resonance cal: the CALIBRATE_VFA wrapper alone runs per tool",
          "[stepper_resonance_cal]") {
    const PrinterDiscovery hw = printer_with({kWrapperMacro});
    const cal::Procedure p = cal::resolve(hw);
    REQUIRE(p.present());
    CHECK(p.per_tool);
    CHECK(p.needs_home);
    CHECK(p.restarts_klipper);
    CHECK(cal::calibrate_gcode(hw, 0) == "CALIBRATE_VFA T=0");
    CHECK(cal::calibrate_gcode(hw, 3) == "CALIBRATE_VFA T=3");
    // A per-tool routine needs a tool.
    CHECK(cal::calibrate_gcode(hw, -1).empty());
}

TEST_CASE("stepper resonance cal: the bare extra runs the stock macro, then saves",
          "[stepper_resonance_cal]") {
    const PrinterDiscovery hw = printer_with({kTester, kFactoryMacro});
    const cal::Procedure p = cal::resolve(hw);
    REQUIRE(p.present());
    CHECK_FALSE(p.per_tool);
    CHECK(p.needs_home);
    CHECK(p.restarts_klipper);
    CHECK(cal::calibrate_gcode(hw, 0) == "STEPPER_RESONANCE_FACTORY_CALIBRATE\nSAVE_CONFIG");
    // Whole-machine: the tool is ignored, even a negative one.
    CHECK(cal::calibrate_gcode(hw, -1) == cal::calibrate_gcode(hw, 2));
}

TEST_CASE("stepper resonance cal: without the stock macro, each XY motor is searched",
          "[stepper_resonance_cal]") {
    const PrinterDiscovery hw = printer_with({kTester});
    REQUIRE(cal::resolve(hw).present());
    CHECK(cal::calibrate_gcode(hw, 0) == "STEPPER_RESONANCE_SEARCH_CALIBRATE STEPPER=stepper_x\n"
                                         "STEPPER_RESONANCE_SEARCH_CALIBRATE STEPPER=stepper_y\n"
                                         "SAVE_CONFIG");
}

TEST_CASE("stepper resonance cal: the wrapper outranks the extra it drives",
          "[stepper_resonance_cal]") {
    const PrinterDiscovery hw = printer_with({kTester, kFactoryMacro, kWrapperMacro});
    CHECK(cal::resolve(hw).per_tool);
    CHECK(cal::calibrate_gcode(hw, 1) == "CALIBRATE_VFA T=1");
}

TEST_CASE("stepper resonance cal: OpenCreator's VFA_COMPENSATION outranks the extra it wraps",
          "[stepper_resonance_cal]") {
    // OpenCreator's printer.vibration.cfg declares [stepper_resonance_tester],
    // STEPPER_RESONANCE_FACTORY_CALIBRATE and the VFA_COMPENSATION wrapper,
    // which refuses unless every head is docked, homes, parks at
    // Z100/X130/Y130 and saves.
    const PrinterDiscovery hw = printer_with({kTester, kFactoryMacro, kCompensationMacro});
    const cal::Procedure p = cal::resolve(hw);
    REQUIRE(p.present());
    CHECK_FALSE(p.per_tool);
    CHECK_FALSE(p.needs_home);
    CHECK(p.restarts_klipper);
    CHECK(cal::calibrate_gcode(hw, 0) == "VFA_COMPENSATION");
    CHECK(cal::calibrate_gcode(hw, -1) == "VFA_COMPENSATION");
}

TEST_CASE("stepper resonance cal: the stock macro alone is not the routine",
          "[stepper_resonance_cal]") {
    // The macro's search command comes from the extra; without it there is
    // nothing to run.
    const PrinterDiscovery macro_only = printer_with({kFactoryMacro});
    CHECK_FALSE(cal::resolve(macro_only).present());
    CHECK(cal::calibrate_gcode(macro_only, 0).empty());

    const PrinterDiscovery bare = printer_with({});
    const cal::Procedure none = cal::resolve(bare);
    CHECK_FALSE(none.present());
    CHECK_FALSE(none.per_tool);
    CHECK_FALSE(none.restarts_klipper);
    CHECK(std::string(none.name).empty());
    CHECK(cal::calibrate_gcode(bare, 0).empty());
}
