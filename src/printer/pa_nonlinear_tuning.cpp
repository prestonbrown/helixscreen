// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_nonlinear_tuning.h"

#include "json_utils.h"

#include <algorithm>
#include <cmath>

#include "hv/json.hpp"

namespace helix::pa {

namespace {

/// Kalico's default sweeps from zero: per mm of tower height.
constexpr float kAdvanceFactor = 0.001f;
constexpr float kAdvanceFactorBowden = 0.01f;
constexpr float kOffsetFactor = 0.01f;
constexpr float kOffsetFactorBowden = 0.02f;
constexpr float kTimeOffsetFactor = 0.0001f;

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

void NonlinearProcedure::remember(float height_mm, std::optional<float> left_mm,
                                  std::optional<float> front_mm) {
    history_.push_back(Judged{index_, repeats_, values_, height_mm, left_mm, front_mm});
}

std::optional<float> NonlinearProcedure::go_back() {
    const auto j = go_back_judgement();
    return j ? std::optional<float>(j->height_mm) : std::nullopt;
}

std::optional<NonlinearProcedure::Reopened> NonlinearProcedure::go_back_judgement() {
    if (history_.empty()) {
        return std::nullopt;
    }
    const Judged j = history_.back();
    history_.pop_back();
    index_ = j.index;
    repeats_ = j.repeats;
    values_ = j.values;
    return Reopened{j.height_mm, j.left_mm, j.front_mm};
}

void NonlinearProcedure::record_height(float height_mm) {
    if (done()) {
        return;
    }
    remember(height_mm);
    const NonlinearPass& p = current();
    apply(p.sweep.param, value_at(p.sweep, height_mm) * kept_fraction(p));
    advance();
}

float NonlinearProcedure::kept_fraction(const NonlinearPass& pass) const {
    return kind_ == ExtruderKind::Bowden && pass.sweep.param == NlParam::Advance
               ? kBowdenAdvanceKept
               : 1.0f;
}

void NonlinearProcedure::record_verdict(SideVerdict verdict, float height_mm) {
    judge_two_sided(verdict, height_mm, std::nullopt, std::nullopt);
}

void NonlinearProcedure::record_heights(float left_mm, float front_mm) {
    judge_two_sided(verdict_from_heights(left_mm, front_mm), (left_mm + front_mm) / 2.0f, left_mm,
                    front_mm);
}

void NonlinearProcedure::judge_two_sided(SideVerdict verdict, float height_mm,
                                         std::optional<float> left_mm,
                                         std::optional<float> front_mm) {
    if (done()) {
        return;
    }
    remember(height_mm, left_mm, front_mm);
    const NonlinearPass& p = current();
    const auto c = correction(verdict);
    if (!c) {
        apply(p.sweep.param, value_at(p.sweep, height_mm));
        advance();
        return;
    }
    if (c->param == NlParam::Offset) {
        values_.nonlinear_offset = c->to;
    } else {
        values_.linear_advance = c->to;
    }
    ++repeats_;
}

std::optional<Correction> NonlinearProcedure::correction(SideVerdict verdict) const {
    if (done() || verdict == SideVerdict::Same || repeats_ >= kMaxRepeats) {
        return std::nullopt;
    }
    // The other parameter of the pair moves (Kalico's guide, step 8). Direct
    // drive sweeps the advance and corrects the offset: a left side best lower
    // than the front has too much offset. Bowden sweeps the offset and corrects
    // the advance, the other way round.
    const bool left_lower = verdict == SideVerdict::LeftLower;
    Correction c = current().sweep.param == NlParam::Advance
                       ? Correction{NlParam::Offset, !left_lower}
                       : Correction{NlParam::Advance, left_lower};
    c.from = c.param == NlParam::Offset ? values_.nonlinear_offset : values_.linear_advance;
    c.to = c.from * (c.raise ? 1.0f + kConvergeStep : 1.0f - kConvergeStep);
    return c;
}

float tower_reached_mm(float reached_mm, float z_mm) {
    // A layer change and a small z hop stay well inside this; an end-of-print
    // lift does not.
    constexpr float kMaxClimbMm = 1.0f;
    return z_mm > reached_mm && z_mm <= reached_mm + kMaxClimbMm ? z_mm : reached_mm;
}

TowerLayerProgress tower_layer_progress(float z_mm, float height_mm, float layer_mm) {
    TowerLayerProgress p;
    if (height_mm <= 0.0f || layer_mm <= 0.0f) {
        return p;
    }
    p.layers = std::max(1, static_cast<int>(std::lround(height_mm / layer_mm)));
    // A layer's nozzle height is its top, so 0.2 mm is layer 1 and 1.0 mm layer 5.
    const int layer = static_cast<int>(std::lround(z_mm / layer_mm));
    p.layer = std::clamp(layer, 1, p.layers);
    p.fraction = std::clamp(z_mm / height_mm, 0.0f, 1.0f);
    return p;
}

TowerTimeLeft::TowerTimeLeft(int expected_print_s, float height_mm, float layer_mm)
    : expected_s_(std::max(0, expected_print_s)), height_mm_(height_mm), layer_mm_(layer_mm) {}

void TowerTimeLeft::sample(float z_mm, double t_s) {
    if (first_t_ < 0.0) {
        first_t_ = t_s;
    }
    z_ = tower_reached_mm(z_, z_mm);
    t_ = t_s;
    if (climb_z_ < 0.0f && z_ >= 2.0f * layer_mm_ - layer_mm_ / 2.0f) {
        climb_z_ = z_;
        climb_t_ = t_s;
    }
}

int TowerTimeLeft::seconds_left() const {
    const float left_mm = std::max(0.0f, height_mm_ - z_);
    if (climb_z_ >= 0.0f && z_ - climb_z_ >= kTowerRateClimbMm) {
        const double per_mm = (t_ - climb_t_) / static_cast<double>(z_ - climb_z_);
        return static_cast<int>(std::lround(per_mm * left_mm));
    }
    const double printed = first_t_ < 0.0 ? 0.0 : t_ - first_t_;
    return std::max(0, static_cast<int>(std::lround(expected_s_ - printed)));
}

SideVerdict verdict_from_heights(float left_mm, float front_mm) {
    // A hair of slack so a typed 26.5 against 27.5 counts as within 1 mm.
    constexpr float kSlack = 1e-4f;
    if (std::abs(left_mm - front_mm) <= kSameHeightMm + kSlack) {
        return SideVerdict::Same;
    }
    return left_mm < front_mm ? SideVerdict::LeftLower : SideVerdict::LeftHigher;
}

FaceLayout tower_face_layout(TowerFace face, float size_x_mm, float size_y_mm) {
    // The slow middle is SLOW_NOTCH_SIZE wide on any face, and the very slow
    // segment VERY_SLOW_SEG long.
    constexpr float kSlowNotchMm = 10.0f;
    constexpr float kVerySlowMm = 0.2f;
    FaceLayout f;
    if (face == TowerFace::Left) {
        // Back corner to front corner: medium, slow middle with the very slow
        // segment at its center, medium.
        f.width_mm = size_y_mm;
        const float half = kSlowNotchMm / 2.0f / size_y_mm;
        const float vs = kVerySlowMm / 2.0f / size_y_mm;
        f.segments = {{0.0f, 0.5f - half, SpeedKind::Medium},
                      {0.5f - half, 0.5f - vs, SpeedKind::Slow},
                      {0.5f - vs, 0.5f + vs, SpeedKind::VerySlow},
                      {0.5f + vs, 0.5f + half, SpeedKind::Slow},
                      {0.5f + half, 1.0f, SpeedKind::Medium}};
        f.spots = {{0.5f - half, SpotKind::SlowDown},
                   {0.5f, SpotKind::VerySlowSpot},
                   {0.5f + half, SpotKind::SpeedUp}};
        return f;
    }
    // Left corner to right corner: fast with a dwell at size_x / 3 left of
    // center, slow middle, fast, a very slow segment at size_x / 3 right of
    // center, fast.
    f.width_mm = size_x_mm;
    const float half = kSlowNotchMm / 2.0f / size_x_mm;
    const float third = (size_x_mm / 3.0f) / size_x_mm;
    const float left_third = 0.5f - third;
    const float right_third = 0.5f + third;
    const float vs = kVerySlowMm / size_x_mm;
    f.segments = {{0.0f, 0.5f - half, SpeedKind::Fast},
                  {0.5f - half, 0.5f + half, SpeedKind::Slow},
                  {0.5f + half, right_third, SpeedKind::Fast},
                  {right_third, right_third + vs, SpeedKind::VerySlow},
                  {right_third + vs, 1.0f, SpeedKind::Fast}};
    f.spots = {{left_third, SpotKind::Pause},
               {0.5f - half, SpotKind::SlowDown},
               {0.5f + half, SpotKind::SpeedUp},
               {right_third, SpotKind::VerySlowSpot}};
    return f;
}

std::vector<TowerFace> judged_faces(TowerSide side) {
    switch (side) {
    case TowerSide::Left:
        return {TowerFace::Left};
    case TowerSide::Front:
        return {TowerFace::Front};
    case TowerSide::LeftVsFront:
        break;
    }
    return {TowerFace::Left, TowerFace::Front};
}

std::vector<SpotKind> judged_spots(const NonlinearPass& pass) {
    if (pass.sweep.param == NlParam::TimeOffset) {
        return {SpotKind::Pause, SpotKind::VerySlowSpot};
    }
    return {SpotKind::SlowDown, SpotKind::SpeedUp, SpotKind::Pause, SpotKind::VerySlowSpot};
}

nlohmann::json NonlinearProcedure::to_json() const {
    nlohmann::json history = nlohmann::json::array();
    for (const auto& h : history_) {
        nlohmann::json entry = {{"index", h.index},
                                {"repeats", h.repeats},
                                {"values", values_json(h.values)},
                                {"height", h.height_mm}};
        if (h.left_mm && h.front_mm) {
            entry["left"] = *h.left_mm;
            entry["front"] = *h.front_mm;
        }
        history.push_back(std::move(entry));
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
                          json_util::safe_float(h, "height"),
                          json_util::read_field<float>(h, "left"),
                          json_util::read_field<float>(h, "front")};
            if (judged.index >= passes) {
                return std::nullopt;
            }
            p.history_.push_back(judged);
        }
    }
    return p;
}

} // namespace helix::pa
