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

#include "helix_theme.h"

#include "lvgl/src/themes/lv_theme_private.h"

#include <stdlib.h>
#include <string.h>

// HelixScreen custom theme structure
typedef struct {
    lv_theme_t base;           // Base LVGL theme structure (MUST be first)
    lv_theme_t* default_theme; // LVGL default theme to delegate to
    lv_style_t input_bg_style; // Custom style for input widget backgrounds
    lv_style_t disabled_style; // Global disabled state style (50% opacity)
    lv_style_t pressed_style;  // Global pressed state style (preserve radius)
    lv_style_t button_style;   // Default button style (grey background)
    bool is_dark_mode;         // Track theme mode for context
} helix_theme_t;

// Static theme instance (singleton pattern matching LVGL's approach)
static helix_theme_t* helix_theme_instance = NULL;

/**
 * Compute input background color from card background
 *
 * Dark mode: Lighten card bg by +22/+23/+27 RGB offset
 * Light mode: Darken card bg by -22/-23/-27 RGB offset
 *
 * @param card_bg Card background color
 * @param is_dark Dark mode flag
 * @return Computed input background color
 */
static lv_color_t compute_input_bg_color(lv_color_t card_bg, bool is_dark) {
    uint32_t rgb = lv_color_to_u32(card_bg);
    int r = (rgb >> 16) & 0xFF;
    int g = (rgb >> 8) & 0xFF;
    int b = rgb & 0xFF;

    // Apply RGB offsets with clamping
    int r_offset = is_dark ? 22 : -22;
    int g_offset = is_dark ? 23 : -23;
    int b_offset = is_dark ? 27 : -27;

    r = (r + r_offset < 0) ? 0 : ((r + r_offset > 255) ? 255 : r + r_offset);
    g = (g + g_offset < 0) ? 0 : ((g + g_offset > 255) ? 255 : g + g_offset);
    b = (b + b_offset < 0) ? 0 : ((b + b_offset > 255) ? 255 : b + b_offset);

    return lv_color_hex((r << 16) | (g << 8) | b);
}

/**
 * Custom theme apply callback
 *
 * Delegates to LVGL default theme, then overrides input widget backgrounds
 * to use our custom computed color.
 *
 * Pattern: Apply default theme first, then selectively override specific widgets.
 */
static void helix_theme_apply(lv_theme_t* theme, lv_obj_t* obj) {
    helix_theme_t* helix = (helix_theme_t*)theme;

    // First, apply default LVGL theme to get all standard styling
    if (helix->default_theme && helix->default_theme->apply_cb) {
        helix->default_theme->apply_cb(helix->default_theme, obj);
    }

    // Apply global disabled state styling (50% opacity for all widgets)
    lv_obj_add_style(obj, &helix->disabled_style, LV_PART_MAIN | LV_STATE_DISABLED);

    // Apply button styling (grey background + radius preservation)
#if LV_USE_BUTTON
    if (lv_obj_check_type(obj, &lv_button_class)) {
        // Default button style: grey background
        lv_obj_add_style(obj, &helix->button_style, LV_PART_MAIN);
        // Preserve radius on press
        lv_obj_add_style(obj, &helix->pressed_style, LV_PART_MAIN | LV_STATE_PRESSED);
    }
#endif

    // Now override input widgets to use our custom background color
#if LV_USE_TEXTAREA
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        // Remove default card background, add our custom input background
        lv_obj_add_style(obj, &helix->input_bg_style, LV_PART_MAIN);
    }
#endif

#if LV_USE_DROPDOWN
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        // Dropdown button background
        lv_obj_add_style(obj, &helix->input_bg_style, LV_PART_MAIN);
    }
    // Dropdown list also uses input bg for consistency
    if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, &helix->input_bg_style, LV_PART_MAIN);
    }
#endif

#if LV_USE_ROLLER
    if (lv_obj_check_type(obj, &lv_roller_class)) {
        lv_obj_add_style(obj, &helix->input_bg_style, LV_PART_MAIN);
    }
#endif

