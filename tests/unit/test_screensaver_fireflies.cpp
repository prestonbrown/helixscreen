// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_cost_model.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_canvas.h"
#include "screensaver_fireflies.h"
#include "screensaver_fireflies_sim.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_registry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::test::diff_against;
using helix::test::FrameDiff;
using helix::test::lit_pixels;
using helix::test::non_opaque_pixels;
using helix::test::TestFrame;
using helix::ui::DirtyRect;
using helix::ui::FirefliesScreensaver;
using helix::ui::FirefliesSim;
using helix::ui::FrameTarget;
using helix::ui::kFirefliesLevelCount;
using helix::ui::kFirefliesLevels;
using helix::ui::kFirefliesMaxBaits;
using helix::ui::length;
using helix::ui::PixelFormat;
using helix::ui::Rgb;
using helix::ui::SaverVec3;

namespace {

constexpr uint32_t W = 320;
constexpr uint32_t H = 200;
constexpr uint32_t kFrameMs = 33;
/// Motion depends on the canvas's shape alone, so motion cases draw into W x H scaled down.
constexpr uint32_t kMotionW = 32;
constexpr uint32_t kMotionH = 20;
/// Motion is the same at every rung, and the bottom one draws the fewest flies, so long motion
/// runs step there.
constexpr size_t kMotionLevel = kFirefliesLevelCount - 1;

/// A sim and its frame.
struct Swarm : helix::test::SimRun<FirefliesSim> {
    Swarm(uint32_t seed, size_t level = 0, PixelFormat format = PixelFormat::XRGB8888,
          uint32_t w = kMotionW, uint32_t h = kMotionH)
        : SimRun(seed, w, h, format) {
        init(level);
    }

    void step(uint32_t dt_ms = kFrameMs) {
        SimRun::step(dt_ms);
    }

    /// Steps `frame_ms` frames until `ms` have passed.
    void run_for(uint32_t ms, uint32_t frame_ms = kFrameMs) {
        for (uint32_t t = 0; t < ms; t += frame_ms) {
            step(frame_ms);
        }
    }
};

bool same_vec(const SaverVec3& a, const SaverVec3& b) {
    return screensaver_same_bits(a.x, b.x) && screensaver_same_bits(a.y, b.y) &&
           screensaver_same_bits(a.z, b.z);
}

/// True when every bait and fly of the two sims sits at the same bits.
bool same_positions(const FirefliesSim& a, const FirefliesSim& b) {
    if (a.flies().size() != b.flies().size() || a.baits().size() != b.baits().size()) {
        return false;
    }
    for (size_t i = 0; i < a.flies().size(); i++) {
        if (!same_vec(a.flies()[i].pos, b.flies()[i].pos)) {
            return false;
        }
    }
    for (size_t i = 0; i < a.baits().size(); i++) {
        if (a.baits()[i].alive != b.baits()[i].alive ||
            !same_vec(a.baits()[i].pos, b.baits()[i].pos)) {
            return false;
        }
    }
    return true;
}

} // namespace

// ============================================================================
// FirefliesSim
// ============================================================================

TEST_CASE("a fireflies population has 2 to 5 baits and 100 to 175 flies, each chasing a live bait",
          "[screensaver][fireflies_sim]") {
    std::set<size_t> bait_counts;
    std::set<size_t> fly_counts;
    const float half_w = 40.0f * static_cast<float>(W) / static_cast<float>(H);
    for (uint32_t seed = 1; seed <= 200; seed++) {
        Swarm s(seed);
        const FirefliesSim& sim = s.sim;
        REQUIRE(sim.baits_alive() >= 2);
        REQUIRE(sim.baits_alive() <= 5);
        REQUIRE(sim.flies().size() >= 100);
        REQUIRE(sim.flies().size() <= 175);
        bait_counts.insert(sim.baits_alive());
        fly_counts.insert(sim.flies().size());
        for (const FirefliesSim::Fly& f : sim.flies()) {
            REQUIRE(f.bait < sim.baits().size());
            REQUIRE(sim.baits()[f.bait].alive);
            REQUIRE(f.pace >= 0.75f);
            REQUIRE(f.pace <= 1.0f);
        }
        for (const FirefliesSim::Bait& b : sim.baits()) {
            if (!b.alive) {
                continue;
            }
            REQUIRE(std::fabs(b.pos.x) <= half_w);
            REQUIRE(std::fabs(b.pos.y) <= 40.0f);
            REQUIRE(std::fabs(b.pos.z) <= 25.0f);
        }
    }
    CHECK(bait_counts.count(2) == 1);
    CHECK(bait_counts.count(5) == 1);
    CHECK(fly_counts.size() >= 60);
}

