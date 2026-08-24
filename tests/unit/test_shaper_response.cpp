// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_shaper_response.cpp
 * @brief Pure-math tests for the ported Klipper input shaper transfer functions
 *
 * The ei case is pinned against FIRMWARE OUTPUT, not against our own formula:
 * the (freq, attenuation) pairs below are read from the ei(29.0) column of a
 * real K1C SHAPER_CALIBRATE AXIS=x CSV (calibration_data_x_20260819_164514.csv,
 * captured 2026-08-19), the column the firmware itself fitted and wrote. If our
 * port drifts from klippy's math, this file goes red.
 */

#include "../../include/shaper_response.h"

#include <cmath>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

namespace {

/// One (frequency, firmware-fitted attenuation) pair from the K1C CSV
struct EiFixturePoint {
    double freq;
    double attenuation;
};

/// The ei(29.0) column of the K1C run: DC passes unchanged, the notch bottoms
/// out near 29 Hz, and the EI side lobes rise again past 2x the shaper freq.
const EiFixturePoint K1C_EI_29[] = {
    {0.0, 1.000},  {1.5, 0.978},  {18.5, 0.220},  {26.2, 0.026},  {29.3, 0.039},
    {37.0, 0.099}, {55.4, 0.611}, {110.9, 0.403}, {147.8, 0.149}, {184.8, 0.235},
};

/// Evenly spaced frequency grid covering one shaper's whole suppression band
std::vector<double> grid(double first, double last, int steps) {
    std::vector<double> out;
    out.reserve(static_cast<size_t>(steps));
    for (int i = 0; i < steps; i++) {
        out.push_back(first + (last - first) * i / (steps - 1));
    }
    return out;
}

/// Mean transfer value of a shaper over a grid - the band-averaged attenuation
double mean_curve(const std::vector<double>& curve) {
    double sum = 0.0;
    for (double v : curve) {
        sum += v;
    }
    return sum / static_cast<double>(curve.size());
}

} // namespace

TEST_CASE("ei transfer curve matches K1C firmware output", "[shaper_response][calibration]") {
    std::vector<double> freqs;
    for (const auto& p : K1C_EI_29) {
        freqs.push_back(p.freq);
    }

    const auto curve = shaper_transfer_curve("ei", 29.0, SHAPER_DEFAULT_DAMPING_RATIO, freqs);

    REQUIRE(curve.size() == freqs.size());
    for (size_t i = 0; i < freqs.size(); i++) {
        INFO("freq " << freqs[i] << " Hz: computed " << curve[i] << ", firmware wrote "
                     << K1C_EI_29[i].attenuation);
        CHECK(std::abs(curve[i] - K1C_EI_29[i].attenuation) < 0.01);
    }
}

TEST_CASE("every shaper type passes DC unchanged and notches at its frequency",
          "[shaper_response][calibration]") {
    const std::string types[] = {"zv", "zvd", "mzv", "ei", "2hump_ei", "3hump_ei"};
    constexpr double SHAPER_FREQ = 50.0;

    for (const auto& type : types) {
        INFO("shaper type " << type);
        const std::vector<double> freqs = {0.0, SHAPER_FREQ};
        const auto curve =
            shaper_transfer_curve(type, SHAPER_FREQ, SHAPER_DEFAULT_DAMPING_RATIO, freqs);
        REQUIRE(curve.size() == 2);

        // A shaper must not distort steady-state motion: H(0) == 1.
        CHECK(std::abs(curve[0] - 1.0) < 1e-9);

        // At its own fitted frequency the shaper is close to transparent-minimal.
        // zv is the shallowest notch of the family (~0.16), so 0.25 bounds all.
        CHECK(curve[1] < 0.25);
    }
}

TEST_CASE("higher-hump shapers suppress a wider band", "[shaper_response][calibration]") {
    // Band-averaged attenuation over 1.25-300 Hz for shapers fitted at 50 Hz.
    // More humps = more taps spread further in time = wider notch, so the mean
    // transfer must drop monotonically along the hump chain.
    const std::vector<double> band = grid(1.25, 300.0, 240);

    const double mean_zv =
        mean_curve(shaper_transfer_curve("zv", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, band));
    const double mean_zvd =
        mean_curve(shaper_transfer_curve("zvd", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, band));
    const double mean_ei =
        mean_curve(shaper_transfer_curve("ei", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, band));
    const double mean_2hump =
        mean_curve(shaper_transfer_curve("2hump_ei", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, band));
    const double mean_3hump =
        mean_curve(shaper_transfer_curve("3hump_ei", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, band));

    CHECK(mean_zv > mean_zvd);
    CHECK(mean_zv > mean_ei);
    CHECK(mean_ei > mean_2hump);
    CHECK(mean_2hump > mean_3hump);
}

TEST_CASE("unknown shaper types degrade to an empty curve", "[shaper_response][calibration]") {
    const std::vector<double> freqs = {0.0, 10.0, 50.0, 100.0};

    // Kalico smooth shapers and vendor-fork names have no ported taps; callers
    // must get an empty curve, never a crash or a fabricated line.
    CHECK(shaper_transfer_curve("smooth_ei", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, freqs).empty());
    CHECK(shaper_transfer_curve("creality_x", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, freqs).empty());
    CHECK(shaper_transfer_curve("", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, freqs).empty());
}

TEST_CASE("residual vibration rewards a notch placed on the peak",
          "[shaper_response][calibration]") {
    // Single sharp resonance at 50 Hz over 0-150 Hz. Residual uses klippy's
    // _estimate_remaining_vibrations(): only signal above psd.max()/20 counts
    // and the shaped spectrum is H*psd, linear - the same number the firmware
    // prints in the comparison table.
    const std::vector<double> freqs = grid(0.0, 150.0, 151);
    std::vector<double> psd;
    psd.reserve(freqs.size());
    for (double f : freqs) {
        const double df = (f - 50.0) / 4.0;
        psd.push_back(f > 0.0 ? 1.0 / (1.0 + df * df) : 0.0);
    }

    const auto notch_on_peak =
        shaper_transfer_curve("ei", 50.0, SHAPER_DEFAULT_DAMPING_RATIO, freqs);
    const auto notch_off_peak =
        shaper_transfer_curve("ei", 80.0, SHAPER_DEFAULT_DAMPING_RATIO, freqs);

    const double residual_on = residual_vibration_percent(psd, notch_on_peak);
    const double residual_off = residual_vibration_percent(psd, notch_off_peak);

    REQUIRE(residual_on >= 0.0);
    REQUIRE(residual_off >= 0.0);
    CHECK(residual_on < residual_off);

    // A fully transparent curve leaves everything: 100%.
    const std::vector<double> flat(psd.size(), 1.0);
    CHECK(std::abs(residual_vibration_percent(psd, flat) - 100.0) < 1e-9);
}

TEST_CASE("residual vibration guards invalid input", "[shaper_response][calibration]") {
    const std::vector<double> psd = {1.0, 2.0, 3.0};
    const std::vector<double> curve = {0.5, 0.5, 0.5};

    CHECK(residual_vibration_percent({}, curve) < 0.0);
    CHECK(residual_vibration_percent(psd, {}) < 0.0);
    // Size mismatch: the verdict is unknowable, not approximately right.
    CHECK(residual_vibration_percent(psd, {0.5, 0.5}) < 0.0);
    // All-zero PSD: division would be 0/0.
    CHECK(residual_vibration_percent({0.0, 0.0, 0.0}, curve) < 0.0);

    // Sanity: the valid pairing still works next to the guards.
    CHECK(residual_vibration_percent(psd, curve) >= 0.0);
}
