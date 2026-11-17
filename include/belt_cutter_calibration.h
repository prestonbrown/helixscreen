// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Motorized belt tensioning and cutter calibration.
//
// Some printers tension their own belts with motorized tensioners that measure
// the belt's pull, and find their filament cutter's position by driving the
// toolhead into it. Both are machine-moving routines the firmware runs on its
// own; HelixScreen only has to know whether the printer offers them and which
// command starts each.
//
// This module is the ONLY place that knows which firmwares offer them and how
// each one runs. The Calibration entries ask the questions below and never
// name the firmware. Adding a firmware means adding one row to the provider
// table in belt_cutter_calibration.cpp; no call site changes.

#include <string>

namespace helix {
class PrinterDiscovery;
}

namespace helix::belt_cutter_cal {

/// Which belts one tensioning run covers.
enum class BeltAxes { Both, X, Y };

/// The printer tensions its own belts with motorized tensioners.
[[nodiscard]] bool can_tension_belts(const PrinterDiscovery& hw);

/// Gcode that tensions @p axes. Empty when the printer cannot. The routine
/// homes X and Y itself and leaves the motors released.
[[nodiscard]] std::string belt_tension_gcode(const PrinterDiscovery& hw, BeltAxes axes);

/// The printer finds its filament cutter's position on its own.
[[nodiscard]] bool can_calibrate_cutter(const PrinterDiscovery& hw);

/// Gcode that finds and stores the cutter position. Empty when the printer
/// cannot. The routine homes X and Y itself when they need it, and refuses
/// while filament is loaded.
[[nodiscard]] std::string cutter_calibration_gcode(const PrinterDiscovery& hw);

/// Which provider row matched, for logs. Empty when none did.
[[nodiscard]] const char* provider_name(const PrinterDiscovery& hw);

/// The entries' names and what they do, untranslated (pass them through lv_tr()).
const char* belt_entry_name();
const char* belt_entry_description();
const char* cutter_entry_name();
const char* cutter_entry_description();

} // namespace helix::belt_cutter_cal
