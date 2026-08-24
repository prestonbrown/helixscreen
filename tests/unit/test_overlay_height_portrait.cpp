// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_height_portrait.cpp
 * @brief Overlay height must reserve the nav strip on the VERTICAL axis in portrait
 *
 * The mirror of test_overlay_width_portrait.cpp. ui_xml/navigation_bar.xml is a
 * full-height vertical strip, so landscape overlays reserve horizontal extent
 * and span the full height. ui_xml/portrait/navigation_bar.xml is a full-WIDTH
 * bottom strip (width="100%" height="#button_height_lg"), so portrait overlays
 * must reserve VERTICAL extent instead — otherwise a height="100%" overlay
 * covers the navigation bar completely, which is what shipped.
 *
 * Transient keeps the gap; destination does not. That distinction is #1178 and
 * is orientation-independent — only the axis it applies to changes.
 */

#include "layout_manager.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
// Representative values; the exact breakpoint rung does not matter to the
// formula, only whether nav_height is subtracted at all.
constexpr int32_t NAV_HEIGHT = 112; // button_height_lg at the xlarge tier
constexpr int32_t GAP = 16;         // space_lg
} // namespace

TEST_CASE("Landscape overlays span the full height", "[theme][overlay-height][portrait]") {
    // The vertical nav strip occupies no vertical extent — it IS full height.
    // Both classes must keep spanning the whole display.
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
        auto hgt = compute_overlay_heights(c.w, c.h, NAV_HEIGHT, GAP);
        CHECK(hgt.destination == c.h);
        CHECK(hgt.transient == c.h);
    }
}

TEST_CASE("Portrait overlays reserve the bottom nav strip", "[theme][overlay-height][portrait]") {
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
        auto hgt = compute_overlay_heights(c.w, c.h, NAV_HEIGHT, GAP);
        CHECK(hgt.destination == c.h - NAV_HEIGHT);
        // The gap signals "you will return from this" (#1178). In portrait it
        // sits between the overlay's bottom edge and the nav bar.
        CHECK(hgt.transient == c.h - NAV_HEIGHT - GAP);
    }
}

TEST_CASE("Portrait overlay no longer covers the navigation bar",
          "[theme][overlay-height][portrait]") {
    // State the regression as a delta: on 480x800 the old behaviour was
    // height="100%" = 800, hiding all 112px of the bottom bar.
    const auto portrait = compute_overlay_heights(480, 800, NAV_HEIGHT, GAP);
    constexpr int32_t old_height = 800;

    CHECK(portrait.destination == 688);
    CHECK(old_height - portrait.destination == NAV_HEIGHT);
    CHECK(portrait.transient == 672);
}

TEST_CASE("Height and width classification agree on orientation",
          "[theme][overlay-height][portrait]") {
    // If these disagreed, an overlay could reserve nav space on both axes or
    // neither. Both must consult the same detect_layout_type().
    struct Case {
        int32_t w;
        int32_t h;
    };
    const Case portrait_cases[] = {{320, 1480}, {480, 800}, {272, 480}, {320, 480}};
    for (const auto& c : portrait_cases) {
        INFO("size " << c.w << "x" << c.h);
        const auto wid = compute_overlay_widths(c.w, c.h, 54, GAP);
        const auto hgt = compute_overlay_heights(c.w, c.h, NAV_HEIGHT, GAP);
        // Portrait: full width reclaimed, vertical extent surrendered.
        CHECK(wid.destination == c.w);
        CHECK(hgt.destination == c.h - NAV_HEIGHT);
    }

    const Case landscape_cases[] = {{800, 480}, {1920, 480}, {480, 272}, {480, 320}};
    for (const auto& c : landscape_cases) {
        INFO("size " << c.w << "x" << c.h);
        const auto wid = compute_overlay_widths(c.w, c.h, 54, GAP);
        const auto hgt = compute_overlay_heights(c.w, c.h, NAV_HEIGHT, GAP);
        // Landscape: horizontal extent surrendered, full height kept.
        CHECK(wid.destination == c.w - 54);
        CHECK(hgt.destination == c.h);
    }
}
