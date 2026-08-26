// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme_loader.h"

#include <cstdio>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("ModePalette index access", "[theme][dual-palette]") {
    ModePalette palette;
    palette.screen_bg = "#282828";
    palette.focus = "#FFFFFF";
    REQUIRE(palette.at(0) == "#282828");
    REQUIRE(palette.at(15) == "#FFFFFF");
}

TEST_CASE("ModePalette::color_names order", "[theme][dual-palette]") {
    auto& names = ModePalette::color_names();
    REQUIRE(names.size() == 16);
    REQUIRE(std::string(names[0]) == "screen_bg");
    REQUIRE(std::string(names[1]) == "overlay_bg");
    REQUIRE(std::string(names[15]) == "focus");
}

TEST_CASE("ModePalette::is_valid - all colors set", "[theme][dual-palette]") {
    ModePalette palette;
    // Fill all 16 colors
    palette.screen_bg = "#111111";
    palette.overlay_bg = "#222222";
    palette.card_bg = "#333333";
    palette.elevated_bg = "#444444";
    palette.border = "#555555";
    palette.text = "#666666";
    palette.text_muted = "#777777";
    palette.text_subtle = "#888888";
    palette.primary = "#999999";
    palette.secondary = "#AAAAAA";
    palette.tertiary = "#BBBBBB";
    palette.info = "#CCCCCC";
    palette.success = "#DDDDDD";
    palette.warning = "#EEEEEE";
    palette.danger = "#FF0000";
    palette.focus = "#FFFFFF";
    REQUIRE(palette.is_valid());
}

TEST_CASE("ModePalette::is_valid - missing colors", "[theme][dual-palette]") {
    ModePalette palette;
    palette.screen_bg = "#111111";
    // Missing other colors
    REQUIRE_FALSE(palette.is_valid());
}

TEST_CASE("ModePalette::at throws on invalid index", "[theme][dual-palette]") {
    ModePalette palette;
    REQUIRE_THROWS_AS(palette.at(16), std::out_of_range);
    REQUIRE_THROWS_AS(palette.at(100), std::out_of_range);
}

TEST_CASE("ThemeData mode support - dual", "[theme][dual-palette]") {
    ThemeData theme;
    theme.name = "Test";
    // Fill dark palette
    theme.dark.screen_bg = "#282828";
    theme.dark.overlay_bg = "#282828";
    theme.dark.card_bg = "#282828";
    theme.dark.elevated_bg = "#282828";
    theme.dark.border = "#282828";
    theme.dark.text = "#FFFFFF";
    theme.dark.text_muted = "#CCCCCC";
    theme.dark.text_subtle = "#999999";
    theme.dark.primary = "#88C0D0";
    theme.dark.secondary = "#A3BE8C";
    theme.dark.tertiary = "#D08770";
    theme.dark.info = "#81A1C1";
    theme.dark.success = "#A3BE8C";
    theme.dark.warning = "#EBCB8B";
    theme.dark.danger = "#BF616A";
    theme.dark.focus = "#88C0D0";
    // Fill light palette
    theme.light.screen_bg = "#FFFFFF";
    theme.light.overlay_bg = "#F0F0F0";
    theme.light.card_bg = "#FFFFFF";
    theme.light.elevated_bg = "#F5F5F5";
    theme.light.border = "#E0E0E0";
    theme.light.text = "#282828";
    theme.light.text_muted = "#555555";
    theme.light.text_subtle = "#888888";
    theme.light.primary = "#5E81AC";
    theme.light.secondary = "#A3BE8C";
    theme.light.tertiary = "#D08770";
    theme.light.info = "#5E81AC";
    theme.light.success = "#3FA47D";
    theme.light.warning = "#B08900";
    theme.light.danger = "#B23A48";
    theme.light.focus = "#5E81AC";

    REQUIRE(theme.supports_dark());
    REQUIRE(theme.supports_light());
    REQUIRE(theme.get_mode_support() == ThemeModeSupport::DUAL_MODE);
}