TEST_CASE("a cleared fireflies sim holds nothing", "[screensaver][fireflies_sim]") {
    Swarm s(3);
    CHECK(s.sim.heap_bytes() > 0);
    CHECK(s.sim.baits().size() == kFirefliesMaxBaits);
    s.sim.clear();
    CHECK(s.sim.heap_bytes() == 0);
}

TEST_CASE("fireflies move identically at 16 ms and 48 ms frames", "[screensaver][fireflies_sim]") {
    // b draws at full size, so the match also holds motion apart from the canvas size.
    Swarm a(11);
    Swarm b(11, 0, PixelFormat::XRGB8888, W, H);
    for (int i = 0; i < 300; i++) {
        a.step(16);
    }
    for (int i = 0; i < 100; i++) {
        b.step(48);
    }
    REQUIRE(a.sim.elapsed_ms() == 4800);
    REQUIRE(b.sim.elapsed_ms() == 4800);
    CHECK(same_positions(a.sim, b.sim));
    // The flies did move, so matching positions mean matching motion.
    Swarm still(11);
    CHECK_FALSE(same_positions(a.sim, still.sim));
}

TEST_CASE("a fireflies level change leaves the motion untouched", "[screensaver][fireflies_sim]") {
    Swarm a(12);
    Swarm b(12);
    a.run_for(2000);
    b.run_for(2000);
    b.sim.request_level(helix::ui::kFirefliesLevelCount - 1);
    a.run_for(4000);
    b.run_for(4000);
    REQUIRE(b.sim.level() == helix::ui::kFirefliesLevelCount - 1);
    CHECK(a.sim.elapsed_ms() == b.sim.elapsed_ms());
    CHECK(same_positions(a.sim, b.sim));
}

namespace {

float mean_distance_to_bait(const FirefliesSim& sim) {
    double sum = 0;
    for (const FirefliesSim::Fly& f : sim.flies()) {
        sum += length(f.pos - sim.baits()[f.bait].pos);
    }
    return static_cast<float>(sum / static_cast<double>(sim.flies().size()));
}

} // namespace

TEST_CASE("flies close in on a stopped bait", "[screensaver][fireflies_sim]") {
    Swarm s(13);
    REQUIRE(s.sim.begin_mode(helix::ui::FirefliesMode::STOP, 10000, s.rng));
    const float start_mean = mean_distance_to_bait(s.sim);
    REQUIRE(start_mean > 30.0f);
    s.run_for(8000);
    REQUIRE(s.sim.mode() == helix::ui::FirefliesMode::STOP);
    const float end_mean = mean_distance_to_bait(s.sim);
    CHECK(end_mean <= 25.0f);
    CHECK(end_mean <= 0.5f * start_mean);
}

TEST_CASE("baits and flies keep to their top speeds", "[screensaver][fireflies_sim]") {
    using helix::ui::kFirefliesBaitSpeed;
    using helix::ui::kFirefliesFlySpeed;
    Swarm s(14, kMotionLevel);
    const float half_w = 40.0f * static_cast<float>(W) / static_cast<float>(H);
    float fastest_fly = 0.0f;
    // Samples outside a limit over the whole run, and the step of the first. Each is
    // the original bound negated, so a NaN counts as a violation. One assertion per
    // limit at the end keeps a million-sample sweep out of the --success report.
    struct Violations {
        int count = 0;
        int first_step = -1;
        void take(bool within, int i) {
            if (!within) {
                count++;
                first_step = first_step < 0 ? i : first_step;
            }
        }
    };
    Violations bait_speed, bait_box, fly_speed;
    for (int i = 0; i < 6000; i++) {
        s.step(helix::ui::kFirefliesStepMs);
        const float mult = s.sim.mode() == helix::ui::FirefliesMode::HYPERSPEED ? 3.0f : 1.0f;
        for (const FirefliesSim::Bait& b : s.sim.baits()) {
            if (!b.alive) {
                continue;
            }
            bait_speed.take(length(b.vel) <= kFirefliesBaitSpeed * mult + 1e-3f, i);
            bait_box.take(std::fabs(b.pos.x) <= half_w + 1.0f &&
                              std::fabs(b.pos.y) <= 40.0f + 1.0f &&
                              std::fabs(b.pos.z) <= 25.0f + 1.0f,
                          i);
        }
        for (const FirefliesSim::Fly& f : s.sim.flies()) {
            const float speed = length(f.vel);
            fly_speed.take(speed <= kFirefliesFlySpeed * f.pace * mult + 1e-3f, i);
            fastest_fly = std::max(fastest_fly, speed);
        }
    }
    CAPTURE(bait_speed.first_step, bait_box.first_step, fly_speed.first_step);
    REQUIRE(bait_speed.count == 0);
    REQUIRE(bait_box.count == 0);
    REQUIRE(fly_speed.count == 0);
    // The flies reached their top speeds, so the cap is what held them.
    CHECK(fastest_fly > 0.7f * kFirefliesFlySpeed);
}

