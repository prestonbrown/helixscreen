// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_fireworks_sim.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <random>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::test::check_replays;
using helix::test::count_pixels;
using helix::test::diff_against;
using helix::test::FrameDiff;
using helix::test::non_opaque_pixels;
using helix::test::print_reading_for;
using helix::ui::ClockMode;
using helix::ui::ClockReading;
using helix::ui::DirtyRect;
using helix::ui::FireworksPacing;
using helix::ui::FireworksSim;
using helix::ui::FrameTarget;
using helix::ui::kFireworksLevelCount;
using helix::ui::kFireworksLevels;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

namespace {

constexpr uint32_t W = 320;
constexpr uint32_t H = 200;

/// The reading of a frame with no print.
const ClockReading kNoPrint{};

/// A frame of the size every fireworks sim test draws.
struct SimFrame : helix::test::TestFrame {
    explicit SimFrame(PixelFormat format) : TestFrame(W, H, format) {}
};

/// A busy sky: a shell every 150 to 400 ms and a finale every 6 s.
FireworksPacing busy_pacing() {
    FireworksPacing pacing;
    pacing.first_launch_ms = 50;
    pacing.min_gap_ms = 150;
    pacing.max_gap_ms = 400;
    pacing.finale_every_ms = 6000;
    pacing.finale_span_ms = 1500;
    return pacing;
}

/// One shell at 50 ms, then nothing for ten minutes.
FireworksPacing single_shell_pacing() {
    FireworksPacing pacing;
    pacing.first_launch_ms = 50;
    pacing.min_gap_ms = 600000;
    pacing.max_gap_ms = 600000;
    pacing.finale_every_ms = 600000;
    return pacing;
}

uint8_t only(FireworksSim::BurstKind kind) {
    return static_cast<uint8_t>(1u << static_cast<unsigned>(kind));
}

/// Pixels brighter than any sky or star colour: a spark or a rocket.
size_t lit_pixels(const SimFrame& frame) {
    return count_pixels(frame.target, [](Rgb c) { return c.r > 120 || c.g > 120; });
}

int brightness(Rgb c) {
    return c.r + c.g + c.b;
}

} // namespace

// ============================================================================
// FireworksSim
// ============================================================================

TEST_CASE("the fireworks sky is a gradient with faint stars over rolling hills, in either format",
          "[screensaver][fireworks_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    std::minstd_rand rng(3);
    FireworksSim sim;
    SimFrame frame(format);
    sim.init(frame.target, rng, 0);

    // The frame holds exactly the computed sky, dithered as a redraw dithers it.
    SimFrame expected(format);
    PixelWriter painter(expected.target);
    for (int32_t y = 0; y < static_cast<int32_t>(H); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(W); x++) {
            painter.put_dithered(x, y, sim.sky_at(x, y));
        }
    }
    CHECK(frame.bytes == expected.bytes);

    const Rgb ground = sim.sky_at(0, static_cast<int32_t>(H) - 1);
    int32_t lowest_top = static_cast<int32_t>(H);
    int32_t highest_top = 0;
    size_t stars = 0;
    int32_t starless_column = -1;
    int32_t starless_top = 0;
    for (int32_t x = 0; x < static_cast<int32_t>(W); x++) {
        int32_t top = static_cast<int32_t>(H) - 1;
        while (top > 0 && sim.sky_at(x, top - 1) == ground) {
            top--;
        }
        lowest_top = std::min(lowest_top, top);
        highest_top = std::max(highest_top, top);
        size_t column_stars = 0;
        for (int32_t y = 0; y < top; y++) {
            column_stars += sim.sky_at(x, y).r >= 40 ? 1 : 0;
        }
        stars += column_stars;
        if (column_stars == 0 && starless_column < 0) {
            starless_column = x;
            starless_top = top;
        }
    }
    CHECK(highest_top > lowest_top);                 // rolling, not flat
    CHECK(lowest_top > static_cast<int32_t>(H) / 2); // the hills stay low
    CHECK(stars > 0);
    CHECK(stars <= static_cast<size_t>(FireworksSim::kStarCount));
    REQUIRE(starless_column >= 0);
    // Darker at the top of the sky than just above the hills.
    CHECK(brightness(sim.sky_at(starless_column, 0)) <
          brightness(sim.sky_at(starless_column, starless_top - 1)));

    if (format == PixelFormat::XRGB8888) {
        CHECK(non_opaque_pixels(frame.target) == 0);
    }
}

