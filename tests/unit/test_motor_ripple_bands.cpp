// SPDX-License-Identifier: GPL-3.0-or-later
#include "motor_ripple_analysis.h"
#include "motor_ripple_plan.h"

#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::calibration::ripple;
using Catch::Approx;

namespace {

std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/motor_ripple/";
    }
    return "tests/fixtures/motor_ripple/";
}

/// One motor's sweep from the spike fixtures (per-speed r1/r2/r4, null above the ceiling).
MotorSweep load_sweep(const std::string& file, const std::string& motor) {
    std::ifstream f(fixture_dir() + file);
    REQUIRE(f.is_open());
    const auto j = nlohmann::json::parse(f, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    MotorSweep sweep;
    sweep.motor = motor == "stepper_x" ? MotorId::StepperX : MotorId::StepperY;
    for (const auto& row : j["motors"][motor]) {
        SpeedResult r;
        r.speed_mm_s = row["speed"].get<float>();
        const char* keys[] = {"r1", "r2", "r4"};
        for (size_t k = 0; k < 3; ++k) {
            if (!row[keys[k]].is_null()) {
                r.ripple[k] = row[keys[k]].get<float>();
            }
        }
        sweep.speeds.push_back(r);
    }
    return sweep;
}

void check_band(const RippleBand& b, float lo, float hi, int harmonic) {
    CHECK(b.lo_mm_s == Approx(lo).margin(0.2f));
    CHECK(b.hi_mm_s == Approx(hi).margin(0.2f));
    CHECK(b.harmonic == harmonic);
}

constexpr const char* kVoron = "voron24_tmc2240_adxl345.json";

} // namespace

TEST_CASE("the Voron's sweep marks the speeds where each ripple meets its 216 Hz resonance",
          "[ripple][bands]") {
    const auto x = find_bands(load_sweep(kVoron, "stepper_x"), 0.8f);
    REQUIRE(x.size() == 3);
    check_band(x[0], 77.0f, 97.0f, 2);
    CHECK(x[0].peak_speed_mm_s == Approx(86.3f));
    CHECK(x[0].freq_hz == Approx(215.75f).margin(1.0f));
    check_band(x[1], 113.1f, 122.1f, 2);
    check_band(x[2], 166.2f, 179.4f, 1);
    CHECK(x[2].peak_speed_mm_s == Approx(172.5f));

    const auto y = find_bands(load_sweep(kVoron, "stepper_y"), 0.8f);
    REQUIRE(y.size() == 3);
    check_band(y[0], 41.6f, 44.9f, 4);
    check_band(y[1], 77.0f, 97.0f, 2);
    check_band(y[2], 153.9f, 179.4f, 1);
}

TEST_CASE("a slow accelerometer's sweep is banded from measured values only", "[ripple][bands]") {
    const auto sweep = load_sweep("qidi_q2_lis2dw.json", "stepper_x");
    const auto bands = find_bands(sweep, 0.7764f);
    CHECK(band_containing(bands, 43.5f) != nullptr);
    CHECK(band_containing(bands, 101.4f) != nullptr);
    for (const auto& b : bands) {
        const auto& at =
            *std::find_if(sweep.speeds.begin(), sweep.speeds.end(),
                          [&](const SpeedResult& r) { return r.speed_mm_s == b.peak_speed_mm_s; });
        const size_t k = b.harmonic == 1 ? 0 : b.harmonic == 2 ? 1 : 2;
        CHECK(at.ripple[k].has_value());
    }
}

TEST_CASE("a flat sweep, or a peak under the floor, makes no band", "[ripple][bands]") {
    MotorSweep flat;
    for (float s : sweep_speeds(500.0f)) {
        SpeedResult r;
        r.speed_mm_s = s;
        r.ripple = {100.0f, 100.0f, 100.0f};
        flat.speeds.push_back(r);
    }
    CHECK(find_bands(flat, 0.8f).empty());
    flat.speeds[10].ripple[1] = 350.0f; // 3.5x the median, but under 500 mm/s^2
    CHECK(find_bands(flat, 0.8f).empty());
    flat.speeds[10].ripple[1] = 600.0f;
    CHECK(find_bands(flat, 0.8f).size() == 1);
}

TEST_CASE("bands from both motors merge, and advice steps just outside them", "[ripple][bands]") {
    auto all = find_bands(load_sweep(kVoron, "stepper_x"), 0.8f);
    const auto y = find_bands(load_sweep(kVoron, "stepper_y"), 0.8f);
    all.insert(all.end(), y.begin(), y.end());
    const auto merged = merge_bands(all);
    REQUIRE(merged.size() == 4);
    check_band(merged[1], 77.0f, 97.0f, 2);
    CHECK(band_containing(merged, 86.0f) != nullptr);
    CHECK(band_containing(merged, 100.0f) == nullptr);
    CHECK(nearest_clear_speed(merged, 86.0f, true) == 76);
    CHECK(nearest_clear_speed(merged, 86.0f, false) == 97); // the band ends at 96.95
    CHECK(nearest_clear_speed(merged, 120.0f, true) == 113);
    CHECK(nearest_clear_speed(merged, 170.0f, false) == 180);
    RippleBand top;
    top.lo_mm_s = 290.0f;
    top.hi_mm_s = 300.0f;
    CHECK_FALSE(nearest_clear_speed({top}, 295.0f, false).has_value());
}
