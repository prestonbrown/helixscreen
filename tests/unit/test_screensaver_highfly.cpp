// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_highfly.h"
#include "screensaver_highfly_sim.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_registry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::ui {

/// Moves a HighFlySim's clocks, for runs far longer than a test can step through.
class HighFlySimTestAccess {
  public:
    static void skip_to(HighFlySim& sim, double t) {
        const double delta = t - sim.t_;
        sim.t_ = t;
        sim.fly_start_s_ += delta;
        sim.next_tower_at_ += delta;
        for (auto& tower : sim.towers_) {
            tower.entered_s += delta;
        }
    }
    /// Moves the next fly-through's start to `t` seconds into the run.
    static void set_fly_start(HighFlySim& sim, double t) {
        sim.fly_start_s_ = t;
    }
    /// Flattens the wisp table, so a fly-through shows its interior alone.
    static void still_wisps(HighFlySim& sim) {
        std::fill(sim.wisp_.begin(), sim.wisp_.end(), 0.0f);
    }
    /// Makes the next step repaint the whole canvas, as the first frame does.
    static void repaint_all(HighFlySim& sim) {
        sim.painted_ = false;
    }
};

/// Reads the text lines a HighFlyScreensaver owns.
class HighFlyScreensaverTestAccess {
  public:
    static const SaverLabel& date_line(const HighFlyScreensaver& saver) {
        return saver.date_line_;
    }
    static const SaverLabel& day_line(const HighFlyScreensaver& saver) {
        return saver.day_line_;
    }
};

} // namespace helix::ui

using helix::ui::build_highfly_field;
using helix::ui::clock_time;
using helix::ui::ClockInput;
using helix::ui::ClockMode;
using helix::ui::ClockPrintPhase;
using helix::ui::ClockReading;
using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::highfly_layer_visibility;
using helix::ui::highfly_whiteout;
using helix::ui::HighFlyClockLayout;
using helix::ui::HighFlyField;
using helix::ui::HighFlyScreensaver;
using helix::ui::HighFlyScreensaverTestAccess;
using helix::ui::HighFlySim;
using helix::ui::HighFlySimTestAccess;
using helix::ui::kHighflyField;
using helix::ui::kHighflyLevelCount;
using helix::ui::kHighflyLevels;
using helix::ui::kHighflyTalus;
using helix::ui::kHighflyTexelsPerUnit;
using helix::ui::kHighflyTowerVariants;
using helix::ui::layout_highfly_clock;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

using helix::test::check_replays;
using helix::test::covers;
using helix::test::diff_against;
using helix::test::reading_at;
using helix::test::TestFrame;
using helix::ui::SaverTestAccess;

namespace {

float luminance(Rgb c) {
    return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}

} // namespace

// ============================================================================
// Field, textures, envelope
// ============================================================================

TEST_CASE("the cloud field tiles and never steps more than the talus slope",
          "[screensaver][highfly]") {
    std::minstd_rand rng(7);
    HighFlyField field;
    build_highfly_field(rng, field);
    REQUIRE(field.height.size() == static_cast<size_t>(kHighflyField) * kHighflyField);
    const int32_t n = kHighflyField;
    float steepest = 0.0f;
    for (int32_t y = 0; y < n; y++) {
        for (int32_t x = 0; x < n; x++) {
            const float h = field.height[static_cast<size_t>(y) * n + x];
            const float right = field.height[static_cast<size_t>(y) * n + (x + 1) % n];
            const float down = field.height[static_cast<size_t>((y + 1) % n) * n + x];
            steepest = std::max({steepest, std::fabs(h - right), std::fabs(h - down)});
        }
    }
    CHECK(steepest <= kHighflyTalus + 1e-4f);
    CHECK(steepest > 0.5f);
}

TEST_CASE("the cloud field has billows tall enough to break the skyline",
          "[screensaver][highfly]") {
    std::minstd_rand rng(7);
    HighFlyField field;
    build_highfly_field(rng, field);
    const auto [lo, hi] = std::minmax_element(field.height.begin(), field.height.end());
    CHECK(*hi / kHighflyTexelsPerUnit > 15.0f);
    CHECK(*lo / kHighflyTexelsPerUnit < 15.0f);
}