TEST_CASE("a fireworks show replays exactly from a seed, and another seed differs",
          "[screensaver][fireworks_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const auto run = [format](uint32_t seed) {
        std::minstd_rand rng(seed);
        FireworksSim sim;
        SimFrame frame(format);
        sim.init(frame.target, rng, 0, busy_pacing());
        std::vector<std::vector<DirtyRect>> boxes;
        std::vector<DirtyRect> dirty;
        constexpr uint32_t kFrameMs[] = {16, 33, 50, 7, 100};
        for (int i = 0; i < 400; i++) {
            sim.step(kFrameMs[i % 5], frame.target, rng, kNoPrint, dirty);
            boxes.push_back(dirty);
        }
        return std::make_pair(frame.bytes, boxes);
    };
    check_replays(run, 9, 10);
}

TEST_CASE("every pixel a fireworks frame changes lies in one of its dirty boxes",
          "[screensaver][fireworks_sim]") {
    using Kind = FireworksSim::BurstKind;
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    const uint8_t kinds = GENERATE(FireworksSim::kAllKinds, only(Kind::PALM), only(Kind::CROSSETTE),
                                   only(Kind::STROBE));
    CAPTURE(static_cast<int>(format), static_cast<int>(kinds));
    std::minstd_rand rng(21);
    FireworksSim sim;
    SimFrame frame(format);
    sim.init(frame.target, rng, 0, busy_pacing());
    sim.set_kinds(kinds);

    std::vector<DirtyRect> dirty;
    size_t changed = 0;
    size_t uncovered = 0;
    size_t most_sparks = 0;
    size_t most_boxes = 0;
    size_t most_lit = 0;
    for (int i = 0; i < 700; i++) {
        const std::vector<uint8_t> before = frame.snapshot();
        sim.step(16, frame.target, rng, kNoPrint, dirty);
        const FrameDiff diff = diff_against(before, frame, dirty);
        changed += diff.changed;
        uncovered += diff.uncovered;
        most_boxes = std::max(most_boxes, dirty.size());
        most_sparks = std::max(most_sparks, sim.sparks_in_use());
        if (i % 50 == 25) {
            most_lit = std::max(most_lit, lit_pixels(frame));
        }
    }
    REQUIRE(changed > 0);
    REQUIRE(most_sparks > 0);
    CHECK(uncovered == 0);
    CHECK(most_boxes > 1);
    CHECK(most_boxes <= 2 * FireworksSim::kMaxBursts);
    CHECK(most_lit > 0);
}

TEST_CASE("each crossette star splits into a cross of four fragments",
          "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(12);
    FireworksSim sim;
    SimFrame frame(PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 0, single_shell_pacing());
    sim.set_kinds(only(FireworksSim::BurstKind::CROSSETTE));

    std::vector<DirtyRect> dirty;
    size_t stars = 0;
    size_t most = 0;
    for (int i = 0; i < 300; i++) {
        sim.step(16, frame.target, rng, kNoPrint, dirty);
        if (stars == 0) {
            stars = sim.sparks_in_use();
        }
        most = std::max(most, sim.sparks_in_use());
    }
    REQUIRE(stars > 0);
    // A few stars, far fewer than the level's sparks per burst, so the split has room.
    CHECK(stars <= kFireworksLevels[0].sparks_per_burst / 4);
    // Every star has split before the first fragment burns out.
    CHECK(most == 4 * stars);
    CHECK(sim.sparks_in_use() == 0);
}

TEST_CASE("burnt-out crossette fragments return to the spark pool",
          "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(31);
    FireworksSim sim;
    SimFrame frame(PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 0, busy_pacing());
    sim.set_kinds(only(FireworksSim::BurstKind::CROSSETTE));

    // Three minutes of shells: far more fragments than the pool holds have lived and died.
    std::vector<DirtyRect> dirty;
    for (uint32_t ms = 0; ms < 180000; ms += 16) {
        sim.step(16, frame.target, rng, kNoPrint, dirty);
    }

    // The shells launched over the next ten seconds still burst.
    size_t most = 0;
    for (uint32_t ms = 0; ms < 10000; ms += 16) {
        sim.step(16, frame.target, rng, kNoPrint, dirty);
        most = std::max(most, sim.sparks_in_use());
    }
    CHECK(most >= kFireworksLevels[0].sparks_per_burst / 8);
}

