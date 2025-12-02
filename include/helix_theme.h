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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize HelixScreen custom theme
 *
 * Creates a wrapper theme that delegates to LVGL default theme but overrides
 * input widget backgrounds to use a different color than cards. This gives
 * input widgets (textarea, dropdown) visual distinction from card backgrounds.
 *
 * Color computation:
 * - Dark mode: Input bg = card bg + (22, 23, 27) RGB offset (lighter)
 * - Light mode: Input bg = card bg - (22, 23, 27) RGB offset (darker)
 *
 * The theme reads all colors from globals.xml via lv_xml_get_const(), ensuring
 * no hardcoded colors in C code.
 *
 * @param display LVGL display to apply theme to
 * @param primary_color Primary theme color (from globals.xml)
 * @param secondary_color Secondary theme color (from globals.xml)
 * @param text_primary_color Primary text color for buttons/labels (theme-aware)
 * @param is_dark Dark mode flag (true = dark mode)
 * @param base_font Base font for theme
 * @param screen_bg Screen background color (from globals.xml variant)
 * @param card_bg Card background color (from globals.xml variant)
 * @param theme_grey Grey color for buttons (from globals.xml variant)
 * @param border_radius Border radius for buttons/cards (from globals.xml)
 * @return Initialized theme, or NULL on failure
 *
 * Example usage:
 * @code
 *   lv_color_t primary = ui_theme_parse_color("#FF4444");
 *   lv_color_t screen_bg = ui_theme_get_color("app_bg_color");
 *   int32_t border_radius = atoi(lv_xml_get_const(NULL, "border_radius"));
 *   lv_theme_t* theme = helix_theme_init(
 *       display, primary, secondary, true, font, screen_bg, card_bg, grey, border_radius
 *   );
 *   lv_display_set_theme(display, theme);
 * @endcode
 */
lv_theme_t* helix_theme_init(lv_display_t* display, lv_color_t primary_color,
                             lv_color_t secondary_color, lv_color_t text_primary_color,
                             bool is_dark, const lv_font_t* base_font, lv_color_t screen_bg,
                             lv_color_t card_bg, lv_color_t theme_grey, int32_t border_radius);

/**
 * @brief Update theme colors in-place without recreating the theme
 *
 * Updates all theme style objects with new colors for runtime dark/light mode
 * switching. This modifies existing styles and calls lv_obj_report_style_change()
 * to trigger LVGL's style refresh cascade.
 *
 * Unlike helix_theme_init(), this function preserves widget state and avoids
 * the overhead of theme recreation.
 *
 * @param is_dark true for dark mode colors, false for light mode
 * @param screen_bg Screen background color
 * @param card_bg Card/panel background color
 * @param theme_grey Grey color for buttons
 * @param text_primary_color Primary text color
 */
void helix_theme_update_colors(bool is_dark, lv_color_t screen_bg, lv_color_t card_bg,
                               lv_color_t theme_grey, lv_color_t text_primary_color);

#ifdef __cplusplus
}
#endif
