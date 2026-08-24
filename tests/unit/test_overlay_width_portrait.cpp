// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_width_portrait.cpp
 * @brief Overlay width must not reserve nav_width on the horizontal axis in portrait
 *
 * `overlay_width_transient` / `overlay_width_destination` are computed as
 * `hor_res - nav_width [- gap]`, which reserves room for the navigation bar.
 * That reservation is only correct when the nav bar is a full-height vertical
 * strip at the leading edge — which is what ui_xml/navigation_bar.xml builds.
 *
 * ui_xml/portrait/navigation_bar.xml is a full-WIDTH strip along the bottom
 * (width="100%" height="#button_height_lg"), so in portrait the bar occupies no
 * horizontal extent at all. Subtracting it there leaves a dead strip of
 * backdrop down the side of every overlay — 54px of 320 on the Waveshare 11.9".
 *
 * The layout classification itself lives in layout_manager so that the
 * threshold that picks ui_xml/portrait/ and the threshold that sizes overlays
 * can never disagree.
 */

#include "../lvgl_test_fixture.h"
#include "layout_manager.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Reads a const back out of the "globals" XML scope as an int.
int32_t get_const_int(const char* name) {
    const char* s = lv_xml_get_const(nullptr, name);
    REQUIRE(s != nullptr);
    return static_cast<int32_t>(std::atoi(s));
}

// Representative values; the exact breakpoint rung does not matter to the
// formula, only whether nav_width is subtracted at all.
constexpr int32_t NAV_WIDTH = 54;
constexpr int32_t GAP = 16;

} // namespace

// ============================================================================
// Classification — shared with resolve_xml_path's portrait/ selection
// ============================================================================

TEST_CASE("detect_layout_type is callable before LayoutManager::init",
          "[layout-manager][portrait]") {
    // theme_manager_register_responsive_spacing runs in Application phase 6;
    // LayoutManager::init runs in phase 8b. The overlay-width computation
    // therefore cannot consult the singleton and must classify from raw
    // dimensions, exactly as the existing ultrawide check does.
    CHECK(detect_layout_type(800, 480) == LayoutType::STANDARD);
    CHECK(detect_layout_type(1920, 480) == LayoutType::ULTRAWIDE);
    CHECK(detect_layout_type(320, 1480) == LayoutType::PORTRAIT);
    CHECK(detect_layout_type(480, 800) == LayoutType::PORTRAIT);
    CHECK(detect_layout_type(272, 480) == LayoutType::MICRO_PORTRAIT);
    CHECK(detect_layout_type(320, 480) == LayoutType::TINY_PORTRAIT);
    CHECK(detect_layout_type(480, 272) == LayoutType::MICRO);
    CHECK(detect_layout_type(480, 320) == LayoutType::TINY);
}

TEST_CASE("is_portrait_layout covers every portrait class", "[layout-manager][portrait]") {
    CHECK(is_portrait_layout(LayoutType::PORTRAIT));
    CHECK(is_portrait_layout(LayoutType::TINY_PORTRAIT));
    CHECK(is_portrait_layout(LayoutType::MICRO_PORTRAIT));

    CHECK_FALSE(is_portrait_layout(LayoutType::STANDARD));
    CHECK_FALSE(is_portrait_layout(LayoutType::ULTRAWIDE));
    CHECK_FALSE(is_portrait_layout(LayoutType::TINY));
    CHECK_FALSE(is_portrait_layout(LayoutType::MICRO));
}

TEST_CASE("classification agrees with the portrait/ variant chain", "[layout-manager][portrait]") {
    // If these two ever disagree, an overlay would be sized for one orientation
    // while being handed the XML of the other.
    struct Case {
        int w;
        int h;
    };
    const Case portrait_cases[] = {{320, 1480}, {480, 800}, {272, 480}, {320, 480}};
    for (const auto& c : portrait_cases) {
        INFO("size " << c.w << "x" << c.h);
        CHECK(is_portrait_layout(detect_layout_type(c.w, c.h)));
    }
}

