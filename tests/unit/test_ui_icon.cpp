// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_icon.cpp
 * @brief Unit tests for ui_icon.cpp - Icon widget with size, variant, and custom color support
 *
 * Tests cover:
 * - Size parsing (xs/sm/md/lg/xl) with valid and invalid values
 * - Variant parsing (primary/secondary/accent/disabled/success/warning/error/none)
 * - Public API functions (set_source, set_size, set_variant, set_color)
 * - Error handling (NULL pointers, invalid strings)
 *
 * The implementation uses:
 * - IconSize enum (XS, SM, MD, LG, XL) - not a struct
 * - IconVariant enum (NONE, PRIMARY, SECONDARY, ACCENT, DISABLED, SUCCESS, WARNING, ERROR)
 * - Static internal functions (parse_size, parse_variant, apply_size, apply_variant)
 * - Public API uses the internal enums internally
 */

#include "ui_fonts.h"

#include "../../include/ui_icon.h"
#include "../../include/ui_icon_codepoints.h"
#include "../lvgl_test_fixture.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

#include "../catch_amalgamated.hpp"

// Test fixture for icon tests - manages spdlog level
class IconTest {
  public:
    IconTest() {
        spdlog::set_level(spdlog::level::debug);
    }
    ~IconTest() {
        spdlog::set_level(spdlog::level::warn);
    }
};

// Public API: NULL pointer handling

TEST_CASE("helix::ui::icon::set_source handles NULL icon", "[ui_icon][api][error]") {
    IconTest fixture;

    // Should log error and return without crashing
    REQUIRE_NOTHROW(helix::ui::icon::set_source(nullptr, "home"));
}

TEST_CASE("helix::ui::icon::set_source handles NULL icon_name", "[ui_icon][api][error]") {
    IconTest fixture;

    // Should log error and return without crashing
    // Note: Using dummy pointer - function should check for NULL before dereferencing
    REQUIRE_NOTHROW(helix::ui::icon::set_source(reinterpret_cast<lv_obj_t*>(0x1), nullptr));
}

TEST_CASE("helix::ui::icon::set_size handles NULL icon", "[ui_icon][api][error]") {
    IconTest fixture;

    REQUIRE_NOTHROW(helix::ui::icon::set_size(nullptr, "md"));
}

TEST_CASE("helix::ui::icon::set_size handles NULL size_str", "[ui_icon][api][error]") {
    IconTest fixture;

    REQUIRE_NOTHROW(helix::ui::icon::set_size(reinterpret_cast<lv_obj_t*>(0x1), nullptr));
}

TEST_CASE("helix::ui::icon::set_variant handles NULL icon", "[ui_icon][api][error]") {
    IconTest fixture;

    REQUIRE_NOTHROW(helix::ui::icon::set_variant(nullptr, "primary"));
}

TEST_CASE("helix::ui::icon::set_variant handles NULL variant_str", "[ui_icon][api][error]") {
    IconTest fixture;

    REQUIRE_NOTHROW(helix::ui::icon::set_variant(reinterpret_cast<lv_obj_t*>(0x1), nullptr));
}

TEST_CASE("helix::ui::icon::set_color handles NULL icon", "[ui_icon][api][error]") {
    IconTest fixture;

    lv_color_t color = lv_color_hex(0xFF0000);
    REQUIRE_NOTHROW(helix::ui::icon::set_color(nullptr, color, LV_OPA_COVER));
}

// Icon Codepoint Lookup

TEST_CASE("Icon codepoint lookup returns valid codepoints", "[ui_icon][codepoint]") {
    IconTest fixture;

    // Test common icons
    const char* home = helix::ui::icon::lookup_codepoint("home");
    REQUIRE(home != nullptr);

    const char* wifi = helix::ui::icon::lookup_codepoint("wifi");
    REQUIRE(wifi != nullptr);

    const char* settings = helix::ui::icon::lookup_codepoint("cog");
    REQUIRE(settings != nullptr);
}

TEST_CASE("Icon codepoint lookup returns nullptr for unknown icons", "[ui_icon][codepoint]") {
    IconTest fixture;

    const char* unknown = helix::ui::icon::lookup_codepoint("nonexistent_icon_xyz");
    REQUIRE(unknown == nullptr);
}

