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

inline constexpr uint16_t kSpoolSpinPrintStepMs = 240;     ///< printing, flow unknown
inline constexpr uint16_t kSpoolSpinPrintSlowStepMs = 320; ///< printing, trickling
inline constexpr uint16_t kSpoolSpinPrintFastStepMs = 120; ///< printing, high flow
inline constexpr uint16_t kSpoolSpinLoadStepMs = 40;       ///< loading or unloading

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
