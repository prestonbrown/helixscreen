// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_breakpoints.cpp
 * @brief Unit tests for breakpoint suffix selection and responsive token fallback
 *
 * Tests the 6-tier breakpoint system: MICRO (≤272), TINY (≤390), SMALL (391-460),
 * MEDIUM (461-550), LARGE (551-700), XLARGE (>700) and the _micro→_tiny→_small /
 * _tiny→_small / _xlarge→_large fallback behavior.
 */

#include "theme_manager.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Breakpoint suffix selection
// ============================================================================

TEST_CASE("Breakpoint suffix returns _micro for heights ≤272", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(272)) == "_micro");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(200)) == "_micro");
}

TEST_CASE("Breakpoint suffix returns _tiny for heights 273-390", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(273)) == "_tiny");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(320)) == "_tiny");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(390)) == "_tiny");
}

TEST_CASE("Breakpoint suffix returns _small for heights 391-460", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(391)) == "_small");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(400)) == "_small");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(460)) == "_small");
}

TEST_CASE("Breakpoint suffix returns _medium for heights 461-550", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(461)) == "_medium");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(480)) == "_medium");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(550)) == "_medium");
}

TEST_CASE("Breakpoint suffix returns _large for heights 551-700", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(551)) == "_large");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(600)) == "_large");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(700)) == "_large");
}

TEST_CASE("Breakpoint suffix returns _xlarge for heights 701-1000", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(701)) == "_xlarge");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(720)) == "_xlarge");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(1000)) == "_xlarge");
}

TEST_CASE("Breakpoint suffix returns _xxlarge for heights >1000", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(1001)) == "_xxlarge");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(1080)) == "_xxlarge");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(2160)) == "_xxlarge");
}

TEST_CASE("Breakpoint constants have correct values", "[theme][breakpoints]") {
    REQUIRE(UI_BREAKPOINT_MICRO_MAX == 272);
    REQUIRE(UI_BREAKPOINT_TINY_MAX == 390);
    REQUIRE(UI_BREAKPOINT_SMALL_MAX == 460);
    REQUIRE(UI_BREAKPOINT_MEDIUM_MAX == 550);
    REQUIRE(UI_BREAKPOINT_LARGE_MAX == 700);
    REQUIRE(UI_BREAKPOINT_XLARGE_MAX == 1000);
}

TEST_CASE("Breakpoint index enum has correct values", "[theme][breakpoints]") {
    REQUIRE(to_int(UiBreakpoint::Micro) == 0);
    REQUIRE(to_int(UiBreakpoint::Tiny) == 1);
    REQUIRE(to_int(UiBreakpoint::Small) == 2);
    REQUIRE(to_int(UiBreakpoint::Medium) == 3);
    REQUIRE(to_int(UiBreakpoint::Large) == 4);
    REQUIRE(to_int(UiBreakpoint::XLarge) == 5);
    REQUIRE(to_int(UiBreakpoint::XXLarge) == 6);
}

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
// Responsive token fallback behavior (XML-based, uses test fixtures)
// ============================================================================

TEST_CASE("Responsive token discovery includes _tiny suffix", "[theme][breakpoints]") {
    // Verify that _tiny tokens are discoverable from XML
    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");

    // fan_card_base_width_tiny and fan_card_height_tiny defined in fan_dial.xml
    REQUIRE(tiny_tokens.count("fan_card_base_width") > 0);
    REQUIRE(tiny_tokens.count("fan_card_height") > 0);
}

TEST_CASE("Tokens without _tiny variant still have _small available", "[theme][breakpoints]") {
    // space_2xl has _small/_medium/_large but no _tiny — verify _small exists for fallback
    auto small_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_small");
    REQUIRE(small_tokens.count("space_2xl") > 0);

    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");
    REQUIRE(tiny_tokens.count("space_2xl") == 0);
}

TEST_CASE("Validation does not require _tiny for complete sets", "[theme][breakpoints]") {
    // _tiny is optional — validation should not warn about missing _tiny
    auto warnings = theme_manager_validate_constant_sets("ui_xml");

    REQUIRE(std::none_of(warnings.begin(), warnings.end(), [](const std::string& w) {
        return w.find("_tiny") != std::string::npos;
    }));
}

TEST_CASE("Validation does not require _xlarge for complete sets", "[theme][breakpoints]") {
    // _xlarge is optional — validation should not warn about missing _xlarge
    auto warnings = theme_manager_validate_constant_sets("ui_xml");

    REQUIRE(std::none_of(warnings.begin(), warnings.end(), [](const std::string& w) {
        return w.find("_xlarge") != std::string::npos;
    }));
}

// ============================================================================
// _micro → _tiny → _small fallback chain
// ============================================================================

TEST_CASE("Token parsing discovers _micro tokens", "[theme][breakpoints]") {
    // Verify that _micro tokens are discoverable from XML
    auto micro_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_micro");

    // button_height_micro, header_height_micro etc. defined in globals.xml
    REQUIRE(micro_tokens.count("button_height") > 0);
    REQUIRE(micro_tokens.count("header_height") > 0);
}

TEST_CASE("Fallback chain: _micro falls back to _tiny then _small", "[theme][breakpoints]") {
    // button_height has _micro, _tiny, _small, _medium, _large — verify all exist
    auto micro_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_micro");
    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");
    auto small_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_small");

    // button_height should exist in all three
    REQUIRE(small_tokens.count("button_height") > 0);
    REQUIRE(tiny_tokens.count("button_height") > 0);
    REQUIRE(micro_tokens.count("button_height") > 0);

    // Verify the fallback chain values differ (otherwise fallback test is meaningless)
    REQUIRE(micro_tokens.at("button_height") != tiny_tokens.at("button_height"));
    REQUIRE(tiny_tokens.at("button_height") != small_tokens.at("button_height"));
}

TEST_CASE("Fallback chain: _micro uses _small when neither _micro nor _tiny exist",
          "[theme][breakpoints]") {
    // space_2xl has _small/_medium/_large but no _micro or _tiny
    auto micro_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_micro");
    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");
    auto small_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_small");

    REQUIRE(small_tokens.count("space_2xl") > 0);
    REQUIRE(tiny_tokens.count("space_2xl") == 0);
    REQUIRE(micro_tokens.count("space_2xl") == 0);
    // Fallback: _micro → _tiny → _small means space_2xl would use _small value on MICRO
}

TEST_CASE("Validation does not require _micro for complete sets", "[theme][breakpoints]") {
    // _micro is optional — validation should not warn about missing _micro
    auto warnings = theme_manager_validate_constant_sets("ui_xml");

    REQUIRE(std::none_of(warnings.begin(), warnings.end(), [](const std::string& w) {
        return w.find("_micro") != std::string::npos;
    }));
}

// ============================================================================
// print_tune z-offset tokens (ui_xml/print_tune_tokens.xml)
// ============================================================================
//
// These three size the z-offset row in ui_xml/components/tune_z_offset.xml,
// landscape and portrait. They were flat 170/90/160 literals, and
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
