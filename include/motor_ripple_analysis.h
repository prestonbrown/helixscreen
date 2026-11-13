// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Motor ripple analysis: accelerometer captures in, ripple amplitudes and
// bands of speeds to avoid out. Pure; no LVGL, no I/O.

#include "belt_tension_types.h"
#include "motor_ripple_types.h"

#include <vector>

namespace helix::calibration::ripple {

/// The highest frequency a capture at this sample rate can measure.
[[nodiscard]] float ceiling_hz(float sample_rate_hz);

/// Each ripple's amplitude in one capture at one belt speed. Short Hann frames
/// give the amplitude at k x speed / electrical_mm per axis, combined in
/// quadrature; only frames with real motion count, and the median over them
/// stands for the cruise. A ripple above the ceiling is nullopt.
[[nodiscard]] SpeedResult analyze_capture(const std::vector<AccelSample>& samples,
                                          float sample_rate_hz, float speed_mm_s,
                                          float electrical_mm);

} // namespace helix::calibration::ripple
