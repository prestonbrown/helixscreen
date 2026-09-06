// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offset_calibration.cpp
 * @brief Tests for the automatic tool offset calibration module.
 *
 * klipper-toolchanger's CALIBRATE_TOOL_OFFSETS macro does the whole job in one
 * blocking gcode. helix::tool_offset_calibration owns its name and the
 * bookkeeping that turns what status already says - which tool is selected,
 * whose offsets were written, whether the rpc finished - into per-tool row
 * states. Nothing is parsed off the console.
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

// ============================================================================
// Run bookkeeping
// ============================================================================

TEST_CASE("tool offset calibration: a run queues every tool", "[tool_offset_cal]") {
    cal::Run run;
    CHECK_FALSE(run.active());
    CHECK(run.step(0) == cal::ToolStep::Idle);

    run.begin(3);

    CHECK(run.active());
    CHECK(run.tool_count() == 3);
    for (int t = 0; t < 3; ++t) {
        CHECK(run.step(t) == cal::ToolStep::Queued);
    }
    CHECK(run.step(3) == cal::ToolStep::Idle);
    CHECK(run.measuring_tool() == -1);
}

TEST_CASE("tool offset calibration: selection and offset writes walk the rows",
          "[tool_offset_cal]") {
    // The example macro's shape - one tool selected and used as the baseline
    // (no offsets written for it), every other tool selected then written,
    // and the first one re-selected to park - but nothing here depends on
    // that order: whatever tool is selected is Measuring, whatever tool is
    // written is Done.
    cal::Run run;
    run.begin(3);

    run.on_tool_selected(0);
    CHECK(run.step(0) == cal::ToolStep::Measuring);
    CHECK(run.measuring_tool() == 0);

    // The baseline tool gets no offset write; selecting the next one ends it.
    run.on_tool_selected(1);
    CHECK(run.step(0) == cal::ToolStep::Done);
    CHECK(run.step(1) == cal::ToolStep::Measuring);

    run.on_tool_measured(1);
    CHECK(run.step(1) == cal::ToolStep::Done);
    CHECK(run.measuring_tool() == -1);

    run.on_tool_selected(2);
    run.on_tool_measured(2);
    CHECK(run.step(2) == cal::ToolStep::Done);

    // The park: a Done tool stays Done.
    run.on_tool_selected(0);
    CHECK(run.step(0) == cal::ToolStep::Done);
    CHECK(run.measuring_tool() == -1);

    run.finish(true);
    CHECK_FALSE(run.active());
    CHECK_FALSE(run.failed());
}

TEST_CASE(
    "tool offset calibration: an offset write lands on the written tool, not the selected one",
    "[tool_offset_cal]") {
    // A macro may write a tool's offsets while another is on the carriage;
    // the write names its tool, so that is the row that finishes.
    cal::Run run;
    run.begin(3);
    run.on_tool_selected(2);
    run.on_tool_measured(1);

    CHECK(run.step(1) == cal::ToolStep::Done);
    CHECK(run.step(2) == cal::ToolStep::Measuring);
}

TEST_CASE("tool offset calibration: a successful finish completes every row", "[tool_offset_cal]") {
    // A run whose status updates we never saw still reports success through
    // the rpc; the macro does not return early.
    cal::Run run;
    run.begin(2);

    run.finish(true);

    CHECK(run.step(0) == cal::ToolStep::Done);
    CHECK(run.step(1) == cal::ToolStep::Done);
    CHECK_FALSE(run.failed());
}

TEST_CASE("tool offset calibration: a failure lands on the tool under the probe",
          "[tool_offset_cal]") {
    // The tool being measured is what failed; the ones after it were never
    // reached and read as if no run had happened; earlier tools keep their
    // result - the macro already wrote their offsets.
    cal::Run run;
    run.begin(3);
    run.on_tool_selected(0);
    run.on_tool_selected(1);

    run.finish(false);

    CHECK(run.step(0) == cal::ToolStep::Done);
    CHECK(run.step(1) == cal::ToolStep::Failed);
    CHECK(run.step(2) == cal::ToolStep::Idle);
    CHECK(run.failed());
    CHECK_FALSE(run.active());
}

TEST_CASE("tool offset calibration: an abort blames nobody", "[tool_offset_cal]") {
    // The user stopped it: not a failure on any tool, and nothing beyond what
    // already finished counts as done.
    cal::Run run;
    run.begin(3);
    run.on_tool_selected(0);
    run.on_tool_selected(1);

    run.abort();

    CHECK(run.step(0) == cal::ToolStep::Done);
    CHECK(run.step(1) == cal::ToolStep::Idle);
    CHECK(run.step(2) == cal::ToolStep::Idle);
    CHECK_FALSE(run.failed());
    CHECK_FALSE(run.active());
}

TEST_CASE("tool offset calibration: console lines outside a run are ignored", "[tool_offset_cal]") {
    // The toolchanger prints "Selected tool" on every tool change, run or no
    // run; a print's tool changes must not paint the calibration rows.
    cal::Run run;
    run.on_tool_selected(1);
    run.on_tool_measured(1);
    CHECK(run.step(1) == cal::ToolStep::Idle);

    run.begin(2);
    run.on_tool_selected(7); // no such tool
    CHECK(run.measuring_tool() == -1);
    run.finish(true);
    run.on_tool_selected(1);
    CHECK(run.step(1) == cal::ToolStep::Done);
}

TEST_CASE("tool offset calibration: the mounted tool starts under the probe", "[tool_offset_cal]") {
    // Selecting the tool that is already on the carriage changes nothing in
    // status, so a run that begins on the mounted tool would never see it
    // Measuring. The mounted tool is therefore assumed under the probe from
    // the start, and hands over like any selected tool.
    cal::Run run;
    run.begin(3, 0);
    CHECK(run.step(0) == cal::ToolStep::Measuring);
    CHECK(run.measuring_tool() == 0);
    CHECK(run.step(1) == cal::ToolStep::Queued);

    run.on_tool_selected(1);
    CHECK(run.step(0) == cal::ToolStep::Done);
    CHECK(run.step(1) == cal::ToolStep::Measuring);

    // No tool mounted, or one this printer does not have: assume nothing.
    cal::Run bare;
    bare.begin(3, -1);
    CHECK(bare.measuring_tool() == -1);
    cal::Run odd;
    odd.begin(3, 7);
    CHECK(odd.measuring_tool() == -1);
}
