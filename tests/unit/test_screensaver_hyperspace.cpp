// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_canvas.h"
#include "screensaver_hyperspace.h"
#include "screensaver_hyperspace_sim.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::test::check_replays;
using helix::test::count_pixels;
using helix::test::non_opaque_pixels;
using helix::test::TestFrame;
using helix::ui::ClockMode;
using helix::ui::ClockReading;
using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::HyperspaceScreensaver;
using helix::ui::HyperspaceSim;
using helix::ui::kHyperspaceDoneDriftSpeed;
using helix::ui::kHyperspaceLevelCount;
using helix::ui::kHyperspaceLevels;
using helix::ui::kHyperspaceMaxRoll;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

namespace {

constexpr uint32_t W = 320;
constexpr uint32_t H = 200;
constexpr uint32_t kFrameMs = 33;

/// A sim and its frame, stepped at kFrameMs, reading `reading` (no print unless set).
struct Flight : helix::test::SimRun<HyperspaceSim> {
    Flight(uint32_t seed, size_t level, PixelFormat format = PixelFormat::XRGB8888, uint32_t w = W,
           uint32_t h = H, const ClockReading& start = {})
        : SimRun(seed, w, h, format), reading(start) {
        init(level, reading);
    }

    void step(uint32_t dt_ms = kFrameMs) {
        SimRun::step(dt_ms, reading);
    }

    ClockReading reading;

    /// Steps until `done` holds or `limit_ms` of flight passes; true when `done` held.
    template <typename Done> bool run_until(Done&& done, uint32_t limit_ms) {
        for (uint32_t t = 0; t < limit_ms; t += kFrameMs) {
            step();
            if (done()) {
                return true;
            }
        }
        return false;
    }

    bool run_to_tunnel(uint32_t limit_ms = 90000) {
        return run_until([this] { return sim.tunnel_amount() == 255; }, limit_ms);
    }
    bool run_to_open_space(uint32_t limit_ms = 90000) {
        return run_until(
            [this] { return sim.tunnel_amount() == 0 && sim.burst_flares_alive() == 0; }, limit_ms);
    }
};

int max_channel(Rgb c) {
    return std::max({c.r, c.g, c.b});
}

/// Share of pixels whose brightest channel reaches `threshold`.
double share_at_least(const TestFrame& frame, int threshold) {
    const size_t count =
        count_pixels(frame.target, [threshold](Rgb c) { return max_channel(c) >= threshold; });
    return static_cast<double>(count) / (frame.target.w * frame.target.h);
}

/// Share of the aligned `block` x `block` squares whose pixels are all one colour.
double uniform_block_share(const TestFrame& frame, int32_t block) {
    const PixelWriter writer(frame.target);
    size_t blocks = 0;
    size_t uniform = 0;
    for (int32_t by = 0; by + block <= static_cast<int32_t>(frame.target.h); by += block) {
        for (int32_t bx = 0; bx + block <= static_cast<int32_t>(frame.target.w); bx += block) {
            const Rgb first = writer.get(bx, by);
            bool same = true;
            for (int32_t y = by; y < by + block && same; y++) {
                for (int32_t x = bx; x < bx + block && same; x++) {
                    same = writer.get(x, y) == first;
                }
            }
            blocks++;
            uniform += same ? 1 : 0;
        }
    }
    return static_cast<double>(uniform) / static_cast<double>(blocks);
}

constexpr int kHueBins = 12;

/// How many of the pixels at least `threshold` bright fall in each 30-degree slice of the wheel,
/// red first and turning through yellow toward green.
std::array<double, kHueBins> hue_histogram(const TestFrame& frame, int threshold) {
    const PixelWriter writer(frame.target);
    std::array<double, kHueBins> bins{};
    for (int32_t y = 0; y < static_cast<int32_t>(frame.target.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(frame.target.w); x++) {
            const Rgb c = writer.get(x, y);
            if (max_channel(c) < threshold) {
                continue;
            }
            // The angle of the colour in the chroma plane.
            const double alpha = c.r - 0.5 * (c.g + c.b);
            const double beta = std::sqrt(3.0) / 2.0 * (c.g - c.b);
            double degrees = std::atan2(beta, alpha) * 180.0 / M_PI;
            degrees = degrees < 0 ? degrees + 360.0 : degrees;
            bins[static_cast<size_t>(degrees / (360.0 / kHueBins)) % kHueBins] += 1.0;
        }
    }
    return bins;
}

/// Slices round the wheel that `before` must turn to best match `after`.
int best_hue_turn(const std::array<double, kHueBins>& before,
                  const std::array<double, kHueBins>& after) {
    int best = 0;
    double best_score = -1.0;
    for (int turn = 0; turn < kHueBins; turn++) {
        double score = 0.0;
        for (int i = 0; i < kHueBins; i++) {
            score +=
                before[static_cast<size_t>(i)] * after[static_cast<size_t>((i + turn) % kHueBins)];
        }
        if (score > best_score) {
            best_score = score;
            best = turn;
        }
    }
    return best;
}

} // namespace