TEST_CASE("crevices shade darker than tops", "[screensaver][highfly]") {
    std::minstd_rand rng(7);
    HighFlyField field;
    build_highfly_field(rng, field);
    std::vector<float> sorted = field.height;
    std::sort(sorted.begin(), sorted.end());
    const float low_cut = sorted[sorted.size() / 10];
    const float high_cut = sorted[sorted.size() * 9 / 10];
    double top_sum = 0.0;
    double bottom_sum = 0.0;
    size_t top_n = 0;
    size_t bottom_n = 0;
    for (size_t i = 0; i < field.height.size(); i++) {
        if (field.height[i] >= high_cut) {
            top_sum += field.shade[i];
            top_n++;
        } else if (field.height[i] <= low_cut) {
            bottom_sum += field.shade[i];
            bottom_n++;
        }
    }
    REQUIRE(top_n > 0);
    REQUIRE(bottom_n > 0);
    CHECK(top_sum / top_n > bottom_sum / bottom_n + 0.15);
}

TEST_CASE("a field replays from its seed", "[screensaver][highfly]") {
    HighFlyField a;
    HighFlyField b;
    HighFlyField c;
    std::minstd_rand ra(7);
    std::minstd_rand rb(7);
    std::minstd_rand rc(8);
    build_highfly_field(ra, a);
    build_highfly_field(rb, b);
    build_highfly_field(rc, c);
    CHECK(a.height == b.height);
    CHECK(a.shade == b.shade);
    CHECK(a.height != c.height);
}

TEST_CASE("billboards hide behind nearer deck and show in front of it", "[screensaver][highfly]") {
    CHECK(highfly_layer_visibility(50.0f, 100.0f) == 0.0f);
    CHECK(highfly_layer_visibility(200.0f, 100.0f) == 1.0f);
    CHECK(highfly_layer_visibility(100.0f, 100.0f) == Catch::Approx(0.5f));
    CHECK(highfly_layer_visibility(INFINITY, 100.0f) == 1.0f);
    float last = -1.0f;
    for (float deck = 50.0f; deck <= 200.0f; deck += 1.0f) {
        const float v = highfly_layer_visibility(deck, 100.0f);
        CHECK(v >= last);
        last = v;
    }
}

TEST_CASE("a fly-through eases in, holds and clears", "[screensaver][highfly]") {
    CHECK(highfly_whiteout(-1.0f) == 0.0f);
    CHECK(highfly_whiteout(0.5f) == Catch::Approx(0.5f));
    CHECK(highfly_whiteout(1.0f) == Catch::Approx(1.0f));
    CHECK(highfly_whiteout(1.4f) == Catch::Approx(1.0f));
    CHECK(highfly_whiteout(1.5f + 0.65f) == Catch::Approx(0.5f));
    CHECK(highfly_whiteout(2.8f) == Catch::Approx(0.0f).margin(1e-5));
    CHECK(highfly_whiteout(5.0f) == 0.0f);
    for (float t = -1.0f; t < 4.0f; t += 0.001f) {
        CHECK(std::fabs(highfly_whiteout(t + 0.001f) - highfly_whiteout(t)) < 0.01f);
    }
}

// ============================================================================
// Clock layout
// ============================================================================

TEST_CASE("the high fly clock fits every screen at every drift", "[screensaver][highfly][clock]") {
    struct Size {
        int32_t w, h;
    };
    for (const Size s : {Size{800, 480}, Size{480, 320}, Size{1024, 600}, Size{480, 800}}) {
        CAPTURE(s.w, s.h);
        const int32_t reach_x = std::max(4, s.w / 25);
        const int32_t reach_y = std::max(4, s.h / 25);
        for (int32_t dx : {-reach_x, 0, reach_x}) {
            for (int32_t dy : {-reach_y, 0, reach_y}) {
                CAPTURE(dx, dy);
                const HighFlyClockLayout l = layout_highfly_clock(s.w, s.h, dx, dy);
                CHECK(l.bounds.x1 >= 0);
                CHECK(l.bounds.y1 >= 0);
                CHECK(l.bounds.x2 < s.w);
                CHECK(l.bounds.y2 < s.h);
                CHECK(l.date_line.x2 > l.date_line.x1);
                CHECK(l.date_line.x2 < l.digits[0].x1);
            }
        }
    }
}

TEST_CASE("digit boxes run left to right without overlapping, seconds sit up and to the right",
          "[screensaver][highfly][clock]") {
    const HighFlyClockLayout l = layout_highfly_clock(800, 480, 0, 0);
    for (size_t i = 1; i < l.digits.size(); i++) {
        CHECK(l.digits[i].x1 > l.digits[i - 1].x2);
    }
    CHECK(l.seconds[0].x1 > l.digits[4].x2);
    CHECK(l.seconds[1].x1 > l.seconds[0].x2);
    CHECK(l.seconds[0].y1 == l.digits[0].y1);
    CHECK(l.seconds[0].y2 < l.digits[0].y2);
    CHECK(l.second_unit < l.digit_unit);
    CHECK(l.meridiem.y1 > l.seconds[0].y2);
    // About 110 px numerals at 800 x 480.
    CHECK(l.digits[0].y2 - l.digits[0].y1 + 1 == Catch::Approx(110).margin(2));
}

