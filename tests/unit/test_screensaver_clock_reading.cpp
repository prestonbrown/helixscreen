// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_clock_scopes.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "display_settings_manager.h"
#include "locale_formats.h"
#include "screensaver_clock.h"
#include "screensaver_clock_reading.h"
#include "screensaver_stroke_digits.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::DisplaySettingsManager;
using helix::TimeFormat;
using helix::ui::clock_mode_is_end;
using helix::ui::clock_time;
using helix::ui::ClockDate;
using helix::ui::ClockInput;
using helix::ui::ClockMeridiem;
using helix::ui::ClockMode;
using helix::ui::ClockPrintPhase;
using helix::ui::ClockState;
using helix::ui::ClockTime;
using helix::ui::StrokeDigitSet;

using helix::test::hms;
using helix::test::LanguageScope;
using helix::test::TestFrame;
using helix::test::TimeFormatScope;

namespace {

uint64_t total_coverage(const StrokeDigitSet& set, int digit) {
    uint64_t sum = 0;
    for (int32_t y = 0; y < set.h; y++) {
        for (int32_t x = 0; x < set.w; x++) {
            sum += set.at(digit, x, y);
        }
    }
    return sum;
}

} // namespace

// ============================================================================
// Time and readings
// ============================================================================

TEST_CASE("clock_time shows 12-hour and 24-hour time with seconds and the marker",
          "[screensaver][clock_reading]") {
    const ClockTime evening = clock_time(hms(21, 28, 7), false);
    CHECK(evening.hour == 9);
    CHECK(evening.hour_of_day == 21);
    CHECK(evening.minute == 28);
    CHECK(evening.second == 7);
    CHECK(evening.meridiem == ClockMeridiem::PM);

    const ClockTime midnight = clock_time(0, false);
    CHECK(midnight.hour == 12);
    CHECK(midnight.meridiem == ClockMeridiem::AM);

    const ClockTime noon = clock_time(hms(12, 0), false);
    CHECK(noon.hour == 12);
    CHECK(noon.meridiem == ClockMeridiem::PM);

    const ClockTime morning = clock_time(hms(9, 0), true);
    CHECK(morning.hour == 9);
    CHECK(morning.meridiem == ClockMeridiem::NONE);

    const ClockTime wrapped = clock_time(-1, true);
    CHECK(wrapped.hour == 23);
    CHECK(wrapped.minute == 59);
    CHECK(wrapped.second == 59);
}

TEST_CASE("a reading carries the date it was given", "[screensaver][clock_reading]") {
    ClockInput in;
    in.date = ClockDate{2019, 10, 21, 1};
    ClockState state;
    state.step(0, in);
    const ClockDate& date = state.reading().date;
    CHECK(date.year == 2019);
    CHECK(date.month == 10);
    CHECK(date.day == 21);
    CHECK(date.weekday == 1);
}

TEST_CASE("a reading carries whether the clock spells print states, in every mode",
          "[screensaver][clock_reading]") {
    ClockInput in;
    CHECK(in.spell_states);
    for (const ClockPrintPhase phase :
         {ClockPrintPhase::NONE, ClockPrintPhase::PRINTING, ClockPrintPhase::PAUSED,
          ClockPrintPhase::COMPLETE, ClockPrintPhase::FAILED, ClockPrintPhase::CANCELLED}) {
        CAPTURE(static_cast<int>(phase));
        for (const bool spell : {true, false}) {
            in.print.phase = phase;
            in.print.remaining_s = 600;
            in.spell_states = spell;
            ClockState state;
            state.step(16, in);
            CHECK(state.reading().spell_states == spell);
        }
    }
}

