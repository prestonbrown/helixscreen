// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <cstdint>

/**
 * @file screensaver_fast_math.h
 * @brief Table sine and e^-s for savers that evaluate them per pixel or per grid cell
 *
 * The tables are built at compile time into .rodata, so a saver that is not running costs no
 * RAM for them. Linear interpolation between entries keeps the sine within 5e-6 of std::sin.
 */

namespace helix::ui {

/// pi and 2 pi as floats, for every saver's angles.
inline constexpr float kSaverPi = 3.14159265358979f;
inline constexpr float kSaverTwoPi = 2.0f * kSaverPi;

inline constexpr int kFastSinN = 1024;
inline constexpr int kFastExpN = 512;
/// e^-s reads as 0 past this.
inline constexpr double kFastExpMax = 9.0;

namespace fast_math_detail {

constexpr double sin_series(double x) {
    // x in [-pi, pi]: the Taylor series converges to float precision in a dozen terms.
    double term = x;
    double sum = x;
    for (int n = 1; n < 14; n++) {
        term *= -x * x / static_cast<double>((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

constexpr double exp_series(double x) {
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n < 60; n++) {
        term *= x / static_cast<double>(n);
        sum += term;
    }
    return sum;
}

} // namespace fast_math_detail

struct FastSinTable {
    float v[kFastSinN + 1];
};

constexpr FastSinTable make_fast_sin_table() {
    FastSinTable t{};
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i <= kFastSinN; i++) {
        double a = 2.0 * pi * static_cast<double>(i) / kFastSinN;
        if (a > pi) {
            a -= 2.0 * pi;
        }
        t.v[i] = static_cast<float>(fast_math_detail::sin_series(a));
    }
    return t;
}

struct FastExpTable {
    float v[kFastExpN + 1];
};

constexpr FastExpTable make_fast_exp_table() {
    FastExpTable t{};
    for (int i = 0; i <= kFastExpN; i++) {
        t.v[i] = static_cast<float>(
            1.0 / fast_math_detail::exp_series(kFastExpMax * static_cast<double>(i) / kFastExpN));
    }
    return t;
}

/// sin over one turn in kFastSinN steps, one entry past the end for interpolation.
inline constexpr FastSinTable kFastSinTable = make_fast_sin_table();
/// e^-s for s from 0 to kFastExpMax in kFastExpN steps.
inline constexpr FastExpTable kFastExpTable = make_fast_exp_table();

/// Floats this large are whole numbers already.
inline constexpr float kSaverWholeFloat = 8388608.0f;

/**
 * @brief std::floor(v), bit for bit but for the sign of a zero, without a libm call
 *
 * MIPS32r2 has no rounding instruction, so std::floor and std::ceil are calls; in a per-pixel
 * loop they cost more than the arithmetic around them. NaN and infinities pass through.
 */
inline float saver_floor(float v) {
    if (!(std::fabs(v) < kSaverWholeFloat)) {
        return v;
    }
    const auto t = static_cast<float>(static_cast<int32_t>(v));
    return t > v ? t - 1.0f : t;
}

/// std::ceil(v), as saver_floor.
inline float saver_ceil(float v) {
    if (!(std::fabs(v) < kSaverWholeFloat)) {
        return v;
    }
    const auto t = static_cast<float>(static_cast<int32_t>(v));
    return t < v ? t + 1.0f : t;
}

inline float fast_sin(float x) {
    constexpr float steps_per_radian = static_cast<float>(kFastSinN) / 6.28318530717958f;
    const float f = x * steps_per_radian;
    const float fl = saver_floor(f);
    const float frac = f - fl;
    const int i = static_cast<int>(static_cast<int64_t>(fl) & (kFastSinN - 1));
    return kFastSinTable.v[i] + (kFastSinTable.v[i + 1] - kFastSinTable.v[i]) * frac;
}

inline float fast_cos(float x) {
    return fast_sin(x + 1.57079632679490f);
}

/// e^-s for s >= 0, 0 past kFastExpMax.
inline float fast_exp_neg(float s) {
    if (s <= 0.0f) {
        return 1.0f;
    }
    const float f = s * (static_cast<float>(kFastExpN) / static_cast<float>(kFastExpMax));
    if (f >= static_cast<float>(kFastExpN)) {
        return 0.0f;
    }
    const auto i = static_cast<int>(f);
    return kFastExpTable.v[i] +
           (kFastExpTable.v[i + 1] - kFastExpTable.v[i]) * (f - static_cast<float>(i));
}

/// e^-(q^2): a Gaussian of `q` standard-ish widths.
inline float fast_gauss(float q) {
    return fast_exp_neg(q * q);
}

} // namespace helix::ui
