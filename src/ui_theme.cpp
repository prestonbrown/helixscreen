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

#include "ui_theme.h"
#include "helix_theme.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/xml/lv_xml.h"
#include <spdlog/spdlog.h>
#include <cstdlib>

static lv_theme_t* current_theme = nullptr;
static bool use_dark_mode = true;
static lv_display_t* theme_display = nullptr;

// Parse hex color string "#FF4444" -> lv_color_hex(0xFF4444)
lv_color_t ui_theme_parse_color(const char* hex_str) {
    if (!hex_str || hex_str[0] != '#') {
        spdlog::error("[Theme] Invalid hex color string: {}", hex_str ? hex_str : "NULL");
        return lv_color_hex(0x000000);
    }
    uint32_t hex = strtoul(hex_str + 1, NULL, 16);
    return lv_color_hex(hex);
}

// No longer needed - helix_theme.c handles all color patching and input widget styling

void ui_theme_register_responsive_padding(lv_display_t* display) {
    // Use custom breakpoints optimized for our hardware: max(hor_res, ver_res)
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    // Determine size suffix for variant lookup (using centralized breakpoints)
    const char* size_suffix;
    const char* size_label;

    if (greater_res <= UI_BREAKPOINT_SMALL_MAX) {  // ≤480: 480x320
        size_suffix = "_small";
        size_label = "SMALL";
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) {  // 481-800: 800x480, up to 800x600
        size_suffix = "_medium";
        size_label = "MEDIUM";
    } else {  // >800: 1024x600, 1280x720+
        size_suffix = "_large";
        size_label = "LARGE";
    }

    // Read size-specific variants from XML
    char variant_name[64];

    snprintf(variant_name, sizeof(variant_name), "padding_normal%s", size_suffix);
    const char* padding_normal = lv_xml_get_const(NULL, variant_name);

    snprintf(variant_name, sizeof(variant_name), "padding_small%s", size_suffix);
    const char* padding_small = lv_xml_get_const(NULL, variant_name);

    snprintf(variant_name, sizeof(variant_name), "padding_tiny%s", size_suffix);
    const char* padding_tiny = lv_xml_get_const(NULL, variant_name);

    snprintf(variant_name, sizeof(variant_name), "gap_normal%s", size_suffix);
    const char* gap_normal = lv_xml_get_const(NULL, variant_name);

    // Validate that variants were found
    if (!padding_normal || !padding_small || !padding_tiny || !gap_normal) {
        spdlog::error("[Theme] Failed to read padding variants for size: {} (normal={}, small={}, tiny={}, gap={})",
                      size_label,
                      padding_normal ? "found" : "NULL",
                      padding_small ? "found" : "NULL",
                      padding_tiny ? "found" : "NULL",
                      gap_normal ? "found" : "NULL");
        return;
    }

    // Register active constants (override defaults in globals scope)
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (scope) {
        lv_xml_register_const(scope, "padding_normal", padding_normal);
        lv_xml_register_const(scope, "padding_small", padding_small);
        lv_xml_register_const(scope, "padding_tiny", padding_tiny);
        lv_xml_register_const(scope, "gap_normal", gap_normal);

        spdlog::info("[Theme] Responsive padding: {} ({}px) - normal={}, small={}, tiny={}, gap={}",
                     size_label, greater_res, padding_normal, padding_small, padding_tiny, gap_normal);
    } else {
        spdlog::warn("[Theme] Failed to get globals scope for padding constants");
    }
}

