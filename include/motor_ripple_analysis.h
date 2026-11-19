// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Motor ripple analysis: accelerometer captures in, ripple amplitudes and
// bands of speeds to avoid out. Pure; no LVGL, no I/O.

#include "belt_tension_types.h"
#include "motor_ripple_types.h"

#include <optional>
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

/// Bands of speed to avoid on one motor. A speed is shaky when any ripple
/// there exceeds `factor` x that ripple's own median over the sweep and
/// `floor_mm_s2`; each shaky speed covers half the gap to its neighbours, and
/// touching ranges merge. A band is labelled with its highest ripple peak.
[[nodiscard]] std::vector<RippleBand> find_bands(const MotorSweep& sweep, float electrical_mm,
                                                 float factor = kBandFactor,
                                                 float floor_mm_s2 = kBandFloorMmS2);
/// Union over motors, sorted by lo; overlapping bands merge under the higher peak.
[[nodiscard]] std::vector<RippleBand> merge_bands(std::vector<RippleBand> bands);
[[nodiscard]] const RippleBand* band_containing(const std::vector<RippleBand>& bands,
                                                float speed_mm_s);
/// Nearest whole mm/s outside every band, below or above `speed_mm_s`; nullopt
/// once that falls outside the sweep range.
[[nodiscard]] std::optional<int> nearest_clear_speed(const std::vector<RippleBand>& bands,
                                                     float speed_mm_s, bool below);

} // namespace helix::calibration::ripple
