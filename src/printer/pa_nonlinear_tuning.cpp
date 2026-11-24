// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_nonlinear_tuning.h"

#include "json_utils.h"

#include <algorithm>

#include "hv/json.hpp"

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

nlohmann::json values_json(const NonlinearValues& v) {
    return {{"model", static_cast<int>(v.model)}, {"advance", v.linear_advance},
            {"offset", v.nonlinear_offset},       {"velocity", v.linearization_velocity},
            {"time_offset", v.time_offset},       {"smooth_time", v.smooth_time}};
}

NonlinearValues values_from(const nlohmann::json& j) {
    using json_util::safe_float;
    NonlinearValues v;
    v.model = json_util::safe_int(j, "model") == static_cast<int>(ModelKind::Recipr)
                  ? ModelKind::Recipr
                  : ModelKind::Tanh;
    v.linear_advance = safe_float(j, "advance", v.linear_advance);
    v.nonlinear_offset = safe_float(j, "offset", v.nonlinear_offset);
    v.linearization_velocity = safe_float(j, "velocity", v.linearization_velocity);
    v.time_offset = safe_float(j, "time_offset", v.time_offset);
    v.smooth_time = safe_float(j, "smooth_time", v.smooth_time);
    return v;
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

void NonlinearProcedure::remember(float height_mm) {
    history_.push_back(Judged{index_, repeats_, values_, height_mm});
}

std::optional<float> NonlinearProcedure::go_back() {
    if (history_.empty()) {
        return std::nullopt;
    }
    const Judged j = history_.back();
    history_.pop_back();
    index_ = j.index;
    repeats_ = j.repeats;
    values_ = j.values;
    return j.height_mm;
}

void NonlinearProcedure::record_height(float height_mm) {
    if (done()) {
        return;
    }
    remember(height_mm);
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
    remember(height_mm);
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

nlohmann::json NonlinearProcedure::to_json() const {
    nlohmann::json history = nlohmann::json::array();
    for (const auto& h : history_) {
        history.push_back({{"index", h.index},
                           {"repeats", h.repeats},
                           {"values", values_json(h.values)},
                           {"height", h.height_mm}});
    }
    return {{"kind", static_cast<int>(kind_)},
            {"passes", passes_.size()},
            {"index", index_},
            {"repeats", repeats_},
            {"values", values_json(values_)},
            {"history", std::move(history)}};
}

std::optional<NonlinearProcedure> NonlinearProcedure::from_json(const nlohmann::json& j) {
    const auto* values = json_util::find_object(j, "values");
    if (!values) {
        return std::nullopt;
    }
    const auto kind = json_util::safe_int(j, "kind") == static_cast<int>(ExtruderKind::Bowden)
                          ? ExtruderKind::Bowden
                          : ExtruderKind::DirectDrive;
    NonlinearProcedure p(kind, values_from(*values).model);
    // A pass index means something only under the same pass order.
    const size_t passes = p.passes_.size();
    p.index_ = json_util::safe_size_t(j, "index");
    if (json_util::safe_size_t(j, "passes") != passes || p.index_ > passes) {
        return std::nullopt;
    }
    p.repeats_ = std::max(0, json_util::safe_int(j, "repeats"));
    p.values_ = values_from(*values);
    if (const auto* history = json_util::find_array(j, "history")) {
        for (const auto& h : *history) {
            const auto* hv = json_util::find_object(h, "values");
            Judged judged{json_util::safe_size_t(h, "index"),
                          std::max(0, json_util::safe_int(h, "repeats")),
                          values_from(hv ? *hv : nlohmann::json::object()),
                          json_util::safe_float(h, "height")};
            if (judged.index >= passes) {
                return std::nullopt;
            }
            p.history_.push_back(judged);
        }
    }
    return p;
}

} // namespace helix::pa
