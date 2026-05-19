// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_manager_new.cpp
 * @brief Unit tests for the new ThemeManager table-driven style system
 *
 * Tests the foundational data structures for the ThemeManager refactor:
 * - StyleRole enum: semantic roles for all styles
 * - ThemePalette struct: holds all semantic colors
 * - StyleEntry struct: binds a role to a configure function
 */

#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Task 1.1: StyleRole Enum Tests
// ============================================================================

TEST_CASE("StyleRole enum has expected values", "[theme-manager][style-role]") {
    REQUIRE(static_cast<int>(StyleRole::Card) >= 0);
    REQUIRE(static_cast<int>(StyleRole::Dialog) >= 0);
    REQUIRE(static_cast<int>(StyleRole::TextPrimary) >= 0);
    REQUIRE(static_cast<int>(StyleRole::ButtonPrimary) >= 0);
    REQUIRE(static_cast<int>(StyleRole::IconPrimary) >= 0);
    REQUIRE(static_cast<int>(StyleRole::COUNT) > 30);
}

TEST_CASE("StyleRole::COUNT equals total style count", "[theme-manager][style-role]") {
    constexpr size_t count = static_cast<size_t>(StyleRole::COUNT);
    REQUIRE(count >= 35);
    REQUIRE(count <= 50);
}

// ============================================================================
// Task 1.2: ThemePalette Struct Tests
// ============================================================================

TEST_CASE("Palette holds semantic colors", "[theme-manager][palette]") {
    ThemePalette p{};
    REQUIRE(sizeof(p.screen_bg) == sizeof(lv_color_t));
    REQUIRE(sizeof(p.primary) == sizeof(lv_color_t));
    REQUIRE(p.border_radius >= 0);
}

// ============================================================================
// Task 1.3: StyleEntry and Configure Function Type Tests
// ============================================================================

TEST_CASE("StyleEntry holds role and configure function", "[theme-manager][style-entry]") {
    auto configure_red = [](lv_style_t* s, const ThemePalette& p) {
        lv_style_set_bg_color(s, lv_color_hex(0xFF0000));
    };
    StyleEntry entry{StyleRole::Card, {}, configure_red};
    REQUIRE(entry.role == StyleRole::Card);
    REQUIRE(entry.configure != nullptr);
}

// ============================================================================
// Task 2.1: ThemeManager Singleton Tests
// ============================================================================

#include "../lvgl_test_fixture.h"

TEST_CASE("ThemeManager is singleton", "[theme-manager][singleton]") {
    auto& tm1 = ThemeManager::instance();
    auto& tm2 = ThemeManager::instance();
    REQUIRE(&tm1 == &tm2);
}

TEST_CASE_METHOD(LVGLTestFixture, "ThemeManager::get_style returns valid style for each role",
                 "[theme-manager][get-style]") {
    auto& tm = ThemeManager::instance();

    // Card style should exist (may be null before init, but pointer should be valid after)
    lv_style_t* card = tm.get_style(StyleRole::Card);
    lv_style_t* btn = tm.get_style(StyleRole::ButtonPrimary);

    // Different roles return different pointers
    REQUIRE(card != btn);
}

// ============================================================================
// Phase 3: Style Configure Function Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Card style has correct properties after init",
                 "[theme-manager][card-style]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_style_t* card = tm.get_style(StyleRole::Card);
    REQUIRE(card != nullptr);

    // Card should have background color set
    lv_style_value_t bg;
    auto res = lv_style_get_prop(card, LV_STYLE_BG_COLOR, &bg);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    // Card should have border
    lv_style_value_t border_width;
    res = lv_style_get_prop(card, LV_STYLE_BORDER_WIDTH, &border_width);
    REQUIRE(res == LV_STYLE_RES_FOUND);
    REQUIRE(border_width.num > 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Text styles have text color set",
                 "[theme-manager][text-style]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_style_t* primary = tm.get_style(StyleRole::TextPrimary);
    lv_style_t* muted = tm.get_style(StyleRole::TextMuted);

    lv_style_value_t c1, c2;
    REQUIRE(lv_style_get_prop(primary, LV_STYLE_TEXT_COLOR, &c1) == LV_STYLE_RES_FOUND);
    REQUIRE(lv_style_get_prop(muted, LV_STYLE_TEXT_COLOR, &c2) == LV_STYLE_RES_FOUND);
}

