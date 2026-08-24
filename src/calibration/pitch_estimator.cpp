// SPDX-License-Identifier: GPL-3.0-or-later

#include "pitch_estimator.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

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

bool QuietSpectrum::learn(std::vector<std::pair<float, float>> psd) {
    clear();
    if (psd.size() < 4) {
        return false;
    }
    // compute_psd() emits bin i at (i+1)*resolution, so the first bin IS the
    // resolution.
    resolution_ = psd.front().first;
    if (resolution_ <= 0.0f) {
        return false;
    }

    // The median, not the mean: one loud tone is exactly what this window is
    // expected to contain, and it must not drag the reference up with it.
    std::vector<float> powers;
    powers.reserve(psd.size());
    for (const auto& bin : psd) {
        powers.push_back(bin.second);
    }
    auto mid = powers.begin() + static_cast<std::ptrdiff_t>(powers.size() / 2);
    std::nth_element(powers.begin(), mid, powers.end());
    reference_ = *mid;
    if (!(reference_ > 0.0f)) {
        // A silent or all-zero window has no scale to measure prominence
        // against. Stay invalid and discount nothing rather than declare every
        // frequency infinitely contaminated.
        resolution_ = 0.0f;
        return false;
    }

    psd_ = std::move(psd);
    spdlog::debug("[QuietSpectrum] {} bins at {:.2f} Hz, median power {:.4g}", psd_.size(),
                  resolution_, reference_);
    return true;
}

float QuietSpectrum::prominence_at(float freq_hz) const {
    if (!valid() || freq_hz <= 0.0f) {
        return 0.0f;
    }
    // The quiet window and the ring-down are different lengths, so a tone lands
    // on differently-numbered bins in each. Take the loudest bin within the
    // match tolerance rather than the one nearest arithmetic.
    const float tol = std::max(resolution_, BIN_MATCH_FRACTION * freq_hz);
    const auto span = static_cast<long>(std::ceil(tol / resolution_));
    const auto centre = static_cast<long>(std::lround(freq_hz / resolution_)) - 1;
    const auto bins = static_cast<long>(psd_.size());

    float peak = 0.0f;
    for (long i = centre - span; i <= centre + span; ++i) {
        if (i < 0 || i >= bins) {
            continue;
        }
        peak = std::max(peak, psd_[static_cast<size_t>(i)].second);
    }
    return peak / reference_;
}

float QuietSpectrum::weight_at(float freq_hz) const {
    if (!valid()) {
        return 1.0f;
    }
    const float prominence = prominence_at(freq_hz);
    if (prominence <= BACKGROUND_PROMINENCE_TOLERANCE) {
        return 1.0f;
    }
    return std::max(MIN_BACKGROUND_WEIGHT, BACKGROUND_PROMINENCE_TOLERANCE / prominence);
}

void QuietSpectrum::clear() {
    psd_.clear();
    reference_ = 0.0f;
    resolution_ = 0.0f;
}

QuietSpectrum quiet_spectrum_for_span(const std::vector<AccelSample>& quiet, float sample_rate,
                                      float span_mm, int n_harmonics) {
    QuietSpectrum out;
    if (quiet.size() < 4 || sample_rate <= 0.0f || span_mm <= 0.0f || n_harmonics < 1) {
        return out;
    }

    float lo_hz = 0.0f, hi_hz = 0.0f;
    if (!search_window_for_span(span_mm, &lo_hz, &hi_hz)) {
        return out;
    }

    // Same bandwidth the pluck path will request, so the two spectra cover the
    // same harmonics - see required_bandwidth_hz().
    out.learn(compute_psd(quiet, sample_rate, required_bandwidth_hz(hi_hz, n_harmonics)));
    return out;
}

float harmonic_concentration(const std::vector<std::pair<float, float>>& psd, float f0,
                             int n_harmonics, float band_lo_hz, const QuietSpectrum* background) {
    if (psd.size() < 4 || f0 <= 0.0f || n_harmonics < 1) {
        return 0.0f;
    }
    const float resolution = psd.front().first;
    if (resolution <= 0.0f) {
        return 0.0f;
    }

    const float band_hi_hz = (static_cast<float>(n_harmonics) + 0.5f) * f0;
    double total = 0.0;
    double on_harmonics = 0.0;

    for (const auto& [freq, power] : psd) {
        if (freq < band_lo_hz || freq > band_hi_hz) {
            continue;
        }
        // Energy that was already there before the pluck counts for neither
        // side of the ratio. Without this a loud fan owns the band and a real
        // pluck scores as unconcentrated as a thump.
        const double weighted =
            static_cast<double>(power) *
            (background != nullptr ? static_cast<double>(background->weight_at(freq)) : 1.0);
        total += weighted;

        for (int h = 1; h <= n_harmonics; ++h) {
            const float harmonic_hz = static_cast<float>(h) * f0;
            const float tol =
                std::max(HARMONIC_MATCH_BINS * resolution, BIN_MATCH_FRACTION * harmonic_hz);
            if (std::fabs(freq - harmonic_hz) <= tol) {
                on_harmonics += weighted;
                break;
            }
        }
    }

    return total > 0.0 ? static_cast<float>(on_harmonics / total) : 0.0f;
}

float required_bandwidth_hz(float search_hi_hz, int n_harmonics) {
    if (search_hi_hz <= 0.0f || n_harmonics < 1) {
        return 0.0f;
    }
    return search_hi_hz * static_cast<float>(n_harmonics) * 1.05f;
}

PitchEstimate estimate_pitch(const std::vector<std::pair<float, float>>& psd, float search_lo_hz,
                             float search_hi_hz, int n_harmonics, const QuietSpectrum* background) {
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
            double bin = static_cast<double>(psd[harmonic_index].second);
            if (background != nullptr) {
                // A fan tone contaminates 2f and 4f as readily as f, so the
                // discount applies to every harmonic a candidate is scored on.
                bin *= static_cast<double>(background->weight_at(psd[harmonic_index].first));
            }
            product *= bin;
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
                                      float span_mm, int n_harmonics,
                                      std::vector<std::pair<float, float>>* out_psd,
                                      const QuietSpectrum* background) {
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
    const PitchEstimate estimate = estimate_pitch(psd, lo_hz, hi_hz, n_harmonics, background);
    if (out_psd) {
        *out_psd = std::move(psd);
    }
    return estimate;
}

} // namespace helix::calibration
