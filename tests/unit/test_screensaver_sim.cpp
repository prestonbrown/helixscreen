// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_starfield_sim.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::StarfieldSim;
using StarAccess = StarfieldScreensaverTestAccess;

namespace {

/// A reading with no print: the sky as it flies between prints.
const helix::ui::ClockReading kNoPrint{};

constexpr uint32_t kFrameW = 320;
constexpr uint32_t kFrameH = 200;

/// A frame painted opaque black, as the canvas hands one to a saver.
struct Frame : helix::test::TestFrame {
    Frame() : TestFrame(kFrameW, kFrameH, helix::ui::PixelFormat::XRGB8888) {
        helix::ui::PixelWriter(target).fill(helix::ui::Rgb{0, 0, 0});
    }
};

} // namespace

// ============================================================================
// StarfieldSim: one frame of stars
// ============================================================================

TEST_CASE("StarfieldSim dirties only around stars near the centre",
          "[screensaver][starfield_sim]") {
    std::minstd_rand rng(11);
    StarfieldSim sim;
    Frame frame;
    sim.init(frame.target, rng, 0, kNoPrint);
    for (auto& star : sim.stars()) {
        star.x = 0.02f;
        star.y = -0.02f;
        star.z = 0.9f;
    }
    std::vector<DirtyRect> dirty;
    sim.step(33, frame.target, rng, kNoPrint, dirty);
    REQUIRE_FALSE(dirty.empty());

    // Erases the stars and draws them again a pixel or two away.
    sim.step(33, frame.target, rng, kNoPrint, dirty);
    REQUIRE_FALSE(dirty.empty());
    DirtyRect span;
    for (const DirtyRect& r : dirty) {
        span.add(r);
    }
    REQUIRE_FALSE(span.empty());
    const int64_t area = static_cast<int64_t>(span.x2 - span.x1 + 1) * (span.y2 - span.y1 + 1);
    CHECK(area < static_cast<int64_t>(kFrameW) * kFrameH / 100);
}

TEST_CASE("init places a full population whatever count the last run flew",
          "[screensaver][starfield_sim]") {
    std::minstd_rand rng(11);
    StarfieldSim sim;
    Frame frame;
    sim.init(frame.target, rng, 0, kNoPrint);
    sim.set_active_count(32);
    REQUIRE(sim.active_count() == 32);

    // A saver re-initialises on every start, and the level it last ran at must not
    // survive into the new run.
    sim.init(frame.target, rng, 0, kNoPrint);
    CHECK(sim.active_count() == StarfieldSim::kNumStars);

    std::vector<DirtyRect> dirty;
    sim.step(33, frame.target, rng, kNoPrint, dirty);
    CHECK(dirty.size() > 32);
}

TEST_CASE("a scattered sky reports one small rect per star", "[screensaver][starfield_sim]") {
    std::minstd_rand rng(23);
    StarfieldSim sim;
    Frame frame;
    sim.init(frame.target, rng, 0, kNoPrint);
    std::vector<DirtyRect> dirty;
    for (int step = 0; step < 60; step++) {
        sim.step(33, frame.target, rng, kNoPrint, dirty);
    }

    // A spread population pushes many rects, each clipped to the frame, whose summed
    // area is a small fraction of it - not one box spanning the screen.
    REQUIRE(dirty.size() > 32);
    CHECK(dirty.size() <= helix::ui::kSaverMaxDirtyAreas);
    int64_t covered = 0;
    for (const DirtyRect& r : dirty) {
        CAPTURE(r.x1, r.y1, r.x2, r.y2);
        CHECK(r.x1 >= 0);
        CHECK(r.y1 >= 0);
        CHECK(r.x2 < static_cast<int32_t>(kFrameW));
        CHECK(r.y2 < static_cast<int32_t>(kFrameH));
        covered += r.area();
    }
    CHECK(covered * 20 < static_cast<int64_t>(kFrameW) * kFrameH);
}

