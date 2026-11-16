// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_motion.h"

#if HELIX_ENABLE_SCREENSAVER
#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_test_access.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::screensaver;

// ============================================================================
// MotionClock
// ============================================================================

TEST_CASE("MotionClock reports the time between calls", "[screensaver_motion]") {
    MotionClock clock;
    clock.reset(1000);
    CHECK(clock.advance(1033) == 33);
    CHECK(clock.advance(1050) == 17);
    CHECK(clock.advance(1050) == 0);
}

TEST_CASE("MotionClock measures across the tick wrap", "[screensaver_motion]") {
    MotionClock clock;
    clock.reset(UINT32_MAX - 10);
    // 10 ms up to UINT32_MAX, 1 ms to wrap to 0, 20 ms more.
    CHECK(clock.advance(20) == 31);
    CHECK(clock.advance(40) == 20);
}

TEST_CASE("MotionClock clamps one long gap", "[screensaver_motion]") {
    MotionClock clock;
    clock.reset(0);
    CHECK(clock.advance(MotionClock::kMaxGapMs) == MotionClock::kMaxGapMs);
    CHECK(clock.advance(MotionClock::kMaxGapMs + 60000) == MotionClock::kMaxGapMs);
    // The clamp spends the whole gap: the next call measures from the late call.
    CHECK(clock.advance(MotionClock::kMaxGapMs + 60010) == 10);
}

TEST_CASE("MotionClock reset restarts the measurement", "[screensaver_motion]") {
    MotionClock clock;
    clock.reset(0);
    CHECK(clock.advance(100) == 100);
    clock.reset(90000);
    CHECK(clock.advance(90025) == 25);
}

// ============================================================================
// flight_pos_at
// ============================================================================

TEST_CASE("flight_pos_at holds the start position until the delay ends", "[screensaver_motion]") {
    const FlightPos pos = flight_pos_at(4999, 700, -80, 10000, 5000, 1600);
    CHECK_FALSE(pos.started);
    CHECK(pos.x == 700);
    CHECK(pos.y == -80);
}

TEST_CASE("flight_pos_at starts at the start position when the delay ends",
          "[screensaver_motion]") {
    const FlightPos pos = flight_pos_at(5000, 700, -80, 10000, 5000, 1600);
    CHECK(pos.started);
    CHECK(pos.x == 700);
    CHECK(pos.y == -80);
}

TEST_CASE("flight_pos_at moves left and down in proportion to flight time",
          "[screensaver_motion]") {
    SECTION("no delay") {
        // 2500 of 10000 ms = a quarter of 1600 px.
        const FlightPos pos = flight_pos_at(2500, 700, -80, 10000, 0, 1600);
        CHECK(pos.started);
        CHECK(pos.x == 300);
        CHECK(pos.y == 320);
    }
    SECTION("measured from the end of the delay") {
        // 110 ms into a 24 s flight: 1600 * 110 / 24000 = 7.33, truncated to 7.
        const FlightPos pos = flight_pos_at(12110, 10, 20, 24000, 12000, 1600);
        CHECK(pos.started);
        CHECK(pos.x == 3);
        CHECK(pos.y == 27);
    }
}

TEST_CASE("flight_pos_at repeats the flight from the start", "[screensaver_motion]") {
    CHECK(flight_pos_at(11000, 700, -80, 10000, 1000, 1600).x == 700);
    CHECK(flight_pos_at(11000, 700, -80, 10000, 1000, 1600).y == -80);
    // 3 flights and 1 s later: 1600 / 10 = 160 px along.
    const FlightPos pos = flight_pos_at(1000 + 3 * 10000 + 1000, 700, -80, 10000, 1000, 1600);
    CHECK(pos.x == 540);
    CHECK(pos.y == 80);
}

