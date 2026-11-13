// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Stepper resonance calibration ("Vertical Artifact Compensation").
//
// Some firmwares ship a routine that measures the resonance of the XY stepper
// motors themselves and tunes a damping compensation for it, which removes the
// fine, evenly spaced vertical lines a motor's own vibration prints into walls.
// It is not input shaping: input shaping cancels frame ringing after a corner,
// this cancels the motor's ripple at constant speed.
//
// This module is the ONLY place that knows which printers have such a routine
// and how each one runs. The Calibration entry asks resolve() and never names
// the firmware. Adding a firmware means adding one row to the procedure table
// in stepper_resonance_calibration.cpp - no call site changes.

#include <string>

namespace helix {
class PrinterDiscovery;
}

namespace helix::stepper_resonance_cal {

/// How this printer runs the calibration. Default-constructed means it has no
/// routine.
struct Procedure {
    /// Which row matched, for logs. Empty when none did.
    const char* name = "";
    /// The routine runs with one chosen tool on the carriage, so the user picks
    /// which. False for a whole-machine routine.
    bool per_tool = false;
    /// The toolhead must be homed first. The routine itself does not home.
    bool needs_home = false;
    /// The routine ends in SAVE_CONFIG, which restarts Klipper: the connection
    /// drops when it finishes, and the user must be told before it starts.
    bool restarts_klipper = false;
    /// Builds the gcode; nullptr when there is no routine.
    std::string (*build)(const PrinterDiscovery& hw, int tool) = nullptr;

    [[nodiscard]] bool present() const {
        return build != nullptr;
    }
};

/// The most specific routine this printer offers, or an absent Procedure.
Procedure resolve(const PrinterDiscovery& hw);

/// Gcode that runs the calibration. @p tool (0-based) picks the tool for a
/// per-tool routine and is ignored otherwise. Empty when the printer has no
/// routine, or a per-tool routine is handed a negative tool. Never includes
/// the homing a Procedure's needs_home asks for.
std::string calibrate_gcode(const PrinterDiscovery& hw, int tool);

/// How this printer switches its motor compensation on and off outside the
/// calibration routine. Prints enable it at start and disable it at the end,
/// so a measurement taken while idle runs uncompensated unless it enables it.
struct CompensationToggle {
    std::string enable_gcode; ///< empty: no toggle
    std::string disable_gcode;
    /// Enable applies fixed factory numbers rather than a saved calibration.
    bool applies_factory_values = false;

    [[nodiscard]] bool present() const {
        return !enable_gcode.empty() && !disable_gcode.empty();
    }
};

[[nodiscard]] CompensationToggle compensation_toggle(const PrinterDiscovery& hw);

/// The entry's name and what it does, untranslated (pass them through lv_tr()).
const char* entry_name();
const char* entry_description();

} // namespace helix::stepper_resonance_cal