// ============================================================================
// HyperspaceSim
// ============================================================================

TEST_CASE("a hyperspace flight replays exactly from a seed, and another seed differs",
          "[screensaver][hyperspace_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const auto run = [format](uint32_t seed) {
        Flight flight(seed, 0, format);
        constexpr uint32_t kStepsMs[] = {16, 33, 50, 7, 100};
        for (int i = 0; i < 600; i++) {
            flight.step(kStepsMs[i % 5]);
        }
        return flight.frame.bytes;
    };
    check_replays(run, 9, 10);
}

TEST_CASE("every hyperspace pixel is opaque and every frame dirties the whole canvas",
          "[screensaver][hyperspace_sim]") {
    const size_t level = GENERATE(range(size_t{0}, kHyperspaceLevelCount));
    CAPTURE(level);
    Flight flight(5, level);
    size_t frames = 0;
    size_t opaque_failures = 0;
    for (uint32_t t = 0; t < 40000; t += kFrameMs) {
        flight.step();
        frames++;
        REQUIRE(flight.dirty.size() == 1);
        CHECK(flight.dirty[0] ==
              DirtyRect::whole(static_cast<int32_t>(W), static_cast<int32_t>(H)));
        if (frames % 30 != 0) {
            continue;
        }
        opaque_failures += non_opaque_pixels(flight.frame.target);
    }
    CHECK(opaque_failures == 0);
}

TEST_CASE("the flight passes through tunnel phases and open space, in either format",
          "[screensaver][hyperspace_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    Flight flight(11, 0, format);

    REQUIRE(flight.run_to_tunnel());
    const double tunnel_lit = share_at_least(flight.frame, 64);
    CAPTURE(tunnel_lit);
    CHECK(tunnel_lit > 0.35);

    REQUIRE(flight.run_to_open_space());
    const double open_lit = share_at_least(flight.frame, 64);
    CAPTURE(open_lit);
    CHECK(open_lit < 0.08);
    // ...but not empty: the star flares still stream past.
    CHECK(share_at_least(flight.frame, 96) > 0.0);

    REQUIRE(flight.run_to_tunnel());
}

TEST_CASE("goo glows behind open space down to the level that drops it",
          "[screensaver][hyperspace_sim]") {
    const auto unlit_share_in_open_space = [](size_t level) {
        Flight flight(17, level);
        REQUIRE(flight.run_to_open_space());
        return 1.0 - share_at_least(flight.frame, 1);
    };
    CHECK(kHyperspaceLevels[2].goo);
    CHECK_FALSE(kHyperspaceLevels[3].goo);
    const double with_goo = unlit_share_in_open_space(2);
    const double without_goo = unlit_share_in_open_space(3);
    CAPTURE(with_goo, without_goo);
    CHECK(with_goo < 0.75);
    CHECK(without_goo > 0.85);
}