TEST_CASE_METHOD(LVGLTestFixture, "Icon styles have text color set",
                 "[theme-manager][icon-style]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_style_t* icon_primary = tm.get_style(StyleRole::IconPrimary);
    lv_style_t* icon_danger = tm.get_style(StyleRole::IconDanger);

    lv_style_value_t c1, c2;
    REQUIRE(lv_style_get_prop(icon_primary, LV_STYLE_TEXT_COLOR, &c1) == LV_STYLE_RES_FOUND);
    REQUIRE(lv_style_get_prop(icon_danger, LV_STYLE_TEXT_COLOR, &c2) == LV_STYLE_RES_FOUND);
}

TEST_CASE_METHOD(LVGLTestFixture, "Button styles have background set",
                 "[theme-manager][button-style]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_style_t* btn = tm.get_style(StyleRole::Button);
    lv_style_t* btn_primary = tm.get_style(StyleRole::ButtonPrimary);

    lv_style_value_t c1, c2;
    REQUIRE(lv_style_get_prop(btn, LV_STYLE_BG_COLOR, &c1) == LV_STYLE_RES_FOUND);
    REQUIRE(lv_style_get_prop(btn_primary, LV_STYLE_BG_COLOR, &c2) == LV_STYLE_RES_FOUND);
}

TEST_CASE_METHOD(LVGLTestFixture, "Severity styles have border color set",
                 "[theme-manager][severity-style]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_style_t* info = tm.get_style(StyleRole::SeverityInfo);
    lv_style_t* danger = tm.get_style(StyleRole::SeverityDanger);

    lv_style_value_t c1, c2;
    REQUIRE(lv_style_get_prop(info, LV_STYLE_BORDER_COLOR, &c1) == LV_STYLE_RES_FOUND);
    REQUIRE(lv_style_get_prop(danger, LV_STYLE_BORDER_COLOR, &c2) == LV_STYLE_RES_FOUND);
}

TEST_CASE_METHOD(LVGLTestFixture, "Spinner style has arc color set",
                 "[theme-manager][spinner-style]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_style_t* spinner = tm.get_style(StyleRole::Spinner);
    REQUIRE(spinner != nullptr);

    lv_style_value_t arc_color;
    REQUIRE(lv_style_get_prop(spinner, LV_STYLE_ARC_COLOR, &arc_color) == LV_STYLE_RES_FOUND);
}

TEST_CASE_METHOD(LVGLTestFixture, "ObjBase style has transparent background",
                 "[theme-manager][obj-base-style]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_style_t* obj_base = tm.get_style(StyleRole::ObjBase);
    REQUIRE(obj_base != nullptr);

    lv_style_value_t bg_opa;
    REQUIRE(lv_style_get_prop(obj_base, LV_STYLE_BG_OPA, &bg_opa) == LV_STYLE_RES_FOUND);
    REQUIRE(bg_opa.num == LV_OPA_0);
}