TEST_CASE("ThemeData mode support - dark only", "[theme][dual-palette]") {
    ThemeData theme;
    theme.name = "Test";
    // Fill only dark palette (same values as above)
    theme.dark.screen_bg = "#282828";
    theme.dark.overlay_bg = "#282828";
    theme.dark.card_bg = "#282828";
    theme.dark.elevated_bg = "#282828";
    theme.dark.border = "#282828";
    theme.dark.text = "#FFFFFF";
    theme.dark.text_muted = "#CCCCCC";
    theme.dark.text_subtle = "#999999";
    theme.dark.primary = "#88C0D0";
    theme.dark.secondary = "#A3BE8C";
    theme.dark.tertiary = "#D08770";
    theme.dark.info = "#81A1C1";
    theme.dark.success = "#A3BE8C";
    theme.dark.warning = "#EBCB8B";
    theme.dark.danger = "#BF616A";
    theme.dark.focus = "#88C0D0";
    // light palette empty

    REQUIRE(theme.supports_dark());
    REQUIRE_FALSE(theme.supports_light());
    REQUIRE(theme.get_mode_support() == ThemeModeSupport::DARK_ONLY);
}

TEST_CASE("ThemeData mode support - light only", "[theme][dual-palette]") {
    ThemeData theme;
    theme.name = "Test";
    // dark palette empty
    // Fill only light palette
    theme.light.screen_bg = "#FFFFFF";
    theme.light.overlay_bg = "#F0F0F0";
    theme.light.card_bg = "#FFFFFF";
    theme.light.elevated_bg = "#F5F5F5";
    theme.light.border = "#E0E0E0";
    theme.light.text = "#282828";
    theme.light.text_muted = "#555555";
    theme.light.text_subtle = "#888888";
    theme.light.primary = "#5E81AC";
    theme.light.secondary = "#A3BE8C";
    theme.light.tertiary = "#D08770";
    theme.light.info = "#5E81AC";
    theme.light.success = "#3FA47D";
    theme.light.warning = "#B08900";
    theme.light.danger = "#B23A48";
    theme.light.focus = "#5E81AC";

    REQUIRE_FALSE(theme.supports_dark());
    REQUIRE(theme.supports_light());
    REQUIRE(theme.get_mode_support() == ThemeModeSupport::LIGHT_ONLY);
}

TEST_CASE("parse_theme_json - new format dual mode", "[theme][dual-palette]") {
    const char* json = R"({
        "name": "Test Dual",
        "dark": {
            "screen_bg": "#2E3440",
            "overlay_bg": "#3B4252",
            "card_bg": "#434C5E",
            "elevated_bg": "#4C566A",
            "border": "#616E88",
            "text": "#ECEFF4",
            "text_muted": "#D8DEE9",
            "text_subtle": "#B8C2D1",
            "primary": "#88C0D0",
            "secondary": "#A3BE8C",
            "tertiary": "#D08770",
            "info": "#81A1C1",
            "success": "#A3BE8C",
            "warning": "#EBCB8B",
            "danger": "#BF616A",
            "focus": "#88C0D0"
        },
        "light": {
            "screen_bg": "#ECEFF4",
            "overlay_bg": "#E5E9F0",
            "card_bg": "#FFFFFF",
            "elevated_bg": "#EDEFF6",
            "border": "#CBD5E1",
            "text": "#2E3440",
            "text_muted": "#3B4252",
            "text_subtle": "#64748B",
            "primary": "#5E81AC",
            "secondary": "#A3BE8C",
            "tertiary": "#D08770",
            "info": "#5E81AC",
            "success": "#3FA47D",
            "warning": "#B08900",
            "danger": "#B23A48",
            "focus": "#5E81AC"
        },
        "border_radius": 12
    })";

    auto theme = parse_theme_json(json, "test_dual.json");

    REQUIRE(theme.name == "Test Dual");
    REQUIRE(theme.supports_dark());
    REQUIRE(theme.supports_light());
    REQUIRE(theme.dark.screen_bg == "#2E3440");
    REQUIRE(theme.light.screen_bg == "#ECEFF4");
    REQUIRE(theme.properties.border_radius_size == 3); // 12px -> Soft
}