TEST_CASE("the tunnel draws at each level's block size, and the last level draws none",
          "[screensaver][hyperspace_sim]") {
    const auto tunnel_frame_stats = [](size_t level) {
        Flight flight(23, level);
        // A level without a tunnel still keeps the phase clock, so a tunnel phase comes round.
        REQUIRE(flight.run_to_tunnel());
        return std::make_pair(share_at_least(flight.frame, 64),
                              std::make_pair(uniform_block_share(flight.frame, 2),
                                             uniform_block_share(flight.frame, 4)));
    };

    const auto full = tunnel_frame_stats(0);
    const auto half = tunnel_frame_stats(2);
    const auto quarter = tunnel_frame_stats(4);
    const auto none = tunnel_frame_stats(kHyperspaceLevelCount - 1);
    CAPTURE(full.second.first, half.second.first, half.second.second, quarter.second.second);
    CAPTURE(full.first, quarter.first, none.first);

    CHECK(kHyperspaceLevels[0].block == 1);
    CHECK(kHyperspaceLevels[2].block == 2);
    CHECK(kHyperspaceLevels[4].block == 4);
    CHECK(kHyperspaceLevels[kHyperspaceLevelCount - 1].block == 0);

    CHECK(full.second.first < 0.7);
    CHECK(half.second.first > 0.9);
    CHECK(half.second.second < 0.7);
    CHECK(quarter.second.second > 0.85);

    CHECK(quarter.first > 0.35);
    CHECK(none.first < 0.08);
}

TEST_CASE("the last level draws fewer stars than the rung above it",
          "[screensaver][hyperspace_sim]") {
    // Both levels draw stars and nothing behind them, and a seed flies the same stars at every
    // level, so stepping them together compares the same sky.
    Flight full(47, kHyperspaceLevelCount - 2);
    Flight fewer(47, kHyperspaceLevelCount - 1);
    REQUIRE_FALSE(kHyperspaceLevels[kHyperspaceLevelCount - 2].goo);
    bool open = false;
    for (uint32_t t = 0; t < 30000 && !open; t += kFrameMs) {
        full.step();
        fewer.step();
        open = full.sim.tunnel_amount() == 0 && full.sim.burst_flares_alive() == 0 && t > 1000;
    }
    REQUIRE(open);
    const double stars_full = share_at_least(full.frame, 1);
    const double stars_fewer = share_at_least(fewer.frame, 1);
    CAPTURE(stars_full, stars_fewer);
    CHECK(stars_fewer > 0.0);
    CHECK(stars_fewer < 0.8 * stars_full);
}

TEST_CASE("the hyperspace colours cycle around the wheel over a few seconds",
          "[screensaver][hyperspace_sim]") {
    Flight flight(29, 0);
    REQUIRE(flight.run_to_tunnel());
    const uint8_t hue_then = flight.sim.hue();
    const auto seen_then = hue_histogram(flight.frame, 64);
    flight.run_until([] { return false; }, 6000);
    REQUIRE(flight.sim.tunnel_amount() == 255);
    const auto seen_now = hue_histogram(flight.frame, 64);
    const int turn = best_hue_turn(seen_then, seen_now);
    CAPTURE(turn, static_cast<int>(hue_then), static_cast<int>(flight.sim.hue()));
    // Six seconds turn the colours a quarter of the way round, give or take a slice.
    CHECK(turn >= 2);
    CHECK(turn <= 4);
    CHECK(flight.sim.hue() != hue_then);
}

TEST_CASE("the camera always flies forward, rolls a little and drifts gently",
          "[screensaver][hyperspace_sim]") {
    Flight flight(31, 0);
    float slowest = 1e9f;
    float most_roll = 0.0f;
    float min_drift = 1e9f;
    float max_drift = -1e9f;
    constexpr uint32_t kStepsMs[] = {16, 33, 100, 250, 5};
    for (int i = 0; i < 2000; i++) {
        flight.step(kStepsMs[i % 5]);
        const auto& cam = flight.sim.camera();
        slowest = std::min(slowest, cam.speed);
        most_roll = std::max(most_roll, std::fabs(cam.roll));
        min_drift = std::min(min_drift, cam.drift_x);
        max_drift = std::max(max_drift, cam.drift_x);
    }
    CAPTURE(slowest, most_roll, min_drift, max_drift);
    CHECK(slowest > 0.0f);
    CHECK(most_roll <= kHyperspaceMaxRoll);
    CHECK(most_roll > 0.05f);
    CHECK(max_drift - min_drift > 4.0f);
    // The vanishing point strays at most a tenth of the frame height from the centre.
    const float bound = 0.1f * static_cast<float>(H) + 0.01f;
    CHECK(std::fabs(min_drift) <= bound);
    CHECK(std::fabs(max_drift) <= bound);
}

