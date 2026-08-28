// SPDX-License-Identifier: GPL-3.0-or-later

#include "pluck_aggregator.h"

#include <algorithm>

namespace helix::calibration {

void PluckAggregator::add(float frequency_hz) {
    if (frequency_hz <= 0.0f) {
        return;
    }
    samples_.push_back(frequency_hz);
}

void PluckAggregator::reset() {
    samples_.clear();
}

float PluckAggregator::median() const {
    if (samples_.empty()) {
        return 0.0f;
    }
    std::vector<float> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 1) {
        return sorted[mid];
    }
    return 0.5f * (sorted[mid - 1] + sorted[mid]);
}

} // namespace helix::calibration
