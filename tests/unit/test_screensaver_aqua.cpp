// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_aqua.h"
#include "screensaver_aqua_sim.h"
#include "screensaver_fast_math.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_registry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::ui {

/// Moves an AquaSim's clocks, for runs far longer than a test can step through.
class AquaSimTestAccess {
  public:
    static void skip_to(AquaSim& sim, double t_sim) {
        sim.t_sim_ = t_sim;
        sim.water_.t_ = t_sim;
        sim.water_.next_impulse_ = t_sim + 0.6;
        sim.water_.impulses_.clear();
    }

    /// Paints the water of `sim` as it stands, its shading using `calm`.
    static void paint_at_calm(AquaSim& sim, FrameTarget& frame, float calm) {
        sim.calm_ = calm;
        sim.paint_water(frame);
    }

    /// The boxes one droplet and one glint at the same place draw, in that order.
    static std::vector<DirtyRect> spray_boxes(AquaSim& sim, FrameTarget& frame,
                                              const AquaDroplet& drop) {
        sim.splashes_.emit(drop);
        sim.glints_.push_back({drop.x, drop.y, drop.z, 0});
        std::vector<DirtyRect> boxes;
        sim.draw_spray(frame, boxes);
        return boxes;
    }

    /// Each bubble's row and speed.
    static std::vector<std::pair<float, float>> bubbles(const AquaSim& sim) {
        std::vector<std::pair<float, float>> out;
        for (const auto& b : sim.bubbles_) {
            out.emplace_back(b.y, b.speed);
        }
        return out;
    }
};

/// Reads the text line an AquaScreensaver owns.
class AquaScreensaverTestAccess {
  public:
    static const SaverLabel& line(const AquaScreensaver& saver) {
        return saver.line_;
    }
};

} // namespace helix::ui

using helix::ui::AquaScreensaver;
using helix::ui::AquaScreensaverTestAccess;
using helix::ui::AquaSimTestAccess;
using helix::ui::ClockInput;
using helix::ui::ClockPrintPhase;
using helix::ui::SaverTestAccess;

using helix::ui::aqua_gerstner_height;
using helix::ui::aqua_rings;
using helix::ui::AquaClockLayout;
using helix::ui::AquaDroplet;
using helix::ui::AquaLanding;
using helix::ui::AquaRings;
using helix::ui::AquaSim;
using helix::ui::AquaSplashes;
using helix::ui::AquaWater;
using helix::ui::AquaWave;
using helix::ui::clock_time;
using helix::ui::ClockMode;
using helix::ui::ClockReading;
using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::kAquaDepth;
using helix::ui::kAquaLevelCount;
using helix::ui::kAquaLevels;
using helix::ui::kAquaMaxDroplets;
using helix::ui::kAquaRimFloor;
using helix::ui::kAquaZNear;
using helix::ui::layout_aqua_clock;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

using helix::test::bytes_differing;
using helix::test::check_replays;
using helix::test::diff_against;
using helix::test::reading_at;
using helix::test::TestFrame;

namespace {

/// A calm tank: a grid that was never evaluated is flat at height 0.
AquaWater calm_water() {
    AquaWater water;
    std::minstd_rand rng(1);
    water.init(rng, 40, 16, 0.42f);
    return water;
}

} // namespace

// ============================================================================
// AquaWater
// ============================================================================

TEST_CASE("the tank modes keep the dispersion relation", "[screensaver][aqua]") {
    AquaWater water;
    std::minstd_rand rng(3);
    water.init(rng, 20, 8, 0.4f);
    for (size_t n = 0; n < water.modes().size(); n++) {
        const auto& m = water.modes()[n];
        const float k = static_cast<float>(n + 1) * helix::ui::kSaverPi / 7.0f;
        CHECK(m.k == Catch::Approx(k));
        CHECK(m.omega * m.omega == Catch::Approx(9.81f * k * std::tanh(1.2f * k)).margin(1e-4));
    }
}

TEST_CASE("Gerstner crests stay peaked", "[screensaver][aqua]") {
    const float k = 2.0f * helix::ui::kSaverPi / 2.6f;
    const float omega = std::sqrt(9.81f * k * std::tanh(1.2f * k));
    const auto widths = [&](float steep) {
        const AquaWave wave{0.13f, k, omega, 1.0f, 0.0f, steep, 0.0f};
        int crest = 0;
        int trough = 0;
        for (int i = 0; i < 2000; i++) {
            const float x = 2.6f * static_cast<float>(i) / 2000.0f;
            const float h = aqua_gerstner_height(wave, x, 5.0f, 0.0f);
            crest += h > 0.065f ? 1 : 0;
            trough += h < -0.065f ? 1 : 0;
        }
        return std::make_pair(crest, trough);
    };
    const auto steep = widths(0.9f);
    CHECK(static_cast<float>(steep.first) < 0.8f * static_cast<float>(steep.second));
    const auto flat = widths(0.0f);
    CHECK(static_cast<float>(flat.first) ==
          Catch::Approx(static_cast<float>(flat.second)).epsilon(0.02));
}

