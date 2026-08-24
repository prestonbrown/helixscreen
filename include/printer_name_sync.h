// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

class IMoonrakerAPI;

namespace helix {

/// Sync printer display name with Mainsail/Fluidd via Moonraker database API.
/// All callbacks are delivered on the UI thread via queue_update().
class PrinterNameSync {
  public:
    /// Resolve printer name: local config → Mainsail DB → Fluidd DB → hostname → fallback.
    /// If local config is empty and an external name is found, seeds local config and
    /// updates PrinterState::active_printer_name_.
    /// @param api       Moonraker API (used for database reads)
    /// @param hostname  Hostname from printer.info discovery (fallback)
    static void resolve(IMoonrakerAPI* api, const std::string& hostname);

    /// Write name to both Mainsail and Fluidd database namespaces (fire-and-forget).
    /// @param api   Moonraker API (used for database writes)
    /// @param name  The new printer name to write
    static void write_back(IMoonrakerAPI* api, const std::string& name);
};

} // namespace helix