void ui_theme_init(lv_display_t* display, bool use_dark_mode_param) {
    theme_display = display;
    use_dark_mode = use_dark_mode_param;

    // Override runtime theme constants based on light/dark mode preference
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope) {
        spdlog::critical("[Theme] FATAL: Failed to get globals scope for runtime constant registration");
        std::exit(EXIT_FAILURE);
    }

    // Read light/dark color variants from XML (MUST exist - fail-fast if missing)
    const char* app_bg_light = lv_xml_get_const(NULL, "app_bg_color_light");
    const char* app_bg_dark = lv_xml_get_const(NULL, "app_bg_color_dark");
    const char* text_primary_light = lv_xml_get_const(NULL, "text_primary_light");
    const char* text_primary_dark = lv_xml_get_const(NULL, "text_primary_dark");
    const char* header_text_light = lv_xml_get_const(NULL, "header_text_light");
    const char* header_text_dark = lv_xml_get_const(NULL, "header_text_dark");

    // Validate ALL required color variants exist
    if (!app_bg_light || !app_bg_dark) {
        spdlog::critical("[Theme] FATAL: Missing app_bg_color_light/dark in globals.xml");
        std::exit(EXIT_FAILURE);
    }
    if (!text_primary_light || !text_primary_dark) {
        spdlog::critical("[Theme] FATAL: Missing text_primary_light/dark in globals.xml");
        std::exit(EXIT_FAILURE);
    }
    if (!header_text_light || !header_text_dark) {
        spdlog::critical("[Theme] FATAL: Missing header_text_light/dark in globals.xml");
        std::exit(EXIT_FAILURE);
    }

    // Register runtime constants based on theme preference
    const char* selected_bg = use_dark_mode ? app_bg_dark : app_bg_light;
    lv_xml_register_const(scope, "app_bg_color", selected_bg);
    spdlog::debug("[Theme] Registered app_bg_color={} for {} mode",
                  selected_bg, use_dark_mode ? "dark" : "light");

    const char* selected_text = use_dark_mode ? text_primary_dark : text_primary_light;
    lv_xml_register_const(scope, "text_primary", selected_text);
    spdlog::debug("[Theme] Registered text_primary={} for {} mode",
                  selected_text, use_dark_mode ? "dark" : "light");

    const char* selected_header = use_dark_mode ? header_text_dark : header_text_light;
    lv_xml_register_const(scope, "header_text_color", selected_header);
    spdlog::debug("[Theme] Registered header_text_color={} for {} mode",
                  selected_header, use_dark_mode ? "dark" : "light");

    spdlog::debug("[Theme] Runtime constants set for {} mode", use_dark_mode ? "dark" : "light");

    // Read colors from globals.xml
    const char* primary_str = lv_xml_get_const(NULL, "primary_color");
    const char* secondary_str = lv_xml_get_const(NULL, "secondary_color");

    if (!primary_str || !secondary_str) {
        spdlog::error("[Theme] Failed to read color constants from globals.xml");
        return;
    }

    lv_color_t primary_color = ui_theme_parse_color(primary_str);
    lv_color_t secondary_color = ui_theme_parse_color(secondary_str);

    // Read base font from globals.xml
    const char* font_body_name = lv_xml_get_const(NULL, "font_body");
    const lv_font_t* base_font = lv_xml_get_font(NULL, font_body_name);
    if (!base_font) {
        spdlog::warn("[Theme] Failed to get font '{}', using montserrat_16", font_body_name);
        base_font = &lv_font_montserrat_16;
    }

    // Read color variants for theme initialization
    const char* screen_bg_str = use_dark_mode ? lv_xml_get_const(NULL, "app_bg_color_dark")
                                               : lv_xml_get_const(NULL, "app_bg_color_light");
    const char* card_bg_str = use_dark_mode ? lv_xml_get_const(NULL, "card_bg_dark")
                                             : lv_xml_get_const(NULL, "card_bg_light");
    const char* theme_grey_str = use_dark_mode ? lv_xml_get_const(NULL, "theme_grey_dark")
                                                : lv_xml_get_const(NULL, "theme_grey_light");

    if (!screen_bg_str || !card_bg_str || !theme_grey_str) {
        spdlog::error("[Theme] Failed to read color variants from globals.xml");
        return;
    }

    lv_color_t screen_bg = ui_theme_parse_color(screen_bg_str);
    lv_color_t card_bg = ui_theme_parse_color(card_bg_str);
    lv_color_t theme_grey = ui_theme_parse_color(theme_grey_str);

    // Initialize custom HelixScreen theme (wraps LVGL default theme)
    current_theme = helix_theme_init(
        display,
        primary_color,
        secondary_color,
        use_dark_mode,
        base_font,
        screen_bg,
        card_bg,
        theme_grey
    );

    if (current_theme) {
        lv_display_set_theme(display, current_theme);
        spdlog::info("[Theme] Initialized HelixScreen theme: {} mode, primary={}, secondary={}, base_font={}",
                     use_dark_mode ? "dark" : "light", primary_str, secondary_str, font_body_name);
        spdlog::info("[Theme] Colors: screen={}, card={}, grey={}",
                     screen_bg_str, card_bg_str, theme_grey_str);

        // Register responsive padding constants AFTER theme init
        ui_theme_register_responsive_padding(display);
    } else {
        spdlog::error("[Theme] Failed to initialize HelixScreen theme");
    }
}

void ui_theme_toggle_dark_mode() {
    if (!theme_display) {
        spdlog::error("[Theme] Cannot toggle: theme not initialized");
        return;
    }

    bool new_use_dark_mode = !use_dark_mode;
    spdlog::info("[Theme] Toggling to {} mode", new_use_dark_mode ? "dark" : "light");

    ui_theme_init(theme_display, new_use_dark_mode);
    lv_obj_invalidate(lv_screen_active());
}

