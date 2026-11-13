// SPDX-License-Identifier: GPL-3.0-or-later
#include "motor_ripple_plan.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace helix::calibration::ripple {

namespace {

constexpr float kEdgeInsetMm = 10.0f;
constexpr float kApproachMaxMmS = 200.0f;
constexpr int kSettleDwellMs = 250;
constexpr uint32_t kMinStepTimeoutMs = 30000;
constexpr float kStepTimeoutFactor = 4.0f;
constexpr float kApproachAllowanceS = 2.0f;

struct Direction {
    float ux = 0.0f;
    float uy = 0.0f;
    float toolhead_per_belt = 1.0f; ///< toolhead speed for one unit of belt speed
};

Direction direction_for(Kinematics k, MotorId m) {
    const float r = 1.0f / std::sqrt(2.0f);
    if (k == Kinematics::CoreXY) {
        return m == MotorId::StepperX ? Direction{r, r, r} : Direction{r, -r, r};
    }
    return m == MotorId::StepperX ? Direction{1.0f, 0.0f, 1.0f} : Direction{0.0f, 1.0f, 1.0f};
}

std::array<float, 2> centre_of(const SweepPlanInputs& in) {
    if (in.probe_point) {
        return {(*in.probe_point)[0], (*in.probe_point)[1]};
    }
    return {(in.x_min + in.x_max) / 2.0f, (in.y_min + in.y_max) / 2.0f};
}

/// Longest segment through the centre along the direction, inset from the limits.
float usable_length(const SweepPlanInputs& in, const Direction& d) {
    const auto c = centre_of(in);
    float half = INFINITY;
    auto limit = [&half](float centre, float lo, float hi, float u) {
        if (std::fabs(u) < 1e-6f) {
            return;
        }
        const float room = std::min(centre - (lo + kEdgeInsetMm), (hi - kEdgeInsetMm) - centre);
        half = std::min(half, std::max(0.0f, room) / std::fabs(u));
    };
    limit(c[0], in.x_min, in.x_max, d.ux);
    limit(c[1], in.y_min, in.y_max, d.uy);
    return std::isfinite(half) ? 2.0f * half : 0.0f;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

float test_accel(const SweepPlanInputs& in) {
    return std::min(kTestAccelMmS2, in.max_accel_mm_s2);
}

} // namespace

std::vector<float> sweep_speeds(float max_velocity_mm_s) {
    const float top = std::min(kSweepMaxMmS, kSweepVelocityFraction * max_velocity_mm_s);
    std::vector<float> out;
    if (top < kSweepMinMmS) {
        return out;
    }
    float s = kSweepMinMmS;
    while (s * kSweepStepRatio < top) {
        out.push_back(std::round(s * 10.0f) / 10.0f);
        s *= kSweepStepRatio;
    }
    out.push_back(std::round(s * 10.0f) / 10.0f);
    if (out.back() < top) {
        out.push_back(std::round(top * 10.0f) / 10.0f);
    }
    return out;
}

std::vector<MotorId> motors_to_measure(const SweepPlanInputs& in) {
    if (in.kinematics == Kinematics::Unsupported) {
        return {};
    }
    if (!in.y_motor_measurable) {
        return {MotorId::StepperX};
    }
    return {MotorId::StepperX, MotorId::StepperY};
}

StepPlan plan_step(const SweepPlanInputs& in, MotorId motor, float speed_mm_s) {
    StepPlan plan;
    if (in.kinematics == Kinematics::Unsupported) {
        plan.refusal = SweepRefusal::UnsupportedKinematics;
        return plan;
    }
    if (!in.has_xy_limits) {
        plan.refusal = SweepRefusal::NoAxisLimits;
        return plan;
    }
    if (!(in.max_velocity_mm_s > 0.0f) || !(speed_mm_s > 0.0f)) {
        plan.refusal = SweepRefusal::NoVelocity;
        return plan;
    }
    const Direction d = direction_for(in.kinematics, motor);
    const float max_pass = kPassDiagonalFraction * usable_length(in, d);
    if (max_pass < kMinPassMm) {
        plan.refusal = SweepRefusal::SpanTooSmall;
        return plan;
    }

    const float v = speed_mm_s * d.toolhead_per_belt;
    const float pass = std::clamp(v * 1.0f, kMinPassMm, max_pass);
    int passes = std::max(2, static_cast<int>(std::ceil(kCruiseSeconds / (pass / v))));
    passes += passes % 2;

    const auto c = centre_of(in);
    const float gcx = c[0] - in.origin_x;
    const float gcy = c[1] - in.origin_y;
    const float hx = pass / 2.0f * d.ux;
    const float hy = pass / 2.0f * d.uy;
    const float approach_feed = std::min(kApproachMaxMmS, in.max_velocity_mm_s) * 60.0f;
    const float feed = v * 60.0f;

    std::vector<std::string> lines;
    lines.push_back(fmt::format("G1 X{:.2f} Y{:.2f} F{:.0f}", gcx - hx, gcy - hy, approach_feed));
    for (int i = 0; i < passes; ++i) {
        const float sign = (i % 2 == 0) ? 1.0f : -1.0f;
        lines.push_back(
            fmt::format("G1 X{:.2f} Y{:.2f} F{:.0f}", gcx + sign * hx, gcy + sign * hy, feed));
    }
    lines.emplace_back("M400");
    lines.push_back(fmt::format("G4 P{}", kSettleDwellMs));

    plan.gcode = join_lines(lines);
    plan.passes = passes;
    plan.pass_mm = pass;
    plan.seconds = static_cast<float>(passes) * pass / v;
    const double timeout = std::ceil(static_cast<double>(kStepTimeoutFactor) *
                                     (plan.seconds + kApproachAllowanceS) * 1000.0);
    plan.timeout_ms = std::max(kMinStepTimeoutMs, static_cast<uint32_t>(timeout));
    return plan;
}

std::string begin_gcode(const SweepPlanInputs& in) {
    float z = std::max(kClearanceZMm, in.current_z);
    if (in.max_z > 0.0f) {
        z = std::min(z, in.max_z);
    }
    const float accel = test_accel(in);
    std::vector<std::string> lines;
    lines.push_back(fmt::format("SAVE_GCODE_STATE NAME={}", kSweepStateName));
    lines.emplace_back("G90");
    lines.push_back(fmt::format("G1 Z{:.1f} F600", z - in.origin_z));
    if (in.minimum_cruise_ratio) {
        lines.push_back(fmt::format("SET_VELOCITY_LIMIT ACCEL={:g} MINIMUM_CRUISE_RATIO=0", accel));
    } else {
        lines.push_back(
            fmt::format("SET_VELOCITY_LIMIT ACCEL={:g} ACCEL_TO_DECEL={:g}", accel, accel));
    }
    return join_lines(lines);
}

std::string restore_gcode(const SweepPlanInputs& in) {
    std::vector<std::string> lines;
    if (in.minimum_cruise_ratio) {
        lines.push_back(fmt::format("SET_VELOCITY_LIMIT ACCEL={:g} MINIMUM_CRUISE_RATIO={:g}",
                                    in.max_accel_mm_s2, *in.minimum_cruise_ratio));
    } else {
        lines.push_back(fmt::format("SET_VELOCITY_LIMIT ACCEL={:g} ACCEL_TO_DECEL={:g}",
                                    in.max_accel_mm_s2, in.max_accel_to_decel));
    }
    lines.push_back(fmt::format("RESTORE_GCODE_STATE NAME={}", kSweepStateName));
    return join_lines(lines);
}

} // namespace helix::calibration::ripple