TEST_CASE("a palm throws a few comets with long tails", "[screensaver][fireworks_sim]") {
    const size_t level = GENERATE(as<size_t>{}, 0, kFireworksLevelCount - 1);
    CAPTURE(level);
    std::minstd_rand rng(17);
    FireworksSim sim;
    SimFrame frame(PixelFormat::XRGB8888);
    sim.init(frame.target, rng, level, single_shell_pacing());
    sim.set_kinds(only(FireworksSim::BurstKind::PALM));

    std::vector<DirtyRect> dirty;
    size_t comets = 0;
    int frames_since_burst = 0;
    for (int i = 0; i < 2000 && frames_since_burst < 20; i++) {
        sim.step(16, frame.target, rng, kNoPrint, dirty);
        if (comets == 0) {
            comets = sim.sparks_in_use();
        } else {
            frames_since_burst++;
        }
    }
    REQUIRE(frames_since_burst == 20);
    CHECK(comets >= 7);
    CHECK(comets <= 12);
    // A 2x2 head alone lights at most 4 pixels, so more than 5 per comet is a tail, even at
    // the level that gives other shells none.
    CHECK(lit_pixels(frame) > 5 * comets);
}

TEST_CASE("palm comets arc over inside the top of a short frame", "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(23);
    FireworksSim sim;
    helix::test::TestFrame frame(480, 320, PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 0, busy_pacing());
    sim.set_kinds(only(FireworksSim::BurstKind::PALM));

    // A comet climbing past the top edge lights the top row on its way out and back.
    const PixelWriter reader(frame.target);
    std::vector<DirtyRect> dirty;
    size_t most = 0;
    size_t top_row_hits = 0;
    for (uint32_t ms = 0; ms < 60000; ms += 16) {
        sim.step(16, frame.target, rng, kNoPrint, dirty);
        most = std::max(most, sim.sparks_in_use());
        for (int32_t x = 0; x < 480; x++) {
            const Rgb c = reader.get(x, 0);
            top_row_hits += (c.r > 120 || c.g > 120) ? 1 : 0;
        }
    }
    REQUIRE(most > 0);
    CHECK(top_row_hits == 0);
}

TEST_CASE("strobe stars hang and blink white", "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(31);
    FireworksSim sim;
    SimFrame frame(PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 0, single_shell_pacing());
    sim.set_kinds(only(FireworksSim::BurstKind::STROBE));

    std::vector<DirtyRect> dirty;
    int burst_frame = -1;
    size_t fewest_lit = SIZE_MAX;
    size_t most_lit = 0;
    size_t white_at_most = 0;
    size_t sparks_at_start = 0;
    size_t fewest_sparks = SIZE_MAX;
    for (int i = 0; i < 400; i++) {
        sim.step(16, frame.target, rng, kNoPrint, dirty);
        if (burst_frame < 0 && sim.sparks_in_use() > 0) {
            burst_frame = i;
        }
        // One to one and a half seconds after the burst every star is blinking.
        if (burst_frame < 0 || i < burst_frame + 62 || i > burst_frame + 94) {
            continue;
        }
        if (sparks_at_start == 0) {
            sparks_at_start = sim.sparks_in_use();
        }
        fewest_sparks = std::min(fewest_sparks, sim.sparks_in_use());
        const size_t lit = lit_pixels(frame);
        fewest_lit = std::min(fewest_lit, lit);
        if (lit > most_lit) {
            most_lit = lit;
            white_at_most = count_pixels(frame.target,
                                         [](Rgb c) { return c.r > 200 && c.g > 200 && c.b > 200; });
        }
    }
    REQUIRE(sparks_at_start > 0);
    // Every star is still alive, yet most are dark on any one frame.
    CHECK(fewest_sparks == sparks_at_start);
    CHECK(most_lit > 0);
    CHECK(most_lit < sparks_at_start);
    CHECK(fewest_lit < most_lit);
    CHECK(white_at_most * 2 > most_lit);
}