TEST_CASE("a print with an estimate reads a countdown with its end time and progress",
          "[screensaver][clock_reading]") {
    ClockInput in;
    in.seconds_of_day = hms(21, 28, 0);
    in.print.phase = ClockPrintPhase::PRINTING;
    in.print.remaining_s = 4320;
    in.print.progress_pct = 37;
    ClockState state;
    state.step(0, in);
    CHECK(state.mode() == ClockMode::COUNTDOWN);
    CHECK(state.reading().countdown_s == 4320);
    CHECK(state.reading().eta_minute_of_day == 1360);
    CHECK(state.reading().progress_pct == 37);
    CHECK(state.reading().time.hour == 9);
    CHECK(state.reading().time.minute == 28);
    CHECK(state.reading().time.meridiem == ClockMeridiem::PM);

    state.step(1000, in);
    CHECK(state.countdown_s() == 4319);

    // Estimator jitter within the resync window is ignored.
    in.print.remaining_s = 4319 + 5;
    state.step(0, in);
    CHECK(state.countdown_s() == 4319);

    in.print.remaining_s = 4000;
    state.step(0, in);
    CHECK(state.countdown_s() == 4000);
    CHECK(state.reading().countdown_s == 4000);
}

TEST_CASE("paused freezes the countdown and a print gone idle reads the clock",
          "[screensaver][clock_reading]") {
    ClockInput in;
    in.seconds_of_day = hms(10, 0);
    in.print.phase = ClockPrintPhase::PRINTING;
    in.print.remaining_s = 600;
    in.print.progress_pct = 42;
    ClockState state;
    state.step(0, in);
    REQUIRE(state.mode() == ClockMode::COUNTDOWN);

    in.print.phase = ClockPrintPhase::PAUSED;
    state.step(0, in);
    state.step(5000, in);
    CHECK(state.mode() == ClockMode::PAUSED);
    CHECK(state.countdown_s() == 600);
    CHECK(state.reading().progress_pct == 42);

    in.print.phase = ClockPrintPhase::NONE;
    in.print.remaining_s = -1;
    state.step(16, in);
    CHECK(state.mode() == ClockMode::CLOCK);
    CHECK(state.reading().countdown_s == 0);
    CHECK(state.reading().progress_pct == 0);
}

TEST_CASE("a running print with no estimate reads its progress", "[screensaver][clock_reading]") {
    ClockInput in;
    in.seconds_of_day = hms(10, 0);
    in.print.phase = ClockPrintPhase::PRINTING;
    in.print.remaining_s = -1;
    in.print.progress_pct = 37;
    ClockState state;
    state.step(0, in);
    CHECK(state.mode() == ClockMode::PROGRESS);
    CHECK(state.reading().progress_pct == 37);
    CHECK_FALSE(clock_mode_is_end(state.mode()));

    in.print.remaining_s = 600;
    state.step(16, in);
    CHECK(state.mode() == ClockMode::COUNTDOWN);
    CHECK(state.reading().progress_pct == 37);
}

TEST_CASE("an ended print reads its end for as long as the input says so",
          "[screensaver][clock_reading]") {
    const auto run_to_end = [](ClockPrintPhase end) {
        ClockInput in;
        in.seconds_of_day = hms(10, 0);
        in.print.phase = ClockPrintPhase::PRINTING;
        in.print.remaining_s = 600;
        in.print.progress_pct = 64;
        ClockState state;
        state.step(0, in);
        in.print.phase = end;
        in.print.remaining_s = 0;
        for (int i = 0; i < 600; i++) {
            state.step(1000, in);
        }
        return state;
    };

    const ClockState done = run_to_end(ClockPrintPhase::COMPLETE);
    CHECK(done.mode() == ClockMode::DONE);
    CHECK(done.reading().progress_pct == 100);
    CHECK(clock_mode_is_end(done.mode()));

    const ClockState failed = run_to_end(ClockPrintPhase::FAILED);
    CHECK(failed.mode() == ClockMode::FAILED);
    CHECK(failed.reading().progress_pct == 64);
    CHECK(clock_mode_is_end(failed.mode()));

    const ClockState cancelled = run_to_end(ClockPrintPhase::CANCELLED);
    CHECK(cancelled.mode() == ClockMode::CANCELLED);
    CHECK(cancelled.reading().progress_pct == 64);
    CHECK(clock_mode_is_end(cancelled.mode()));

    ClockState after = done;
    ClockInput idle;
    idle.seconds_of_day = hms(10, 30);
    after.step(16, idle);
    CHECK(after.mode() == ClockMode::CLOCK);
}

TEST_CASE("an end seen without a running print still reads as that end",
          "[screensaver][clock_reading]") {
    ClockInput in;
    in.seconds_of_day = hms(10, 0);
    in.print.phase = ClockPrintPhase::COMPLETE;
    in.print.progress_pct = 100;
    ClockState state;
    state.step(0, in);
    CHECK(state.mode() == ClockMode::DONE);
}