TEST_CASE("Icon codepoint lookup handles NULL", "[ui_icon][codepoint][error]") {
    IconTest fixture;

    const char* result = helix::ui::icon::lookup_codepoint(nullptr);
    REQUIRE(result == nullptr);
}

TEST_CASE("Icon codepoint lookup handles empty string", "[ui_icon][codepoint][error]") {
    IconTest fixture;

    const char* result = helix::ui::icon::lookup_codepoint("");
    REQUIRE(result == nullptr);
}

// The Spoolman logo is not an MDI glyph: it comes from helix-custom-icons.ttf,
// merged into the icon faces by regen_mdi_fonts.sh. A face regenerated without
// that font still compiles, and the icon then draws nothing at all.
TEST_CASE("Spoolman icon resolves to a custom glyph present in every icon face",
          "[ui_icon][codepoint]") {
    const char* utf8 = helix::ui::icon::lookup_codepoint("spoolman");
    REQUIRE(utf8 != nullptr);
    REQUIRE(std::strlen(utf8) == 4);
    const auto* b = reinterpret_cast<const unsigned char*>(utf8);
    const uint32_t cp =
        ((b[0] & 0x07u) << 18) | ((b[1] & 0x3Fu) << 12) | ((b[2] & 0x3Fu) << 6) | (b[3] & 0x3Fu);
    REQUIRE(cp == 0xFE000);

    const lv_font_t* faces[] = {&mdi_icons_14, &mdi_icons_16, &mdi_icons_24,
                                &mdi_icons_32, &mdi_icons_48, &mdi_icons_64,
                                &mdi_icons_80, &mdi_icons_96, &mdi_icons_128};
    for (const lv_font_t* face : faces) {
        lv_font_glyph_dsc_t dsc{};
        INFO("face line height " << face->line_height);
        REQUIRE(lv_font_get_glyph_dsc(face, &dsc, cp, 0));
        CHECK(dsc.box_w > 0);
        CHECK(dsc.box_h > 0);
    }
}

// Legacy Prefix Stripping

TEST_CASE("strip_legacy_prefix removes mat_ prefix", "[ui_icon][legacy]") {
    IconTest fixture;

    const char* result = helix::ui::icon::strip_legacy_prefix("mat_home");
    REQUIRE(strcmp(result, "home") == 0);
}

TEST_CASE("strip_legacy_prefix does NOT strip _img suffix without mat_ prefix",
          "[ui_icon][legacy]") {
    IconTest fixture;

    // The implementation ONLY handles names starting with "mat_"
    // A plain "_img" suffix without "mat_" prefix is NOT stripped
    const char* result = helix::ui::icon::strip_legacy_prefix("home_img");
    REQUIRE(strcmp(result, "home_img") == 0); // Returns original, unchanged
}

TEST_CASE("strip_legacy_prefix removes both prefix and suffix", "[ui_icon][legacy]") {
    IconTest fixture;

    const char* result = helix::ui::icon::strip_legacy_prefix("mat_wifi_img");
    REQUIRE(strcmp(result, "wifi") == 0);
}

TEST_CASE("strip_legacy_prefix returns original if no prefix/suffix", "[ui_icon][legacy]") {
    IconTest fixture;

    const char* result = helix::ui::icon::strip_legacy_prefix("wifi");
    REQUIRE(strcmp(result, "wifi") == 0);
}

TEST_CASE("strip_legacy_prefix handles NULL", "[ui_icon][legacy][error]") {
    IconTest fixture;

    const char* result = helix::ui::icon::strip_legacy_prefix(nullptr);
    REQUIRE(result == nullptr);
}

TEST_CASE("strip_legacy_prefix handles empty string", "[ui_icon][legacy][error]") {
    IconTest fixture;

    const char* result = helix::ui::icon::strip_legacy_prefix("");
    REQUIRE(result != nullptr);
    REQUIRE(strlen(result) == 0);
}

// Reactive icon tests: icon widgets update their color when the theme changes.
// The implementation uses shared styles from ThemeManager which update in-place
// when the theme mode changes.

#include "../lvgl_ui_test_fixture.h"
#include "theme_manager.h"

