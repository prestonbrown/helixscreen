// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file macro_patterns.h
 * @brief Canonical macro-name tables shared by every consumer of a capability
 *
 * A capability that is recognised by macro name has exactly ONE list of names,
 * and it lives here. Both the discovery-time scan (PrinterDiscovery, which walks
 * the printer's object list once) and the slot resolver (StandardMacros, which
 * picks a macro per semantic slot) read the same table, so the two can no longer
 * disagree about what counts as, say, a nozzle-clean macro.
 *
 * Names are stored UPPERCASE. Klipper macro names are case-insensitive and both
 * consumers upper-case before comparing, so the table needs no case variants.
 *
 * Order is priority order: StandardMacros::try_detect() takes the first entry
 * the printer defines, so the most specific/most conventional name comes first.
 */

#include <string>
#include <vector>

namespace helix::macro_patterns {

/**
 * @brief Macro names that mean "clean/wipe the nozzle"
 *
 * Read by PrinterDiscovery's macro scan (which caches nozzle_clean_macro_, the
 * backing store for has_nozzle_clean_macro()) and by StandardMacros' CleanNozzle
 * slot detection. Historically these were two hand-maintained lists that drifted
 * apart — CLEAR_NOZZLE existed only in the slot resolver, PURGE_NOZZLE and
 * NOZZLE_CLEAN only in discovery — so a printer could drive the Controls-panel
 * button while the capability chip stayed hidden.
 */
inline const std::vector<std::string>& clean_nozzle() {
    static const std::vector<std::string> kPatterns = {"CLEAN_NOZZLE", "NOZZLE_WIPE",
                                                       "WIPE_NOZZLE",  "CLEAR_NOZZLE",
                                                       "PURGE_NOZZLE", "NOZZLE_CLEAN"};
    return kPatterns;
}

} // namespace helix::macro_patterns
