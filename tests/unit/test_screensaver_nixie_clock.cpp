// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_clock_reading.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_nixie_clock.h"
#include "screensaver_nixie_clock_sim.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_print_colors.h"

#include <algorithm>
#include <set>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::test::bytes_differing;
using helix::test::hms;
using helix::test::TestFrame;
using helix::ui::clock_face;
using helix::ui::ClockInput;
using helix::ui::ClockMode;
using helix::ui::ClockPrintPhase;
using helix::ui::DirtyRect;
using helix::ui::FlipFace;
using helix::ui::FlipLayout;
using helix::ui::kClockDriftStepMs;
using helix::ui::kNixieClockLevelCount;
using helix::ui::kNixieClockLevels;
using helix::ui::kNixieCycleEveryMs;
using helix::ui::kNixieCycleMs;
using helix::ui::kNixieCycleStepMs;
using helix::ui::kNixieFadeMs;
using helix::ui::kNixieTubeGeometry;
using helix::ui::NixieClockScreensaver;
using helix::ui::NixieClockSim;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

namespace {

/// Pixels inside `box` lit by neon: red well above blue, as only the glow is.
size_t glowing_in(const TestFrame& frame, const DirtyRect& box) {
    const PixelWriter writer(frame.target);
    size_t lit = 0;
    for (int32_t y = box.y1; y <= box.y2; y++) {
        for (int32_t x = box.x1; x <= box.x2; x++) {
            const Rgb c = writer.get(x, y);
            lit += c.r > 120 && c.r > c.b + 80 ? 1 : 0;
        }
    }
    return lit;
}

/// Sum of the red channel over `box`: how brightly it glows.
uint64_t red_in(const TestFrame& frame, const DirtyRect& box) {
    const PixelWriter writer(frame.target);
    uint64_t sum = 0;
    for (int32_t y = box.y1; y <= box.y2; y++) {
        for (int32_t x = box.x1; x <= box.x2; x++) {
            sum += writer.get(x, y).r;
        }
    }
    return sum;
}

std::vector<uint8_t> box_bytes(const TestFrame& frame, const DirtyRect& box) {
    std::vector<uint8_t> out;
    const uint32_t bpp = helix::ui::pixel_bytes(frame.target.format);
    for (int32_t y = box.y1; y <= box.y2; y++) {
        const size_t row = static_cast<size_t>(y) * frame.target.stride;
        out.insert(out.end(), frame.bytes.begin() + static_cast<ptrdiff_t>(row + box.x1 * bpp),
                   frame.bytes.begin() + static_cast<ptrdiff_t>(row + (box.x2 + 1) * bpp));
    }
    return out;
}

bool inside(const DirtyRect& inner, const DirtyRect& outer) {
    return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 &&
           inner.y2 <= outer.y2;
}

/// Steps `sim` by `total_ms` in frames of `frame_ms` showing `face`.
void run_for(NixieClockSim& sim, TestFrame& frame, const FlipFace& face, uint32_t total_ms,
             uint32_t frame_ms = 33) {
    std::vector<DirtyRect> dirty;
    for (uint32_t t = 0; t < total_ms; t += frame_ms) {
        sim.step(frame_ms, frame.target, face, dirty);
    }
}

} // namespace

// ============================================================================
// The tubes
// ============================================================================

TEST_CASE("the nixie clock draws a lit numeral in each tube over its dark wire stack, in "
          "either format",
          "[screensaver][screensaver_nixie_clock]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    TestFrame frame(480, 272, format);
    NixieClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const FlipLayout& layout = sim.layout();
    const PixelWriter writer(frame.target);

    REQUIRE(layout.card_count == 4);
    CHECK(writer.get(0, 0) == NixieClockSim::kBackground);
    for (size_t i = 0; i < layout.card_count; i++) {
        CAPTURE(i);
        const DirtyRect& tube = layout.cards[i];
        CHECK(glowing_in(frame, sim.glow_box(i)) > 40);
        // The tube's corners are outside its rounded glass and socket.
        CHECK(writer.get(tube.x1, tube.y1) == NixieClockSim::kBackground);
        // The socket under the glass is lit grey metal.
        const DirtyRect socket = sim.meridiem_box();
        const Rgb metal = writer.get((tube.x1 + tube.x2) / 2, socket.y1 + 1);
        CHECK(metal.g > 15);
        CHECK(metal.r < 120);
    }
    // The colon between hours and minutes is two neon dots.
    REQUIRE(layout.colon_count == 1);
    CHECK(glowing_in(frame, layout.colons[0]) > 2);
}

