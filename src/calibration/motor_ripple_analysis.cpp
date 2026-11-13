// SPDX-License-Identifier: GPL-3.0-or-later
#include "motor_ripple_analysis.h"

#include <algorithm>
#include <cmath>

namespace helix::calibration::ripple {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kFrameCycles = 6.0f; ///< cycles of the 1x ripple per frame
constexpr float kMinFrameS = 0.16f;
constexpr float kMaxFrameS = 0.5f;
constexpr int kMinFrameSamples = 16;
constexpr float kHighPassHz = 15.0f;
constexpr float kMovingFraction = 0.3f; ///< of the 90th percentile broadband
constexpr float kMovingPercentile = 0.9f;

float percentile(std::vector<float> v, float p) {
    if (v.empty()) {
        return 0.0f;
    }
    const size_t idx =
        std::min(v.size() - 1, static_cast<size_t>(p * static_cast<float>(v.size() - 1) + 0.5f));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
    return v[idx];
}

float median(std::vector<float> v) {
    if (v.empty()) {
        return 0.0f;
    }
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const float upper = v[mid];
    if (v.size() % 2 == 1) {
        return upper;
    }
    const float lower = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return (lower + upper) / 2.0f;
}

/// Goertzel power at one frequency over a windowed, mean-removed frame.
float goertzel_power(const float* x, const float* w, int n, float mean, float coeff) {
    float s1 = 0.0f;
    float s2 = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float s0 = (x[i] - mean) * w[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

} // namespace

float electrical_cycle_mm(const MotorGeometry& g) {
    if (g.full_steps_per_rotation <= 0 || !(g.gear_ratio > 0.0f)) {
        return 0.0f;
    }
    return g.rotation_distance_mm / g.gear_ratio * 4.0f /
           static_cast<float>(g.full_steps_per_rotation);
}

float ripple_freq_hz(float speed_mm_s, int harmonic, float electrical_mm) {
    if (!(electrical_mm > 0.0f)) {
        return 0.0f;
    }
    return static_cast<float>(harmonic) * speed_mm_s / electrical_mm;
}

float ceiling_hz(float sample_rate_hz) {
    return kCeilingFraction * sample_rate_hz;
}

SpeedResult analyze_capture(const std::vector<AccelSample>& samples, float sample_rate_hz,
                            float speed_mm_s, float electrical_mm) {
    SpeedResult out;
    out.speed_mm_s = speed_mm_s;
    if (!(sample_rate_hz > 0.0f) || !(electrical_mm > 0.0f) || !(speed_mm_s > 0.0f)) {
        return out;
    }
    const float f1 = speed_mm_s / electrical_mm;
    const float frame_s = std::clamp(kFrameCycles / f1, kMinFrameS, kMaxFrameS);
    const int n = static_cast<int>(std::lround(frame_s * sample_rate_hz));
    const int total = static_cast<int>(samples.size());
    if (n < kMinFrameSamples || total < n) {
        return out;
    }
    const int hop = std::max(1, n / 4);
    const int frames = (total - n) / hop + 1;

    // Axes split out once, plus a first-order high-pass copy for the broadband level.
    std::vector<float> axis[3];
    std::vector<float> hp[3];
    const float rc = 1.0f / (2.0f * kPi * kHighPassHz);
    const float dt = 1.0f / sample_rate_hz;
    const float alpha = rc / (rc + dt);
    for (int a = 0; a < 3; ++a) {
        axis[a].resize(static_cast<size_t>(total));
        hp[a].resize(static_cast<size_t>(total));
    }
    for (int i = 0; i < total; ++i) {
        const auto& s = samples[static_cast<size_t>(i)];
        axis[0][static_cast<size_t>(i)] = s.x;
        axis[1][static_cast<size_t>(i)] = s.y;
        axis[2][static_cast<size_t>(i)] = s.z;
    }
    for (int a = 0; a < 3; ++a) {
        hp[a][0] = 0.0f;
        for (int i = 1; i < total; ++i) {
            const auto ui = static_cast<size_t>(i);
            hp[a][ui] = alpha * (hp[a][ui - 1] + axis[a][ui] - axis[a][ui - 1]);
        }
    }

    std::vector<float> w(static_cast<size_t>(n));
    float sum_w = 0.0f;
    for (int i = 0; i < n; ++i) {
        w[static_cast<size_t>(i)] =
            0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(n - 1));
        sum_w += w[static_cast<size_t>(i)];
    }

    const float ceiling = ceiling_hz(sample_rate_hz);
    std::array<bool, 3> measurable{};
    std::array<float, 3> coeff{};
    for (size_t k = 0; k < kHarmonics.size(); ++k) {
        const float fk = static_cast<float>(kHarmonics[k]) * f1;
        measurable[k] = fk < ceiling;
        coeff[k] = 2.0f * std::cos(2.0f * kPi * fk / sample_rate_hz);
    }

    std::vector<float> broadband(static_cast<size_t>(frames));
    std::array<std::vector<float>, 3> amps;
    for (auto& v : amps) {
        v.resize(static_cast<size_t>(frames));
    }
    for (int f = 0; f < frames; ++f) {
        const int start = f * hop;
        float energy = 0.0f;
        std::array<float, 3> mean{};
        for (int a = 0; a < 3; ++a) {
            const float* x = axis[a].data() + start;
            const float* h = hp[a].data() + start;
            float sum = 0.0f;
            for (int i = 0; i < n; ++i) {
                sum += x[i];
                energy += h[i] * h[i];
            }
            mean[static_cast<size_t>(a)] = sum / static_cast<float>(n);
        }
        broadband[static_cast<size_t>(f)] = std::sqrt(energy / static_cast<float>(n));
        for (size_t k = 0; k < kHarmonics.size(); ++k) {
            if (!measurable[k]) {
                continue;
            }
            float power = 0.0f;
            for (int a = 0; a < 3; ++a) {
                power += goertzel_power(axis[a].data() + start, w.data(), n,
                                        mean[static_cast<size_t>(a)], coeff[k]);
            }
            amps[k][static_cast<size_t>(f)] = 2.0f * std::sqrt(std::max(0.0f, power)) / sum_w;
        }
    }

    const float moving_floor = kMovingFraction * percentile(broadband, kMovingPercentile);
    std::vector<float> moving_bb;
    std::array<std::vector<float>, 3> moving_amps;
    for (int f = 0; f < frames; ++f) {
        if (broadband[static_cast<size_t>(f)] <= moving_floor) {
            continue;
        }
        moving_bb.push_back(broadband[static_cast<size_t>(f)]);
        for (size_t k = 0; k < kHarmonics.size(); ++k) {
            moving_amps[k].push_back(amps[k][static_cast<size_t>(f)]);
        }
    }
    out.frames_used = static_cast<int>(moving_bb.size());
    if (moving_bb.empty()) {
        return out;
    }
    out.broadband = median(moving_bb);
    for (size_t k = 0; k < kHarmonics.size(); ++k) {
        if (measurable[k]) {
            out.ripple[k] = median(moving_amps[k]);
        }
    }
    return out;
}

std::vector<RippleBand> find_bands(const MotorSweep& sweep, float electrical_mm, float factor,
                                   float floor_mm_s2) {
    const auto& sp = sweep.speeds;
    std::vector<RippleBand> out;
    if (sp.empty()) {
        return out;
    }
    std::array<float, 3> med{};
    for (size_t k = 0; k < kHarmonics.size(); ++k) {
        std::vector<float> vals;
        for (const auto& r : sp) {
            if (r.ripple[k]) {
                vals.push_back(*r.ripple[k]);
            }
        }
        med[k] = median(vals);
    }

    std::optional<RippleBand> cur;
    for (size_t i = 0; i < sp.size(); ++i) {
        int best_k = -1;
        float best = 0.0f;
        for (size_t k = 0; k < kHarmonics.size(); ++k) {
            const auto& v = sp[i].ripple[k];
            if (v && *v > factor * med[k] && *v > floor_mm_s2 && *v > best) {
                best = *v;
                best_k = static_cast<int>(k);
            }
        }
        if (best_k < 0) {
            continue;
        }
        const float s = sp[i].speed_mm_s;
        const float lo = i > 0 ? (sp[i - 1].speed_mm_s + s) / 2.0f : s;
        const float hi = i + 1 < sp.size() ? (sp[i + 1].speed_mm_s + s) / 2.0f : s;
        const int harmonic = kHarmonics[static_cast<size_t>(best_k)];
        if (cur && cur->hi_mm_s >= lo - 1e-3f) {
            cur->hi_mm_s = hi;
        } else {
            if (cur) {
                out.push_back(*cur);
            }
            cur = RippleBand{};
            cur->motor = sweep.motor;
            cur->lo_mm_s = lo;
            cur->hi_mm_s = hi;
        }
        if (best > cur->peak_mm_s2) {
            cur->harmonic = harmonic;
            cur->peak_speed_mm_s = s;
            cur->peak_mm_s2 = best;
            cur->freq_hz = ripple_freq_hz(s, harmonic, electrical_mm);
        }
    }
    if (cur) {
        out.push_back(*cur);
    }
    return out;
}

std::vector<RippleBand> merge_bands(std::vector<RippleBand> bands) {
    std::sort(bands.begin(), bands.end(),
              [](const RippleBand& a, const RippleBand& b) { return a.lo_mm_s < b.lo_mm_s; });
    std::vector<RippleBand> out;
    for (const auto& b : bands) {
        if (!out.empty() && b.lo_mm_s <= out.back().hi_mm_s) {
            auto& m = out.back();
            m.hi_mm_s = std::max(m.hi_mm_s, b.hi_mm_s);
            if (b.peak_mm_s2 > m.peak_mm_s2) {
                const float lo = m.lo_mm_s;
                const float hi = m.hi_mm_s;
                m = b;
                m.lo_mm_s = lo;
                m.hi_mm_s = hi;
            }
        } else {
            out.push_back(b);
        }
    }
    return out;
}

const RippleBand* band_containing(const std::vector<RippleBand>& bands, float speed_mm_s) {
    for (const auto& b : bands) {
        if (speed_mm_s >= b.lo_mm_s && speed_mm_s <= b.hi_mm_s) {
            return &b;
        }
    }
    return nullptr;
}

std::optional<int> nearest_clear_speed(const std::vector<RippleBand>& bands, float speed_mm_s,
                                       bool below) {
    float x = speed_mm_s;
    while (const RippleBand* b = band_containing(bands, x)) {
        x = below ? std::floor(b->lo_mm_s - 1e-3f) : std::ceil(b->hi_mm_s + 1e-3f);
    }
    if (x < kSweepMinMmS || x > kSweepMaxMmS) {
        return std::nullopt;
    }
    return static_cast<int>(std::lround(x));
}

} // namespace helix::calibration::ripple