TEST_CASE("parse_theme_json - new format dark only", "[theme][dual-palette]") {
    const char* json = R"({
        "name": "Dracula",
        "dark": {
            "screen_bg": "#282A36",
            "overlay_bg": "#21222C",
            "card_bg": "#44475A",
            "elevated_bg": "#6272A4",
            "border": "#6272A4",
            "text": "#F8F8F2",
            "text_muted": "#BFBFBF",
            "text_subtle": "#6272A4",
            "primary": "#BD93F9",
            "secondary": "#50FA7B",
            "tertiary": "#FFB86C",
            "info": "#8BE9FD",
            "success": "#50FA7B",
            "warning": "#F1FA8C",
            "danger": "#FF5555",
            "focus": "#BD93F9"
        },
        "border_radius": 8
    })";

    auto theme = parse_theme_json(json, "dracula.json");

    REQUIRE(theme.name == "Dracula");
    REQUIRE(theme.supports_dark());
    REQUIRE_FALSE(theme.supports_light());
    REQUIRE(theme.get_mode_support() == ThemeModeSupport::DARK_ONLY);
}

TEST_CASE("parse_theme_json - legacy format falls back to the built-in theme",
          "[theme][dual-palette]") {
    // Legacy format with "colors" object is no longer supported
    // Should fall back to the built-in theme
    const char* json = R"({
        "name": "Legacy Theme",
        "colors": {
            "bg_darkest": "#2E3440",
            "bg_dark": "#3B4252",
            "surface_elevated": "#434C5E",
            "surface_dim": "#4C566A",
            "text_light": "#D8DEE9",
            "bg_light": "#E5E9F0",
            "bg_lightest": "#ECEFF4",
            "accent_highlight": "#8FBCBB",
            "accent_primary": "#88C0D0",
            "accent_secondary": "#81A1C1",
            "accent_tertiary": "#5E81AC",
            "status_error": "#BF616A",
            "status_danger": "#D08770",
            "status_warning": "#EBCB8B",
            "status_success": "#A3BE8C",
            "status_special": "#B48EAD"
        },
        "border_radius": 12
    })";

    auto theme = parse_theme_json(json, "legacy.json");

    // Legacy format is not supported - falls back to the built-in theme, which is
    // the configured default rather than Nord (see get_builtin_fallback_theme).
    REQUIRE(theme.name == "HelixScreen");
    REQUIRE(theme.filename == helix::DEFAULT_THEME);
    REQUIRE(theme.is_valid());
}

TEST_CASE("save and reload theme - round trip new format", "[theme][dual-palette]") {
    ThemeData original;
    original.name = "RoundTrip";
    original.filename = "roundtrip";
    // Set dark palette
    original.dark.screen_bg = "#111111";
    original.dark.overlay_bg = "#222222";
    original.dark.card_bg = "#333333";
    original.dark.elevated_bg = "#444444";
    original.dark.border = "#555555";
    original.dark.text = "#FFFFFF";
    original.dark.text_muted = "#CCCCCC";
    original.dark.text_subtle = "#999999";
    original.dark.primary = "#88C0D0";
    original.dark.secondary = "#A3BE8C";
    original.dark.tertiary = "#D08770";
    original.dark.info = "#81A1C1";
    original.dark.success = "#A3BE8C";
    original.dark.warning = "#EBCB8B";
    original.dark.danger = "#BF616A";
    original.dark.focus = "#88C0D0";
    // Set light palette
    original.light.screen_bg = "#FFFFFF";
    original.light.overlay_bg = "#F0F0F0";
    original.light.card_bg = "#FAFAFA";
    original.light.elevated_bg = "#F5F5F5";
    original.light.border = "#E0E0E0";
    original.light.text = "#111111";
    original.light.text_muted = "#555555";
    original.light.text_subtle = "#888888";
    original.light.primary = "#5E81AC";
    original.light.secondary = "#A3BE8C";
    original.light.tertiary = "#D08770";
    original.light.info = "#5E81AC";
    original.light.success = "#3FA47D";
    original.light.warning = "#B08900";
    original.light.danger = "#B23A48";
    original.light.focus = "#5E81AC";

    std::string path = "/tmp/test_theme_dual_roundtrip_" + std::to_string(getpid()) + ".json";
    REQUIRE(save_theme_to_file(original, path));

    auto loaded = load_theme_from_file(path);
    REQUIRE(loaded.name == "RoundTrip");
    REQUIRE(loaded.dark.screen_bg == original.dark.screen_bg);
    REQUIRE(loaded.light.screen_bg == original.light.screen_bg);
    REQUIRE(loaded.supports_dark());
    REQUIRE(loaded.supports_light());

    // Cleanup
    std::remove(path.c_str());
}