TEST_CASE("an unlit tube shows only its dark wires, and every digit glows its own shape",
          "[screensaver][screensaver_nixie_clock]") {
    TestFrame frame(480, 272, PixelFormat::XRGB8888);
    NixieClockSim sim;
    // 12-hour time before ten leaves the first tube blank.
    sim.init(frame.target, 0, clock_face(hms(9, 41), false));
    CHECK(glowing_in(frame, sim.glow_box(0)) == 0);
    CHECK(red_in(frame, sim.glow_box(0)) > 0);
    CHECK(glowing_in(frame, sim.glow_box(1)) > 40);

    std::vector<std::vector<uint8_t>> seen;
    for (int d = 0; d < 10; d++) {
        sim.init(frame.target, 2, clock_face(hms(d, d), true));
        const std::vector<uint8_t> tube = box_bytes(frame, sim.layout().cards[3]);
        for (const auto& other : seen) {
            CHECK(bytes_differing(tube, other) > 0);
        }
        seen.push_back(tube);
    }
}

TEST_CASE("a changed numeral fades out as the new one warms up, and only its tube is redrawn",
          "[screensaver][screensaver_nixie_clock]") {
    TestFrame frame(480, 272, PixelFormat::XRGB8888);
    NixieClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const DirtyRect glow = sim.glow_box(3);
    const std::vector<uint8_t> start = box_bytes(frame, glow);

    std::vector<DirtyRect> dirty;
    const FlipFace next = clock_face(hms(10, 9), true);
    sim.step(0, frame.target, next, dirty);
    CHECK(sim.fading());
    CHECK(sim.fade_progress(0) == 1.0f);
    CHECK(sim.fade_progress(3) == 0.0f);
    CHECK(sim.lit(3) == '9');
    // Nothing has warmed yet: the tube still shows the old numeral alone.
    CHECK(bytes_differing(box_bytes(frame, glow), start) == 0);

    sim.step(kNixieFadeMs / 2, frame.target, next, dirty);
    CHECK(sim.fade_progress(3) == Catch::Approx(0.5f));
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0] == glow);
    const std::vector<uint8_t> half = box_bytes(frame, glow);
    CHECK(bytes_differing(half, start) > 0);

    sim.step(kNixieFadeMs / 2, frame.target, next, dirty);
    CHECK_FALSE(sim.fading());
    const std::vector<uint8_t> end = box_bytes(frame, glow);
    CHECK(bytes_differing(end, half) > 0);

    // Halfway, both numerals glow at half strength, so the box is dimmer than either end
    // wherever only one of them has wire.
    TestFrame fresh_frame(480, 272, PixelFormat::XRGB8888);
    NixieClockSim fresh;
    fresh.init(fresh_frame.target, 0, next);
    CHECK(bytes_differing(end, box_bytes(fresh_frame, glow)) == 0);
    TestFrame mid(480, 272, PixelFormat::XRGB8888);
    NixieClockSim mid_sim;
    mid_sim.init(mid.target, 0, clock_face(hms(10, 8), true));
    mid_sim.step(0, mid.target, next, dirty);
    mid_sim.step(kNixieFadeMs / 2, mid.target, next, dirty);
    TestFrame old_frame(480, 272, PixelFormat::XRGB8888);
    NixieClockSim old_sim;
    old_sim.init(old_frame.target, 0, clock_face(hms(10, 8), true));
    const uint64_t mid_red = red_in(mid, glow);
    CHECK(mid_red < red_in(old_frame, glow) + red_in(fresh_frame, glow));
    CHECK(mid_red > std::min(red_in(old_frame, glow), red_in(fresh_frame, glow)) / 2);
}

