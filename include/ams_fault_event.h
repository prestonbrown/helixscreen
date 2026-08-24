// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "error_event.h"

#include <string>
#include <utility>
#include <vector>

/**
 * @file ams_fault_event.h
 * @brief Shared plumbing for AMS backend fault events.
 *
 * Every filament-system backend that claims a Klipper error line does the same
 * three things: recognise the `!!` broadcast, strip its prefix to get readable
 * detail text, and hand back a CRITICAL + sticky @ref helix::ErrorEvent with a
 * recovery set. These helpers hold that shared shape so AFC, Happy Hare and the
 * rest cannot drift apart on it.
 *
 * Header-only inline helpers, matching operation_patterns.h — there is no state
 * and nothing here is worth a translation unit.
 */

namespace helix {

/**
 * @brief True when @p line is one of Klipper's `!!` emergency broadcasts.
 *
 * These are the only lines an AMS backend classifier considers; `Error:`
 * command errors and plain responses are left to the generic classifier.
 */
[[nodiscard]] inline bool is_bang_line(const std::string& line) {
    return line.size() >= 2 && line[0] == '!' && line[1] == '!';
}

/**
 * @brief Strip the leading `!!` (and the single space after it, when present)
 *        so the remainder can be shown as user-facing detail text.
 *
 * The space is only consumed when there is something after it: a line of
 * exactly `"!! x"` (size 4) drops to `"x"`, while `"!! "` (size 3) keeps the
 * trailing space rather than yielding an empty detail. That is the historical
 * behaviour of the two hand-rolled copies this replaces, preserved verbatim.
 *
 * @pre @p line satisfies is_bang_line() — callers must guard first, since a
 *      shorter string would make the substr throw.
 */
[[nodiscard]] inline std::string strip_bang_prefix(const std::string& line) {
    return (line.size() > 3 && line[2] == ' ') ? line.substr(3) : line.substr(2);
}

/**
 * @brief Build the ErrorEvent shape every AMS backend fault shares.
 *
 * A filament-system fault is always CRITICAL (the print is stopped or about to
 * be) and always sticky (it must survive until the user acts on it), so those
 * two are baked in rather than passed. Everything else the backend cares about
 * — source, wording, and the recovery set from build_recovery_actions() — comes
 * from the caller.
 *
 * Deliberately thin: it returns the event by value so a backend that needs a
 * field this does not cover (`code`, `raw_detail`, a non-CRITICAL severity) can
 * simply assign it on the result.
 */
[[nodiscard]] inline ErrorEvent make_ams_fault_event(ErrorSource source, std::string title,
                                                     std::string detail,
                                                     std::vector<RecoveryAction> actions) {
    ErrorEvent e;
    e.source = source;
    e.severity = ErrorSeverity::CRITICAL;
    e.title = std::move(title);
    e.detail = std::move(detail);
    e.recovery_actions = std::move(actions);
    e.sticky = true;
    return e;
}

} // namespace helix
