// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_manager_test_access.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "printer_state.h"
#include "screensaver_aqua.h"
#include "screensaver_canvas.h"
#include "screensaver_clock_reading.h"
#include "screensaver_flip_clock.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_highfly.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_print_colors.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::test::bytes_differing;
using helix::test::check_replays;
using helix::test::diff_against;
using helix::test::FrameDiff;
using helix::test::hms;
using helix::test::TestFrame;
using helix::ui::clock_face;
using helix::ui::ClockInput;
using helix::ui::ClockMeridiem;
using helix::ui::ClockMode;
using helix::ui::ClockPrintPhase;
using helix::ui::countdown_face;
using helix::ui::DirtyRect;
using helix::ui::FlipClockScreensaver;
using helix::ui::FlipClockSim;
using helix::ui::FlipClockState;
using helix::ui::FlipFace;
using helix::ui::FlipLayout;
using helix::ui::FlipShape;
using helix::ui::FrameTarget;
using helix::ui::kClockCountdownResyncS;
using helix::ui::kClockDriftStepMs;
using helix::ui::kClockMaxCountdownS;
using helix::ui::kFlipClockLevelCount;
using helix::ui::kFlipClockLevels;
using helix::ui::kFlipDurationMs;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;
using helix::ui::SaverTestAccess;

