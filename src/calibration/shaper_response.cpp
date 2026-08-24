// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file shaper_response.cpp
 * @brief Klipper input shaper tap definitions and transfer evaluation
 *
 * The tap formulas mirror klippy/extras/shaper_defs.py and the per-frequency
 * evaluation mirrors shaper_calibrate.py's _estimate_shaper(): each tap is
 * weighted by a decaying envelope exp(-zeta*omega*(T_last - T_i)) and the
 * magnitude of the weighted tap sum is the transfer at that frequency. The
 * curve is maxed over Klipper's TEST_DAMPING_RATIOS, which is what the
 * firmware writes into a calibration CSV's fitted columns.
 */

#include "shaper_response.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace helix {
namespace calibration {

namespace {

/// Damping ratios Klipper pessimizes the fitted curve over when writing a
/// calibration CSV (shaper_calibrate.py TEST_DAMPING_RATIOS). Matching the
/// set is what makes our curves agree with the firmware's column values.
constexpr std::array<double, 3> TEST_DAMPING_RATIOS = {0.075, 0.1, 0.15};

/// Vibration tolerance all EI-family shapers derive their taps from
/// (shaper_defs.py SHAPER_VIBRATION_REDUCTION = 20).
constexpr double VIBRATION_REDUCTION = 20.0;

/// Input shaper taps: amplitudes A and trigger times T (seconds)
struct ShaperTaps {
    std::array<double, 5> a{};
    std::array<double, 5> t{};
    size_t count = 0;
};

/// Port of shaper_defs.py get_zv_shaper()
ShaperTaps zv_taps(double freq, double zeta) {
    const double df = std::sqrt(1.0 - zeta * zeta);
    const double k = std::exp(-zeta * M_PI / df);
    const double t_d = 1.0 / (freq * df);
    return {{1.0, k}, {0.0, 0.5 * t_d}, 2};
}

/// Port of shaper_defs.py get_zvd_shaper()
ShaperTaps zvd_taps(double freq, double zeta) {
    const double df = std::sqrt(1.0 - zeta * zeta);
    const double k = std::exp(-zeta * M_PI / df);
    const double t_d = 1.0 / (freq * df);
    return {{1.0, 2.0 * k, k * k}, {0.0, 0.5 * t_d, t_d}, 3};
}

/// Port of shaper_defs.py get_mzv_shaper()
ShaperTaps mzv_taps(double freq, double zeta) {
    const double df = std::sqrt(1.0 - zeta * zeta);
    const double k = std::exp(-0.75 * zeta * M_PI / df);
    const double t_d = 1.0 / (freq * df);

    const double a1 = 1.0 - 1.0 / std::sqrt(2.0);
    const double a2 = (std::sqrt(2.0) - 1.0) * k;
    const double a3 = a1 * k * k;
    return {{a1, a2, a3}, {0.0, 0.375 * t_d, 0.75 * t_d}, 3};
}

/// Port of shaper_defs.py get_ei_shaper()
ShaperTaps ei_taps(double freq, double zeta) {
    const double v_tol = 1.0 / VIBRATION_REDUCTION;
    const double df = std::sqrt(1.0 - zeta * zeta);
    const double k = std::exp(-zeta * M_PI / df);
    const double t_d = 1.0 / (freq * df);

    const double a1 = 0.25 * (1.0 + v_tol);
    const double a2 = 0.5 * (1.0 - v_tol) * k;
    const double a3 = a1 * k * k;
    return {{a1, a2, a3}, {0.0, 0.5 * t_d, t_d}, 3};
}

/// Port of shaper_defs.py get_2hump_ei_shaper()
ShaperTaps two_hump_ei_taps(double freq, double zeta) {
    const double v_tol = 1.0 / VIBRATION_REDUCTION;
    const double df = std::sqrt(1.0 - zeta * zeta);
    const double k = std::exp(-zeta * M_PI / df);
    const double t_d = 1.0 / (freq * df);

    const double v2 = v_tol * v_tol;
    const double x = std::cbrt(v2 * (std::sqrt(1.0 - v2) + 1.0));
    const double a1 = (3.0 * x * x + 2.0 * x + 3.0 * v2) / (16.0 * x);
    const double a2 = (0.5 - a1) * k;
    const double a3 = a2 * k;
    const double a4 = a1 * k * k * k;
    return {{a1, a2, a3, a4}, {0.0, 0.5 * t_d, t_d, 1.5 * t_d}, 4};
}

/// Port of shaper_defs.py get_3hump_ei_shaper()
ShaperTaps three_hump_ei_taps(double freq, double zeta) {
    const double v_tol = 1.0 / VIBRATION_REDUCTION;
    const double df = std::sqrt(1.0 - zeta * zeta);
    const double k = std::exp(-zeta * M_PI / df);
    const double t_d = 1.0 / (freq * df);

    const double k2 = k * k;
    const double a1 = 0.0625 * (1.0 + 3.0 * v_tol + 2.0 * std::sqrt(2.0 * (v_tol + 1.0) * v_tol));
    const double a2 = 0.25 * (1.0 - v_tol) * k;
    const double a3 = (0.5 * (1.0 + v_tol) - 2.0 * a1) * k2;
    const double a4 = a2 * k2;
    const double a5 = a1 * k2 * k2;
    return {{a1, a2, a3, a4, a5}, {0.0, 0.5 * t_d, t_d, 1.5 * t_d, 2.0 * t_d}, 5};
}

/// Tap table keyed by the shaper names Klipper reports; anything else
/// (Kalico smooth shapers, vendor-fork names) has no ported definition.
ShaperTaps taps_for(const std::string& type, double freq, double zeta) {
    if (type == "zv") {
        return zv_taps(freq, zeta);
    }
    if (type == "zvd") {
        return zvd_taps(freq, zeta);
    }
    if (type == "mzv") {
        return mzv_taps(freq, zeta);
    }
    if (type == "ei") {
        return ei_taps(freq, zeta);
    }
    if (type == "2hump_ei") {
        return two_hump_ei_taps(freq, zeta);
    }
    if (type == "3hump_ei") {
        return three_hump_ei_taps(freq, zeta);
    }
    return {};
}

} // anonymous namespace

std::vector<double> shaper_transfer_curve(const std::string& shaper_type, double shaper_freq_hz,
                                          double damping_ratio,
                                          const std::vector<double>& freqs_hz) {
    if (!(shaper_freq_hz > 0.0) || freqs_hz.empty()) {
        return {};
    }

    const ShaperTaps taps = taps_for(shaper_type, shaper_freq_hz, damping_ratio);
    if (taps.count == 0) {
        return {};
    }

    const double t_last = taps.t[taps.count - 1];
    double sum_a = 0.0;
    for (size_t i = 0; i < taps.count; i++) {
        sum_a += taps.a[i];
    }
    const double inv_d = 1.0 / sum_a;

    std::vector<double> curve;
    curve.reserve(freqs_hz.size());
    for (double f : freqs_hz) {
        // Pessimize over the test damping ratios, exactly as the firmware's
        // CSV writer does: the reported attenuation is the worst case.
        double worst = 0.0;
        for (double dr : TEST_DAMPING_RATIOS) {
            const double omega = 2.0 * M_PI * f;
            const double damping = dr * omega;
            const double omega_d = omega * std::sqrt(1.0 - dr * dr);
            double s_part = 0.0;
            double c_part = 0.0;
            for (size_t i = 0; i < taps.count; i++) {
                const double w = taps.a[i] * std::exp(-damping * (t_last - taps.t[i]));
                s_part += w * std::sin(omega_d * taps.t[i]);
                c_part += w * std::cos(omega_d * taps.t[i]);
            }
            worst = std::max(worst, std::sqrt(s_part * s_part + c_part * c_part) * inv_d);
        }
        curve.push_back(worst);
    }
    return curve;
}

double residual_vibration_percent(const std::vector<double>& psd,
                                  const std::vector<double>& transfer_curve) {
    if (psd.empty() || transfer_curve.size() != psd.size()) {
        return -1.0;
    }

    // Klipper's _estimate_remaining_vibrations(): only vibration above
    // psd.max()/SHAPER_VIBRATION_REDUCTION counts (a shaper cannot attenuate
    // further than that factor, so anything below is treated as noise), and
    // the shaped spectrum is H*psd, linear. Matching that formula keeps the
    // verdict row comparable to the vibrations% the comparison table shows.
    double psd_max = 0.0;
    for (double p : psd) {
        psd_max = std::max(psd_max, p);
    }
    const double threshold = psd_max / VIBRATION_REDUCTION;

    double total = 0.0;
    double residual = 0.0;
    for (size_t i = 0; i < psd.size(); i++) {
        total += std::max(psd[i] - threshold, 0.0);
        residual += std::max(psd[i] * transfer_curve[i] - threshold, 0.0);
    }
    if (!(total > 0.0)) {
        return -1.0;
    }
    return residual / total * 100.0;
}

} // namespace calibration
} // namespace helix
