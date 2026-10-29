// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_auth.h"

#include <string>

class MoonrakerManager; // NAMESPACE_OK: forward declaration of the global MoonrakerManager

namespace helix {

/// The WebSocket URL of the active printer's Moonraker, the one the reconnects below use.
/// @p default_host stands in when the printer has no saved host.
std::string active_printer_ws_url(const std::string& default_host = "");

/// The HTTP base URL of the active printer's Moonraker, for file transfers and REST.
std::string active_printer_http_url(const std::string& default_host = "");

/// The active printer's Moonraker address and API key, from its saved settings: the
/// same host and port active_printer_ws_url() connects to, so the key matches it.
moonraker_auth::Credential active_printer_credential(const std::string& default_host = "");

/// Sets the active printer's API key in its settings entry, false when @p api_key is blank.
/// The caller saves the config. The key never leaves that entry.
void set_active_printer_api_key(const std::string& api_key);

/// Connects @p manager to a Moonraker with what every connection has: the print-start
/// collector, created first. False when the transport could not start (e.g. no internal
/// RAM for its task); there is no collector then.
bool connect_printer(MoonrakerManager& manager, const std::string& ws_url,
                     const std::string& http_url);

/// connect_printer() to the active printer, through the global manager.
bool connect_active_printer();

/// Points the live connection at the active printer as a different printer than the one it
/// served: disconnects, applies the old printer's queued notifications, drops its AMS
/// backends (discovery builds them only when none exist), shows the new printer's name, then
/// reconnects. False when the connect could not start. Main thread only.
bool retarget_printer_connection();

} // namespace helix