TEST_CASE("a stall advances the swarm a capped step", "[screensaver][fireflies_sim]") {
    Swarm s(15);
    s.step(5000);
    CHECK(s.sim.elapsed_ms() == 500);
}

namespace {

/// `deg` wrapped into [-180, 180).
float wrap180(float deg) {
    float d = std::fmod(deg + 180.0f, 360.0f);
    if (d < 0.0f) {
        d += 360.0f;
    }
    return d - 180.0f;
}

} // namespace

TEST_CASE("each swarm's hue turns 15 degrees a second", "[screensaver][fireflies_sim]") {
    Swarm s(21);
    std::vector<float> before;
    for (const FirefliesSim::Bait& b : s.sim.baits()) {
        before.push_back(b.hue_deg);
    }
    s.run_for(2000, 10);
    REQUIRE(s.sim.elapsed_ms() == 2000);
    for (size_t i = 0; i < s.sim.baits().size(); i++) {
        const FirefliesSim::Bait& b = s.sim.baits()[i];
        if (!b.alive) {
            continue;
        }
        CAPTURE(i, before[i], b.hue_deg);
        CHECK(b.hue_deg >= 0.0f);
        CHECK(b.hue_deg < 360.0f);
        CHECK(std::fabs(wrap180(b.hue_deg - before[i] - 30.0f)) < 0.01f);
    }
}

TEST_CASE("a fly's hue swings 20 degrees either way with its speed",
          "[screensaver][fireflies_sim]") {
    Swarm s(23);
    s.run_for(3000);
    float lowest = 0.0f;
    float highest = 0.0f;
    for (size_t i = 0; i < s.sim.flies().size(); i++) {
        const FirefliesSim::Fly& f = s.sim.flies()[i];
        const float offset = wrap180(s.sim.fly_hue_deg(i) - s.sim.baits()[f.bait].hue_deg);
        const float expected =
            -20.0f + 40.0f * std::min(1.0f, length(f.vel) / helix::ui::kFirefliesFlySpeed);
        CAPTURE(i, offset, expected);
        REQUIRE(s.sim.fly_hue_deg(i) >= 0.0f);
        REQUIRE(s.sim.fly_hue_deg(i) < 360.0f);
        CHECK(std::fabs(offset - expected) < 0.01f);
        lowest = std::min(lowest, offset);
        highest = std::max(highest, offset);
    }
    CHECK(lowest < -5.0f);
    CHECK(highest > 5.0f);
}

TEST_CASE("a fly's tail grows to 2.25 s of history and stops there",
          "[screensaver][fireflies_sim]") {
    Swarm s(24, kMotionLevel);
    const auto all_ages_in = [&](uint32_t lo, uint32_t hi) {
        return s.sim.tail_age_ms() >= lo && s.sim.tail_age_ms() < hi;
    };
    CHECK(all_ages_in(0, 1));
    s.run_for(1000, 10);
    CHECK(all_ages_in(1000, 1001));
    s.run_for(4000, 10);
    REQUIRE(s.sim.elapsed_ms() == 5000);
    CHECK(all_ages_in(2250, 2300));
    s.run_for(40, 10);
    CHECK(all_ages_in(2250, 2300));
    CHECK(s.sim.tail_age_ms() == 2290);
    s.run_for(24960, 10);
    REQUIRE(s.sim.elapsed_ms() == 30000);
    CHECK(all_ages_in(2250, 2300));
}

TEST_CASE("a tail is widest and brightest in its middle and dims toward its old end",
          "[screensaver][fireflies_sim]") {
    using helix::ui::fireflies_tail_brightness;
    using helix::ui::fireflies_tail_width;
    const auto b = fireflies_tail_brightness;
    CHECK(b(0.0f) == 0.0f);
    CHECK(std::fabs(b(1.0f)) < 1e-6f);
    CHECK(b(0.5f) > b(0.1f));
    CHECK(b(0.5f) > b(0.9f));
    CHECK(b(0.9f) < b(0.1f));
    CHECK(b(-0.1f) == 0.0f);
    CHECK(b(1.1f) == 0.0f);
    CHECK(fireflies_tail_width(0.5f, 3.0f) == 3);
    CHECK(fireflies_tail_width(0.0f, 3.0f) == 1);
    CHECK(fireflies_tail_width(0.5f, 1.0f) == 1);
    CHECK(fireflies_tail_width(0.25f, 5.0f) == 4);
}