TEST_CASE("flight_pos_at is exact at the end of a long 24 s flight", "[screensaver_motion]") {
    // Last millisecond of the flight: 1600 * 23999 / 24000 = 1599.93, truncated to 1599.
    const FlightPos pos = flight_pos_at(23999, 0, 0, 24000, 0, 1600);
    CHECK(pos.x == -1599);
    CHECK(pos.y == 1599);
}

// ============================================================================
// flap_frame_at
// ============================================================================

namespace {
std::vector<int> flap_sequence(int32_t delay_ms, uint32_t step_ms, uint8_t initial, int count) {
    std::vector<int> frames;
    for (int i = 0; i < count; i++) {
        frames.push_back(flap_frame_at(static_cast<uint32_t>(delay_ms) + i * step_ms, delay_ms,
                                       step_ms, initial));
    }
    return frames;
}
} // namespace

TEST_CASE("flap_frame_at ping-pongs through the four wing frames", "[screensaver_motion]") {
    CHECK(flap_sequence(0, 50, 0, 13) == std::vector<int>{0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1, 0});
}

TEST_CASE("flap_frame_at starts a reversed toaster at its initial frame", "[screensaver_motion]") {
    CHECK(flap_sequence(0, 50, 2, 8) == std::vector<int>{2, 3, 2, 1, 0, 1, 2, 3});
}

TEST_CASE("flap_frame_at holds each frame for one step", "[screensaver_motion]") {
    CHECK(flap_frame_at(0, 0, 100, 0) == 0);
    CHECK(flap_frame_at(99, 0, 100, 0) == 0);
    CHECK(flap_frame_at(100, 0, 100, 0) == 1);
    CHECK(flap_frame_at(299, 0, 100, 0) == 2);
    CHECK(flap_frame_at(300, 0, 100, 0) == 3);
}

TEST_CASE("flap_frame_at shows the initial frame until the delay ends", "[screensaver_motion]") {
    CHECK(flap_frame_at(0, 4000, 50, 2) == 2);
    CHECK(flap_frame_at(3999, 4000, 50, 2) == 2);
    CHECK(flap_sequence(4000, 50, 2, 3) == std::vector<int>{2, 3, 2});
}

// ============================================================================
// StepAccumulator
// ============================================================================

TEST_CASE("StepAccumulator carries a partial step", "[screensaver_motion]") {
    StepAccumulator acc;
    CHECK(acc.steps_due(60, 100, 3) == 0);
    CHECK(acc.steps_due(60, 100, 3) == 1); // 120: one step, 20 carried
    CHECK(acc.steps_due(79, 100, 3) == 0); // 99
    CHECK(acc.steps_due(1, 100, 3) == 1);  // 100
}

TEST_CASE("StepAccumulator gives one step per step length in a long gap", "[screensaver_motion]") {
    StepAccumulator acc;
    CHECK(acc.steps_due(300, 100, 3) == 3);
    CHECK(acc.steps_due(0, 100, 3) == 0);
}

TEST_CASE("StepAccumulator caps a call and drops the steps beyond the cap",
          "[screensaver_motion]") {
    StepAccumulator acc;
    // 4 steps due, 50 ms carried.
    CHECK(acc.steps_due(450, 100, 3) == 3);
    // The fourth step is not owed to the next call.
    CHECK(acc.steps_due(0, 100, 3) == 0);
    // The partial step still is.
    CHECK(acc.steps_due(50, 100, 3) == 1);
}

TEST_CASE("StepAccumulator reset drops the partial step", "[screensaver_motion]") {
    StepAccumulator acc;
    CHECK(acc.steps_due(90, 100, 3) == 0);
    acc.reset();
    CHECK(acc.steps_due(90, 100, 3) == 0);
    CHECK(acc.steps_due(10, 100, 3) == 1);
}

// ============================================================================
// Owned random numbers
// ============================================================================

