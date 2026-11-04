// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// XML constant registration: the palette and theme-property tokens a theme
// supplies, the static constants discovered in ui_xml/, and the swatch
// description subjects the theme editor binds to.

#include "ui_observer_guard.h"

#include "border_radius_sizes.h"
#include "display_metrics.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"
#include "theme_manager_internal.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace helix;

namespace helix::theme_detail {

/**
 * Auto-register theme-aware color constants from all XML files
 *
 * Parses all XML files in ui_xml/ to find color pairs (xxx_light, xxx_dark) and registers
 * the base name (xxx) as a runtime constant with the appropriate value
 * based on current theme mode.
 */
void register_color_pairs(lv_xml_component_scope_t* scope, bool dark_mode) {
    // Find all color tokens with _light and _dark suffixes from all XML files
    auto light_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "color", "_light");
    auto dark_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "color", "_dark");

    // For each _light color, check if _dark exists and register base name
    int registered = 0;
    for (const auto& [base_name, light_val] : light_tokens) {
        auto dark_it = dark_tokens.find(base_name);
        if (dark_it != dark_tokens.end()) {
            const char* selected = dark_mode ? dark_it->second.c_str() : light_val.c_str();
            spdlog::trace("[Theme] Registering color {}: selected={}", base_name, selected);
            // set, not register: register is first-write-wins, and a live mode
            // switch must reach the base token every later XML resolves.
            lv_xml_set_const(scope, base_name.c_str(), selected);
            registered++;
        }
    }

    spdlog::trace("[Theme] Auto-registered {} theme-aware color pairs (dark_mode={})", registered,
                  dark_mode);
}

/**
 * Register static constants from all XML files
 *
 * Parses all XML files for <color>, <px>, and <string> elements and registers
 * any that do NOT have dynamic suffixes (_light, _dark, _small, _medium, _large).
 * These static constants are registered first so dynamic variants can override them.
 */
void register_static_constants(lv_xml_component_scope_t* scope) {
    int color_count = 0, px_count = 0, string_count = 0;

    auto color_tokens =
        theme_manager_parse_all_xml_for_element(theme_detail::ui_xml_dir(), "color");

    for (const auto& [name, value] : color_tokens) {
        if (!has_dynamic_suffix(name)) {
            lv_xml_register_const(scope, name.c_str(), value.c_str());
            color_count++;
        }
    }

    const double px_scale = helix::DisplayMetrics::active_scale();
    for (const auto& [name, value] :
         theme_manager_parse_all_xml_for_element(theme_detail::ui_xml_dir(), "px")) {
        if (!has_dynamic_suffix(name)) {
            // Static tokens scale too. They are fixed-size boxes (icon badges,
            // chips, swatches, column widths) whose contents are scaled fonts,
            // so leaving them authored-size is what makes the glyph overflow.
            const std::string scaled = theme_detail::scale_px_token(name, value, px_scale);
            lv_xml_register_const(scope, name.c_str(), scaled.c_str());
            px_count++;
        }
    }

    for (const auto& [name, value] :
         theme_manager_parse_all_xml_for_element(theme_detail::ui_xml_dir(), "string")) {
        if (!has_dynamic_suffix(name)) {
            lv_xml_register_const(scope, name.c_str(), value.c_str());
            string_count++;
        }
    }

    spdlog::debug(
        "[Theme] Registered {} static colors, {} static px, {} static strings (ui_scale={:.3f})",
        color_count, px_count, string_count, px_scale);
}

/**
 * @brief Register semantic colors from dual-palette system
 *
 * Uses the new ModePalette from theme.dark and theme.light to register
 * all 16 semantic color names with _light/_dark variants.
 *
 * For themes with only one mode (dark-only or light-only), only the available
 * variant is registered. For dual-mode themes, both variants are registered.
 *
 * Also registers legacy aliases for backward compatibility with existing XML.
 *
 * @param scope LVGL XML scope to register constants in
 * @param theme Theme data with dual palettes
 * @param dark_mode Whether to use dark mode values for base names
 */