TEST_CASE("a starburst of flares bursts ahead within the first few seconds",
          "[screensaver][hyperspace_sim]") {
    Flight flight(37, 0);
    REQUIRE(flight.sim.bursts_started() == 0);
    REQUIRE(flight.run_until([&] { return flight.sim.bursts_started() > 0; }, 15000));
    flight.step();
    CHECK(flight.sim.burst_flares_alive() > HyperspaceSim::kBurstFlares / 2);
    // Its flares burn out rather than piling up.
    REQUIRE(flight.run_until([&] { return flight.sim.burst_flares_alive() == 0; }, 15000));
}

TEST_CASE("a hyperspace run sizes its tunnel for its start level",
          "[screensaver][hyperspace_sim]") {
    const auto tunnel_at_start = [](size_t level) {
        Flight flight(41, level);
        flight.step();
        return flight.sim.tunnel_bytes();
    };
    const size_t full = tunnel_at_start(0);
    const size_t half = tunnel_at_start(2);
    const size_t quarter = tunnel_at_start(4);
    CAPTURE(full, half, quarter);
    CHECK(half < full);
    CHECK(quarter < half);
    CHECK(quarter > 0);
    CHECK(tunnel_at_start(kHyperspaceLevelCount - 1) == 0);
}

TEST_CASE("a hyperspace level change applies at the next frame, and neither frames nor step-downs "
          "allocate",
          "[screensaver][hyperspace_sim]") {
    Flight flight(41, 0);
    flight.step();
    REQUIRE(flight.sim.heap_bytes() > 0);
    const size_t start_tunnel = flight.sim.tunnel_bytes();
    REQUIRE(start_tunnel > 0);
    size_t held = flight.sim.heap_bytes();
    for (size_t level = 1; level < kHyperspaceLevelCount + 1; level++) {
        CAPTURE(level);
        flight.sim.request_level(level);
        flight.step();
        const size_t at = std::min(level, kHyperspaceLevelCount - 1);
        CHECK(flight.sim.level() == at);
        const size_t bytes = flight.sim.heap_bytes();
        CHECK(bytes <= held);
        if (kHyperspaceLevels[at].block != 0) {
            // A coarser tunnel fits in the tables the run already holds.
            CHECK(flight.sim.tunnel_bytes() == start_tunnel);
        } else {
            // A level without a tunnel lets its tables go.
            CHECK(flight.sim.tunnel_bytes() == 0);
        }
        for (int i = 0; i < 20; i++) {
            flight.step();
        }
        CHECK(flight.sim.heap_bytes() == bytes);
        held = bytes;
    }
    // A finer level than the run holds rebuilds the tables it needs.
    flight.sim.request_level(0);
    flight.step();
    CHECK(flight.sim.level() == 0);
    CHECK(flight.sim.tunnel_bytes() == start_tunnel);

    flight.sim.clear();
    CHECK(flight.sim.heap_bytes() == 0);
}

TEST_CASE("an odd-sized hyperspace frame fills every pixel and never writes past a row",
          "[screensaver][hyperspace_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    const size_t level = GENERATE(size_t{0}, size_t{2}, size_t{4});
    CAPTURE(static_cast<int>(format), level);
    constexpr uint32_t OW = 321;
    constexpr uint32_t kOh = 203;
    constexpr uint8_t kSentinel = 0xA5;
    // Every byte starts as the sentinel, and the guard row past the frame catches a block
    // written below its last row.
    TestFrame frame(OW, kOh, format, 7, kSentinel, kSentinel);
    std::minstd_rand rng(43);
    HyperspaceSim sim;
    sim.init(frame.target, rng, level, ClockReading{});
    std::vector<DirtyRect> dirty;
    bool tunnel_seen = false;
    for (uint32_t t = 0; t < 30000 && !tunnel_seen; t += kFrameMs) {
        sim.step(kFrameMs, frame.target, rng, ClockReading{}, dirty);
        tunnel_seen = sim.tunnel_amount() == 255;
    }
    REQUIRE(tunnel_seen);

    CHECK(frame.padding_intact());
    if (format == PixelFormat::XRGB8888) {
        CHECK(non_opaque_pixels(frame.target) == 0);
    }
    // The last column and row, which a block only partly covers, are drawn too.
    const PixelWriter writer(frame.target);
    size_t edge_lit = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(kOh); y++) {
        edge_lit += max_channel(writer.get(OW - 1, y)) > 0 ? 1 : 0;
    }
    for (int32_t x = 0; x < static_cast<int32_t>(OW); x++) {
        edge_lit += max_channel(writer.get(x, kOh - 1)) > 0 ? 1 : 0;
    }
    CHECK(edge_lit > (OW + kOh) / 2);
}

