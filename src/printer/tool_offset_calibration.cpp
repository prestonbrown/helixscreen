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
#include <chrono>
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

/// VRTX's calibration (Slimohan/VRTX): a blocking macro like the one above that measures
/// every tool on the Nudge probe, writes each result with SET_TOOL_PARAMETER and keeps
/// it in save_variables itself, so the panel only has to run it.
constexpr const char* kVrtxPanelMacro = "VRTX_CALIBRATE";

/// Z-Mod's Creator 5 calibration (c5pro_display_off.cfg): opens "remove the build
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

bool vrtx_panel_detect(const PrinterDiscovery& hw) {
    return hw.has_tool_changer() && hw.has_macro(kVrtxPanelMacro);
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
    {"VRTX", vrtx_panel_detect, ProcedureKind::Panel, kVrtxPanelMacro, kMeasureDescription},
    {"changer firmware offset wizard", wizard_prompt_detect, ProcedureKind::PrinterPrompt,
     kWizardPromptMacro, kMeasureDescription},
    {"changer firmware prompt", prompt_detect, ProcedureKind::PrinterPrompt, kPromptMacro,
     kMeasureDescription},
};

ProgressSink& progress_sink() {
    static ProgressSink sink;
    return sink;
}

/// A run is in progress from its first tagged line until its completion line.
/// The completion sentence carries no tag, so a macro that prints the same words
/// outside a run must not read as one.
struct RunState {
    bool active = false;
    std::chrono::steady_clock::time_point last_line;
};

RunState& run_state() {
    static RunState state;
    return state;
}

/// The longest silence inside a live run. The gap between lines is a head's
/// nozzle heating to purge temperature, the purge, and two measuring passes,
/// which takes minutes (Z-Mod documents "several minutes" for the whole run).
/// An estimate, not a measured figure: a run quiet for longer is treated as
/// aborted.
constexpr std::chrono::minutes kRunIdleTimeout{15};

} // namespace

bool is_progress_line(const std::string& line) {
    return line.find("[CALIB] ") != std::string::npos ||
           toolchanger_addon::parse_zmod_calibration_line(line).has_value();
}

std::optional<ProgressEvent> parse_progress_line(const std::string& line) {
    using Step = toolchanger_addon::ZmodCalibrationStep;
    const auto step = toolchanger_addon::parse_zmod_calibration_line(line);
    if (!step) {
        return std::nullopt;
    }
    ProgressEvent event;
    switch (step->kind) {
    case Step::Kind::Started:
        event.kind = ProgressEvent::Kind::Started;
        break;
    case Step::Kind::Skipped:
        event.kind = ProgressEvent::Kind::Skipped;
        break;
    case Step::Kind::Measured:
        event.kind = ProgressEvent::Kind::Measured;
        break;
    case Step::Kind::Complete:
        event.kind = ProgressEvent::Kind::Complete;
        break;
    }
    event.tool = step->tool;
    event.station = step->station;
    event.x_um = step->x_um;
    event.y_um = step->y_um;
    event.z_um = step->z_um;
    return event;
}

void set_progress_sink(ProgressSink sink) {
    progress_sink() = std::move(sink);
}

bool report_progress_line(const std::string& line, std::chrono::steady_clock::time_point now) {
    const auto event = parse_progress_line(line);
    if (!event) {
        return false;
    }
    RunState& run = run_state();
    if (run.active && now - run.last_line > kRunIdleTimeout) {
        run.active = false;
    }
    if (event->kind == ProgressEvent::Kind::Complete) {
        if (!run.active) {
            return false;
        }
        run.active = false;
    } else {
        run.active = true;
        run.last_line = now;
    }
    if (progress_sink()) {
        progress_sink()(*event);
    }
    return true;
}

void end_progress_run() {
    run_state().active = false;
}

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