TEST_CASE("the grid samples the analytic surface", "[screensaver][aqua]") {
    AquaWater water;
    std::minstd_rand rng(3);
    const float u_max = (200.0f + 8.0f) / 500.0f;
    water.init(rng, 160, 64, u_max);
    const float t = 7.3f;
    water.evaluate(t, rng);
    std::minstd_rand pick(9);
    for (int i = 0; i < 50; i++) {
        const float z =
            kAquaZNear + 0.05f + (kAquaDepth - 0.1f) * helix::ui::screensaver::unit_random(pick);
        const float u = (helix::ui::screensaver::unit_random(pick) * 2.0f - 1.0f) * u_max * 0.95f;
        const float x = u * z;
        CAPTURE(x, z);
        CHECK(water.height_at(x, z) == Catch::Approx(water.exact_height(x, z, t)).margin(0.01));
    }
}

TEST_CASE("amplitude zero flattens the slosh and the waves", "[screensaver][aqua]") {
    AquaWater water;
    std::minstd_rand rng(3);
    water.init(rng, 40, 16, 0.416f);
    // Before the first impulse, at 0.6 sim seconds: only the modes and waves are left to flatten.
    const double t = 0.3;
    const auto tallest_cell = [&water] {
        float tallest = 0.0f;
        for (const float h : water.heights()) {
            tallest = std::max(tallest, std::fabs(h));
        }
        return tallest;
    };
    water.evaluate(t, rng);
    REQUIRE(tallest_cell() > 0.01f);
    water.set_amplitude(0.0f);
    water.evaluate(t, rng);
    CHECK(tallest_cell() == Catch::Approx(0.0f).margin(1e-5));
    for (int k = 0; k < water.grid_z(); k++) {
        for (int i = 0; i < water.grid_x(); i++) {
            const float z = water.cell_z(k);
            CHECK(water.exact_height(water.cell_u(i) * z, z, t) ==
                  Catch::Approx(0.0f).margin(1e-5));
        }
    }
}

TEST_CASE("impulses start behind the near edge and expire after 12 sim seconds",
          "[screensaver][aqua]") {
    AquaWater water;
    std::minstd_rand rng(5);
    water.init(rng, 20, 8, 0.4f);
    std::vector<double> starts;
    for (float t = 0.0f; t < 120.0f; t += 0.05f) {
        water.evaluate(t, rng);
        for (const auto& e : water.impulses()) {
            CHECK(e.z >= 4.7f - 1e-4f);
            CHECK(e.z <= 5.9f + 1e-4f);
            CHECK(t - e.t0 < 12.0f);
            if (std::find(starts.begin(), starts.end(), e.t0) == starts.end()) {
                starts.push_back(e.t0);
            }
        }
    }
    REQUIRE(starts.size() > 20);
    std::sort(starts.begin(), starts.end());
    CHECK(starts.front() == Catch::Approx(0.6f));
    for (size_t i = 1; i < starts.size(); i++) {
        const auto gap = static_cast<float>(starts[i] - starts[i - 1]);
        CHECK(gap >= 2.0f - 1e-4f);
        CHECK(gap <= 4.0f + 1e-4f);
    }
}

TEST_CASE("evaluate allocates nothing", "[screensaver][aqua]") {
    AquaWater water;
    std::minstd_rand rng(3);
    water.init(rng, 160, 64, 0.416f);
    water.evaluate(0.0f, rng);
    const size_t heap = water.heap_bytes();
    for (int i = 1; i <= 100; i++) {
        water.evaluate(static_cast<float>(i) * 0.3f, rng);
    }
    CHECK(water.heap_bytes() == heap);
    water.clear();
    CHECK(water.heap_bytes() == 0);
}

// ============================================================================
// AquaSplashes: Preston's droplet rule
// ============================================================================

TEST_CASE("a droplet is removed only once it is below the surface at its own position",
          "[screensaver][aqua][droplets]") {
    AquaWater water;
    std::minstd_rand rng(3);
    water.init(rng, 40, 16, 0.416f);
    AquaSplashes splashes;
    splashes.init(kAquaMaxDroplets);
    std::vector<AquaLanding> landed;
    landed.reserve(kAquaMaxDroplets);
    size_t landings = 0;
    for (int i = 0; i < 24000; i++) {
        const float t = static_cast<float>(i) * 0.0125f;
        water.evaluate(t, rng);
        landed.clear();
        splashes.step(0.0125f, water, rng, &landed);
        for (const AquaLanding& l : landed) {
            CHECK(l.y < l.surface);
        }
        landings += landed.size();
        for (const AquaDroplet& d : splashes.droplets()) {
            if (d.y < water.height_at(d.x, d.z)) {
                FAIL("a droplet below the surface is still flying");
            }
        }
    }
    CHECK(splashes.emitted() > 10);
    CHECK(landings > 0);
}

TEST_CASE("a droplet in flight never expires on a timer", "[screensaver][aqua][droplets]") {
    AquaWater water = calm_water();
    AquaSplashes splashes;
    splashes.init(kAquaMaxDroplets);
    AquaDroplet drop;
    drop.y = 0.5f;
    drop.z = 5.0f;
    drop.vy = 5.0f;
    REQUIRE(splashes.emit(drop));
    float t = 0.0f;
    bool landed = false;
    std::vector<AquaLanding> landings;
    while (t < 10.0f) {
        const bool was_up = splashes.droplets().size() == 1;
        landings.clear();
        splashes.move(0.01f, water, &landings);
        t += 0.01f;
        if (!splashes.droplets().empty()) {
            CHECK(splashes.droplets()[0].y >= 0.0f);
        } else {
            REQUIRE(was_up);
            REQUIRE(landings.size() == 1);
            CHECK(landings[0].y < 0.0f);
            landed = true;
            break;
        }
        if (t > 2.49f && t < 2.51f) {
            CHECK(splashes.droplets().size() == 1);
        }
    }
    CHECK(landed);
    CHECK(t > 2.5f);
}

