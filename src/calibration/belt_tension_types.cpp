// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file belt_tension_types.cpp
 * @brief Implementation of belt tension analysis functions
 *
 * Provides PSD computation via DFT, CSV parsing for Klipper accelerometer
 * data, Pearson correlation for belt path similarity, and two-path curve
 * comparison with a provisional verdict.
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
// Path identity: belt_path_stepper() / belt_layout_from_int()
// ============================================================================

const char* belt_path_stepper(BeltPath path) {
    return path == BeltPath::PATH_A ? "stepper_y" : "stepper_x";
}

BeltLayout belt_layout_from_int(int value) {
    return value > 0 && value < kBeltLayoutCount ? static_cast<BeltLayout>(value)
                                                 : BeltLayout::UNSET;
}

// ============================================================================
// Band-limited comparison: band_similarity() / detect_belt_peaks() /
// pair_belt_peaks() / verdict_for_similarity() / compare_belt_paths()
// ============================================================================

BeltCurve curve_in_band(const BeltCurve& curve, float band_min_hz, float band_max_hz) {
    BeltCurve out;
    for (const auto& bin : curve) {
        if (bin.first >= band_min_hz && bin.first <= band_max_hz) {
            out.push_back(bin);
        }
    }
    return out;
}

namespace {

/// Linear interpolation at freq, clamped to the end bins outside the curve
/// (numpy's np.interp semantics).
float interp_at(const BeltCurve& curve, float freq) {
    if (freq <= curve.front().first) {
        return curve.front().second;
    }
    for (size_t i = 1; i < curve.size(); ++i) {
        if (curve[i].first >= freq) {
            const float f0 = curve[i - 1].first;
            const float f1 = curve[i].first;
            if (f1 - f0 < 1e-6f) {
                return curve[i - 1].second;
            }
            const float t = (freq - f0) / (f1 - f0);
            return curve[i - 1].second + t * (curve[i].second - curve[i - 1].second);
        }
    }
    return curve.back().second;
}

/// Percentile of ascending `values` with linear interpolation between closest
/// ranks - numpy's default - so thresholds match the reference computations.
float percentile_linear(const std::vector<float>& values, float pct) {
    const float pos = pct / 100.0f * static_cast<float>(values.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = std::min(lo + 1, values.size() - 1);
    return values[lo] + (pos - static_cast<float>(lo)) * (values[hi] - values[lo]);
}

} // namespace

BeltVerdict verdict_for_similarity(float similarity_percent) {
    using namespace belt_verdict;
    return similarity_percent >= kMatchedSimilarity ? BeltVerdict::MATCHED
           : similarity_percent >= kCloseSimilarity ? BeltVerdict::CLOSE
                                                    : BeltVerdict::ADJUST;
}

float band_similarity(const BeltCurve& a, const BeltCurve& b, float band_min_hz,
                      float band_max_hz) {
    const BeltCurve xa = curve_in_band(a, band_min_hz, band_max_hz);
    const BeltCurve xb = curve_in_band(b, band_min_hz, band_max_hz);
    if (xa.size() < 3 || xb.size() < 3) {
        return 0.0f;
    }

    // Pearson over A's in-band bins with B interpolated onto them. Double
    // accumulators: psd values reach 1e5 and products 1e10, past where float
    // rounding stays quiet in the headline percent.
    std::vector<double> va(xa.size()), vb(xa.size());
    for (size_t i = 0; i < xa.size(); ++i) {
        va[i] = xa[i].second;
        vb[i] = interp_at(xb, xa[i].first);
    }
    const double mean_a =
        std::accumulate(va.begin(), va.end(), 0.0) / static_cast<double>(va.size());
    const double mean_b =
        std::accumulate(vb.begin(), vb.end(), 0.0) / static_cast<double>(vb.size());
    double sum_ab = 0.0, sum_aa = 0.0, sum_bb = 0.0;
    for (size_t i = 0; i < va.size(); ++i) {
        const double da = va[i] - mean_a;
        const double db = vb[i] - mean_b;
        sum_ab += da * db;
        sum_aa += da * da;
        sum_bb += db * db;
    }
    const double denom = std::sqrt(sum_aa * sum_bb);
    if (denom < 1e-12) {
        return 0.0f;
    }
    const double r = sum_ab / denom;
    return static_cast<float>(std::clamp(r * 100.0, 0.0, 100.0));
}

std::vector<BeltPeak> detect_belt_peaks(const BeltCurve& curve, float band_min_hz,
                                        float band_max_hz) {
    const BeltCurve x = curve_in_band(curve, band_min_hz, band_max_hz);
    std::vector<BeltPeak> peaks;
    if (x.size() < 3) {
        return peaks;
    }
    float max_v = 0.0f;
    for (const auto& bin : x) {
        max_v = std::max(max_v, bin.second);
    }
    const float min_amplitude = belt_verdict::kPeakThresholdFraction * max_v;
    // A bin needs both neighbours in-band, so the band's edge bins cannot be
    // peaks. Ascending frequency comes free from the input order.
    for (size_t i = 1; i + 1 < x.size(); ++i) {
        if (x[i].second > x[i - 1].second && x[i].second >= x[i + 1].second &&
            x[i].second >= min_amplitude) {
            peaks.push_back({x[i].first, x[i].second});
        }
    }
    return peaks;
}

BeltPeakPairing pair_belt_peaks(const std::vector<BeltPeak>& a, const std::vector<BeltPeak>& b) {
    BeltPeakPairing out;

    std::vector<float> dists;
    dists.reserve(a.size() * b.size());
    for (const auto& pa : a) {
        for (const auto& pb : b) {
            dists.push_back(std::abs(pa.freq_hz - pb.freq_hz));
        }
    }
    if (dists.empty()) {
        out.threshold_hz = belt_verdict::kPairMaxHz;
    } else {
        std::sort(dists.begin(), dists.end());
        const float median = percentile_linear(dists, 50.0f);
        const float iqr = percentile_linear(dists, 75.0f) - percentile_linear(dists, 25.0f);
        out.threshold_hz = std::min(median + 1.5f * iqr, belt_verdict::kPairMaxHz);
    }

    // Greedy: repeatedly take the globally closest remaining (a, b) inside the
    // threshold. Scanning a then b in order and keeping only strictly closer
    // candidates breaks distance ties toward the lower a, then lower b, index.
    std::vector<bool> used_a(a.size(), false), used_b(b.size(), false);
    for (;;) {
        size_t best_i = a.size(), best_j = 0;
        float best_d = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            if (used_a[i]) {
                continue;
            }
            for (size_t j = 0; j < b.size(); ++j) {
                if (used_b[j]) {
                    continue;
                }
                const float d = std::abs(a[i].freq_hz - b[j].freq_hz);
                if (d <= out.threshold_hz && (best_i == a.size() || d < best_d)) {
                    best_i = i;
                    best_j = j;
                    best_d = d;
                }
            }
        }
        if (best_i == a.size()) {
            break;
        }
        out.pairs.push_back({a[best_i], b[best_j]});
        used_a[best_i] = true;
        used_b[best_j] = true;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        if (!used_a[i]) {
            out.unpaired_a.push_back(a[i]);
        }
    }
    for (size_t j = 0; j < b.size(); ++j) {
        if (!used_b[j]) {
            out.unpaired_b.push_back(b[j]);
        }
    }
    std::sort(out.pairs.begin(), out.pairs.end(), [](const BeltPeakPair& l, const BeltPeakPair& r) {
        return l.a.amplitude + l.b.amplitude > r.a.amplitude + r.b.amplitude;
    });
    return out;
}

