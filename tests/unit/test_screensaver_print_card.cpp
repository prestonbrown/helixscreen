// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_clock_reading.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_print_card.h"
#include "screensaver_print_colors.h"
#include "screensaver_registry.h"
#include "screensaver_starfield.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::test::bytes_differing;
using helix::test::TestFrame;
using helix::ui::ClockMode;
using helix::ui::DirtyRect;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::print_card_text;
using helix::ui::PrintCardFace;
using helix::ui::PrintCardLayout;
using helix::ui::PrintCardPainter;
using helix::ui::Rgb;

namespace {

/// The card painted for `face` at 800x480, as raw bytes.
std::vector<uint8_t> card_bytes(PixelFormat format, const PrintCardFace& face) {
    const PrintCardLayout layout = helix::ui::layout_print_card(800, 480);
    PrintCardPainter painter;
    painter.init(layout);
    TestFrame frame(static_cast<uint32_t>(layout.w), static_cast<uint32_t>(layout.h), format);
    painter.draw(frame.target, face);
    return frame.bytes;
}

/// Pixels of the card for `face` exactly `colour` as `format` stores it.
size_t pixels_of(PixelFormat format, const PrintCardFace& face, Rgb colour) {
    const PrintCardLayout layout = helix::ui::layout_print_card(800, 480);
    PrintCardPainter painter;
    painter.init(layout);
    TestFrame frame(static_cast<uint32_t>(layout.w), static_cast<uint32_t>(layout.h), format);
    painter.draw(frame.target, face);
    const PixelWriter writer(frame.target);
    TestFrame probe(1, 1, format);
    PixelWriter(probe.target).put(0, 0, colour);
    const Rgb stored = PixelWriter(probe.target).get(0, 0);
    size_t n = 0;
    for (int32_t y = 0; y < layout.h; y++) {
        for (int32_t x = 0; x < layout.w; x++) {
            n += writer.get(x, y) == stored ? 1 : 0;
        }
    }
    return n;
}

PrintCardFace face_of(ClockMode mode, int32_t pct, int32_t countdown_s = 0) {
    PrintCardFace f;
    f.mode = mode;
    f.progress_pct = pct;
    f.countdown_s = countdown_s;
    f.has_countdown = countdown_s > 0;
    return f;
}

} // namespace

TEST_CASE("each print state has its own colour, distinct at 16-bit",
          "[screensaver][screensaver_print_card]") {
    using helix::ui::print_state_color;
    const Rgb accent = PrintCardPainter::kAccent;
    CHECK(print_state_color(ClockMode::PAUSED, accent) == helix::ui::kPrintPaused);
    CHECK(print_state_color(ClockMode::DONE, accent) == helix::ui::kPrintDone);
    CHECK(print_state_color(ClockMode::FAILED, accent) == helix::ui::kPrintFailed);
    CHECK(print_state_color(ClockMode::CANCELLED, accent) == helix::ui::kPrintFailed);
    CHECK(print_state_color(ClockMode::COUNTDOWN, accent) == accent);
    CHECK(print_state_color(ClockMode::PROGRESS, accent) == accent);
    CHECK(print_state_color(ClockMode::CLOCK, accent) == accent);

    const Rgb colours[] = {helix::ui::kPrintPaused, helix::ui::kPrintDone, helix::ui::kPrintFailed,
                           accent};
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = i + 1; j < 4; j++) {
            CAPTURE(i, j);
            CHECK(PixelWriter::pack(colours[i], PixelFormat::RGB565) !=
                  PixelWriter::pack(colours[j], PixelFormat::RGB565));
        }
    }
}

TEST_CASE("the card sits centred at the bottom of every screen shape, inside it and clear of the "
          "centre band at every drift offset",
          "[screensaver][screensaver_print_card]") {
    struct Screen {
        int32_t w;
        int32_t h;
    };
    for (const Screen s : {Screen{800, 480}, Screen{1024, 600}, Screen{480, 272}, Screen{272, 480},
                           Screen{1920, 1080}, Screen{320, 240}}) {
        CAPTURE(s.w, s.h);
        const PrintCardLayout layout = helix::ui::layout_print_card(s.w, s.h);
        REQUIRE(layout.w > layout.ring_d);
        // Readable across a room: the ring is a fifth of the short side.
        CHECK(layout.ring_d >= std::min(s.w, s.h) / 5);
        const DirtyRect home = helix::ui::print_card_home(s.w, s.h, layout);
        CHECK(home.x2 - home.x1 + 1 == layout.w);
        CHECK(home.y2 - home.y1 + 1 == layout.h);
        int32_t max_dx = 0;
        int32_t max_dy = 0;
        helix::ui::clock_drift_range(home, s.w, s.h, max_dx, max_dy);
        CHECK(max_dx > 0);
        CHECK(max_dy > 0);
        CHECK(home.x1 - max_dx >= 0);
        CHECK(home.y1 - max_dy >= 0);
        CHECK(home.x2 + max_dx < s.w);
        CHECK(home.y2 + max_dy < s.h);
        CHECK(home.x1 == (s.w - layout.w) / 2);
        // The planet, the tunnel's light and the fireflies' text sit in the middle of the
        // screen: the card stays in its bottom third.
        CHECK(home.y1 - max_dy >= s.h * 2 / 3);
    }
}