TEST_CASE("a star's size spans the full 1..3 pixels of its depth range",
          "[screensaver][starfield_sim]") {
    std::minstd_rand rng(3);
    StarfieldSim sim;
    Frame frame;
    sim.init(frame.target, rng, 0, kNoPrint);
    // Parked dead centre and moving nowhere, so each one draws every step.
    for (auto& star : sim.stars()) {
        star.x = 0.0f;
        star.y = 0.0f;
        star.speed = 0.0f;
    }
    sim.stars()[0].z = 0.02f; // just above the recycle bound: the closest a star draws
    sim.stars()[1].z = 0.5f;
    sim.stars()[2].z = 0.9f; // far away
    std::vector<DirtyRect> dirty;

    sim.step(33, frame.target, rng, kNoPrint, dirty);

    REQUIRE(sim.stars()[0].prev_size == 3);
    REQUIRE(sim.stars()[1].prev_size == 2);
    REQUIRE(sim.stars()[2].prev_size == 1);
    REQUIRE(dirty.size() == sim.stars().size());
    CHECK(dirty[0].area() == 9);
}

TEST_CASE("the active star count sets how many rects a frame pushes",
          "[screensaver][starfield_sim]") {
    std::minstd_rand rng(31);
    StarfieldSim sim;
    Frame frame;
    sim.init(frame.target, rng, 0, kNoPrint);
    // Parked dead centre and moving nowhere, so every active star draws every step.
    for (auto& star : sim.stars()) {
        star.x = 0.0f;
        star.y = 0.0f;
        star.z = 0.5f;
        star.speed = 0.0f;
    }
    std::vector<DirtyRect> dirty;

    for (const int count : {StarfieldSim::kNumStars, 96, 64, 32}) {
        CAPTURE(count);
        sim.set_active_count(count);
        sim.step(33, frame.target, rng, kNoPrint, dirty); // stars parked by the change erase here
        sim.step(33, frame.target, rng, kNoPrint, dirty);
        CHECK(sim.active_count() == count);
        CHECK(dirty.size() == static_cast<size_t>(count));
        CHECK(dirty.size() <= helix::ui::kSaverMaxDirtyAreas);
    }

    sim.set_active_count(0);
    CHECK(sim.active_count() == 1); // clamped: a level never empties the sky
}

// ============================================================================
// StarfieldSim: the planet a print approaches
// ============================================================================

namespace {

using helix::ui::ClockMode;
using helix::ui::ClockReading;
using helix::ui::StarfieldPlanet;

/// A sky at kFrameW x kFrameH, started on `reading`, that steps 33 ms frames on `reading`.
struct Sky {
    explicit Sky(const ClockReading& start, uint32_t seed = 41) : rng(seed), reading(start) {
        sim.init(frame.target, rng, 0, reading);
    }
    void step(uint32_t dt_ms = 33) {
        sim.step(dt_ms, frame.target, rng, reading, dirty);
    }
    /// Steps `ms` of flight, checking every frame's dirty boxes cover what it changed.
    void fly(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 33) {
            const std::vector<uint8_t> before = frame.snapshot();
            step();
            uncovered += helix::test::diff_against(before, frame, dirty).uncovered;
        }
    }
    std::minstd_rand rng;
    StarfieldSim sim;
    Frame frame;
    ClockReading reading;
    std::vector<DirtyRect> dirty;
    size_t uncovered = 0;
};

ClockReading print_at(ClockMode mode, int32_t pct) {
    return helix::test::print_reading_for(mode, pct);
}

/// The planet's radius at `pct` on the test frame: 2% to 22% of its shorter side.
int32_t planet_radius_at(int32_t pct) {
    const float share = 0.02f + 0.20f * static_cast<float>(pct) / 100.0f;
    return static_cast<int32_t>(std::lround(share * static_cast<float>(kFrameH)));
}

} // namespace

