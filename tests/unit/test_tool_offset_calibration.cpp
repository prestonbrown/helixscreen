// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offset_calibration.cpp
 * @brief Tests for the automatic tool offset calibration module.
 *
 * klipper-toolchanger's CALIBRATE_TOOL_OFFSETS macro does the whole job in one
 * blocking gcode. helix::tool_offset_calibration owns its name and the
 * capability question; the rpc's completion is the run's.
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "lvgl.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "snapmaker_nozzle_offset.h"
#include "system_settings_manager.h"
#include "tool_offset_calibration.h"

#include <string>
#include <vector>

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

/// A Snapmaker U1, whose firmware ships the per-head offset calibration macros.
PrinterDiscovery snapmaker_u1() {
    json objects = json::array({"gcode_move", "toolhead", "extruder",
                                "gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART",
                                "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT"});
    for (int t = 0; t < 4; ++t) {
        for (const char* step :
             {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN", "WAIT_COOL", "PROBE_CALIBRATE"}) {
            objects.push_back(std::string("gcode_macro _EXTRUDER_OFFSET_ACTION_") + step + "_T" +
                              std::to_string(t));
        }
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// Readers of the resolved procedure, one per kind: empty when the printer's
/// entry runs some other kind.
std::string resolved_panel_gcode(const PrinterDiscovery& hw) {
    const cal::Procedure p = cal::resolve_procedure(hw);
    return p.kind == cal::ProcedureKind::Panel ? p.gcode : std::string{};
}

std::string resolved_prompt_gcode(const PrinterDiscovery& hw) {
    const cal::Procedure p = cal::resolve_procedure(hw);
    return p.kind == cal::ProcedureKind::PrinterPrompt ? p.gcode : std::string{};
}

std::string resolved_guided_flow(const PrinterDiscovery& hw) {
    const cal::Procedure p = cal::resolve_procedure(hw);
    return p.kind == cal::ProcedureKind::GuidedFlow ? p.flow_id : std::string{};
}

std::string resolved_entry_description(const PrinterDiscovery& hw) {
    return cal::resolve_procedure(hw).entry_description;
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
    CHECK(resolved_panel_gcode(hw) == "CALIBRATE_TOOL_OFFSETS");
}

TEST_CASE("tool offset calibration: the gcode is the bare macro, or nothing", "[tool_offset_cal]") {
    CHECK(resolved_panel_gcode(calibrating_toolchanger()) == "CALIBRATE_TOOL_OFFSETS");
    CHECK(resolved_panel_gcode(printer_with({"toolchanger", "tool T0"})).empty());
}

TEST_CASE("tool offset calibration: a U1 calibrates through its own guided flow",
          "[tool_offset_cal]") {
    const PrinterDiscovery u1 = snapmaker_u1();
    CHECK(cal::supported(u1));
    CHECK(resolved_guided_flow(u1) == helix::snapmaker::nozzle_offset::kGuidedFlowId);
    CHECK(resolved_panel_gcode(u1).empty());

    // A macro-calibrating tool changer names no flow: the macro panel runs it.
    CHECK(resolved_guided_flow(calibrating_toolchanger()).empty());
}

// ============================================================================
// A changer firmware whose calibration opens a printer-side prompt
// ============================================================================

namespace {

/// A Creator 5 Pro on Z-Mod: zmod_color plus the carriage grab buttons, and no
/// klipper-toolchanger.
PrinterDiscovery zmod_c5(bool with_calibrate_macro) {
    json objects = json::array({"gcode_move", "toolhead", "extruder", "extruder1", "extruder2",
                                "extruder3", "zmod", "zmod_color"});
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
    }
    if (with_calibrate_macro) {
        objects.push_back("gcode_macro CALIBRATE_EXTRUDERS");
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

} // namespace

TEST_CASE("tool offset calibration: a Z-Mod Creator 5 hands the run to its own prompt",
          "[tool_offset_cal]") {
    const PrinterDiscovery c5 = zmod_c5(/*with_calibrate_macro=*/true);
    CHECK(cal::supported(c5));
    CHECK(resolved_prompt_gcode(c5) == "CALIBRATE_EXTRUDERS");
    // The panel never runs a prompt-only macro: its rpc answers before the run.
    CHECK(resolved_panel_gcode(c5).empty());
    CHECK(resolved_guided_flow(c5).empty());
}

TEST_CASE("tool offset calibration: the prompt needs the macro and the changer",
          "[tool_offset_cal]") {
    const PrinterDiscovery bare_c5 = zmod_c5(/*with_calibrate_macro=*/false);
    CHECK_FALSE(cal::supported(bare_c5));
    CHECK(resolved_prompt_gcode(bare_c5).empty());

    // An AD5X on Z-Mod publishes zmod_color but has no carriage: no changer row.
    const PrinterDiscovery ad5x =
        printer_with({"zmod", "zmod_color", "gcode_macro CALIBRATE_EXTRUDERS"});
    CHECK_FALSE(cal::supported(ad5x));
    CHECK(resolved_prompt_gcode(ad5x).empty());
}

TEST_CASE("tool offset calibration: CALIBRATE_TOOL_OFFSETS outranks a firmware prompt",
          "[tool_offset_cal]") {
    // The blocking macro reports its results through ToolState; the prompt
    // reports nothing, so where both exist the panel runs.
    const PrinterDiscovery both = printer_with(
        {"zmod_color", "gcode_button extruder_grab1", "toolchanger", "tool T0", "tool T1",
         "gcode_macro CALIBRATE_EXTRUDERS", "gcode_macro CALIBRATE_TOOL_OFFSETS"});
    CHECK(resolved_prompt_gcode(both).empty());
    CHECK(resolved_panel_gcode(both) == "CALIBRATE_TOOL_OFFSETS");
}

TEST_CASE("tool offset calibration: a macro toolchanger and a bare printer send no prompt",
          "[tool_offset_cal]") {
    CHECK(resolved_prompt_gcode(calibrating_toolchanger()).empty());
    CHECK(resolved_panel_gcode(calibrating_toolchanger()) == "CALIBRATE_TOOL_OFFSETS");

    const PrinterDiscovery bare = printer_with({});
    CHECK_FALSE(cal::supported(bare));
    CHECK(resolved_prompt_gcode(bare).empty());
    CHECK(resolved_panel_gcode(bare).empty());
}

// ============================================================================
// INDX: the entry seats a tool by hand
// ============================================================================

namespace {

PrinterDiscovery indx_printer(bool manual_seat) {
    json objects = json::array({"gcode_move", "toolhead", "extruder", "indx", "save_variables",
                                "gcode_macro TOOL_POSITIONS", "gcode_macro _TOOLCHANGE_VARS",
                                "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL"});
    if (manual_seat) {
        objects.push_back("gcode_macro MANUAL_TOOL_SEAT");
        objects.push_back("gcode_macro MANUAL_TOOL_REMOVE");
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

} // namespace

TEST_CASE("tool offset calibration: an INDX seats a tool by hand", "[tool_offset_cal]") {
    const PrinterDiscovery indx = indx_printer(true);
    CHECK(resolved_guided_flow(indx) == "indx_seat_by_hand");
    CHECK(cal::supported(indx));
    CHECK(resolved_panel_gcode(indx).empty());

    const PrinterDiscovery bare = indx_printer(false);
    CHECK(resolved_guided_flow(bare).empty());
    CHECK_FALSE(cal::supported(bare));
}

TEST_CASE("tool offset calibration: the entry describes what it does", "[tool_offset_cal]") {
    const std::string measure = "Measure every tool's XYZ offset automatically";
    CHECK(resolved_entry_description(calibrating_toolchanger()) == measure);
    CHECK(resolved_entry_description(snapmaker_u1()) == measure);
    CHECK(resolved_entry_description(indx_printer(true)) ==
          "Seat a tool by hand for the first Z home");
}

TEST_CASE("tool offset calibration: the Advanced row's description follows the language",
          "[tool_offset_cal][i18n]") {
    lv_init_safe();
    helix::PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);
    state.set_hardware(indx_printer(true));
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    lv_subject_t* text = lv_xml_get_subject(nullptr, "printer_tool_offset_cal_description");
    REQUIRE(text != nullptr);
    CHECK(std::string(lv_subject_get_string(text)) == "Seat a tool by hand for the first Z home");

    auto& settings = SystemSettingsManager::instance();
    settings.init_subjects();
    settings.set_language("de");
    CHECK(std::string(lv_subject_get_string(text)) ==
          "Für die erste Z-Referenzfahrt ein Werkzeug von Hand einsetzen");

    settings.set_language("en");
    CHECK(std::string(lv_subject_get_string(text)) == "Seat a tool by hand for the first Z home");
    PrinterStateTestAccess::reset(state);
}

// ============================================================================
// Precedence across every procedure
// ============================================================================

TEST_CASE("tool offset calibration: each procedure outranks the ones after it",
          "[tool_offset_cal]") {
    // Every row's signal at once, then each removed in precedence order.
    const std::vector<std::string> u1 = {"gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART",
                                         "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT"};
    std::vector<std::string> u1_steps;
    for (int t = 0; t < 4; ++t) {
        for (const char* step :
             {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN", "WAIT_COOL", "PROBE_CALIBRATE"}) {
            u1_steps.push_back(std::string("gcode_macro _EXTRUDER_OFFSET_ACTION_") + step + "_T" +
                               std::to_string(t));
        }
    }
    const std::vector<std::string> indx = {"indx",
                                           "save_variables",
                                           "gcode_macro TOOL_POSITIONS",
                                           "gcode_macro _TOOLCHANGE_VARS",
                                           "gcode_macro CHANGE_TOOL",
                                           "gcode_macro PARK_TOOL",
                                           "gcode_macro MANUAL_TOOL_SEAT",
                                           "gcode_macro MANUAL_TOOL_REMOVE"};
    const std::vector<std::string> panel = {"toolchanger", "tool T0", "tool T1",
                                            "gcode_macro CALIBRATE_TOOL_OFFSETS"};
    const std::vector<std::string> prompt = {"toolchanger", "gcode_macro CALIBRATE_EXTRUDERS"};

    auto build = [&](bool with_u1, bool with_indx, bool with_panel, bool with_prompt) {
        json objects = json::array({"gcode_move", "toolhead", "extruder"});
        auto add = [&](const std::vector<std::string>& names) {
            for (const auto& n : names) {
                objects.push_back(n);
            }
        };
        if (with_u1) {
            add(u1);
            add(u1_steps);
        }
        if (with_indx) {
            add(indx);
        }
        if (with_panel) {
            add(panel);
        }
        if (with_prompt) {
            add(prompt);
        }
        PrinterDiscovery hw;
        hw.parse_objects(objects);
        return hw;
    };

#if HELIX_HAS_SNAPMAKER
    const cal::Procedure all = cal::resolve_procedure(build(true, true, true, true));
    CHECK(all.kind == cal::ProcedureKind::GuidedFlow);
    CHECK(all.flow_id == helix::snapmaker::nozzle_offset::kGuidedFlowId);
#endif
    const cal::Procedure no_u1 = cal::resolve_procedure(build(false, true, true, true));
    CHECK(no_u1.kind == cal::ProcedureKind::GuidedFlow);
    CHECK(no_u1.flow_id == "indx_seat_by_hand");

    const cal::Procedure no_flow = cal::resolve_procedure(build(false, false, true, true));
    CHECK(no_flow.kind == cal::ProcedureKind::Panel);
    CHECK(no_flow.gcode == "CALIBRATE_TOOL_OFFSETS");

    const cal::Procedure prompt_only = cal::resolve_procedure(build(false, false, false, true));
    CHECK(prompt_only.kind == cal::ProcedureKind::PrinterPrompt);
    CHECK(prompt_only.gcode == "CALIBRATE_EXTRUDERS");
    CHECK(prompt_only.flow_id.empty());

    const cal::Procedure none = cal::resolve_procedure(build(false, false, false, false));
    CHECK(none.kind == cal::ProcedureKind::None);
    CHECK(none.gcode.empty());
    CHECK(none.flow_id.empty());
    CHECK_FALSE(cal::supported(build(false, false, false, false)));
}
