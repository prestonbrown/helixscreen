// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Automatic tool offset calibration.
//
// klipper-toolchanger ships a CALIBRATE_TOOL_OFFSETS macro
// (examples/calibrate-offsets.cfg) that does the whole job in one blocking
// gcode: heats every nozzle, locates the calibration sensor with the first
// tool, then for every other tool selects it, probes it against the sensor and
// writes the result into that tool's gcode_x/y/z_offset with SET_TOOL_PARAMETER
// + SAVE_TOOL_PARAMETER. Nothing in the macro persists: the staged parameters
// wait for a SAVE_CONFIG, which is exactly what the per-tool offset save path
// (helix::tool_offsets, ToolState's dirty tracking) already handles.
//
// This module is the ONLY place that knows the macro's name. The panel asks
// these questions and never names the firmware. There is no per-tool entry point: the macro is all-or-nothing,
// so this module offers exactly one gcode.
//
// Nothing is parsed off the console. Everything the screen needs is already in
// subscribed status: which tool is on the carriage (`toolchanger`.tool_number,
// ToolState's active tool), which tools have been measured (the macro's
// SET_TOOL_PARAMETER writes land on the `tool T<n>` objects and reach
// ToolState through helix::tool_offsets), and when the run is over (the rpc
// completes). Run below turns those three signals into per-tool row states.

#include <string>
#include <vector>

namespace helix {
class PrinterDiscovery;
}

namespace helix::tool_offset_calibration {

/// The macro's command name. klipper-toolchanger's example config defines it
/// as [gcode_macro CALIBRATE_TOOL_OFFSETS]; its presence is the capability.
inline constexpr const char* kMacro = "CALIBRATE_TOOL_OFFSETS";

/// Whether this printer can calibrate its tool offsets automatically: a tool
/// changer that defines the macro. False on every single-toolhead printer and
/// on a tool changer whose config never adopted the example macro.
bool supported(const PrinterDiscovery& hw);

/// Gcode that calibrates every tool, or an empty string when the printer has
/// no such procedure. Blocking on the printer: Moonraker's printer.gcode.script
/// answers when the macro finishes, so the rpc's completion IS the run's.
std::string calibrate_all_gcode(const PrinterDiscovery& hw);

/// Where one tool stands in a run.
enum class ToolStep {
    Idle,      ///< not part of a run
    Queued,    ///< part of the run, not reached yet
    Measuring, ///< the machine is probing this tool now
    Done,      ///< measured in this run
    Failed     ///< the run stopped on this tool
};

/// Bookkeeping for one run of the macro, driven by status - tool selection
/// and offset writes - and by the rpc's completion. Pure state, so the
/// panel's row logic is testable without a printer or a widget.
///
/// The rules assume nothing about which tool is the reference or in what
/// order the macro works: a tool reads Measuring from the moment it is
/// selected until either its offsets are written or another tool is selected
/// (the macro only moves on when it is done with a tool); and a tool selected
/// again after it is Done (the park at the end) stays Done.
class Run {
  public:
    /// Start a run over tools 0..tool_count-1: every tool reads Queued, except
    /// @p mounted_tool, which reads Measuring - the tool on the carriage is
    /// the one under the probe until the toolchanger selects another, and
    /// selecting the tool that is already mounted produces no status change
    /// to learn that from. -1 (or out of range) assumes nothing.
    void begin(int tool_count, int mounted_tool = -1);

    /// The rpc finished. ok=true: every tool that was still Queued or
    /// Measuring is Done (the macro does not return early). ok=false: the
    /// Measuring tool is Failed, Queued tools drop back to Idle, Done stays.
    void finish(bool ok);

    /// The run was aborted from our side: nothing is Failed, nothing is Done
    /// beyond what already was; Queued and Measuring drop back to Idle.
    void abort();

    /// The toolchanger now has @p tool on the carriage. Out-of-range tools are
    /// ignored; a Done tool stays Done.
    void on_tool_selected(int tool);

    /// @p tool's offsets were written - its pass is over, whatever else the
    /// macro is still doing.
    void on_tool_measured(int tool);

    [[nodiscard]] bool active() const {
        return active_;
    }
    [[nodiscard]] int tool_count() const {
        return static_cast<int>(steps_.size());
    }
    /// The tool currently Measuring, or -1.
    [[nodiscard]] int measuring_tool() const;
    /// Idle for a tool outside the run.
    [[nodiscard]] ToolStep step(int tool) const;
    /// Whether the last finished run ended on a failure.
    [[nodiscard]] bool failed() const {
        return failed_;
    }

  private:
    void set(int tool, ToolStep step);

    bool active_ = false;
    bool failed_ = false;
    std::vector<ToolStep> steps_;
};

} // namespace helix::tool_offset_calibration