BeltComparison compare_belt_paths(const BeltCurve& a, const BeltCurve& b, float band_min_hz,
                                  float band_max_hz) {
    BeltComparison out;
    if (curve_in_band(a, band_min_hz, band_max_hz).size() < 3 ||
        curve_in_band(b, band_min_hz, band_max_hz).size() < 3) {
        return out;
    }
    out.valid = true;
    for (const auto* curve : {&a, &b}) {
        for (const auto& bin : curve_in_band(*curve, band_min_hz, band_max_hz)) {
            out.band_max_psd = std::max(out.band_max_psd, bin.second);
        }
    }
    out.similarity_percent = band_similarity(a, b, band_min_hz, band_max_hz);
    out.verdict = verdict_for_similarity(out.similarity_percent);
    out.peaks = pair_belt_peaks(detect_belt_peaks(a, band_min_hz, band_max_hz),
                                detect_belt_peaks(b, band_min_hz, band_max_hz));
    spdlog::debug("[BeltTension] Comparison: similarity {:.1f}% in [{:.0f},{:.0f}] Hz, {} pairs, "
                  "{}+{} unpaired (threshold {:.2f} Hz)",
                  out.similarity_percent, band_min_hz, band_max_hz, out.peaks.pairs.size(),
                  out.peaks.unpaired_a.size(), out.peaks.unpaired_b.size(), out.peaks.threshold_hz);
    return out;
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
// HannWindowedSignal
// ============================================================================

HannWindowedSignal::HannWindowedSignal(const float* x, size_t n) {
    if (x == nullptr || n < 2) {
        return;
    }

    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) {
        mean += x[i];
    }
    mean /= static_cast<double>(n);

    // The window's cosine advances by a fixed rotation per sample, so the loop
    // needs no cos call.
    const double window_step = 2.0 * M_PI / static_cast<double>(n - 1);
    const double wc = std::cos(window_step);
    const double ws = std::sin(window_step);
    double win_re = 1.0; // cos(window_step * i)
    double win_im = 0.0;
    v_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * win_re;
        v_[i] = (static_cast<double>(x[i]) - mean) * w;
        sum_w2_ += w * w;

        const double nwr = win_re * wc - win_im * ws;
        win_im = win_re * ws + win_im * wc;
        win_re = nwr;
    }
}

