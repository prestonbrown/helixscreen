// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#if HELIX_ENABLE_SCREENSAVER
#include "screensaver_level_store.h"
#include "screensaver_registry.h"

#include <optional>
#endif

/**
 * @file clock_face.h
 * @brief What a home clock tile shows: its face, whether the face fits, and how often it moves
 *
 * No LVGL. The faces are the clock savers' own painters (helix::ClockFaceView hosts them), so
 * everything but Text exists only in a build with screensavers.
 */

namespace helix {

/// The faces, in the order the picker lists them.
enum class ClockFace : uint8_t { TEXT, CHRONOGRAPH, STATION, NEON, BAUHAUS, ORRERY, FLIP, NIXIE };
inline constexpr size_t kClockFaceCount = 8;

/// The face's stable name, stored at the widget instance's "face" config key.
const char* clock_face_id(ClockFace face);

/// The face `id` names; Text for an empty or unknown name.
ClockFace parse_clock_face(std::string_view id);

/// The face's English name; the picker translates it.
const char* clock_face_label(ClockFace face);

#if HELIX_ENABLE_SCREENSAVER

/// Smallest dial diameter, in px, each analog face is shown at. The chronograph's subdials and
/// date window need more room than a plain dial.
inline constexpr int32_t kClockMinChronoDialPx = 110;
inline constexpr int32_t kClockMinDialPx = 72;
/// Smallest digit width, in px, with the four digits of HH:MM in one row: a flip card, a nixie
/// tube (its glass is narrower than a card for the same glyph).
inline constexpr int32_t kClockMinFlipCardPx = 18;
inline constexpr int32_t kClockMinNixieTubePx = 16;

/// The face's size on a `w` x `h` canvas, in the unit clock_face_min_px() states: an analog
/// face's dial diameter, or a flip card's or nixie tube's width with HH:MM in one row. 0 for Text.
int32_t clock_face_measure_px(ClockFace face, int32_t w, int32_t h);

/// The smallest clock_face_measure_px() `face` is shown at; 0 for Text.
int32_t clock_face_min_px(ClockFace face);

/// `chosen` where it fits a `w` x `h` canvas, else Text.
ClockFace clock_face_shown(ClockFace chosen, int32_t w, int32_t h);

/// The saver whose painter and measurement the face shares; OFF for Text.
ScreensaverType clock_face_saver(ClockFace face);

/// The fastest rung a tile runs: a home tile never paints at the saver's 16 ms top rung.
inline constexpr size_t kClockTileFastestLevel = 1;

/// How a face moves on this board.
struct ClockFaceTempo {
    size_t level = 0;       ///< the rung of the face's saver ladder it paints at
    bool seconds = false;   ///< analog: the seconds hand shows; otherwise a change a minute
    bool animate = false;   ///< flip and nixie: a change flips or fades rather than swaps
    uint32_t period_ms = 0; ///< the rung's frame period, while anything moves
};

/**
 * @brief How `face` moves, from what its saver measured on this board
 *
 * `standing` is ScreensaverManager::standing_here() for clock_face_saver(face). Too heavy: the
 * bottom rung, no seconds, a change a minute. Measured: the settled rung, never faster than
 * kClockTileFastestLevel, with seconds. Never measured: the bottom rung, with seconds on two or
 * more `cpu_cores`. Flip and nixie animate where their rung animates.
 */
ClockFaceTempo clock_face_tempo(ClockFace face, const std::optional<ui::SaverLevelEntry>& standing,
                                int cpu_cores);

/// Milliseconds from `seconds_of_day` plus `ms_into_second` to the next whole minute: 1 to 60000.
uint32_t clock_ms_to_next_minute(int32_t seconds_of_day, uint32_t ms_into_second);

#endif // HELIX_ENABLE_SCREENSAVER

} // namespace helix