TEST_CASE("the drift range keeps a spare pixel and at most a 25th of the screen",
          "[screensaver][clock_reading]") {
    int32_t dx = 0;
    int32_t dy = 0;
    // A small box in a big screen: capped at a 25th.
    helix::ui::clock_drift_range({300, 200, 499, 279}, 800, 480, dx, dy);
    CHECK(dx == 32);
    CHECK(dy == 19);
    // A box near the edges: the room less a pixel.
    helix::ui::clock_drift_range({10, 6, 789, 470}, 800, 480, dx, dy);
    CHECK(dx == 9);
    CHECK(dy == 5);
    helix::ui::clock_drift_range({0, 0, 799, 479}, 800, 480, dx, dy);
    CHECK(dx == 0);
    CHECK(dy == 0);
}

// ============================================================================
// Stroke digits
// ============================================================================

TEST_CASE("stroke digits size their box from the stroke", "[screensaver][clock_reading]") {
    StrokeDigitSet bold;
    helix::ui::build_stroke_digits(50, 0.2f, bold);
    CHECK(bold.w == 60);
    CHECK(bold.h == 90);
    StrokeDigitSet thin;
    helix::ui::build_stroke_digits(50, 0.06f, thin);
    CHECK(thin.w == 53);
    CHECK(thin.h == 83);
}

TEST_CASE("a thin stroke covers fewer pixels than a bold one", "[screensaver][clock_reading]") {
    StrokeDigitSet bold;
    helix::ui::build_stroke_digits(50, 0.2f, bold);
    StrokeDigitSet thin;
    helix::ui::build_stroke_digits(50, 0.06f, thin);
    for (int digit = 0; digit < 10; digit++) {
        CAPTURE(digit);
        const uint64_t t = total_coverage(thin, digit);
        CHECK(t > 0);
        CHECK(t * 2 < total_coverage(bold, digit));
    }
}

TEST_CASE("a stroke digit's centre line is fully covered", "[screensaver][clock_reading]") {
    StrokeDigitSet thin;
    helix::ui::build_stroke_digits(50, 0.06f, thin);
    // Digit one's upright runs at x = 0.55 path units, shifted by half a stroke into the box.
    const auto x = static_cast<int32_t>((0.55f + 0.03f) * 50.0f);
    CHECK(thin.at(1, x, thin.h / 2) >= 250);
    CHECK(thin.at(1, 2, thin.h / 2) == 0);
}

TEST_CASE("nine is six turned half a turn", "[screensaver][clock_reading]") {
    for (int i = 0; i <= 10; i++) {
        for (int j = 0; j <= 16; j++) {
            const float x = static_cast<float>(i) / 10.0f;
            const float y = static_cast<float>(j) / 10.0f;
            CHECK(helix::ui::stroke_digit_distance(9, x, y) ==
                  helix::ui::stroke_digit_distance(6, 1.0f - x, 1.6f - y));
        }
    }
    CHECK(helix::ui::stroke_digit_distance(9, 0.5f, 0.5f) !=
          helix::ui::stroke_digit_distance(6, 0.5f, 0.5f));
}

TEST_CASE("two_digits splits a clock number into its tens and units",
          "[screensaver][clock_reading]") {
    CHECK(helix::ui::two_digits(0) == std::array<int, 2>{0, 0});
    CHECK(helix::ui::two_digits(7) == std::array<int, 2>{0, 7});
    CHECK(helix::ui::two_digits(42) == std::array<int, 2>{4, 2});
    CHECK(helix::ui::two_digits(59) == std::array<int, 2>{5, 9});
}

