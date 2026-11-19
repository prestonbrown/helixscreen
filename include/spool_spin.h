// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <cstdint>

namespace helix::ui {

/// How a lane's spool turns: @p direction 0 stands still, +1 pays out
/// (printing, loading), -1 winds back (unloading). @p step_ms is the time per
/// vent phase (spool_vents.h), so a smaller value spins faster.
struct SpoolSpin {
    int8_t direction = 0;
    uint16_t step_ms = 0;

    bool operator==(const SpoolSpin& o) const {
        return direction == o.direction && step_ms == o.step_ms;
    }
    bool operator!=(const SpoolSpin& o) const {
        return !(*this == o);
    }
};

/// Phases in one whole turn of a spool; a phase is 7.5 degrees (spool_vent_phase_count).
inline constexpr int kSpoolSpinPhasesPerTurn = 48;
/// How many times faster than the real spool a drawn one turns: a real spool feeding a print
/// turns too slowly to read as moving.
inline constexpr float kSpoolSpinVisualMultiplier = 2.0f;
/// The slowest a feeding spool turns: at least one turn in this many seconds.
inline constexpr float kSpoolSpinFloorTurnS = 3.5f;
/// The fastest a spool turns, so its vents never strobe against the frame rate.
inline constexpr float kSpoolSpinCapTurnS = 1.0f;
/// The radius filament leaves a typical part-used 1 kg spool at.
inline constexpr float kSpoolSpinWoundRadiusMm = 80.0f;
/// A load or unload's feed when the extruder reports none, as the filament unit drives it.
inline constexpr float kSpoolSpinLoadFeedMmS = 150.0f;

/// The time per vent phase of a spool feeding @p filament_mm_s of filament: the real turn rate
/// times kSpoolSpinVisualMultiplier, held between the floor and the cap. Tray and art spools
/// both turn at this rate.
uint16_t spool_spin_step_ms(float filament_mm_s);

/// Everything the rotation of one lane's spool depends on.
struct SpoolSpinInput {
    int lane = -1;
    int current_slot = -1;
    int target_slot = -1; ///< the lane a load is headed for, -1 when none is named
    AmsAction action = AmsAction::IDLE;
    bool printing = false;             ///< PrintJobState::PRINTING, not paused
    bool lane_active_loaded = false;   ///< this lane is the one feeding the extruder
    int extruder_velocity_centimm = 0; ///< live extruder velocity in 0.01 mm/s; <= 0 is unknown
    bool animations_enabled = false;
    bool reduced_effects = false; ///< this board is too slow for decorative animation
};

/// The rotation for one lane. A load or unload spins the moving lane fast; a
/// printing lane turns slowly, paced by the flow when it is known; anything
/// else stands still, as does every lane with animations off or on a slow board.
SpoolSpin decide_spool_spin(const SpoolSpinInput& input);

} // namespace helix::ui
