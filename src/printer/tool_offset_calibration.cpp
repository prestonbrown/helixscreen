// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_offset_calibration.h"

#include "printer_discovery.h"

#include <algorithm>

namespace helix::tool_offset_calibration {

bool supported(const PrinterDiscovery& hw) {
    return hw.has_tool_changer() && hw.has_macro(kMacro);
}

std::string calibrate_all_gcode(const PrinterDiscovery& hw) {
    if (!supported(hw)) {
        return {};
    }
    // The command alias is always the uppercased name whatever case printer.cfg
    // spells the section in (klippy/extras/gcode_macro.py), so the constant is
    // the right spelling here - unlike a SET_GCODE_VARIABLE MACRO= mux key.
    return kMacro;
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

void Run::begin(int tool_count, int mounted_tool) {
    steps_.assign(static_cast<size_t>(std::max(tool_count, 0)), ToolStep::Queued);
    active_ = !steps_.empty();
    failed_ = false;
    on_tool_selected(mounted_tool);
}

void Run::finish(bool ok) {
    if (!active_) {
        return;
    }
    for (auto& step : steps_) {
        if (step == ToolStep::Measuring) {
            step = ok ? ToolStep::Done : ToolStep::Failed;
        } else if (step == ToolStep::Queued) {
            step = ok ? ToolStep::Done : ToolStep::Idle;
        }
    }
    failed_ = !ok;
    active_ = false;
}

void Run::abort() {
    if (!active_) {
        return;
    }
    for (auto& step : steps_) {
        if (step == ToolStep::Measuring || step == ToolStep::Queued) {
            step = ToolStep::Idle;
        }
    }
    failed_ = false;
    active_ = false;
}

void Run::on_tool_selected(int tool) {
    if (!active_ || tool < 0 || tool >= tool_count()) {
        return;
    }
    if (steps_[static_cast<size_t>(tool)] == ToolStep::Done) {
        // The park at the end of the macro re-selects a finished tool; it
        // does not go back under the probe.
        return;
    }
    // The macro only moves on once it is done with the previous tool, so a
    // tool still Measuring when another is selected is finished - a tool the
    // macro never writes offsets for (the one it measures the others against)
    // ends its pass exactly this way.
    for (auto& step : steps_) {
        if (step == ToolStep::Measuring) {
            step = ToolStep::Done;
        }
    }
    set(tool, ToolStep::Measuring);
}

void Run::on_tool_measured(int tool) {
    if (!active_) {
        return;
    }
    set(tool, ToolStep::Done);
}

int Run::measuring_tool() const {
    for (size_t i = 0; i < steps_.size(); ++i) {
        if (steps_[i] == ToolStep::Measuring) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ToolStep Run::step(int tool) const {
    if (tool < 0 || tool >= tool_count()) {
        return ToolStep::Idle;
    }
    return steps_[static_cast<size_t>(tool)];
}

void Run::set(int tool, ToolStep step) {
    if (tool >= 0 && tool < tool_count()) {
        steps_[static_cast<size_t>(tool)] = step;
    }
}

} // namespace helix::tool_offset_calibration
