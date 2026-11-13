// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Motion for the motor ripple sweep: one G-code script per motor and speed.
//
// On CoreXY a diagonal move turns one motor: stepper_x is A = x + y, so a move
// along (1, 1) turns only A, and stepper_y is B = x - y, turned alone along
// (1, -1). For belt speed s the toolhead feedrate is s / sqrt(2). On Cartesian
// X moves turn the X motor and Y moves the Y motor. Speeds are motor belt
// speeds, the same number as an axis-aligned print speed.

#include "motor_ripple_types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix::calibration::ripple {

inline constexpr const char* kSweepStateName = "helix_ripple_sweep"; ///< SAVE_GCODE_STATE NAME=

struct SweepPlanInputs {
    Kinematics kinematics = Kinematics::Unsupported;
    /// toolhead axis limits, machine coordinates
    float x_min = 0.0f;
    float x_max = 0.0f;
    float y_min = 0.0f;
    float y_max = 0.0f;
    bool has_xy_limits = false;
    /// [resonance_tester] probe point; the centre of the limits when absent
    std::optional<std::array<float, 3>> probe_point;
    /// gcode_move.homing_origin: a G-code coordinate is machine minus origin
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float origin_z = 0.0f;
    float max_velocity_mm_s = 0.0f;
    float max_accel_mm_s2 = 0.0f;
    float current_z = 0.0f;
    float max_z = 0.0f;
    /// nullopt: the firmware has max_accel_to_decel instead
    std::optional<float> minimum_cruise_ratio;
    float max_accel_to_decel = 0.0f;
    /// false on a Cartesian without an accelerometer riding the Y axis
    bool y_motor_measurable = true;
};

enum class SweepRefusal { None, UnsupportedKinematics, NoAxisLimits, NoVelocity, SpanTooSmall };

struct StepPlan {
    SweepRefusal refusal = SweepRefusal::None;
    /// approach, passes, M400, G4 P250; empty when refused
    std::string gcode;
    int passes = 0;          ///< even, >= 2
    float pass_mm = 0.0f;    ///< toolhead travel per pass
    float seconds = 0.0f;    ///< cruise time over all passes
    uint32_t timeout_ms = 0; ///< max(30000, 4 x the whole script's move time)
};

/// From kSweepMinMmS, each kSweepStepRatio above the last, up to
/// min(kSweepMaxMmS, kSweepVelocityFraction x max velocity). Empty when that
/// ceiling is below the start.
[[nodiscard]] std::vector<float> sweep_speeds(float max_velocity_mm_s);
[[nodiscard]] std::vector<MotorId> motors_to_measure(const SweepPlanInputs& in);
[[nodiscard]] StepPlan plan_step(const SweepPlanInputs& in, MotorId motor, float speed_mm_s);
/// SAVE_GCODE_STATE, G90, Z to the clearance height, the test velocity limits.
[[nodiscard]] std::string begin_gcode(const SweepPlanInputs& in);
/// The printer's own velocity limits back, then RESTORE_GCODE_STATE.
[[nodiscard]] std::string restore_gcode(const SweepPlanInputs& in);

} // namespace helix::calibration::ripple
