// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_helpers/sample_checks.h"
#include "sweep_synth.h"

#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using Catch::Approx;

namespace {

SweepSynthSpec quiet_spec() {
    SweepSynthSpec spec;
    spec.plan = SweepPlan{5.0f, 135.0f, 1.0f};
    spec.drive_start_s = 1.5;
    spec.rate_hz = 3053.0f;
    spec.noise_mm_s2 = 0.0f;
    spec.gravity_mm_s2 = 9810.0f;
    return spec;
}

SweepAxisGain flat_gain(float x, float y, float z) {
    return [x, y, z](float) { return std::array<float, 3>{x, y, z}; };
}

std::vector<float> axis(const std::vector<AccelSample>& samples, int which) {
    std::vector<float> out;
    out.reserve(samples.size());
    for (const auto& s : samples) {
        out.push_back(which == 0 ? s.x : which == 1 ? s.y : s.z);
    }
    return out;
}

} // namespace

TEST_CASE("SweepSynth's ramp spans the plan from the drive start", "[sweep_preview][synth]") {
    SweepSynth synth(quiet_spec(), flat_gain(100.0f, 50.0f, 10.0f));
    CHECK(std::isnan(synth.drive_hz_at(1.0)));
    CHECK(synth.drive_hz_at(1.5) == 5.0f);
    CHECK(synth.drive_hz_at(61.5) == 65.0f);
    CHECK(synth.drive_hz_at(131.5) == 135.0f);
    CHECK(std::isnan(synth.drive_hz_at(131.6)));
}

TEST_CASE("SweepSynth's stream time continues across calls", "[sweep_preview][synth]") {
    SweepSynth synth(quiet_spec(), flat_gain(100.0f, 50.0f, 10.0f));
    const auto first = synth.next(3053);
    const auto second = synth.next(3053);
    REQUIRE(first.size() == 3053);
    REQUIRE(second.size() == 3053);
    CHECK(first[0].time == 0.0f);
    CHECK(first[100].time == Approx(100.0 / 3053.0).margin(1e-6));
    CHECK(second[0].time == Approx(1.0).margin(1e-6));
    CHECK(second[10].time == Approx(3063.0 / 3053.0).margin(1e-6));
    CHECK(synth.time_s() == Approx(2.0).margin(1e-9));
}

TEST_CASE("SweepSynth carries only gravity before the ramp", "[sweep_preview][synth]") {
    SweepSynth synth(quiet_spec(), flat_gain(100.0f, 50.0f, 10.0f));
    const auto before = synth.next(4579); // 1.4998 s
    helix::test::SampleChecks samples;
    for (size_t i = 0; i < before.size(); ++i) {
        const auto& s = before[i];
        SAMPLE_REQUIRE(samples, s.x == 9810.0f, i);
        SAMPLE_REQUIRE(samples, s.y == 0.0f, i);
        SAMPLE_REQUIRE(samples, s.z == 0.0f, i);
    }
    samples.done(3 * 4579);
}

TEST_CASE("SweepSynth's drive phase is continuous across calls", "[sweep_preview][synth]") {
    auto spec = quiet_spec();
    spec.drive_start_s = 0.1;
    spec.noise_mm_s2 = 30.0f;
    SweepSynth split(spec, flat_gain(100.0f, 50.0f, 10.0f));
    SweepSynth whole(spec, flat_gain(100.0f, 50.0f, 10.0f));

    auto a = split.next(1000);
    const auto b = split.next(2053);
    a.insert(a.end(), b.begin(), b.end());
    const auto c = whole.next(3053);
    helix::test::SampleChecks samples;
    REQUIRE(a.size() == c.size());
    for (size_t i = 0; i < a.size(); ++i) {
        SAMPLE_REQUIRE(samples, a[i].time == c[i].time, i);
        SAMPLE_REQUIRE(samples, a[i].x == c[i].x, i);
        SAMPLE_REQUIRE(samples, a[i].y == c[i].y, i);
        SAMPLE_REQUIRE(samples, a[i].z == c[i].z, i);
    }
    samples.done(4 * 3053);
}

TEST_CASE("SweepSynth drives each axis at its gain", "[sweep_preview][synth]") {
    SweepSynth synth(quiet_spec(), flat_gain(100.0f, 50.0f, 10.0f));
    // Drive 60 Hz is at 56.5 s; take [56.0, 57.0).
    static_cast<void>(synth.next(static_cast<size_t>(56.0 * 3053.0)));
    REQUIRE(synth.drive_hz_at(synth.time_s()) == Approx(59.5f).margin(0.01));
    const auto window = synth.next(3053);
    const auto x = axis(window, 0);
    const auto y = axis(window, 1);

    const double n = static_cast<double>(window.size());
    const double expected_x = 100.0 * 100.0 * n / (3.0 * 3053.0);
    const float px = goertzel_psd(x.data(), x.size(), 60.0f, 3053.0f);
    const float py = goertzel_psd(y.data(), y.size(), 60.0f, 3053.0f);
    CHECK(px == Approx(expected_x).epsilon(0.10));
    CHECK(py == Approx(px / 4.0f).epsilon(0.10));
}

TEST_CASE("SweepSynth's hiss is bounded and seeded", "[sweep_preview][synth]") {
    auto spec = quiet_spec();
    spec.noise_mm_s2 = 30.0f;
    SweepSynth synth(spec, flat_gain(0.0f, 0.0f, 0.0f));
    const auto samples = synth.next(20000);
    bool any_nonzero = false;
    helix::test::SampleChecks checks;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& s = samples[i];
        // x is rounded at 9810's float spacing, so its top end can land on 30.
        SAMPLE_REQUIRE(checks, s.x - 9810.0f >= -30.0f, i);
        SAMPLE_REQUIRE(checks, s.x - 9810.0f <= 30.0f, i);
        SAMPLE_REQUIRE(checks, s.y >= -30.0f, i);
        SAMPLE_REQUIRE(checks, s.y < 30.0f, i);
        SAMPLE_REQUIRE(checks, s.z >= -30.0f, i);
        SAMPLE_REQUIRE(checks, s.z < 30.0f, i);
        any_nonzero = any_nonzero || s.y != 0.0f;
    }
    checks.done(6 * 20000);
    CHECK(any_nonzero);

    SweepSynth same(spec, flat_gain(0.0f, 0.0f, 0.0f));
    const auto again = same.next(20000);
    auto other_spec = spec;
    other_spec.seed = 1357u;
    SweepSynth other(other_spec, flat_gain(0.0f, 0.0f, 0.0f));
    const auto different = other.next(20000);

    bool identical = true;
    bool differs = false;
    for (size_t i = 0; i < samples.size(); ++i) {
        identical = identical && samples[i].x == again[i].x && samples[i].y == again[i].y &&
                    samples[i].z == again[i].z;
        differs = differs || samples[i].y != different[i].y;
    }
    CHECK(identical);
    CHECK(differs);
}
