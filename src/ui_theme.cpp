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

#include "ui_theme.h"

#include "ui_error_reporting.h"

#include "helix_theme.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/libs/expat/expat.h"
#include "lvgl/src/xml/lv_xml.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

// Expat callback data for collecting color base names with _light suffix
struct ColorParserData {
    std::vector<std::string> light_color_bases; // Base names (without _light suffix)
};

// Expat element start handler - finds <color name="xxx_light"> elements
static void XMLCALL color_element_start(void* user_data, const XML_Char* name,
                                        const XML_Char** attrs) {
    if (strcmp(name, "color") != 0)
        return;

    ColorParserData* data = static_cast<ColorParserData*>(user_data);

    // Find the "name" attribute
    for (int i = 0; attrs[i]; i += 2) {
        if (strcmp(attrs[i], "name") == 0) {
            const char* color_name = attrs[i + 1];
            size_t len = strlen(color_name);

            // Check if name ends with "_light"
            const char* suffix = "_light";
            size_t suffix_len = strlen(suffix);
            if (len > suffix_len && strcmp(color_name + len - suffix_len, suffix) == 0) {
                // Extract base name (without _light)
                std::string base_name(color_name, len - suffix_len);
                data->light_color_bases.push_back(base_name);
            }
            break;
        }
    }
}

/**
 * Auto-register theme-aware color constants from globals.xml
 *
 * Parses globals.xml to find color pairs (xxx_light, xxx_dark) and registers
 * the base name (xxx) as a runtime constant with the appropriate value
 * based on current theme mode.
 */