TEST_CASE("the cap limits emission and never culls", "[screensaver][aqua][droplets]") {
    AquaWater water = calm_water();
    AquaSplashes splashes;
    splashes.init(kAquaMaxDroplets);
    std::minstd_rand rng(4);
    AquaDroplet high;
    high.y = 3.0f;
    high.z = 5.0f;
    for (size_t i = 0; i < kAquaMaxDroplets; i++) {
        CHECK(splashes.emit(high));
    }
    CHECK_FALSE(splashes.emit(high));
    CHECK(splashes.droplets().size() == kAquaMaxDroplets);
    splashes.step(0.01f, water, rng, nullptr);
    CHECK(splashes.droplets().size() == kAquaMaxDroplets);
    CHECK(splashes.emitted() == kAquaMaxDroplets);

    AquaWater real;
    std::minstd_rand wave_rng(3);
    real.init(wave_rng, 40, 16, 0.416f);
    AquaSplashes live;
    live.init(kAquaMaxDroplets);
    std::vector<AquaLanding> landed;
    landed.reserve(kAquaMaxDroplets);
    size_t total_landed = 0;
    for (int i = 0; i < 16000; i++) {
        real.evaluate(static_cast<float>(i) * 0.0125f, wave_rng);
        landed.clear();
        live.step(0.0125f, real, wave_rng, &landed);
        total_landed += landed.size();
        REQUIRE(live.droplets().size() <= kAquaMaxDroplets);
        REQUIRE(live.emitted() == live.droplets().size() + total_landed);
    }
}

TEST_CASE("droplets stay inside the tank depth", "[screensaver][aqua][droplets]") {
    AquaWater water = calm_water();
    AquaSplashes splashes;
    splashes.init(kAquaMaxDroplets);
    for (int i = 0; i < 20; i++) {
        AquaDroplet d;
        d.y = 1.0f;
        d.z = kAquaZNear + 0.2f * static_cast<float>(i);
        d.vy = 4.0f;
        d.vz = (i % 2 == 0 ? 1.0f : -1.0f) * 3.0f;
        REQUIRE(splashes.emit(d));
    }
    for (int i = 0; i < 600; i++) {
        splashes.move(0.01f, water, nullptr);
        for (const AquaDroplet& d : splashes.droplets()) {
            CHECK(d.z >= kAquaZNear);
            CHECK(d.z <= kAquaZNear + kAquaDepth);
        }
    }
}

// ============================================================================
// Rings and layout
// ============================================================================

TEST_CASE("the rings fill through the day, the hour and the minute", "[screensaver][aqua][clock]") {
    const int32_t t1 = 10 * 3600 + 30 * 60 + 15;
    AquaRings r = aqua_rings(reading_at(t1));
    CHECK(r.hour == Catch::Approx(0.87535f).margin(1e-4));
    CHECK(r.minute == Catch::Approx(0.50417f).margin(1e-4));
    CHECK(r.top == Catch::Approx(0.25f).margin(1e-4));
    const int32_t t2 = 22 * 3600 + 30 * 60 + 15;
    CHECK(aqua_rings(reading_at(t2, true)).hour == Catch::Approx(0.93767f).margin(1e-4));
    CHECK(aqua_rings(reading_at(t2, false)).hour == Catch::Approx(0.87535f).margin(1e-4));
    r = aqua_rings(reading_at(0));
    CHECK(r.hour == 0.0f);
    CHECK(r.minute == 0.0f);
    CHECK(r.top == 0.0f);
}

TEST_CASE("during a print the top ring is progress", "[screensaver][aqua][clock]") {
    ClockReading r = reading_at(10 * 3600 + 30 * 60 + 15);
    r.mode = ClockMode::COUNTDOWN;
    r.progress_pct = 42;
    AquaRings rings = aqua_rings(r);
    CHECK(rings.top == Catch::Approx(0.42f));
    CHECK(rings.hour == Catch::Approx(0.87535f).margin(1e-4));
    CHECK(rings.minute == Catch::Approx(0.50417f).margin(1e-4));
    r.mode = ClockMode::PAUSED;
    CHECK(aqua_rings(r).top == Catch::Approx(0.42f));
    r.mode = ClockMode::DONE;
    r.progress_pct = 100;
    CHECK(aqua_rings(r).top == Catch::Approx(1.0f));
}

