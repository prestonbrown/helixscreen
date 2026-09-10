// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_zone_presentation.h
 * @brief Pure classification of an environment zone for display.
 *
 * The overview row, the selector tab and the detail header all need the same
 * answers about a zone. They render them differently, so what is shared here is the
 * decision and never the drawing.
 *
 * LVGL-free and translation-free: callers pass translated words in.
 */

#pragma once

#include "ams_environment_zone.h"

#include <string>
#include <vector>

namespace helix::ui {

/// How a zone's humidity reads against filament storage.
enum class ZoneVerdict : int {
    Ok = 0,       ///< Dry enough for everything the user is likely storing
    Marginal = 1, ///< Fine for PLA, not for ASA or ABS
    TooHumid = 2, ///< Above threshold for everything
    Unknown = 3   ///< No humidity sensor, so no verdict can be given
};

/// Humidity boundaries, shared so the row and the detail strip cannot disagree.
inline constexpr float kZoneHumidityOkMax = 35.0f;
inline constexpr float kZoneHumidityMarginalMax = 55.0f;

/**
 * @brief Classify a zone's humidity.
 *
 * A zone with no humidity sensor is `Unknown` rather than `Ok`: an unmeasured box is
 * not a dry box, and the passive rigs this serves exist to answer exactly that.
 */
[[nodiscard]] ZoneVerdict zone_verdict(const helix::printer::EnvironmentZone& zone);

/// Which word a status row prints for a zone.
enum class ZoneStatusKind {
    Drying,  ///< A cycle is running
    Passive, ///< No dryer to drive
    Verdict  ///< Print the humidity verdict itself
};

/**
 * @brief What a status row reports for a zone.
 *
 * The word a row prints and the colour it renders at both come from this one value, so
 * a row cannot name one state while colouring for another.
 */
struct ZoneStatus {
    ZoneStatusKind kind;  ///< Which word to print
    ZoneVerdict severity; ///< Which colour band to render at
};

/**
 * @brief Classify what a status row should say about a zone, and how loudly.
 *
 * An active cycle carries no severity: the machine is already acting on the humidity, so
 * a wet reading mid-cycle is the cycle working rather than something to flag. A passive
 * zone keeps its verdict, because a box nobody can drive is exactly the one whose colour
 * is the only signal the row gives.
 */
[[nodiscard]] ZoneStatus zone_status(const helix::printer::EnvironmentZone& zone);

/**
 * @brief What the zone is called on screen.
 *
 * A zone the backend named uses that name. A single-slot zone with no name is one slot,
 * numbered one-based. Anything else falls back to the system type and a unit ordinal.
 *
 * @param unit_word Translated word for "Unit"
 * @param slot_word Translated word for "Slot"
 * @param type_name Filament system name, for the last-resort form
 */
[[nodiscard]] std::string zone_display_label(const helix::printer::EnvironmentZone& zone,
                                             const std::string& unit_word,
                                             const std::string& slot_word,
                                             const std::string& type_name);

/// Whether a set of zones covers more than one unit, which is when grouping headers earn
/// their space.
[[nodiscard]] bool zones_span_units(const std::vector<helix::printer::EnvironmentZone>& zones);

/**
 * @brief The slots a zone covers, as the user reads them.
 *
 * One-based and contracted to a range. Slots are contiguous within a zone on every
 * shape we serve, so a range is honest; an empty zone gets an empty string rather
 * than a range of nothing.
 *
 * @param plural_word Translated word for "Slots"
 * @param singular_word Translated word for "Slot"
 */
[[nodiscard]] std::string zone_slot_text(const helix::printer::EnvironmentZone& zone,
                                         const std::string& plural_word,
                                         const std::string& singular_word);

} // namespace helix::ui
