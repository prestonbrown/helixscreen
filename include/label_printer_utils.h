// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/text_io.h"
#include "label_printer.h"
#include "mdns_discovery.h"
#include "spoolman_types.h"

#include <algorithm>
#include <string>
#include <vector>

namespace helix {

class IppPrinter;

/// Print a spool label using the configured printer backend.
/// Handles backend selection, USB device scanning with fallback, and error reporting.
/// Callback fires on UI thread.
void print_spool_label(const SpoolInfo& spool, PrintCallback callback);

/// Translate a raw label-printer error string into a user-actionable toast message.
///
/// Raw error strings come from BlueZ D-Bus ("br-connection-profile-unavailable",
/// "Connection timed out"), our Niimbot protocol layer ("cmd[3] 0x20 response
/// timeout"), the BLE GATT layer ("GATT characteristic not found"), CUPS/IPP,
/// and USB — none of which mean anything to the user. This maps known
/// signatures to actionable advice; unknown errors fall back to a generic
/// "Print failed". Result is already run through lv_tr() and safe to show
/// directly in a toast.
std::string friendly_label_printer_error(const std::string& raw);

/// Persist a printer picked from network discovery: address, port (631 for an IPP
/// printer with no SRV port), protocol ("raw" or "ipp") and its IPP resource path.
void save_discovered_network_printer(const DiscoveredPrinter& printer, const std::string& protocol);

/// A discovered network printer as the settings page ranks it: best candidate first.
struct RankedNetworkPrinter {
    std::string ip_address;
    std::string protocol; ///< "raw" or "ipp"
};

/// What the settings page does with a fresh discovery list.
struct DiscoveredPrinterPick {
    int select = -1; ///< index to save as the printer, or -1 to keep the saved one
    int show = -1;   ///< index the dropdown shows, or -1 when the saved printer is not listed
};

/**
 * Decide which discovered printer to save and which to show. Raw and IPP discovery
 * report separately, in either order, so an automatic pick is revisited whenever a
 * better-ranked printer appears; a printer the user chose is never replaced.
 *
 * @param ranked         discovered printers, best candidate first
 * @param saved_ip       saved printer address ("" when none)
 * @param saved_protocol saved printer protocol
 * @param may_auto_pick  true while no printer is saved or the saved one was picked automatically
 */
DiscoveredPrinterPick pick_discovered_printer(const std::vector<RankedNetworkPrinter>& ranked,
                                              const std::string& saved_ip,
                                              const std::string& saved_protocol,
                                              bool may_auto_pick);

/// Point an IPP printer at the configured address, port and resource path.
void apply_ipp_target(IppPrinter& printer);

/**
 * Score how likely a discovered printer is to be a label printer.
 * Higher scores = more likely. 0 = definitely not a label printer.
 */
inline int label_printer_score(const DiscoveredPrinter& printer) {
    auto lower = [](const std::string& s) {
        std::string out = s;
        out = helix::text_io::to_lower(out);
        return out;
    };
    std::string name = lower(printer.name);
    std::string host = lower(printer.hostname);

    // Strong signals — these are definitely label printers
    if (name.find("ql-") != std::string::npos || name.find("ql ") != std::string::npos)
        return 100;
    if (name.find("td-") != std::string::npos || name.find("td ") != std::string::npos)
        return 100;
    if (name.find("labelwriter") != std::string::npos || name.find("dymo") != std::string::npos)
        return 90;
    if (name.find("zebra") != std::string::npos)
        return 80;
    if (name.find("rollo") != std::string::npos || name.find("munbyn") != std::string::npos)
        return 80;
    if (name.find("label") != std::string::npos)
        return 70;

    // Moderate signals from hostname (BRW prefix = Brother wireless)
    if (host.find("brw") != std::string::npos)
        return 50;

    // Negative signals — definitely NOT label printers
    if (name.find("laserjet") != std::string::npos || name.find("officejet") != std::string::npos)
        return 0;
    if (name.find("inkjet") != std::string::npos || name.find("pixma") != std::string::npos)
        return 0;
    if (name.find("ecotank") != std::string::npos || name.find("envy") != std::string::npos)
        return 0;
    if (name.find("epson") != std::string::npos && name.find("et-") != std::string::npos)
        return 0;

    // Unknown — could be anything, low score but still show
    return 10;
}

/// Score how likely a discovered printer is to be a standard page printer (for IPP sheet labels).
/// Higher scores = more likely. 0 = definitely not a page printer.
inline int ipp_printer_score(const DiscoveredPrinter& printer) {
    auto lower = [](const std::string& s) {
        std::string out = s;
        out = helix::text_io::to_lower(out);
        return out;
    };
    std::string name = lower(printer.name);

    // Label printers — NOT standard page printers
    if (name.find("ql-") != std::string::npos || name.find("ql ") != std::string::npos)
        return 0;
    if (name.find("labelwriter") != std::string::npos || name.find("dymo") != std::string::npos)
        return 0;
    if (name.find("zebra") != std::string::npos)
        return 0;
    if (name.find("niimbot") != std::string::npos || name.find("phomemo") != std::string::npos)
        return 0;

    // Strong positive signals for standard printers
    if (name.find("laserjet") != std::string::npos || name.find("officejet") != std::string::npos)
        return 100;
    if (name.find("pixma") != std::string::npos || name.find("ecotank") != std::string::npos)
        return 100;
    if (name.find("epson") != std::string::npos)
        return 90;
    if (name.find("envy") != std::string::npos || name.find("inkjet") != std::string::npos)
        return 90;
    if (name.find("brother") != std::string::npos)
        return 70; // Could be label or page printer
    if (name.find("canon") != std::string::npos)
        return 80;
    if (name.find("hp") != std::string::npos)
        return 60;

    // Unknown — probably a standard printer if found via _ipp._tcp
    return 50;
}

} // namespace helix