TEST_CASE("the water stands higher as the print goes", "[screensaver][aqua][clock]") {
    using helix::ui::aqua_level_target;
    using helix::ui::kAquaBrimRise;
    ClockReading r = reading_at(9 * 3600);
    CHECK(aqua_level_target(r) == 0.0f);
    r.mode = ClockMode::PROGRESS;
    r.progress_pct = 0;
    CHECK(aqua_level_target(r) == 0.0f);
    r.mode = ClockMode::COUNTDOWN;
    r.progress_pct = 50;
    CHECK(aqua_level_target(r) == Catch::Approx(kAquaBrimRise * 0.5f));
    r.progress_pct = 100;
    CHECK(aqua_level_target(r) == Catch::Approx(kAquaBrimRise));
    r.mode = ClockMode::PAUSED;
    r.progress_pct = 50;
    CHECK(aqua_level_target(r) == Catch::Approx(kAquaBrimRise * 0.5f));
    r.mode = ClockMode::DONE;
    CHECK(aqua_level_target(r) == Catch::Approx(kAquaBrimRise));
    r.mode = ClockMode::FAILED;
    CHECK(aqua_level_target(r) == 0.0f);
    r.mode = ClockMode::CANCELLED;
    CHECK(aqua_level_target(r) == 0.0f);
    CHECK(kAquaBrimRise < 0.8f);
}

namespace {

/// Water pixels (not black) in the top half of an 800x480 aqua frame after 30 frames of `r`.
size_t water_above_middle(const ClockReading& r) {
    TestFrame frame(800, 480, PixelFormat::XRGB8888);
    std::minstd_rand rng(7);
    AquaSim sim;
    sim.init(frame.target, rng, 1, r);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < 30; i++) {
        sim.step(33, frame.target, rng, r, dirty);
    }
    const PixelWriter writer(frame.target);
    size_t n = 0;
    for (int32_t y = 0; y < 240; y++) {
        for (int32_t x = 0; x < 800; x++) {
            const Rgb c = writer.get(x, y);
            n += (c.r + c.g + c.b) > 12 ? 1 : 0;
        }
    }
    return n;
}

/// Bytes per frame that move by more than the water's shimmer, over 60 frames of `r` at rung 1:
/// a measure of motion, not of the fine per-frame flicker every rung keeps.
double frame_change(const ClockReading& r) {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(5);
    AquaSim sim;
    sim.init(frame.target, rng, 1, r);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < 10; i++) {
        sim.step(33, frame.target, rng, r, dirty);
    }
    size_t changed = 0;
    for (int i = 0; i < 60; i++) {
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(33, frame.target, rng, r, dirty);
        for (size_t b = 0; b < before.size(); b++) {
            changed += std::abs(before[b] - frame.bytes[b]) > 6 ? 1 : 0;
        }
    }
    return static_cast<double>(changed) / 60.0;
}

} // namespace

TEST_CASE("the water line rises with progress", "[screensaver][aqua][clock]") {
    ClockReading r = reading_at(9 * 3600);
    r.mode = ClockMode::COUNTDOWN;
    r.progress_pct = 0;
    const size_t low = water_above_middle(r);
    r.progress_pct = 100;
    const size_t high = water_above_middle(r);
    // Ten more rows of water across the screen, at least: the far water is dim enough that
    // part of the rise does not count.
    CHECK(high > low + 800 * 10);
}

TEST_CASE("paused water calms", "[screensaver][aqua][clock]") {
    ClockReading r = reading_at(9 * 3600);
    r.mode = ClockMode::COUNTDOWN;
    r.progress_pct = 40;
    const double running = frame_change(r);
    r.mode = ClockMode::PAUSED;
    const double paused = frame_change(r);
    REQUIRE(running > 0.0);
    CHECK(paused < running * 0.8);
}

TEST_CASE("calm water loses its fine wrinkles along with its waves", "[screensaver][aqua][clock]") {
    TestFrame base_frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(5);
    AquaSim base;
    const ClockReading r = reading_at(9 * 3600);
    base.init(base_frame.target, rng, 1, r);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < 10; i++) {
        base.step(33, base_frame.target, rng, r, dirty);
    }
    const auto painted = [&base](float calm) {
        AquaSim copy = base;
        TestFrame frame(400, 240, PixelFormat::XRGB8888);
        AquaSimTestAccess::paint_at_calm(copy, frame.target, calm);
        return frame.bytes;
    };
    const auto distance = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        double sum = 0.0;
        for (size_t i = 0; i < a.size(); i++) {
            sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        }
        return sum;
    };
    const std::vector<uint8_t> glassy = painted(0.0f);
    const double at_full = distance(painted(1.0f), glassy);
    const double at_calm = distance(painted(helix::ui::kAquaCalm), glassy);
    REQUIRE(at_full > 0.0);
    CHECK(at_calm > 0.0);
    CHECK(at_calm < at_full);
}

TEST_CASE("spray sits where the raised camera sees it", "[screensaver][aqua][droplets]") {
    const auto boxes_at = [](int progress_pct) {
        TestFrame frame(800, 480, PixelFormat::XRGB8888);
        std::minstd_rand rng(3);
        AquaSim sim;
        ClockReading r = reading_at(9 * 3600);
        r.mode = ClockMode::COUNTDOWN;
        r.progress_pct = progress_pct;
        sim.init(frame.target, rng, 1, r);
        AquaDroplet drop;
        drop.y = 0.5f;
        drop.z = 5.0f;
        return AquaSimTestAccess::spray_boxes(sim, frame.target, drop);
    };
    const std::vector<DirtyRect> rest = boxes_at(0);
    const std::vector<DirtyRect> raised = boxes_at(100);
    REQUIRE(rest.size() == 2);
    REQUIRE(raised.size() == 2);
    // Focal length 1000 px at this canvas, over the droplet's depth.
    const auto rows = static_cast<int32_t>(helix::ui::kAquaBrimRise * 1000.0f / 5.0f);
    for (size_t i = 0; i < rest.size(); i++) {
        CAPTURE(i);
        CHECK(rest[i].y1 - raised[i].y1 == Catch::Approx(rows).margin(1));
    }
}