TEST_CASE("the wind takes a new heading every 4 to 8 tail lengths",
          "[screensaver][fireflies_sim]") {
    Swarm s(22, kMotionLevel);
    std::vector<uint64_t> changes;
    uint32_t seen = 0;
    float strongest_calm = 0.0f;
    double total = 0.0;
    int samples = 0;
    for (uint32_t t = 0; t < 200000; t += 100) {
        s.step(100);
        if (s.sim.wind_changes() != seen) {
            REQUIRE(s.sim.wind_changes() == seen + 1);
            seen = s.sim.wind_changes();
            changes.push_back(s.sim.last_wind_change_ms());
        }
        if (s.sim.mode() != helix::ui::FirefliesMode::WIND_GUST) {
            strongest_calm = std::max(strongest_calm, length(s.sim.wind()));
        }
        total += length(s.sim.wind());
        samples++;
    }
    REQUIRE(s.sim.wind_changes() >= 10);
    CHECK(changes[0] >= 9000);
    CHECK(changes[0] <= 18000);
    for (size_t i = 1; i < changes.size(); i++) {
        CAPTURE(i);
        CHECK(changes[i] - changes[i - 1] >= 9000);
        CHECK(changes[i] - changes[i - 1] <= 18000);
    }
    CHECK(strongest_calm <= 6.0f + 1e-3f);
    // Blowing at all: the tails bend.
    CHECK(total / samples > 1.5);
}

namespace {

using helix::ui::FirefliesMode;

std::vector<size_t> swarm_sizes(const FirefliesSim& sim) {
    std::vector<size_t> sizes(sim.baits().size(), 0);
    for (const FirefliesSim::Fly& f : sim.flies()) {
        sizes[f.bait]++;
    }
    return sizes;
}

std::vector<uint8_t> fly_baits(const FirefliesSim& sim) {
    std::vector<uint8_t> out;
    for (const FirefliesSim::Fly& f : sim.flies()) {
        out.push_back(f.bait);
    }
    return out;
}

bool every_fly_chases_a_live_bait(const FirefliesSim& sim) {
    return std::all_of(sim.flies().begin(), sim.flies().end(), [&](const FirefliesSim::Fly& f) {
        return f.bait < sim.baits().size() && sim.baits()[f.bait].alive;
    });
}

} // namespace

TEST_CASE("ten minutes of fireflies runs every mode and never orphans a fly",
          "[screensaver][fireflies_sim]") {
    Swarm s(31, kMotionLevel, PixelFormat::XRGB8888, 20, 12);
    for (uint32_t t = 0; t < 600000; t += 100) {
        s.step(100);
        REQUIRE(s.sim.baits_alive() >= 2);
        REQUIRE(s.sim.baits_alive() <= 5);
        REQUIRE(every_fly_chases_a_live_bait(s.sim));
    }
    for (const FirefliesMode m :
         {FirefliesMode::STOP, FirefliesMode::LOOP, FirefliesMode::GLOW, FirefliesMode::HYPERSPEED,
          FirefliesMode::SPLIT, FirefliesMode::MERGE, FirefliesMode::WIND_GUST}) {
        CAPTURE(static_cast<int>(m));
        CHECK(s.sim.modes_begun(m) >= 1);
    }
    CHECK(s.sim.modes_begun(FirefliesMode::NORMAL) == 0);
}

TEST_CASE("a split at five baits and a merge at two are refused", "[screensaver][fireflies_sim]") {
    Swarm s(32);
    for (int k = 0; k < 5 && s.sim.baits_alive() < kFirefliesMaxBaits; k++) {
        REQUIRE(s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng));
    }
    REQUIRE(s.sim.baits_alive() == kFirefliesMaxBaits);
    const std::vector<uint8_t> at_five = fly_baits(s.sim);
    const uint32_t splits = s.sim.modes_begun(FirefliesMode::SPLIT);
    CHECK_FALSE(s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng));
    CHECK(s.sim.baits_alive() == kFirefliesMaxBaits);
    CHECK(fly_baits(s.sim) == at_five);
    CHECK(s.sim.modes_begun(FirefliesMode::SPLIT) == splits);

    for (int k = 0; k < 5 && s.sim.baits_alive() > helix::ui::kFirefliesMinBaits; k++) {
        REQUIRE(s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng));
        REQUIRE(every_fly_chases_a_live_bait(s.sim));
    }
    REQUIRE(s.sim.baits_alive() == helix::ui::kFirefliesMinBaits);
    const std::vector<uint8_t> at_two = fly_baits(s.sim);
    CHECK_FALSE(s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng));
    CHECK(s.sim.baits_alive() == helix::ui::kFirefliesMinBaits);
    CHECK(fly_baits(s.sim) == at_two);
}