TEST_CASE("the spark pool is sized at start, never grows, and a full pool drops sparks",
          "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(5);
    FireworksSim sim;
    SimFrame frame(PixelFormat::XRGB8888);

    sim.init(frame.target, rng, 0, busy_pacing());
    CHECK(sim.spark_capacity() == static_cast<size_t>(kFireworksLevels[0].sparks_per_burst) *
                                      kFireworksLevels[0].bursts_at_once);

    // Sized for the cheapest level, then asked for the most expensive: one burst fills the pool.
    const size_t cheapest = kFireworksLevelCount - 1;
    sim.init(frame.target, rng, cheapest, busy_pacing());
    const size_t capacity = sim.spark_capacity();
    REQUIRE(capacity == static_cast<size_t>(kFireworksLevels[cheapest].sparks_per_burst) *
                            kFireworksLevels[cheapest].bursts_at_once);
    sim.request_level(0);

    std::vector<DirtyRect> dirty;
    size_t most = 0;
    size_t capacity_changes = 0;
    for (int i = 0; i < 1200; i++) {
        sim.step(16, frame.target, rng, kNoPrint, dirty);
        capacity_changes += sim.spark_capacity() != capacity ? 1 : 0;
        most = std::max(most, sim.sparks_in_use());
    }
    CHECK(sim.level() == 0);
    CHECK(capacity_changes == 0);
    // Bursts after the pool filled found it full and dropped what did not fit.
    CHECK(most == capacity);
}

TEST_CASE("a fireworks level request waits for the next shell launch",
          "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(8);
    FireworksSim sim;
    SimFrame frame(PixelFormat::RGB565);
    FireworksPacing pacing;
    pacing.first_launch_ms = 300;
    pacing.min_gap_ms = 1000;
    pacing.max_gap_ms = 1000;
    pacing.finale_every_ms = 600000;
    sim.init(frame.target, rng, 0, pacing);
    std::vector<DirtyRect> dirty;
    const auto run_ms = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 20) {
            sim.step(20, frame.target, rng, kNoPrint, dirty);
        }
    };

    run_ms(400); // the first shell launched at 300 ms
    REQUIRE(sim.level() == 0);
    sim.request_level(2);
    run_ms(800); // 1200 ms; the next launch is due at 1300 ms
    CHECK(sim.level() == 0);
    run_ms(200); // 1400 ms
    CHECK(sim.level() == 2);

    sim.request_level(9); // clamped to the last level, again at the next launch
    run_ms(800);          // 2200 ms
    CHECK(sim.level() == 2);
    run_ms(200); // 2400 ms
    CHECK(sim.level() == kFireworksLevelCount - 1);
}

// ============================================================================
// The print
// ============================================================================

namespace {

/// Steps `sim` in 20 ms frames for `ms` on `print`; the shells launched meanwhile.
uint32_t run_show(FireworksSim& sim, SimFrame& frame, std::minstd_rand& rng,
                  const ClockReading& print, uint32_t ms) {
    const uint32_t before = sim.shells_launched();
    std::vector<DirtyRect> dirty;
    for (uint32_t t = 0; t < ms; t += 20) {
        sim.step(20, frame.target, rng, print, dirty);
    }
    return sim.shells_launched() - before;
}

} // namespace

TEST_CASE("launches come faster and bigger as the print goes", "[screensaver][fireworks_sim]") {
    // One peony: half the level's sparks at the start of a print, all of them at the end.
    for (const int32_t pct : {0, 100}) {
        CAPTURE(pct);
        std::minstd_rand rng(4);
        FireworksSim sim;
        SimFrame frame(PixelFormat::XRGB8888);
        sim.init(frame.target, rng, 0, single_shell_pacing());
        sim.set_kinds(only(FireworksSim::BurstKind::PEONY));
        const ClockReading print = print_reading_for(ClockMode::COUNTDOWN, pct);
        std::vector<DirtyRect> dirty;
        size_t most = 0;
        for (int i = 0; i < 300; i++) {
            sim.step(16, frame.target, rng, print, dirty);
            most = std::max(most, sim.sparks_in_use());
        }
        const size_t full = kFireworksLevels[0].sparks_per_burst;
        CHECK(most == (pct == 0 ? full / 2 : full));
    }

    // Two minutes of the show's own pacing at either end of a print.
    const auto launches_at = [](int32_t pct) {
        std::minstd_rand rng(6);
        FireworksSim sim;
        SimFrame frame(PixelFormat::RGB565);
        sim.init(frame.target, rng, 0);
        return run_show(sim, frame, rng, print_reading_for(ClockMode::COUNTDOWN, pct), 120000);
    };
    const uint32_t slow = launches_at(0);
    const uint32_t fast = launches_at(100);
    CAPTURE(slow, fast);
    CHECK(slow > 0);
    CHECK(fast * 2 > slow * 3);
}