TEST_CASE("a finished print sends one burst of bubbles up from the bottom",
          "[screensaver][aqua][clock]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(5);
    AquaSim sim;
    ClockReading r = reading_at(9 * 3600);
    r.mode = ClockMode::COUNTDOWN;
    r.progress_pct = 99;
    sim.init(frame.target, rng, 1, r);
    std::vector<DirtyRect> dirty;
    sim.step(33, frame.target, rng, r, dirty);
    const auto sent = [&sim] {
        size_t n = 0;
        for (const auto& [y, speed] : AquaSimTestAccess::bubbles(sim)) {
            n += y >= 239.0f && speed == 0.6f ? 1 : 0;
        }
        return n;
    };
    REQUIRE(sent() == 0);

    r.mode = ClockMode::DONE;
    r.progress_pct = 100;
    sim.step(33, frame.target, rng, r, dirty);
    REQUIRE(AquaSimTestAccess::bubbles(sim).size() >= helix::ui::kAquaDoneBurst);
    CHECK(sent() == helix::ui::kAquaDoneBurst);
    const auto burst = AquaSimTestAccess::bubbles(sim);

    sim.step(33, frame.target, rng, r, dirty);
    const auto later = AquaSimTestAccess::bubbles(sim);
    for (size_t i = 0; i < helix::ui::kAquaDoneBurst; i++) {
        CAPTURE(i);
        CHECK(later[i].first < burst[i].first);
    }
}

TEST_CASE("the still rung repaints the water only per level step", "[screensaver][aqua][clock]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    AquaSim sim;
    ClockReading r = reading_at(9 * 3600);
    r.mode = ClockMode::COUNTDOWN;
    r.progress_pct = 0;
    sim.init(frame.target, rng, helix::ui::kAquaLevelCount - 1, r);
    std::vector<DirtyRect> dirty;
    const DirtyRect whole = DirtyRect::whole(400, 240);
    size_t full = 0;
    // A whole print at the still rung's 250 ms frames, 100 frames per percent.
    for (int pct = 0; pct <= 100; pct++) {
        r.progress_pct = pct;
        for (int i = 0; i < 100; i++) {
            sim.step(250, frame.target, rng, r, dirty);
            full += std::count(dirty.begin(), dirty.end(), whole) > 0 ? 1 : 0;
        }
    }
    CHECK(full >= 5);
    CHECK(full <=
          static_cast<size_t>(helix::ui::kAquaBrimRise / helix::ui::kAquaStillLevelStep) + 2);

    // The print ends and the printer goes idle: the water drops back in one repaint, not one
    // per step on the way down.
    r.mode = ClockMode::DONE;
    for (int i = 0; i < 40; i++) {
        sim.step(250, frame.target, rng, r, dirty);
    }
    r.mode = ClockMode::CLOCK;
    size_t after = 0;
    for (int i = 0; i < 400; i++) {
        sim.step(250, frame.target, rng, r, dirty);
        after += std::count(dirty.begin(), dirty.end(), whole) > 0 ? 1 : 0;
    }
    CHECK(after <= 1);
}

TEST_CASE("the clock fits in the water at every drift and screen size",
          "[screensaver][aqua][clock]") {
    struct Size {
        int32_t w, h;
    };
    for (const Size s : {Size{800, 480}, Size{480, 320}, Size{1024, 600}, Size{480, 800}}) {
        CAPTURE(s.w, s.h);
        const int32_t reach_x = std::max(4, s.w / 25);
        const int32_t reach_y = std::max(4, s.h / 25);
        for (int32_t dx : {-reach_x, 0, reach_x}) {
            for (int32_t dy : {-reach_y, 0, reach_y}) {
                const AquaClockLayout l = layout_aqua_clock(s.w, s.h, dx, dy);
                CAPTURE(dx, dy);
                CHECK(l.bounds.x1 >= 0);
                CHECK(l.bounds.y1 >= 0);
                CHECK(l.bounds.x2 < s.w);
                CHECK(l.bounds.y2 < s.h);
                CHECK(l.text.y1 > l.hour_cy + l.ring_r);
                CHECK(l.top_cy + l.top_r < l.hour_cy - l.ring_r);
                CHECK(l.hour_cx + l.ring_r < l.minute_cx - l.ring_r);
                // Two digits side by side fit inside a ring.
                const int32_t digits_w =
                    2 * static_cast<int32_t>(std::lround(1.06f * l.digit_unit));
                CHECK(digits_w < 2 * l.ring_r - 2 * l.ring_width);
            }
        }
    }
}

// ============================================================================
// AquaSim
// ============================================================================

TEST_CASE("an aqua run replays exactly from a seed, and another seed differs",
          "[screensaver][aqua]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const auto run = [format](uint32_t seed) {
        TestFrame frame(320, 192, format);
        std::minstd_rand rng(seed);
        AquaSim sim;
        const ClockReading r = reading_at(9 * 3600);
        sim.init(frame.target, rng, 1, r);
        std::vector<DirtyRect> dirty;
        for (int i = 0; i < 60; i++) {
            sim.step(33, frame.target, rng, r, dirty);
        }
        return frame.bytes;
    };
    check_replays(run, 3, 4);
}

