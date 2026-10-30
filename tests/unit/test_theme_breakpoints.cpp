// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_breakpoints.cpp
 * @brief breakpoint_for() tier boundaries, responsive_pick(), and the print_tune z-offset tokens
 *
 * The suffix ladder through theme_manager_get_breakpoint_suffix() is pinned in
 * test_display_resolution.cpp; the optional-tier fallback in test_plugin_consts.cpp
 * (theme_manager_pick_px_tier) and test_vertical_breakpoint_tokens.cpp.
 */

#include "theme_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// breakpoint_for() + responsive_pick() — the shared primitives that
// theme_manager_get_breakpoint_suffix() and every responsive site now route
// through. The tier boundaries are load-bearing: the landscape display is tuned
// against them and must not shift (memory project_portrait_landscape_invariant_rule).
// ============================================================================

TEST_CASE("breakpoint_for classifies each tier at its inclusive boundary", "[theme][breakpoints]") {
    REQUIRE(breakpoint_for(1) == UiBreakpoint::Micro);
    REQUIRE(breakpoint_for(272) == UiBreakpoint::Micro); // inclusive top of MICRO
    REQUIRE(breakpoint_for(273) == UiBreakpoint::Tiny);
    REQUIRE(breakpoint_for(390) == UiBreakpoint::Tiny);
    REQUIRE(breakpoint_for(391) == UiBreakpoint::Small);
    REQUIRE(breakpoint_for(460) == UiBreakpoint::Small);
    REQUIRE(breakpoint_for(461) == UiBreakpoint::Medium);
    REQUIRE(breakpoint_for(550) == UiBreakpoint::Medium);
    REQUIRE(breakpoint_for(551) == UiBreakpoint::Large);
    REQUIRE(breakpoint_for(700) == UiBreakpoint::Large);
    REQUIRE(breakpoint_for(701) == UiBreakpoint::XLarge);
    REQUIRE(breakpoint_for(1000) == UiBreakpoint::XLarge);
    REQUIRE(breakpoint_for(1001) == UiBreakpoint::XXLarge);
    REQUIRE(breakpoint_for(4000) == UiBreakpoint::XXLarge);
}

TEST_CASE("breakpoint_for matches the shipping landscape displays", "[theme][breakpoints]") {
    // Constrained axis is the height in landscape. Load-bearing: re-tiering any of
    // these would resize a production display the layout has been tuned against.
    REQUIRE(breakpoint_for(272) == UiBreakpoint::Micro);  // 480x272
    REQUIRE(breakpoint_for(320) == UiBreakpoint::Tiny);   // 480x320
    REQUIRE(breakpoint_for(480) == UiBreakpoint::Medium); // 800x480
    REQUIRE(breakpoint_for(600) == UiBreakpoint::Large);  // 1024x600
    REQUIRE(breakpoint_for(720) == UiBreakpoint::XLarge); // 1280x720
}

TEST_CASE("responsive_pick returns the value matching the tier", "[theme][breakpoints]") {
    // Distinct sentinel per tier so any mis-wired case is caught.
    auto pick = [](UiBreakpoint bp) { return responsive_pick(bp, 10, 20, 30, 40, 50, 60, 70); };
    REQUIRE(pick(UiBreakpoint::Micro) == 10);
    REQUIRE(pick(UiBreakpoint::Tiny) == 20);
    REQUIRE(pick(UiBreakpoint::Small) == 30);
    REQUIRE(pick(UiBreakpoint::Medium) == 40);
    REQUIRE(pick(UiBreakpoint::Large) == 50);
    REQUIRE(pick(UiBreakpoint::XLarge) == 60);
    REQUIRE(pick(UiBreakpoint::XXLarge) == 70);
}

TEST_CASE("responsive_pick + breakpoint_for reproduce the public suffix ladder",
          "[theme][breakpoints]") {
    // This composition is exactly how theme_manager_get_breakpoint_suffix() is now
    // implemented — assert it end-to-end and against the public API it backs, so a
    // regression in either primitive fails here.
    auto suffix = [](int32_t res) {
        return responsive_pick(breakpoint_for(res), "_micro", "_tiny", "_small", "_medium",
                               "_large", "_xlarge", "_xxlarge");
    };
    REQUIRE(std::string(suffix(272)) == "_micro");
    REQUIRE(std::string(suffix(480)) == "_medium");
    REQUIRE(std::string(suffix(2160)) == "_xxlarge");
    REQUIRE(std::string(suffix(480)) == theme_manager_get_breakpoint_suffix(480));
    REQUIRE(std::string(suffix(600)) == theme_manager_get_breakpoint_suffix(600));
}

// ============================================================================
// print_tune z-offset tokens (ui_xml/print_tune_tokens.xml)
// ============================================================================
//
// These three size the z-offset row in both ui_xml/print_tune_panel.xml and
// ui_xml/portrait/print_tune_panel.xml. They were flat 170/90/160 literals, and
// at 272x480 that left the four z-step buttons 0px wide — present, bound, and
// impossible to tap.
//
// The failure mode being pinned is silent in both directions:
//   - Drop one of the required _small/_medium/_large tiers and
//     theme_manager_resolve_px_tokens() skips the token entirely (it `continue`s
//     on an incomplete triplet). `#z_tune_indicator_w` then resolves to nothing
//     and the widget lands at width 0. Nothing warns.
//   - Flatten the ladder to one value at every tier and the tokens still
//     resolve, the gates still pass, and the panel is back to hardcoded sizing
//     wearing a token's name.

TEST_CASE("print_tune z-offset tokens declare the required triplet", "[theme][breakpoints]") {
    auto small = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_small");
    auto medium = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_medium");
    auto large = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_large");

    for (const char* base : {"z_tune_indicator_w", "z_tune_indicator_h", "z_tune_dir_col_w"}) {
        INFO("token: " << base);
        REQUIRE(small.count(base) > 0);
        REQUIRE(medium.count(base) > 0);
        REQUIRE(large.count(base) > 0);
    }
}

TEST_CASE("print_tune z-offset tokens actually shrink on narrow displays", "[theme][breakpoints]") {
    auto micro = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_micro");
    auto tiny = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");
    auto medium = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_medium");

    for (const char* base : {"z_tune_indicator_w", "z_tune_indicator_h", "z_tune_dir_col_w"}) {
        INFO("token: " << base);
        REQUIRE(micro.count(base) > 0);
        REQUIRE(tiny.count(base) > 0);

        const int micro_v = std::stoi(micro.at(base));
        const int tiny_v = std::stoi(tiny.at(base));
        const int medium_v = std::stoi(medium.at(base));

        // Monotonic, and micro strictly smaller than medium — a flat ladder is
        // the regression this catches.
        REQUIRE(micro_v <= tiny_v);
        REQUIRE(tiny_v <= medium_v);
        REQUIRE(micro_v < medium_v);
    }
}
