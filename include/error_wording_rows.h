// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The row shape behind error_wording, for the modules that contribute a
// firmware's own row set. Nothing outside those modules includes this.

#include "error_wording.h"

#include <cstddef>

namespace helix::error_wording {

/// One message: the anchored, case-sensitive prefix of its reason line, an
/// optional second literal the reason must contain, and an optional capture
/// pattern that must match too. Each key is read from the firmware's own raise
/// sites.
struct Row {
    const char* id;
    const char* prefix;
    /// nullptr, or a literal the reason line must also contain.
    const char* contains;
    /// nullptr, or a helix::Regex whose groups become captures 1..n. The row
    /// matches only where it matches.
    const char* capture;
    /// Search @ref capture over the whole message rather than the reason line:
    /// for a detail Klipper adds on a later line.
    bool capture_in_body;
    Category category;
    Severity severity;
    /// English source; summary nullptr for a row that only classifies. "{0}"
    /// is the shutdown prefix's board, "{1}" on the row's captures.
    const char* summary;
    const char* cause;
    const char* fix;
};

/// One firmware's rows, or the generic set every printer shares.
struct RowSet {
    const Row* rows;
    size_t count;
    /// nullptr: every printer. Otherwise only printers it detects.
    bool (*detect)(const PrinterDiscovery& hw);
};

namespace flashforge {
/// ZMOD's (AD5M, AD5X, Creator 5) messages, English and Russian.
RowSet zmod_rows();
/// Forge-X's (AD5M) messages.
RowSet forge_x_rows();
/// OpenCreator's (Creator 5) messages.
RowSet opencreator_rows();
} // namespace flashforge

} // namespace helix::error_wording
