// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <vector>

/**
 * @file pluck_aggregator.h
 * @brief Running median of per-pluck frequency estimates
 *
 * A single gated pluck is right about 95% of the time (measured, see
 * pluck_detector.h). The aggregator keeps collecting past the commit
 * threshold so a user can keep plucking and watch the value hold steady.
 */

namespace helix::calibration {

class PluckAggregator {
  public:
    /// Accepted plucks required before the median is trustworthy enough to show
    /// as a committed result.
    ///
    /// Measured, on the reference Voron 2.4, and reported in this feature's own
    /// design spec (docs/superpowers/specs/2026-08-09-live-belt-tuner-design.md,
    /// "Median accuracy, gated to firm plucks"): 1 pluck 82%, 3 plucks 91%,
    /// **5 plucks 97%**, 7 plucks 98%. Five is where the curve flattens - the
    /// step from 3 to 5 buys 6 points, the step from 5 to 7 buys 1, and each
    /// extra pluck is a real thing a user has to do while leaning over a
    /// gantry.
    ///
    /// @note Do NOT re-derive this from the per-pluck accuracy. Treating five
    /// plucks as independent Bernoulli draws at 95% gives a majority-correct
    /// probability of ~0.999, which is about 30x optimistic in error rate
    /// against the 97% actually measured. The draws are not independent: the
    /// failure mode this feature keeps hitting is correlated - a fan running,
    /// a stale span, a resonance in the room - and it biases every pluck in a
    /// session the same way. That is also why more plucks stop helping.
    static constexpr size_t COMMIT_AFTER = 5;

    /// Record an estimate. Non-positive values are ignored.
    void add(float frequency_hz);

    void reset();

    [[nodiscard]] size_t count() const {
        return samples_.size();
    }
    [[nodiscard]] bool committed() const {
        return samples_.size() >= COMMIT_AFTER;
    }

    /// Median of everything recorded, or 0 if nothing has been.
    [[nodiscard]] float median() const;

  private:
    std::vector<float> samples_;
};

} // namespace helix::calibration
