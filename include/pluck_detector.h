// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <cstddef>
#include <vector>

/**
 * @file pluck_detector.h
 * @brief Onset detection and strength gating for belt plucks
 *
 * Thresholds here are measured, not chosen. Across 60 real captures on a
 * Voron 2.4: below 5x the noise floor nothing was a pluck at all, between
 * 5-9x pitch estimation was right 64% of the time, and above 9x it was right
 * 95% of the time. Rejecting weak strikes contributes more accuracy than
 * averaging does - ungated, a median never exceeded 48% at any sample count.
 */

namespace helix::calibration {

/// A ring-down segment ready for spectral analysis.
///
/// Deliberately has no strength field: extract_ringdown() is static and has
/// no access to the detection window it was pulled from, so it cannot
/// compute one. The detection-window ratio (see PluckDetector::rms_ratio())
/// belongs to whichever caller owns the live buffer.
struct PluckWindow {
    std::vector<AccelSample> samples;
};

class PluckDetector {
  public:
    /// Minimum strength, as a multiple of the noise floor, for a strike to count.
    static constexpr float MIN_RMS_RATIO = 9.0f;
    /// Skip past the impact transient before analysing.
    static constexpr float SKIP_MS = 40.0f;
    /// Length of ring-down to analyse.
    static constexpr float ANALYZE_MS = 500.0f;

    /// Broadband RMS with per-axis DC removed. Static so callers can measure a
    /// buffer without owning a detector.
    static float window_rms(const AccelSample* samples, size_t count);

    void set_noise_floor(float rms) {
        noise_floor_ = rms;
    }
    [[nodiscard]] float noise_floor() const {
        return noise_floor_;
    }

    /// Measure the floor from a buffer captured while the machine is still.
    bool learn_noise_floor(const std::vector<AccelSample>& quiet);

    /// Strength of a window as a multiple of the noise floor. 0 if no floor set.
    ///
    /// Operates on the live detection window (the buffer being watched for a
    /// strike, onset included) - not on an extracted ring-down. A ring-down
    /// has already decayed for SKIP_MS+ before the caller ever sees it, so its
    /// RMS reads several times lower than the detection window that triggered
    /// it. Feeding extract_ringdown() output back into this rejects strikes
    /// that were genuinely firm.
    [[nodiscard]] float rms_ratio(const AccelSample* samples, size_t count) const;

    /// True if this window is strong enough to analyse. Same detection-window
    /// contract as rms_ratio() - see its comment. MIN_RMS_RATIO was calibrated
    /// against detection windows across 60 captures; it is not meaningful
    /// against a ring-down.
    [[nodiscard]] bool passes_gate(const AccelSample* samples, size_t count) const;

    /// Locate the strongest transient in `buffer` and extract the ring-down
    /// beginning SKIP_MS after it.
    static bool extract_ringdown(const std::vector<AccelSample>& buffer, float sample_rate,
                                 PluckWindow* out);

  private:
    float noise_floor_ = 0.0f;
};

} // namespace helix::calibration
