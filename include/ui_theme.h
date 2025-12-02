// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "lvgl/lvgl.h"

// Theme colors - only essential colors used by code that needs explicit control
// (Most colors provided automatically by LVGL theme system)
#define UI_COLOR_PRIMARY lv_color_hex(0xFF4444)   // rgb(255, 68, 68) - Primary/Active color (red)
#define UI_COLOR_SECONDARY lv_color_hex(0x00AAFF) // rgb(0, 170, 255) - Secondary accent (blue)

// Text colors
#define UI_COLOR_TEXT_PRIMARY lv_color_hex(0xFFFFFF)   // rgb(255, 255, 255) - White text
#define UI_COLOR_TEXT_SECONDARY lv_color_hex(0xAAAAAA) // rgb(170, 170, 170) - Gray text

// Layout constants
#define UI_NAV_WIDTH_PERCENT 10 // Nav bar is 1/10th of screen width
#define UI_NAV_ICON_SIZE 64     // Base icon size for 1024x800
#define UI_NAV_PADDING 16       // Padding between elements

// Calculate nav width based on actual screen
#define UI_NAV_WIDTH(screen_w) ((screen_w) / 10)

// Responsive breakpoints (based on max(width, height))
// Optimized for our target hardware: 480x320, 800x480, 1024x600, 1280x720
#define UI_BREAKPOINT_SMALL_MAX 480 // 480x320 and below → SMALL
#define UI_BREAKPOINT_MEDIUM_MAX                                                                   \
    800 // 481-800: 800x480, up to 800x600 → MEDIUM
        // >800: 1024x600, 1280x720+ → LARGE

// Screen size targets (reference only, use breakpoints above for logic)
#define UI_SCREEN_LARGE_W 1280
#define UI_SCREEN_LARGE_H 720
#define UI_SCREEN_MEDIUM_W 1024
#define UI_SCREEN_MEDIUM_H 600
#define UI_SCREEN_SMALL_W 800
#define UI_SCREEN_SMALL_H 480
#define UI_SCREEN_TINY_W 480
#define UI_SCREEN_TINY_H 320

// Padding constants (matching globals.xml values)
#define UI_PADDING_NORMAL 20 // padding_normal - standard padding
#define UI_PADDING_MEDIUM 12 // padding_medium - buttons, flex items
#define UI_PADDING_SMALL 10  // padding_small - compact layouts
#define UI_PADDING_TINY 6    // padding_tiny - very small screens

// Opacity constants (matching globals.xml values)
#define UI_DISABLED_OPA 128 // disabled_opa - 50% opacity for disabled/dimmed elements

// Responsive navigation bar width (applied in C++ based on screen size)
#define UI_NAV_WIDTH_TINY 64   // Tiny screens: 42px button + margins
#define UI_NAV_WIDTH_SMALL 76  // Small screens: 60px button + 8px padding each side
#define UI_NAV_WIDTH_MEDIUM 94 // Medium screens: 70px button + 12px padding each side
#define UI_NAV_WIDTH_LARGE 102 // Large screens: 70px button + 16px padding each side

// Font references (extern declarations for fonts used in C++ code)
extern const lv_font_t lv_font_montserrat_10;
extern const lv_font_t lv_font_montserrat_12;
extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_16;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t lv_font_montserrat_28;

// Semantic font constants (matching globals.xml)
#define UI_FONT_HEADING (&lv_font_montserrat_20) // Section headings, large text
#define UI_FONT_BODY (&lv_font_montserrat_16)    // Standard body text, medium text
#define UI_FONT_SMALL                                                                              \
    (&lv_font_montserrat_12) // Small text (hints, helpers, warnings, chart labels)

/**
 * @brief Initialize LVGL theme system
 *
 * Creates and applies LVGL theme with light or dark mode.
 * Must be called before creating any widgets.
 *
 * @param display LVGL display instance
 * @param use_dark_mode true for dark theme, false for light theme
 */
void ui_theme_init(lv_display_t* display, bool use_dark_mode);

/**
 * @brief Register responsive padding styles
 *
 * Creates padding styles that adapt based on screen size breakpoints.
 * Should be called after ui_theme_init().
 *
 * @param display LVGL display instance
 */
void ui_theme_register_responsive_padding(lv_display_t* display);

/**
 * @brief Toggle between light and dark themes
 *
 * Switches theme mode, re-registers XML color constants, updates theme
 * styles in-place, and forces a widget tree refresh. All existing widgets
 * will update to the new color scheme without recreation.
 */
void ui_theme_toggle_dark_mode();

/**
 * @brief Force style refresh on widget tree
 *
 * Walks the widget tree starting from root and forces style recalculation
 * on each widget. This is called automatically by ui_theme_toggle_dark_mode()
 * but can be used independently for custom refresh scenarios.
 *
 * @param root Root widget to start refresh from (typically lv_screen_active())
 */
void ui_theme_refresh_widget_tree(lv_obj_t* root);

/**
 * @brief Check if dark mode is currently active
 *
 * @return true if dark mode enabled, false if light mode
 */
bool ui_theme_is_dark_mode();

/**
 * @brief Parse hex color string to lv_color_t
 *
 * Supports both "#RRGGBB" and "RRGGBB" formats.
 *
 * @param hex_str Hex color string (e.g., "#FF0000" or "FF0000")
 * @return LVGL color object
 */
lv_color_t ui_theme_parse_color(const char* hex_str);

/**
 * @brief Get themed color by base name
 *
 * Retrieves color from globals.xml with automatic _light/_dark
 * variant selection based on current theme mode.
 *
 * Example: base_name="card_bg" → "card_bg_light" or "card_bg_dark"
 *
 * @param base_name Base color name (without _light/_dark suffix)
 * @return Themed color for current mode
 */
lv_color_t ui_theme_get_color(const char* base_name);

/**
 * @brief Apply themed background color to widget
 *
 * Sets widget background color using theme-aware color lookup.
 * Automatically selects _light or _dark variant.
 *
 * @param obj Widget to style
 * @param base_name Base color name (without _light/_dark suffix)
 * @param part Widget part to style (default: LV_PART_MAIN)
 */
void ui_theme_apply_bg_color(lv_obj_t* obj, const char* base_name, lv_part_t part = LV_PART_MAIN);

/**
 * @brief Get font height in pixels
 *
 * Returns the line height of the font, useful for layout calculations.
 *
 * @param font LVGL font pointer
 * @return Font height in pixels
 */
int32_t ui_theme_get_font_height(const lv_font_t* font);

/**
 * @brief Set overlay widget width to fill space after nav bar
 *
 * Utility for overlay panels/widgets that use x="#nav_width" positioning.
 * Sets width to (screen_width - nav_width).
 *
 * @param obj Widget to resize (typically an overlay panel or detail view)
 * @param screen Parent screen to calculate width from
 */
void ui_set_overlay_width(lv_obj_t* obj, lv_obj_t* screen);
