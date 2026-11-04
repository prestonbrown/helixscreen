// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <map>
#include <string>
#include <vector>

namespace helix {
class PrinterDiscovery;
} // namespace helix

/**
 * @file pre_print_preferences.h
 * @brief Which firmwares store pre-print option settings, and how to read them.
 *
 * Most printers treat a pre-print toggle as a property of one job: the value
 * shapes that print and is forgotten. Some firmwares instead keep the setting
 * themselves, across prints and reboots, and the gcode the slicer emits is
 * gated on what they hold. On those, a row rendered from a static database
 * default would claim a state the machine does not have, and would disagree
 * with every other client looking at the same printer.
 *
 * This is the one module that knows which firmwares those are and where each
 * keeps its values. Generic code asks the capability questions below and never
 * names a firmware. Adding another one is a row in the provider table.
 */
namespace helix::preprint_prefs {

/// Status objects carrying stored pre-print settings for this printer, for the
/// subscription builder. Empty when the firmware stores none.
[[nodiscard]] std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// True when this firmware keeps pre-print option settings across prints, so a
/// row's default must be read from the machine rather than the database.
[[nodiscard]] bool firmware_persists_options(const PrinterDiscovery& hw);

/// Name of the provider backing this printer, for logs. Empty when none.
[[nodiscard]] std::string provider_name(const PrinterDiscovery& hw);

/// The values this firmware currently holds, keyed by pre-print option id.
///
/// Absence is silence, never an off: an option missing from the frame is left
/// out of the map rather than reported false, so a caller cannot mistake a
/// partial status update for the user having switched something off.
[[nodiscard]] std::map<std::string, bool> read_persisted_defaults(const PrinterDiscovery& hw,
                                                                  const nlohmann::json& status);

} // namespace helix::preprint_prefs