TEST_CASE("every pixel an aqua frame changes lies in one of its dirty boxes",
          "[screensaver][aqua]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    TestFrame frame(400, 240, format);
    std::minstd_rand rng(3);
    AquaSim sim;
    int32_t sod = 9 * 3600 + 59 * 60 + 50;
    sim.init(frame.target, rng, 1, reading_at(sod));
    std::vector<DirtyRect> dirty;
    size_t changed = 0;
    size_t uncovered = 0;
    for (int i = 0; i < 60; i++) {
        sod += i % 30 == 0 ? 1 : 0;
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(33, frame.target, rng, reading_at(sod), dirty);
        const auto d = diff_against(before, frame, dirty);
        changed += d.changed;
        uncovered += d.uncovered;
    }
    REQUIRE(changed > 0);
    CHECK(uncovered == 0);
    CHECK(frame.padding_intact());
}

TEST_CASE("the near-edge rim is always lit", "[screensaver][aqua]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    AquaSim sim;
    const ClockReading r = reading_at(9 * 3600);
    sim.init(frame.target, rng, 1, r);
    std::vector<DirtyRect> dirty;
    const PixelWriter writer(frame.target);
    const DirtyRect& clock = sim.clock_layout().bounds;
    // The pixel holding the edge sits within half a row of the line's centre, where the line
    // is at least 0.7 of its brightness; the grey ramp and the cool tint bring red down to
    // this.
    const int floor_red =
        static_cast<int>(std::pow(0.7f * kAquaRimFloor, 1.05f) * 0.94f * 255.0f) - 2;
    size_t lit_checked = 0;
    for (int i = 0; i < 400; i++) {
        sim.step(50, frame.target, rng, r, dirty);
        for (int32_t x = 0; x < 400; x += 7) {
            CHECK(sim.rim_brightness(x) >= kAquaRimFloor);
            const float e = sim.rim_row(x);
            const auto y = static_cast<int32_t>(std::floor(e));
            if (y < 0 || y >= 240 ||
                (x >= clock.x1 && x <= clock.x2 && y >= clock.y1 && y <= clock.y2)) {
                continue;
            }
            CHECK(writer.get(x, y).r >= floor_red);
            lit_checked++;
        }
    }
    CHECK(lit_checked > 1000);
}

TEST_CASE("nothing draws above the water when the splashes are off", "[screensaver][aqua]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    AquaSim sim;
    const ClockReading r = reading_at(9 * 3600);
    sim.init(frame.target, rng, 3, r);
    REQUIRE_FALSE(kAquaLevels[3].splashes);
    std::vector<DirtyRect> dirty;
    const PixelWriter writer(frame.target);
    const DirtyRect& clock = sim.clock_layout().bounds;
    const int32_t scale = kAquaLevels[3].scale;
    size_t lit_above = 0;
    for (int i = 0; i < 100; i++) {
        sim.step(100, frame.target, rng, r, dirty);
        if (!sim.splashes().droplets().empty()) {
            continue;
        }
        for (int32_t x = 0; x < 400; x++) {
            const float top = sim.renderer().top_at(std::min(x / scale, 99)) * scale;
            const int32_t above = static_cast<int32_t>(std::floor(top)) - 2 * scale - 1;
            for (int32_t y = 0; y < above; y++) {
                if (x >= clock.x1 && x <= clock.x2 && y >= clock.y1 && y <= clock.y2) {
                    continue;
                }
                const Rgb c = writer.get(x, y);
                lit_above += (c.r | c.g | c.b) != 0 ? 1 : 0;
            }
        }
    }
    CHECK(lit_above == 0);
}

/// Mean |grey change| between consecutive 50 ms frames over the water's body: pixels the
/// surface covers in both frames, clear of its silhouette, the near edge and the clock.
double shimmer(uint32_t seed) {
    TestFrame frame(800, 480, PixelFormat::XRGB8888);
    std::minstd_rand rng(seed);
    AquaSim sim;
    const ClockReading r = reading_at(9 * 3600);
    sim.init(frame.target, rng, 1, r);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < 40; i++) {
        sim.step(50, frame.target, rng, r, dirty);
    }
    const PixelWriter writer(frame.target);
    const DirtyRect& clock = sim.clock_layout().bounds;
    const auto grey = [&](int32_t x, int32_t y) {
        const Rgb c = writer.get(x, y);
        return (c.r + c.g + c.b) / 3.0;
    };
    double sum = 0.0;
    size_t samples = 0;
    std::vector<double> before(800 * 480);
    std::vector<uint8_t> covered(400 * 240);
    for (int pair = 0; pair < 20; pair++) {
        for (int32_t y = 0; y < 240; y++) {
            for (int32_t x = 0; x < 400; x++) {
                covered[y * 400 + x] = sim.renderer().coverage_at(x, y) == 255 ? 1 : 0;
            }
        }
        for (int32_t y = 0; y < 480; y++) {
            for (int32_t x = 0; x < 800; x++) {
                before[y * 800 + x] = grey(x, y);
            }
        }
        sim.step(50, frame.target, rng, r, dirty);
        for (int32_t y = 0; y < 480; y++) {
            for (int32_t x = 0; x < 800; x++) {
                const int32_t ix = x / 2;
                const int32_t iy = y / 2;
                if (covered[iy * 400 + ix] == 0 || sim.renderer().coverage_at(ix, iy) != 255 ||
                    static_cast<float>(y) <= sim.renderer().top_at(ix) * 2.0f + 6.0f ||
                    static_cast<float>(y) >= sim.rim_row(x) - 2.0f ||
                    (x >= clock.x1 && x <= clock.x2 && y >= clock.y1 && y <= clock.y2)) {
                    continue;
                }
                sum += std::fabs(grey(x, y) - before[y * 800 + x]);
                samples++;
            }
        }
    }
    REQUIRE(samples > 10000);
    return sum / static_cast<double>(samples);
}