TEST_CASE("unit_random stays within [0, 1] and spans it", "[screensaver_motion]") {
    std::minstd_rand rng(42);
    float lo = 1.0f;
    float hi = 0.0f;
    for (int i = 0; i < 10000; i++) {
        const float v = unit_random(rng);
        REQUIRE(v >= 0.0f);
        REQUIRE(v <= 1.0f);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    CHECK(lo < 0.01f);
    CHECK(hi > 0.99f);
}

TEST_CASE("random_below stays within [0, n) and reaches every value", "[screensaver_motion]") {
    std::minstd_rand rng(7);
    std::vector<int> seen(6, 0);
    for (int i = 0; i < 6000; i++) {
        const int v = random_below(rng, 6);
        REQUIRE(v >= 0);
        REQUIRE(v < 6);
        seen[static_cast<size_t>(v)]++;
    }
    for (int count : seen) {
        CHECK(count > 0);
    }
}

// ============================================================================
// Shared rng and wrap helpers, each proven equal to the spellings the savers write by hand
// ============================================================================

namespace {

int32_t fireflies_random_ms(std::minstd_rand& rng, int32_t lo, int32_t hi) {
    return lo + random_below(rng, hi - lo + 1);
}

float fireflies_uniform(std::minstd_rand& rng, float lo, float hi) {
    return lo + (hi - lo) * unit_random(rng);
}

float fireflies_wrap360(float deg) {
    float d = std::fmod(deg, 360.0f);
    if (d < 0.0f) {
        d += 360.0f;
    }
    return d >= 360.0f ? 0.0f : d;
}

void pipes_dissolve_shuffle(std::minstd_rand& rng, std::vector<uint32_t>& order) {
    for (size_t i = order.size(); i-- > 1;) {
        const auto j = static_cast<size_t>(random_below(rng, static_cast<int>(i + 1)));
        std::swap(order[i], order[j]);
    }
}

} // namespace

TEST_CASE("random_int draws the same integers as lo + random_below(span)", "[screensaver_motion]") {
    for (const auto& [lo, hi] :
         {std::pair{0, 0}, std::pair{-5, 5}, std::pair{8000, 15000}, std::pair{3, 4}}) {
        std::minstd_rand a(99);
        std::minstd_rand b(99);
        bool saw_lo = false;
        bool saw_hi = false;
        for (int i = 0; i < 5000; i++) {
            const int got = random_int(a, lo, hi);
            REQUIRE(got == fireflies_random_ms(b, lo, hi));
            REQUIRE(got >= lo);
            REQUIRE(got <= hi);
            saw_lo = saw_lo || got == lo;
            saw_hi = saw_hi || got == hi;
        }
        if (hi - lo < 100) {
            CHECK(saw_lo);
            CHECK(saw_hi);
        }
    }
}

TEST_CASE("random_between is fireflies' uniform and random_normal(mean, sd) its spelled-out "
          "form",
          "[screensaver_motion]") {
    std::minstd_rand a(7);
    std::minstd_rand b(7);
    for (int i = 0; i < 2000; i++) {
        REQUIRE(screensaver_same_bits(random_between(a, -3.5f, 12.25f),
                                      fireflies_uniform(b, -3.5f, 12.25f)));
    }
    for (int i = 0; i < 2000; i++) {
        const float got = random_normal(a, -0.1f, 0.3f);
        const float expect = -0.1f + 0.3f * random_normal(b);
        REQUIRE(screensaver_same_bits(got, expect));
    }
}

TEST_CASE("shuffle_in_place gives the pipes dissolve's permutation for every seed",
          "[screensaver_motion]") {
    for (uint32_t seed = 1; seed <= 10; seed++) {
        for (const size_t n : {size_t{0}, size_t{1}, size_t{2}, size_t{7}, size_t{4800}}) {
            std::vector<uint32_t> got(n);
            std::vector<uint32_t> expect(n);
            for (size_t i = 0; i < n; i++) {
                got[i] = expect[i] = static_cast<uint32_t>(i);
            }
            std::minstd_rand a(seed);
            std::minstd_rand b(seed);
            shuffle_in_place(a, got.begin(), got.end());
            pipes_dissolve_shuffle(b, expect);
            REQUIRE(got == expect);
            // Both drew the same number of values.
            REQUIRE(a() == b());
        }
    }
    std::vector<uint32_t> order(64);
    for (size_t i = 0; i < order.size(); i++) {
        order[i] = static_cast<uint32_t>(i);
    }
    std::vector<uint32_t> shuffled = order;
    std::minstd_rand rng(3);
    shuffle_in_place(rng, shuffled.begin(), shuffled.end());
    CHECK(shuffled != order);
    std::sort(shuffled.begin(), shuffled.end());
    CHECK(shuffled == order);
}

TEST_CASE("wrap_index folds any integer into [0, n)", "[screensaver_motion]") {
    for (const int32_t n : {1, 7, 60, 86400}) {
        for (int64_t x = -3 * static_cast<int64_t>(n) - 5; x <= 3 * static_cast<int64_t>(n) + 5;
             x += (n > 100 ? 37 : 1)) {
            const auto expect = static_cast<int32_t>(((x % n) + n) % n);
            REQUIRE(wrap_index(x, n) == expect);
            REQUIRE(wrap_index(x, n) >= 0);
            REQUIRE(wrap_index(x, n) < n);
        }
    }
    CHECK(wrap_index(-1, 86400) == 86399);
    CHECK(wrap_index(86400, 86400) == 0);
}

TEST_CASE("wrap_period is fireflies' wrap360 and never returns the period",
          "[screensaver_motion]") {
    size_t mismatches = 0;
    for (int i = -400000; i <= 400000; i++) {
        const float deg = static_cast<float>(i) * 0.0037f;
        mismatches += !screensaver_same_bits(wrap_period(deg, 360.0f), fireflies_wrap360(deg));
    }
    for (const float edge : {-360.0f, 360.0f, -720.0f, std::nextafter(0.0f, -1.0f), -1e-7f}) {
        mismatches += !screensaver_same_bits(wrap_period(edge, 360.0f), fireflies_wrap360(edge));
        CHECK(wrap_period(edge, 360.0f) < 360.0f);
        CHECK(wrap_period(edge, 360.0f) >= 0.0f);
    }
    CHECK(mismatches == 0);
}

#if HELIX_ENABLE_SCREENSAVER

// ============================================================================
// Starfield and pipes run on frame time
// ============================================================================

namespace {

using StarAccess = StarfieldScreensaverTestAccess;
using PipesAccess = PipesScreensaverTestAccess;

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "StarfieldScreensaver moves stars in proportion to frame time",
                 "[screensaver][screensaver_motion]") {
    StarfieldScreensaver ss;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());

    // Near the centre and far from the camera, every star stays on screen for the whole
    // window, so none is recycled.
    StarAccess::place_stars(ss, 0.05f, 0.05f, 0.8f);
    const auto placed = StarAccess::stars(ss);
    REQUIRE_FALSE(placed.empty());

    advance_saver(ss, 66);
    const auto after_66 = StarAccess::stars(ss);
    advance_saver(ss, 11);
    const auto after_77 = StarAccess::stars(ss);

    for (size_t i = 0; i < placed.size(); i++) {
        CAPTURE(i);
        // A recycled star moves to a new random point, which would void the depth checks.
        REQUIRE(screensaver_same_bits(after_77[i].x, placed[i].x));
        REQUIRE(screensaver_same_bits(after_77[i].y, placed[i].y));
        // A star's speed is depth per 33 ms.
        CHECK(after_66[i].z == Catch::Approx(placed[i].z - placed[i].speed * 66.0f / 33.0f));
        CHECK(after_77[i].z == Catch::Approx(placed[i].z - placed[i].speed * 77.0f / 33.0f));
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "StarfieldScreensaver with the same seed replays the same stars",
                 "[screensaver][screensaver_motion]") {
    StarfieldScreensaver a;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_a{a};
    StarfieldScreensaver b;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_b{b};
    StarfieldScreensaver other;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_other{other};
    SaverTestAccess::set_fixed_seed(a, 1234);
    SaverTestAccess::set_fixed_seed(b, 1234);
    SaverTestAccess::set_fixed_seed(other, 99);

    a.start();
    b.start();
    other.start();
    REQUIRE(a.is_active());
    REQUIRE(b.is_active());
    REQUIRE(other.is_active());
    CHECK(StarAccess::stars(a) == StarAccess::stars(b));
    CHECK_FALSE(StarAccess::stars(a) == StarAccess::stars(other));

    // The two tick in turn, so a random sequence they shared would hand each a different
    // part of it whenever a star is recycled.
    int recycled = 0;
    for (int frame = 0; frame < 30; frame++) {
        lv_tick_inc(33);
        fire_timer(SaverTestAccess::timer(a));
        fire_timer(SaverTestAccess::timer(b));
        for (const auto& star : StarAccess::stars(a)) {
            if (screensaver_same_bits(star.z, 1.0f)) {
                recycled++;
            }
        }
    }
    REQUIRE(recycled > 0);
    CHECK(StarAccess::stars(a) == StarAccess::stars(b));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "StarfieldScreensaver restarts its seed and clock on start after stop",
                 "[screensaver][screensaver_motion]") {
    StarfieldScreensaver ss;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 4321);

    ss.start();
    REQUIRE(ss.is_active());
    const auto at_start = StarAccess::stars(ss);
    advance_saver(ss, 66);
    const auto first_frame = StarAccess::stars(ss);

    int moved = 0;
    for (size_t i = 0; i < at_start.size(); i++) {
        // A recycled star has a new random position.
        if (!screensaver_same_bits(first_frame[i].x, at_start[i].x)) {
            continue;
        }
        CAPTURE(i);
        CHECK(first_frame[i].z == Catch::Approx(at_start[i].z - at_start[i].speed * 2.0f));
        moved++;
    }
    REQUIRE(moved > 0);

    ss.stop();
    lv_tick_inc(5000);
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(StarAccess::stars(ss) == at_start);

    advance_saver(ss, 66);
    CHECK(StarAccess::stars(ss) == first_frame);
}

