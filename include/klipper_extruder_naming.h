// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

/**
 * @file klipper_extruder_naming.h
 * @brief Single definition of Klipper's extruder object naming rule.
 *
 * Klipper names extruder objects `extruder`, `extruder1`, `extruder2`, …
 * `extruder<N>` — the bare name is index 0 and every later one carries its
 * index as a decimal suffix. Both the predicate and the parse below share one
 * grammar so a name can never classify as an extruder in one place and fail to
 * yield a number in another.
 *
 * Deliberately a tiny standalone header: subsystems that only need the naming
 * rule (temperature state, graph widgets, tool state) should not have to pull
 * in the whole AMS type surface to get it.
 *
 * @warning AFC section names are NOT Klipper extruder names. An AFC config
 * section like `[AFC_extruder my_toolhead]` names a lane, not a Klipper
 * extruder object, and must be resolved through
 * `AmsBackendAfc::klipper_extruder_name_unlocked()` before anything here is
 * applied to it.
 */

namespace helix {

/**
 * @brief Tool number implied by a Klipper extruder name: "extruder" = 0, "extruderN" = N.
 *
 * A positional convention, and the third of three numbering systems that can all
 * disagree on one machine: Klipper's `tool T<n>` objects, AFC's per-lane `map`
 * aliases, and extruder-name position. On the reporter's toolchanger AFC maps T0
 * to the `extruder5` lane while Klipper's `tool T0` is `extruder`
 * (prestonbrown/helixscreen#1229).
 *
 * Returns nullopt rather than a fallback for anything that is not an
 * `extruder`-prefixed name, so callers must decide what an unidentifiable
 * extruder means. Badge rendering needs that distinction: silently mapping an
 * empty name to 0 would label every toolhead "E0" on backends that never
 * populate SlotInfo::extruder_name at all.
 *
 * @param ext_name Klipper extruder object name, e.g. "extruder" or "extruder5"
 * @return Tool number, or nullopt if @p ext_name is not a valid extruder name
 */
[[nodiscard]] inline std::optional<int> tool_number_for_extruder(std::string_view ext_name) {
    constexpr std::string_view PREFIX = "extruder";
    if (ext_name == PREFIX) {
        return 0;
    }
    if (ext_name.size() <= PREFIX.size() || ext_name.substr(0, PREFIX.size()) != PREFIX) {
        return std::nullopt;
    }
    const std::string_view digits = ext_name.substr(PREFIX.size());
    // No real machine has a four-digit extruder index; the bound also keeps the
    // accumulate below well clear of overflow without pulling in <climits>.
    if (digits.size() > 3 || !std::all_of(digits.begin(), digits.end(),
                                          [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : digits) {
        value = value * 10 + (c - '0');
    }
    return value;
}

/**
 * @brief Whether @p name is a Klipper extruder object name.
 *
 * Exactly the grammar tool_number_for_extruder() parses, so a name that
 * classifies as an extruder here is guaranteed to yield a number there — the
 * two can never disagree.
 *
 * Rejects `extruder_stepper` (and any other `extruder`-prefixed object that is
 * not a numbered extruder). Callers that deliberately want the looser
 * "anything extruder-ish" match — hardware discovery, sensor enumeration —
 * should keep their own prefix test rather than use this.
 */
[[nodiscard]] inline bool is_extruder_name(std::string_view name) {
    return tool_number_for_extruder(name).has_value();
}

} // namespace helix