// The approved Python prototype measured 7.8 to 13.8 on about this metric over seeds 3 to 5;
// most of it is the highlights moving, not frame-to-frame sparkle. The bound is a regression
// tripwire at that level, not a comparison with the prototype.
TEST_CASE("the water's frame-to-frame change stays at the approved look's level",
          "[screensaver][aqua][shimmer]") {
    double total = 0.0;
    for (const uint32_t seed : {3u, 4u, 5u}) {
        const double m = shimmer(seed);
        WARN("aqua shimmer, seed " << seed << ": mean |grey change| per 50 ms frame = " << m);
        CHECK(m >= 0.3);
        total += m;
    }
    CHECK(total / 3.0 <= 11.5);
}

TEST_CASE("the ladder steps to coarser water and ends standing still", "[screensaver][aqua]") {
    CHECK(kAquaLevels[0].period_ms == helix::ui::kSaverFastPeriod);
    for (size_t i = 1; i < kAquaLevelCount; i++) {
        CHECK(kAquaLevels[i].period_ms >= kAquaLevels[i - 1].period_ms);
    }
    CHECK(kAquaLevels[kAquaLevelCount - 1].still);

    TestFrame frame(400, 240, PixelFormat::RGB565);
    std::minstd_rand rng(3);
    AquaSim sim;
    int32_t sod = 9 * 3600 + 15 * 60;
    sim.init(frame.target, rng, kAquaLevelCount - 1, reading_at(sod));
    std::vector<DirtyRect> dirty;
    sim.step(250, frame.target, rng, reading_at(sod), dirty);
    for (int i = 0; i < 10; i++) {
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(250, frame.target, rng, reading_at(sod), dirty);
        CHECK(dirty.empty());
        CHECK(bytes_differing(frame.bytes, before) == 0);
    }
    const DirtyRect clock = sim.clock_layout().bounds;
    const std::vector<uint8_t> before = frame.bytes;
    sim.step(250, frame.target, rng, reading_at(sod + 60), dirty);
    REQUIRE_FALSE(dirty.empty());
    for (const DirtyRect& d : dirty) {
        CHECK(d.x1 >= clock.x1);
        CHECK(d.y1 >= clock.y1);
        CHECK(d.x2 <= clock.x2);
        CHECK(d.y2 <= clock.y2);
    }
    const auto d = diff_against(before, frame, dirty);
    CHECK(d.changed > 0);
    CHECK(d.uncovered == 0);
}

TEST_CASE("emission follows time, not frame rate", "[screensaver][aqua][droplets]") {
    // Which crests qualify depends on the rate of rise each frame measures, so one seed's
    // count wanders a little between frame rates; summed over several it holds. Rung 2 is the
    // coarsest water that still splashes.
    const auto emitted = [](uint32_t period, uint32_t seed) {
        TestFrame frame(80, 48, PixelFormat::RGB565);
        std::minstd_rand rng(seed);
        AquaSim sim;
        const ClockReading r = reading_at(9 * 3600);
        sim.init(frame.target, rng, 2, r);
        std::vector<DirtyRect> dirty;
        for (uint32_t t = 0; t < 30000; t += period) {
            sim.step(period, frame.target, rng, r, dirty);
        }
        return static_cast<double>(sim.splashes().emitted());
    };
    double fast = 0.0;
    double slow = 0.0;
    for (const uint32_t seed : {3u, 4u, 5u, 6u}) {
        fast += emitted(33, seed);
        slow += emitted(100, seed);
    }
    REQUIRE(slow > 300.0);
    CHECK(fast == Catch::Approx(slow).epsilon(0.15));
    // A per-frame emission would throw about three times as many at 33 ms frames.
    CHECK(fast < 1.5 * slow);
}

/// Mean |byte change| between consecutive 50 ms frames of `sim`, over `frames` frames.
double mean_change(AquaSim& sim, TestFrame& frame, std::minstd_rand& rng, const ClockReading& r,
                   int frames) {
    std::vector<DirtyRect> dirty;
    double sum = 0.0;
    for (int i = 0; i < frames; i++) {
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(50, frame.target, rng, r, dirty);
        for (size_t b = 0; b < before.size(); b++) {
            sum += std::abs(static_cast<int>(before[b]) - static_cast<int>(frame.bytes[b]));
        }
    }
    return sum / static_cast<double>(frames) / static_cast<double>(frame.bytes.size());
}

