// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// General-motion envelope narrower than the kinematic one.
//
// Some firmwares keep a service zone (a nozzle brush, a purge bin) inside the
// kinematic travel but outside the print area, and only allow it within a
// narrow band of the other axis. While a move enters or just left that zone
// they widen toolhead.axis_maximum to reach it, so a whole-bed move planned
// from axis_maximum at that moment can target a point the firmware refuses.
//
// This module is the ONLY place that knows which firmwares do that and where
// each reports its print-area limit. Generic code (PrinterMotionState, the
// discovery subscription) asks these functions and never names a firmware.
//
// Adding a firmware means adding one Provider to the table in
// motion_envelope.cpp - no call site changes.

#include "json_fwd.h"

#include <optional>
#include <string>
#include <vector>

namespace helix::motion_envelope {

/// Klipper status objects that must be subscribed for read_y_max_mm() to ever
/// return a value, given printer.objects.list. Empty for printers without a
/// service zone.
std::vector<std::string> required_status_objects(const std::vector<std::string>& printer_objects);

/// The highest machine Y (mm) that motion anywhere along X may target, read
/// out of a Moonraker status frame. nullopt when this frame carries none: the
/// printer has no such zone, or the delta-only status object is not in this
/// frame. Callers treat nullopt as "no news", never as "cleared".
std::optional<float> read_y_max_mm(const nlohmann::json& status);

} // namespace helix::motion_envelope
