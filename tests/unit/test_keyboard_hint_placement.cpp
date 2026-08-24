// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_keyboard_hint_placement.cpp
 * @brief Containment tests for on-screen keyboard alternate-character hints
 *
 * The keyboard draws a small glyph in each key's top-right corner showing the
 * character you get by long-pressing it. Key sizes vary enormously — a 2-unit
 * "?123" next to a 12-unit spacebar, across breakpoints from micro panels to
 * 1024px displays — so the hint must be placed against the key's real rect and
 * must never spill onto a neighbouring key.
 *
 * These call the real KeyboardManager::compute_hint_area. It is static and takes
 * plain geometry, so no keyboard widget or LVGL display is constructed.
 */

#include "ui_keyboard_manager.h"

#include "lvgl.h"

#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

lv_area_t make_area(int32_t x1, int32_t y1, int32_t w, int32_t h) {
    // LVGL areas are inclusive on both edges.
    lv_area_t a;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x1 + w - 1;
    a.y2 = y1 + h - 1;
    return a;
}

bool contains(const lv_area_t& outer, const lv_area_t& inner) {
    return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 &&
           inner.y2 <= outer.y2;
}

} // namespace

TEST_CASE("Keyboard hint: sits inside a normal key", "[ui][keyboard_hint]") {
    lv_area_t btn = make_area(100, 200, 64, 48);
    lv_area_t hint{};

    REQUIRE(KeyboardManager::compute_hint_area(btn, 9, 10, &hint));
    REQUIRE(contains(btn, hint));

    // Anchored top-right, not centred or top-left.
    REQUIRE(hint.x2 < btn.x2);
    REQUIRE(hint.x1 > btn.x1 + lv_area_get_width(&btn) / 2);
    REQUIRE(hint.y1 < btn.y1 + lv_area_get_height(&btn) / 2);
}

TEST_CASE("Keyboard hint: requested glyph size is preserved", "[ui][keyboard_hint]") {
    lv_area_t btn = make_area(0, 0, 80, 60);
    lv_area_t hint{};

    REQUIRE(KeyboardManager::compute_hint_area(btn, 11, 14, &hint));
    REQUIRE(lv_area_get_width(&hint) == 11);
    REQUIRE(lv_area_get_height(&hint) == 14);
}

TEST_CASE("Keyboard hint: rejected when the key is too small", "[ui][keyboard_hint]") {
    lv_area_t tiny = make_area(0, 0, 6, 6);
    lv_area_t hint{};

    // A 10x12 glyph cannot fit a 6x6 key even at a 1px inset. Returning false is
    // what stops the hint being drawn over the neighbouring key.
    REQUIRE_FALSE(KeyboardManager::compute_hint_area(tiny, 10, 12, &hint));
}

TEST_CASE("Keyboard hint: falls back to a tight inset before giving up", "[ui][keyboard_hint]") {
    // Inset is keyed off button height: 60 / 20 = 3. A 9x10 glyph at that inset needs
    // 9 + 6 = 15 of width, which this narrow key does not have — but it does fit at
    // the 1px fallback (9 + 2 = 11 <= 13).
    lv_area_t narrow = make_area(0, 0, 13, 60);
    lv_area_t hint{};

    REQUIRE(KeyboardManager::compute_hint_area(narrow, 9, 10, &hint));
    REQUIRE(contains(narrow, hint));
    // Confirms the fallback actually ran rather than the preferred inset fitting.
    REQUIRE(narrow.x2 - hint.x2 == 1);
}

TEST_CASE("Keyboard hint: small keys hug the corner more tightly than large ones",
          "[ui][keyboard_hint]") {
    // The reason inset is derived from the key rather than the glyph: a cramped key
    // must push the hint further into its corner to stay clear of the centred letter.
    lv_area_t small_key = make_area(0, 0, 48, 34);  // micro breakpoint
    lv_area_t large_key = make_area(0, 0, 128, 96); // xlarge breakpoint
    lv_area_t small_hint{};
    lv_area_t large_hint{};

    REQUIRE(KeyboardManager::compute_hint_area(small_key, 9, 10, &small_hint));
    REQUIRE(KeyboardManager::compute_hint_area(large_key, 9, 10, &large_hint));

    const int32_t small_gap = small_key.x2 - small_hint.x2;
    const int32_t large_gap = large_key.x2 - large_hint.x2;
    REQUIRE(small_gap < large_gap);
    REQUIRE(small_gap >= 1);
}

TEST_CASE("Keyboard hint: degenerate glyph sizes are rejected", "[ui][keyboard_hint]") {
    lv_area_t btn = make_area(0, 0, 64, 48);
    lv_area_t hint{};

    REQUIRE_FALSE(KeyboardManager::compute_hint_area(btn, 0, 10, &hint));
    REQUIRE_FALSE(KeyboardManager::compute_hint_area(btn, 9, 0, &hint));
    REQUIRE_FALSE(KeyboardManager::compute_hint_area(btn, -3, 10, &hint));
    REQUIRE_FALSE(KeyboardManager::compute_hint_area(btn, 9, 10, nullptr));
}

TEST_CASE("Keyboard hint: never escapes the key at any realistic size", "[ui][keyboard_hint]") {
    // The invariant that matters. Sweeps key widths from a cramped micro-breakpoint
    // key up to a full-width spacebar, key heights across every row height we ship,
    // and glyph sizes spanning font_xs at every DPI. Either the call refuses, or the
    // rect it produces is fully inside the key. There is no third outcome.
    const std::vector<int32_t> widths = {4, 8, 12, 16, 20, 24, 32, 40, 56, 64, 80, 120, 200, 420};
    const std::vector<int32_t> heights = {6, 10, 14, 18, 24, 30, 36, 48, 60, 72};
    const std::vector<int32_t> glyphs = {4, 6, 8, 9, 10, 12, 14, 16, 20};

    // Offset origin: real keys are never at (0,0), and an anchoring bug that
    // happens to work at the origin must still be caught.
    const int32_t ox = 137;
    const int32_t oy = 291;

    size_t fitted = 0;
    size_t refused = 0;

    for (int32_t w : widths) {
        for (int32_t h : heights) {
            for (int32_t g : glyphs) {
                lv_area_t btn = make_area(ox, oy, w, h);
                lv_area_t hint{};
                const int32_t gw = (g * 9) / 10; // hint_w is 0.9 * hint_h in the caller

                if (KeyboardManager::compute_hint_area(btn, gw, g, &hint)) {
                    INFO("key " << w << "x" << h << " glyph " << gw << "x" << g);
                    REQUIRE(contains(btn, hint));
                    fitted++;
                } else {
                    refused++;
                }
            }
        }
    }

    // Guard against a vacuous pass: the sweep must actually exercise both branches.
    REQUIRE(fitted > 0);
    REQUIRE(refused > 0);
}
