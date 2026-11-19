// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <optional>
#include <string>
#include <string_view>

/**
 * @file afc_fault_position.h
 * @brief Maps an AFC lane-fault message to where the filament stopped.
 *
 * AFC ships a monospace position diagram welded to each fault sentence:
 *
 * @code
 * lane1 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH
 * ||=====||====||==>--||
 * TRG   LOAD   HUB   TOOL
 * @endcode
 *
 * The art is a **hardcoded string literal per error site**, not a rendering of live
 * sensor state, so it carries nothing the sentence does not. Two different faults
 * (pre- and post-extruder-gear) even emit identical art, and the column spacing
 * differs between `AFC.py` and `AFC_BoxTurtle.py`. Parsing it would therefore be both
 * brittle and strictly less precise than reading the sentence.
 *
 * So we map the **message text** to a position instead, and strip the art rows from
 * what we display. Unrecognised messages return `std::nullopt` and are returned
 * untouched by afc_strip_position_diagram() — upstream rewording degrades to today's
 * plain-text rendering rather than breaking.
 *
 * The message→position contract is documented in
 * `docs/devel/FILAMENT_MANAGEMENT.md` § "AFC console response contract".
 *
 * Pure functions over strings — no LVGL, no printer state, no locking.
 */

namespace helix::afc {

/// The lane sensor an AFC fault message places the fault at: `CHECK FILAMENT AT TRIGGER` and
/// `LOAD TRIGGER NOT TRIGGERED` both fire with the prep switch made and the load switch never
/// reached, so both are the load sensor; NONE for any other message.
[[nodiscard]] LaneSensor afc_fault_lane_sensor(std::string_view message);

/**
 * @brief Where the filament had reached when AFC raised this fault.
 *
 * Matching is case-insensitive on stable message fragments, each required to sit on
 * word boundaries so a lane name or an embedded filename cannot trip it.
 *
 *   `LOAD TRIGGER NOT TRIGGERED`         -> SPOOL     short of the lane trigger
 *   `CHECK FILAMENT AT TRIGGER`          -> SPOOL     short of the lane trigger
 *   `did not trigger hub sensor`         -> HUB       past lane, short of the hub
 *   `pre extruder gear toolhead sensor`  -> OUTPUT    past hub, short of the toolhead
 *   `post extruder gear toolhead sensor` -> TOOLHEAD  at toolhead, short of the gears
 *
 * @param message The fault message as AFC emitted it (lane prefix and `!!` optional).
 * @return The reached segment, or `std::nullopt` when the message is not one we know.
 */
[[nodiscard]] std::optional<PathSegment> afc_fault_position(std::string_view message);

/**
 * @brief Remove AFC's ASCII position-diagram rows from a recognised fault message.
 *
 * Drops only the two art rows — the bar row (`||=====||====||==>--||`) and the label
 * row (`TRG   LOAD   HUB   TOOL`) — leaving every other line byte-for-byte intact.
 *
 * Gated on recognition: a message afc_fault_position() does not recognise is returned
 * unchanged, so nothing we failed to understand can be mangled.
 *
 * @param message The fault message as AFC emitted it.
 * @return The message without its art rows, or the input unchanged.
 */
[[nodiscard]] std::string afc_strip_position_diagram(std::string_view message);

} // namespace helix::afc
