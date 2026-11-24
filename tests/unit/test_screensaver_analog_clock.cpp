// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "display_settings_manager.h"
#include "screensaver_analog_clock.h"
#include "screensaver_analog_clock_sim.h"
#include "screensaver_clock_reading.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <set>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::test::diff_against;
using helix::test::FrameDiff;
using helix::test::hms;
using helix::test::print_reading_for;
using helix::test::reading_at;
using helix::test::TestFrame;
using helix::ui::analog_hands;
using helix::ui::analog_motion;
using helix::ui::analog_spring;
using helix::ui::analog_style_for_run;
using helix::ui::AnalogClockScreensaver;
using helix::ui::AnalogClockSim;
using helix::ui::AnalogFace;
using helix::ui::AnalogHands;
using helix::ui::AnalogMotion;
using helix::ui::AnalogStyle;
using helix::ui::AnalogText;
using helix::ui::AnalogTextRole;
using helix::ui::ClockInput;
using helix::ui::ClockMode;
using helix::ui::ClockPrintPhase;
using helix::ui::ClockReading;
using helix::ui::DirtyRect;
using helix::ui::kAnalogFaceShuffle;
using helix::ui::kAnalogSpringMs;
using helix::ui::kAnalogStyleCount;
using helix::ui::PixelFormat;

namespace {

constexpr AnalogStyle kStyles[] = {AnalogStyle::CHRONOGRAPH, AnalogStyle::STATION,
                                   AnalogStyle::NEON, AnalogStyle::BAUHAUS, AnalogStyle::ORRERY};

/// Level 2 ticks once a second with no trails.
constexpr size_t kTickLevel = 2;

/// A sim on a frame, stepped with a reading the test sets.
struct AnalogRun {
    AnalogRun(AnalogStyle style, uint32_t w, uint32_t h, ClockReading r,
              PixelFormat format = PixelFormat::XRGB8888)
        : frame(w, h, format), face{style, r} {
        helix::ui::PixelWriter(frame.target).fill({});
    }
    void init(size_t level) {
        sim.init(frame.target, level, face);
    }
    void step(uint32_t dt_ms) {
        sim.step(dt_ms, frame.target, face, dirty);
    }

    TestFrame frame;
    AnalogFace face;
    AnalogClockSim sim;
    std::vector<DirtyRect> dirty;
};

bool places_text(const std::vector<AnalogText>& texts, AnalogTextRole role) {
    return std::any_of(texts.begin(), texts.end(),
                       [role](const AnalogText& t) { return t.role == role; });
}

} // namespace

TEST_CASE("analog hands point where the time says, each moving its face's way",
          "[screensaver][screensaver_analog_clock]") {
    const helix::ui::ClockTime t = helix::ui::clock_time(hms(10, 9, 36), false);

    const AnalogHands sweep = analog_hands(t, 500, AnalogMotion::SWEEP);
    CHECK(sweep.second == Catch::Approx(219.0f));
    CHECK(sweep.minute == Catch::Approx(57.6f));
    CHECK(sweep.hour == Catch::Approx(304.8f));

    // A tick holds the whole second.
    CHECK(analog_hands(t, 0, AnalogMotion::TICK).second == Catch::Approx(216.0f));
    CHECK(analog_hands(t, 999, AnalogMotion::TICK).second == Catch::Approx(216.0f));

    // A sprung step starts from the last second and settles on this one.
    CHECK(analog_hands(t, 0, AnalogMotion::SPRUNG).second == Catch::Approx(210.0f));
    CHECK(analog_hands(t, kAnalogSpringMs, AnalogMotion::SPRUNG).second == Catch::Approx(216.0f));
    // Its minute hand holds through the minute and springs over at :00.
    CHECK(analog_hands(t, 0, AnalogMotion::SPRUNG).minute == Catch::Approx(54.0f));
    const helix::ui::ClockTime top = helix::ui::clock_time(hms(10, 10, 0), false);
    CHECK(analog_hands(top, 0, AnalogMotion::SPRUNG).minute == Catch::Approx(54.0f));
    CHECK(analog_hands(top, kAnalogSpringMs, AnalogMotion::SPRUNG).minute == Catch::Approx(60.0f));
}

TEST_CASE("a sprung step overshoots before it settles", "[screensaver][screensaver_analog_clock]") {
    CHECK(analog_spring(0) == 0.0f);
    float most = 0.0f;
    for (uint32_t ms = 0; ms < kAnalogSpringMs; ms += 5) {
        most = std::max(most, analog_spring(ms));
    }
    CHECK(most > 1.02f);
    CHECK(analog_spring(kAnalogSpringMs) == 1.0f);
}

TEST_CASE("each face moves its own way, and the lowest rung ticks them all",
          "[screensaver][screensaver_analog_clock]") {
    CHECK(analog_motion(AnalogStyle::CHRONOGRAPH, true) == AnalogMotion::SWEEP);
    CHECK(analog_motion(AnalogStyle::STATION, true) == AnalogMotion::SPRUNG);
    CHECK(analog_motion(AnalogStyle::BAUHAUS, true) == AnalogMotion::TICK);
    for (const AnalogStyle s : kStyles) {
        CHECK(analog_motion(s, false) == AnalogMotion::TICK);
    }
}

