// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_gcode_guards.h"

#include "gcode_homing.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"

namespace helix::api {

bool reject_homing_during_active_print(const std::string& gcode, helix::PrinterState& state,
                                       bool silent,
                                       const std::function<void(const MoonrakerError&)>& on_error,
                                       const char* log_tag) {
    if (!helix::is_homing_gcode(gcode)) {
        return false;
    }
    // RAW_PRINT_STATE_OK: this guard must NOT move to job_holds_machine().
    // PrintPreparationManager sends the user's configured pre-start block through
    // execute_gcode() from inside the preparing window
    // (ui_print_preparation_manager.cpp), and is_homing_gcode() matches any line
    // whose first token is G28 — on the K2 that block is the forced bed mesh.
    // Widening this to Preparing makes the app refuse its own pre-start G-code
    // and breaks print start on every printer whose block homes.
    //
    // The window is not left open: a firmware-side PRINT_START already reads
    // PRINTING here, and during a host-side block the app is the only thing
    // driving the toolhead — every affordance that could send a competing G28
    // (the motion/calibration XML bindings, the AMS filament ops, the bypass
    // tile) is disabled by job_holds_machine().
    // RAW_PRINT_STATE_OK: see the full reason above - widening this refuses the
    // app's own pre-start G-code.
    const helix::PrintJobState pstate = state.get_print_job_state();
    if (pstate != helix::PrintJobState::PRINTING && pstate != helix::PrintJobState::PAUSED) {
        return false;
    }
    if (!silent) {
        spdlog::warn("{} Refusing homing G-code during active print (state={}): '{}'", log_tag,
                     static_cast<int>(pstate), gcode.substr(0, 60));
    }
    if (on_error) {
        on_error(MoonrakerError::not_ready("printer.gcode.script",
                                           "Homing is disabled while a print is in progress"));
    }
    return true;
}

} // namespace helix::api
