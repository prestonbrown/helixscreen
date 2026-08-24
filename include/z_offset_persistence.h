// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Firmware-persisted z-offset.
//
// On most printers the z-offset the next print will use IS Klipper's live
// gcode_move offset (homing_origin[2]), so reading that is the whole story.
// Some firmwares instead keep the authoritative value in their own storage and
// only push it into gcode_move for the duration of a print - zeroing the live
// offset in their END_PRINT/CANCEL_PRINT macros. On those, homing_origin reads
// 0.000 whenever the printer is idle and is an outright lie about what the next
// print will apply.
//
// This module is the ONLY place that knows which firmwares behave that way and
// where each one stores its value. Generic code (PrinterMotionState, the
// discovery subscription, Application startup) asks these functions and never
// names a firmware.
//
// Adding a firmware means adding one Provider to the table in
// z_offset_persistence.cpp - no call site changes.

#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class PrinterDiscovery;
}

namespace helix::zoffset {

/// Klipper status objects that must be subscribed for
/// read_persisted_offset_microns() to ever return a value on this printer.
/// Empty for printers whose z-offset lives only in gcode_move.
std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// The firmware-persisted z-offset in microns, read out of a Moonraker status
/// frame. nullopt when this frame carries no persisted offset - either because
/// the printer has no such firmware, or because the status object is delta-only
/// and simply is not in this frame. Callers must treat nullopt as "no news",
/// never as "cleared".
std::optional<int> read_persisted_offset_microns(const nlohmann::json& status);

/// One-shot gcode that switches the firmware's z-offset persistence on, or an
/// empty string when the printer needs no such call. Send once per session, and
/// only while idle - it is a gcode injection.
std::string persistence_enable_gcode(const PrinterDiscovery& hw);

/// Whether this printer keeps its authoritative z-offset outside gcode_move.
bool firmware_persists_z_offset(const PrinterDiscovery& hw);

/// Human-readable name of the matched firmware, for logging. Empty when none.
std::string persistence_provider_name(const PrinterDiscovery& hw);

/// Gate for sending persistence_enable_gcode(): the printer needs it, no print
/// is running, and we have not already sent it this session.
inline bool should_enable_persistence(bool firmware_needs_enable, bool print_active,
                                      bool already_sent) {
    return firmware_needs_enable && !print_active && !already_sent;
}

} // namespace helix::zoffset
