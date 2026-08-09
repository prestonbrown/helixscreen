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

    // Onset is the largest instantaneous deviation from the buffer mean.
    double mean_x = 0.0, mean_y = 0.0, mean_z = 0.0;
    for (const auto& s : buffer) {
        mean_x += s.x;
        mean_y += s.y;
        mean_z += s.z;
    }
    const double n = static_cast<double>(buffer.size());
    mean_x /= n;
    mean_y /= n;
    mean_z /= n;

    size_t onset = 0;
    double peak = -1.0;
    for (size_t i = 0; i < buffer.size(); ++i) {
        const double dx = buffer[i].x - mean_x;
        const double dy = buffer[i].y - mean_y;
        const double dz = buffer[i].z - mean_z;
        const double mag = dx * dx + dy * dy + dz * dz;
        if (mag > peak) {
            peak = mag;
            onset = i;
        }
    }

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
    out->rms_ratio = 0.0f;
    return true;
}

} // namespace helix::calibration
