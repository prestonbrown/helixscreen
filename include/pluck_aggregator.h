// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <vector>

/**
 * @file pluck_aggregator.h
 * @brief Running median of per-pluck frequency estimates
 *
 * A single gated pluck is right about 95% of the time; a median of five is
 * effectively always right. The aggregator keeps collecting past the commit
 * threshold so a user can keep plucking and watch the value hold steady.
 */

namespace helix::calibration {

class PluckAggregator {
  public:
    /// Accepted plucks required before the median is trustworthy enough to show
    /// as a committed result.
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