TEST_CASE("the clock sits at the cloud line", "[screensaver][highfly][clock]") {
    for (const int32_t h : {320, 480, 600}) {
        const int32_t w = h * 5 / 3;
        const HighFlyClockLayout l = layout_highfly_clock(w, h, 0, 0);
        const float line = 0.56f * static_cast<float>(h);
        CHECK(static_cast<float>(l.digits[0].y1) < line + 0.1f * h);
        CHECK(static_cast<float>(l.digits[0].y2) > line - 0.1f * h);
    }
}

// ============================================================================
// HighFlySim
// ============================================================================

TEST_CASE("a high fly run replays exactly from a seed, and another seed differs",
          "[screensaver][highfly]") {
    const auto run = [](uint32_t seed) {
        TestFrame frame(320, 192, PixelFormat::RGB565);
        std::minstd_rand rng(seed);
        HighFlySim sim;
        const ClockReading r = reading_at(10 * 3600);
        sim.init(frame.target, rng, 1, r);
        std::vector<DirtyRect> dirty;
        for (int i = 0; i < 60; i++) {
            sim.step(33, frame.target, rng, r, dirty);
        }
        return frame.bytes;
    };
    check_replays(run, 3, 4);
}

TEST_CASE("the sky above the cloud band repaints only for the clock without towers",
          "[screensaver][highfly]") {
    REQUIRE_FALSE(kHighflyLevels[3].towers);
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    HighFlySim sim;
    const ClockReading r = reading_at(10 * 3600);
    sim.init(frame.target, rng, 3, r);
    std::vector<DirtyRect> dirty;
    size_t checked = 0;
    for (int i = 0; i < 600; i++) {
        const int32_t band_before = sim.band_top();
        sim.step(50, frame.target, rng, r, dirty);
        // The cloud a fly-through enters shows for about half a minute before it.
        if (sim.fly_since_s() >= 0.0f || sim.next_fly_s() < 35.0f || i == 0) {
            continue;
        }
        // A frame repaints the band from wherever its top was last frame or is now.
        const int32_t band = std::min(band_before, sim.band_top()) * sim.scale();
        CHECK(band >= 240 * 35 / 100);
        const DirtyRect clock = sim.clock_layout().bounds;
        DirtyRect padded{clock.x1 - 2 * sim.scale(), clock.y1 - 2 * sim.scale(),
                         clock.x2 + 2 * sim.scale(), clock.y2 + 2 * sim.scale()};
        for (const DirtyRect& d : dirty) {
            if (d.y1 < band) {
                CHECK(helix::test::contains(padded, d));
            }
        }
        checked++;
    }
    CHECK(checked > 100);
}

TEST_CASE("every pixel a high fly frame changes lies in one of its dirty boxes",
          "[screensaver][highfly]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    TestFrame frame(240, 144, format);
    std::minstd_rand rng(3);
    HighFlySim sim;
    int32_t sod = 10 * 3600 + 59 * 60;
    sim.init(frame.target, rng, 0, reading_at(sod));
    std::vector<DirtyRect> dirty;
    size_t changed = 0;
    size_t uncovered = 0;
    bool flew = false;
    // Long enough for towers to come and go and one fly-through.
    for (int i = 0; i < 1300; i++) {
        sod += i % 10 == 0 ? 1 : 0;
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(100, frame.target, rng, reading_at(sod), dirty);
        flew = flew || sim.fly_since_s() > 0.0f;
        const auto d = diff_against(before, frame, dirty);
        changed += d.changed;
        uncovered += d.uncovered;
    }
    CHECK(flew);
    REQUIRE(changed > 0);
    CHECK(uncovered == 0);
    CHECK(frame.padding_intact());
}

