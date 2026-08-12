// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_listen_session.h"

#include "pitch_estimator.h"

#include <algorithm>

namespace helix::calibration {

namespace {
/// Boundary of the measured 5-to-9x band where pitch estimation is unreliable
/// (64% right) but a strike clearly occurred. Below this a batch is silently
/// dropped; between this and MIN_RMS_RATIO it is reported as a rejected event
/// so the UI can say "pluck harder" instead of staying silent.
constexpr float MIN_DETECTABLE_RATIO = PluckDetector::MIN_RMS_RATIO / 3.0f;
} // namespace

BeltListenSession::BeltListenSession(float span_mm, float sample_rate_hz)
    : span_mm_(span_mm), sample_rate_hz_(sample_rate_hz) {}

bool BeltListenSession::learn_noise_floor(const std::vector<AccelSample>& quiet) {
    return detector_.learn_noise_floor(quiet);
}

void BeltListenSession::set_noise_floor(float rms) {
    detector_.set_noise_floor(rms);
}

float BeltListenSession::noise_floor() const {
    return detector_.noise_floor();
}

size_t BeltListenSession::accepted_count() const {
    return aggregator_.count();
}

size_t BeltListenSession::rejected_count() const {
    return rejected_;
}

float BeltListenSession::median_hz() const {
    return aggregator_.median();
}

bool BeltListenSession::committed() const {
    return aggregator_.committed();
}

std::optional<PluckEvent> BeltListenSession::push(const AccelBatch& batch) {
    window_.insert(window_.end(), batch.samples.begin(), batch.samples.end());
    if (window_.size() > DETECTION_WINDOW_SAMPLES) {
        window_.erase(window_.begin(),
                      window_.begin() +
                          static_cast<long>(window_.size() - DETECTION_WINDOW_SAMPLES));
    }

    if (cooldown_samples_ > 0) {
        cooldown_samples_ -= std::min(cooldown_samples_, batch.samples.size());
        return std::nullopt;
    }

    if (!batch.contiguous()) {
        return std::nullopt;
    }

    if (window_.size() < DETECTION_WINDOW_SAMPLES || noise_floor() <= 0.0f) {
        return std::nullopt;
    }

    const float ratio = detector_.rms_ratio(window_.data(), window_.size());

    if (!detector_.passes_gate(window_.data(), window_.size())) {
        if (ratio < MIN_DETECTABLE_RATIO) {
            return std::nullopt;
        }
        // A strike happened but was too soft to trust a pitch estimate from.
        rejected_++;
        cooldown_samples_ = static_cast<size_t>(
            (PluckDetector::SKIP_MS + PluckDetector::ANALYZE_MS) / 1000.0f * sample_rate_hz_);
        return PluckEvent{0.0f, ratio, false};
    }

    PluckWindow ringdown;
    cooldown_samples_ = static_cast<size_t>((PluckDetector::SKIP_MS + PluckDetector::ANALYZE_MS) /
                                            1000.0f * sample_rate_hz_);

    if (!PluckDetector::extract_ringdown(window_, sample_rate_hz_, &ringdown)) {
        rejected_++;
        return PluckEvent{0.0f, ratio, false};
    }

    const PitchEstimate est = estimate_pitch_for_span(ringdown.samples, sample_rate_hz_, span_mm_);
    if (!est.valid) {
        rejected_++;
        return PluckEvent{0.0f, ratio, false};
    }

    aggregator_.add(est.frequency_hz);
    return PluckEvent{est.frequency_hz, ratio, true};
}

void BeltListenSession::reset() {
    window_.clear();
    aggregator_.reset();
    rejected_ = 0;
    cooldown_samples_ = 0;
}

} // namespace helix::calibration
