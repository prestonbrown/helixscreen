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

#include "ui_icon.h"

#include "ui_theme.h"

#include "lvgl/lvgl.h"
#include "lvgl/src/xml/lv_xml.h"
#include "lvgl/src/xml/lv_xml_component.h"
#include "lvgl/src/xml/lv_xml_parser.h"
#include "lvgl/src/xml/lv_xml_style.h"
#include "lvgl/src/xml/lv_xml_utils.h"
#include "lvgl/src/xml/lv_xml_widget.h"
#include "lvgl/src/xml/parsers/lv_xml_obj_parser.h"

#include <spdlog/spdlog.h>

#include <cstring>

/**
 * Size mapping: semantic name -> {width, height, scale}
 * Material Design icons are 64x64 at scale 256 (1:1 ratio)
 * Scale = (size / 64) * 256 = size * 4
 */
struct IconSize {
    int32_t width;
    int32_t height;
    int32_t scale;
};

static const IconSize SIZE_XS = {16, 16, 64};
static const IconSize SIZE_SM = {24, 24, 96};
static const IconSize SIZE_MD = {32, 32, 128};
static const IconSize SIZE_LG = {48, 48, 192};
static const IconSize SIZE_XL = {64, 64, 256};

/**
 * Variant mapping: semantic name -> {recolor, opacity}
 * Colors are resolved from global theme constants at runtime.
 */
enum class IconVariant { NONE, PRIMARY, SECONDARY, ACCENT, DISABLED, WARNING, ERROR };

/**
 * Parse size string to IconSize struct
 */
static bool parse_size(const char* size_str, IconSize* out_size) {
    if (strcmp(size_str, "xs") == 0) {
        *out_size = SIZE_XS;
        return true;
    } else if (strcmp(size_str, "sm") == 0) {
        *out_size = SIZE_SM;
        return true;
    } else if (strcmp(size_str, "md") == 0) {
        *out_size = SIZE_MD;
        return true;
    } else if (strcmp(size_str, "lg") == 0) {
        *out_size = SIZE_LG;
        return true;
    } else if (strcmp(size_str, "xl") == 0) {
        *out_size = SIZE_XL;
        return true;
    }

    spdlog::warn("Invalid icon size '{}', using default 'xl'", size_str);
    *out_size = SIZE_XL;
    return false;
}

/**
 * Parse variant string to IconVariant enum
 */
static IconVariant parse_variant(const char* variant_str) {
    if (!variant_str || strlen(variant_str) == 0) {
        return IconVariant::NONE;
    } else if (strcmp(variant_str, "primary") == 0) {
        return IconVariant::PRIMARY;
    } else if (strcmp(variant_str, "secondary") == 0) {
        return IconVariant::SECONDARY;
    } else if (strcmp(variant_str, "accent") == 0) {
        return IconVariant::ACCENT;
    } else if (strcmp(variant_str, "disabled") == 0) {
        return IconVariant::DISABLED;
    } else if (strcmp(variant_str, "warning") == 0) {
        return IconVariant::WARNING;
    } else if (strcmp(variant_str, "error") == 0) {
        return IconVariant::ERROR;
    } else if (strcmp(variant_str, "none") == 0) {
        return IconVariant::NONE;
    }

    spdlog::warn("Invalid icon variant '{}', using default 'none'", variant_str);
    return IconVariant::NONE;
}

/**
 * Apply size properties to icon widget
 */
static void apply_size(lv_obj_t* obj, const IconSize& size) {
    lv_obj_set_size(obj, size.width, size.height);
    lv_image_set_scale(obj, size.scale);
}

/**
 * Apply variant color styling to icon widget
 * Uses colors from ui_theme.h (single source of truth)
 */
static void apply_variant(lv_obj_t* obj, IconVariant variant) {
    (void)obj; // Unused parameter
    lv_color_t color;
    lv_opa_t opa = LV_OPA_COVER;

    switch (variant) {
    case IconVariant::PRIMARY:
        // Primary text color (white in dark mode)
        color = UI_COLOR_TEXT_PRIMARY;
        break;
    case IconVariant::SECONDARY:
        // Secondary text color (gray)
        color = UI_COLOR_TEXT_SECONDARY;
        break;
    case IconVariant::ACCENT:
        // Accent color (red)
        color = UI_COLOR_PRIMARY;
        break;
    case IconVariant::DISABLED:
        // Primary text color at 50% opacity
        color = UI_COLOR_TEXT_PRIMARY;
        opa = LV_OPA_50;
        break;
    case IconVariant::WARNING:
        // Warning color (orange) from globals.xml
        color = ui_theme_get_color("warning_color");
        break;
    case IconVariant::ERROR:
        // Error color (red) from globals.xml
        color = ui_theme_get_color("error_color");
        break;
    case IconVariant::NONE:
    default:
        lv_obj_set_style_image_recolor_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
        return;
    }

    lv_obj_set_style_image_recolor(obj, color, LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(obj, opa, LV_PART_MAIN);
}

/**
 * XML create function for icon widget
 * Called by LVGL XML parser when <icon> or <lv_image> is encountered
 */
static void* ui_icon_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs; // Unused parameter
    lv_obj_t* parent = (lv_obj_t*)lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_image_create(parent);

    // Set default source to mat_home
    const void* default_src = lv_xml_get_image(NULL, "mat_home");
    if (default_src) {
        lv_image_set_src(obj, default_src);
    }

    // Apply default size (xl = 64x64, scale 256)
    apply_size(obj, SIZE_XL);

    // No variant by default (no recoloring)
    apply_variant(obj, IconVariant::NONE);

    return obj;
}

