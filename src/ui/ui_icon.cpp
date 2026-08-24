// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_icon.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_variant.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_style.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

/**
 * Icon size enum - maps to MDI font sizes
 */
enum class IconSize { XS, SM, MD, LG, XL };

/**
 * Parse size string to IconSize enum
 */
static IconSize parse_size(const char* size_str) {
    if (!size_str || strlen(size_str) == 0) {
        return IconSize::XL; // Default
    }

    if (strcmp(size_str, "xs") == 0) {
        return IconSize::XS;
    } else if (strcmp(size_str, "sm") == 0) {
        return IconSize::SM;
    } else if (strcmp(size_str, "md") == 0) {
        return IconSize::MD;
    } else if (strcmp(size_str, "lg") == 0) {
        return IconSize::LG;
    } else if (strcmp(size_str, "xl") == 0) {
        return IconSize::XL;
    }

    spdlog::warn("[Icon] Invalid size '{}', using default 'xl'", size_str);
    return IconSize::XL;
}

/**
 * Per-size icon font metadata: XML constant name and hardcoded fallback.
 */
struct IconFontEntry {
    const char* xml_const;
    const lv_font_t* fallback;
};

static const IconFontEntry& get_font_entry(IconSize size) {
    static const IconFontEntry entries[] = {
        {"icon_font_xs", &mdi_icons_16}, // XS
        {"icon_font_sm", &mdi_icons_24}, // SM
        {"icon_font_md", &mdi_icons_32}, // MD
        {"icon_font_lg", &mdi_icons_48}, // LG
        {"icon_font_xl", &mdi_icons_64}, // XL
    };
    static_assert(sizeof(entries) / sizeof(entries[0]) == static_cast<int>(IconSize::XL) + 1,
                  "entries array size must match IconSize enum range");
    return entries[static_cast<int>(size)];
}

/// One entry per IconSize, filled on first use and cleared whenever the
/// breakpoint re-points the icon_font_* constants.
static const lv_font_t* s_cached_font[5] = {};
static bool s_font_resolved[5] = {};

/**
 * Resolve the MDI font for a given size via XML constants.
 *
 * Resolves once per size and caches the result. Falls back to a hardcoded
 * font if the XML constant is missing or the font is not compiled,
 * logging an error on first occurrence.
 */
static const lv_font_t* get_font_for_size(IconSize size) {
    const int idx = static_cast<int>(size);

    if (s_font_resolved[idx]) {
        return s_cached_font[idx];
    }

    const auto& entry = get_font_entry(size);
    const lv_font_t* font = nullptr;

    const char* font_name = lv_xml_get_const_silent(nullptr, entry.xml_const);
    if (font_name) {
        font = lv_xml_get_font(nullptr, font_name);
        if (!font) {
            spdlog::error("[Icon] Font '{}' (from constant '{}') is not compiled — using fallback",
                          font_name, entry.xml_const);
        }
    } else {
        spdlog::error("[Icon] Font constant '{}' not found in globals.xml — using fallback",
                      entry.xml_const);
    }

    if (!font) {
        font = entry.fallback;
    }

    spdlog::debug("[Icon] Using icon font for '{}' — line_height={}px", entry.xml_const,
                  lv_font_get_line_height(font));

    s_cached_font[idx] = font;
    s_font_resolved[idx] = true;
    return font;
}

void ui_icon_invalidate_font_cache() {
    for (bool& r : s_font_resolved) {
        r = false;
    }
}

/**
 * Apply size to icon widget (font only - let content determine dimensions)
 *
 * Uses LV_SIZE_CONTENT so the widget automatically sizes to fit the font glyph.
 * This prevents clipping when font line_height differs from nominal size
 * (e.g., 32px font may have 33px line_height due to glyph bounding boxes).
 */
static void apply_size(lv_obj_t* obj, IconSize size) {
    const lv_font_t* font = get_font_for_size(size);

    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
}

/**
 * Apply icon source (lookup codepoint and set label text)
 */