static void ui_theme_register_color_pairs(lv_xml_component_scope_t* scope, bool dark_mode) {
    // Read globals.xml
    std::ifstream file("ui_xml/globals.xml");
    if (!file.is_open()) {
        NOTIFY_ERROR("Could not open ui_xml/globals.xml for color pair registration");
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string xml_content = buffer.str();
    file.close();

    // Parse with expat to find _light colors
    ColorParserData parser_data;
    XML_Parser parser = XML_ParserCreate(nullptr);
    XML_SetUserData(parser, &parser_data);
    XML_SetElementHandler(parser, color_element_start, nullptr);

    if (XML_Parse(parser, xml_content.c_str(), xml_content.size(), XML_TRUE) == XML_STATUS_ERROR) {
        NOTIFY_ERROR("XML parse error in globals.xml line {}: {}", XML_GetCurrentLineNumber(parser),
                     XML_ErrorString(XML_GetErrorCode(parser)));
        XML_ParserFree(parser);
        return;
    }
    XML_ParserFree(parser);

    // For each _light color, check if _dark exists and register base name
    int registered = 0;
    for (const auto& base_name : parser_data.light_color_bases) {
        std::string light_name = base_name + "_light";
        std::string dark_name = base_name + "_dark";

        const char* light_val = lv_xml_get_const(nullptr, light_name.c_str());
        const char* dark_val = lv_xml_get_const(nullptr, dark_name.c_str());

        if (light_val && dark_val) {
            const char* selected = dark_mode ? dark_val : light_val;
            spdlog::debug("[Theme] Registering {}: dark_mode={}, light={}, dark={}, selected={}",
                          base_name, dark_mode, light_val, dark_val, selected);
            lv_xml_register_const(scope, base_name.c_str(), selected);

            // Verify registration worked
            const char* verify = lv_xml_get_const(nullptr, base_name.c_str());
            if (verify && strcmp(verify, selected) != 0) {
                spdlog::error("[Theme] MISMATCH! {} registered as {} but reads back as {}",
                              base_name, selected, verify);
            }
            registered++;
        }
    }

    spdlog::debug("[Theme] Auto-registered {} theme-aware color pairs (dark_mode={})",
                  registered, dark_mode);
}

void ui_theme_register_responsive_padding(lv_display_t* display) {
    // Use custom breakpoints optimized for our hardware: max(hor_res, ver_res)
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    // Determine size suffix for variant lookup (using centralized breakpoints)
    const char* size_suffix;
    const char* size_label;

    if (greater_res <= UI_BREAKPOINT_SMALL_MAX) { // ≤480: 480x320
        size_suffix = "_small";
        size_label = "SMALL";
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) { // 481-800: 800x480, up to 800x600
        size_suffix = "_medium";
        size_label = "MEDIUM";
    } else { // >800: 1024x600, 1280x720+
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
        spdlog::error("[Theme] Failed to read padding variants for size: {} (normal={}, small={}, "
                      "tiny={}, gap={})",
                      size_label, padding_normal ? "found" : "NULL",
                      padding_small ? "found" : "NULL", padding_tiny ? "found" : "NULL",
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

        spdlog::debug(
            "[Theme] Responsive padding: {} ({}px) - normal={}, small={}, tiny={}, gap={}",
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
        spdlog::critical(
            "[Theme] FATAL: Failed to get globals scope for runtime constant registration");
        std::exit(EXIT_FAILURE);
    }

    // Auto-register all color pairs from globals.xml (xxx_light/xxx_dark -> xxx)
    // This handles app_bg_color, text_primary, header_text, theme_grey, card_bg, etc.
    ui_theme_register_color_pairs(scope, use_dark_mode);

    // Validate critical color pairs were registered (fail-fast if missing)
    static const char* required_colors[] = {"app_bg_color", "text_primary", "header_text", nullptr};
    for (const char** name = required_colors; *name != nullptr; ++name) {
        if (!lv_xml_get_const(nullptr, *name)) {
            spdlog::critical(
                "[Theme] FATAL: Missing required color pair {}_light/{}_dark in globals.xml", *name,
                *name);
            std::exit(EXIT_FAILURE);
        }
    }

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

    // Read color values from auto-registered constants
    const char* screen_bg_str = lv_xml_get_const(nullptr, "app_bg_color");
    const char* card_bg_str = lv_xml_get_const(nullptr, "card_bg");
    const char* theme_grey_str = lv_xml_get_const(nullptr, "theme_grey");
    const char* text_primary_str = lv_xml_get_const(nullptr, "text_primary");

    if (!screen_bg_str || !card_bg_str || !theme_grey_str || !text_primary_str) {
        spdlog::error("[Theme] Failed to read auto-registered color constants");
        return;
    }

    lv_color_t screen_bg = ui_theme_parse_color(screen_bg_str);
    lv_color_t card_bg = ui_theme_parse_color(card_bg_str);
    lv_color_t theme_grey = ui_theme_parse_color(theme_grey_str);
    lv_color_t text_primary_color = ui_theme_parse_color(text_primary_str);

    // Read border radius from globals.xml
    const char* border_radius_str = lv_xml_get_const(nullptr, "border_radius");
    if (!border_radius_str) {
        spdlog::error("[Theme] Failed to read border_radius from globals.xml");
        return;
    }
    int32_t border_radius = atoi(border_radius_str);

    // Initialize custom HelixScreen theme (wraps LVGL default theme)
    current_theme =
        helix_theme_init(display, primary_color, secondary_color, text_primary_color, use_dark_mode,
                         base_font, screen_bg, card_bg, theme_grey, border_radius);

    if (current_theme) {
        lv_display_set_theme(display, current_theme);
        spdlog::info("[Theme] Initialized HelixScreen theme: {} mode",
                     use_dark_mode ? "dark" : "light");
        spdlog::debug("[Theme] Colors: primary={}, secondary={}, screen={}, card={}, grey={}",
                      primary_str, secondary_str, screen_bg_str, card_bg_str, theme_grey_str);

        // Register responsive padding constants AFTER theme init
        ui_theme_register_responsive_padding(display);
    } else {
        spdlog::error("[Theme] Failed to initialize HelixScreen theme");
    }
}

/**
 * Walk widget tree and force style refresh on each widget
 *
 * This is needed for widgets that have local/inline styles from XML.
 * Theme styles are automatically refreshed by lv_obj_report_style_change(),
 * but local styles need explicit refresh.
 */
static lv_obj_tree_walk_res_t refresh_style_cb(lv_obj_t* obj, void* user_data) {
    (void)user_data;
    // Force LVGL to recalculate all style properties for this widget
    lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY);
    return LV_OBJ_TREE_WALK_NEXT;
}

void ui_theme_refresh_widget_tree(lv_obj_t* root) {
    if (!root)
        return;

    // Walk entire tree and refresh each widget's styles
    lv_obj_tree_walk(root, refresh_style_cb, nullptr);
}

void ui_theme_toggle_dark_mode() {
    if (!theme_display) {
        spdlog::error("[Theme] Cannot toggle: theme not initialized");
        return;
    }

    bool new_use_dark_mode = !use_dark_mode;
    use_dark_mode = new_use_dark_mode;
    spdlog::info("[Theme] Switching to {} mode", new_use_dark_mode ? "dark" : "light");

    // Read color values directly from _light/_dark variants
    // Note: We can't update lv_xml_register_const() values at runtime (LVGL limitation),
    // so we read the appropriate variant directly based on the new theme mode.
    const char* suffix = new_use_dark_mode ? "_dark" : "_light";

    auto get_themed_color = [suffix](const char* base_name) -> const char* {
        char full_name[128];
        snprintf(full_name, sizeof(full_name), "%s%s", base_name, suffix);
        return lv_xml_get_const(nullptr, full_name);
    };

    const char* screen_bg_str = get_themed_color("app_bg_color");
    const char* card_bg_str = get_themed_color("card_bg");
    const char* theme_grey_str = get_themed_color("theme_grey");
    const char* text_primary_str = get_themed_color("text_primary");

    if (!screen_bg_str || !card_bg_str || !theme_grey_str || !text_primary_str) {
        spdlog::error("[Theme] Failed to read color constants for {} mode",
                      new_use_dark_mode ? "dark" : "light");
        return;
    }

    lv_color_t screen_bg = ui_theme_parse_color(screen_bg_str);
    lv_color_t card_bg = ui_theme_parse_color(card_bg_str);
    lv_color_t theme_grey = ui_theme_parse_color(theme_grey_str);
    lv_color_t text_primary_color = ui_theme_parse_color(text_primary_str);

    spdlog::debug("[Theme] New colors: screen={}, card={}, grey={}, text={}",
                  screen_bg_str, card_bg_str, theme_grey_str, text_primary_str);

    // Update helix theme styles in-place (triggers lv_obj_report_style_change)
    helix_theme_update_colors(new_use_dark_mode, screen_bg, card_bg, theme_grey, text_primary_color);

    // Force style refresh on entire widget tree for local/inline styles
    ui_theme_refresh_widget_tree(lv_screen_active());

    // Invalidate screen to trigger redraw
    lv_obj_invalidate(lv_screen_active());

    spdlog::info("[Theme] Theme toggle complete");
}

bool ui_theme_is_dark_mode() {
    return use_dark_mode;
}

/**
 * Get theme-appropriate color variant with fallback for static colors
 *
 * First attempts to look up {base_name}_light and {base_name}_dark from globals.xml,
 * selecting the appropriate one based on current theme mode. If the theme variants
 * don't exist, falls back to {base_name} directly (for static colors like
 * warning_color, error_color that are the same in both themes).
 *
 * @param base_name Color constant base name (e.g., "app_bg_color", "warning_color")
 * @return Parsed color, or black (0x000000) if not found
 *
 * Example:
 *   lv_color_t bg = ui_theme_get_color("app_bg_color");
 *   // Returns app_bg_color_light in light mode, app_bg_color_dark in dark mode
 *
 *   lv_color_t warn = ui_theme_get_color("warning_color");
 *   // Returns warning_color directly (static, no theme variants)
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
        // Fallback: try the base name directly (for static colors like warning_color, error_color)
        const char* base_str = lv_xml_get_const(nullptr, base_name);
        if (base_str) {
            lv_color_t color = ui_theme_parse_color(base_str);
            spdlog::debug("[Theme] ui_theme_get_color({}) = {} (0x{:06X}) (static, no theme variants)",
                          base_name, base_str, lv_color_to_u32(color) & 0xFFFFFF);
            return color;
        }

        spdlog::error("[Theme] Color not found: {} (no _light/_dark variants, no static fallback)",
                      base_name);
        return lv_color_hex(0x000000);
    }

    // Select appropriate variant based on theme mode
    const char* selected_str = use_dark_mode ? dark_str : light_str;
    lv_color_t color = ui_theme_parse_color(selected_str);

    spdlog::debug("[Theme] ui_theme_get_color({}) = {} (0x{:06X}) ({} mode)", base_name,
                  selected_str, lv_color_to_u32(color) & 0xFFFFFF,
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

    spdlog::debug("[Theme] Applied background color {} (0x{:06X}) to object (part={})", base_name,
                  lv_color_to_u32(color) & 0xFFFFFF, static_cast<int>(part));
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

void ui_set_overlay_width(lv_obj_t* obj, lv_obj_t* screen) {
    if (!obj || !screen) {
        spdlog::warn("[Theme] ui_set_overlay_width: NULL pointer");
        return;
    }

    lv_coord_t screen_width = lv_obj_get_width(screen);
    lv_coord_t nav_width = UI_NAV_WIDTH(screen_width);
    lv_obj_set_width(obj, screen_width - nav_width);
}