TEST_CASE_METHOD(LVGLTestFixture, "PipesScreensaver grows one step per 100 ms of frame time",
                 "[screensaver][screensaver_motion]") {
    PipesScreensaver one_call;
    ScreensaverStopOnExit<PipesScreensaver> stop_one{one_call};
    PipesScreensaver per_step;
    ScreensaverStopOnExit<PipesScreensaver> stop_per_step{per_step};
    SaverTestAccess::set_fixed_seed(one_call, 77);
    SaverTestAccess::set_fixed_seed(per_step, 77);

    one_call.start();
    per_step.start();
    REQUIRE(one_call.is_active());
    REQUIRE(per_step.is_active());
    REQUIRE(PipesAccess::same_scene(one_call, per_step));

    for (int i = 0; i < 3; i++) {
        advance_saver(per_step, 100);
    }
    fire_timer(SaverTestAccess::timer(one_call));

    // A 300 ms gap grows each of the two starting pipes three times.
    const auto pipes = PipesAccess::pipes(one_call);
    CHECK(pipes[0].segment_count == 3);
    CHECK(PipesAccess::same_scene(one_call, per_step));
}

TEST_CASE_METHOD(LVGLTestFixture, "PipesScreensaver caps the steps one callback draws",
                 "[screensaver][screensaver_motion]") {
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 5);
    ss.start();
    REQUIRE(ss.is_active());

    // Four steps are due; three are drawn.
    advance_saver(ss, 450);
    CHECK(PipesAccess::pipes(ss)[0].segment_count == 3);

    // The fourth is dropped rather than owed to the next callback.
    fire_timer(SaverTestAccess::timer(ss));
    CHECK(PipesAccess::pipes(ss)[0].segment_count == 3);

    // The 50 ms left over still counts toward the next step.
    advance_saver(ss, 50);
    CHECK(PipesAccess::pipes(ss)[0].segment_count == 4);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PipesScreensaver restarts its seed and step clock on start after stop",
                 "[screensaver][screensaver_motion]") {
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 3);

    ss.start();
    REQUIRE(ss.is_active());
    const auto at_start = PipesAccess::pipes(ss);
    advance_saver(ss, 250);
    REQUIRE(PipesAccess::pipes(ss)[0].segment_count == 2);

    ss.stop();
    lv_tick_inc(5000);
    ss.start();
    REQUIRE(ss.is_active());
    // The seed replays the starting pipe.
    CHECK(PipesAccess::pipes(ss)[0] == at_start[0]);

    // Neither the 50 ms left from the first run nor the time spent stopped counts.
    advance_saver(ss, 60);
    CHECK(PipesAccess::pipes(ss)[0].segment_count == 0);
    advance_saver(ss, 40);
    CHECK(PipesAccess::pipes(ss)[0].segment_count == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "PipesScreensaver with the same seed replays the same scene",
                 "[screensaver][screensaver_motion]") {
    PipesScreensaver a;
    ScreensaverStopOnExit<PipesScreensaver> stop_a{a};
    PipesScreensaver b;
    ScreensaverStopOnExit<PipesScreensaver> stop_b{b};
    PipesScreensaver other;
    ScreensaverStopOnExit<PipesScreensaver> stop_other{other};
    SaverTestAccess::set_fixed_seed(a, 2024);
    SaverTestAccess::set_fixed_seed(b, 2024);
    SaverTestAccess::set_fixed_seed(other, 7);

    a.start();
    b.start();
    other.start();
    REQUIRE(a.is_active());
    REQUIRE(b.is_active());
    REQUIRE(other.is_active());
    CHECK(PipesAccess::same_scene(a, b));
    CHECK_FALSE(PipesAccess::same_scene(a, other));

    // The two grow in turn, so a random sequence they shared would steer them apart.
    for (int i = 0; i < 20; i++) {
        lv_tick_inc(100);
        fire_timer(SaverTestAccess::timer(a));
        fire_timer(SaverTestAccess::timer(b));
    }
    int grown = 0;
    for (const auto& pipe : PipesAccess::pipes(a)) {
        grown += pipe.segment_count;
    }
    REQUIRE(grown >= 20);
    CHECK(PipesAccess::same_scene(a, b));
}

