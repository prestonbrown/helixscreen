// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_offset_calibration.h"

#include "printer_discovery.h"

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

} // namespace helix::tool_offset_calibration