// Helper: Create a dark mode test palette with distinct colors
static ThemePalette make_dark_test_palette() {
    ThemePalette p = {};
    p.screen_bg = lv_color_hex(0x121212);
    p.overlay_bg = lv_color_hex(0x1A1A1A);
    p.card_bg = lv_color_hex(0x1E1E1E);
    p.elevated_bg = lv_color_hex(0x2D2D2D);
    p.border = lv_color_hex(0x424242);
    p.text = lv_color_hex(0xE0E0E0);
    p.text_muted = lv_color_hex(0xA0A0A0);
    p.text_subtle = lv_color_hex(0x808080);
    p.primary = lv_color_hex(0x2196F3);
    p.secondary = lv_color_hex(0x03DAC6);
    p.tertiary = lv_color_hex(0x6C757D);
    p.info = lv_color_hex(0x42A5F5);
    p.success = lv_color_hex(0x4CAF50);
    p.warning = lv_color_hex(0xFFA726);
    p.danger = lv_color_hex(0xEF5350);
    p.focus = lv_color_hex(0x4FC3F7);
    return p;
}

// Helper: Create a dark mode test palette with configurable primary color
static ThemePalette make_dark_test_palette_with_primary(lv_color_t primary) {
    ThemePalette p = make_dark_test_palette();
    p.primary = primary;
    return p;
}