#if LV_USE_SPINBOX
    if (lv_obj_check_type(obj, &lv_spinbox_class)) {
        lv_obj_add_style(obj, &helix->input_bg_style, LV_PART_MAIN);
    }
#endif
}

lv_theme_t* helix_theme_init(lv_display_t* display, lv_color_t primary_color,
                             lv_color_t secondary_color, lv_color_t text_primary_color,
                             bool is_dark, const lv_font_t* base_font, lv_color_t screen_bg,
                             lv_color_t card_bg, lv_color_t theme_grey, int32_t border_radius) {
    // Clean up previous theme instance if exists
    if (helix_theme_instance) {
        lv_style_reset(&helix_theme_instance->input_bg_style);
        lv_style_reset(&helix_theme_instance->disabled_style);
        lv_style_reset(&helix_theme_instance->pressed_style);
        lv_style_reset(&helix_theme_instance->button_style);
        free(helix_theme_instance);
        helix_theme_instance = NULL;
    }

    // Allocate new theme instance
    helix_theme_instance = (helix_theme_t*)malloc(sizeof(helix_theme_t));
    if (!helix_theme_instance) {
        return NULL;
    }
    memset(helix_theme_instance, 0, sizeof(helix_theme_t));

    // Initialize LVGL default theme (this does most of the heavy lifting)
    helix_theme_instance->default_theme =
        lv_theme_default_init(display, primary_color, secondary_color, is_dark, base_font);

    if (!helix_theme_instance->default_theme) {
        free(helix_theme_instance);
        helix_theme_instance = NULL;
        return NULL;
    }

    // Configure base theme structure
    helix_theme_instance->base.apply_cb = helix_theme_apply;
    helix_theme_instance->base.parent = NULL; // No parent theme
    helix_theme_instance->base.user_data = NULL;
    helix_theme_instance->base.disp = display;
    helix_theme_instance->base.color_primary = primary_color;
    helix_theme_instance->base.color_secondary = secondary_color;
    // Copy font settings from default theme
    helix_theme_instance->base.font_small = helix_theme_instance->default_theme->font_small;
    helix_theme_instance->base.font_normal = helix_theme_instance->default_theme->font_normal;
    helix_theme_instance->base.font_large = helix_theme_instance->default_theme->font_large;
    helix_theme_instance->base.flags = 0;
    helix_theme_instance->is_dark_mode = is_dark;

    // Compute input widget background color
    lv_color_t input_bg = compute_input_bg_color(card_bg, is_dark);

    // Initialize custom input background style
    lv_style_init(&helix_theme_instance->input_bg_style);
    lv_style_set_bg_color(&helix_theme_instance->input_bg_style, input_bg);
    lv_style_set_bg_opa(&helix_theme_instance->input_bg_style, LV_OPA_COVER);

    // Initialize global disabled state style (50% opacity)
    lv_style_init(&helix_theme_instance->disabled_style);
    lv_style_set_opa(&helix_theme_instance->disabled_style, LV_OPA_50);

    // Initialize global pressed state style (preserve radius for buttons)
    lv_style_init(&helix_theme_instance->pressed_style);
    lv_style_set_radius(&helix_theme_instance->pressed_style, border_radius);

    // Initialize default button style (grey background with border radius, no shadow, theme-aware
    // text color)
    lv_style_init(&helix_theme_instance->button_style);
    lv_style_set_bg_color(&helix_theme_instance->button_style, theme_grey);
    lv_style_set_bg_opa(&helix_theme_instance->button_style, LV_OPA_COVER);
    lv_style_set_radius(&helix_theme_instance->button_style, border_radius);
    lv_style_set_shadow_width(&helix_theme_instance->button_style, 0);
    lv_style_set_text_color(&helix_theme_instance->button_style, text_primary_color);

    // CRITICAL: Now we need to patch the default theme's color fields
    // This is necessary because LVGL's default theme bakes colors into pre-computed
    // styles during init. We must update both the theme color fields AND the styles.
    // This mirrors the approach in ui_theme_patch_colors() but is cleaner since
    // we control the theme lifecycle.

    // Access internal default theme structure to patch colors
    // NOTE: This uses LVGL private API - may need updates when upgrading LVGL
    typedef enum {
        DISP_SMALL = 0,
        DISP_MEDIUM = 1,
        DISP_LARGE = 2,
    } disp_size_t;

    typedef struct {
        lv_style_t scr;
        lv_style_t scrollbar;
        lv_style_t scrollbar_scrolled;
        lv_style_t card;
        lv_style_t btn;
        // We only need to access card style, but include others for alignment
        // Full structure defined in lv_theme_default.c
    } theme_styles_partial_t;

    typedef struct {
        lv_theme_t base;
        disp_size_t disp_size;
        int32_t disp_dpi;
        lv_color_t color_scr;
        lv_color_t color_text;
        lv_color_t color_card;
        lv_color_t color_grey;
        bool inited;
        theme_styles_partial_t styles; // Partial - only what we need to access
    } default_theme_t;

    default_theme_t* def_theme = (default_theme_t*)helix_theme_instance->default_theme;

    // Update theme color fields
    def_theme->color_scr = screen_bg;
    def_theme->color_card = card_bg;
    def_theme->color_grey = theme_grey;

    // Update pre-computed style colors
    // Screen background
    lv_style_set_bg_color(&def_theme->styles.scr, screen_bg);

    // Card backgrounds (multiple styles use this)
    lv_style_set_bg_color(&def_theme->styles.card, card_bg);

    // Button background and radius
    lv_style_set_bg_color(&def_theme->styles.btn, theme_grey);
    lv_style_set_radius(&def_theme->styles.btn, border_radius);

    return (lv_theme_t*)helix_theme_instance;
}