TEST_CASE("a split hands every other fly to a new bait", "[screensaver][fireflies_sim]") {
    Swarm s(33);
    s.run_for(1000);
    if (s.sim.baits_alive() == kFirefliesMaxBaits) {
        REQUIRE(s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng));
    }
    REQUIRE(s.sim.baits_alive() < kFirefliesMaxBaits);
    const std::vector<size_t> before = swarm_sizes(s.sim);
    size_t parent = 0;
    for (size_t i = 0; i < before.size(); i++) {
        if (s.sim.baits()[i].alive && before[i] > before[parent]) {
            parent = i;
        }
    }
    const FirefliesSim::Bait parent_bait = s.sim.baits()[parent];
    std::vector<bool> was_alive;
    for (const FirefliesSim::Bait& b : s.sim.baits()) {
        was_alive.push_back(b.alive);
    }

    REQUIRE(s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng));
    CHECK(s.sim.mode() == FirefliesMode::NORMAL);
    size_t born = kFirefliesMaxBaits;
    for (size_t i = 0; i < was_alive.size(); i++) {
        if (!was_alive[i] && s.sim.baits()[i].alive) {
            born = i;
        }
    }
    REQUIRE(born < kFirefliesMaxBaits);
    const std::vector<size_t> after = swarm_sizes(s.sim);
    const size_t c = before[parent];
    CHECK(after[parent] == (c + 1) / 2);
    CHECK(after[born] == c / 2);
    const FirefliesSim::Bait& child = s.sim.baits()[born];
    CHECK(std::fabs(wrap180(child.hue_deg - parent_bait.hue_deg - 120.0f)) < 0.01f);
    CHECK(same_vec(child.pos, parent_bait.pos));
    CHECK(child.vel.x == -parent_bait.vel.x);
    CHECK(every_fly_chases_a_live_bait(s.sim));

    // The split swarms go on drawing their hues apart by the same 120 degrees.
    s.run_for(1000, 10);
    CHECK(std::fabs(wrap180(s.sim.baits()[born].hue_deg - s.sim.baits()[parent].hue_deg - 120.0f)) <
          0.01f);
}

TEST_CASE("a merge moves the smallest swarm to the nearest other bait",
          "[screensaver][fireflies_sim]") {
    Swarm s(34);
    s.run_for(1000);
    for (int k = 0; k < 5 && s.sim.baits_alive() < 4; k++) {
        REQUIRE(s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng));
    }
    REQUIRE(s.sim.baits_alive() >= 4);
    s.run_for(1000);
    const std::vector<size_t> before = swarm_sizes(s.sim);
    size_t smallest = kFirefliesMaxBaits;
    for (size_t i = 0; i < before.size(); i++) {
        if (s.sim.baits()[i].alive &&
            (smallest == kFirefliesMaxBaits || before[i] < before[smallest])) {
            smallest = i;
        }
    }
    size_t nearest = kFirefliesMaxBaits;
    for (size_t i = 0; i < before.size(); i++) {
        if (i == smallest || !s.sim.baits()[i].alive) {
            continue;
        }
        if (nearest == kFirefliesMaxBaits ||
            length(s.sim.baits()[i].pos - s.sim.baits()[smallest].pos) <
                length(s.sim.baits()[nearest].pos - s.sim.baits()[smallest].pos)) {
            nearest = i;
        }
    }
    REQUIRE(nearest < kFirefliesMaxBaits);
    const size_t alive = s.sim.baits_alive();

    REQUIRE(s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng));
    const std::vector<size_t> after = swarm_sizes(s.sim);
    CHECK(s.sim.baits_alive() == alive - 1);
    CHECK_FALSE(s.sim.baits()[smallest].alive);
    CHECK(after[smallest] == 0);
    CHECK(after[nearest] == before[nearest] + before[smallest]);
    CHECK(every_fly_chases_a_live_bait(s.sim));
}