// Semantic variant names

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: accepts 'text' variant name (new name for 'primary')",
                 "[reactive-icon][variant-names]") {
    // Create icon widget via XML with new variant name
    const char* attrs[] = {"src", "home", "variant", "text", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    // Get the icon's text color - should match icon_text_style
    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    // Get expected color from the shared style
    lv_style_t* text_style = ThemeManager::instance().get_style(StyleRole::IconText);
    REQUIRE(text_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(text_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    // Icon should have the text style's color
    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ui_icon: accepts 'muted' variant name (new name for 'secondary')",
                 "[reactive-icon][variant-names]") {
    const char* attrs[] = {"src", "home", "variant", "muted", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    lv_style_t* muted_style = ThemeManager::instance().get_style(StyleRole::TextMuted);
    REQUIRE(muted_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(muted_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ui_icon: accepts 'primary' variant name (new name for 'accent')",
                 "[reactive-icon][variant-names]") {
    // "primary" is the accent/brand color
    const char* attrs[] = {"src", "home", "variant", "primary", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    lv_style_t* primary_style = ThemeManager::instance().get_style(StyleRole::IconPrimary);
    REQUIRE(primary_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(primary_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: accepts 'danger' variant name (new name for 'error')",
                 "[reactive-icon][variant-names]") {
    const char* attrs[] = {"src", "home", "variant", "danger", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    lv_style_t* danger_style = ThemeManager::instance().get_style(StyleRole::IconDanger);
    REQUIRE(danger_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(danger_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

// The secondary variant uses the secondary accent color style

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: accepts 'secondary' variant for secondary accent",
                 "[reactive-icon][variant-names]") {
    const char* attrs[] = {"src", "home", "variant", "secondary", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    lv_style_t* secondary_style = ThemeManager::instance().get_style(StyleRole::IconSecondary);
    REQUIRE(secondary_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(secondary_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

// Reactive theme change: icons update when the theme changes

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: text variant color updates on theme change",
                 "[reactive-icon][reactive]") {
    // Create icon with text variant
    const char* attrs[] = {"src", "home", "variant", "text", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t before = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    uint32_t before_rgb = lv_color_to_u32(before) & 0x00FFFFFF;
    INFO("Initial icon text color: 0x" << std::hex << before_rgb);

    // Update theme colors to dark mode (significantly different colors)
    ThemePalette dark_palette = make_dark_test_palette();
    auto& tm = ThemeManager::instance();
    tm.set_palettes(dark_palette, dark_palette);
    tm.set_dark_mode(true);

    lv_obj_report_style_change(nullptr);

    lv_color_t after = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    uint32_t after_rgb = lv_color_to_u32(after) & 0x00FFFFFF;
    INFO("After theme change icon text color: 0x" << std::hex << after_rgb);

    // Icon color should change (light mode dark text -> dark mode light text)
    REQUIRE_FALSE(lv_color_eq(before, after));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: muted variant color updates on theme change",
                 "[reactive-icon][reactive]") {
    const char* attrs[] = {"src", "home", "variant", "muted", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t before = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    // Update to dark mode
    ThemePalette dark_palette = make_dark_test_palette();
    auto& tm = ThemeManager::instance();
    tm.set_palettes(dark_palette, dark_palette);
    tm.set_dark_mode(true);

    lv_obj_report_style_change(nullptr);

    lv_color_t after = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    REQUIRE_FALSE(lv_color_eq(before, after));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: primary variant color updates on theme change",
                 "[reactive-icon][reactive]") {
    const char* attrs[] = {"src", "home", "variant", "primary", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t before = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    // Update to dark mode with DIFFERENT primary color
    ThemePalette dark_palette = make_dark_test_palette_with_primary(lv_color_hex(0xFF5722));
    auto& tm = ThemeManager::instance();
    tm.set_palettes(dark_palette, dark_palette);
    tm.set_dark_mode(true);

    lv_obj_report_style_change(nullptr);

    lv_color_t after = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    // Primary color changed, so icon should too
    REQUIRE_FALSE(lv_color_eq(before, after));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: success variant color matches shared style",
                 "[reactive-icon][semantic]") {
    const char* attrs[] = {"src", "home", "variant", "success", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    lv_style_t* success_style = ThemeManager::instance().get_style(StyleRole::IconSuccess);
    REQUIRE(success_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(success_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: warning variant color matches shared style",
                 "[reactive-icon][semantic]") {
    const char* attrs[] = {"src", "home", "variant", "warning", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    lv_style_t* warning_style = ThemeManager::instance().get_style(StyleRole::IconWarning);
    REQUIRE(warning_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(warning_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: danger variant color matches shared style",
                 "[reactive-icon][semantic]") {
    const char* attrs[] = {"src", "home", "variant", "danger", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    lv_style_t* danger_style = ThemeManager::instance().get_style(StyleRole::IconDanger);
    REQUIRE(danger_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(danger_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: none variant uses text style",
                 "[reactive-icon][semantic]") {
    const char* attrs[] = {"src", "home", "variant", "none", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    // none variant should use text style (same as default)
    lv_style_t* text_style = ThemeManager::instance().get_style(StyleRole::IconText);
    REQUIRE(text_style != nullptr);
    lv_style_value_t value;
    lv_style_res_t res = lv_style_get_prop(text_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    REQUIRE(lv_color_eq(icon_color, value.color));

    lv_obj_delete(icon);
}

// Multiple icons update together

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: multiple icons update together on theme change",
                 "[reactive-icon][reactive]") {
    // Create multiple icons with same variant
    const char* attrs[] = {"src", "home", "variant", "text", nullptr};
    lv_obj_t* icon1 = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    lv_obj_t* icon2 = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    lv_obj_t* icon3 = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));

    REQUIRE(icon1 != nullptr);
    REQUIRE(icon2 != nullptr);
    REQUIRE(icon3 != nullptr);

    lv_color_t before1 = lv_obj_get_style_text_color(icon1, LV_PART_MAIN);
    lv_color_t before2 = lv_obj_get_style_text_color(icon2, LV_PART_MAIN);
    lv_color_t before3 = lv_obj_get_style_text_color(icon3, LV_PART_MAIN);

    // All icons should have the same initial color
    REQUIRE(lv_color_eq(before1, before2));
    REQUIRE(lv_color_eq(before2, before3));

    // Update to dark mode
    ThemePalette dark_palette = make_dark_test_palette();
    auto& tm = ThemeManager::instance();
    tm.set_palettes(dark_palette, dark_palette);
    tm.set_dark_mode(true);

    lv_obj_report_style_change(nullptr);

    lv_color_t after1 = lv_obj_get_style_text_color(icon1, LV_PART_MAIN);
    lv_color_t after2 = lv_obj_get_style_text_color(icon2, LV_PART_MAIN);
    lv_color_t after3 = lv_obj_get_style_text_color(icon3, LV_PART_MAIN);

    // All icons should still have the same color (consistency)
    REQUIRE(lv_color_eq(after1, after2));
    REQUIRE(lv_color_eq(after2, after3));

    // And the color should have changed from before (reactivity)
    REQUIRE_FALSE(lv_color_eq(before1, after1));

    lv_obj_delete(icon1);
    lv_obj_delete(icon2);
    lv_obj_delete(icon3);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_icon: style matches shared style after theme change",
                 "[reactive-icon][reactive]") {
    const char* attrs[] = {"src", "home", "variant", "text", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    lv_style_t* shared_style = ThemeManager::instance().get_style(StyleRole::IconText);
    REQUIRE(shared_style != nullptr);

    // Update to dark mode
    ThemePalette dark_palette = make_dark_test_palette();
    auto& tm = ThemeManager::instance();
    tm.set_palettes(dark_palette, dark_palette);
    tm.set_dark_mode(true);

    lv_obj_report_style_change(nullptr);

    lv_style_value_t style_value;
    lv_style_res_t res = lv_style_get_prop(shared_style, LV_STYLE_TEXT_COLOR, &style_value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    lv_color_t icon_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    uint32_t style_rgb = lv_color_to_u32(style_value.color) & 0x00FFFFFF;
    uint32_t icon_rgb = lv_color_to_u32(icon_color) & 0x00FFFFFF;
    INFO("Shared style text_color: 0x" << std::hex << style_rgb);
    INFO("Icon actual text_color: 0x" << std::hex << icon_rgb);

    // The icon should have the same color as the shared style after update
    // ui_icon applies the shared style through lv_obj_add_style()
    REQUIRE(lv_color_eq(icon_color, style_value.color));

    lv_obj_delete(icon);
}

// helix::ui::icon::set_variant with semantic names

TEST_CASE_METHOD(LVGLUITestFixture, "helix::ui::icon::set_variant: accepts new variant names",
                 "[reactive-icon][api]") {
    // Create a plain icon
    const char* attrs[] = {"src", "home", nullptr};
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon != nullptr);

    // Set to 'text' variant using API
    helix::ui::icon::set_variant(icon, "text");
    lv_color_t text_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    // Should match shared text style
    lv_style_t* text_style = ThemeManager::instance().get_style(StyleRole::IconText);
    lv_style_value_t value;
    lv_style_get_prop(text_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(lv_color_eq(text_color, value.color));

    // Set to 'muted' variant using API
    helix::ui::icon::set_variant(icon, "muted");
    lv_color_t muted_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    // Should match shared muted style
    lv_style_t* muted_style = ThemeManager::instance().get_style(StyleRole::TextMuted);
    lv_style_get_prop(muted_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(lv_color_eq(muted_color, value.color));

    // Set to 'danger' variant using API
    helix::ui::icon::set_variant(icon, "danger");
    lv_color_t danger_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    // Should match shared danger style
    lv_style_t* danger_style = ThemeManager::instance().get_style(StyleRole::IconDanger);
    lv_style_get_prop(danger_style, LV_STYLE_TEXT_COLOR, &value);
    REQUIRE(lv_color_eq(danger_color, value.color));

    lv_obj_delete(icon);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a contrast icon reads against the nearest filled ancestor",
                 "[ui_icon]") {
    lv_obj_t* card = lv_obj_create(test_screen());
    lv_obj_t* clear = lv_obj_create(card);
    lv_obj_set_style_bg_opa(clear, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t* glyph = lv_label_create(clear);
    const lv_color_t text = theme_manager_get_color("text");

    const lv_color_t dark = theme_manager_get_color("screen_bg");
    lv_obj_set_style_bg_color(card, dark, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    const lv_color_t on_dark = helix::ui::icon::contrast_color(glyph);
    CHECK(lv_color_eq(on_dark, theme_manager_get_contrast_adjusted_text(text, dark)));

    // The fill changes under the icon and the answer follows it.
    const lv_color_t light = theme_manager_get_color("text");
    lv_obj_set_style_bg_color(card, light, LV_PART_MAIN);
    const lv_color_t on_light = helix::ui::icon::contrast_color(glyph);
    CHECK(lv_color_eq(on_light, theme_manager_get_contrast_adjusted_text(text, light)));
    CHECK_FALSE(lv_color_eq(on_dark, on_light));
}
