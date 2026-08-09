// SPDX-License-Identifier: GPL-3.0-or-later

#include "pitch_estimator.h"

#include <spdlog/spdlog.h>

namespace helix::calibration {

float expected_frequency_for_span(float span_mm) {
    if (span_mm <= 0.0f) {
        return 0.0f;
    }
    return REFERENCE_FREQUENCY_HZ * REFERENCE_SPAN_MM / span_mm;
}

bool search_window_for_span(float span_mm, float* lo_hz, float* hi_hz) {
    if (lo_hz == nullptr || hi_hz == nullptr) {
        return false;
    }
    const float expected = expected_frequency_for_span(span_mm);
    if (expected <= 0.0f) {
        return false;
    }
    *lo_hz = expected * SEARCH_WINDOW_LO_FRACTION;
    *hi_hz = expected * SEARCH_WINDOW_HI_FRACTION;
    return true;
}

float required_bandwidth_hz(float search_hi_hz, int n_harmonics) {
    if (search_hi_hz <= 0.0f || n_harmonics < 1) {
        return 0.0f;
    }
    return search_hi_hz * static_cast<float>(n_harmonics) * 1.05f;
}

PitchEstimate estimate_pitch(const std::vector<std::pair<float, float>>& psd, float search_lo_hz,
                             float search_hi_hz, int n_harmonics) {
    PitchEstimate out;
    if (psd.size() < 4 || search_hi_hz <= search_lo_hz || n_harmonics < 1) {
        return out;
    }

    const size_t bins = psd.size();
    // compute_psd() emits bin i at frequency (i+1)*resolution - there is no DC
    // bin - so harmonic h of candidate index i lives at index h*(i+1)-1.
    double best_product = -1.0;
    for (size_t i = 0; i < bins; ++i) {
        const float freq = psd[i].first;
        if (freq < search_lo_hz || freq > search_hi_hz) {
            continue;
        }

        double product = 1.0; // double: four multiplied PSD bins underflow float
        bool series_complete = true;
        for (int h = 1; h <= n_harmonics; ++h) {
            const size_t harmonic_index = static_cast<size_t>(h) * (i + 1) - 1;
            if (harmonic_index >= bins) {
                series_complete = false;
                break;
            }
            product *= static_cast<double>(psd[harmonic_index].second);
        }
        if (!series_complete) {
            continue;
        }

        // product == 0.0 means every harmonic bin in this candidate's series
        // was zero - an all-zero (or silent) PSD must not produce a confident
        // "valid" estimate just because 0.0 beats the -1.0 sentinel.
        if (product > 0.0 && product > best_product) {
            best_product = product;
            out.frequency_hz = freq;
            out.valid = true;
        }
    }

    if (out.valid) {
        spdlog::debug("[PitchEstimator] f0={:.1f} Hz (window {:.1f}-{:.1f}, {} harmonics)",
                      out.frequency_hz, search_lo_hz, search_hi_hz, n_harmonics);
    } else {
        spdlog::warn("[PitchEstimator] no candidate had a complete harmonic series (PSD top "
                     "freq {:.1f} Hz, window {:.1f}-{:.1f} Hz needs {:.1f} Hz of bandwidth for "
                     "{} harmonics)",
                     psd.back().first, search_lo_hz, search_hi_hz,
                     required_bandwidth_hz(search_hi_hz, n_harmonics), n_harmonics);
    }
    return out;
}

PitchEstimate estimate_pitch_for_span(const std::vector<AccelSample>& samples, float sample_rate,
                                      float span_mm, int n_harmonics) {
    PitchEstimate out;
    if (span_mm <= 0.0f || sample_rate <= 0.0f) {
        return out;
    }

    float lo_hz = 0.0f, hi_hz = 0.0f;
    if (!search_window_for_span(span_mm, &lo_hz, &hi_hz)) {
        return out;
    }

    const float bandwidth = required_bandwidth_hz(hi_hz, n_harmonics);
    auto psd = compute_psd(samples, sample_rate, bandwidth);
    return estimate_pitch(psd, lo_hz, hi_hz, n_harmonics);
}

} // namespace helix::calibration
