// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_spinner.cpp
 * @brief Unit tests for ui_spinner.cpp - Reactive spinner widget
 *
 * A spinner's arc reads the shared StyleRole::Spinner style rather than an
 * inline colour, so a theme change recolours every spinner in place.
 */

#include "../lvgl_ui_test_fixture.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

// Helper: Create a dark mode test palette with configurable primary color
static ThemePalette make_dark_test_palette_with_primary(lv_color_t primary) {
    ThemePalette p = {};
    p.screen_bg = lv_color_hex(0x121212);
    p.overlay_bg = lv_color_hex(0x1A1A1A);
    p.card_bg = lv_color_hex(0x1E1E1E);
    p.elevated_bg = lv_color_hex(0x2D2D2D);
    p.border = lv_color_hex(0x424242);
    p.text = lv_color_hex(0xE0E0E0);
    p.text_muted = lv_color_hex(0xA0A0A0);
    p.text_subtle = lv_color_hex(0x808080);
    p.primary = primary;
    p.secondary = lv_color_hex(0x03DAC6);
    p.tertiary = lv_color_hex(0x6C757D);
    p.info = lv_color_hex(0x42A5F5);
    p.success = lv_color_hex(0x4CAF50);
    p.warning = lv_color_hex(0xFFA726);
    p.danger = lv_color_hex(0xEF5350);
    p.focus = lv_color_hex(0x4FC3F7);
    return p;
}

// ============================================================================
// Reactive Spinner Tests
// ============================================================================
// These tests verify that spinner widgets update their arc color when the theme
// changes. The implementation uses shared styles from ThemeManager which
// update in-place when the theme mode changes.
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "ui_spinner: style matches shared style after theme change",
                 "[reactive-spinner][reactive]") {
    // Create spinner widget via XML
    const char* attrs[] = {"size", "sm", nullptr};
    lv_obj_t* spinner = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spinner", attrs));
    REQUIRE(spinner != nullptr);

    // Get the shared spinner style
    lv_style_t* shared_style = ThemeManager::instance().get_style(StyleRole::Spinner);
    REQUIRE(shared_style != nullptr);

    // Update to dark mode with different primary color (purple)
    ThemePalette palette = make_dark_test_palette_with_primary(lv_color_hex(0x9C27B0));
    auto& tm = ThemeManager::instance();
    tm.set_palettes(palette, palette);
    tm.set_dark_mode(true);

    lv_obj_report_style_change(nullptr);

    // Get the updated color from the shared style
    lv_style_value_t style_value;
    lv_style_res_t res = lv_style_get_prop(shared_style, LV_STYLE_ARC_COLOR, &style_value);
    REQUIRE(res == LV_STYLE_RES_FOUND);

    // Get the actual color from the spinner widget
    lv_color_t spinner_color = lv_obj_get_style_arc_color(spinner, LV_PART_INDICATOR);

    // Log for debugging
    uint32_t style_rgb = lv_color_to_u32(style_value.color) & 0x00FFFFFF;
    uint32_t spinner_rgb = lv_color_to_u32(spinner_color) & 0x00FFFFFF;
    INFO("Shared style arc_color: 0x" << std::hex << style_rgb);
    INFO("Spinner actual arc_color: 0x" << std::hex << spinner_rgb);

    // The spinner should have the same color as the shared style after update
    // This verifies the spinner is actually using the shared style
    REQUIRE(lv_color_eq(spinner_color, style_value.color));

    lv_obj_delete(spinner);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_spinner: multiple spinners update together on theme change",
                 "[reactive-spinner][reactive]") {
    // Create multiple spinners with different sizes
    const char* attrs_sm[] = {"size", "sm", nullptr};
    const char* attrs_md[] = {"size", "md", nullptr};
    const char* attrs_lg[] = {"size", "lg", nullptr};

    lv_obj_t* spinner1 = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spinner", attrs_sm));
    lv_obj_t* spinner2 = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spinner", attrs_md));
    lv_obj_t* spinner3 = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spinner", attrs_lg));

    REQUIRE(spinner1 != nullptr);
    REQUIRE(spinner2 != nullptr);
    REQUIRE(spinner3 != nullptr);

    // Get initial colors - all should be the same (same primary color)
    lv_color_t before1 = lv_obj_get_style_arc_color(spinner1, LV_PART_INDICATOR);
    lv_color_t before2 = lv_obj_get_style_arc_color(spinner2, LV_PART_INDICATOR);
    lv_color_t before3 = lv_obj_get_style_arc_color(spinner3, LV_PART_INDICATOR);

    // All spinners should have the same initial color
    REQUIRE(lv_color_eq(before1, before2));
    REQUIRE(lv_color_eq(before2, before3));

    // Update to dark mode with different primary color (cyan)
    ThemePalette palette = make_dark_test_palette_with_primary(lv_color_hex(0x00BCD4));
    auto& tm = ThemeManager::instance();
    tm.set_palettes(palette, palette);
    tm.set_dark_mode(true);

    lv_obj_report_style_change(nullptr);

    // Get colors after theme change
    lv_color_t after1 = lv_obj_get_style_arc_color(spinner1, LV_PART_INDICATOR);
    lv_color_t after2 = lv_obj_get_style_arc_color(spinner2, LV_PART_INDICATOR);
    lv_color_t after3 = lv_obj_get_style_arc_color(spinner3, LV_PART_INDICATOR);

    // All spinners should still have the same color (consistency)
    REQUIRE(lv_color_eq(after1, after2));
    REQUIRE(lv_color_eq(after2, after3));

    // And the color should have changed from before (reactivity)
    REQUIRE_FALSE(lv_color_eq(before1, after1));

    lv_obj_delete(spinner1);
    lv_obj_delete(spinner2);
    lv_obj_delete(spinner3);
}