// ============================================================================
// HyperspaceSim: the print's light at the end of the tunnel
// ============================================================================

namespace {

ClockReading print_at(ClockMode mode, int32_t pct) {
    return helix::test::print_reading_for(mode, pct);
}

Flight print_flight(uint32_t seed, size_t level, const ClockReading& reading) {
    return Flight(seed, level, PixelFormat::XRGB8888, W, H, reading);
}

/// Pixels within `radius` of the vanishing point whose dimmest channel reaches `threshold`:
/// warm white light, where the saturated star and goo colours keep one channel low.
size_t white_near_vanishing_point(const Flight& flight, float radius, int threshold) {
    const PixelWriter writer(flight.frame.target);
    const float vx = static_cast<float>(W) / 2.0f + flight.sim.camera().drift_x;
    const float vy = static_cast<float>(H) / 2.0f + flight.sim.camera().drift_y;
    size_t n = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(H); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(W); x++) {
            const float dx = static_cast<float>(x) - vx;
            const float dy = static_cast<float>(y) - vy;
            const Rgb c = writer.get(x, y);
            if (dx * dx + dy * dy <= radius * radius && std::min({c.r, c.g, c.b}) >= threshold) {
                n++;
            }
        }
    }
    return n;
}

} // namespace

TEST_CASE("the light at the end of the tunnel grows with the print",
          "[screensaver][hyperspace_sim]") {
    const size_t level = GENERATE(size_t{0}, kHyperspaceLevelCount - 1);
    CAPTURE(level);
    Flight none(53, level);
    Flight early = print_flight(53, level, print_at(ClockMode::COUNTDOWN, 20));
    Flight late = print_flight(53, level, print_at(ClockMode::COUNTDOWN, 80));
    for (int i = 0; i < 10; i++) {
        none.step();
        early.step();
        late.step();
    }
    CHECK(none.sim.light_radius() == 0.0f);
    const float side = static_cast<float>(std::min(W, H));
    const float end = (level == 0 ? 0.18f : 0.09f) * side;
    CHECK(early.sim.light_radius() == Catch::Approx(4.0f + (end - 4.0f) * 0.2f));
    CHECK(late.sim.light_radius() == Catch::Approx(4.0f + (end - 4.0f) * 0.8f));

    // The light is drawn: a white glow round the vanishing point, wider later in the print.
    const float reach = late.sim.light_radius();
    const size_t lit_none = white_near_vanishing_point(none, reach, 40);
    const size_t lit_early = white_near_vanishing_point(early, reach, 40);
    const size_t lit_late = white_near_vanishing_point(late, reach, 40);
    CAPTURE(lit_none, lit_early, lit_late);
    CHECK(lit_early > lit_none);
    CHECK(lit_late > 3 * lit_early);
}

TEST_CASE("a paused print slows the flight to a quarter", "[screensaver][hyperspace_sim]") {
    Flight flying = print_flight(59, 0, print_at(ClockMode::COUNTDOWN, 40));
    Flight paused = print_flight(59, 0, print_at(ClockMode::COUNTDOWN, 40));
    paused.reading = print_at(ClockMode::PAUSED, 40);
    for (int i = 0; i < 90; i++) {
        flying.step();
        paused.step();
    }
    CHECK(paused.sim.speed_scale() == 0.25f);
    CHECK(paused.sim.camera().speed == Catch::Approx(flying.sim.camera().speed * 0.25f));
}