TEST_CASE("a finished print loops the finale", "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(9);
    FireworksSim sim;
    SimFrame frame(PixelFormat::XRGB8888);
    sim.init(frame.target, rng, kFireworksLevelCount - 1);
    const ClockReading done = print_reading_for(ClockMode::DONE, 100);
    std::vector<DirtyRect> dirty;
    // Every ten seconds of a minute, finale shells are still going up.
    for (int window = 0; window < 6; window++) {
        CAPTURE(window);
        bool finale = false;
        const uint32_t before = sim.shells_launched();
        for (uint32_t t = 0; t < 10000; t += 20) {
            sim.step(20, frame.target, rng, done, dirty);
            finale = finale || sim.finale_running();
        }
        CHECK(finale);
        CHECK(sim.shells_launched() - before >= 3);
    }
    // A new print stops the finale at once.
    for (int i = 0; i < 1000 && !sim.finale_running(); i++) {
        sim.step(20, frame.target, rng, done, dirty);
    }
    REQUIRE(sim.finale_running());
    sim.step(20, frame.target, rng, print_reading_for(ClockMode::PROGRESS, 0), dirty);
    CHECK_FALSE(sim.finale_running());
}

TEST_CASE("a failed print fizzles once in red and goes quiet", "[screensaver][fireworks_sim]") {
    const auto mode = GENERATE(ClockMode::FAILED, ClockMode::CANCELLED);
    CAPTURE(static_cast<int>(mode));
    std::minstd_rand rng(13);
    FireworksSim sim;
    SimFrame frame(PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 0, busy_pacing());
    const ClockReading ended = print_reading_for(mode, 40);
    std::vector<DirtyRect> dirty;
    size_t most = 0;
    size_t red = 0;
    for (uint32_t t = 0; t < 20000; t += 16) {
        sim.step(16, frame.target, rng, ended, dirty);
        most = std::max(most, sim.sparks_in_use());
        red = std::max(red, count_pixels(frame.target, [](Rgb c) {
                           return c.r > 180 && c.g < 100 && c.b < 100;
                       }));
    }
    CHECK(sim.shells_launched() == 1);
    CHECK(most > 0);
    CHECK(most <= kFireworksLevels[0].sparks_per_burst / 4);
    CHECK(red > 0);
    CHECK(sim.sparks_in_use() == 0);
}

TEST_CASE("a paused print launches nothing for an hour", "[screensaver][fireworks_sim]") {
    std::minstd_rand rng(15);
    FireworksSim sim;
    SimFrame frame(PixelFormat::RGB565);
    sim.init(frame.target, rng, 0, busy_pacing());
    const ClockReading running = print_reading_for(ClockMode::COUNTDOWN, 40);
    REQUIRE(run_show(sim, frame, rng, running, 5000) > 0);

    std::vector<DirtyRect> dirty;
    const ClockReading paused = print_reading_for(ClockMode::PAUSED, 40);
    const uint32_t before = sim.shells_launched();
    for (uint32_t t = 0; t < 3600u * 1000u; t += 100) {
        sim.step(100, frame.target, rng, paused, dirty);
    }
    CHECK(sim.shells_launched() == before);
    CHECK(sim.sparks_in_use() == 0);
    CHECK_FALSE(sim.finale_running());
    // Resuming owes nothing: at most the one shell that was due.
    CHECK(run_show(sim, frame, rng, running, 100) <= 1);
    CHECK_FALSE(sim.finale_running());
}

#endif // HELIX_ENABLE_SCREENSAVER