TEST_CASE("the ring fills with progress in the state colour",
          "[screensaver][screensaver_print_card]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const Rgb accent = PrintCardPainter::kAccent;
    const size_t none = pixels_of(format, face_of(ClockMode::COUNTDOWN, 0, 600), accent);
    const size_t half = pixels_of(format, face_of(ClockMode::COUNTDOWN, 50, 600), accent);
    const size_t most = pixels_of(format, face_of(ClockMode::COUNTDOWN, 90, 600), accent);
    CHECK(none == 0);
    CHECK(half > 100);
    CHECK(most > half);
    CHECK(pixels_of(format, face_of(ClockMode::PAUSED, 50, 600), helix::ui::kPrintPaused) > 100);
    CHECK(pixels_of(format, face_of(ClockMode::PAUSED, 50, 600), accent) == 0);
}

TEST_CASE("each end state draws its own mark in its own colour",
          "[screensaver][screensaver_print_card]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const auto done = card_bytes(format, face_of(ClockMode::DONE, 100));
    const auto failed = card_bytes(format, face_of(ClockMode::FAILED, 100));
    const auto cancelled = card_bytes(format, face_of(ClockMode::CANCELLED, 100));
    CHECK(bytes_differing(done, failed) > 0);
    CHECK(bytes_differing(failed, cancelled) > 0);
    CHECK(bytes_differing(done, cancelled) > 0);
    CHECK(pixels_of(format, face_of(ClockMode::DONE, 100), helix::ui::kPrintDone) > 100);
    CHECK(pixels_of(format, face_of(ClockMode::FAILED, 40), helix::ui::kPrintFailed) > 100);
}

TEST_CASE("the card shows hours and minutes left, minutes under an hour, and percent without an "
          "estimate",
          "[screensaver][screensaver_print_card]") {
    CHECK(print_card_text(face_of(ClockMode::COUNTDOWN, 10, 3725)) == "1h02");
    CHECK(print_card_text(face_of(ClockMode::COUNTDOWN, 10, 59)) == "1m");
    CHECK(print_card_text(face_of(ClockMode::COUNTDOWN, 10, 1325)) == "23m");
    CHECK(print_card_text(face_of(ClockMode::COUNTDOWN, 10, 999 * 3600)) == "99h59");
    CHECK(print_card_text(face_of(ClockMode::PROGRESS, 63)) == "63%");
    CHECK(print_card_text(face_of(ClockMode::PAUSED, 63)) == "63%");
    CHECK(print_card_text(face_of(ClockMode::PAUSED, 63, 125)) == "3m");
    CHECK(print_card_text(face_of(ClockMode::FAILED, 41)) == "41%");
    CHECK(print_card_text(face_of(ClockMode::DONE, 100)) == "100%");

    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const auto hour = card_bytes(format, face_of(ClockMode::COUNTDOWN, 10, 3725));
    const auto minute = card_bytes(format, face_of(ClockMode::COUNTDOWN, 10, 59));
    const auto pct = card_bytes(format, face_of(ClockMode::PROGRESS, 10));
    CHECK(bytes_differing(hour, minute) > 0);
    CHECK(bytes_differing(minute, pct) > 0);
    CHECK(bytes_differing(hour, card_bytes(format, face_of(ClockMode::COUNTDOWN, 10, 3725))) == 0);
}

