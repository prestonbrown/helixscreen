// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_listen_session.h"

#include "pitch_estimator.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix::calibration {

namespace {
/// Boundary of the measured 5-to-9x band where pitch estimation is unreliable
/// (64% right) but a strike clearly occurred. Below this a batch is silently
/// dropped; between this and MIN_RMS_RATIO it is reported as a rejected event
/// so the UI can say "pluck harder" instead of staying silent.
constexpr float MIN_DETECTABLE_RATIO = PluckDetector::MIN_RMS_RATIO / 3.0f;
} // namespace

BeltListenSession::BeltListenSession(float span_mm, float sample_rate_hz)
    : span_mm_(span_mm), sample_rate_hz_(sample_rate_hz), capture_(belt_capture_dir(), span_mm) {}

bool BeltListenSession::learn_noise_floor(const std::vector<AccelSample>& quiet) {
    // The same buffer answers two questions: how loud is it in here, and what
    // is already ringing in here. The second is what a scalar floor cannot
    // say, and it is what tells a belt tone apart from a fan tone.
    quiet_spectrum_ = quiet_spectrum_for_span(quiet, sample_rate_hz_, span_mm_, DEFAULT_HARMONICS);
    capture_.write_quiet(quiet, sample_rate_hz_);
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
    if (ratio < MIN_DETECTABLE_RATIO) {
        return std::nullopt;
    }

    // Nothing is resolved until the ring-down has arrived - not an acceptance
    // and not a rejection. A strike crosses both thresholds on its leading
    // edge, while the window holds a slice of the attack and no envelope at
    // all. Answering there is wrong twice over: it tells someone who plucked
    // firmly that they were too soft, and the cooldown that goes with it then
    // swallows the real ring-down two batches behind. Measured across the
    // fixture set at every window phase, resolving early cost 46 of 204.
    if (!PluckDetector::ringdown_ready(window_.data(), window_.size(), sample_rate_hz_)) {
        return std::nullopt;
    }

    // Past this point the window is resolved one way or the other, so the
    // cooldown runs regardless of the outcome.
    cooldown_samples_ = static_cast<size_t>((PluckDetector::SKIP_MS + PluckDetector::ANALYZE_MS) /
                                            1000.0f * sample_rate_hz_);

    // Diagnostic numbers for BeltCaptureWriter, computed once so every branch
    // below can report them regardless of which check actually resolved the
    // event - a rejection is the case most worth diagnosing, and "which check
    // rejected it and by how much" needs these even when the verdict itself
    // never consulted them. Skipped entirely when capture is off: one pass
    // over the 2048-sample window is cheap, but it is not free, and this path
    // runs on every resolved strike whether or not anyone is capturing.
    std::optional<float> onset_rise_val;
    std::optional<float> decay_end_val;
    if (capture_.enabled()) {
        onset_rise_val = PluckDetector::onset_rise(window_.data(), window_.size(), sample_rate_hz_);
        const float decay =
            PluckDetector::decay_end_ratio(window_.data(), window_.size(), sample_rate_hz_);
        if (decay >= 0.0f) {
            decay_end_val = decay;
        }
    }

    // Writes the detection window (always) and the ring-down (once one has
    // been extracted) for this event. concentration/estimate_hz are only
    // known once the pipeline has actually reached pitch estimation, so
    // earlier rejections pass nullopt/0 rather than a number the code never
    // computed.
    const auto capture_event = [&](bool accepted, PluckReject reject,
                                   const std::vector<AccelSample>* ringdown,
                                   std::optional<float> concentration, float estimate_hz) {
        if (!capture_.enabled()) {
            return;
        }
        CaptureVerdict v;
        v.accepted = accepted;
        v.reject = reject;
        v.rms_ratio = ratio;
        v.onset_rise = onset_rise_val;
        v.decay_end_ratio = decay_end_val;
        v.harmonic_concentration = concentration;
        v.estimate_hz = estimate_hz;
        v.median_hz = aggregator_.median();
        capture_.write_event(window_, ringdown, sample_rate_hz_, v);
    };

    if (!detector_.passes_gate(window_.data(), window_.size())) {
        // A strike happened but was too soft to trust a pitch estimate from.
        rejected_++;
        capture_event(false, PluckReject::TOO_SOFT, nullptr, std::nullopt, 0.0f);
        return PluckEvent{0.0f, ratio, false, PluckReject::TOO_SOFT};
    }

    const auto not_a_pluck = [&](const std::vector<AccelSample>* ringdown,
                                 std::optional<float> concentration,
                                 float estimate_hz) -> std::optional<PluckEvent> {
        rejected_++;
        capture_event(false, PluckReject::NOT_A_PLUCK, ringdown, concentration, estimate_hz);
        return PluckEvent{0.0f, ratio, false, PluckReject::NOT_A_PLUCK};
    };

    // Energy said something happened. Shape says whether a string was involved.
    if (!PluckDetector::has_sharp_onset(window_.data(), window_.size(), sample_rate_hz_) ||
        !PluckDetector::has_pluck_decay(window_.data(), window_.size(), sample_rate_hz_)) {
        spdlog::debug("[BeltListen] {:.1f}x window rejected on envelope shape", ratio);
        return not_a_pluck(nullptr, std::nullopt, 0.0f);
    }

    PluckWindow ringdown;
    if (!PluckDetector::extract_ringdown(window_, sample_rate_hz_, &ringdown)) {
        return not_a_pluck(nullptr, std::nullopt, 0.0f);
    }

    std::vector<std::pair<float, float>> psd;
    const PitchEstimate est = estimate_pitch_for_span(ringdown.samples, sample_rate_hz_, span_mm_,
                                                      DEFAULT_HARMONICS, &psd, &quiet_spectrum_);
    if (!est.valid) {
        return not_a_pluck(&ringdown.samples, std::nullopt, 0.0f);
    }

    // A pluck puts its energy into a harmonic series; a thump spreads it. The
    // band starts at the search window's floor so the structural gantry mode,
    // which sits below it and is the loudest thing in the capture, does not
    // drown every real pluck.
    float band_lo = 0.0f, band_hi = 0.0f;
    if (!search_window_for_span(span_mm_, &band_lo, &band_hi)) {
        return not_a_pluck(&ringdown.samples, std::nullopt, est.frequency_hz);
    }
    const float concentration =
        harmonic_concentration(psd, est.frequency_hz, DEFAULT_HARMONICS, band_lo, &quiet_spectrum_);
    if (concentration < MIN_HARMONIC_CONCENTRATION) {
        spdlog::debug("[BeltListen] {:.1f}x window at {:.1f} Hz rejected: only {:.2f} of in-band "
                      "energy on its harmonics",
                      ratio, est.frequency_hz, concentration);
        return not_a_pluck(&ringdown.samples, concentration, est.frequency_hz);
    }

    last_spectrum_ = std::move(psd);
    aggregator_.add(est.frequency_hz);
    capture_event(true, PluckReject::NONE, &ringdown.samples, concentration, est.frequency_hz);
    return PluckEvent{est.frequency_hz, ratio, true, PluckReject::NONE};
}

void BeltListenSession::reset() {
    window_.clear();
    last_spectrum_.clear();
    aggregator_.reset();
    rejected_ = 0;
    cooldown_samples_ = 0;
}

} // namespace helix::calibration