bool ui_theme_is_dark_mode() {
    return use_dark_mode;
}

/**
 * Get theme-appropriate color variant
 *
 * Looks up {base_name}_light and {base_name}_dark from globals.xml,
 * selects the appropriate one based on current theme mode, and returns
 * the parsed lv_color_t.
 *
 * @param base_name Color constant base name (e.g., "app_bg_color", "card_bg")
 * @return Parsed color, or black (0x000000) if not found
 *
 * Example:
 *   lv_color_t bg = ui_theme_get_color("app_bg_color");
 *   // Returns app_bg_color_light in light mode, app_bg_color_dark in dark mode
 */
lv_color_t ui_theme_get_color(const char* base_name) {
    if (!base_name) {
        spdlog::error("[Theme] ui_theme_get_color: NULL base_name");
        return lv_color_hex(0x000000);
    }

    // Construct variant names: {base_name}_light and {base_name}_dark
    char light_name[128];
    char dark_name[128];
    snprintf(light_name, sizeof(light_name), "%s_light", base_name);
    snprintf(dark_name, sizeof(dark_name), "%s_dark", base_name);

    // Look up color strings from globals.xml
    const char* light_str = lv_xml_get_const(nullptr, light_name);
    const char* dark_str = lv_xml_get_const(nullptr, dark_name);

    if (!light_str || !dark_str) {
        spdlog::error("[Theme] Color variant not found: {} (light={}, dark={})",
                      base_name, light_str ? "found" : "missing", dark_str ? "found" : "missing");
        return lv_color_hex(0x000000);
    }

    // Select appropriate variant based on theme mode
    const char* selected_str = use_dark_mode ? dark_str : light_str;
    lv_color_t color = ui_theme_parse_color(selected_str);

    spdlog::debug("[Theme] ui_theme_get_color({}) = {} (0x{:06X}) ({} mode)",
                  base_name, selected_str, lv_color_to_u32(color) & 0xFFFFFF,
                  use_dark_mode ? "dark" : "light");

    return color;
}

/**
 * Apply theme-appropriate background color to object
 *
 * Convenience wrapper that gets the color variant and applies it to the object.
 *
 * @param obj LVGL object to apply color to
 * @param base_name Color constant base name (e.g., "app_bg_color", "card_bg")
 * @param part Style part to apply to (default: LV_PART_MAIN)
 *
 * Example:
 *   ui_theme_apply_bg_color(screen, "app_bg_color", LV_PART_MAIN);
 *   // Applies app_bg_color_light/dark depending on theme mode
 */
void ui_theme_apply_bg_color(lv_obj_t* obj, const char* base_name, lv_part_t part) {
    if (!obj) {
        spdlog::error("[Theme] ui_theme_apply_bg_color: NULL object");
        return;
    }

    lv_color_t color = ui_theme_get_color(base_name);
    lv_obj_set_style_bg_color(obj, color, part);

    spdlog::info("[Theme] Applied background color {} (0x{:06X}) to object (part={})",
                 base_name, lv_color_to_u32(color) & 0xFFFFFF, static_cast<int>(part));
}

/**
 * Get font line height in pixels
 *
 * Returns the total vertical space a line of text will occupy for the given font.
 * This includes ascender, descender, and line gap. Useful for calculating layout
 * heights before widgets are created.
 *
 * @param font Font to query (e.g., UI_FONT_HEADING, &lv_font_montserrat_16)
 * @return Line height in pixels, or 0 if font is NULL
 *
 * Examples:
 *   int32_t heading_h = ui_theme_get_font_height(UI_FONT_HEADING);  // ~24px
 *   int32_t body_h = ui_theme_get_font_height(UI_FONT_BODY);        // ~20px
 *   int32_t small_h = ui_theme_get_font_height(UI_FONT_SMALL);      // ~15px
 *
 *   // Calculate total height for multi-line layout
 *   int32_t total = ui_theme_get_font_height(UI_FONT_HEADING) +
 *                   (ui_theme_get_font_height(UI_FONT_BODY) * 3) +
 *                   (4 * 8);  // 4 gaps of 8px padding
 */
int32_t ui_theme_get_font_height(const lv_font_t* font) {
    if (!font) {
        spdlog::warn("[Theme] ui_theme_get_font_height: NULL font pointer");
        return 0;
    }

    int32_t height = lv_font_get_line_height(font);

    spdlog::trace("[Theme] Font line height: {}px", height);

    return height;
}
