// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_offset_calibration.h"

#include "indx_contract.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"
#include "translation_loader.h"

#if HELIX_HAS_SNAPMAKER
#include "snapmaker_nozzle_offset.h"
#endif

#include <algorithm>
#include <string_view>

namespace helix::tool_offset_calibration {

namespace {

constexpr const char* kMeasureDescription =
    TR_NOOP("Measure every tool's XYZ offset automatically");

/// klipper-toolchanger's example macro. The command alias is always the
/// uppercased name whatever case printer.cfg spells the section in
/// (klippy/extras/gcode_macro.py), so the constant is the right spelling to
/// send - unlike a SET_GCODE_VARIABLE MACRO= mux key.
constexpr const char* kPanelMacro = "CALIBRATE_TOOL_OFFSETS";

/// Z-Mod's Creator 5 calibration (c5pro_generic.cfg): opens "remove the build
/// plate" in an action:prompt whose OK runs _T_CALIBRATE_EXTRUDERS.
constexpr const char* kPromptMacro = "CALIBRATE_EXTRUDERS";

/// OpenCreator's Creator 5 calibration (printer.creator5.cfg): with no
/// arguments it runs C5_OFFSET_WIZARD, an action:prompt that picks the tool,
/// confirms the build plate is out and then measures.
constexpr const char* kWizardPromptMacro = "C5_CALIBRATE_OFFSETS";

/// AFC's `[AFC_Toolchanger <name>]` unit: AFC owns head swaps, each
/// AFC_extruder lane is a tool.
constexpr std::string_view kAfcToolchangerUnitPrefix = "AFC_Toolchanger ";

/// One way a printer calibrates its tool offsets.
struct Row {
    const char* name;
    bool (*detect)(const PrinterDiscovery& hw);
    ProcedureKind kind;
    /// The panel or prompt macro, or the guided flow id.
    const char* target;
    const char* entry_description;
};

bool has_afc_toolchanger_unit(const PrinterDiscovery& hw) {
    const auto& units = hw.afc_unit_object_names();
    return std::any_of(units.begin(), units.end(), [](const std::string& unit) {
        return std::string_view(unit).substr(0, kAfcToolchangerUnitPrefix.size()) ==
               kAfcToolchangerUnitPrefix;
    });
}

/// A tool changer in any dialect: klipper-toolchanger, or a changer firmware
/// toolchanger_addon claims.
bool is_tool_changer(const PrinterDiscovery& hw) {
    return hw.has_tool_changer() || toolchanger_addon::present(hw);
}

bool panel_detect(const PrinterDiscovery& hw) {
    return hw.has_tool_changer() && hw.has_macro(kPanelMacro);
}

bool prompt_detect(const PrinterDiscovery& hw) {
    return is_tool_changer(hw) && hw.has_macro(kPromptMacro);
}

/// OpenCreator's changer is an AFC Toolchanger unit, which no other row counts.
bool wizard_prompt_detect(const PrinterDiscovery& hw) {
    return (is_tool_changer(hw) || has_afc_toolchanger_unit(hw)) &&
           hw.has_macro(kWizardPromptMacro);
}

/// Precedence order: the first row that detects wins.
const Row kRows[] = {
#if HELIX_HAS_SNAPMAKER
    {"Snapmaker nozzle offset", snapmaker::nozzle_offset::supported, ProcedureKind::GuidedFlow,
     snapmaker::nozzle_offset::kGuidedFlowId, kMeasureDescription},
#endif
    {"INDX seat by hand", indx::supports_seat_by_hand, ProcedureKind::GuidedFlow,
     indx::kSeatByHandFlowId, TR_NOOP("Seat a tool by hand for the first Z home")},
    {"klipper-toolchanger", panel_detect, ProcedureKind::Panel, kPanelMacro, kMeasureDescription},
    {"changer firmware offset wizard", wizard_prompt_detect, ProcedureKind::PrinterPrompt,
     kWizardPromptMacro, kMeasureDescription},
    {"changer firmware prompt", prompt_detect, ProcedureKind::PrinterPrompt, kPromptMacro,
     kMeasureDescription},
};

} // namespace

Procedure resolve_procedure(const PrinterDiscovery& hw) {
    for (const auto& row : kRows) {
        if (!row.detect(hw)) {
            continue;
        }
        Procedure p;
        p.kind = row.kind;
        p.name = row.name;
        p.entry_description = row.entry_description;
        if (row.kind == ProcedureKind::GuidedFlow) {
            p.flow_id = row.target;
        } else {
            p.gcode = row.target;
        }
        return p;
    }
    return {};
}

bool supported(const PrinterDiscovery& hw) {
    return resolve_procedure(hw).kind != ProcedureKind::None;
}

} // namespace helix::tool_offset_calibration
