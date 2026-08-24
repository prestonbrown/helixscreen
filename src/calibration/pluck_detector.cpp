// SPDX-License-Identifier: GPL-3.0-or-later

#include "pluck_detector.h"

#include <spdlog/spdlog.h>

#include <cmath>

namespace helix::calibration {

float PluckDetector::window_rms(const AccelSample* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0f;
    }

    double mean_x = 0.0, mean_y = 0.0, mean_z = 0.0;
    for (size_t i = 0; i < count; ++i) {
        mean_x += samples[i].x;
        mean_y += samples[i].y;
        mean_z += samples[i].z;
    }
    const double n = static_cast<double>(count);
    mean_x /= n;
    mean_y /= n;
    mean_z /= n;

    double accum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double dx = samples[i].x - mean_x;
        const double dy = samples[i].y - mean_y;
        const double dz = samples[i].z - mean_z;
        accum += dx * dx + dy * dy + dz * dz;
    }
    return static_cast<float>(std::sqrt(accum / n));
}

bool PluckDetector::learn_noise_floor(const std::vector<AccelSample>& quiet) {
    if (quiet.empty()) {
        return false;
    }
    noise_floor_ = window_rms(quiet.data(), quiet.size());
    spdlog::debug("[PluckDetector] noise floor {:.2f}, gate at {:.2f}", noise_floor_,
                  noise_floor_ * MIN_RMS_RATIO);
    return noise_floor_ > 0.0f;
}

float PluckDetector::rms_ratio(const AccelSample* samples, size_t count) const {
    if (noise_floor_ <= 0.0f) {
        return 0.0f;
    }
    return window_rms(samples, count) / noise_floor_;
}

bool PluckDetector::passes_gate(const AccelSample* samples, size_t count) const {
    return rms_ratio(samples, count) >= MIN_RMS_RATIO;
}

size_t PluckDetector::find_onset(const AccelSample* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0;
    }

    double mean_x = 0.0, mean_y = 0.0, mean_z = 0.0;
    for (size_t i = 0; i < count; ++i) {
        mean_x += samples[i].x;
        mean_y += samples[i].y;
        mean_z += samples[i].z;
    }
    const double n = static_cast<double>(count);
    mean_x /= n;
    mean_y /= n;
    mean_z /= n;

    size_t onset = 0;
    double peak = -1.0;
    for (size_t i = 0; i < count; ++i) {
        const double dx = samples[i].x - mean_x;
        const double dy = samples[i].y - mean_y;
        const double dz = samples[i].z - mean_z;
        const double mag = dx * dx + dy * dy + dz * dz;
        if (mag > peak) {
            peak = mag;
            onset = i;
        }
    }
    return onset;
}

bool PluckDetector::has_sharp_onset(const AccelSample* samples, size_t count, float sample_rate) {
    if (samples == nullptr || sample_rate <= 0.0f) {
        return false;
    }

    const size_t segment = static_cast<size_t>(sample_rate * ENVELOPE_SEGMENT_MS / 1000.0f);
    const size_t lookback = static_cast<size_t>(ONSET_LOOKBACK_MS / ENVELOPE_SEGMENT_MS);
    if (segment == 0 || lookback == 0) {
        return false;
    }
    const size_t segments = count / segment;
    if (segments <= lookback) {
        return false;
    }

    size_t peak = 0;
    float peak_rms = -1.0f;
    for (size_t i = 0; i < segments; ++i) {
        const float rms = window_rms(samples + i * segment, segment);
        if (rms > peak_rms) {
            peak_rms = rms;
            peak = i;
        }
    }

    // The evidence is the quiet that came before the strike. A window whose
    // loudest moment sits at its very start has none - and so does a steady
    // tone, whose peak lands wherever noise happens to put it.
    if (peak < lookback) {
        return false;
    }

    const float before = window_rms(samples + (peak - lookback) * segment, segment);
    if (before <= 0.0f) {
        return peak_rms > 0.0f;
    }
    return peak_rms / before >= MIN_ONSET_RISE;
}

bool PluckDetector::ringdown_ready(const AccelSample* samples, size_t count, float sample_rate) {
    if (samples == nullptr || sample_rate <= 0.0f || count == 0) {
        return false;
    }
    const size_t onset = find_onset(samples, count);
    const size_t post = count - onset;
    const size_t min_post = static_cast<size_t>(sample_rate * MIN_RINGDOWN_MS / 1000.0f);
    return post >= min_post && post >= static_cast<size_t>(DECAY_SEGMENTS);
}

bool PluckDetector::has_pluck_decay(const AccelSample* samples, size_t count, float sample_rate) {
    if (samples == nullptr || sample_rate <= 0.0f) {
        return false;
    }

    const size_t onset = find_onset(samples, count);
    if (onset >= count) {
        return false;
    }
    const size_t post = count - onset;
    // An event whose onset lands at the very end of the window has no envelope
    // to read. As a predicate, "cannot tell" has to mean no - that is exactly
    // what something still ramping up looks like. A caller that can wait for
    // more samples should ask ringdown_ready() first, which wants a good deal
    // more than this floor.
    const size_t min_post = static_cast<size_t>(sample_rate * MIN_DECAY_SPAN_MS / 1000.0f);
    if (post < min_post || post < static_cast<size_t>(DECAY_SEGMENTS)) {
        return false;
    }

    const size_t segment = post / static_cast<size_t>(DECAY_SEGMENTS);
    float first = 0.0f;
    float previous = 0.0f;
    float last = 0.0f;
    for (int i = 0; i < DECAY_SEGMENTS; ++i) {
        const float rms = window_rms(samples + onset + static_cast<size_t>(i) * segment, segment);
        if (i == 0) {
            first = rms;
        } else if (previous <= 0.0f || rms > previous * MAX_DECAY_RISE) {
            return false;
        }
        previous = rms;
        last = rms;
    }

    if (first <= 0.0f) {
        return false;
    }
    // A steady tone passes the per-segment rise tolerance trivially - every
    // segment is the same. What it cannot do is end quieter than it started.
    return last <= first * MAX_DECAY_END_RATIO;
}

bool PluckDetector::extract_ringdown(const std::vector<AccelSample>& buffer, float sample_rate,
                                     PluckWindow* out) {
    if (out == nullptr || sample_rate <= 0.0f || buffer.size() < 256) {
        return false;
    }

    const size_t analyze_len = static_cast<size_t>(sample_rate * ANALYZE_MS / 1000.0f);
    const size_t skip_len = static_cast<size_t>(sample_rate * SKIP_MS / 1000.0f);
    if (analyze_len == 0 || buffer.size() < analyze_len) {
        return false;
    }

    const size_t onset = find_onset(buffer.data(), buffer.size());

    size_t begin = onset + skip_len;
    if (begin + analyze_len > buffer.size()) {
        // Not enough ring-down after the onset; take the tail instead of
        // silently analysing pre-pluck silence.
        if (buffer.size() < analyze_len) {
            return false;
        }
        begin = buffer.size() - analyze_len;
    }

    out->samples.assign(buffer.begin() + static_cast<std::ptrdiff_t>(begin),
                        buffer.begin() + static_cast<std::ptrdiff_t>(begin + analyze_len));
    return true;
}

} // namespace helix::calibration