TEST_CASE("stop halts the baits, loop circles them, hyperspeed triples them, a gust quadruples "
          "the wind",
          "[screensaver][fireflies_sim]") {
    using helix::ui::kFirefliesFlySpeed;
    const auto alive_baits = [](const FirefliesSim& sim) {
        std::vector<size_t> out;
        for (size_t i = 0; i < sim.baits().size(); i++) {
            if (sim.baits()[i].alive) {
                out.push_back(i);
            }
        }
        return out;
    };

    SECTION("stop") {
        Swarm s(35);
        s.run_for(1000);
        REQUIRE(s.sim.begin_mode(FirefliesMode::STOP, 5000, s.rng));
        s.run_for(1000);
        REQUIRE(s.sim.mode() == FirefliesMode::STOP);
        for (const size_t i : alive_baits(s.sim)) {
            CHECK(length(s.sim.baits()[i].vel) < 0.5f);
        }
    }

    SECTION("loop") {
        Swarm s(36);
        s.run_for(1000);
        REQUIRE(s.sim.begin_mode(FirefliesMode::LOOP, 6000, s.rng));
        s.run_for(1000, 10);
        const std::vector<size_t> baits = alive_baits(s.sim);
        std::vector<float> travelled(kFirefliesMaxBaits, 0.0f);
        for (int i = 0; i < 300; i++) {
            std::vector<SaverVec3> was;
            for (const FirefliesSim::Bait& b : s.sim.baits()) {
                was.push_back(b.pos);
            }
            s.step(10);
            REQUIRE(s.sim.mode() == FirefliesMode::LOOP);
            for (const size_t b : baits) {
                const FirefliesSim::Bait& bait = s.sim.baits()[b];
                REQUIRE(length(bait.pos - bait.loop_centre) <= 20.0f);
                travelled[b] += length(bait.pos - was[b]);
            }
        }
        for (const size_t b : baits) {
            CHECK(travelled[b] >= 60.0f);
        }
    }

    SECTION("hyperspeed") {
        Swarm s(37);
        s.run_for(1000);
        REQUIRE(s.sim.begin_mode(FirefliesMode::HYPERSPEED, 3000, s.rng));
        float fastest = 0.0f;
        for (int i = 0; i < 100; i++) {
            s.step(10);
            for (const FirefliesSim::Fly& f : s.sim.flies()) {
                fastest = std::max(fastest, length(f.vel));
            }
        }
        CHECK(fastest > 1.5f * kFirefliesFlySpeed);
        s.run_for(2000, 10);
        REQUIRE(s.sim.mode() == FirefliesMode::NORMAL);
        s.run_for(2000, 10);
        for (const FirefliesSim::Fly& f : s.sim.flies()) {
            CHECK(length(f.vel) <= kFirefliesFlySpeed + 1e-3f);
        }
    }

    SECTION("wind gust") {
        Swarm s(38);
        s.run_for(1000);
        const float calm = length(s.sim.wind());
        REQUIRE(calm > 0.0f);
        REQUIRE(s.sim.begin_mode(FirefliesMode::WIND_GUST, 4000, s.rng));
        s.run_for(1000, 10);
        CHECK(length(s.sim.wind()) >= 3.0f * calm);
        s.run_for(3500, 10);
        REQUIRE(s.sim.mode() == FirefliesMode::NORMAL);
        CHECK(length(s.sim.wind()) <= 6.0f + 1e-3f);
    }
}

namespace {

/// Sum of every channel of every pixel of `frame`.
uint64_t light_sum(const TestFrame& frame) {
    const helix::ui::PixelWriter writer(frame.target);
    uint64_t sum = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(frame.target.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(frame.target.w); x++) {
            const Rgb c = writer.get(x, y);
            sum += static_cast<uint64_t>(c.r) + c.g + c.b;
        }
    }
    return sum;
}

} // namespace

TEST_CASE("every changed fireflies pixel lies in a dirty box", "[screensaver][fireflies_sim]") {
    for (const PixelFormat format : {PixelFormat::XRGB8888, PixelFormat::RGB565}) {
        for (const size_t level : {size_t{0}, helix::ui::kFirefliesLevelCount - 1}) {
            CAPTURE(static_cast<int>(format), level);
            Swarm s(41, level, format, W, H);
            bool changed_any = false;
            // Split first unless every slot is taken; the other change follows.
            const bool split_first = s.sim.baits_alive() < kFirefliesMaxBaits;
            bool split = false;
            bool merged = false;
            for (int frame = 0; frame < 300; frame++) {
                if (frame == (split_first ? 100 : 200)) {
                    split = s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng);
                }
                if (frame == (split_first ? 200 : 100)) {
                    merged = s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng);
                }
                // GLOW draws halos; a level change drops flies whose last tiles must clear.
                if (frame == 50) {
                    REQUIRE(s.sim.begin_mode(FirefliesMode::GLOW, 3000, s.rng));
                }
                if (frame == 150) {
                    s.sim.request_level(level == 0 ? helix::ui::kFirefliesLevelCount - 1 : 0);
                }
                const std::vector<uint8_t> before = s.frame.snapshot();
                s.step();
                REQUIRE(s.dirty.size() <= helix::ui::kFirefliesMaxDirtyAreas);
                const FrameDiff diff = diff_against(before, s.frame, s.dirty);
                changed_any = changed_any || diff.changed > 0;
                CAPTURE(frame);
                REQUIRE(diff.uncovered == 0);
            }
            CHECK(split);
            CHECK(merged);
            CHECK(changed_any);
        }
    }
}

