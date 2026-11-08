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
 * picks a macro per semantic slot) read the same table, so the two cannot
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
 * slot detection. A name known to only one of them would let a printer drive
 * the Controls-panel button while the capability chip stays hidden.
 */
inline const std::vector<std::string>& clean_nozzle() {
    static const std::vector<std::string> kPatterns = {"CLEAN_NOZZLE", "NOZZLE_WIPE",
                                                       "WIPE_NOZZLE",  "CLEAR_NOZZLE",
                                                       "PURGE_NOZZLE", "NOZZLE_CLEAN"};
    return kPatterns;
}

/**
 * @brief Macro names that mean "park the toolhead"
 *
 * Read by StandardMacros' ParkToolhead slot detection. Matching is exact-name
 * everywhere this table is consumed, so an underscore-prefixed form such as
 * _PARK never matches: the leading underscore is the config author marking a
 * macro internal, and invoking one is machine motion nobody sanctioned.
 */
inline const std::vector<std::string>& park_toolhead() {
    static const std::vector<std::string> kPatterns = {"PARK", "PARK_TOOLHEAD", "TOOLHEAD_PARK"};
    return kPatterns;
}

/**
 * @brief The macro that wraps a multi-head filament feed
 *
 * Named in four places: the discovery scan that sets the capability flag, the
 * subscription that requests the macro object, the reconcile that reads its
 * `doing` interlock from that object, and the backend that resolves the
 * config-case object key. A drift between any two makes the interlock lookup
 * miss a key nothing requested, and the cleanup silently does nothing.
 */
inline constexpr const char* AUTO_FEEDING_BATCH = "AUTO_FEEDING_BATCH";

} // namespace helix::macro_patterns
