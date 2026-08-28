// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file belt_tension_types.cpp
 * @brief Implementation of belt tension analysis functions
 *
 * Provides PSD computation via DFT, CSV parsing for Klipper accelerometer
 * data, Pearson correlation for belt path similarity, and status evaluation.
 */

#include "belt_tension_types.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace helix::calibration {

// ============================================================================
// belt_status_to_string()
// ============================================================================

const char* belt_status_to_string(BeltStatus status) {
    switch (status) {
    case BeltStatus::GOOD:
        return "Good";
    case BeltStatus::WARNING:
        return "Needs adjustment";
    case BeltStatus::BAD:
        return "Out of range";
    }
    return "";
}

// ============================================================================
// evaluate_belt_status()
// ============================================================================

BeltStatus evaluate_belt_status(float measured_hz, float target_hz, float tolerance_hz) {
    float delta = std::abs(measured_hz - target_hz);

    if (delta <= tolerance_hz) {
        return BeltStatus::GOOD;
    } else if (delta <= 2.0f * tolerance_hz) {
        return BeltStatus::WARNING;
    }
    return BeltStatus::BAD;
}

// ============================================================================
// BeltTensionResult::overall_status()
// ============================================================================

BeltStatus BeltTensionResult::overall_status() const {
    if (!is_complete()) {
        return BeltStatus::GOOD; // No data yet, neutral status
    }

    // Large A/B delta is always BAD regardless of individual status
    if (frequency_delta > tolerance * 1.5f) {
        return BeltStatus::BAD;
    }

    BeltStatus status_a = evaluate_belt_status(path_a.peak_frequency, target_frequency, tolerance);
    BeltStatus status_b = evaluate_belt_status(path_b.peak_frequency, target_frequency, tolerance);

    // Return the worse of the two
    if (status_a == BeltStatus::BAD || status_b == BeltStatus::BAD) {
        return BeltStatus::BAD;
    }
    if (status_a == BeltStatus::WARNING || status_b == BeltStatus::WARNING) {
        return BeltStatus::WARNING;
    }
    return BeltStatus::GOOD;
}

// ============================================================================
// BeltTensionResult::recommendation()
// ============================================================================

std::string BeltTensionResult::recommendation() const {
    if (!is_complete()) {
        return "Run a measurement to get recommendations.";
    }

    float freq_a = path_a.peak_frequency;
    float freq_b = path_b.peak_frequency;
    float delta = std::abs(freq_a - freq_b);

    BeltStatus status_a = evaluate_belt_status(freq_a, target_frequency, tolerance);
    BeltStatus status_b = evaluate_belt_status(freq_b, target_frequency, tolerance);

    bool a_low = freq_a < (target_frequency - tolerance);
    bool b_low = freq_b < (target_frequency - tolerance);
    bool a_high = freq_a > (target_frequency + tolerance);
    bool b_high = freq_b > (target_frequency + tolerance);

    // Both good and similar frequencies
    if (status_a == BeltStatus::GOOD && status_b == BeltStatus::GOOD && delta <= tolerance) {
        return "Belt tension looks good!";
    }

    // Both low
    if (a_low && b_low) {
        return "Both belts need tightening.";
    }

    // Both high
    if (a_high && b_high) {
        return "Both belts are overtightened.";
    }

    // Frequencies differ significantly
    if (delta > tolerance) {
        if (freq_a < freq_b) {
            return "Tighten Path A belt to match Path B.";
        }
        return "Tighten Path B belt to match Path A.";
    }

    // One is off, the other is fine
    if (status_a != BeltStatus::GOOD && status_b == BeltStatus::GOOD) {
        if (a_low) {
            return "Tighten Path A belt.";
        }
        return "Loosen Path A belt.";
    }
    if (status_b != BeltStatus::GOOD && status_a == BeltStatus::GOOD) {
        if (b_low) {
            return "Tighten Path B belt.";
        }
        return "Loosen Path B belt.";
    }

    return "Adjust belt tension toward the target frequency.";
}

// ============================================================================
// calculate_similarity()
// ============================================================================