TEST_CASE_METHOD(LVGLTestFixture, "All registered configure functions are called",
                 "[theme-manager][configure-all]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    // Check that all major style roles have properties set
    struct TestCase {
        StyleRole role;
        lv_style_prop_t prop;
    };

    TestCase cases[] = {
        {StyleRole::Card, LV_STYLE_BG_COLOR},
        {StyleRole::Dialog, LV_STYLE_BG_COLOR},
        {StyleRole::TextPrimary, LV_STYLE_TEXT_COLOR},
        {StyleRole::IconPrimary, LV_STYLE_TEXT_COLOR},
        {StyleRole::Button, LV_STYLE_BG_COLOR},
        {StyleRole::ButtonPrimary, LV_STYLE_BG_COLOR},
        {StyleRole::SeverityInfo, LV_STYLE_BORDER_COLOR},
        {StyleRole::Dropdown, LV_STYLE_BG_COLOR},
        {StyleRole::Checkbox, LV_STYLE_BG_COLOR},
        {StyleRole::Switch, LV_STYLE_BG_COLOR},
        {StyleRole::Slider, LV_STYLE_BG_COLOR},
        {StyleRole::Spinner, LV_STYLE_ARC_COLOR},
        {StyleRole::Arc, LV_STYLE_ARC_COLOR},
    };

    for (const auto& tc : cases) {
        lv_style_t* style = tm.get_style(tc.role);
        REQUIRE(style != nullptr);

        lv_style_value_t val;
        INFO("Testing StyleRole index " << static_cast<int>(tc.role));
        REQUIRE(lv_style_get_prop(style, tc.prop, &val) == LV_STYLE_RES_FOUND);
    }
}

// ============================================================================
// Phase 4: Dark/Light Mode Switching Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "ThemeManager toggles dark mode", "[theme-manager][dark-mode]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    // Explicitly set to dark mode first (may not be default if other tests ran)
    tm.set_dark_mode(true);
    REQUIRE(tm.is_dark_mode() == true);

    // Toggle to light
    tm.set_dark_mode(false);
    REQUIRE(tm.is_dark_mode() == false);

    // Toggle back
    tm.toggle_dark_mode();
    REQUIRE(tm.is_dark_mode() == true);
}

TEST_CASE_METHOD(LVGLTestFixture, "Styles update when mode changes",
                 "[theme-manager][mode-change]") {
    auto& tm = ThemeManager::instance();
    // Reset to clean state: prior tests may have called set_palettes() with
    // identical light/dark palettes, making dark/light comparison impossible
    tm.shutdown();
    tm.set_palettes(ThemePalette{}, ThemePalette{});
    tm.init();

    // Ensure we start in dark mode
    tm.set_dark_mode(true);

    lv_style_t* card = tm.get_style(StyleRole::Card);
    lv_style_value_t dark_bg;
    lv_style_get_prop(card, LV_STYLE_BG_COLOR, &dark_bg);

    tm.set_dark_mode(false);

    lv_style_value_t light_bg;
    lv_style_get_prop(card, LV_STYLE_BG_COLOR, &light_bg);

    // Colors should be different in light vs dark mode
    bool different = (dark_bg.color.red != light_bg.color.red) ||
                     (dark_bg.color.green != light_bg.color.green) ||
                     (dark_bg.color.blue != light_bg.color.blue);
    REQUIRE(different);
}

// ============================================================================
// Phase 4: Color Lookup API Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "get_color returns palette colors",
                 "[theme-manager][get-color]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_color_t primary = tm.get_color("primary");
    lv_color_t danger = tm.get_color("danger");

    // Should return actual colors (not default black or magenta error)
    REQUIRE((primary.red + primary.green + primary.blue) > 0);
    REQUIRE(danger.red > 0); // Danger should have red component

    // Unknown returns magenta
    lv_color_t unknown = tm.get_color("nonexistent");
    REQUIRE(unknown.red == 0xFF);
    REQUIRE(unknown.green == 0x00);
    REQUIRE(unknown.blue == 0xFF);
}

// ============================================================================
// Phase 4: Preview System Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "preview_palette applies temporarily",
                 "[theme-manager][preview]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    lv_style_t* card = tm.get_style(StyleRole::Card);
    lv_style_value_t original_bg;
    lv_style_get_prop(card, LV_STYLE_BG_COLOR, &original_bg);

    // Preview a different palette
    ThemePalette preview_palette = tm.current_palette();
    preview_palette.card_bg = lv_color_hex(0xFF0000); // Red

    tm.preview_palette(preview_palette);
    REQUIRE(tm.is_previewing() == true);

    lv_style_value_t preview_bg;
    lv_style_get_prop(card, LV_STYLE_BG_COLOR, &preview_bg);
    REQUIRE(preview_bg.color.red == 0xFF);

    // Cancel reverts
    tm.cancel_preview();
    REQUIRE(tm.is_previewing() == false);

    lv_style_value_t reverted_bg;
    lv_style_get_prop(card, LV_STYLE_BG_COLOR, &reverted_bg);
    REQUIRE(reverted_bg.color.red == original_bg.color.red);
}