double HannWindowedSignal::power(double omega) const {
    // exp(-j omega i) advances by complex multiplication rather than a cos/sin
    // per sample (5.8x faster on an Allwinner H616). It is double because a
    // float phasor accumulates enough rotation error over thousands of samples
    // to smear the peak.
    const double bc = std::cos(omega);
    const double bs = std::sin(omega);
    double ph_re = 1.0;
    double ph_im = 0.0;
    double acc_re = 0.0;
    double acc_im = 0.0;
    for (const double v : v_) {
        acc_re += v * ph_re;
        acc_im += v * ph_im;
        const double npr = ph_re * bc + ph_im * bs;
        ph_im = ph_im * bc - ph_re * bs;
        ph_re = npr;
    }
    return acc_re * acc_re + acc_im * acc_im;
}

namespace {

// The same operations, in the same order, as HannWindowedSignal::power() runs
// for each axis and frequency. The phasor update is the loop's serial chain:
// three axes share one, and B frequencies run B chains side by side.
template <size_t B>
void power_block(const double* const (&v)[3], size_t n, const double* omegas,
                 std::array<double, 3>* out) {
    double bc[B], bs[B], ph_re[B], ph_im[B];
    double re[3][B] = {}, im[3][B] = {};
    for (size_t b = 0; b < B; ++b) {
        bc[b] = std::cos(omegas[b]);
        bs[b] = std::sin(omegas[b]);
        ph_re[b] = 1.0;
        ph_im[b] = 0.0;
    }
    for (size_t i = 0; i < n; ++i) {
        for (size_t b = 0; b < B; ++b) {
            for (size_t a = 0; a < 3; ++a) {
                re[a][b] += v[a][i] * ph_re[b];
                im[a][b] += v[a][i] * ph_im[b];
            }
            const double npr = ph_re[b] * bc[b] + ph_im[b] * bs[b];
            ph_im[b] = ph_im[b] * bc[b] - ph_re[b] * bs[b];
            ph_re[b] = npr;
        }
    }
    for (size_t b = 0; b < B; ++b) {
        for (size_t a = 0; a < 3; ++a) {
            out[b][a] = re[a][b] * re[a][b] + im[a][b] * im[a][b];
        }
    }
}

} // namespace

void HannWindowedSignal::power(const std::array<HannWindowedSignal, 3>& axes, const double* omegas,
                               size_t count, std::array<double, 3>* out) {
    const size_t n = axes[0].v_.size();
    if (axes[1].v_.size() != n || axes[2].v_.size() != n) {
        for (size_t k = 0; k < count; ++k) {
            out[k] = {axes[0].power(omegas[k]), axes[1].power(omegas[k]), axes[2].power(omegas[k])};
        }
        return;
    }
    const double* const v[3] = {axes[0].v_.data(), axes[1].v_.data(), axes[2].v_.data()};
    constexpr size_t kBlock = 4;
    size_t k = 0;
    for (; k + kBlock <= count; k += kBlock) {
        power_block<kBlock>(v, n, omegas + k, out + k);
    }
    for (; k < count; ++k) {
        power_block<1>(v, n, omegas + k, out + k);
    }
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

    // Each axis is windowed on its own, and its power is summed into psd in
    // X, Y, Z order.
    std::vector<float> signal(n);
    auto windowed_axis = [&](float AccelSample::*field) {
        for (size_t i = 0; i < n; ++i) {
            signal[i] = samples[i].*field;
        }
        return HannWindowedSignal(signal.data(), n);
    };
    const std::array<HannWindowedSignal, 3> axes{windowed_axis(&AccelSample::x),
                                                 windowed_axis(&AccelSample::y),
                                                 windowed_axis(&AccelSample::z)};
    std::vector<double> omegas(max_bin);
    for (size_t k = 1; k <= max_bin; ++k) {
        omegas[k - 1] = 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n);
    }
    std::vector<std::array<double, 3>> power(max_bin);
    HannWindowedSignal::power(axes, omegas.data(), max_bin, power.data());
    const double scale = static_cast<double>(n) * sample_rate;
    for (size_t k = 0; k < max_bin; ++k) {
        for (const double p : power[k]) {
            psd[k].second += static_cast<float>(p / scale);
        }
    }

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