static void apply_source(lv_obj_t* obj, const char* src) {
    if (!src || strlen(src) == 0) {
        src = "image_broken_variant"; // Default icon (broken image)
    }

    // Try direct lookup first
    const char* codepoint = ui_icon::lookup_codepoint(src);

    // If not found, try stripping legacy "mat_" prefix and "_img" suffix
    if (!codepoint) {
        const char* stripped = ui_icon::strip_legacy_prefix(src);
        if (stripped != src) {
            codepoint = ui_icon::lookup_codepoint(stripped);
        }
    }

    if (codepoint) {
        lv_label_set_text(obj, codepoint);
        spdlog::trace("[Icon] Set icon '{}' -> codepoint", src);
    } else {
        // Fallback to broken image icon
        const char* fallback = ui_icon::lookup_codepoint("image_broken_variant");
        if (fallback) {
            lv_label_set_text(obj, fallback);
        }
        spdlog::warn("[Icon] Icon '{}' not found, using 'image_broken_variant' fallback", src);
    }
}

/**
 * XML create function for icon widget
 * Called by LVGL XML parser when <icon> is encountered
 */
static void* ui_icon_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs;
    lv_obj_t* parent = (lv_obj_t*)lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_label_create(parent);

    // Apply default size (xl = 64px)
    apply_size(obj, IconSize::XL);

    // Apply default source (broken image icon)
    apply_source(obj, "image_broken_variant");

    // Default variant (primary text color)
    helix::ui::apply_variant_text_style(obj, helix::ui::Variant::NONE);

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
    IconSize size = IconSize::XL;
    helix::ui::Variant variant = helix::ui::Variant::NONE;
    const char* src = nullptr;
    bool size_set = false;
    bool variant_set = false;
    bool custom_color_set = false;
    lv_color_t custom_color;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "src") == 0) {
            src = value;
        } else if (strcmp(name, "size") == 0) {
            size = parse_size(value);
            size_set = true;
        } else if (strcmp(name, "variant") == 0) {
            variant = helix::ui::parse_variant(value);
            variant_set = true;
        } else if (strcmp(name, "color") == 0) {
            custom_color = lv_xml_to_color(value);
            custom_color_set = true;
        }
    }

    // Apply size first (sets font)
    if (size_set) {
        apply_size(obj, size);
    }

    // Apply source (sets label text to codepoint)
    if (src) {
        apply_source(obj, src);
    }

    // Custom color overrides variant
    if (custom_color_set) {
        lv_obj_set_style_text_color(obj, custom_color, LV_PART_MAIN);
        lv_obj_set_style_text_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    } else if (variant_set) {
        helix::ui::apply_variant_text_style(obj, variant);
    }
}

/**
 * Register the icon widget with LVGL's XML system
 */
void ui_icon_register_widget() {
    lv_xml_register_widget("icon", ui_icon_xml_create, ui_icon_xml_apply);
    spdlog::trace("[Icon] Font-based icon widget registered with XML system");
}

// Public API implementations

void ui_icon_set_source(lv_obj_t* icon, const char* icon_name) {
    if (!icon || !icon_name) {
        spdlog::error("[Icon] Invalid parameters to ui_icon_set_source");
        return;
    }

    apply_source(icon, icon_name);
    spdlog::trace("[Icon] Changed icon source to '{}'", icon_name);
}

void ui_icon_set_size(lv_obj_t* icon, const char* size_str) {
    if (!icon || !size_str) {
        spdlog::error("[Icon] Invalid parameters to ui_icon_set_size");
        return;
    }

    IconSize size = parse_size(size_str);
    apply_size(icon, size);
    spdlog::trace("[Icon] Changed icon size to '{}'", size_str);
}

void ui_icon_set_variant(lv_obj_t* icon, const char* variant_str) {
    if (!icon || !variant_str) {
        spdlog::error("[Icon] Invalid parameters to ui_icon_set_variant");
        return;
    }

    auto variant = helix::ui::parse_variant(variant_str);
    helix::ui::apply_variant_text_style(icon, variant);
    spdlog::trace("[Icon] Changed icon variant to '{}'", variant_str);
}

void ui_icon_set_color(lv_obj_t* icon, lv_color_t color, lv_opa_t opa) {
    if (!icon) {
        spdlog::error("[Icon] Invalid icon parameter to ui_icon_set_color");
        return;
    }

    lv_obj_set_style_text_color(icon, color, LV_PART_MAIN);
    lv_obj_set_style_text_opa(icon, opa, LV_PART_MAIN);
}

void ui_icon_set_clickable(lv_obj_t* icon, bool clickable) {
    if (!icon) {
        spdlog::error("[Icon] Invalid icon parameter to ui_icon_set_clickable");
        return;
    }

    if (clickable) {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }
    spdlog::trace("[Icon] Set clickable: {}", clickable);
}