TEST_CASE("between changes a nixie frame writes nothing",
          "[screensaver][screensaver_nixie_clock]") {
    TestFrame frame(480, 272, PixelFormat::RGB565);
    NixieClockSim sim;
    const FlipFace face = clock_face(hms(7, 30), false);
    sim.init(frame.target, 0, face);
    std::vector<DirtyRect> dirty;
    const std::vector<uint8_t> before = frame.bytes;
    for (int i = 0; i < 100; i++) {
        sim.step(16, frame.target, face, dirty);
        CHECK(dirty.empty());
    }
    CHECK(bytes_differing(frame.bytes, before) == 0);
}

TEST_CASE("a level that does not fade swaps the numeral at once",
          "[screensaver][screensaver_nixie_clock]") {
    REQUIRE_FALSE(kNixieClockLevels[kNixieClockLevelCount - 1].fade);
    TestFrame frame(480, 272, PixelFormat::XRGB8888);
    NixieClockSim sim;
    sim.init(frame.target, kNixieClockLevelCount - 1, clock_face(hms(10, 8), true));
    std::vector<DirtyRect> dirty;
    sim.step(0, frame.target, clock_face(hms(10, 9), true), dirty);
    CHECK_FALSE(sim.fading());
    CHECK(dirty.size() == 1);

    SECTION("and a level request mid-fade finishes the fade") {
        sim.request_level(0);
        sim.step(0, frame.target, clock_face(hms(10, 10), true), dirty);
        REQUIRE(sim.fading());
        sim.request_level(kNixieClockLevelCount - 1);
        sim.step(0, frame.target, clock_face(hms(10, 10), true), dirty);
        CHECK_FALSE(sim.fading());
        CHECK(sim.level() == kNixieClockLevelCount - 1);
    }
}

// ============================================================================
// Cathode cycling
// ============================================================================

TEST_CASE("every ten minutes of the time the tubes run through all ten numerals and fade back",
          "[screensaver][screensaver_nixie_clock]") {
    TestFrame frame(480, 272, PixelFormat::XRGB8888);
    NixieClockSim sim;
    const FlipFace face = clock_face(hms(10, 8), true);
    sim.init(frame.target, 0, face);

    // The cycle starts once the period has passed; frames the drift relayouts do not count, so
    // it may start a few frames late.
    std::vector<DirtyRect> dirty;
    uint32_t elapsed = 0;
    while (!sim.cycling() && elapsed < kNixieCycleEveryMs + 5000) {
        sim.step(100, frame.target, face, dirty);
        elapsed += 100;
    }
    REQUIRE(sim.cycling());
    CHECK(elapsed >= kNixieCycleEveryMs);
    CHECK(elapsed < kNixieCycleEveryMs + 2000);

    std::set<char> shown;
    while (sim.cycling()) {
        shown.insert(sim.lit(3));
        // A drift step during the cycle would end it; none falls this close.
        sim.step(kNixieCycleStepMs / 2, frame.target, face, dirty);
    }
    CHECK(shown.size() == 10);
    // Back on the face's numerals, fading in.
    CHECK(sim.lit(0) == '1');
    CHECK(sim.lit(3) == '8');
    run_for(sim, frame, face, kNixieFadeMs, 16);
    CHECK_FALSE(sim.fading());
}

TEST_CASE("the cathodes never cycle on a level that does not fade or while a print shows",
          "[screensaver][screensaver_nixie_clock]") {
    TestFrame frame(480, 272, PixelFormat::XRGB8888);
    NixieClockSim sim;
    SECTION("the instant level") {
        const FlipFace face = clock_face(hms(10, 8), true);
        sim.init(frame.target, kNixieClockLevelCount - 1, face);
        run_for(sim, frame, face, kNixieCycleEveryMs + 5000, 250);
        CHECK_FALSE(sim.cycling());
    }
    SECTION("a print counting down") {
        const FlipFace face =
            helix::ui::flip_face(helix::test::print_reading_for(ClockMode::COUNTDOWN, 40));
        sim.init(frame.target, 0, face);
        run_for(sim, frame, face, kNixieCycleEveryMs + 5000, 100);
        CHECK_FALSE(sim.cycling());
    }
}

