// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spool_spin.h"

namespace helix::ui {

namespace {

constexpr int kSlowFlowCentimm = 150; // below 1.5 mm/s the spool trickles
constexpr int kHighFlowCentimm = 500; // from 5 mm/s the spool is visibly busy

uint16_t print_step_ms(int velocity_centimm) {
    if (velocity_centimm <= 0)
        return kSpoolSpinPrintStepMs;
    if (velocity_centimm < kSlowFlowCentimm)
        return kSpoolSpinPrintSlowStepMs;
    if (velocity_centimm < kHighFlowCentimm)
        return kSpoolSpinPrintStepMs;
    return kSpoolSpinPrintFastStepMs;
}

} // namespace

SpoolSpin decide_spool_spin(const SpoolSpinInput& in) {
    if (!in.animations_enabled || in.reduced_effects)
        return {};
    if (in.action == AmsAction::LOADING) {
        const int moving = in.target_slot >= 0 ? in.target_slot : in.current_slot;
        return in.lane == moving ? SpoolSpin{1, kSpoolSpinLoadStepMs} : SpoolSpin{};
    }
    if (in.action == AmsAction::UNLOADING)
        return in.lane == in.current_slot ? SpoolSpin{-1, kSpoolSpinLoadStepMs} : SpoolSpin{};
    if (in.action == AmsAction::IDLE && in.printing && in.lane_active_loaded)
        return {1, print_step_ms(in.extruder_velocity_centimm)};
    return {};
}

} // namespace helix::ui