TEST_CASE("fireflies draw only light on black, and never past a row",
          "[screensaver][fireflies_sim]") {
    constexpr uint8_t kUntouched = 0xAA;
    constexpr uint32_t OW = 321;
    constexpr uint32_t kOh = 203;
    for (const PixelFormat format : {PixelFormat::XRGB8888, PixelFormat::RGB565}) {
        CAPTURE(static_cast<int>(format));
        // Every byte starts untouched, a guard row lies below the frame, and the canvas hands a
        // saver a black frame.
        TestFrame frame(OW, kOh, format, 7, kUntouched, kUntouched);
        helix::ui::PixelWriter(frame.target).fill({0, 0, 0});
        std::minstd_rand rng(43);
        FirefliesSim sim;
        std::vector<DirtyRect> dirty;
        sim.init(frame.target, rng, 0);
        for (int i = 0; i < 150; i++) {
            sim.step(kFrameMs, frame.target, rng, dirty);
        }
        CHECK(frame.padding_intact());
        if (format == PixelFormat::XRGB8888) {
            CHECK(non_opaque_pixels(frame.target) == 0);
        }
        CHECK(lit_pixels(frame.target) > 0);
    }
}

TEST_CASE("an RGB565 swarm shows at least 60% of the light an XRGB8888 one does",
          "[screensaver][fireflies_sim]") {
    Swarm wide(44, 0, PixelFormat::XRGB8888, W, H);
    Swarm narrow(44, 0, PixelFormat::RGB565, W, H);
    wide.run_for(3000);
    narrow.run_for(3000);
    const size_t wide_lit = lit_pixels(wide.frame.target);
    REQUIRE(wide_lit > 500);
    CHECK(static_cast<double>(lit_pixels(narrow.frame.target)) >=
          0.6 * static_cast<double>(wide_lit));
}

TEST_CASE("glow brightens the swarm", "[screensaver][fireflies_sim]") {
    Swarm plain(42, 0, PixelFormat::XRGB8888, W, H);
    Swarm glowing(42, 0, PixelFormat::XRGB8888, W, H);
    REQUIRE(glowing.sim.begin_mode(FirefliesMode::GLOW, 5000, glowing.rng));
    plain.run_for(3000);
    glowing.run_for(3000);
    REQUIRE(glowing.sim.mode() == FirefliesMode::GLOW);
    REQUIRE(same_positions(plain.sim, glowing.sim));
    plain.step(0);
    glowing.step(0);
    const uint64_t plain_sum = light_sum(plain.frame);
    REQUIRE(plain_sum > 0);
    CHECK(static_cast<double>(light_sum(glowing.frame)) >= 1.3 * static_cast<double>(plain_sum));
}

// 60% is the design limit, not a measurement with headroom: level 0 runs close to it, and a
// change that pushes past it should be a deliberate retuning of the tiles or the ladder.
TEST_CASE("a fireflies frame dirties a fraction of the canvas", "[screensaver][fireflies_sim]") {
    Swarm s(45, 0, PixelFormat::XRGB8888, 800, 480);
    double dirty_px = 0;
    int frames = 0;
    for (uint32_t t = 0; t < 10000; t += helix::ui::kSaverFastPeriod) {
        s.step(helix::ui::kSaverFastPeriod);
        for (const DirtyRect& r : s.dirty) {
            dirty_px += static_cast<double>(r.area());
        }
        frames++;
    }
    CHECK(dirty_px / frames < 0.6 * 800.0 * 480.0);
}

TEST_CASE("a fireflies level change applies at the next frame and frames allocate nothing",
          "[screensaver][fireflies_sim]") {
    Swarm s(51);
    const size_t at_init = s.sim.heap_bytes();
    REQUIRE(at_init > 0);
    for (size_t level = 0; level <= kFirefliesLevelCount; level++) {
        CAPTURE(level);
        s.sim.request_level(level);
        s.step();
        CHECK(s.sim.level() == std::min(level, kFirefliesLevelCount - 1));
        for (int i = 0; i < 20; i++) {
            s.step();
            REQUIRE(s.sim.heap_bytes() == at_init);
        }
    }
}