// ============================================================================
// Overlay widths
// ============================================================================

TEST_CASE("Landscape overlays still reserve the nav strip", "[theme][overlay-width][portrait]") {
    // Guards the #1178 behaviour: the vertical nav bar occupies real horizontal
    // extent in landscape, and both widths must keep accounting for it.
    struct Case {
        int32_t w;
        int32_t h;
        const char* label;
    };
    const Case cases[] = {
        {800, 480, "800x480 (STANDARD)"},
        {1920, 480, "1920x480 (ULTRAWIDE)"},
        {480, 272, "480x272 (MICRO)"},
        {480, 320, "480x320 (TINY)"},
    };
    for (const auto& c : cases) {
        INFO(c.label);
        auto w = compute_overlay_widths(c.w, c.h, NAV_WIDTH, GAP);
        CHECK(w.destination == c.w - NAV_WIDTH);
        CHECK(w.transient == c.w - NAV_WIDTH - GAP);
    }
}

TEST_CASE("Portrait overlays claim the full screen width", "[theme][overlay-width][portrait]") {
    // ui_xml/portrait/navigation_bar.xml is a bottom strip: width="100%",
    // height="#button_height_lg". It consumes zero horizontal extent, so a
    // destination overlay must span the whole display.
    struct Case {
        int32_t w;
        int32_t h;
        const char* label;
    };
    const Case cases[] = {
        {320, 1480, "Waveshare 11.9 (PORTRAIT)"},
        {480, 800, "480x800 (PORTRAIT)"},
        {272, 480, "272x480 (MICRO_PORTRAIT)"},
        {320, 480, "320x480 (TINY_PORTRAIT)"},
    };
    for (const auto& c : cases) {
        INFO(c.label);
        auto w = compute_overlay_widths(c.w, c.h, NAV_WIDTH, GAP);
        CHECK(w.destination == c.w);
        // Transient is ALSO full width in portrait. The "you will return from
        // this" gap (#1178) is spent on whichever axis the nav bar occupies —
        // in portrait that is vertical (compute_overlay_heights carries it,
        // above the bottom nav strip), not horizontal. A horizontal sliver
        // here would land as an 8px inset on BOTH edges under
        // LV_ALIGN_TOP_MID, which is not what the gap means.
        CHECK(w.transient == c.w);
    }
}

TEST_CASE("Portrait recovers exactly the width the nav strip was taking",
          "[theme][overlay-width][portrait]") {
    // State the regression as a delta: on the 320x1480 target the old
    // arithmetic handed back 266 of 320. Assert we reclaimed those 54px rather
    // than merely rearranging the formula.
    const auto portrait = compute_overlay_widths(320, 1480, NAV_WIDTH, GAP);
    const int32_t old_destination = 320 - NAV_WIDTH;

    CHECK(old_destination == 266);
    CHECK(portrait.destination == 320);
    CHECK(portrait.destination - old_destination == NAV_WIDTH);
}

TEST_CASE("Overlay width formula is wired into both const paths",
          "[theme][overlay-width][portrait]") {
    // The formula lives in one function, but two call sites publish it: startup
    // registration and the resize refresh. Neither may compute its own version.
    // (The const registry ignores duplicate registrations, so this asserts the
    // wiring exists at the default test resolution rather than sweeping sizes.)
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    theme_manager_register_responsive_spacing(disp);
    theme_manager_refresh_layout_constants(disp);

    const int32_t hor = lv_display_get_horizontal_resolution(disp);
    const int32_t ver = lv_display_get_vertical_resolution(disp);
    const auto expected =
        compute_overlay_widths(hor, ver, get_const_int("nav_width"), get_const_int("space_lg"));

    CHECK(get_const_int("overlay_width_transient") == expected.transient);
    CHECK(get_const_int("overlay_width_destination") == expected.destination);
}