TEST_CASE("a blitted stroke digit blends its coverage over the frame and clips at the edges",
          "[screensaver][clock_reading]") {
    StrokeDigitSet set;
    helix::ui::build_stroke_digits(20, 0.2f, set);
    const helix::ui::Rgb white{255, 255, 255};
    TestFrame frame(static_cast<uint32_t>(set.w + 8), static_cast<uint32_t>(set.h + 8),
                    helix::ui::PixelFormat::XRGB8888);
    helix::ui::PixelWriter writer(frame.target);
    helix::ui::blit_stroke_digit(writer, set, 4, 4, 4, white, 1.0f);
    size_t full = 0;
    for (int32_t gy = 0; gy < set.h; gy++) {
        for (int32_t gx = 0; gx < set.w; gx++) {
            const uint8_t cov = set.at(4, gx, gy);
            const helix::ui::Rgb c = frame.at(gx + 4, gy + 4);
            CAPTURE(gx, gy, static_cast<int>(cov));
            CHECK(c.g == cov);
            full += cov == 255 ? 1 : 0;
        }
    }
    CHECK(full > 0);
    CHECK(frame.at(1, 1) == helix::ui::Rgb{});

    // Half opacity lands half way; a digit hanging off every edge writes only the frame.
    TestFrame half(static_cast<uint32_t>(set.w), static_cast<uint32_t>(set.h),
                   helix::ui::PixelFormat::XRGB8888);
    helix::ui::PixelWriter half_writer(half.target);
    helix::ui::blit_stroke_digit(half_writer, set, 8, 0, 0, white, 0.5f);
    helix::ui::blit_stroke_digit(half_writer, set, 8, -set.w / 2, -set.h / 2, white, 1.0f);
    helix::ui::blit_stroke_digit(half_writer, set, 8, set.w / 2, set.h / 2, white, 1.0f);
    CHECK(half.padding_intact());
    for (int32_t gy = 0; gy < set.h; gy++) {
        for (int32_t gx = 0; gx < set.w; gx++) {
            if (set.at(8, gx, gy) == 255 && set.at(8, gx + set.w / 2, gy + set.h / 2) == 0 &&
                set.at(8, gx - set.w / 2, gy - set.h / 2) == 0) {
                CHECK(half.at(gx, gy).g == 128);
            }
        }
    }
}

TEST_CASE("a digit's halo spreads past its strokes and fades out", "[screensaver][clock_reading]") {
    StrokeDigitSet thin;
    helix::ui::build_stroke_digits(50, 0.06f, thin);
    StrokeDigitSet halo;
    helix::ui::build_stroke_halo(thin, 5, halo);
    CHECK(halo.w == thin.w + 10);
    CHECK(halo.h == thin.h + 10);
    // Digit one's upright: covered under the stroke, still shaded a few px beside it, clear
    // well away from it.
    const auto x = static_cast<int32_t>((0.55f + 0.03f) * 50.0f) + 5;
    const int32_t y = halo.h / 2;
    CHECK(halo.at(1, x, y) > 120);
    CHECK(halo.at(1, x + 4, y) > 30);
    CHECK(halo.at(1, x + 4, y) < halo.at(1, x, y));
    CHECK(halo.at(1, x + 12, y) == 0);
    CHECK(halo.at(1, 0, y) == 0);
}

TEST_CASE("clear frees the coverage", "[screensaver][clock_reading]") {
    StrokeDigitSet set;
    helix::ui::build_stroke_digits(40, 0.1f, set);
    REQUIRE(set.heap_bytes() > 0);
    set.clear();
    CHECK(set.heap_bytes() == 0);
    CHECK(set.at(3, 0, 0) == 0);
}