TEST_CASE("each fireflies rung lights less than the one above", "[screensaver][fireflies_sim]") {
    std::vector<size_t> lit;
    for (size_t level = 0; level < kFirefliesLevelCount; level++) {
        Swarm s(52, level, PixelFormat::XRGB8888, 800, 480);
        s.run_for(4000);
        lit.push_back(lit_pixels(s.frame.target));
    }
    for (size_t level = 2; level < kFirefliesLevelCount; level++) {
        CAPTURE(level, lit[level - 1], lit[level]);
        CHECK(lit[level] < lit[level - 1]);
    }
}

TEST_CASE("a fireflies level draws only its first flies", "[screensaver][fireflies_sim]") {
    // Straight after init every tail is collapsed on its fly, so a frame shows bodies alone.
    const auto bodies_lit = [](size_t level) {
        Swarm s(54, level, PixelFormat::XRGB8888, 800, 480);
        s.step(0);
        return lit_pixels(s.frame.target);
    };
    const size_t all = bodies_lit(2);
    const size_t few = bodies_lit(kFirefliesLevelCount - 1);
    REQUIRE(all > 0);
    CAPTURE(all, few);
    CHECK(few > 0);
    CHECK(static_cast<double>(few) < 0.5 * static_cast<double>(all));
}

namespace {

/// What a level's frames cost a board, per the K1C model, scaled to the level's frame period.
double fireflies_cost(size_t level) {
    Swarm s(53, level, PixelFormat::RGB565, 800, 480);
    const uint32_t period = kFirefliesLevels[level].period_ms;
    helix::test::SaverLoad load;
    constexpr int kFrames = 240;
    for (int i = 0; i < kFrames; i++) {
        s.step(period);
        load.add_frame(s.dirty);
    }
    load.average_over(kFrames);
    return load.cost() * 33.0 / static_cast<double>(period);
}

} // namespace

// The model prices only the redraw LVGL does for the dirty areas, not the sim's own drawing into
// the canvas, so it understates fireflies; the device gate is the real measure.
TEST_CASE("the fireflies bottom rung's redraw fits a 1-core board's budget",
          "[screensaver][fireflies_sim]") {
    const double bottom = fireflies_cost(kFirefliesLevelCount - 1);
    const double top = fireflies_cost(0);
    CAPTURE(bottom, top);
    CHECK(bottom < helix::test::budget_pct(1));
    CHECK(top > bottom);
}

TEST_CASE("a new wind heading during a gust never compounds the gust",
          "[screensaver][fireflies_sim]") {
    constexpr float kWindMax = 6.0f;
    constexpr float kGustMax = 4.0f * kWindMax;
    for (uint32_t seed = 1; seed <= 8; seed++) {
        CAPTURE(seed);
        Swarm s(seed, kMotionLevel, PixelFormat::XRGB8888, 20, 12);
        // Gust back to back, so every heading change lands inside a gust.
        const uint32_t changes_before = s.sim.wind_changes();
        float strongest = 0.0f;
        for (uint32_t t = 0; t < 60000; t += 100) {
            if (s.sim.mode() == FirefliesMode::NORMAL) {
                REQUIRE(s.sim.begin_mode(FirefliesMode::WIND_GUST, 4000, s.rng));
            }
            s.step(100);
            strongest = std::max(strongest, length(s.sim.wind()));
        }
        REQUIRE(s.sim.wind_changes() >= changes_before + 3);
        CHECK(strongest <= kGustMax + 1e-3f);
        // The last gust ends; the calm wind is back within its bounds at once and stays there.
        s.run_for(4000, 100);
        for (uint32_t t = 0; t < 3000; t += 100) {
            s.step(100);
            REQUIRE(s.sim.mode() == FirefliesMode::NORMAL);
            CHECK(length(s.sim.wind()) <= kWindMax + 1e-3f);
        }
    }
}

// ============================================================================
// FirefliesScreensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a fireflies level request applies at once, frame period included",
                 "[screensaver][screensaver_fireflies]") {
    FirefliesScreensaver ss;
    ScreensaverStopOnExit<FirefliesScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 4);
    ss.start();
    REQUIRE(ss.is_active());

    const size_t last = kFirefliesLevelCount - 1;
    ss.request_level(last);
    CHECK(ss.level() == last);
    CHECK(SaverTestAccess::timer(ss)->period == kFirefliesLevels[last].period_ms);
    advance_saver(ss, 100);
    CHECK(ss.is_active());
    CHECK(ss.sim().level() == last);
}

#endif // HELIX_ENABLE_SCREENSAVER