void helix_theme_update_colors(bool is_dark, lv_color_t screen_bg, lv_color_t card_bg,
                               lv_color_t theme_grey, lv_color_t text_primary_color) {
    if (!helix_theme_instance) {
        return;
    }

    // Update our custom styles in-place
    helix_theme_instance->is_dark_mode = is_dark;

    // Recompute input widget background color
    lv_color_t input_bg = compute_input_bg_color(card_bg, is_dark);
    lv_style_set_bg_color(&helix_theme_instance->input_bg_style, input_bg);

    // Update button style colors
    lv_style_set_bg_color(&helix_theme_instance->button_style, theme_grey);
    lv_style_set_text_color(&helix_theme_instance->button_style, text_primary_color);

    // Update LVGL default theme's internal styles
    // This is the same private API access pattern used in helix_theme_init
    typedef enum {
        DISP_SMALL = 0,
        DISP_MEDIUM = 1,
        DISP_LARGE = 2,
    } disp_size_t;

    typedef struct {
        lv_style_t scr;
        lv_style_t scrollbar;
        lv_style_t scrollbar_scrolled;
        lv_style_t card;
        lv_style_t btn;
    } theme_styles_partial_t;

    typedef struct {
        lv_theme_t base;
        disp_size_t disp_size;
        int32_t disp_dpi;
        lv_color_t color_scr;
        lv_color_t color_text;
        lv_color_t color_card;
        lv_color_t color_grey;
        bool inited;
        theme_styles_partial_t styles;
    } default_theme_t;

    default_theme_t* def_theme = (default_theme_t*)helix_theme_instance->default_theme;

    // Update theme color fields
    def_theme->color_scr = screen_bg;
    def_theme->color_card = card_bg;
    def_theme->color_grey = theme_grey;
    def_theme->color_text = text_primary_color;

    // Update pre-computed style colors
    lv_style_set_bg_color(&def_theme->styles.scr, screen_bg);
    lv_style_set_text_color(&def_theme->styles.scr, text_primary_color);
    lv_style_set_bg_color(&def_theme->styles.card, card_bg);
    lv_style_set_bg_color(&def_theme->styles.btn, theme_grey);

    // Notify LVGL that all styles have changed - triggers refresh cascade
    // NULL means "all styles changed", which forces a complete style recalculation
    lv_obj_report_style_change(NULL);
}
