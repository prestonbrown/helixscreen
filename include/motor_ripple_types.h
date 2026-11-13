// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Motor ripple speed sweep: the shared vocabulary.
//
// A stepper's torque wobbles once, twice and four times per electrical cycle
// (the 1x, 2x and 4x ripple). At the print speed where one of those
// frequencies lands on a frame or toolhead resonance the toolhead shakes and
// walls show fine vertical lines. The sweep measures each ripple's amplitude
// at each speed; docs/devel/MOTOR_RIPPLE_CHECK.md has the method.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix::calibration::ripple {

inline constexpr float kSweepMinMmS = 20.0f;
inline constexpr float kSweepMaxMmS = 300.0f;
inline constexpr float kSweepStepRatio = 1.08f;
inline constexpr float kSweepVelocityFraction = 0.9f;
inline constexpr float kCruiseSeconds = 2.0f;
inline constexpr float kMinPassMm = 30.0f;
inline constexpr float kPassDiagonalFraction = 0.85f;
inline constexpr float kTestAccelMmS2 = 10000.0f;
inline constexpr float kClearanceZMm = 20.0f;
inline constexpr float kCeilingFraction = 0.45f;
inline constexpr float kBandFactor = 3.0f;
inline constexpr float kBandFloorMmS2 = 500.0f;
inline constexpr std::array<int, 3> kHarmonics = {1, 2, 4};

enum class Kinematics { CoreXY, Cartesian, Unsupported };
enum class MotorId { StepperX, StepperY };

struct MotorGeometry {
    float rotation_distance_mm = 0.0f;
    float gear_ratio = 1.0f; ///< product of the gear_ratio pairs; 1 when none
    int full_steps_per_rotation = 200;
};

struct SpeedResult {
    float speed_mm_s = 0.0f;
    /// mm/s^2, indexed like kHarmonics; nullopt above the measurable ceiling
    std::array<std::optional<float>, 3> ripple;
    float broadband = 0.0f; ///< relative only
    int frames_used = 0;
};

struct MotorSweep {
    MotorId motor = MotorId::StepperX;
    std::vector<SpeedResult> speeds; ///< ascending speed
};

struct RippleBand {
    MotorId motor = MotorId::StepperX;
    float lo_mm_s = 0.0f;
    float hi_mm_s = 0.0f;
    int harmonic = 1;
    float peak_speed_mm_s = 0.0f;
    float peak_mm_s2 = 0.0f;
    float freq_hz = 0.0f;
};

struct SkippedMotor {
    MotorId motor = MotorId::StepperY;
    std::string reason; ///< untranslated
};

struct SweepResult {
    float sample_rate_hz = 0.0f;
    float ceiling_hz = 0.0f;
    int64_t unix_time = 0;
    std::vector<MotorSweep> sweeps;             ///< compensation off, or the only run
    std::vector<MotorSweep> compensated_sweeps; ///< compensation on; empty unless compare mode
    std::vector<RippleBand> bands; ///< from compensated_sweeps when present, else sweeps
    std::vector<SkippedMotor> skipped;
};

/// Belt travel per electrical cycle: rotation_distance / gear * 4 / full_steps.
[[nodiscard]] float electrical_cycle_mm(const MotorGeometry& g);
[[nodiscard]] float ripple_freq_hz(float speed_mm_s, int harmonic, float electrical_mm);

} // namespace helix::calibration::ripple