TEST_CASE(
    "the clock face setting picks a face, and Shuffle never shows the same face twice running",
    "[screensaver][screensaver_analog_clock]") {
    CHECK(analog_style_for_run(1, false, -1, 7) == AnalogStyle::STATION);
    CHECK(analog_style_for_run(1, false, 1, 7) == AnalogStyle::STATION);

    std::set<int> seen;
    for (uint32_t draw = 0; draw < 40; draw++) {
        const auto next =
            static_cast<int>(analog_style_for_run(kAnalogFaceShuffle, false, 3, draw));
        CHECK(next != 3);
        seen.insert(next);
    }
    CHECK(seen.size() == kAnalogStyleCount - 1);
    // The Random saver shuffles whatever face is set.
    CHECK(analog_style_for_run(0, true, 0, 0) != AnalogStyle::CHRONOGRAPH);
}

TEST_CASE("every analog face's dirty boxes cover what it changes, with and without a print",
          "[screensaver][screensaver_analog_clock]") {
    const AnalogStyle style = GENERATE(from_range(std::begin(kStyles), std::end(kStyles)));
    const ClockMode mode = GENERATE(ClockMode::CLOCK, ClockMode::COUNTDOWN, ClockMode::PAUSED);
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(style), static_cast<int>(mode), static_cast<int>(format));
    AnalogRun r(style, 400, 240, print_reading_for(mode, 40, hms(9, 41, 58)), format);
    r.init(0);
    size_t changed = 0;
    size_t uncovered = 0;
    for (int i = 0; i < 90; i++) {
        const std::vector<uint8_t> before = r.frame.snapshot();
        r.face.reading = print_reading_for(mode, 40, hms(9, 41, 58) + i * 33 / 1000);
        r.step(33);
        const FrameDiff d = diff_against(before, r.frame, r.dirty);
        changed += d.changed;
        uncovered += d.uncovered;
    }
    CHECK(changed > 0);
    CHECK(uncovered == 0);
}

TEST_CASE("between ticks an analog frame writes nothing, and the dial redraws only when what it "
          "shows changes",
          "[screensaver][screensaver_analog_clock]") {
    const AnalogStyle style = GENERATE(from_range(std::begin(kStyles), std::end(kStyles)));
    CAPTURE(static_cast<int>(style));
    AnalogRun r(style, 400, 240, print_reading_for(ClockMode::COUNTDOWN, 40, hms(9, 41, 10)));
    r.init(kTickLevel);

    // The same second: no hand moves, no pixel is written.
    REQUIRE_FALSE(r.sim.prepare(100, r.frame.target, r.face));
    r.sim.compose(r.frame.target, r.dirty);
    CHECK(r.dirty.empty());

    // The next second moves the seconds hand; the dial stays.
    r.face.reading = print_reading_for(ClockMode::COUNTDOWN, 40, hms(9, 41, 11));
    CHECK_FALSE(r.sim.prepare(900, r.frame.target, r.face));
    r.sim.compose(r.frame.target, r.dirty);
    CHECK_FALSE(r.dirty.empty());

    // New progress redraws the dial, and the whole frame with it.
    r.face.reading = print_reading_for(ClockMode::COUNTDOWN, 41, hms(9, 41, 11));
    CHECK(r.sim.prepare(10, r.frame.target, r.face));
    r.sim.compose(r.frame.target, r.dirty);
    REQUIRE(r.dirty.size() == 1);
    CHECK(r.dirty[0] == DirtyRect::whole(400, 240));
}

TEST_CASE("a settled station clock draws nothing until its next tick",
          "[screensaver][screensaver_analog_clock]") {
    AnalogRun r(AnalogStyle::STATION, 400, 240, reading_at(hms(9, 41, 10)));
    r.init(0);
    r.step(100);
    CHECK_FALSE(r.dirty.empty()); // still springing
    r.step(kAnalogSpringMs);
    r.step(50);
    CHECK(r.dirty.empty());
}

TEST_CASE("a paused chronograph stops its seconds hand, and it runs again on resume",
          "[screensaver][screensaver_analog_clock]") {
    AnalogRun r(AnalogStyle::CHRONOGRAPH, 400, 240,
                print_reading_for(ClockMode::PAUSED, 40, hms(9, 41, 10)));
    r.init(0);
    const float stopped = r.sim.hands().second;
    r.face.reading = print_reading_for(ClockMode::PAUSED, 40, hms(9, 41, 14));
    r.step(400);
    CHECK(r.sim.hands().second == stopped);

    r.face.reading = print_reading_for(ClockMode::COUNTDOWN, 40, hms(9, 41, 15));
    r.step(33);
    CHECK(r.sim.hands().second != stopped);
}

