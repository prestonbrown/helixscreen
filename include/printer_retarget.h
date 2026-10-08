// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

class MoonrakerManager; // NAMESPACE_OK: forward declaration of the global MoonrakerManager

namespace helix {

/// The WebSocket URL of the active printer's Moonraker, the one the reconnects below use.
/// @p default_host stands in when the printer has no saved host.
std::string active_printer_ws_url(const std::string& default_host = "");

/// The HTTP base URL of the active printer's Moonraker, for file transfers and REST.
std::string active_printer_http_url(const std::string& default_host = "");

/// Connects @p manager to a Moonraker with what every connection has: the print-start
/// collector, created first. False when the transport could not start (e.g. no internal
/// RAM for its task); there is no collector then.
bool connect_printer(MoonrakerManager& manager, const std::string& ws_url,
                     const std::string& http_url);

/// connect_printer() to the active printer, through the global manager.
bool connect_active_printer();

/// Reconnects the live client to the host and port the active printer's config names.
/// Main thread only. False when there is no client or manager, or the connect could not start.
bool reconnect_active_printer();

/// Points the live connection at the active printer as a different printer than the one it
/// served: disconnects, applies the old printer's queued notifications, drops its AMS
/// backends (discovery builds them only when none exist), shows the new printer's name, then
/// reconnects. False when the connect could not start. Main thread only.
bool retarget_printer_connection();

} // namespace helix
