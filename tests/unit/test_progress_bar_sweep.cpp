// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_progress_bar_sweep.cpp
 * @brief The estimate bar's highlight crosses the unfilled span left to right,
 *        and its animation runs only while there is a visible span to sweep.
 *
 * Run with: ./build/bin/helix-tests "[progress_bar]"
 */

#include "ui_progress_bar.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using helix::ui::kProgressSweepPeriodMs;
using helix::ui::progress_sweep_band;

TEST_CASE("the sweep band crosses the span left to right, then starts over", "[progress_bar]") {
    const int32_t x1 = 100;
    const int32_t w = 300;

    const auto start = progress_sweep_band(x1, w, 0);
    CHECK(start.x1 == x1);
    CHECK(start.x2 - start.x1 + 1 == w / 3);

    const auto last = progress_sweep_band(x1, w, kProgressSweepPeriodMs - 1);
    CHECK(last.x2 >= x1 + w - 2);
    CHECK(last.x2 <= x1 + w - 1);

    // One period later it is back at the left, never travelling back.
    CHECK(progress_sweep_band(x1, w, kProgressSweepPeriodMs).x1 == start.x1);
    int32_t prev = start.x1;
    for (uint32_t t = 50; t < kProgressSweepPeriodMs; t += 50) {
        const auto band = progress_sweep_band(x1, w, t);
        CHECK(band.x1 >= prev);
        CHECK(band.x1 >= x1);
        CHECK(band.x2 <= x1 + w - 1);
        prev = band.x1;
    }
}

TEST_CASE("the sweep eases in and out and fades at the ends of a pass", "[progress_bar]") {
    const int32_t w = 300;
    const uint32_t tenth = kProgressSweepPeriodMs / 10;
    auto x_at = [&](uint32_t t) { return progress_sweep_band(0, w, t).x1; };

    // Slower at the ends than through the middle.
    const int32_t first = x_at(tenth) - x_at(0);
    const int32_t middle =
        x_at(kProgressSweepPeriodMs / 2 + tenth / 2) - x_at(kProgressSweepPeriodMs / 2 - tenth / 2);
    const int32_t final = x_at(kProgressSweepPeriodMs - 1) - x_at(kProgressSweepPeriodMs - tenth);
    CHECK(first < middle);
    CHECK(final < middle);

    // Invisible as a pass starts, strongest midway, nearly gone as it ends, so
    // starting over at the left never shows as a jump.
    CHECK(progress_sweep_band(0, w, 0).strength == 0);
    CHECK(progress_sweep_band(0, w, kProgressSweepPeriodMs / 2).strength == 255);
    CHECK(progress_sweep_band(0, w, kProgressSweepPeriodMs - 1).strength < 5);
}

TEST_CASE("a span too narrow for the band carries none", "[progress_bar]") {
    const auto none = progress_sweep_band(50, 7, kProgressSweepPeriodMs / 2);
    CHECK(none.x2 < none.x1);
    const auto narrow = progress_sweep_band(0, 12, 0);
    CHECK(narrow.x2 - narrow.x1 + 1 == 8);
}

TEST_CASE_METHOD(LVGLUITestFixture, "only a visible estimate bar keeps an animation running",
                 "[progress_bar]") {
    const char* attrs[] = {"width", "200", nullptr};
    auto* estimate =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "progress_bar_estimate", attrs));
    auto* plain = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "progress_bar", attrs));
    REQUIRE(estimate != nullptr);
    REQUIRE(plain != nullptr);

    lv_refr_now(nullptr);
    // The sweep is keyed on the bar, so only the estimate bar owns one.
    CHECK(lv_anim_get(estimate, nullptr) != nullptr);
    CHECK(lv_anim_get(plain, nullptr) == nullptr);

    // At a non-zero estimate it still sweeps: the value may sit still.
    lv_bar_set_value(estimate, 40, LV_ANIM_OFF);
    lv_refr_now(nullptr);
    CHECK(lv_anim_get(estimate, nullptr) != nullptr);

    lv_obj_set_hidden(estimate, true);
    process_lvgl(100);
    CHECK(lv_anim_get(estimate, nullptr) == nullptr);

    lv_obj_set_hidden(estimate, false);
    lv_refr_now(nullptr);
    CHECK(lv_anim_get(estimate, nullptr) != nullptr);

    // A full bar has no unfilled span left to sweep.
    lv_bar_set_value(estimate, 100, LV_ANIM_OFF);
    process_lvgl(100);
    CHECK(lv_anim_get(estimate, nullptr) == nullptr);
    lv_refr_now(nullptr);
    CHECK(lv_anim_get(estimate, nullptr) == nullptr);
}
