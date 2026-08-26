// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "border_radius_sizes.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("BorderRadiusSizes: size count", "[theme]") {
    REQUIRE(BorderRadiusSizes::count() == 8);
}

TEST_CASE("BorderRadiusSizes: name lookup", "[theme]") {
    REQUIRE(std::string(BorderRadiusSizes::name(0)) == "None");
    REQUIRE(std::string(BorderRadiusSizes::name(3)) == "Soft");
    REQUIRE(std::string(BorderRadiusSizes::name(7)) == "Full");
}

TEST_CASE("BorderRadiusSizes: pixel value at breakpoint", "[theme]") {
    // "Soft" (index 3) at Large breakpoint = 10
    REQUIRE(BorderRadiusSizes::pixels(3, "_large") == 10);
    // "Soft" at Micro = 3
    REQUIRE(BorderRadiusSizes::pixels(3, "_micro") == 3);
    // "None" is always 0
    REQUIRE(BorderRadiusSizes::pixels(0, "_xxlarge") == 0);
    // "Full" is always 9999
    REQUIRE(BorderRadiusSizes::pixels(7, "_tiny") == 9999);
}

TEST_CASE("BorderRadiusSizes: clamp out-of-range index", "[theme]") {
    // Index 99 should clamp to max valid (7)
    REQUIRE(BorderRadiusSizes::pixels(99, "_large") == 9999);
    // Negative index should clamp to 0
    REQUIRE(BorderRadiusSizes::pixels(-1, "_large") == 0);
}

TEST_CASE("BorderRadiusSizes: nearest_size_index migration", "[theme]") {
    // Raw 12 should map to "Soft" (index 3) — exact match at Large breakpoint
    REQUIRE(BorderRadiusSizes::nearest_size_index(12) == 3);
    // Raw 0 maps to "None"
    REQUIRE(BorderRadiusSizes::nearest_size_index(0) == 0);
    // Raw 40 (old max) maps to "Full" (index 7)
    REQUIRE(BorderRadiusSizes::nearest_size_index(40) == 7);
    // Raw 9 is between Subtle(8) and Soft(12) at XXLarge — closer to Subtle
    REQUIRE(BorderRadiusSizes::nearest_size_index(9) == 2);
    // Raw 14 is equidistant between Soft(12) and Rounded(16) — first match wins (Soft)
    REQUIRE(BorderRadiusSizes::nearest_size_index(14) == 3);
    // Raw 1 is closer to None(0) than Minimal(4)
    REQUIRE(BorderRadiusSizes::nearest_size_index(1) == 0);
    // Raw 2 is equidistant between None(0) and Minimal(4) — first match wins (None)
    REQUIRE(BorderRadiusSizes::nearest_size_index(2) == 0);
    // Raw 3 is closer to Minimal(4) than None(0)
    REQUIRE(BorderRadiusSizes::nearest_size_index(3) == 1);
}

// ============================================================================
// Theme JSON migration tests
// ============================================================================

#include "theme_loader.h"

TEST_CASE("Theme JSON migration: old border_radius integer becomes border_radius_size", "[theme]") {
    std::string old_json = R"({
        "name": "Test Theme",
        "dark": {
            "screen_bg": "#2e3440", "overlay_bg": "#3b4252", "card_bg": "#434c5e",
            "elevated_bg": "#4c566a", "border": "#4c566a", "text": "#eceff4",
            "text_muted": "#d8dee9", "text_subtle": "#a5b1c2", "primary": "#88c0d0",
            "secondary": "#81a1c1", "tertiary": "#5e81ac", "info": "#88c0d0",
            "success": "#a3be8c", "warning": "#ebcb8b", "danger": "#bf616a",
            "focus": "#8fbcbb"
        },
        "border_radius": 12
    })";
    auto theme = helix::parse_theme_json(old_json, "test.json");
    REQUIRE(theme.properties.border_radius_size == 3); // 12 -> "Soft"
}

TEST_CASE("Theme JSON: new border_radius_size field is used directly", "[theme]") {
    std::string new_json = R"({
        "name": "Test Theme",
        "dark": {
            "screen_bg": "#2e3440", "overlay_bg": "#3b4252", "card_bg": "#434c5e",
            "elevated_bg": "#4c566a", "border": "#4c566a", "text": "#eceff4",
            "text_muted": "#d8dee9", "text_subtle": "#a5b1c2", "primary": "#88c0d0",
            "secondary": "#81a1c1", "tertiary": "#5e81ac", "info": "#88c0d0",
            "success": "#a3be8c", "warning": "#ebcb8b", "danger": "#bf616a",
            "focus": "#8fbcbb"
        },
        "border_radius_size": 5
    })";
    auto theme = helix::parse_theme_json(new_json, "test.json");
    REQUIRE(theme.properties.border_radius_size == 5); // "Bold"
}

TEST_CASE("Theme JSON save: writes border_radius_size, not raw border_radius", "[theme]") {
    auto theme = helix::get_builtin_fallback_theme();
    theme.properties.border_radius_size = 4;

    std::string tmp = "/tmp/test_theme_save.json";
    REQUIRE(helix::save_theme_to_file(theme, tmp));

    auto reloaded = helix::load_theme_from_file(tmp);
    REQUIRE(reloaded.properties.border_radius_size == 4);
}