TEST_CASE("the planet grows with the print", "[screensaver][starfield_sim]") {
    SECTION("no print, no planet") {
        Sky sky(kNoPrint);
        sky.fly(3000);
        CHECK(sky.sim.planet().r == 0);
    }
    SECTION("its radius follows the progress, centred") {
        int32_t last = 0;
        for (const int32_t pct : {25, 50, 75}) {
            CAPTURE(pct);
            Sky sky(print_at(ClockMode::COUNTDOWN, pct));
            sky.step();
            const StarfieldPlanet& planet = sky.sim.planet();
            CHECK(planet.r == planet_radius_at(pct));
            CHECK(planet.cx == static_cast<int32_t>(kFrameW / 2));
            CHECK(planet.cy == static_cast<int32_t>(kFrameH / 2));
            const helix::ui::Rgb centre = sky.frame.at(planet.cx, planet.cy);
            CHECK(std::max({centre.r, centre.g, centre.b}) > 40);
            CHECK(planet.r > last);
            last = planet.r;
        }
    }
    SECTION("it eases to a new size, and once there a frame leaves it alone") {
        Sky sky(print_at(ClockMode::COUNTDOWN, 20));
        sky.step();
        const int32_t from = sky.sim.planet().r;
        sky.reading = print_at(ClockMode::COUNTDOWN, 80);
        sky.step();
        CHECK(sky.sim.planet().r > from);
        CHECK(sky.sim.planet().r < planet_radius_at(80));
        sky.fly(6000);
        CHECK(sky.sim.planet().r == planet_radius_at(80));
        CHECK(sky.uncovered == 0);

        const DirtyRect disc{
            sky.sim.planet().cx - sky.sim.planet().r, sky.sim.planet().cy - sky.sim.planet().r,
            sky.sim.planet().cx + sky.sim.planet().r, sky.sim.planet().cy + sky.sim.planet().r};
        sky.step();
        const bool redrawn =
            std::any_of(sky.dirty.begin(), sky.dirty.end(),
                        [&disc](const DirtyRect& r) { return r.area() >= disc.area(); });
        CHECK_FALSE(redrawn);
    }
    SECTION("a paused print slows the stars to 15%") {
        Sky sky(print_at(ClockMode::COUNTDOWN, 40));
        sky.reading = print_at(ClockMode::PAUSED, 40);
        sky.fly(2000);
        CHECK(sky.sim.speed_scale() == Catch::Approx(0.15f));
        StarfieldSim::Star& star = sky.sim.stars()[0];
        star.x = 0.5f;
        star.y = 0.5f;
        star.z = 0.8f;
        star.speed = 0.02f;
        sky.step(33);
        CHECK(star.z == Catch::Approx(0.8f - 0.02f * 0.15f));
        sky.reading = print_at(ClockMode::COUNTDOWN, 40);
        sky.fly(2000);
        CHECK(sky.sim.speed_scale() == 1.0f);
    }
}

TEST_CASE("stars never punch holes in the planet", "[screensaver][starfield_sim]") {
    Sky sky(print_at(ClockMode::COUNTDOWN, 75));
    sky.step();
    const StarfieldPlanet planet = sky.sim.planet();
    REQUIRE(planet.r == planet_radius_at(75));
    const std::vector<uint8_t> drawn = sky.frame.snapshot();

    // Every star flies across the disc and its limb.
    std::minstd_rand place(9);
    std::uniform_real_distribution<float> spread(-0.12f, 0.12f);
    std::uniform_real_distribution<float> depth(0.3f, 1.0f);
    for (auto& star : sky.sim.stars()) {
        star.x = spread(place);
        star.y = spread(place);
        star.z = depth(place);
    }
    size_t changed = 0;
    size_t holes = 0;
    for (int i = 0; i < 60; i++) {
        sky.step(helix::test::kUnevenFrameMs[i % 5]);
        const helix::ui::PixelWriter now(sky.frame.target);
        const helix::ui::PixelWriter then(FrameTarget{const_cast<uint8_t*>(drawn.data()),
                                                      sky.frame.target.stride, kFrameW, kFrameH,
                                                      sky.frame.target.format});
        for (int32_t y = planet.cy - planet.r; y <= planet.cy + planet.r; y++) {
            for (int32_t x = planet.cx - planet.r; x <= planet.cx + planet.r; x++) {
                if (planet.covers(x, y) && !(now.get(x, y) == then.get(x, y))) {
                    holes++;
                }
            }
        }
        changed += sky.dirty.size();
    }
    CHECK(changed > 0);
    CHECK(holes == 0);
    CHECK(sky.sim.planet() == planet);
}

TEST_CASE("a finished print arrives at the planet", "[screensaver][starfield_sim]") {
    const size_t all = static_cast<size_t>(kFrameW) * kFrameH;
    SECTION("it grows to fill the view over four seconds and holds") {
        Sky sky(print_at(ClockMode::COUNTDOWN, 90));
        sky.step();
        const int32_t from = sky.sim.planet().r;
        sky.reading = print_at(ClockMode::DONE, 100);
        sky.fly(2000);
        CHECK(sky.sim.planet().r > from);
        CHECK(helix::test::lit_pixels(sky.frame.target) < all);
        sky.fly(2200);
        CHECK(helix::test::lit_pixels(sky.frame.target) == all);
        const StarfieldPlanet arrived = sky.sim.planet();
        sky.fly(3000);
        CHECK(sky.sim.planet() == arrived);
        CHECK(sky.dirty.empty());
        CHECK(sky.uncovered == 0);
    }
    SECTION("a sky started after the print finished opens on the planet") {
        Sky sky(print_at(ClockMode::DONE, 100));
        sky.step();
        CHECK(helix::test::lit_pixels(sky.frame.target) == all);
    }
}