namespace {

/// One character per card, a blank card as a space, groups joined with ':'.
std::string cards_text(const FlipFace& face) {
    std::string out;
    size_t i = 0;
    for (size_t g = 0; g < face.shape.group_count; g++) {
        if (g > 0) {
            out += ':';
        }
        for (size_t c = 0; c < face.shape.groups[g]; c++, i++) {
            out += face.cards[i].digit == '\0' ? ' ' : face.cards[i].digit;
        }
    }
    return out;
}

/// The small row the same way, a colon after the first `sub_colons` groups and a space after
/// the rest.
std::string sub_text(const FlipFace& face) {
    std::string out;
    size_t i = 0;
    for (size_t g = 0; g < face.sub_shape.group_count; g++) {
        if (g > 0) {
            out += g <= face.sub_colons ? ':' : ' ';
        }
        for (size_t c = 0; c < face.sub_shape.groups[g]; c++, i++) {
            out += face.sub[i].digit == '\0' ? ' ' : face.sub[i].digit;
        }
    }
    return out;
}

FlipShape shape_of(std::initializer_list<uint8_t> groups) {
    FlipShape shape;
    for (const uint8_t g : groups) {
        shape.groups[shape.group_count++] = g;
    }
    return shape;
}

ClockInput printing(int32_t remaining_s, int32_t progress = 40) {
    ClockInput in;
    in.seconds_of_day = hms(14, 0);
    in.print.phase = ClockPrintPhase::PRINTING;
    in.print.remaining_s = remaining_s;
    in.print.progress_pct = progress;
    return in;
}

/// Pixels inside `box` at least as bright as half the digit colour.
size_t lit_in(const TestFrame& frame, const DirtyRect& box) {
    const PixelWriter writer(frame.target);
    size_t lit = 0;
    for (int32_t y = box.y1; y <= box.y2; y++) {
        for (int32_t x = box.x1; x <= box.x2; x++) {
            lit += writer.get(x, y).g > FlipClockSim::kDigit.g / 2 ? 1 : 0;
        }
    }
    return lit;
}

std::vector<uint8_t> card_bytes(const TestFrame& frame, const DirtyRect& box) {
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

/// The clock reading of the saver the manager runs, which must be a `Saver` of `type`, after
/// advancing it `advance_ms`.
template <typename Saver>
helix::ui::ClockReading active_clock_reading(ScreensaverType type, uint32_t advance_ms = 0) {
    helix::ui::SaverBase* saver =
        helix::ScreensaverManagerTestAccess::active(ScreensaverManager::instance());
    REQUIRE(saver != nullptr);
    REQUIRE(saver->type() == type);
    if (advance_ms > 0) {
        advance_saver(*saver, advance_ms);
    }
    return static_cast<Saver*>(saver)->clock().reading();
}

} // namespace

// ============================================================================
// Faces
// ============================================================================

TEST_CASE("the clock face shows 24-hour time with a leading zero and no marker",
          "[screensaver][screensaver_flip_clock]") {
    CHECK(cards_text(clock_face(hms(9, 5), true)) == "09:05");
    CHECK(clock_face(hms(9, 5), true).shape == shape_of({2, 2}));
    CHECK(cards_text(clock_face(hms(0, 0), true)) == "00:00");
    CHECK(cards_text(clock_face(hms(23, 59, 59), true)) == "23:59");
    CHECK(clock_face(hms(13, 0), true).meridiem == ClockMeridiem::NONE);
    CHECK_FALSE(clock_face(hms(13, 0), true).has_sub());
}

TEST_CASE("the clock face shows 12-hour time with a blank leading card, marked AM or PM",
          "[screensaver][screensaver_flip_clock]") {
    CHECK(cards_text(clock_face(hms(0, 30), false)) == "12:30");
    CHECK(clock_face(hms(0, 30), false).meridiem == ClockMeridiem::AM);
    CHECK(cards_text(clock_face(hms(9, 42), false)) == " 9:42");
    CHECK(clock_face(hms(9, 42), false).shape == shape_of({2, 2}));
    CHECK(clock_face(hms(11, 59), false).meridiem == ClockMeridiem::AM);
    CHECK(cards_text(clock_face(hms(12, 0), false)) == "12:00");
    CHECK(clock_face(hms(12, 0), false).meridiem == ClockMeridiem::PM);
    CHECK(cards_text(clock_face(hms(13, 7), false)) == " 1:07");
    CHECK(clock_face(hms(23, 59), false).meridiem == ClockMeridiem::PM);
}

TEST_CASE("the countdown face shows H:MM:SS under an hour, past ten hours and at the limits",
          "[screensaver][screensaver_flip_clock]") {
    CHECK(countdown_face(hms(1, 2, 5)).shape == shape_of({1, 2, 2}));
    CHECK(countdown_face(hms(10, 2, 5)).shape == shape_of({2, 2, 2}));
    CHECK(countdown_face(hms(100, 2, 5)).shape == shape_of({3, 2, 2}));
    CHECK(countdown_face(hms(100, 2, 5)).shape.card_count() == 7);
    CHECK(cards_text(countdown_face(hms(1, 2, 5))) == "1:02:05");
    CHECK(cards_text(countdown_face(59)) == "0:00:59");
    CHECK(cards_text(countdown_face(hms(0, 45, 0))) == "0:45:00");
    CHECK(cards_text(countdown_face(hms(10, 20, 30))) == "10:20:30");
    CHECK(cards_text(countdown_face(hms(37, 0, 1))) == "37:00:01");
    CHECK(cards_text(countdown_face(0)) == "0:00:00");
    CHECK(cards_text(countdown_face(-5)) == "0:00:00");
    CHECK(cards_text(countdown_face(hms(123, 4, 5))) == "123:04:05");
    CHECK(cards_text(countdown_face(kClockMaxCountdownS)) == "999:59:59");
    CHECK(cards_text(countdown_face(kClockMaxCountdownS + 3600)) == "999:59:59");
}

// ============================================================================
// FlipClockState
// ============================================================================

TEST_CASE("the cards show the time, a running print's percent before its estimate, then the "
          "countdown",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    ClockInput in;
    in.seconds_of_day = hms(21, 5);
    in.use_24h = true;

    SECTION("idle") {
        state.step(16, in);
        CHECK(state.mode() == ClockMode::CLOCK);
        CHECK(cards_text(state.face()) == "21:05");
    }
    SECTION("preparing without an estimate") {
        in.print.phase = ClockPrintPhase::PREPARING;
        state.step(16, in);
        CHECK(state.mode() == ClockMode::PROGRESS);
        CHECK(cards_text(state.face()) == "0%");
        CHECK(sub_text(state.face()) == "21:05");
    }
    SECTION("preparing once an estimate exists") {
        in.print.phase = ClockPrintPhase::PREPARING;
        in.print.remaining_s = 600;
        state.step(16, in);
        CHECK(state.mode() == ClockMode::COUNTDOWN);
        CHECK(cards_text(state.face()) == "0:10:00");
    }
    SECTION("printing without an estimate") {
        in.print.phase = ClockPrintPhase::PRINTING;
        in.print.remaining_s = -1;
        state.step(16, in);
        CHECK(state.mode() == ClockMode::PROGRESS);
        CHECK(cards_text(state.face()) == "0%");
    }
}

TEST_CASE("the cards spell how the print stands, with its details on the small row",
          "[screensaver][screensaver_flip_clock]") {
    const auto face_for = [](ClockPrintPhase phase, int32_t remaining, int32_t pct, bool h24) {
        FlipClockState state;
        ClockInput in = printing(hms(1, 12), pct);
        in.seconds_of_day = hms(20, 40, 0);
        in.use_24h = h24;
        state.step(16, in);
        in.print.phase = phase;
        in.print.remaining_s = remaining;
        state.step(16, in);
        return state.face();
    };
    struct Row {
        const char* name;
        ClockPrintPhase phase;
        int32_t remaining;
        int32_t pct;
        bool h24;
        const char* main;
        const char* sub;
        ClockMode mode;
    };
    // 20:40 + 1:12 = 21:52; 8:40 PM + 1:12 = 9:52.
    const Row rows[] = {
        {"countdown 24h", ClockPrintPhase::PRINTING, hms(1, 12), 63, true, "1:12:00", "21:52 63%",
         ClockMode::COUNTDOWN},
        {"countdown 12h", ClockPrintPhase::PRINTING, hms(1, 12), 63, false, "1:12:00", "9:52 63%",
         ClockMode::COUNTDOWN},
        {"paused", ClockPrintPhase::PAUSED, hms(1, 12), 63, true, "PAUSE", "1:12 63%",
         ClockMode::PAUSED},
        {"done", ClockPrintPhase::COMPLETE, 0, 100, true, "DONE", "100%", ClockMode::DONE},
        {"failed", ClockPrintPhase::FAILED, 0, 41, true, "FAIL", "41%", ClockMode::FAILED},
        {"cancelled", ClockPrintPhase::CANCELLED, 0, 72, true, "STOP", "72%", ClockMode::CANCELLED},
    };
    for (const Row& r : rows) {
        CAPTURE(r.name);
        const FlipFace face = face_for(r.phase, r.remaining, r.pct, r.h24);
        CHECK(cards_text(face) == r.main);
        CHECK(sub_text(face) == r.sub);
        CHECK(face.mode == r.mode);
    }

    // Before an estimate the cards show the percentage over the time of day.
    FlipClockState state;
    ClockInput in = printing(-1, 37);
    in.seconds_of_day = hms(20, 40, 0);
    in.use_24h = true;
    state.step(16, in);
    CHECK(cards_text(state.face()) == "37%");
    CHECK(sub_text(state.face()) == "20:40");
}

TEST_CASE("letter faces carry no AM/PM marker", "[screensaver][screensaver_flip_clock]") {
    for (const ClockPrintPhase end :
         {ClockPrintPhase::PAUSED, ClockPrintPhase::COMPLETE, ClockPrintPhase::FAILED}) {
        FlipClockState state;
        ClockInput in = printing(600, 40);
        in.use_24h = false;
        state.step(16, in);
        in.print.phase = end;
        state.step(16, in);
        CHECK(state.face().meridiem == ClockMeridiem::NONE);
    }
}

TEST_CASE("the finish time on the small row wraps past midnight",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    ClockInput in = printing(hms(0, 30));
    in.seconds_of_day = hms(23, 50);
    in.use_24h = true;
    state.step(16, in);
    CHECK(sub_text(state.face()) == "00:20 40%");
}

TEST_CASE("the countdown ticks at wall-clock rate and ignores small estimate jitter",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    ClockInput in = printing(1000);
    state.step(16, in);
    REQUIRE(state.countdown_s() == 1000);

    // The estimate holds still while a second passes: the cards still tick.
    state.step(1000, in);
    CHECK(state.countdown_s() == 999);
    state.step(2000, in);
    CHECK(state.countdown_s() == 997);

    // An estimate a few seconds above the running count never flips it backwards.
    in.print.remaining_s = 997 + kClockCountdownResyncS;
    state.step(0, in);
    CHECK(state.countdown_s() == 997);

    // A real re-estimate resets it.
    in.print.remaining_s = 1500;
    state.step(0, in);
    CHECK(state.countdown_s() == 1500);
    in.print.remaining_s = 400;
    state.step(0, in);
    CHECK(state.countdown_s() == 400);

    // It never counts below zero.
    in.print.remaining_s = 2;
    state.step(0, in);
    state.step(5000, in);
    CHECK(state.countdown_s() == 0);
}

TEST_CASE("a paused print freezes the countdown under PAUSED until it resumes",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    ClockInput in = printing(hms(0, 30));
    state.step(16, in);
    state.step(5000, in);
    REQUIRE(state.countdown_s() == hms(0, 29, 55));

    in.print.phase = ClockPrintPhase::PAUSED;
    state.step(16, in);
    CHECK(state.mode() == ClockMode::PAUSED);
    for (int i = 0; i < 300; i++) {
        state.step(1000, in);
    }
    CHECK(state.countdown_s() == hms(0, 29, 55));
    CHECK(cards_text(state.face()) == "PAUSE");
    CHECK(sub_text(state.face()) == "0:29 40%");

    in.print.phase = ClockPrintPhase::PRINTING;
    state.step(0, in);
    CHECK(state.mode() == ClockMode::COUNTDOWN);
    state.step(1000, in);
    CHECK(state.countdown_s() == hms(0, 29, 54));
}

TEST_CASE("a finished print holds DONE until the print goes idle",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    ClockInput in = printing(30, 99);
    in.use_24h = true;
    state.step(16, in);
    REQUIRE(state.mode() == ClockMode::COUNTDOWN);

    in.print.phase = ClockPrintPhase::COMPLETE;
    in.print.remaining_s = 0;
    state.step(16, in);
    CHECK(state.mode() == ClockMode::DONE);
    CHECK(cards_text(state.face()) == "DONE");

    state.step(3600 * 1000, in);
    CHECK(state.mode() == ClockMode::DONE);

    in.print.phase = ClockPrintPhase::NONE;
    state.step(16, in);
    CHECK(state.mode() == ClockMode::CLOCK);
    CHECK(cards_text(state.face()) == "14:00");
}

TEST_CASE("a failed or cancelled print holds its end until the print goes idle",
          "[screensaver][screensaver_flip_clock]") {
    const ClockPrintPhase end = GENERATE(ClockPrintPhase::FAILED, ClockPrintPhase::CANCELLED);
    FlipClockState state;
    ClockInput in = printing(300);
    state.step(16, in);
    in.print.phase = end;
    state.step(16, in);
    CHECK(state.mode() ==
          (end == ClockPrintPhase::FAILED ? ClockMode::FAILED : ClockMode::CANCELLED));
    in.print.phase = ClockPrintPhase::NONE;
    in.print.remaining_s = -1;
    state.step(16, in);
    CHECK(state.mode() == ClockMode::CLOCK);
}

TEST_CASE("the flip clock reads every print lifecycle state, and an idle printer's last end",
          "[screensaver][screensaver_flip_clock]") {
    using helix::PrintOutcome;
    using helix::ui::clock_print_phase;
    CHECK(clock_print_phase(PrintState::Idle, PrintOutcome::NONE) == ClockPrintPhase::NONE);
    CHECK(clock_print_phase(PrintState::Preparing, PrintOutcome::NONE) ==
          ClockPrintPhase::PREPARING);
    CHECK(clock_print_phase(PrintState::Printing, PrintOutcome::NONE) == ClockPrintPhase::PRINTING);
    CHECK(clock_print_phase(PrintState::Paused, PrintOutcome::NONE) == ClockPrintPhase::PAUSED);
    CHECK(clock_print_phase(PrintState::Complete, PrintOutcome::COMPLETE) ==
          ClockPrintPhase::COMPLETE);
    CHECK(clock_print_phase(PrintState::Cancelled, PrintOutcome::CANCELLED) ==
          ClockPrintPhase::CANCELLED);
    CHECK(clock_print_phase(PrintState::Error, PrintOutcome::ERROR) == ClockPrintPhase::FAILED);
    // A printer back on standby after a print still reports how that print ended.
    CHECK(clock_print_phase(PrintState::Idle, PrintOutcome::COMPLETE) == ClockPrintPhase::COMPLETE);
    CHECK(clock_print_phase(PrintState::Idle, PrintOutcome::CANCELLED) ==
          ClockPrintPhase::CANCELLED);
    CHECK(clock_print_phase(PrintState::Idle, PrintOutcome::ERROR) == ClockPrintPhase::FAILED);
    CHECK(clock_print_phase(PrintState::Printing, PrintOutcome::COMPLETE) ==
          ClockPrintPhase::PRINTING);
}

// ============================================================================
// Layout
// ============================================================================

TEST_CASE("every face fits every screen shape at the widest drift, small row included",
          "[screensaver][screensaver_flip_clock]") {
    struct Screen {
        int32_t w;
        int32_t h;
    };
    const Screen screen = GENERATE(Screen{800, 480}, Screen{480, 272}, Screen{1024, 600},
                                   Screen{480, 800}, Screen{272, 480}, Screen{1280, 720});
    struct Faces {
        FlipShape shape;
        FlipShape sub;
    };
    // The clock; countdowns with one, two and three hour digits and the finish time and
    // percent under them; PAUSE over time left and percent; DONE over 100%; 37% over the time.
    const Faces faces = GENERATE(
        Faces{shape_of({2, 2}), shape_of({})}, Faces{shape_of({1, 2, 2}), shape_of({1, 2, 3})},
        Faces{shape_of({2, 2, 2}), shape_of({2, 2, 3})},
        Faces{shape_of({3, 2, 2}), shape_of({2, 2, 4})}, Faces{shape_of({5}), shape_of({2, 2, 4})},
        Faces{shape_of({4}), shape_of({4})}, Faces{shape_of({3}), shape_of({2, 2})});
    const FlipShape& shape = faces.shape;
    const uint8_t count = shape.card_count();
    const uint8_t sub_count = faces.sub.card_count();
    CAPTURE(screen.w, screen.h, count, shape.groups[0], sub_count);

    helix::ui::ClockDrift drift;
    drift.set_range(helix::ui::flip_centred_bounds(screen.w, screen.h, shape, faces.sub), screen.w,
                    screen.h);
    const int32_t max_dx = drift.max_dx;
    const int32_t max_dy = drift.max_dy;
    CHECK(max_dx > 0);
    CHECK(max_dy > 0);
    const DirtyRect whole{0, 0, screen.w - 1, screen.h - 1};
    for (int sx : {-1, 0, 1}) {
        for (int sy : {-1, 0, 1}) {
            const FlipLayout layout = helix::ui::layout_flip_cards(
                screen.w, screen.h, shape, faces.sub, sx * max_dx, sy * max_dy);
            REQUIRE(layout.card_count == count);
            REQUIRE(layout.sub_count == sub_count);
            CHECK(layout.shape == shape);
            CHECK(inside(layout.bounds(), whole));
            for (size_t j = count; j < count + sub_count; j++) {
                CHECK(inside(layout.cards[j], whole));
                // Small cards sit below the main row and are smaller than its cards.
                CHECK(layout.cards[j].y1 > layout.cards[count - 1].y2);
                CHECK(layout.cards[j].y2 - layout.cards[j].y1 <
                      layout.cards[0].y2 - layout.cards[0].y1);
                for (size_t k = j + 1; k < count + sub_count; k++) {
                    CHECK_FALSE(layout.cards[j].intersects(layout.cards[k]));
                }
            }
            CHECK(inside(layout.meridiem, layout.cards[0]));
            for (size_t i = 0; i < count; i++) {
                CHECK(inside(layout.cards[i], whole));
                CHECK(layout.cards[i].x2 - layout.cards[i].x1 ==
                      layout.cards[0].x2 - layout.cards[0].x1);
                CHECK(layout.cards[i].y2 - layout.cards[i].y1 ==
                      layout.cards[0].y2 - layout.cards[0].y1);
                for (size_t j = i + 1; j < count; j++) {
                    CHECK_FALSE(layout.cards[i].intersects(layout.cards[j]));
                }
                for (size_t c = 0; c < layout.colon_count; c++) {
                    CHECK_FALSE(layout.cards[i].intersects(layout.colons[c]));
                }
            }
            for (size_t c = 0; c < layout.colon_count; c++) {
                CHECK(inside(layout.colons[c], layout.bounds()));
            }
            CHECK(layout.split_top > 0);
            CHECK(layout.split_bottom > layout.split_top);
        }
    }

    const FlipLayout layout =
        helix::ui::layout_flip_cards(screen.w, screen.h, shape, faces.sub, 0, 0);
    // The cards take a large share of the screen; a single word across a portrait screen is
    // bound by its width.
    CHECK(layout.bounds().area() * 100 > static_cast<int64_t>(screen.w) * screen.h * 15);
    // A face with no small row centres its cards.
    if (sub_count == 0) {
        const DirtyRect b = layout.bounds();
        CHECK(std::abs((b.y1 + b.y2) - (screen.h - 1)) <= 1);
    }
    // A portrait screen puts each group on its own row with no colons; a landscape one lines
    // every card up with a colon between groups. A single group has nothing to stack.
    const bool rows = layout.cards[count - 1].y1 > layout.cards[0].y2;
    CHECK(rows == (screen.h > screen.w && shape.group_count > 1));
    CHECK(layout.main_colon_count == (rows ? 0 : shape.group_count - 1));
    // The cards of a group always share a row.
    size_t i = 0;
    for (size_t g = 0; g < shape.group_count; g++) {
        for (size_t c = 1; c < shape.groups[g]; c++) {
            CHECK(layout.cards[i + c].y1 == layout.cards[i].y1);
            CHECK(layout.cards[i + c].x1 > layout.cards[i + c - 1].x2);
        }
        i += shape.groups[g];
    }
}

TEST_CASE("the drift moves the cards once per step and stays within its range",
          "[screensaver][screensaver_flip_clock]") {
    int32_t dx = 0;
    int32_t dy = 0;
    helix::ui::clock_drift_at(0, 20, 10, dx, dy);
    CHECK(dx == 0);
    CHECK(dy == 0);
    helix::ui::clock_drift_at(kClockDriftStepMs - 1, 20, 10, dx, dy);
    CHECK(dx == 0);
    CHECK(dy == 0);
    bool moved = false;
    for (uint32_t step = 1; step < 200; step++) {
        helix::ui::clock_drift_at(step * kClockDriftStepMs, 20, 10, dx, dy);
        CHECK(std::abs(dx) <= 20);
        CHECK(std::abs(dy) <= 10);
        moved = moved || dx != 0 || dy != 0;
    }
    CHECK(moved);
}

// ============================================================================
// FlipClockSim
// ============================================================================

TEST_CASE("the flip clock draws light digits on dark cards over black, in either format",
          "[screensaver][screensaver_flip_clock]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    TestFrame frame(400, 240, format);
    FlipClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const FlipLayout& layout = sim.layout();
    const PixelWriter writer(frame.target);

    CHECK(writer.get(0, 0) == FlipClockSim::kBackground);
    for (size_t i = 0; i < layout.card_count; i++) {
        CAPTURE(i);
        const DirtyRect& card = layout.cards[i];
        const int32_t cx = card.x1 + layout.corner + 1;
        // Card face just inside the corner, and the split line across the middle.
        CHECK(writer.get(cx, card.y1 + layout.corner + 1).g < 60);
        CHECK(writer.get(cx, card.y1 + layout.corner + 1).g > 10);
        CHECK(writer.get(cx, card.y1 + layout.split_top) == FlipClockSim::kBackground);
        CHECK(lit_in(frame, card) > 100);
    }
    CHECK(writer.get(layout.cards[0].x1, layout.cards[0].y1) == FlipClockSim::kBackground);
    // The colon between hours and minutes.
    REQUIRE(layout.colon_count == 1);
    const DirtyRect& colon = layout.colons[0];
    size_t colon_px = 0;
    for (int32_t y = colon.y1; y <= colon.y2; y++) {
        for (int32_t x = colon.x1; x <= colon.x2; x++) {
            colon_px += writer.get(x, y) == FlipClockSim::kBackground ? 0 : 1;
        }
    }
    CHECK(colon_px > 4);
}

TEST_CASE("a blank card draws no digit", "[screensaver][screensaver_flip_clock]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(9, 41), false));
    CHECK(lit_in(frame, sim.layout().cards[0]) == 0);
    CHECK(lit_in(frame, sim.layout().cards[1]) > 50);
}

TEST_CASE("different digits draw different cards", "[screensaver][screensaver_flip_clock]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    FlipClockSim sim;
    std::vector<std::vector<uint8_t>> seen;
    for (int d = 0; d < 10; d++) {
        FlipFace face = clock_face(hms(d, d), true);
        sim.init(frame.target, 2, face);
        const std::vector<uint8_t> card = card_bytes(frame, sim.layout().cards[3]);
        for (const auto& other : seen) {
            CHECK(bytes_differing(card, other) > 0);
        }
        seen.push_back(card);
    }
}

TEST_CASE("a changed card folds over the flip duration while the others stay still",
          "[screensaver][screensaver_flip_clock]") {
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const DirtyRect minutes = sim.layout().cards[3];
    const std::vector<uint8_t> start = card_bytes(frame, minutes);

    std::vector<DirtyRect> dirty;
    const FlipFace next = clock_face(hms(10, 9), true);
    sim.step(0, frame.target, next, dirty);
    CHECK(sim.flipping());
    CHECK(sim.flip_progress(0) == 1.0f);
    CHECK(sim.flip_progress(3) == 0.0f);

    sim.step(kFlipDurationMs / 4, frame.target, next, dirty);
    CHECK(sim.flip_progress(3) == Catch::Approx(0.25f));
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0] == minutes);
    const std::vector<uint8_t> quarter = card_bytes(frame, minutes);
    CHECK(bytes_differing(quarter, start) > 0);

    sim.step(kFlipDurationMs / 2, frame.target, next, dirty);
    CHECK(sim.flip_progress(3) == Catch::Approx(0.75f));
    const std::vector<uint8_t> three_quarters = card_bytes(frame, minutes);
    CHECK(bytes_differing(three_quarters, quarter) > 0);

    sim.step(kFlipDurationMs / 4, frame.target, next, dirty);
    CHECK_FALSE(sim.flipping());
    const std::vector<uint8_t> end = card_bytes(frame, minutes);
    CHECK(bytes_differing(end, three_quarters) > 0);

    // The finished flip draws exactly what a still card of the new digits draws.
    TestFrame still(400, 240, PixelFormat::XRGB8888);
    FlipClockSim fresh;
    fresh.init(still.target, 0, next);
    CHECK(bytes_differing(end, card_bytes(still, minutes)) == 0);
}