TEST_CASE("a finished print drops out into drifting stars", "[screensaver][hyperspace_sim]") {
    SECTION("from the tunnel, the flight slows to a drift that keeps moving") {
        Flight flight = print_flight(61, 0, print_at(ClockMode::COUNTDOWN, 70));
        REQUIRE(flight.run_to_tunnel());
        const float cruising = flight.sim.camera().speed;
        flight.reading = print_at(ClockMode::DONE, 100);
        const uint32_t bursts = flight.sim.bursts_started();
        const bool slowed = flight.run_until(
            [&flight] { return flight.sim.speed_scale() == kHyperspaceDoneDriftSpeed; }, 20000);
        REQUIRE(slowed);
        CHECK(flight.sim.camera().speed > 0.0f);
        CHECK(flight.sim.camera().speed < cruising);
        CHECK(flight.sim.tunnel_amount() == 0);
        CHECK(flight.sim.bursts_started() == bursts);
        CHECK(flight.sim.light_radius() > 0.0f);

        const std::vector<uint8_t> earlier = flight.frame.snapshot();
        for (int i = 0; i < 300; i++) {
            flight.step();
            CHECK(flight.dirty.size() == 1);
        }
        CHECK(helix::test::bytes_differing(earlier, flight.frame.bytes) > 0);
        CHECK(flight.sim.bursts_started() == bursts);
    }
    SECTION("a starburst due just after the print finishes never comes") {
        // The same seed and print fly the same burst schedule: find when the second burst
        // comes, then finish the print a second before it.
        Flight ahead = print_flight(83, 0, print_at(ClockMode::COUNTDOWN, 70));
        int due = 0;
        for (; due < 3000 && ahead.sim.bursts_started() < 2; due++) {
            ahead.step();
        }
        REQUIRE(ahead.sim.bursts_started() == 2);
        Flight flight = print_flight(83, 0, print_at(ClockMode::COUNTDOWN, 70));
        for (int i = 0; i < due - 30; i++) {
            flight.step();
        }
        REQUIRE(flight.sim.bursts_started() == 1);
        flight.reading = print_at(ClockMode::DONE, 100);
        for (int i = 0; i < 300; i++) {
            flight.step();
        }
        CHECK(flight.sim.bursts_started() == 1);
    }
    SECTION("a flight started after the print finished opens drifting, at every level") {
        for (size_t level = 0; level < kHyperspaceLevelCount; level++) {
            CAPTURE(level);
            Flight flight = print_flight(67, level, print_at(ClockMode::DONE, 100));
            flight.step();
            REQUIRE(flight.dirty.size() == 1);
            CHECK(flight.sim.tunnel_amount() == 0);
            CHECK(flight.sim.speed_scale() == kHyperspaceDoneDriftSpeed);
            flight.step();
            CHECK(flight.dirty.size() == 1);
        }
    }
    SECTION("a new print takes off again") {
        Flight flight = print_flight(71, 0, print_at(ClockMode::DONE, 100));
        flight.step();
        REQUIRE(flight.sim.speed_scale() == kHyperspaceDoneDriftSpeed);
        flight.reading = print_at(ClockMode::PROGRESS, 0);
        flight.step();
        CHECK(flight.dirty.size() == 1);
        for (int i = 0; i < 60; i++) {
            flight.step();
        }
        CHECK(flight.sim.speed_scale() == 1.0f);
        CHECK(flight.sim.camera().speed > 0.0f);
    }
}

TEST_CASE("a failed print dims the light and closes the tunnel", "[screensaver][hyperspace_sim]") {
    const ClockMode mode = GENERATE(ClockMode::FAILED, ClockMode::CANCELLED);
    CAPTURE(static_cast<int>(mode));
    SECTION("from the tunnel") {
        Flight flight = print_flight(73, 0, print_at(ClockMode::COUNTDOWN, 60));
        REQUIRE(flight.run_to_tunnel());
        REQUIRE(flight.sim.light_radius() > 0.0f);
        flight.reading = print_at(mode, 60);
        for (int i = 0; i < 20; i++) {
            flight.step();
        }
        CHECK(flight.sim.light_gain() > 0.0f);
        CHECK(flight.sim.light_gain() < 1.0f);
        for (int i = 0; i < 90; i++) {
            flight.step();
        }
        CHECK(flight.sim.light_radius() == 0.0f);
        CHECK(flight.sim.tunnel_amount() == 0);
        CHECK(flight.sim.camera().speed > 0.0f);
        CHECK(flight.dirty.size() == 1);
    }
    SECTION("a flight started after the print failed shows neither light nor tunnel") {
        Flight flight = print_flight(79, 0, print_at(mode, 60));
        uint8_t most = 0;
        for (uint32_t t = 0; t < 30000; t += kFrameMs) {
            flight.step();
            most = std::max(most, flight.sim.tunnel_amount());
        }
        CHECK(most == 0);
        CHECK(flight.sim.light_radius() == 0.0f);
    }
}

// ============================================================================
// HyperspaceScreensaver
// ============================================================================

#endif // HELIX_ENABLE_SCREENSAVER
