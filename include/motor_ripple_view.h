// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the Vertical Artifact Check screen says: pure text and decisions over a
// sweep, so the panel only binds them.

#include "motor_ripple_types.h"

#include <string>
#include <vector>

namespace helix::calibration::ripple {

[[nodiscard]] Kinematics ripple_kinematics(const std::string& kinematics);
/// The check needs an accelerometer, a kinematics it can drive one motor at a
/// time on, and the steppers' geometry.
[[nodiscard]] bool ripple_check_supported(const std::string& kinematics, bool has_accelerometer,
                                          bool has_geometry);

/// "Y motor: 86 mm/s (23 of 74)", with "compensation on" in compare mode.
[[nodiscard]] std::string ripple_progress_text(MotorId motor, float speed, size_t index,
                                               size_t total, bool compensated);
/// One line per band, merged across motors, or a line saying none shook.
[[nodiscard]] std::string ripple_band_summary(const std::vector<RippleBand>& bands);
/// Each ripple's worst speed with compensation off against on, with the reduction.
[[nodiscard]] std::string ripple_reduction_summary(const SweepResult& result, bool factory_values);
/// Values of a sweep (`xs` ascending) at n evenly spaced points from lo to
/// hi, linearly between measured neighbours; NaN outside the sweep or next to
/// an unmeasured value. The chart spaces its points evenly.
[[nodiscard]] std::vector<float> ripple_resample_uniform(const std::vector<float>& xs,
                                                         const std::vector<float>& ys, float lo,
                                                         float hi, size_t n);
/// Whole minutes a run takes, rounded up; about 3.5 s per speed per pass.
[[nodiscard]] int ripple_estimate_minutes(size_t motors, size_t speeds, bool compare);

} // namespace helix::calibration::ripple