// ============================================================================
// Layout, print and memory
// ============================================================================

TEST_CASE("the tubes and their sealed tips fit every screen at the widest drift, small row "
          "included, with the AM/PM socket inside the first tube",
          "[screensaver][screensaver_nixie_clock]") {
    struct Screen {
        int32_t w;
        int32_t h;
    };
    const Screen screen = GENERATE(Screen{800, 480}, Screen{480, 272}, Screen{480, 320},
                                   Screen{1024, 600}, Screen{480, 800});
    CAPTURE(screen.w, screen.h);
    const ClockMode mode =
        GENERATE(ClockMode::CLOCK, ClockMode::COUNTDOWN, ClockMode::PAUSED, ClockMode::DONE);
    CAPTURE(static_cast<int>(mode));
    const FlipFace face = mode == ClockMode::CLOCK
                              ? clock_face(hms(22, 48), false)
                              : helix::ui::flip_face(helix::test::print_reading_for(mode, 40));
    TestFrame frame(static_cast<uint32_t>(screen.w), static_cast<uint32_t>(screen.h),
                    PixelFormat::XRGB8888);
    NixieClockSim sim;
    sim.init(frame.target, 0, face);
    const DirtyRect whole{0, 0, screen.w - 1, screen.h - 1};
    helix::ui::ClockDrift drift;
    drift.set_range(helix::ui::flip_centred_bounds(screen.w, screen.h, face.shape, face.sub_shape,
                                                   face.sub_colons, kNixieTubeGeometry),
                    screen.w, screen.h);
    for (const int32_t sx : {-1, 1}) {
        for (const int32_t sy : {-1, 1}) {
            const FlipLayout layout = helix::ui::layout_flip_cards(
                screen.w, screen.h, face.shape, face.sub_shape, sx * drift.max_dx,
                sy * drift.max_dy, face.sub_colons, kNixieTubeGeometry);
            CHECK(inside(layout.bounds(), whole));
            // Each tube's tip moves with its tube.
            for (size_t i = 0; i < layout.card_count + size_t{layout.sub_count}; i++) {
                CAPTURE(i);
                const DirtyRect tip = sim.tip_box(i);
                const int32_t dx = layout.cards[i].x1 - sim.layout().cards[i].x1;
                const int32_t dy = layout.cards[i].y1 - sim.layout().cards[i].y1;
                CHECK(inside({tip.x1 + dx, tip.y1 + dy, tip.x2 + dx, tip.y2 + dy}, whole));
            }
        }
    }
    // Every tube has its tip drawn, inside its own box and above its numeral.
    const PixelWriter writer(frame.target);
    for (size_t i = 0; i < sim.layout().card_count + size_t{sim.layout().sub_count}; i++) {
        CAPTURE(i);
        const DirtyRect tip = sim.tip_box(i);
        CHECK(inside(tip, sim.layout().cards[i]));
        CHECK(tip.y2 < sim.glow_box(i).y1 + 1);
        size_t glass = 0;
        size_t stray = 0;
        // The rows above the dome hold only the tip.
        for (int32_t y = tip.y1; y < tip.y1 + 3; y++) {
            for (int32_t x = sim.layout().cards[i].x1; x <= sim.layout().cards[i].x2; x++) {
                const bool lit = !(writer.get(x, y) == NixieClockSim::kBackground);
                glass += lit ? 1 : 0;
                stray += lit && (x < tip.x1 || x > tip.x2) ? 1 : 0;
            }
        }
        CHECK(glass > 0);
        CHECK(stray == 0);
    }
    const DirtyRect marker = sim.meridiem_box();
    CHECK(inside(marker, sim.layout().cards[0]));
    CHECK(marker.y1 > sim.glow_box(0).y2);
    CHECK(marker.y2 - marker.y1 >= 8);
}

