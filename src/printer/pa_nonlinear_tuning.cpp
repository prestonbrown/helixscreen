// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_nonlinear_tuning.h"

namespace helix::pa {

namespace {

/// Kalico's default sweeps from zero: per mm of tower height.
constexpr float kAdvanceFactor = 0.001f;
constexpr float kAdvanceFactorBowden = 0.01f;
constexpr float kOffsetFactor = 0.01f;
constexpr float kOffsetFactorBowden = 0.02f;
constexpr float kTimeOffsetFactor = 0.0001f;

/// What a Bowden run keeps of its first advance: the offset adds a boost of
/// its own at low speed.
constexpr float kBowdenAdvanceKept = 0.8f;
constexpr float kConvergeStep = 0.1f;

NonlinearPass pass(NlParam p, float factor, TowerSide side) {
    return NonlinearPass{TowerSweep{p, 0.0f, factor}, side};
}

} // namespace

NonlinearProcedure::NonlinearProcedure(ExtruderKind kind, ModelKind model) : kind_(kind) {
    values_.model = model == ModelKind::Recipr ? ModelKind::Recipr : ModelKind::Tanh;
    if (kind == ExtruderKind::Bowden) {
        values_.linearization_velocity = 2.0f;
        passes_ = {pass(NlParam::Advance, kAdvanceFactorBowden, TowerSide::Front),
                   pass(NlParam::Offset, kOffsetFactorBowden, TowerSide::LeftVsFront),
                   pass(NlParam::TimeOffset, kTimeOffsetFactor, TowerSide::Front),
                   pass(NlParam::Offset, kOffsetFactorBowden, TowerSide::LeftVsFront)};
    } else {
        values_.linearization_velocity = 1.0f;
        passes_ = {pass(NlParam::Offset, kOffsetFactor, TowerSide::Left),
                   pass(NlParam::Advance, kAdvanceFactor, TowerSide::LeftVsFront),
                   pass(NlParam::TimeOffset, kTimeOffsetFactor, TowerSide::Front)};
    }
}

void NonlinearProcedure::apply(NlParam param, float value) {
    switch (param) {
    case NlParam::Advance:
        values_.linear_advance = value;
        break;
    case NlParam::Offset:
        values_.nonlinear_offset = value;
        break;
    case NlParam::TimeOffset:
        values_.time_offset = value;
        break;
    }
}

void NonlinearProcedure::advance() {
    ++index_;
    repeats_ = 0;
}

void NonlinearProcedure::record_height(float height_mm) {
    if (done()) {
        return;
    }
    const NonlinearPass& p = current();
    float value = value_at(p.sweep, height_mm);
    if (kind_ == ExtruderKind::Bowden && p.sweep.param == NlParam::Advance) {
        value *= kBowdenAdvanceKept;
    }
    apply(p.sweep.param, value);
    advance();
}

void NonlinearProcedure::record_verdict(SideVerdict verdict, float height_mm) {
    if (done()) {
        return;
    }
    const NonlinearPass& p = current();
    if (verdict == SideVerdict::Same || repeats_ >= kMaxRepeats) {
        apply(p.sweep.param, value_at(p.sweep, height_mm));
        advance();
        return;
    }
    // The other parameter of the pair moves. Direct drive sweeps the advance
    // and corrects the offset: a left side best lower than the front has too
    // much offset. Bowden sweeps the offset and corrects the advance, the
    // other way round.
    const bool left_lower = verdict == SideVerdict::LeftLower;
    if (p.sweep.param == NlParam::Advance) {
        values_.nonlinear_offset *= left_lower ? 1.0f - kConvergeStep : 1.0f + kConvergeStep;
    } else {
        values_.linear_advance *= left_lower ? 1.0f + kConvergeStep : 1.0f - kConvergeStep;
    }
    ++repeats_;
}

} // namespace helix::pa