TEST_CASE("a folding flap shows its lit edge on the way up and on the way down",
          "[screensaver][screensaver_flip_clock]") {
    TestFrame frame(800, 480, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const DirtyRect card = sim.layout().cards[3];
    const int32_t split = card.y1 + sim.layout().split_top;
    // A column beside the digit, clear of the rounded corners.
    const int32_t x = card.x1 + sim.layout().corner + 1;
    const PixelWriter writer(frame.target);
    const auto rim_rows = [&](int32_t y1, int32_t y2) {
        size_t rows = 0;
        for (int32_t y = y1; y <= y2; y++) {
            rows += writer.get(x, y) == FlipClockSim::kFlapRim ? 1 : 0;
        }
        return rows;
    };
    CHECK(rim_rows(card.y1, card.y2) == 0);

    std::vector<DirtyRect> dirty;
    const FlipFace next = clock_face(hms(10, 9), true);
    sim.step(0, frame.target, next, dirty);
    sim.step(kFlipDurationMs / 3, frame.target, next, dirty);
    CHECK(rim_rows(card.y1, split - 1) == 1);
    CHECK(rim_rows(split, card.y2) == 0);

    sim.step(kFlipDurationMs / 3, frame.target, next, dirty);
    CHECK(rim_rows(card.y1, split - 1) == 0);
    CHECK(rim_rows(split, card.y2) == 1);

    sim.step(kFlipDurationMs, frame.target, next, dirty);
    CHECK(rim_rows(card.y1, card.y2) == 0);
}

TEST_CASE("only the digits that change flip, each on its own card and all at once",
          "[screensaver][screensaver_flip_clock]") {
    struct Change {
        const char* name;
        FlipFace from;
        FlipFace to;
        std::vector<size_t> flips;
    };
    const Change change = GENERATE(
        Change{"10:08 -> 10:09", clock_face(hms(10, 8), true), clock_face(hms(10, 9), true), {3}},
        Change{"10:59 -> 11:00",
               clock_face(hms(10, 59), true),
               clock_face(hms(11, 0), true),
               {1, 2, 3}},
        Change{"09:59 -> 10:00",
               clock_face(hms(9, 59), true),
               clock_face(hms(10, 0), true),
               {0, 1, 2, 3}},
        Change{"12h 1:09 -> 1:10",
               clock_face(hms(1, 9), false),
               clock_face(hms(1, 10), false),
               {2, 3}},
        Change{"12h 9:59 -> 10:00",
               clock_face(hms(9, 59), false),
               clock_face(hms(10, 0), false),
               {0, 1, 2, 3}},
        Change{"0:00:10 -> 0:00:09", countdown_face(10), countdown_face(9), {3, 4}},
        Change{"0:10:00 -> 0:09:59",
               countdown_face(hms(0, 10, 0)),
               countdown_face(hms(0, 9, 59)),
               {1, 2, 3, 4}},
        Change{"1:00:00 -> 0:59:59",
               countdown_face(hms(1, 0, 0)),
               countdown_face(hms(0, 59, 59)),
               {0, 1, 2, 3, 4}},
        Change{"12:30:00 -> 12:29:59",
               countdown_face(hms(12, 30, 0)),
               countdown_face(hms(12, 29, 59)),
               {2, 3, 4, 5}});
    CAPTURE(change.name);
    const auto flips = [&change](size_t i) {
        return std::find(change.flips.begin(), change.flips.end(), i) != change.flips.end();
    };

    TestFrame frame(800, 480, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, 0, change.from);
    const FlipLayout layout = sim.layout();
    std::vector<std::vector<uint8_t>> before;
    for (size_t i = 0; i < layout.card_count; i++) {
        before.push_back(card_bytes(frame, layout.cards[i]));
    }

    std::vector<DirtyRect> dirty;
    sim.step(0, frame.target, change.to, dirty);
    for (const uint32_t dt : {kFlipDurationMs / 3, kFlipDurationMs / 3}) {
        sim.step(dt, frame.target, change.to, dirty);
        // One box per flipping card, and nothing for the cards that stay.
        std::vector<DirtyRect> expected;
        for (const size_t i : change.flips) {
            expected.push_back(layout.cards[i]);
        }
        CHECK(dirty == expected);
        for (size_t i = 0; i < layout.card_count; i++) {
            CAPTURE(i);
            if (flips(i)) {
                // Every changed card is at the same point of its own fold.
                CHECK(sim.flip_progress(i) == Catch::Approx(sim.flip_progress(change.flips[0])));
                CHECK(sim.flip_progress(i) < 1.0f);
                CHECK(bytes_differing(card_bytes(frame, layout.cards[i]), before[i]) > 0);
            } else {
                CHECK(sim.flip_progress(i) == 1.0f);
                CHECK(bytes_differing(card_bytes(frame, layout.cards[i]), before[i]) == 0);
            }
        }
    }
}

TEST_CASE("between changes a frame writes nothing", "[screensaver][screensaver_flip_clock]") {
    TestFrame frame(400, 240, PixelFormat::RGB565);
    FlipClockSim sim;
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

TEST_CASE("a level that does not animate swaps the digits at once",
          "[screensaver][screensaver_flip_clock]") {
    REQUIRE_FALSE(kFlipClockLevels[kFlipClockLevelCount - 1].animate);
    TestFrame frame(400, 240, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, kFlipClockLevelCount - 1, clock_face(hms(10, 8), true));
    std::vector<DirtyRect> dirty;
    sim.step(0, frame.target, clock_face(hms(10, 9), true), dirty);
    CHECK_FALSE(sim.flipping());
    CHECK(dirty.size() == 1);

    SECTION("and a level request mid-flip finishes the flip") {
        sim.request_level(0);
        sim.step(0, frame.target, clock_face(hms(10, 10), true), dirty);
        REQUIRE(sim.flipping());
        sim.request_level(kFlipClockLevelCount - 1);
        sim.step(0, frame.target, clock_face(hms(10, 10), true), dirty);
        CHECK_FALSE(sim.flipping());
        CHECK(sim.level() == kFlipClockLevelCount - 1);
    }
}

TEST_CASE("the letters draw in the state colour", "[screensaver][screensaver_flip_clock]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    struct Row {
        ClockMode mode;
        Rgb colour;
    };
    for (const Row r : {Row{ClockMode::DONE, helix::ui::kPrintDone},
                        Row{ClockMode::FAILED, helix::ui::kPrintFailed},
                        Row{ClockMode::CANCELLED, helix::ui::kPrintFailed},
                        Row{ClockMode::PAUSED, helix::ui::kPrintPaused},
                        Row{ClockMode::COUNTDOWN, helix::ui::kPrintDone}}) {
        CAPTURE(static_cast<int>(r.mode));
        helix::ui::ClockReading reading;
        reading.mode = r.mode;
        reading.progress_pct = 64;
        reading.countdown_s = 600;
        reading.has_countdown = true;
        TestFrame frame(800, 480, format);
        FlipClockSim sim;
        sim.init(frame.target, 0, helix::ui::flip_face(reading));
        TestFrame probe(1, 1, format);
        PixelWriter(probe.target).put(0, 0, r.colour);
        const Rgb stored = PixelWriter(probe.target).get(0, 0);
        const PixelWriter writer(frame.target);
        size_t hits = 0;
        const DirtyRect& card = sim.layout().cards[0];
        for (int32_t y = card.y1; y <= card.y2; y++) {
            for (int32_t x = card.x1; x <= card.x2; x++) {
                hits += writer.get(x, y) == stored ? 1 : 0;
            }
        }
        // A running print stays white; every end and the pause draw in their own colour.
        if (r.mode == ClockMode::COUNTDOWN) {
            CHECK(hits == 0);
        } else {
            CHECK(hits > 50);
        }
    }
}

TEST_CASE("small cards flip only where their character changes",
          "[screensaver][screensaver_flip_clock]") {
    TestFrame frame(800, 480, PixelFormat::XRGB8888);
    FlipClockSim sim;
    FlipClockState state;
    ClockInput in = printing(hms(1, 0), 63);
    in.seconds_of_day = hms(20, 0, 0);
    in.use_24h = true;
    state.step(0, in);
    sim.init(frame.target, 0, state.face());
    REQUIRE(sim.layout().sub_count == 7);

    // Hold the countdown still so only the percent changes.
    in.print.progress_pct = 64;
    std::vector<DirtyRect> dirty;
    state.step(0, in);
    sim.step(0, frame.target, state.face(), dirty);
    REQUIRE(dirty.size() == 1);
    // "21:00 64%": the units card of the percent, the sixth small card.
    const FlipLayout& layout = sim.layout();
    CHECK(dirty[0] == layout.cards[layout.card_count + 5]);
}

TEST_CASE("every pixel a flip clock frame changes lies in one of its dirty boxes",
          "[screensaver][screensaver_flip_clock]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    TestFrame frame(480, 272, format);
    FlipClockSim sim;
    FlipClockState state;
    ClockInput in;
    in.seconds_of_day = hms(9, 59, 58);
    state.step(0, in);
    sim.init(frame.target, 0, state.face());

    std::vector<DirtyRect> dirty;
    size_t changed = 0;
    size_t uncovered = 0;
    // Clock flips, a print starting (three cards), seconds ticking, a pause, the end, and
    // drift steps along the way.
    for (int i = 0; i < 2000; i++) {
        if (i == 400) {
            in.print.phase = ClockPrintPhase::PRINTING;
            in.print.remaining_s = hms(0, 1, 5);
        }
        if (i == 900) {
            in.print.phase = ClockPrintPhase::PAUSED;
        }
        if (i == 1000) {
            in.print.phase = ClockPrintPhase::PRINTING;
        }
        if (i == 1500) {
            in.print.phase = ClockPrintPhase::COMPLETE;
            in.print.remaining_s = 0;
        }
        in.seconds_of_day = (in.seconds_of_day + (i % 3 == 0 ? 1 : 0)) % 86400;
        // The percent on the small row ticks while the big cards fold.
        in.print.progress_pct = 40 + i / 150;
        const uint32_t dt = i % 50 == 0 ? kClockDriftStepMs / 4 : 33;
        state.step(dt, in);
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(dt, frame.target, state.face(), dirty);
        const FrameDiff diff = diff_against(before, frame, dirty);
        changed += diff.changed;
        uncovered += diff.uncovered;
    }
    REQUIRE(changed > 0);
    CHECK(uncovered == 0);
    CHECK(frame.padding_intact());
}

TEST_CASE("a flip clock run replays exactly and a different print draws differently",
          "[screensaver][screensaver_flip_clock]") {
    // The clock draws no random numbers: what tells two runs apart is the print they count.
    auto run = [](uint32_t remaining_s) {
        TestFrame frame(320, 200, PixelFormat::RGB565);
        FlipClockSim sim;
        FlipClockState state;
        ClockInput in = printing(static_cast<int32_t>(remaining_s));
        state.step(0, in);
        sim.init(frame.target, 0, state.face());
        std::vector<DirtyRect> dirty;
        for (int i = 0; i < 600; i++) {
            const uint32_t dt = i % 97 == 0 ? kClockDriftStepMs / 2 : 33;
            state.step(dt, in);
            sim.step(dt, frame.target, state.face(), dirty);
        }
        return frame.bytes;
    };
    check_replays(run, hms(2, 0, 0), hms(3, 0, 0));
}

// ============================================================================
// FlipClockScreensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "the flip clock saver counts down the print its source reports",
                 "[screensaver][screensaver_flip_clock]") {
    FlipClockScreensaver ss;
    ScreensaverStopOnExit<FlipClockScreensaver> stop_on_exit{ss};
    ClockInput reported = printing(hms(1, 0, 0), 50);
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(ss.clock().mode() == ClockMode::COUNTDOWN);
    CHECK(cards_text(ss.clock().face()) == "1:00:00");

    reported.print.phase = ClockPrintPhase::PAUSED;
    advance_saver(ss, 33);
    CHECK(ss.clock().mode() == ClockMode::PAUSED);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "the clock that follows a finished print shows whole cards where the small row was",
    "[screensaver][screensaver_flip_clock]") {
    FlipClockScreensaver ss;
    ScreensaverStopOnExit<FlipClockScreensaver> stop_on_exit{ss};
    ClockInput reported = printing(30, 90);
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ss.sim().layout().sub_count > 0);
    advance_saver(ss, 33);
    reported.print.phase = ClockPrintPhase::COMPLETE;
    advance_saver(ss, 500);
    REQUIRE(ss.clock().mode() == ClockMode::DONE);
    reported.print.phase = ClockPrintPhase::NONE;
    advance_saver(ss, 500);
    REQUIRE(ss.clock().mode() == ClockMode::CLOCK);

    // Every card pixel off the split line and clear of the rounded corners is card or digit,
    // never the black background.
    const FrameTarget frame =
        helix::test::frame_of(lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss)));
    const PixelWriter writer(frame);
    const FlipLayout& layout = ss.sim().layout();
    REQUIRE(layout.sub_count == 0);
    size_t black = 0;
    for (size_t i = 0; i < layout.card_count; i++) {
        const DirtyRect& card = layout.cards[i];
        for (int32_t y = card.y1 + layout.corner; y <= card.y2 - layout.corner; y++) {
            const int32_t row = y - card.y1;
            if (row >= layout.split_top && row < layout.split_bottom) {
                continue;
            }
            for (int32_t x = card.x1 + layout.corner; x <= card.x2 - layout.corner; x++) {
                const Rgb c = writer.get(x, y);
                black += c.r == 0 && c.g == 0 && c.b == 0 ? 1 : 0;
            }
        }
    }
    CHECK(black == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "the manager feeds every clock saver the print its host reports",
                 "[screensaver][screensaver_flip_clock]") {
    helix::ScopedGateEnv gate_env;
    auto& mgr = ScreensaverManager::instance();
    helix::ui::SaverHost host;
    host.print_status = [] {
        return helix::ui::ClockPrint{ClockPrintPhase::PRINTING, hms(1, 0, 0), 50};
    };
    mgr.set_host(host);
    mgr.start(ScreensaverType::FLIP_CLOCK);
    CHECK(active_clock_reading<FlipClockScreensaver>(ScreensaverType::FLIP_CLOCK).mode ==
          ClockMode::COUNTDOWN);
    mgr.start(ScreensaverType::AQUA);
    CHECK(active_clock_reading<helix::ui::AquaScreensaver>(ScreensaverType::AQUA).mode ==
          ClockMode::COUNTDOWN);
    mgr.start(ScreensaverType::HIGHFLY);
    CHECK(active_clock_reading<helix::ui::HighFlyScreensaver>(ScreensaverType::HIGHFLY).mode ==
          ClockMode::COUNTDOWN);
    mgr.stop();
    mgr.set_host({});
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a print that ends while no saver runs shows its end when a saver starts",
                 "[screensaver][screensaver_flip_clock]") {
    helix::ScopedGateEnv gate_env;
    auto& mgr = ScreensaverManager::instance();
    uint64_t wall_ns = 0;
    helix::ScopedScriptedCpuClock clock(mgr,
                                        [&wall_ns] { return helix::ui::CpuSample{0, wall_ns}; });
    ClockPrintPhase phase = ClockPrintPhase::PRINTING;
    uint32_t inactive_ms = 0;
    helix::ui::SaverHost host;
    host.print_status = [&phase] { return helix::ui::ClockPrint{phase, -1, 80}; };
    host.inactive_ms = [&inactive_ms] { return inactive_ms; };
    mgr.set_host(host);
    const auto tick_minutes = [&](int minutes) {
        for (int i = 0; i < minutes; i++) {
            wall_ns += 60ULL * 1000 * 1000 * 1000;
            inactive_ms += 60000;
            lv_tick_inc(60000);
            mgr.on_idle_check_tick();
        }
    };

    tick_minutes(2);
    phase = ClockPrintPhase::COMPLETE;
    tick_minutes(10);
    mgr.start(ScreensaverType::FLIP_CLOCK);
    CHECK(active_clock_reading<FlipClockScreensaver>(ScreensaverType::FLIP_CLOCK).mode ==
          ClockMode::DONE);

    // A touch after the end acknowledges it: the next saver shows the clock.
    mgr.stop(helix::SaverRunEnd::TOUCH);
    inactive_ms = 0;
    tick_minutes(1);
    mgr.start(ScreensaverType::FLIP_CLOCK);
    CHECK(active_clock_reading<FlipClockScreensaver>(ScreensaverType::FLIP_CLOCK).mode ==
          ClockMode::CLOCK);
    mgr.stop();
    mgr.set_host({});
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a print's end reaches a running saver at once, with the progress it reached",
                 "[screensaver][screensaver_flip_clock]") {
    helix::ScopedGateEnv gate_env;
    auto& mgr = ScreensaverManager::instance();
    helix::ui::ClockPrint status{ClockPrintPhase::PRINTING, hms(0, 30), 64};
    helix::ui::SaverHost host;
    host.print_status = [&status] { return status; };
    host.inactive_ms = [] { return 3600000u; };
    mgr.set_host(host);
    mgr.start(ScreensaverType::FLIP_CLOCK);
    REQUIRE(active_clock_reading<FlipClockScreensaver>(ScreensaverType::FLIP_CLOCK).mode ==
            ClockMode::COUNTDOWN);

    // The host reports the end, its progress already reset; no idle-check tick has run.
    status = {ClockPrintPhase::CANCELLED, -1, 0};
    const helix::ui::ClockReading r =
        active_clock_reading<FlipClockScreensaver>(ScreensaverType::FLIP_CLOCK, 33);
    CHECK(r.mode == ClockMode::CANCELLED);
    CHECK(r.progress_pct == 64);
    mgr.stop();
    mgr.set_host({});
}

#endif // HELIX_ENABLE_SCREENSAVER
