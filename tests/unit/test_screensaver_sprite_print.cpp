// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_screensaver.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_bounce_test_access.h"
#include "../test_helpers/screensaver_test_access.h"
#include "platform_capabilities.h"
#include "screensaver_bounce.h"
#include "screensaver_print_colors.h"

#include <cstdlib>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

#if HELIX_ENABLE_SCREENSAVER

using helix::BouncingLogoScreensaver;
using helix::BouncingPrinterScreensaver;
using helix::ui::ClockInput;
using helix::ui::ClockPrintPhase;

namespace {

ClockInput print_input(ClockPrintPhase phase, int32_t pct) {
    ClockInput in;
    in.seconds_of_day = 14 * 3600;
    in.print.phase = phase;
    in.print.progress_pct = pct;
    return in;
}

const lv_color32_t& pixel_at(const lv_draw_buf_t& buf, int32_t x, int32_t y) {
    return reinterpret_cast<const lv_color32_t*>(buf.data + y * buf.header.stride)[x];
}

bool is_grey(const lv_color32_t& p, int tolerance = 0) {
    return std::abs(p.red - p.green) <= tolerance && std::abs(p.green - p.blue) <= tolerance;
}

/// Mean luma of the opaque pixels of a 32-bit buffer.
double mean_luma(const lv_draw_buf_t& buf) {
    double sum = 0.0;
    int n = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(buf.header.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(buf.header.w); x++) {
            const lv_color32_t& p = pixel_at(buf, x, y);
            if (p.alpha > 0) {
                sum += (p.red * 77 + p.green * 150 + p.blue * 29) >> 8;
                n++;
            }
        }
    }
    return n > 0 ? sum / n : 0.0;
}

/// Rows above `line` of `fill` are `art` in grey; the rest are `art` itself. Returns how many
/// opaque pixels above the line were coloured in `art`, so a caller knows the check bit.
int check_fill(const lv_draw_buf_t& art, const lv_draw_buf_t& fill, int32_t line) {
    REQUIRE(fill.header.w == art.header.w);
    REQUIRE(fill.header.h == art.header.h);
    int coloured_above = 0;
    int mismatches = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(art.header.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(art.header.w); x++) {
            const lv_color32_t& a = pixel_at(art, x, y);
            const lv_color32_t& f = pixel_at(fill, x, y);
            if (y < line) {
                const uint8_t luma =
                    static_cast<uint8_t>((a.red * 77 + a.green * 150 + a.blue * 29) >> 8);
                const bool ok =
                    f.alpha == a.alpha && f.red == luma && f.green == luma && f.blue == luma;
                mismatches += ok ? 0 : 1;
                coloured_above += (a.alpha > 0 && !is_grey(a)) ? 1 : 0;
            } else {
                const bool ok =
                    f.alpha == a.alpha && f.red == a.red && f.green == a.green && f.blue == a.blue;
                mismatches += ok ? 0 : 1;
            }
        }
    }
    CHECK(mismatches == 0);
    return coloured_above;
}

int32_t fill_line(const lv_draw_buf_t& art, int32_t pct) {
    return static_cast<int32_t>(art.header.h) * (100 - pct) / 100;
}

/// Opaque snapshot pixels in the top `rows_pct` of the drawn sprite: total and coloured.
struct SnapshotTally {
    int opaque = 0;
    int coloured = 0;
};

SnapshotTally snapshot_top(lv_obj_t* sprite, int32_t rows_pct) {
    SnapshotTally tally;
    lv_obj_update_layout(sprite);
    lv_draw_buf_t* snap = lv_snapshot_take(sprite, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    const int32_t rows = static_cast<int32_t>(snap->header.h) * rows_pct / 100;
    for (int32_t y = 0; y < rows; y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(snap->header.w); x++) {
            const lv_color32_t& p = pixel_at(*snap, x, y);
            if (p.alpha == 0) {
                continue;
            }
            tally.opaque++;
            tally.coloured += is_grey(p, 2) ? 0 : 1;
        }
    }
    lv_draw_buf_destroy(snap);
    return tally;
}

std::unique_ptr<BouncingPrinterScreensaver> make_bounce(bool logo) {
    if (logo) {
        return std::make_unique<BouncingLogoScreensaver>();
    }
    return std::make_unique<BouncingPrinterScreensaver>();
}