void register_semantic_colors(lv_xml_component_scope_t* scope, const helix::ThemeData& theme,
                              bool dark_mode) {
    // Check which palettes are available
    bool has_dark = theme.supports_dark();
    bool has_light = theme.supports_light();

    // Determine which palette to use for base name registration
    // For dark-only themes in light mode, still use dark palette
    // For light-only themes in dark mode, still use light palette
    const helix::ModePalette* current_palette = nullptr;
    if (dark_mode && has_dark) {
        current_palette = &theme.dark;
    } else if (!dark_mode && has_light) {
        current_palette = &theme.light;
    } else if (has_dark) {
        current_palette = &theme.dark;
    } else if (has_light) {
        current_palette = &theme.light;
    }

    if (!current_palette) {
        spdlog::error("[Theme] No valid palette available in theme");
        return;
    }

    // Register helper - registers base, _dark, and _light variants (if available)
    auto register_color = [&](const char* name, size_t index) {
        const std::string& current_val = current_palette->at(index);

        char dark_name[128], light_name[128];
        snprintf(dark_name, sizeof(dark_name), "%s_dark", name);
        snprintf(light_name, sizeof(light_name), "%s_light", name);

        // Base name takes the current mode's value. set, not register: register
        // is first-write-wins, and a mode or theme switch must reach every XML
        // built afterwards.
        if (!current_val.empty()) {
            lv_xml_set_const(scope, name, current_val.c_str());
        }

        // Register _dark variant if dark palette is available
        if (has_dark) {
            const std::string& dark_val = theme.dark.at(index);
            if (!dark_val.empty()) {
                lv_xml_set_const(scope, dark_name, dark_val.c_str());
            }
        }

        // Register _light variant if light palette is available
        if (has_light) {
            const std::string& light_val = theme.light.at(index);
            if (!light_val.empty()) {
                lv_xml_set_const(scope, light_name, light_val.c_str());
            }
        }
    };

    // Register all 16 semantic colors from ModePalette
    auto& names = helix::ModePalette::color_names();
    for (size_t i = 0; i < 16; ++i) {
        register_color(names[i], i);
    }

    // Text on a solid primary or danger fill, for XML styles that paint one
    // without a ui_button to run the contrast pass (selected pills, chips,
    // faulted cells).
    // Literal names at the call sites: the XML linter's schema extractor finds
    // runtime consts by them.
    auto to_hex = [](lv_color_t color, char(&buf)[8]) -> const char* {
        snprintf(buf, sizeof(buf), "#%06x", lv_color_to_u32(color) & 0xFFFFFF);
        return buf;
    };
    auto text_on = [&](const std::string& fill, char(&buf)[8]) -> const char* {
        return to_hex(theme_manager_get_contrast_adjusted_text(
                          theme_manager_parse_hex_color(current_palette->text.c_str()),
                          theme_manager_parse_hex_color(fill.c_str())),
                      buf);
    };
    char on_primary[8];
    char on_danger[8];
    lv_xml_set_const(scope, "text_on_primary", text_on(current_palette->primary, on_primary));
    lv_xml_set_const(scope, "text_on_danger", text_on(current_palette->danger, on_danger));

    // A chart's measured data curve: the primary accent, shifted only as far as
    // it takes to read on the screen_bg plot, since some palettes pick a primary
    // close to their own background.
    const lv_color_t plot_bg = theme_manager_parse_hex_color(current_palette->screen_bg.c_str());
    char measured[8];
    lv_xml_set_const(scope, "chart_measured",
                     to_hex(theme_manager_get_contrast_adjusted_text(
                                theme_manager_parse_hex_color(current_palette->primary.c_str()),
                                plot_bg, plot_bg, LV_OPA_COVER, kChartCurveContrastThreshold),
                            measured));

    // Swatch descriptions for theme editor - registered as string subjects
    // so bind_text="swatch_N_desc" works in XML (consts don't resolve for bind_text)
    static constexpr const char* swatch_descriptions[ThemeSubjects::kSwatchCount] = {
        "App background",    "Panel/sidebar background", "Card surfaces",
        "Elevated surfaces", "Borders and dividers",     "Primary text",
        "Secondary text",    "Subtle/hint text",         "Primary accent",
        "Secondary accent",  "Tertiary accent",          "Info states",
        "Success states",    "Warning states",           "Danger/error states",
        "Focus ring",
    };

    if (!subjects().swatch_ready) {
        for (size_t i = 0; i < ThemeSubjects::kSwatchCount; ++i) {
            lv_subject_init_string(&subjects().swatch_desc[i], subjects().swatch_bufs[i], nullptr,
                                   ThemeSubjects::kSwatchBufSize, swatch_descriptions[i]);
            char key[24];
            snprintf(key, sizeof(key), "swatch_%zu_desc", i);
            ObserverGuard::mark_subject_teardown_exempt(&subjects().swatch_desc[i]);
            lv_xml_register_subject(nullptr, key, &subjects().swatch_desc[i]);
        }
        subjects().swatch_ready = true;
    }

    spdlog::debug("[Theme] Registered 16 semantic colors + legacy aliases (dark={}, light={})",
                  has_dark, has_light);
}