/**
 * XML apply function for icon widget
 * Called by LVGL XML parser to process attributes
 */
static void ui_icon_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* obj = (lv_obj_t*)lv_xml_state_get_item(state);

    // First apply common lv_obj properties (width, height, align, etc.)
    lv_xml_obj_apply(state, attrs);

    // Then process icon-specific properties
    IconSize size = SIZE_XL;
    IconVariant variant = IconVariant::NONE;
    bool size_set = false;
    bool variant_set = false;
    bool custom_color_set = false;
    lv_color_t custom_color;
    lv_opa_t custom_opa = LV_OPA_COVER;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "src") == 0) {
            const void* img_src = lv_xml_get_image(NULL, value);
            if (img_src) {
                lv_image_set_src(obj, img_src);
                spdlog::debug("[Icon] Set icon source: '{}'", value);
            } else {
                spdlog::warn("[Icon] Icon image '{}' not found in XML registry", value);
            }
        } else if (strcmp(name, "size") == 0) {
            parse_size(value, &size);
            size_set = true;
        } else if (strcmp(name, "variant") == 0) {
            variant = parse_variant(value);
            variant_set = true;
        } else if (strcmp(name, "color") == 0) {
            custom_color = lv_xml_to_color(value);
            custom_color_set = true;
        }
    }

    if (size_set) {
        apply_size(obj, size);
    }

    // Custom color overrides variant
    if (custom_color_set) {
        lv_obj_set_style_image_recolor(obj, custom_color, LV_PART_MAIN);
        lv_obj_set_style_image_recolor_opa(obj, custom_opa, LV_PART_MAIN);
    } else if (variant_set) {
        apply_variant(obj, variant);
    }
}

/**
 * Register the icon widget with LVGL's XML system
 */
void ui_icon_register_widget() {
    lv_xml_register_widget("icon", ui_icon_xml_create, ui_icon_xml_apply);
    spdlog::trace("Icon widget registered with XML system");
}

// Public API implementations

void ui_icon_set_source(lv_obj_t* icon, const char* icon_name) {
    if (!icon || !icon_name) {
        spdlog::error("[Icon] Invalid parameters to ui_icon_set_source");
        return;
    }

    const void* img_src = lv_xml_get_image(NULL, icon_name);
    if (img_src) {
        lv_image_set_src(icon, img_src);
        spdlog::debug("[Icon] Changed icon source to '{}'", icon_name);
    } else {
        spdlog::warn("[Icon] Icon image '{}' not found in registry", icon_name);
    }
}

void ui_icon_set_size(lv_obj_t* icon, const char* size_str) {
    if (!icon || !size_str) {
        spdlog::error("[Icon] Invalid parameters to ui_icon_set_size");
        return;
    }

    IconSize size;
    if (parse_size(size_str, &size)) {
        apply_size(icon, size);
        spdlog::debug("[Icon] Changed icon size to '{}'", size_str);
    }
}

void ui_icon_set_variant(lv_obj_t* icon, const char* variant_str) {
    if (!icon || !variant_str) {
        spdlog::error("[Icon] Invalid parameters to ui_icon_set_variant");
        return;
    }

    IconVariant variant = parse_variant(variant_str);
    apply_variant(icon, variant);
    spdlog::debug("[Icon] Changed icon variant to '{}'", variant_str);
}

void ui_icon_set_color(lv_obj_t* icon, lv_color_t color, lv_opa_t opa) {
    if (!icon) {
        spdlog::error("[Icon] Invalid icon parameter to ui_icon_set_color");
        return;
    }

    lv_obj_set_style_image_recolor(icon, color, LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(icon, opa, LV_PART_MAIN);
    spdlog::debug("[Icon] Set custom color (opa: {})", opa);
}
