// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_live_data.h"

#include <algorithm>
#include <cmath>

namespace helix::calibration {

namespace {

/// Bucket boundary for reducing `n` inputs to `out_n` output points. Computed
/// in size_t rather than by an incrementing float step so the last bucket
/// always ends exactly at n and no input is dropped or read twice.
inline size_t bucket_edge(size_t index, size_t n, size_t out_n) {
    return index * n / out_n;
}

} // namespace

float belt_match_percent(float reference_hz, float measured_hz) {
    if (reference_hz <= 0.0f) {
        return 0.0f;
    }
    const float pct = 100.0f * (1.0f - std::fabs(measured_hz - reference_hz) / reference_hz);
    return std::clamp(pct, 0.0f, 100.0f);
}

bool belt_frequencies_match(float a_hz, float b_hz) {
    return std::fabs(a_hz - b_hz) < BELT_RESOLUTION_HZ;
}

bool belt_should_show_idle_hint(uint32_t ms_since_last_event) {
    return ms_since_last_event >= IDLE_HINT_MS;
}

BeltLiveData& BeltLiveData::instance() {
    static BeltLiveData s_instance;
    return s_instance;
}

void BeltLiveData::set_waveform(const std::vector<AccelSample>& window) {
    if (window.empty()) {
        waveform_.clear();
        return;
    }

    // Per-axis DC removal, same definition of "signal" as PluckDetector.
    double mean_x = 0.0, mean_y = 0.0, mean_z = 0.0;
    for (const AccelSample& s : window) {
        mean_x += s.x;
        mean_y += s.y;
        mean_z += s.z;
    }
    const double n = static_cast<double>(window.size());
    mean_x /= n;
    mean_y /= n;
    mean_z /= n;

    const size_t out_n = std::min(window.size(), TRACE_POINTS);
    waveform_.assign(out_n, 0.0f);

    for (size_t i = 0; i < out_n; ++i) {
        const size_t begin = bucket_edge(i, window.size(), out_n);
        const size_t end = bucket_edge(i + 1, window.size(), out_n);
        float peak = 0.0f;
        for (size_t j = begin; j < end; ++j) {
            const double dx = window[j].x - mean_x;
            const double dy = window[j].y - mean_y;
            const double dz = window[j].z - mean_z;
            peak = std::max(peak, static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz)));
        }
        waveform_[i] = peak;
    }
}

void BeltLiveData::set_spectrum(const std::vector<std::pair<float, float>>& psd) {
    if (psd.empty()) {
        spectrum_.clear();
        return;
    }

    const size_t out_n = std::min(psd.size(), TRACE_POINTS);
    spectrum_.assign(out_n, 0.0f);

    for (size_t i = 0; i < out_n; ++i) {
        const size_t begin = bucket_edge(i, psd.size(), out_n);
        const size_t end = bucket_edge(i + 1, psd.size(), out_n);
        float peak = psd[begin].second;
        for (size_t j = begin + 1; j < end; ++j) {
            peak = std::max(peak, psd[j].second);
        }
        spectrum_[i] = peak;
    }
}

void BeltLiveData::clear() {
    waveform_.clear();
    spectrum_.clear();
}

} // namespace helix::calibration