TEST_CASE_METHOD(LVGLTestFixture, "the pipes saver declares the pipes ladder",
                 "[screensaver][screensaver_motion]") {
    PipesScreensaver ss;
    REQUIRE(ss.level_count() == 3);
    CHECK(ss.level_period_ms(1) == 33);
    CHECK(ss.level_period_ms(2) == 100);
    for (size_t i = 0; i < helix::ui::kPipesLevelCount; i++) {
        CHECK(helix::ui::kPipesLevels[i].scale == (i == 2 ? 2 : 1));
    }
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "a pipes level request changes the period at once and the scale at the next screen",
    "[screensaver][screensaver_motion]") {
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 9);
    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(PipesAccess::sim(ss).renderer().scale() == 1);

    ss.request_level(2);
    CHECK(ss.level() == 2);
    CHECK(SaverTestAccess::timer(ss)->period == 100);
    CHECK(PipesAccess::sim(ss).renderer().scale() == 1);

    // Run the screen out and through its dissolve; the next screen draws at half resolution.
    const uint32_t first = PipesAccess::sim(ss).screens_started();
    for (int i = 0; i < 20000 && PipesAccess::sim(ss).screens_started() == first; i++) {
        REQUIRE(PipesAccess::sim(ss).renderer().scale() == 1);
        advance_saver(ss, 300);
    }
    REQUIRE(PipesAccess::sim(ss).screens_started() == first + 1);
    CHECK(PipesAccess::sim(ss).renderer().scale() == 2);
}

#endif // HELIX_ENABLE_SCREENSAVER