/// Overlay children that are neither the sprite nor the progress card: confetti bursts.
int extra_children(const BouncingPrinterScreensaver& ss) {
    lv_obj_t* overlay = lv_obj_get_parent(ss.sprite());
    int n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(overlay); i++) {
        lv_obj_t* child = lv_obj_get_child(overlay, static_cast<int32_t>(i));
        if (child != ss.sprite() && child != ss.print_card_obj()) {
            n++;
        }
    }
    return n;
}

} // namespace

// ============================================================================
// Bouncing printer and logo
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "the sprite fills with colour from the bottom as the print goes",
                 "[screensaver][bounce]") {
    const bool logo = GENERATE(false, true);
    CAPTURE(logo);
    auto ss = make_bounce(logo);
    ScreensaverStopOnExit<BouncingPrinterScreensaver> stop_on_exit{*ss};
    ClockInput in = print_input(ClockPrintPhase::PRINTING, 30);
    ss->set_input_source([&in] { return in; });
    ss->start();
    REQUIRE(ss->is_active());

    const lv_draw_buf_t* art = ss->sprite_art();
    const lv_draw_buf_t* fill = ss->fill_art();
    REQUIRE(art != nullptr);
    REQUIRE(fill != nullptr);
    CHECK(lv_image_get_src(ss->sprite()) == fill);
    // Wall tints pause, so the grey reads as grey.
    CHECK(lv_obj_get_style_image_recolor_opa(ss->sprite(), LV_PART_MAIN) == LV_OPA_TRANSP);
    CHECK(check_fill(*art, *fill, fill_line(*art, 30)) > 0);

    // What LVGL draws is the edited buffer: the top of the sprite is grey at 30%.
    const SnapshotTally grey_top = snapshot_top(ss->sprite(), 50);
    REQUIRE(grey_top.opaque > 0);
    CHECK(grey_top.coloured == 0);

    in.print.progress_pct = 60;
    advance_saver(*ss, 33);
    check_fill(*art, *fill, fill_line(*art, 60));

    in.print.progress_pct = 100;
    advance_saver(*ss, 33);
    check_fill(*art, *fill, 0);
    CHECK(snapshot_top(ss->sprite(), 50).coloured > 0);

    // A new print starts grey again.
    in.print.progress_pct = 10;
    advance_saver(*ss, 33);
    check_fill(*art, *fill, fill_line(*art, 10));

    // No print: the art in its own colours, as before any print.
    in = print_input(ClockPrintPhase::NONE, 0);
    advance_saver(*ss, 33);
    CHECK(lv_image_get_src(ss->sprite()) == art);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a finished print throws confetti on every bounce, one burst at a time",
                 "[screensaver][bounce]") {
    const bool logo = GENERATE(false, true);
    CAPTURE(logo);
    auto ss = make_bounce(logo);
    ScreensaverStopOnExit<BouncingPrinterScreensaver> stop_on_exit{*ss};

    SECTION("a burst at a time, and a fresh one at the next bounce once it ends") {
        if (!helix::PlatformCapabilities::detect().supports_animations) {
            SKIP("this board does not animate, so a finished print flashes instead");
        }
        ClockInput in = print_input(ClockPrintPhase::COMPLETE, 100);
        ss->set_input_source([&in] { return in; });
        ss->start();
        REQUIRE(ss->is_active());
        CHECK(lv_image_get_src(ss->sprite()) == ss->sprite_art());
        CHECK(lv_obj_get_style_image_recolor_opa(ss->sprite(), LV_PART_MAIN) == LV_OPA_TRANSP);

        lv_obj_t* first = nullptr;
        int bursts = 0;
        for (int i = 0; i < 30000 / 33; i++) {
            advance_saver(*ss, 33);
            if (ss->confetti() != nullptr && ss->confetti() != first) {
                first = ss->confetti();
                bursts++;
            }
            REQUIRE(extra_children(*ss) <= 1);
        }
        // Its own timer never runs here, so the first burst is still alive.
        CHECK(bursts == 1);
        // The confetti flies behind the progress card.
        lv_obj_t* card = ss->print_card_obj();
        REQUIRE(card != nullptr);
        CHECK(lv_obj_get_index(card) ==
              static_cast<int32_t>(lv_obj_get_child_count(lv_obj_get_parent(card))) - 1);

        for (int round = 0; round < 3; round++) {
            CAPTURE(round);
            lv_obj_delete(ss->confetti());
            REQUIRE(ss->confetti() == nullptr);
            for (int i = 0; i < 15000 / 33 && ss->confetti() == nullptr; i++) {
                advance_saver(*ss, 33);
            }
            CHECK(ss->confetti() != nullptr);
        }
    }

    SECTION("the cheapest rung flashes green on every bounce instead") {
        ClockInput in = print_input(ClockPrintPhase::COMPLETE, 100);
        ss->set_input_source([&in] { return in; });
        ss->set_start_level(ss->level_count() - 1);
        ss->start();
        REQUIRE(ss->is_active());
        lv_obj_t* overlay = lv_obj_get_parent(ss->sprite());
        const lv_color_t flash =
            lv_color_mix(lv_color_make(helix::ui::kPrintDone.r, helix::ui::kPrintDone.g,
                                       helix::ui::kPrintDone.b),
                         lv_color_black(), 56);
        int flashes = 0;
        bool was_flashing = false;
        for (int i = 0; i < 30000 / 33; i++) {
            advance_saver(*ss, 33);
            const bool flashing =
                lv_color_eq(lv_obj_get_style_bg_color(overlay, LV_PART_MAIN), flash);
            flashes += (flashing && !was_flashing) ? 1 : 0;
            was_flashing = flashing;
            REQUIRE(ss->confetti() == nullptr);
        }
        CHECK(flashes >= 3);
    }

    SECTION("a print paused for an hour throws nothing") {
        ClockInput in = print_input(ClockPrintPhase::PAUSED, 40);
        ss->set_input_source([&in] { return in; });
        ss->start();
        REQUIRE(ss->is_active());
        for (int i = 0; i < 3600; i++) {
            advance_saver(*ss, 1000);
        }
        CHECK(ss->confetti() == nullptr);
        CHECK(extra_children(*ss) == 0);
        CHECK(lv_image_get_src(ss->sprite()) == ss->fill_art());
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "with no print, every corner hit throws its own burst, however many are alive",
                 "[screensaver][bounce]") {
    if (!helix::PlatformCapabilities::detect().supports_animations) {
        SKIP("this board does not animate, so a corner flashes instead");
    }
    BouncingPrinterScreensaver ss;
    ScreensaverStopOnExit<BouncingPrinterScreensaver> stop_on_exit{ss};
    ClockInput in = print_input(ClockPrintPhase::NONE, 0);
    ss.set_input_source([&in] { return in; });
    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(extra_children(ss) == 0);
    for (int hit = 1; hit <= 2; hit++) {
        CAPTURE(hit);
        helix::BounceTestAccess::aim_at_corner(ss, 0.5f);
        for (int i = 0; i < 1000 / 33; i++) {
            advance_saver(ss, 33);
        }
        // Its own timer never runs here, so every burst thrown is still alive.
        CHECK(extra_children(ss) == hit);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a failed print tints the sprite red", "[screensaver][bounce]") {
    const bool logo = GENERATE(false, true);
    const ClockPrintPhase end = GENERATE(ClockPrintPhase::FAILED, ClockPrintPhase::CANCELLED);
    CAPTURE(logo, static_cast<int>(end));
    auto ss = make_bounce(logo);
    ScreensaverStopOnExit<BouncingPrinterScreensaver> stop_on_exit{*ss};
    ClockInput in = print_input(ClockPrintPhase::PRINTING, 40);
    ss->set_input_source([&in] { return in; });
    ss->start();
    REQUIRE(ss->is_active());
    advance_saver(*ss, 33);
    in.print.phase = end;

    const lv_color_t red = lv_color_make(helix::ui::kPrintFailed.r, helix::ui::kPrintFailed.g,
                                         helix::ui::kPrintFailed.b);
    // Steady through every wall it meets.
    for (int i = 0; i < 20000 / 33; i++) {
        advance_saver(*ss, 33);
        REQUIRE(lv_color_eq(lv_obj_get_style_image_recolor(ss->sprite(), LV_PART_MAIN), red));
        REQUIRE(lv_obj_get_style_image_recolor_opa(ss->sprite(), LV_PART_MAIN) > LV_OPA_TRANSP);
    }
    // The picture keeps the progress the print reached.
    CHECK(lv_image_get_src(ss->sprite()) == ss->fill_art());
    check_fill(*ss->sprite_art(), *ss->fill_art(), fill_line(*ss->sprite_art(), 40));
    CHECK(ss->confetti() == nullptr);
}

// ============================================================================
// Flying toasters
// ============================================================================

namespace {

/// Toast sprites on the overlay showing `shade`'s art, and those showing any other shade.
struct ToastTally {
    int this_shade = 0;
    int other_shades = 0;
};

ToastTally tally_toasts(const FlyingToasterScreensaver& ss, int shade) {
    ToastTally tally;
    lv_obj_t* overlay = lv_obj_get_parent(ss.popped_toast());
    for (uint32_t i = 0; i < lv_obj_get_child_count(overlay); i++) {
        lv_obj_t* child = lv_obj_get_child(overlay, static_cast<int32_t>(i));
        if (child == ss.popped_toast()) {
            continue;
        }
        const void* src = lv_image_get_src(child);
        for (int s = -1; s <= FlyingToasterScreensaver::kToastBurnt; s++) {
            if (src == ss.toast_art(s)) {
                (s == shade ? tally.this_shade : tally.other_shades)++;
            }
        }
    }
    return tally;
}

bool shows_toast(const FlyingToasterScreensaver& ss, lv_obj_t* obj) {
    const void* src = lv_image_get_src(obj);
    for (int s = -1; s <= FlyingToasterScreensaver::kToastBurnt; s++) {
        if (src == ss.toast_art(s)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "toast browns as the print goes", "[screensaver][toasters]") {
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};
    ClockInput in = print_input(ClockPrintPhase::NONE, 0);
    ss.set_input_source([&in] { return in; });
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(ss.toast_shade() == -1);
    CHECK(tally_toasts(ss, -1).this_shade > 0);
    CHECK(tally_toasts(ss, -1).other_shades == 0);

    // Each shade is darker than the one before it.
    for (int s = 0; s + 1 < FlyingToasterScreensaver::kToastShadeCount; s++) {
        CAPTURE(s);
        REQUIRE(ss.toast_art(s) != nullptr);
        CHECK(mean_luma(*ss.toast_art(s + 1)) < mean_luma(*ss.toast_art(s)));
    }

    int previous = -1;
    for (const int32_t pct : {5, 30, 60, 95}) {
        CAPTURE(pct);
        in = print_input(ClockPrintPhase::PRINTING, pct);
        advance_saver(ss, 33);
        const int shade = ss.toast_shade();
        CHECK(shade == pct * FlyingToasterScreensaver::kToastShadeCount / 100);
        CHECK(shade > previous);
        previous = shade;
        const ToastTally tally = tally_toasts(ss, shade);
        CHECK(tally.this_shade > 0);
        CHECK(tally.other_shades == 0);
    }

    in = print_input(ClockPrintPhase::NONE, 0);
    advance_saver(ss, 33);
    CHECK(ss.toast_shade() == -1);
    CHECK(tally_toasts(ss, -1).other_shades == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "a failed print burns the toast", "[screensaver][toasters]") {
    const ClockPrintPhase end = GENERATE(ClockPrintPhase::FAILED, ClockPrintPhase::CANCELLED);
    CAPTURE(static_cast<int>(end));
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};
    ClockInput in = print_input(ClockPrintPhase::PRINTING, 50);
    ss.set_input_source([&in] { return in; });
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(tally_toasts(ss, ss.toast_shade()).other_shades == 0);
    in.print.phase = end;
    advance_saver(ss, 33);

    CHECK(ss.toast_shade() == FlyingToasterScreensaver::kToastBurnt);
    const ToastTally tally = tally_toasts(ss, FlyingToasterScreensaver::kToastBurnt);
    CHECK(tally.this_shade > 0);
    CHECK(tally.other_shades == 0);
    CHECK(mean_luma(*ss.toast_art(FlyingToasterScreensaver::kToastBurnt)) <
          mean_luma(*ss.toast_art(FlyingToasterScreensaver::kToastShadeCount - 1)) / 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "a finished print pops toast", "[screensaver][toasters]") {
    const bool cheapest = GENERATE(false, true);
    CAPTURE(cheapest);
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};
    // Started after the print ended: the end state shows at once.
    ClockInput in = print_input(ClockPrintPhase::COMPLETE, 100);
    ss.set_input_source([&in] { return in; });
    if (cheapest) {
        ss.set_start_level(ss.level_count() - 1);
    }
    ss.start();
    REQUIRE(ss.is_active());
    lv_obj_t* pop = ss.popped_toast();
    REQUIRE(pop != nullptr);
    CHECK(shows_toast(ss, pop));

    std::vector<uint32_t> pop_starts;
    bool was_shown = false;
    for (uint32_t t = 33; t <= 9000; t += 33) {
        advance_saver(ss, 33);
        const bool shown = !lv_obj_has_flag(pop, LV_OBJ_FLAG_HIDDEN);
        if (shown && !was_shown) {
            pop_starts.push_back(t);
        }
        was_shown = shown;
        if (!shown) {
            continue;
        }
        // It rises out of a toaster that is on screen with it.
        lv_obj_t* overlay = lv_obj_get_parent(pop);
        bool from_toaster = false;
        for (uint32_t i = 0; i < lv_obj_get_child_count(overlay); i++) {
            lv_obj_t* child = lv_obj_get_child(overlay, static_cast<int32_t>(i));
            if (child == pop || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) ||
                shows_toast(ss, child) || child == ss.print_card_obj()) {
                continue;
            }
            from_toaster |=
                lv_obj_get_style_x(child, LV_PART_MAIN) == lv_obj_get_style_x(pop, LV_PART_MAIN) &&
                lv_obj_get_style_y(child, LV_PART_MAIN) > lv_obj_get_style_y(pop, LV_PART_MAIN);
        }
        REQUIRE(from_toaster);
    }
    REQUIRE(pop_starts.size() >= 2);
    for (size_t i = 1; i < pop_starts.size(); i++) {
        CHECK(pop_starts[i] - pop_starts[i - 1] >= 3000);
    }

    // No print, no pop.
    in = print_input(ClockPrintPhase::NONE, 0);
    for (int i = 0; i < 4000 / 33; i++) {
        advance_saver(ss, 33);
        REQUIRE(lv_obj_has_flag(pop, LV_OBJ_FLAG_HIDDEN));
    }
}

// ============================================================================
// Restarting on a finished print
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a bouncing saver restarted on a finished print, its old overlay not yet deleted, "
                 "runs on",
                 "[screensaver][bounce]") {
    if (!helix::PlatformCapabilities::detect().supports_animations) {
        SKIP("this board does not animate, so a finished print throws no confetti");
    }
    const bool logo = GENERATE(false, true);
    CAPTURE(logo);
    auto ss = make_bounce(logo);
    ScreensaverStopOnExit<BouncingPrinterScreensaver> stop_on_exit{*ss};
    ClockInput in = print_input(ClockPrintPhase::COMPLETE, 100);
    ss->set_input_source([&in] { return in; });
    ss->start();
    REQUIRE(ss->is_active());
    for (int i = 0; i < 30000 / 33 && ss->confetti() == nullptr; i++) {
        advance_saver(*ss, 33);
    }
    REQUIRE(ss->confetti() != nullptr);

    // No lv_timer_handler between: the old overlay and its burst still await deletion.
    ss->stop();
    ss->start();
    REQUIRE(ss->is_active());
    CHECK(ss->confetti() == nullptr);
    for (int i = 0; i < 30000 / 33 && ss->confetti() == nullptr; i++) {
        advance_saver(*ss, 33);
    }
    REQUIRE(ss->confetti() != nullptr);
    CHECK(lv_obj_get_parent(ss->confetti()) == lv_obj_get_parent(ss->sprite()));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the toasters restarted on a finished print, their old overlay not yet deleted, "
                 "pop on",
                 "[screensaver][toasters]") {
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};
    ClockInput in = print_input(ClockPrintPhase::COMPLETE, 100);
    ss.set_input_source([&in] { return in; });
    ss.start();
    REQUIRE(ss.is_active());
    lv_obj_t* first = ss.popped_toast();
    REQUIRE(first != nullptr);
    bool popped = false;
    for (int i = 0; i < 6000 / 33 && !popped; i++) {
        advance_saver(ss, 33);
        popped = !lv_obj_has_flag(first, LV_OBJ_FLAG_HIDDEN);
    }
    REQUIRE(popped);

    // No lv_timer_handler between: the old overlay and its toast still await deletion.
    ss.stop();
    ss.start();
    REQUIRE(ss.is_active());
    lv_obj_t* pop = ss.popped_toast();
    REQUIRE(pop != nullptr);
    popped = false;
    for (int i = 0; i < 6000 / 33 && !popped; i++) {
        advance_saver(ss, 33);
        popped = !lv_obj_has_flag(pop, LV_OBJ_FLAG_HIDDEN);
    }
    CHECK(popped);
}

#endif // HELIX_ENABLE_SCREENSAVER