TEST_CASE("the card face follows the reading", "[screensaver][screensaver_print_card]") {
    helix::ui::ClockReading r;
    r.mode = ClockMode::COUNTDOWN;
    r.countdown_s = 3725;
    r.has_countdown = true;
    r.progress_pct = 12;
    const PrintCardFace f = helix::ui::print_card_face(r);
    CHECK(f.mode == ClockMode::COUNTDOWN);
    // The text shows whole minutes, so the face drops the seconds: down from an hour up, up below
    // it, so a running print never reads 0m.
    CHECK(f.countdown_s == 3720);
    r.countdown_s = 1325;
    CHECK(helix::ui::print_card_face(r).countdown_s == 1380);
    CHECK(f.has_countdown);
    CHECK(f.progress_pct == 12);
}

TEST_CASE("the card shows for paused and ended prints whatever the setting, and for running "
          "prints only with it",
          "[screensaver][screensaver_print_card]") {
    using helix::ui::print_card_visible;
    using helix::ui::PrintDisplay;
    for (const bool setting : {false, true}) {
        CAPTURE(setting);
        CHECK_FALSE(print_card_visible(PrintDisplay::CARD, setting, ClockMode::CLOCK));
        CHECK(print_card_visible(PrintDisplay::CARD, setting, ClockMode::PROGRESS) == setting);
        CHECK(print_card_visible(PrintDisplay::CARD, setting, ClockMode::COUNTDOWN) == setting);
        CHECK(print_card_visible(PrintDisplay::CARD, setting, ClockMode::PAUSED));
        CHECK(print_card_visible(PrintDisplay::CARD, setting, ClockMode::DONE));
        CHECK(print_card_visible(PrintDisplay::CARD, setting, ClockMode::FAILED));
        CHECK(print_card_visible(PrintDisplay::CARD, setting, ClockMode::CANCELLED));
        for (const ClockMode m :
             {ClockMode::CLOCK, ClockMode::PROGRESS, ClockMode::COUNTDOWN, ClockMode::PAUSED,
              ClockMode::DONE, ClockMode::FAILED, ClockMode::CANCELLED}) {
            CHECK_FALSE(print_card_visible(PrintDisplay::SCENE, setting, m));
        }
    }
}

TEST_CASE("only savers without their own print display show the card",
          "[screensaver][screensaver_print_card]") {
    for (const helix::ui::ScreensaverInfo& info : helix::ui::kScreensavers) {
        CAPTURE(info.name);
        const bool own = info.type == ScreensaverType::FLIP_CLOCK ||
                         info.type == ScreensaverType::AQUA ||
                         info.type == ScreensaverType::HIGHFLY;
        CHECK((info.print_display == helix::ui::PrintDisplay::SCENE) == own);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a saver without its own display shows the card over its scene",
                 "[screensaver][screensaver_print_card]") {
    StarfieldScreensaver ss;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
    helix::ui::ClockInput reported;
    reported.print = {helix::ui::ClockPrintPhase::PRINTING, 3725, 34};
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    advance_saver(ss, 33);
    REQUIRE(ss.print_card_shown());
    CHECK(ss.print_card_face().mode == ClockMode::COUNTDOWN);
    CHECK(print_card_text(ss.print_card_face()) == "1h02");

    // The card sits on screen, above the scene.
    lv_obj_t* card = ss.print_card_obj();
    REQUIRE(card != nullptr);
    lv_obj_update_layout(card);
    lv_area_t a;
    lv_obj_get_coords(card, &a);
    const lv_display_t* disp = lv_display_get_default();
    CHECK(a.x1 >= 0);
    CHECK(a.y1 >= 0);
    CHECK(a.x2 < lv_display_get_horizontal_resolution(disp));
    CHECK(a.y2 < lv_display_get_vertical_resolution(disp));
    CHECK(lv_obj_get_index(card) ==
          static_cast<int32_t>(lv_obj_get_child_count(lv_obj_get_parent(card))) - 1);

    // The setting hides a running print's card, never a paused or ended one.
    reported.show_print_progress = false;
    advance_saver(ss, 33);
    CHECK_FALSE(ss.print_card_shown());
    reported.print.phase = helix::ui::ClockPrintPhase::PAUSED;
    advance_saver(ss, 33);
    CHECK(ss.print_card_shown());
    reported.print.phase = helix::ui::ClockPrintPhase::FAILED;
    advance_saver(ss, 33);
    CHECK(ss.print_card_shown());
    CHECK(ss.print_card_face().mode == ClockMode::FAILED);
    reported.print.phase = helix::ui::ClockPrintPhase::NONE;
    advance_saver(ss, 33);
    CHECK_FALSE(ss.print_card_shown());
}

#endif // HELIX_ENABLE_SCREENSAVER