TEST_CASE("a print's state colours the neon: green done, red failed",
          "[screensaver][screensaver_nixie_clock]") {
    TestFrame frame(480, 272, PixelFormat::XRGB8888);
    NixieClockSim sim;
    const PixelWriter writer(frame.target);
    struct Sums {
        uint64_t r = 0;
        uint64_t g = 0;
        uint64_t b = 0;
    };
    // Over the glowing pixels only: the grey glass around them carries no ink.
    const auto sums = [&](const DirtyRect& box) {
        Sums out;
        for (int32_t y = box.y1; y <= box.y2; y++) {
            for (int32_t x = box.x1; x <= box.x2; x++) {
                const Rgb c = writer.get(x, y);
                if (std::max({c.r, c.g, c.b}) < 150) {
                    continue;
                }
                out.r += c.r;
                out.g += c.g;
                out.b += c.b;
            }
        }
        return out;
    };
    sim.init(frame.target, 0,
             helix::ui::flip_face(helix::test::print_reading_for(ClockMode::DONE, 100)));
    const Sums done = sums(sim.glow_box(0));
    CHECK(done.g > done.r);
    sim.init(frame.target, 0,
             helix::ui::flip_face(helix::test::print_reading_for(ClockMode::FAILED, 40)));
    const Sums failed = sums(sim.glow_box(0));
    CHECK(failed.r > failed.g * 5 / 4);
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const Sums neon = sums(sim.glow_box(0));
    CHECK(neon.r > neon.g * 5 / 4);
    CHECK(neon.g > neon.b);
}

TEST_CASE("the cached tube art is sized for the screen and freed by clear",
          "[screensaver][screensaver_nixie_clock]") {
    struct Screen {
        uint32_t w;
        uint32_t h;
        size_t clock_cap;
    };
    // Four tubes for the time at 800x480 (the K-Touch's panel too) and at the smallest
    // breakpoint: the tube bases plus ten digits' glow.
    const Screen screen = GENERATE(Screen{800, 480, 500 * 1024}, Screen{480, 272, 200 * 1024});
    CAPTURE(screen.w, screen.h);
    TestFrame frame(screen.w, screen.h, PixelFormat::RGB565);
    NixieClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const size_t clock_bytes = sim.heap_bytes();
    CAPTURE(clock_bytes);
    CHECK(clock_bytes > 0);
    CHECK(clock_bytes < screen.clock_cap);

    // A countdown adds the small tubes' art and the large tubes shrink.
    sim.init(frame.target, 0,
             helix::ui::flip_face(helix::test::print_reading_for(ClockMode::COUNTDOWN, 40)));
    CAPTURE(sim.heap_bytes());
    CHECK(sim.heap_bytes() < clock_bytes);
    sim.clear();
    CHECK(sim.heap_bytes() == 0);
}

// ============================================================================
// The saver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "the nixie clock saver counts down the print its source reports",
                 "[screensaver][screensaver_nixie_clock]") {
    NixieClockScreensaver ss;
    ScreensaverStopOnExit<NixieClockScreensaver> stop_on_exit{ss};
    ClockInput reported;
    reported.seconds_of_day = hms(14, 0);
    reported.print.phase = ClockPrintPhase::PRINTING;
    reported.print.remaining_s = hms(1, 0, 0);
    reported.print.progress_pct = 50;
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(ss.clock().mode() == ClockMode::COUNTDOWN);
    CHECK(ss.sim().layout().card_count == 5);
    CHECK(ss.sim().layout().sub_count > 0);

    reported.print.phase = ClockPrintPhase::PAUSED;
    helix::ui::advance_saver(ss, 33);
    CHECK(ss.clock().mode() == ClockMode::PAUSED);
}

#endif // HELIX_ENABLE_SCREENSAVER