/**
 * @brief Register theme properties (border_radius, border_width, etc.) as XML constants
 *
 * These override the default values from globals.xml, allowing themes to customize
 * geometry like corner radius and border width - similar to how colors work.
 *
 * IMPORTANT: Must be called BEFORE register_static_constants() since
 * LVGL ignores duplicate lv_xml_register_const calls (first registration wins).
 *
 * @param scope LVGL XML scope to register constants in
 * @param theme Theme data with properties
 */
void register_theme_properties(lv_xml_component_scope_t* scope, const helix::ThemeData& theme,
                               bool dark_mode) {
    char buf[32];

    // Register border_radius and button_radius from size table + current breakpoint
    int32_t resp_res = responsive_dimension(runtime().display);
    const char* suffix = theme_manager_get_breakpoint_suffix(resp_res);
    int radius_px = helix::BorderRadiusSizes::pixels(theme.properties.border_radius_size, suffix);
    snprintf(buf, sizeof(buf), "%d", radius_px);
    lv_xml_register_const(scope, "border_radius", buf);

    // Register border_width
    snprintf(buf, sizeof(buf), "%d", theme.properties.border_width);
    lv_xml_register_const(scope, "border_width", buf);

    // Register border_opacity (0-255)
    snprintf(buf, sizeof(buf), "%d", theme.properties.border_opacity);
    lv_xml_register_const(scope, "border_opacity", buf);

    // Register shadow properties
    snprintf(buf, sizeof(buf), "%d", theme.properties.shadow_intensity);
    lv_xml_register_const(scope, "shadow_intensity", buf);

    snprintf(buf, sizeof(buf), "%d", theme.properties.shadow_opa);
    lv_xml_register_const(scope, "shadow_opa", buf);

    snprintf(buf, sizeof(buf), "%d", theme.properties.shadow_offset_y);
    lv_xml_register_const(scope, "shadow_offset_y", buf);

    // The colour a cast shadow is drawn in. Fixed, not part of the themeable
    // palette: a shadow is the absence of light in both light and dark themes,
    // and the palette's 16 semantic slots are surfaces/text/accents. Exists so
    // XML never has to hardcode a hex (see ui_xml/overlay_panel.xml).
    lv_xml_register_const(scope, "shadow_cast", "0x000000");

    // Opacity for the transient-overlay cast shadow (#1178). Mode-dependent
    // because the same alpha reads very differently against the surface behind
    // it: on dark themes the strip is already near-black and the shadow needs
    // weight to register at all, while on light themes it lands on a white
    // panel and the same value reads as a heavy black band.
    lv_xml_register_const(scope, "overlay_shadow_opa", dark_mode ? "200" : "100");

    spdlog::debug("[Theme] Registered properties: border_radius={}px (size={}, {}), "
                  "border_width={}, border_opacity={}, shadow=({},{},{})",
                  radius_px, theme.properties.border_radius_size,
                  helix::BorderRadiusSizes::name(theme.properties.border_radius_size),
                  theme.properties.border_width, theme.properties.border_opacity,
                  theme.properties.shadow_intensity, theme.properties.shadow_opa,
                  theme.properties.shadow_offset_y);
}

/**
 * @brief Register fixed object color palette tokens for the exclude-object map view.
 *
 * These 8 colors are theme-invariant — they are chosen to be distinguishable on
 * dark thumbnail backgrounds. They are registered as hard-coded constants so they
 * are available regardless of whether globals.xml is loaded from the filesystem.
 *
 * Registered tokens: object_color_1 through object_color_8.
 * LVGL ignores duplicate lv_xml_register_const calls (first registration wins).
 *
 * @param scope LVGL XML scope to register constants in
 */
void register_object_colors(lv_xml_component_scope_t* scope) {
    static const struct {
        const char* name;
        uint32_t hex;
    } OBJECT_COLORS[] = {
        {"object_color_1", 0x7c8aff}, // periwinkle blue
        {"object_color_2", 0x4ecdc4}, // teal
        {"object_color_3", 0xf9c74f}, // golden yellow
        {"object_color_4", 0xa78bfa}, // soft purple
        {"object_color_5", 0xf472b6}, // pink
        {"object_color_6", 0xfb923c}, // orange
        {"object_color_7", 0x34d399}, // emerald
        {"object_color_8", 0x60a5fa}, // sky blue
    };

    for (const auto& entry : OBJECT_COLORS) {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%06x", entry.hex);
        lv_xml_register_const(scope, entry.name, buf);
    }

    spdlog::debug("[Theme] Registered {} object color palette tokens", std::size(OBJECT_COLORS));
}

} // namespace helix::theme_detail