TEST_CASE("aqua still moves smoothly thirty days into a run", "[screensaver][aqua]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    AquaSim sim;
    const ClockReading r = reading_at(9 * 3600);
    sim.init(frame.target, rng, 1, r);
    mean_change(sim, frame, rng, r, 40);
    const double early = mean_change(sim, frame, rng, r, 40);
    AquaSimTestAccess::skip_to(sim, 30.0 * 86400.0 * helix::ui::kAquaTimeScale);
    mean_change(sim, frame, rng, r, 40);
    const double late = mean_change(sim, frame, rng, r, 40);
    CAPTURE(early, late);
    REQUIRE(early > 0.05);
    CHECK(late > 0.5 * early);
    CHECK(late < 2.0 * early);
}

TEST_CASE("a drift step at the still rung repaints only the clock's old and new places",
          "[screensaver][aqua]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    AquaSim sim;
    const ClockReading r = reading_at(9 * 3600);
    sim.init(frame.target, rng, kAquaLevelCount - 1, r);
    std::vector<DirtyRect> dirty;
    const DirtyRect first = sim.clock_layout().bounds;
    bool stepped = false;
    for (uint32_t t = 0; t < 3 * helix::ui::kClockDriftStepMs && !stepped; t += 250) {
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(250, frame.target, rng, r, dirty);
        if (!(sim.clock_layout().bounds == first)) {
            stepped = true;
            int64_t area = 0;
            for (const DirtyRect& d : dirty) {
                area += d.area();
            }
            CHECK(area <= 2 * first.area());
            const auto d = diff_against(before, frame, dirty);
            CHECK(d.changed > 0);
            CHECK(d.uncovered == 0);
        } else if (t > 0) {
            CHECK(dirty.empty());
        }
    }
    CHECK(stepped);
}

TEST_CASE("aqua frames and step-downs never grow the heap, and the still rung keeps only the clock",
          "[screensaver][aqua]") {
    const size_t start = GENERATE(range(size_t{0}, kAquaLevelCount));
    CAPTURE(start);
    TestFrame frame(800, 480, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    AquaSim sim;
    const ClockReading r = reading_at(9 * 3600);
    sim.init(frame.target, rng, start, r);
    std::vector<DirtyRect> dirty;
    const size_t heap = sim.heap_bytes();
    CHECK(heap <= 1536u * 1024u);
    for (size_t level = start; level < kAquaLevelCount; level++) {
        sim.request_level(level);
        for (int i = 0; i < 20; i++) {
            sim.step(level + 1 == kAquaLevelCount ? 250 : 33, frame.target, rng, r, dirty);
            CHECK(sim.heap_bytes() <= heap);
        }
    }
    // The still rung has painted its scene and holds just what redraws the clock.
    const size_t still = sim.heap_bytes();
    if (start + 1 < kAquaLevelCount) {
        CHECK(still * 2 < heap);
    }

    // A finer rung asked for afterwards builds the scene again and draws it whole.
    sim.request_level(1);
    sim.step(33, frame.target, rng, r, dirty);
    CHECK(sim.heap_bytes() > still);
    CHECK(dirty.size() == 1);
    CHECK(dirty[0] == DirtyRect::whole(800, 480));
    sim.clear();
    CHECK(sim.heap_bytes() == 0);
}

// ============================================================================
// AquaScreensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "the aqua saver shows the print its source reports",
                 "[screensaver][aqua]") {
    AquaScreensaver ss;
    ScreensaverStopOnExit<AquaScreensaver> stop_on_exit{ss};
    ClockInput reported;
    reported.seconds_of_day = 14 * 3600;
    reported.print.phase = ClockPrintPhase::PRINTING;
    reported.print.remaining_s = 4320;
    reported.print.progress_pct = 40;
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(ss.clock().mode() == ClockMode::COUNTDOWN);
    CHECK(AquaScreensaverTestAccess::line(ss).text().rfind("ETA ", 0) == 0);

    reported.print.phase = ClockPrintPhase::NONE;
    reported.print.remaining_s = -1;
    advance_saver(ss, 33);
    CHECK(ss.clock().mode() == ClockMode::CLOCK);
    CHECK(AquaScreensaverTestAccess::line(ss).text().rfind("ETA ", 0) == std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "the aqua saver's text line turns the print state's colour",
                 "[screensaver][aqua]") {
    AquaScreensaver ss;
    ScreensaverStopOnExit<AquaScreensaver> stop_on_exit{ss};
    ClockInput reported;
    reported.seconds_of_day = 14 * 3600;
    reported.print.phase = ClockPrintPhase::PRINTING;
    reported.print.remaining_s = 4320;
    reported.print.progress_pct = 40;
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    // The water and the running line are cool; only the paused amber is far redder than blue.
    const auto amber_pixels = [&ss] {
        const FrameTarget frame =
            helix::test::frame_of(lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss)));
        const PixelWriter writer(frame);
        const DirtyRect& box = AquaScreensaverTestAccess::line(ss).box();
        size_t n = 0;
        for (int32_t y = box.y1; y <= box.y2; y++) {
            for (int32_t x = box.x1; x <= box.x2; x++) {
                const Rgb c = writer.get(x, y);
                n += c.r - c.b > 60 ? 1 : 0;
            }
        }
        return n;
    };
    advance_saver(ss, 33);
    CHECK(amber_pixels() == 0);

    reported.print.phase = ClockPrintPhase::PAUSED;
    advance_saver(ss, 33);
    REQUIRE(ss.clock().mode() == ClockMode::PAUSED);
    CHECK(amber_pixels() > 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
