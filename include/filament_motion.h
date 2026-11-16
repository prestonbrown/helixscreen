// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

/**
 * @file filament_motion.h
 * @brief The manual filament move the Extrude / Retract buttons are running.
 *
 * Published as the int subject "filament_manual_motion" (FilamentPanel owns
 * it) and read by every widget that draws filament moving: the filament
 * panel's toolhead glyph and the AMS path canvas.
 */

namespace helix {

/// Values of the "filament_manual_motion" subject.
enum class FilamentMotion : int {
    Idle = 0,
    Extrude = 1, ///< filament travels toward the nozzle
    Retract = 2, ///< filament travels back toward the spool
};

/// Name the motion subject is registered under in the XML registry.
inline constexpr const char* kFilamentMotionSubjectName = "filament_manual_motion";

/// Shortest time a manual move shows as running, so a tiny or fast move still
/// reads as motion.
inline constexpr uint32_t kMinManualMotionMs = 400;
/// Longest, so a bogus speed setting cannot pin the glyph on for minutes.
inline constexpr uint32_t kMaxManualMotionMs = 120000;

/**
 * @brief How long a manual move of @p length_mm at @p speed_mm_s takes to run.
 *
 * Klipper answers a G1 as soon as it is queued, before the filament has
 * moved, so the move's own duration is what the motion display has to
 * last. Clamped to [kMinManualMotionMs, kMaxManualMotionMs]; a non-positive
 * length or speed gives the minimum.
 */
uint32_t manual_motion_duration_ms(int length_mm, int speed_mm_s);

/// Map a subject value back to the enum; anything unknown reads as Idle.
FilamentMotion filament_motion_from_int(int value);

} // namespace helix