TEST_CASE("a failed print's planet shrinks and drifts away", "[screensaver][starfield_sim]") {
    const ClockMode mode = GENERATE(ClockMode::FAILED, ClockMode::CANCELLED);
    CAPTURE(static_cast<int>(mode));
    SECTION("over six seconds") {
        Sky sky(print_at(ClockMode::COUNTDOWN, 50));
        sky.step();
        const StarfieldPlanet from = sky.sim.planet();
        sky.reading = print_at(mode, 50);
        sky.fly(3000);
        CHECK(sky.sim.planet().r < from.r);
        CHECK(sky.sim.planet().r > 0);
        CHECK(sky.sim.planet().cx != from.cx);
        sky.fly(3200);
        CHECK(sky.sim.planet().r == 0);
        CHECK(sky.uncovered == 0);
        // Only stars are left: a sliver of the frame.
        CHECK(helix::test::lit_pixels(sky.frame.target) < kFrameW * kFrameH / 50);
    }
    SECTION("a sky started after the print failed shows no planet") {
        Sky sky(print_at(mode, 50));
        sky.fly(1000);
        CHECK(sky.sim.planet().r == 0);
    }
}

// ============================================================================
// StarfieldScreensaver: stepping and invalidating in the timer callback
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a starfield frame invalidates only what its step changed",
                 "[screensaver][starfield_sim]") {
    StarfieldScreensaver ss;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    // Stars near the centre change a few pixels, far less than the whole canvas.
    StarAccess::place_stars(ss, 0.02f, -0.02f, 0.9f);
    lv_obj_t* canvas_obj = SaverTestAccess::canvas(ss);
    lv_obj_update_layout(canvas_obj);
    lv_area_t coords;
    lv_obj_get_coords(canvas_obj, &coords);
    const int64_t canvas_px = lv_area_get_size(&coords);

    helix::test::InvalidatedAreas invalidated(lv_obj_get_display(canvas_obj));
    advance_saver(ss, 33);

    // The frame erases and redraws the star cluster, so it invalidates, and every area
    // it invalidates is a fraction of the canvas.
    REQUIRE_FALSE(invalidated.areas.empty());
    for (const lv_area_t& a : invalidated.areas) {
        CHECK(lv_area_get_size(&a) < canvas_px);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a starfield level thins the stars and the areas a frame invalidates",
                 "[screensaver][starfield_sim]") {
    StarfieldScreensaver ss;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ss.level_count() == 4);
    REQUIRE(StarAccess::active_star_count(ss) == 150);
    lv_obj_t* canvas_obj = SaverTestAccess::canvas(ss);
    // Invalidations are clipped to the object's coords, which a layout pass computes.
    lv_obj_update_layout(canvas_obj);
    helix::test::InvalidatedAreas invalidated(lv_obj_get_display(canvas_obj));

    // Stars drawn on frame at a fixed depth, so a frame's area count is its star count.
    StarAccess::place_stars(ss, 0.02f, -0.02f, 0.9f);
    const auto step_frame = [&]() {
        invalidated.areas.clear();
        advance_saver(ss, 33);
    };

    step_frame();
    // One area per star, never the single spanning box a scattered sky would force.
    CHECK(invalidated.areas.size() == 150);
    CHECK(invalidated.areas.size() <= helix::ui::kSaverMaxDirtyAreas);

    const int level_stars[] = {150, 96, 64, 32};
    for (size_t level = 1; level < ss.level_count(); level++) {
        CAPTURE(level);
        ss.request_level(level);
        CHECK(StarAccess::active_star_count(ss) == level_stars[level]);
        step_frame(); // the stars the rung parks erase here
        step_frame();
        CHECK(invalidated.areas.size() == static_cast<size_t>(level_stars[level]));
        CHECK(invalidated.areas.size() <= helix::ui::kSaverMaxDirtyAreas);
    }
    CHECK(ss.level() == 3);
}

#endif // HELIX_ENABLE_SCREENSAVER
