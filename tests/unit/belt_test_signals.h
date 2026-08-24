// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../../include/belt_tension_types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file belt_test_signals.h
 * @brief Synthetic accelerometer signal helpers shared by the belt tests
 *
 * The belt tests are threshold tests: they assert that a measured quantity
 * lands on one side of a constant. A fresh random draw each run would make
 * them flaky rather than thorough, so the noise here comes from a plain LCG
 * and every run analyses the same buffer.
 */

namespace helix::calibration::test {

/// Deterministic hiss in [-1, 1).
struct Hiss {
    uint32_t state = 2468u;

    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 23) - 1.0f;
    }
};

/**
 * @brief Broadband hiss on all three axes, gravity on X
 *
 * Gravity sits on X in all eight real captures - the toolhead mounts the
 * accelerometer with X vertical, not Z - so synthetic buffers meant to sit
 * beside them carry it on the same axis.
 *
 * The content is broadband on purpose. A bed of pure sinusoids has almost no
 * energy between its lines, which drives the median bin of its spectrum
 * towards zero and makes QuietSpectrum read every bin as wildly contaminated;
 * a few stray tones also land inside the 77-165 Hz search window. Neither
 * resembles a real accelerometer at rest.
 *
 * @param amplitude Peak deviation per axis. With uniform noise on three axes
 *        the combined broadband RMS that PluckDetector::window_rms() measures
 *        comes out equal to this amplitude, so a caller sizing a noise floor
 *        can use it directly.
 */
inline std::vector<AccelSample> hiss_bed(size_t count, float amplitude, float rate_hz, Hiss& rng,
                                         float t0 = 0.0f) {
    std::vector<AccelSample> out(count);
    for (size_t i = 0; i < count; ++i) {
        out[i].time = t0 + static_cast<float>(i) / rate_hz;
        out[i].x = 9810.0f + amplitude * rng.next();
        out[i].y = amplitude * rng.next();
        out[i].z = amplitude * rng.next();
    }
    return out;
}

} // namespace helix::calibration::test