TEST_CASE("the chronograph shows the print on its subdials, with captions only where they fit",
          "[screensaver][screensaver_analog_clock]") {
    AnalogRun big(AnalogStyle::CHRONOGRAPH, 800, 480,
                  print_reading_for(ClockMode::COUNTDOWN, 62, hms(10, 9, 36)));
    big.init(0);
    const std::vector<AnalogText>& texts = big.sim.texts();
    CHECK(places_text(texts, AnalogTextRole::VALUE_PRINT));
    CHECK(places_text(texts, AnalogTextRole::VALUE_LEFT));
    CHECK(places_text(texts, AnalogTextRole::VALUE_ENDS));
    CHECK(places_text(texts, AnalogTextRole::CAPTION_PRINT));
    CHECK(places_text(texts, AnalogTextRole::MAKER));
    CHECK_FALSE(places_text(texts, AnalogTextRole::WEEKDAY_LETTER));

    AnalogRun small(AnalogStyle::CHRONOGRAPH, 480, 272,
                    print_reading_for(ClockMode::COUNTDOWN, 62, hms(10, 9, 36)));
    small.init(0);
    CHECK(places_text(small.sim.texts(), AnalogTextRole::VALUE_PRINT));
    CHECK_FALSE(places_text(small.sim.texts(), AnalogTextRole::CAPTION_PRINT));

    // With no print, the subdials show the day: a 24-hour hand and the weekdays.
    AnalogRun idle(AnalogStyle::CHRONOGRAPH, 800, 480, reading_at(hms(10, 9, 36)));
    idle.init(0);
    CHECK(places_text(idle.sim.texts(), AnalogTextRole::CAPTION_24H));
    CHECK(std::count_if(idle.sim.texts().begin(), idle.sim.texts().end(), [](const AnalogText& t) {
              return t.role == AnalogTextRole::WEEKDAY_LETTER;
          }) == 7);
    CHECK_FALSE(places_text(idle.sim.texts(), AnalogTextRole::VALUE_PRINT));
}

TEST_CASE("every face's text sits on the screen at every size and drift",
          "[screensaver][screensaver_analog_clock]") {
    const AnalogStyle style = GENERATE(from_range(std::begin(kStyles), std::end(kStyles)));
    const auto size =
        GENERATE(std::pair<uint32_t, uint32_t>{800, 480}, std::pair<uint32_t, uint32_t>{480, 272},
                 std::pair<uint32_t, uint32_t>{480, 800});
    const ClockMode mode = GENERATE(ClockMode::CLOCK, ClockMode::COUNTDOWN, ClockMode::DONE);
    CAPTURE(static_cast<int>(style), size.first, size.second, static_cast<int>(mode));
    AnalogRun r(style, size.first, size.second, print_reading_for(mode, 62, hms(10, 9, 36)));
    r.init(0);
    const DirtyRect screen =
        DirtyRect::whole(static_cast<int32_t>(size.first), static_cast<int32_t>(size.second));
    // Ten drift steps visit the corners of the drift range.
    for (int step = 0; step < 10; step++) {
        for (const AnalogText& t : r.sim.texts()) {
            CHECK(helix::test::contains(screen, t.box));
        }
        r.step(helix::ui::kClockDriftStepMs);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "the analog clock saver shows the face the setting names",
                 "[screensaver][screensaver_analog_clock]") {
    helix::DisplaySettingsManager::instance().set_screensaver_clock_face(
        static_cast<int>(AnalogStyle::NEON));
    AnalogClockScreensaver ss;
    ScreensaverStopOnExit<AnalogClockScreensaver> stop_on_exit{ss};
    ClockInput reported;
    reported.seconds_of_day = hms(14, 0);
    reported.print.phase = ClockPrintPhase::PRINTING;
    reported.print.remaining_s = hms(1, 0, 0);
    reported.print.progress_pct = 50;
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(ss.style() == AnalogStyle::NEON);
    CHECK(ss.clock().mode() == ClockMode::COUNTDOWN);
    REQUIRE(places_text(ss.sim().texts(), AnalogTextRole::COUNTDOWN));

    // The countdown's words are written into the dial, so the canvas carries them.
    const AnalogText& line =
        *std::find_if(ss.sim().texts().begin(), ss.sim().texts().end(),
                      [](const AnalogText& t) { return t.role == AnalogTextRole::COUNTDOWN; });
    const helix::ui::FrameTarget frame =
        helix::test::frame_of(lv_canvas_get_draw_buf(helix::ui::SaverTestAccess::canvas(ss)));
    const helix::ui::PixelWriter writer(frame);
    size_t lit = 0;
    for (int32_t y = line.box.y1; y <= line.box.y2; y++) {
        for (int32_t x = line.box.x1; x <= line.box.x2; x++) {
            lit += writer.get(x, y).r > 120 ? 1 : 0;
        }
    }
    CHECK(lit > 0);
    helix::DisplaySettingsManager::instance().set_screensaver_clock_face(0);
}

#endif // HELIX_ENABLE_SCREENSAVER