// ============================================================================
// Contrast Text API Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "theme_manager_get_contrast_color returns light text for dark background",
                 "[theme-manager][contrast]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    // Dark background (low brightness)
    lv_color_t dark_bg = lv_color_hex(0x2E3440);
    lv_color_t text = theme_manager_get_contrast_color(dark_bg);

    // Should return light colored text (high brightness)
    int brightness = theme_compute_brightness(text);
    REQUIRE(brightness > 128);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "theme_manager_get_contrast_color returns dark text for light background",
                 "[theme-manager][contrast]") {
    auto& tm = ThemeManager::instance();
    tm.init();

    // Light background (high brightness)
    lv_color_t light_bg = lv_color_hex(0xECEFF4);
    lv_color_t text = theme_manager_get_contrast_color(light_bg);

    // Should return dark colored text (low brightness)
    int brightness = theme_compute_brightness(text);
    REQUIRE(brightness < 128);
}

// ============================================================================
// responsive_dimension() — picks the smaller screen axis so portrait layouts
// adapt to their cramped width instead of their tall height.
// ============================================================================

namespace {
// Bufferless display: responsive_dimension() only reads the resolution, never
// renders, so no draw buffer / flush_cb is needed.
lv_display_t* make_test_display(int32_t w, int32_t h) {
    return lv_display_create(w, h);
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "responsive_dimension picks the smaller screen axis",
                 "[theme][breakpoints]") {
    SECTION("landscape — min is height (unchanged behavior)") {
        lv_display_t* d = make_test_display(800, 480);
        REQUIRE(responsive_dimension(d) == 480);
        lv_display_delete(d);

        d = make_test_display(1024, 600);
        REQUIRE(responsive_dimension(d) == 600);
        lv_display_delete(d);
    }

    SECTION("portrait — min is width (the fix)") {
        lv_display_t* d = make_test_display(480, 800);
        REQUIRE(responsive_dimension(d) == 480); // was 800 (ver_res) before
        lv_display_delete(d);

        d = make_test_display(320, 480);
        REQUIRE(responsive_dimension(d) == 320);
        lv_display_delete(d);

        d = make_test_display(272, 480);
        REQUIRE(responsive_dimension(d) == 272);
        lv_display_delete(d);
    }

    SECTION("square — either axis") {
        lv_display_t* d = make_test_display(480, 480);
        REQUIRE(responsive_dimension(d) == 480);
        lv_display_delete(d);
    }

    SECTION("null display falls back to the default display") {
        // lv_display_get_default() returns the first display created — here the
        // fixture's TEST_DISPLAY_WIDTH x TEST_DISPLAY_HEIGHT screen — so the
        // fallback resolves to its smaller axis, not any later-created display.
        REQUIRE(responsive_dimension(nullptr) == std::min(TEST_DISPLAY_WIDTH, TEST_DISPLAY_HEIGHT));
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "responsive_dimension feeds breakpoint selection — portrait no longer overflows",
                 "[theme][breakpoints]") {
    // End-to-end: a 480x800 portrait screen used to resolve to _xlarge (via
    // ver_res=800) and overflow; it now resolves to _medium via the 480 width.
    lv_display_t* d = make_test_display(480, 800);
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(responsive_dimension(d))) == "_medium");
    lv_display_delete(d);

    // 320x480 portrait resolves to _tiny instead of _medium.
    d = make_test_display(320, 480);
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(responsive_dimension(d))) == "_tiny");
    lv_display_delete(d);

    // Landscape 800x480 is byte-identical to the old behavior — still _medium.
    d = make_test_display(800, 480);
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(responsive_dimension(d))) == "_medium");
    lv_display_delete(d);
}