TEST_CASE("every stroke glyph has a centre line and only its own strokes",
          "[screensaver][clock_reading]") {
    using helix::ui::kStrokeGlyphChars;
    using helix::ui::stroke_glyph_distance;
    for (const char* c = kStrokeGlyphChars; *c; ++c) {
        CAPTURE(*c);
        // Some point of the box lies on the centre line, and the box is not all line.
        float nearest = 1e9f;
        float farthest = 0.0f;
        for (int yi = 0; yi <= 32; yi++) {
            for (int xi = 0; xi <= 20; xi++) {
                const float d = stroke_glyph_distance(*c, xi / 20.0f,
                                                      helix::ui::kStrokeDigitPathH * yi / 32.0f);
                nearest = std::min(nearest, d);
                farthest = std::max(farthest, d);
            }
        }
        CHECK(nearest < 0.03f);
        CHECK(farthest > 0.2f);
    }
    // Letters whose right side is straight run its full straight length, as O's does.
    for (const char c : {'D', 'O', 'N', 'U'}) {
        CAPTURE(c);
        for (const float y : {0.7f, 0.8f, 0.9f}) {
            CAPTURE(y);
            CHECK(stroke_glyph_distance(c, 1.0f, y) < 0.01f);
        }
    }

    // The print-state symbols are glyphs too, each centred in the box so one card reads alone.
    for (const char c : {helix::ui::kGlyphCheck, helix::ui::kGlyphCross, helix::ui::kGlyphSquare,
                         helix::ui::kGlyphPause}) {
        CAPTURE(static_cast<int>(c));
        CHECK(std::strchr(kStrokeGlyphChars, c) != nullptr);
        float left = 1e9f;
        float right = -1e9f;
        float top = 1e9f;
        float bottom = -1e9f;
        for (int yi = 0; yi <= 160; yi++) {
            for (int xi = 0; xi <= 100; xi++) {
                const float x = xi / 100.0f;
                const float y = yi / 100.0f;
                if (stroke_glyph_distance(c, x, y) < 0.01f) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        REQUIRE(right > left);
        CHECK(std::fabs((left + right) / 2.0f - 0.5f) < 0.05f);
        CHECK(std::fabs((top + bottom) / 2.0f - helix::ui::kStrokeDigitPathH / 2.0f) < 0.05f);
        CHECK(right - left > 0.6f);
    }

    // Digits are the same glyphs as stroke_digit_distance draws.
    CHECK(stroke_glyph_distance('6', 0.3f, 0.7f) ==
          helix::ui::stroke_digit_distance(6, 0.3f, 0.7f));
    CHECK(stroke_glyph_distance('?', 0.5f, 0.5f) > 100.0f);
}

TEST_CASE("letters that share strokes still draw differently", "[screensaver][clock_reading]") {
    helix::ui::StrokeGlyphSet set;
    set.init(30, 0.2f);
    const auto bytes = [&set](char c) {
        set.ensure(c);
        std::vector<uint8_t> out;
        for (int32_t y = 0; y < set.h; y++) {
            for (int32_t x = 0; x < set.w; x++) {
                out.push_back(set.at(c, x, y));
            }
        }
        return out;
    };
    const std::pair<char, char> pairs[] = {{'O', 'D'}, {'E', 'F'}, {'P', 'D'}, {'I', 'L'},
                                           {'S', '5'}, {'O', '0'}, {'U', 'O'}, {'C', 'O'}};
    for (const auto& [a, b] : pairs) {
        CAPTURE(a, b);
        CHECK(bytes(a) != bytes(b));
    }
}

TEST_CASE("a glyph set holds only the glyphs it was asked for", "[screensaver][clock_reading]") {
    helix::ui::StrokeGlyphSet set;
    set.init(30, 0.2f);
    CHECK(set.heap_bytes() == 0);
    CHECK(set.at('D', set.w / 2, set.h / 2) == 0);
    set.ensure('D');
    const size_t one = set.heap_bytes();
    CHECK(one > 0);
    set.ensure('D');
    CHECK(set.heap_bytes() == one);
    set.ensure('%');
    CHECK(set.heap_bytes() > one);
    // The left upright of D is fully covered.
    CHECK(set.at('D', static_cast<int32_t>(0.1f * 30.0f), set.h / 2) >= 250);
    set.clear();
    CHECK(set.heap_bytes() == 0);
}

// ============================================================================
// Text
// ============================================================================

namespace {

/// The weekday in the active language. With the system locale active the name
/// is whatever this host's libc spells (macOS capitalises French, glibc does
/// not), so the expectation is libc's own; with the built-in tables it is the
/// pinned spelling.
std::string expected_weekday(const struct tm& day, const char* table_name) {
    if (!helix::ui::locale_uses_system_locale())
        return table_name;
    char buf[64];
    strftime(buf, sizeof(buf), "%A", &day);
    return buf;
}

struct tm october_21_2019() {
    struct tm t {};
    t.tm_year = 2019 - 1900;
    t.tm_mon = 9;
    t.tm_mday = 21;
    t.tm_wday = 1;
    return t;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the long date and weekday read in the active language",
                 "[screensaver][clock_reading][locale]") {
    const struct tm day = october_21_2019();
    {
        LanguageScope lang("en");
        CHECK(helix::ui::format_localized_long_date(&day) == "October 21, 2019");
        CHECK(helix::ui::format_localized_weekday(&day) == "Monday");
    }
    {
        LanguageScope lang("de");
        CHECK(helix::ui::format_localized_long_date(&day) == "21. Oktober 2019");
        CHECK(helix::ui::format_localized_weekday(&day) == expected_weekday(day, "Montag"));
    }
    {
        LanguageScope lang("fr");
        CHECK(helix::ui::format_localized_long_date(&day) == "21 octobre 2019");
        CHECK(helix::ui::format_localized_weekday(&day) == expected_weekday(day, "lundi"));
    }
    {
        LanguageScope lang("es");
        CHECK(helix::ui::format_localized_long_date(&day) == "21 de octubre de 2019");
        CHECK(helix::ui::format_localized_weekday(&day) == expected_weekday(day, "lunes"));
    }
    {
        LanguageScope lang("ja");
        CHECK(helix::ui::format_localized_long_date(&day) == "2019年10月21日");
        CHECK(helix::ui::format_localized_weekday(&day) == expected_weekday(day, "月曜日"));
    }
    {
        LanguageScope lang("ru");
        CHECK(helix::ui::format_localized_long_date(&day) == "21 октября 2019");
    }
    {
        LanguageScope lang("xx");
        CHECK(helix::ui::format_localized_long_date(&day) == "October 21, 2019");
    }
    CHECK(helix::ui::format_localized_weekday(nullptr).empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "the print readout says when it ends and how long is left",
                 "[screensaver][clock_reading]") {
    LanguageScope lang("en");
    TimeFormatScope format(TimeFormat::HOUR_12);
    helix::ui::ClockReading reading;
    reading.mode = ClockMode::COUNTDOWN;
    reading.eta_minute_of_day = 1360;
    reading.countdown_s = 4320;
    helix::ui::ClockPrintText text = helix::ui::clock_print_text(reading);
    CHECK(text.eta == "ETA 10:40 PM");
    CHECK(text.left == "1h 12m left");

    reading.mode = ClockMode::PAUSED;
    reading.has_countdown = true;
    text = helix::ui::clock_print_text(reading);
    CHECK(text.eta == "PAUSED");
    CHECK(text.left == "1h 12m left");
    reading.has_countdown = false;
    text = helix::ui::clock_print_text(reading);
    CHECK(text.left.empty());

    reading.mode = ClockMode::DONE;
    text = helix::ui::clock_print_text(reading);
    CHECK(text.eta == "DONE");
    CHECK(text.left.empty());

    reading.mode = ClockMode::FAILED;
    CHECK(helix::ui::clock_print_text(reading).eta == "FAILED");
    reading.mode = ClockMode::CANCELLED;
    CHECK(helix::ui::clock_print_text(reading).eta == "CANCELED");
    reading.mode = ClockMode::PROGRESS;
    reading.progress_pct = 37;
    text = helix::ui::clock_print_text(reading);
    CHECK(text.eta == "37%");
    CHECK(text.left.empty());

    reading.mode = ClockMode::CLOCK;
    text = helix::ui::clock_print_text(reading);
    CHECK(text.eta.empty());
    CHECK(text.left.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "the date text joins nothing and uses locale_formats",
                 "[screensaver][clock_reading]") {
    LanguageScope lang("en");
    const helix::ui::ClockDateText text = helix::ui::clock_date_text(ClockDate{2019, 10, 21, 1});
    CHECK(text.date == "October 21, 2019");
    CHECK(text.weekday == "Monday");
}

TEST_CASE_METHOD(LVGLTestFixture, "the meridiem reads AM or PM and nothing in 24-hour time",
                 "[screensaver][clock_reading]") {
    LanguageScope lang("en");
    CHECK(helix::ui::clock_meridiem_text(ClockMeridiem::AM) == "AM");
    CHECK(helix::ui::clock_meridiem_text(ClockMeridiem::PM) == "PM");
    CHECK(helix::ui::clock_meridiem_text(ClockMeridiem::NONE).empty());
}

#endif // HELIX_ENABLE_SCREENSAVER
