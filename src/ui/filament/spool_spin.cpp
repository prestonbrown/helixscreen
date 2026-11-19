// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spool_spin.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace helix::ui {

uint16_t spool_spin_step_ms(float filament_mm_s) {
    constexpr float kTwoPi = 6.2831853f;
    const float turns_per_s = std::max(filament_mm_s, 0.0f) / (kTwoPi * kSpoolSpinWoundRadiusMm) *
                              kSpoolSpinVisualMultiplier;
    const float floor_turns_per_s = 1.0f / kSpoolSpinFloorTurnS;
    const float turn_s = 1.0f / std::max(turns_per_s, floor_turns_per_s);
    const float clamped = std::max(turn_s, kSpoolSpinCapTurnS);
    return static_cast<uint16_t>(std::lround(clamped * 1000.0f / kSpoolSpinPhasesPerTurn));
}

int spool_payout_turn(float outlet_back, float outlet_up) {
    // An outlet near level counts as below and one near plumb as in front, so a unit whose
    // outlets sit level with or under its spools turns them all one way.
    const bool front = outlet_back <= std::abs(outlet_up) * 0.5f;
    const bool below = outlet_up <= std::abs(outlet_back) * 0.5f;
    return front == below ? 1 : -1;
}

namespace {

/// The feed in mm/s from the live extruder velocity, 0 when unknown or retracting.
float feed_mm_s(int velocity_centimm) {
    return velocity_centimm > 0 ? static_cast<float>(velocity_centimm) / 100.0f : 0.0f;
}

/// A load moves the filament unit's drive, which the extruder velocity misses until the
/// filament reaches the extruder.
float load_feed_mm_s(int velocity_centimm) {
    return std::max(feed_mm_s(std::abs(velocity_centimm)), kSpoolSpinLoadFeedMmS);
}

} // namespace

SpoolSpin decide_spool_spin(const SpoolSpinInput& in) {
    if (!in.animations_enabled || in.reduced_effects)
        return {};
    if (in.action == AmsAction::LOADING) {
        const int moving = in.target_slot >= 0 ? in.target_slot : in.current_slot;
        return in.lane == moving
                   ? SpoolSpin{1, spool_spin_step_ms(load_feed_mm_s(in.extruder_velocity_centimm))}
                   : SpoolSpin{};
    }
    if (in.action == AmsAction::UNLOADING)
        return in.lane == in.current_slot
                   ? SpoolSpin{-1, spool_spin_step_ms(load_feed_mm_s(in.extruder_velocity_centimm))}
                   : SpoolSpin{};
    if (in.action == AmsAction::IDLE && in.printing && in.lane_active_loaded)
        return {1, spool_spin_step_ms(feed_mm_s(in.extruder_velocity_centimm))};
    return {};
}

} // namespace helix::ui