float calculate_similarity(const std::vector<std::pair<float, float>>& curve_a,
                           const std::vector<std::pair<float, float>>& curve_b) {
    if (curve_a.empty() || curve_b.empty()) {
        return 0.0f;
    }

    // Determine common frequency range
    float min_freq = std::max(curve_a.front().first, curve_b.front().first);
    float max_freq = std::min(curve_a.back().first, curve_b.back().first);

    if (min_freq >= max_freq) {
        return 0.0f;
    }

    // Interpolate both curves to common frequency bins (1 Hz resolution)
    int num_bins = static_cast<int>(max_freq - min_freq);
    if (num_bins < 2) {
        return 0.0f;
    }

    // Linear interpolation helper
    auto interpolate = [](const std::vector<std::pair<float, float>>& curve, float freq) -> float {
        // Find bracketing points
        for (size_t i = 1; i < curve.size(); ++i) {
            if (curve[i].first >= freq) {
                float f0 = curve[i - 1].first;
                float f1 = curve[i].first;
                float v0 = curve[i - 1].second;
                float v1 = curve[i].second;
                if (std::abs(f1 - f0) < 1e-6f) {
                    return v0;
                }
                float t = (freq - f0) / (f1 - f0);
                return v0 + t * (v1 - v0);
            }
        }
        return curve.back().second;
    };

    // Build interpolated arrays
    std::vector<float> vals_a(num_bins);
    std::vector<float> vals_b(num_bins);

    for (int i = 0; i < num_bins; ++i) {
        float freq = min_freq + static_cast<float>(i);
        vals_a[i] = interpolate(curve_a, freq);
        vals_b[i] = interpolate(curve_b, freq);
    }

    // Pearson correlation coefficient
    float mean_a =
        std::accumulate(vals_a.begin(), vals_a.end(), 0.0f) / static_cast<float>(num_bins);
    float mean_b =
        std::accumulate(vals_b.begin(), vals_b.end(), 0.0f) / static_cast<float>(num_bins);

    float sum_ab = 0.0f;
    float sum_a2 = 0.0f;
    float sum_b2 = 0.0f;

    for (int i = 0; i < num_bins; ++i) {
        float da = vals_a[i] - mean_a;
        float db = vals_b[i] - mean_b;
        sum_ab += da * db;
        sum_a2 += da * da;
        sum_b2 += db * db;
    }

    float denom = std::sqrt(sum_a2 * sum_b2);
    if (denom < 1e-10f) {
        return 0.0f;
    }

    float r = sum_ab / denom;
    // Clamp to [0, 1] and convert to percentage
    return std::clamp(r * 100.0f, 0.0f, 100.0f);
}

// ============================================================================
// parse_accel_csv()
// ============================================================================

std::vector<AccelSample> parse_accel_csv(const std::string& csv_data) {
    std::vector<AccelSample> samples;
    std::istringstream stream(csv_data);
    std::string line;

    while (std::getline(stream, line)) {
        // Skip empty lines and comment headers
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Parse "time,accel_x,accel_y,accel_z"
        AccelSample sample{};
        char comma1, comma2, comma3;
        std::istringstream line_stream(line);

        if (line_stream >> sample.time >> comma1 >> sample.x >> comma2 >> sample.y >> comma3 >>
            sample.z) {
            if (comma1 == ',' && comma2 == ',' && comma3 == ',') {
                samples.push_back(sample);
            }
        }
    }

    spdlog::debug("[BeltTension] Parsed {} accelerometer samples from CSV", samples.size());
    return samples;
}

// ============================================================================
// compute_psd()
// ============================================================================