TEST_CASE("each frame repaints where the towers and the clock just were, through a busy drift",
          "[screensaver][highfly]") {
    const ClockReading r = reading_at(10 * 3600);
    constexpr int32_t kW = 100;
    constexpr int32_t kH = 60;
    // Find a drift step with both towers up; the towers' schedule does not depend on when the
    // fly-through comes, so a probe run with none finds it.
    double drift_at = -1.0;
    {
        TestFrame probe_frame(kW, kH, PixelFormat::RGB565);
        std::minstd_rand rng(3);
        HighFlySim probe;
        probe.init(probe_frame.target, rng, 0, r);
        HighFlySimTestAccess::set_fly_start(probe, 4000.0);
        std::vector<DirtyRect> dirty;
        DirtyRect clock = probe.clock_layout().bounds;
        for (int i = 0; i < 6000 && drift_at < 0.0; i++) {
            probe.step(100, probe_frame.target, rng, r, dirty);
            if (!(probe.clock_layout().bounds == clock)) {
                clock = probe.clock_layout().bounds;
                if (probe.towers_visible() == 2) {
                    drift_at = probe.elapsed_s();
                }
            }
        }
    }
    REQUIRE(drift_at > 0.0);

    TestFrame frame(kW, kH, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    HighFlySim sim;
    sim.init(frame.target, rng, 0, r);
    // The cloud ahead shows for about 32 s before its whiteout: put that over the drift step.
    HighFlySimTestAccess::set_fly_start(sim, drift_at + 20.0);
    std::vector<DirtyRect> dirty;
    bool busy_drift = false;
    size_t missed = 0;
    for (int i = 0; i < 6000; i++) {
        // Where the towers and the clock were: each frame's dirty boxes must cover all of it,
        // or the old pixels stay on screen as a strip.
        std::vector<DirtyRect> was;
        for (size_t t = 0; t < 2; t++) {
            const DirtyRect b = sim.tower_box(t);
            if (!b.empty()) {
                was.push_back({b.x1 * 2, b.y1 * 2, b.x2 * 2 + 1, b.y2 * 2 + 1});
            }
        }
        const DirtyRect clock_before = sim.clock_layout().bounds;
        was.push_back(clock_before);
        sim.step(100, frame.target, rng, r, dirty);
        const bool drifted = !(sim.clock_layout().bounds == clock_before);
        if (drifted) {
            busy_drift = busy_drift || (sim.towers_visible() == 2 && sim.next_fly_s() < 32.0f);
        }
        for (const DirtyRect& w : was) {
            for (int32_t y = std::max(0, w.y1); y <= std::min(kH - 1, w.y2); y++) {
                for (int32_t x = std::max(0, w.x1); x <= std::min(kW - 1, w.x2); x++) {
                    missed += covers(dirty, x, y) ? 0 : 1;
                }
            }
        }
        if (sim.fly_since_s() >= 0.0f) {
            break; // the whiteout repaints everything anyway
        }
    }
    CHECK(missed == 0);
    CHECK(busy_drift);
}

TEST_CASE("the sun glows at the top centre", "[screensaver][highfly]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    HighFlySim sim;
    sim.init(frame.target, rng, 1, reading_at(10 * 3600));
    const PixelWriter w(frame.target);
    CHECK(luminance(w.get(200, 2)) > luminance(w.get(4, 2)) + 30.0f);
}

TEST_CASE("towers come every 13 to 14 seconds, at most two at once, in more than one shape",
          "[screensaver][highfly]") {
    TestFrame frame(100, 60, PixelFormat::RGB565);
    std::minstd_rand rng(3);
    HighFlySim sim;
    const ClockReading r = reading_at(10 * 3600);
    sim.init(frame.target, rng, 0, r);
    std::vector<DirtyRect> dirty;
    size_t most = 0;
    std::vector<int> variants;
    for (int i = 0; i < 1200; i++) {
        sim.step(100, frame.target, rng, r, dirty);
        most = std::max(most, sim.towers_visible());
        for (size_t t = 0; t < 2; t++) {
            const int v = sim.tower_variant(t);
            if (v >= 0 && std::find(variants.begin(), variants.end(), v) == variants.end()) {
                variants.push_back(v);
            }
        }
    }
    CHECK(most <= 2);
    CHECK(variants.size() >= 2);
    const auto& entries = sim.tower_entries();
    REQUIRE(entries.size() >= 7);
    for (size_t i = 1; i < entries.size(); i++) {
        const float gap = entries[i] - entries[i - 1];
        CHECK(gap >= 13.0f - 1e-3f);
        CHECK(gap <= 14.0f + 1e-3f);
    }
    for (size_t a = 0; a < kHighflyTowerVariants; a++) {
        for (size_t b = a + 1; b < kHighflyTowerVariants; b++) {
            const auto& ta = sim.tower_texture(a).rgba;
            const auto& tb = sim.tower_texture(b).rgba;
            REQUIRE(ta.size() == tb.size());
            size_t differ = 0;
            for (size_t i = 3; i < ta.size(); i += 4) {
                differ += std::abs(static_cast<int>(ta[i]) - static_cast<int>(tb[i])) > 16 ? 1 : 0;
            }
            CHECK(differ * 20 > ta.size() / 4);
        }
    }
}

TEST_CASE("a tower visibly moves from the moment it appears until it leaves",
          "[screensaver][highfly]") {
    TestFrame frame(400, 240, PixelFormat::RGB565);
    std::minstd_rand rng(3);
    HighFlySim sim;
    const ClockReading r = reading_at(10 * 3600);
    sim.init(frame.target, rng, 0, r);
    std::vector<DirtyRect> dirty;
    const size_t entries = sim.tower_entries().size();
    // Run until a new tower enters, then follow its slot: the farthest live tower.
    int slot = -1;
    for (int i = 0; i < 400 && slot < 0; i++) {
        sim.step(50, frame.target, rng, r, dirty);
        if (sim.tower_entries().size() > entries) {
            const DirtyRect a = sim.tower_box(0);
            const DirtyRect b = sim.tower_box(1);
            slot = (b.empty() || (!a.empty() && a.y2 - a.y1 < b.y2 - b.y1)) ? 0 : 1;
        }
    }
    REQUIRE(slot >= 0);
    // A tower fades in from nothing, so it is followed from the first frame it shows in.
    for (int i = 0; i < 20 && sim.tower_box(static_cast<size_t>(slot)).empty(); i++) {
        sim.step(50, frame.target, rng, r, dirty);
    }
    const DirtyRect first = sim.tower_box(static_cast<size_t>(slot));
    REQUIRE_FALSE(first.empty());
    const auto centre = [](const DirtyRect& b) { return (b.x1 + b.x2) / 2; };
    // Three seconds in, it has slid sideways or grown by at least 3% of the internal width.
    for (int i = 0; i < 60; i++) {
        sim.step(50, frame.target, rng, r, dirty);
    }
    const DirtyRect later = sim.tower_box(static_cast<size_t>(slot));
    REQUIRE_FALSE(later.empty());
    const int moved = std::abs(centre(later) - centre(first));
    const int grew = (later.y2 - later.y1) - (first.y2 - first.y1);
    CAPTURE(moved, grew);
    CHECK(std::max(moved, grew) >= 6);
    // It keeps coming on and is gone within 40 s, not parked on the horizon. Leaving shows as
    // the slot going empty, or, if the next tower takes the slot in the same frame, as the
    // tower in it suddenly far smaller: a newcomer at the entry depth.
    bool left = false;
    int32_t height = later.y2 - later.y1;
    for (int i = 0; i < 800 && !left; i++) {
        sim.step(50, frame.target, rng, r, dirty);
        const DirtyRect now = sim.tower_box(static_cast<size_t>(slot));
        const int32_t now_h = now.y2 - now.y1;
        left = now.empty() || now_h * 10 < height * 7;
        height = now_h;
    }
    CHECK(left);
}

TEST_CASE("over the clock the interior and the wisps together leave the numerals readable",
          "[screensaver][highfly]") {
    const float most = helix::ui::kHighflyClockCoverMax;
    for (const float wisp : {0.0f, 0.3f, 0.7f, 1.0f}) {
        for (const float amt : {0.3f, 1.0f}) {
            CAPTURE(wisp, amt);
            // What shows of the scene: the gap between a black and a white scene's results.
            const Rgb dark = helix::ui::highfly_inside(Rgb{0, 0, 0}, amt, wisp, most);
            const Rgb light = helix::ui::highfly_inside(Rgb{255, 255, 255}, amt, wisp, most);
            const float shows = static_cast<float>(light.g - dark.g) / 255.0f;
            CHECK(shows >= 1.0f - most - 0.01f);
            // Away from the clock the wisp is not held back.
            const Rgb free_dark = helix::ui::highfly_inside(Rgb{0, 0, 0}, amt, wisp);
            const Rgb free_light = helix::ui::highfly_inside(Rgb{255, 255, 255}, amt, wisp);
            const float k = amt * helix::ui::kHighflyWhiteoutMax;
            CHECK(static_cast<float>(free_light.g - free_dark.g) / 255.0f ==
                  Catch::Approx((1.0f - k) * (1.0f - wisp)).margin(0.01f));
        }
    }
}

TEST_CASE("high fly still moves smoothly thirty days into a run", "[screensaver][highfly]") {
    // Per-frame change at a rung with no layers, so the deck's scroll is most of it: it must
    // keep coming every frame, not in occasional jumps between still frames.
    const auto changes = [](HighFlySim& sim, TestFrame& frame, std::minstd_rand& rng,
                            const ClockReading& r, int frames) {
        std::vector<DirtyRect> dirty;
        std::vector<double> out;
        for (int i = 0; i < frames; i++) {
            const std::vector<uint8_t> before = frame.bytes;
            sim.step(50, frame.target, rng, r, dirty);
            double sum = 0.0;
            for (size_t b = 0; b < before.size(); b++) {
                sum += std::abs(static_cast<int>(before[b]) - static_cast<int>(frame.bytes[b]));
            }
            out.push_back(sum / static_cast<double>(frame.bytes.size()));
        }
        return out;
    };
    const auto mean = [](const std::vector<double>& v) {
        double sum = 0.0;
        for (const double x : v) {
            sum += x;
        }
        return sum / static_cast<double>(v.size());
    };
    REQUIRE(kHighflyLevels[2].layers == 0);
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    HighFlySim sim;
    const ClockReading r = reading_at(10 * 3600);
    sim.init(frame.target, rng, 2, r);
    changes(sim, frame, rng, r, 20);
    const std::vector<double> early = changes(sim, frame, rng, r, 40);
    HighFlySimTestAccess::skip_to(sim, 30.0 * 86400.0);
    changes(sim, frame, rng, r, 20);
    const std::vector<double> late = changes(sim, frame, rng, r, 40);
    const double early_mean = mean(early);
    const double late_mean = mean(late);
    CAPTURE(early_mean, late_mean);
    REQUIRE(early_mean > 0.05);
    CHECK(late_mean > 0.5 * early_mean);
    CHECK(late_mean < 2.0 * early_mean);
    CHECK(*std::min_element(early.begin(), early.end()) > 0.4 * early_mean);
    CHECK(*std::min_element(late.begin(), late.end()) > 0.4 * late_mean);
}

TEST_CASE("the fly-through interior stops at 62% of the way to blue-grey",
          "[screensaver][highfly]") {
    const Rgb inside{200, 216, 230};
    for (const Rgb scene : {Rgb{12, 78, 124}, Rgb{252, 254, 255}, Rgb{0, 0, 0}}) {
        const Rgb full = helix::ui::highfly_inside(scene, 1.0f);
        CHECK(full.g == Catch::Approx(scene.g + 0.62f * (inside.g - scene.g)).margin(1));
        CHECK(full.b == Catch::Approx(scene.b + 0.62f * (inside.b - scene.b)).margin(1));
        CHECK(helix::ui::highfly_inside(scene, 0.0f) == scene);
        CHECK(helix::ui::highfly_inside(scene, 2.0f) == full);
    }
}

TEST_CASE("a fly-through renders its interior 62% of the way over the scene",
          "[screensaver][highfly]") {
    // Two runs from one seed, the second with its fly-through pushed past the end: at the
    // hold, the first frame is the second's scene taken 62% of the way to the interior.
    TestFrame with(400, 240, PixelFormat::XRGB8888);
    TestFrame without(400, 240, PixelFormat::XRGB8888);
    std::minstd_rand rng_a(3);
    std::minstd_rand rng_b(3);
    HighFlySim a;
    HighFlySim b;
    const ClockReading r = reading_at(10 * 3600);
    a.init(with.target, rng_a, 3, r);
    b.init(without.target, rng_b, 3, r);
    HighFlySimTestAccess::set_fly_start(a, 40.0);
    HighFlySimTestAccess::set_fly_start(b, 4000.0);
    HighFlySimTestAccess::still_wisps(a);
    std::vector<DirtyRect> dirty;
    bool held = false;
    for (int i = 0; i < 600 && !held; i++) {
        a.step(100, with.target, rng_a, r, dirty);
        b.step(100, without.target, rng_b, r, dirty);
        held = a.fly_since_s() > 1.1f && a.fly_since_s() < 1.4f;
    }
    REQUIRE(held);
    const PixelWriter wa(with.target);
    const PixelWriter wb(without.target);
    const DirtyRect clock = a.clock_layout().bounds;
    const Rgb inside{200, 216, 230};
    size_t checked = 0;
    double whitened = 0.0;
    for (int32_t y = 4; y < 236; y += 9) {
        for (int32_t x = 4; x < 396; x += 11) {
            if (x >= clock.x1 && x <= clock.x2 && y >= clock.y1 && y <= clock.y2) {
                continue;
            }
            const Rgb pa = wa.get(x, y);
            const Rgb pb = wb.get(x, y);
            whitened += std::abs(pa.g - pb.g);
            if (std::abs(inside.b - pb.b) < 40) {
                continue; // too near the interior colour already to measure the share
            }
            const float share = (static_cast<float>(pa.b) - pb.b) / (inside.b - pb.b);
            CAPTURE(x, y, static_cast<int>(pa.b), static_cast<int>(pb.b));
            CHECK(share >= 0.59f);
            CHECK(share <= 0.69f);
            checked++;
        }
    }
    CHECK(checked > 40);
    // And the whiteout is there to see: the frames differ by many levels on average.
    CHECK(whitened / 600.0 > 20.0);
}

TEST_CASE("a fly-through comes every one to two minutes", "[screensaver][highfly]") {
    TestFrame frame(100, 60, PixelFormat::RGB565);
    std::minstd_rand rng(3);
    HighFlySim sim;
    const ClockReading r = reading_at(10 * 3600);
    sim.init(frame.target, rng, 3, r);
    std::vector<DirtyRect> dirty;
    const PixelWriter w(frame.target);
    bool whitened = false;
    for (int i = 0; i < 3000; i++) {
        sim.step(100, frame.target, rng, r, dirty);
        const float since = sim.fly_since_s();
        if (since > 1.1f && since < 1.4f) {
            // At the hold the whole screen is at least 62% of the way to the interior.
            const Rgb top = w.get(50, 2);
            CHECK(top.g >= 140);
            whitened = true;
        }
    }
    CHECK(whitened);
    const auto& starts = sim.fly_starts();
    REQUIRE(starts.size() >= 2);
    CHECK(starts[0] >= 60.0f);
    CHECK(starts[0] <= 120.0f);
    for (size_t i = 1; i < starts.size(); i++) {
        const auto gap = static_cast<float>(starts[i] - starts[i - 1]);
        CHECK(gap >= 60.0f - 1e-3f);
        CHECK(gap <= 120.0f + 1e-3f);
    }
}

TEST_CASE("wisps sample smoothly, without radial seams", "[screensaver][highfly]") {
    // Wide, so the clock on the middle row leaves columns clear either side of it.
    constexpr int32_t kW = 800;
    constexpr int32_t kH = 240;
    TestFrame frame(kW, kH, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    HighFlySim sim;
    const ClockReading r = reading_at(10 * 3600);
    sim.init(frame.target, rng, 0, r);
    std::vector<DirtyRect> dirty;
    // Whole seconds up to the cloud ahead, then frames, to the hold of the fly-through.
    while (sim.next_fly_s() > 2.0f) {
        sim.step(1000, frame.target, rng, r, dirty);
    }
    while (sim.fly_since_s() < 1.2f) {
        sim.step(50, frame.target, rng, r, dirty);
    }
    REQUIRE(sim.fly_since_s() < 1.4f);
    const PixelWriter w(frame.target);
    const auto grey = [&](int32_t x, int32_t y) {
        const Rgb c = w.get(x, y);
        return (c.r + c.g + c.b) / 3;
    };
    // The wisps' angle wraps on the ray left of the centre, along the middle row, so the rows
    // either side of it sample opposite ends of the wisp table. Over the interior the scene
    // there is flat: smooth sampling moves a pixel 1 grey level between them, a seam 5 or more.
    const DirtyRect& clock = sim.clock_layout().bounds;
    int most = 0;
    int columns = 0;
    for (int32_t x = 8; x < kW / 2 - 8; x++) {
        if (x >= clock.x1 - 1 && x <= clock.x2 + 1 && clock.y1 <= kH / 2 + 1 &&
            clock.y2 >= kH / 2 - 1) {
            continue;
        }
        most = std::max(most, std::abs(grey(x, kH / 2 - 1) - grey(x, kH / 2 + 1)));
        columns++;
    }
    REQUIRE(columns >= 32);
    CHECK(most <= 3);
}

TEST_CASE("the ladder drops layers before coarsening the deck, slows, and ends standing still",
          "[screensaver][highfly]") {
    CHECK(kHighflyLevels[0].period_ms == helix::ui::kSaverFastPeriod);
    size_t first_no_layers = kHighflyLevelCount;
    size_t first_coarse = kHighflyLevelCount;
    for (size_t i = 0; i < kHighflyLevelCount; i++) {
        if (i > 0) {
            CHECK(kHighflyLevels[i].period_ms >= kHighflyLevels[i - 1].period_ms);
        }
        if (kHighflyLevels[i].layers == 0 && first_no_layers == kHighflyLevelCount) {
            first_no_layers = i;
        }
        if (kHighflyLevels[i].scale == 4 && first_coarse == kHighflyLevelCount) {
            first_coarse = i;
        }
    }
    CHECK(first_no_layers < first_coarse);
    CHECK(kHighflyLevels[kHighflyLevelCount - 1].still);
    // Before it stands still the coarse deck still drifts, at 5 to 10 frames a second.
    const helix::ui::HighFlyLevel& slow = kHighflyLevels[kHighflyLevelCount - 2];
    CHECK_FALSE(slow.still);
    CHECK(slow.scale == 4);
    CHECK(slow.period_ms >= 100);
    CHECK(slow.period_ms <= 200);

    TestFrame frame(400, 240, PixelFormat::RGB565);
    std::minstd_rand rng(3);
    HighFlySim sim;
    int32_t sod = 10 * 3600 + 15 * 60;
    sim.init(frame.target, rng, kHighflyLevelCount - 1, reading_at(sod));
    std::vector<DirtyRect> dirty;
    sim.step(250, frame.target, rng, reading_at(sod), dirty);
    for (int i = 0; i < 10; i++) {
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(250, frame.target, rng, reading_at(sod), dirty);
        CHECK(dirty.empty());
        CHECK(frame.bytes == before);
    }
    const DirtyRect clock = sim.clock_layout().bounds;
    const DirtyRect padded{clock.x1 - 4, clock.y1 - 4, clock.x2 + 4, clock.y2 + 4};
    const std::vector<uint8_t> before = frame.bytes;
    sim.step(250, frame.target, rng, reading_at(sod + 60), dirty);
    REQUIRE_FALSE(dirty.empty());
    for (const DirtyRect& d : dirty) {
        CHECK(helix::test::contains(padded, d));
    }
    const auto d = diff_against(before, frame, dirty);
    CHECK(d.changed > 0);
    CHECK(d.uncovered == 0);
}

TEST_CASE(
    "high fly frames and step-downs never grow the heap, and the still rung keeps only the clock",
    "[screensaver][highfly]") {
    const size_t start = GENERATE(range(size_t{0}, kHighflyLevelCount));
    CAPTURE(start);
    TestFrame frame(800, 480, PixelFormat::XRGB8888);
    std::minstd_rand rng(3);
    HighFlySim sim;
    const ClockReading r = reading_at(10 * 3600);
    sim.init(frame.target, rng, start, r);
    std::vector<DirtyRect> dirty;
    const size_t heap = sim.heap_bytes();
    CHECK(heap <= 4608u * 1024u);
    for (size_t level = start; level < kHighflyLevelCount; level++) {
        sim.request_level(level);
        for (int i = 0; i < 20; i++) {
            sim.step(level + 1 == kHighflyLevelCount ? 250 : 33, frame.target, rng, r, dirty);
            CHECK(sim.heap_bytes() <= heap);
        }
    }
    // The still rung has painted its scene and holds just what redraws the clock.
    const size_t still = sim.heap_bytes();
    if (start + 1 < kHighflyLevelCount) {
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
// HighFlyScreensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "the high fly saver shows the print its source reports",
                 "[screensaver][highfly]") {
    HighFlyScreensaver ss;
    ScreensaverStopOnExit<HighFlyScreensaver> stop_on_exit{ss};
    ClockInput reported;
    reported.seconds_of_day = 14 * 3600;
    reported.print.phase = ClockPrintPhase::PRINTING;
    reported.print.remaining_s = 4320;
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(ss.clock().mode() == ClockMode::COUNTDOWN);
    const std::string date = HighFlyScreensaverTestAccess::date_line(ss).text();
    const std::string day = HighFlyScreensaverTestAccess::day_line(ss).text();
    CHECK(date.rfind("ETA ", 0) == 0);
    REQUIRE(day.size() > 5);
    CHECK(day.compare(day.size() - 5, 5, " left") == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a changed high fly text line waits for a frame that repaints under it",
                 "[screensaver][highfly]") {
    HighFlyScreensaver ss;
    ScreensaverStopOnExit<HighFlyScreensaver> stop_on_exit{ss};
    ClockInput reported;
    reported.seconds_of_day = 14 * 3600;
    ss.set_input_source([&reported] { return reported; });
    // The still rung repaints only when the clock face changes, so a frame with the same
    // time repaints nothing.
    ss.set_start_level(kHighflyLevelCount - 1);
    ss.start();
    REQUIRE(ss.is_active());
    advance_saver(ss, 250);
    const std::string date = HighFlyScreensaverTestAccess::date_line(ss).text();
    REQUIRE_FALSE(date.empty());

    reported.print.phase = ClockPrintPhase::PRINTING;
    reported.print.remaining_s = 4320;
    advance_saver(ss, 250);
    REQUIRE(ss.clock().mode() == ClockMode::COUNTDOWN);
    CHECK(HighFlyScreensaverTestAccess::date_line(ss).text() == date);

    reported.seconds_of_day += 1;
    advance_saver(ss, 250);
    CHECK(HighFlyScreensaverTestAccess::date_line(ss).text().rfind("ETA ", 0) == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