std::vector<std::pair<float, float>> compute_psd(const std::vector<AccelSample>& samples,
                                                 float sample_rate, float max_freq_hz) {
    std::vector<std::pair<float, float>> psd;

    if (samples.size() < 4) {
        spdlog::warn("[BeltTension] Too few samples ({}) for PSD computation", samples.size());
        return psd;
    }

    size_t n = samples.size();

    // Compute PSD per-axis and sum (avoids sqrt nonlinearity that creates harmonics
    // when one axis has a large DC component like gravity)
    // This matches Klipper/Shake&Tune's approach.

    // DFT parameters
    if (max_freq_hz <= 0.0f) {
        spdlog::warn("[BeltTension] No bandwidth requested, falling back to 250 Hz - this misses "
                     "the harmonic series pitch estimation needs above ~60 Hz fundamentals");
    }
    const float bandwidth = (max_freq_hz > 0.0f) ? max_freq_hz : 250.0f;
    // Clamp to Nyquist BEFORE the float->size_t cast below: an unclamped bandwidth
    // (e.g. a caller-supplied 1e12 Hz) produces a float product wildly out of
    // size_t range, and casting that is undefined behaviour rather than a
    // large-then-clamped value.
    const float effective_bandwidth = std::min(bandwidth, sample_rate * 0.5f);
    size_t max_bin = std::min(
        n / 2, static_cast<size_t>(effective_bandwidth * static_cast<float>(n) / sample_rate));
    if (max_bin == 0) {
        spdlog::warn("[BeltTension] Bandwidth {:.1f} Hz yields no bins at {:.1f} Hz sample rate",
                     bandwidth, sample_rate);
        return psd;
    }
    float freq_resolution = sample_rate / static_cast<float>(n);

    spdlog::debug("[BeltTension] Computing PSD: {} samples, {:.1f} Hz sample rate, {:.2f} Hz "
                  "resolution, {} bins",
                  n, sample_rate, freq_resolution, max_bin);

    psd.resize(max_bin, {0.0f, 0.0f});

    // Process each axis independently: extract signal, remove DC, apply window, DFT
    auto process_axis = [&](auto accessor) {
        std::vector<float> signal(n);
        for (size_t i = 0; i < n; ++i) {
            signal[i] = accessor(samples[i]);
        }

        // Remove DC offset
        float mean = std::accumulate(signal.begin(), signal.end(), 0.0f) / static_cast<float>(n);
        for (auto& s : signal) {
            s -= mean;
        }

        // Apply Hanning window
        for (size_t i = 0; i < n; ++i) {
            float window =
                0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * static_cast<float>(i) /
                                        static_cast<float>(n - 1)));
            signal[i] *= window;
        }

        // DFT for this axis, accumulate power into psd.
        //
        // exp(-j*omega*i) is advanced by complex multiplication rather than
        // recomputed per sample. Measured 5.8x faster on an Allwinner H616.
        // The accumulators are double deliberately: a float phasor accumulates
        // enough rotation error over thousands of samples to smear the peak.
        for (size_t k = 1; k <= max_bin; ++k) {
            const double omega = 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n);
            const double step_cos = std::cos(omega);
            const double step_sin = std::sin(omega);

            double phasor_cos = 1.0; // angle 0
            double phasor_sin = 0.0;
            double real = 0.0;
            double imag = 0.0;

            for (size_t i = 0; i < n; ++i) {
                const double s = signal[i];
                real += s * phasor_cos;
                imag -= s * phasor_sin;
                const double next_cos = phasor_cos * step_cos - phasor_sin * step_sin;
                phasor_sin = phasor_cos * step_sin + phasor_sin * step_cos;
                phasor_cos = next_cos;
            }

            const double power =
                (real * real + imag * imag) / (static_cast<double>(n) * sample_rate);
            psd[k - 1].second += static_cast<float>(power);
        }
    };

    // Process X, Y, Z axes
    process_axis([](const AccelSample& s) { return s.x; });
    process_axis([](const AccelSample& s) { return s.y; });
    process_axis([](const AccelSample& s) { return s.z; });

    // Fill in frequency values
    for (size_t k = 0; k < max_bin; ++k) {
        psd[k].first = static_cast<float>(k + 1) * freq_resolution;
    }

    spdlog::debug("[BeltTension] PSD computation complete: {} frequency bins", psd.size());
    return psd;
}

// ============================================================================
// find_peak_frequency()
// ============================================================================

PeakResult find_peak_frequency(const std::vector<std::pair<float, float>>& psd, float min_freq,
                               float max_freq) {
    PeakResult result;

    if (psd.empty()) {
        return result;
    }

    for (const auto& [freq, power] : psd) {
        if (freq < min_freq || freq > max_freq) {
            continue;
        }
        if (power > result.amplitude) {
            result.frequency = freq;
            result.amplitude = power;
            result.found = true;
        }
    }

    if (result.found) {
        spdlog::debug("[BeltTension] Peak found at {:.1f} Hz (amplitude: {:.4f})", result.frequency,
                      result.amplitude);
    } else {
        spdlog::warn("[BeltTension] No peak found in range [{:.0f}, {:.0f}] Hz", min_freq,
                     max_freq);
    }

    return result;
}

} // namespace helix::calibration
