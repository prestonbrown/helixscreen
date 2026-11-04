// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Live recolouring: walking a widget tree after a theme or mode switch and
// re-applying palette colours to widgets whose styles were baked inline, using
// widget class for the interactive controls and a colour-swap map (old palette
// value to new) for plain containers.

#include "ui_fonts.h"
#include "ui_text.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_obj_private.h"       // obj->styles: LVGL has no style iterator
#include "lvgl/src/core/lv_obj_style_private.h" // lv_obj_style_t fields
#include "theme_manager.h"
#include "theme_manager_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace helix;
using helix::theme_detail::runtime;

// Color-swap map for container theming (replaces name-based heuristics)
struct ColorSwapEntry {
    lv_color_t from;
    lv_color_t to;
};

static std::vector<ColorSwapEntry> bg_swap_map;
static std::vector<ColorSwapEntry> border_swap_map;

static bool color_eq(lv_color_t a, lv_color_t b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

/// Add entry to swap map, skipping duplicates where `from` already exists.
/// Logs a debug warning on collision so theme authors can spot flattened palettes.
static void swap_map_add(std::vector<ColorSwapEntry>& map, lv_color_t from, lv_color_t to,
                         const char* name) {
    for (const auto& e : map) {
        if (color_eq(e.from, from)) {
            spdlog::debug("[Theme] Swap map collision: '{}' has same color as earlier entry "
                          "(0x{:02X}{:02X}{:02X}), skipping",
                          name, from.red, from.green, from.blue);
            return;
        }
    }
    map.push_back({from, to});
}

/// True for ThemeManager's shared styles whose colors the walker replaces: the
/// semantic text styles on labels and the button surfaces on buttons.
static bool is_walker_replaced_style(const lv_style_t* style) {
    auto& tm = ThemeManager::instance();
    if (style == tm.get_style(StyleRole::TextPrimary) ||
        style == tm.get_style(StyleRole::TextMuted))
        return true;
    static constexpr StyleRole kButtonRoles[] = {
        StyleRole::Button,
        StyleRole::ButtonPrimary,
        StyleRole::ButtonSecondary,
        StyleRole::ButtonTertiary,
        StyleRole::ButtonDanger,
        StyleRole::ButtonGhost,
        StyleRole::ButtonTransparent,
        StyleRole::ButtonOutline,
        StyleRole::ButtonSuccess,
        StyleRole::ButtonWarning,
        StyleRole::ButtonDisabled,
        StyleRole::ButtonPressed,
    };
    for (StyleRole r : kButtonRoles) {
        if (style == tm.get_style(r))
            return true;
    }
    return false;
}

/// True when `prop` at `selector` comes from a style someone chose for `obj`:
/// a bound style, a component <style> or one added from C++. Checked for a
/// label's text color and a button's bg and border colors. A local value, an
/// LVGL theme style and is_walker_replaced_style() do not count; those are
/// what the walker is there to replace.
static bool color_from_chosen_style(lv_obj_t* obj, lv_style_prop_t prop,
                                    lv_style_selector_t selector) {
    const bool applies = prop == LV_STYLE_TEXT_COLOR
                             ? lv_obj_check_type(obj, &lv_label_class)
                             : (prop == LV_STYLE_BG_COLOR || prop == LV_STYLE_BORDER_COLOR) &&
                                   lv_obj_check_type(obj, &lv_button_class);
    if (!applies)
        return false;
    for (uint32_t i = 0; i < obj->style_cnt; i++) {
        const lv_obj_style_t& entry = obj->styles[i];
        // A disabled entry still counts: lv_obj_bind_style toggles its style
        // disabled, and a local value written now would hide it once enabled.
        if (entry.is_local || entry.is_trans || entry.is_theme)
            continue;
        if (entry.selector != selector || is_walker_replaced_style(entry.style))
            continue;
        lv_style_value_t value;
        if (lv_style_get_prop(entry.style, prop, &value) == LV_STYLE_RES_FOUND)
            return true;
    }
    return false;
}

// Every local colour this file writes goes through here.
static void set_palette_color(lv_obj_t* obj, lv_style_prop_t prop, lv_color_t color,
                              lv_style_selector_t selector) {
    // A color the XML author wrote inline (a token or a literal) is theirs: a
    // token follows a theme switch through lv_xml_reapply_token_styles(), and a
    // literal (white on a black camera backdrop) must not follow it at all.
    if (lv_xml_obj_has_authored_style(obj, prop, selector))
        return;
    // A local value outranks every normal style, so writing one would hide it.
    if (color_from_chosen_style(obj, prop, selector))
        return;
    lv_style_value_t value{};
    value.color = color;
    lv_obj_set_local_style_prop(obj, prop, value, selector);
}

namespace helix::theme_detail {

void set_swap_maps(const helix::ModePalette* old_palette, const helix::ModePalette& new_palette) {
    bg_swap_map.clear();
    border_swap_map.clear();
    if (!old_palette) {
        return;
    }
    auto p = theme_manager_parse_hex_color;
    const helix::ModePalette& old_mp = *old_palette;
    const helix::ModePalette& new_mp = new_palette;
    swap_map_add(bg_swap_map, p(old_mp.screen_bg.c_str()), p(new_mp.screen_bg.c_str()),
                 "screen_bg");
    swap_map_add(bg_swap_map, p(old_mp.card_bg.c_str()), p(new_mp.card_bg.c_str()), "card_bg");
    swap_map_add(bg_swap_map, p(old_mp.elevated_bg.c_str()), p(new_mp.elevated_bg.c_str()),
                 "elevated_bg");
    swap_map_add(bg_swap_map, p(old_mp.overlay_bg.c_str()), p(new_mp.overlay_bg.c_str()),
                 "overlay_bg");
    swap_map_add(bg_swap_map, p(old_mp.border.c_str()), p(new_mp.border.c_str()), "border");
    swap_map_add(border_swap_map, p(old_mp.border.c_str()), p(new_mp.border.c_str()), "border");
}

} // namespace helix::theme_detail

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

void theme_manager_refresh_widget_tree(lv_obj_t* root) {
    if (!root)
        return;

    // Walk entire tree and refresh each widget's styles
    lv_obj_tree_walk(root, refresh_style_cb, nullptr);
}

// ============================================================================
// Palette Application Functions (for DRY preview styling)
// ============================================================================

/**
 * Check if a font is one of the MDI icon fonts (forward declaration)
 */
static bool is_muted_text_font(const lv_font_t* font);

/**
 * Helper to update button label text with contrast-aware color
 *
 * A filled button is an accent surface: its text starts from the palette text
 * colour and shifts toward its own pole just enough to stay readable
 * (theme_manager_get_contrast_adjusted_text()).
 */
static void apply_button_text_contrast(lv_obj_t* btn) {
    if (!btn)
        return;

    // Get button's background color and pick a readable foreground for it
    lv_color_t bg_color = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);
    lv_color_t current_text = theme_manager_get_color("text");
    lv_color_t text_color = theme_manager_get_contrast_adjusted_text(current_text, bg_color);

    // Check for disabled state - use muted color
    bool btn_disabled = lv_obj_has_state(btn, LV_STATE_DISABLED);
    if (btn_disabled) {
        // Blend toward gray for disabled state
        text_color = lv_color_mix(text_color, lv_color_hex(0x888888), 128);
    }

    // Get current muted color to detect text/muted-variant icons
    lv_color_t current_muted = theme_manager_get_color("text_muted");

    // Also check contrast text from both palettes for icon detection
    auto& tm = ThemeManager::instance();
    lv_color_t dark_text = tm.dark_palette().text;
    lv_color_t light_text = tm.light_palette().text;

    // Helper lambda to check if icon color is a "text-like" color that should get contrast.
    // Pure black and white count: they are what a previous pass of this helper
    // wrote, and a re-preview against a different fill must be free to flip them.
    auto is_text_variant_color = [&](lv_color_t c) {
        return lv_color_eq(c, current_text) || lv_color_eq(c, current_muted) ||
               lv_color_eq(c, dark_text) || lv_color_eq(c, light_text) ||
               lv_color_eq(c, lv_color_black()) || lv_color_eq(c, lv_color_white());
    };

    // Update all label children in the button
    // For icons: only apply contrast if they're using text/muted variant
    // Skip icons with semantic colors (primary, warning, etc.)
    uint32_t count = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const lv_font_t* font = lv_obj_get_style_text_font(child, LV_PART_MAIN);
            if (helix::ui::is_icon_font(font)) {
                // Icon: only apply contrast if it's using text/muted variant
                lv_color_t icon_color = lv_obj_get_style_text_color(child, LV_PART_MAIN);
                if (is_text_variant_color(icon_color)) {
                    set_palette_color(child, LV_STYLE_TEXT_COLOR, text_color, LV_PART_MAIN);
                }
            } else {
                // Regular label: use muted color for muted-style fonts, contrast for others
                const lv_font_t* font = lv_obj_get_style_text_font(child, LV_PART_MAIN);
                set_palette_color(child, LV_STYLE_TEXT_COLOR,
                                  is_muted_text_font(font) ? current_muted : text_color,
                                  LV_PART_MAIN);
            }
        }
        // Also check nested containers (some buttons have container > label structure)
        uint32_t nested_count = lv_obj_get_child_count(child);
        for (uint32_t j = 0; j < nested_count; j++) {
            lv_obj_t* nested = lv_obj_get_child(child, j);
            if (lv_obj_check_type(nested, &lv_label_class)) {
                const lv_font_t* nested_font = lv_obj_get_style_text_font(nested, LV_PART_MAIN);
                if (helix::ui::is_icon_font(nested_font)) {
                    lv_color_t icon_color = lv_obj_get_style_text_color(nested, LV_PART_MAIN);
                    if (is_text_variant_color(icon_color)) {
                        set_palette_color(nested, LV_STYLE_TEXT_COLOR, text_color, LV_PART_MAIN);
                    }
                } else {
                    set_palette_color(nested, LV_STYLE_TEXT_COLOR,
                                      is_muted_text_font(nested_font) ? current_muted : text_color,
                                      LV_PART_MAIN);
                }
            }
        }
    }
}

/**
 * Check if a font is a "small" semantic font (text_small, text_xs, text_heading use muted color)
 * Returns true for fonts that should use text_muted color
 */
static bool is_muted_text_font(const lv_font_t* font) {
    if (!font)
        return false;

    // Get semantic font pointers for comparison
    static const lv_font_t* font_small = nullptr;
    static const lv_font_t* font_xs = nullptr;
    static const lv_font_t* font_heading = nullptr;
    static bool fonts_initialized = false;

    if (!fonts_initialized) {
        const char* small_name = lv_xml_get_const(nullptr, "font_small");
        const char* xs_name = lv_xml_get_const(nullptr, "font_xs");
        const char* heading_name = lv_xml_get_const(nullptr, "font_heading");
        if (small_name)
            font_small = lv_xml_get_font(nullptr, small_name);
        if (xs_name)
            font_xs = lv_xml_get_font(nullptr, xs_name);
        if (heading_name)
            font_heading = lv_xml_get_font(nullptr, heading_name);
        fonts_initialized = true;
    }

    // text_small, text_xs, and text_heading all use muted color (per ui_text.cpp)
    return font == font_small || font == font_xs || font == font_heading;
}

void theme_apply_palette_to_widget(lv_obj_t* obj, const helix::ModePalette& palette) {
    if (!obj)
        return;

    // Parse palette colors
    lv_color_t screen_bg = theme_manager_parse_hex_color(palette.screen_bg.c_str());
    lv_color_t overlay_bg = theme_manager_parse_hex_color(palette.overlay_bg.c_str());
    lv_color_t card_bg = theme_manager_parse_hex_color(palette.card_bg.c_str());
    lv_color_t elevated_bg = theme_manager_parse_hex_color(palette.elevated_bg.c_str());
    lv_color_t border = theme_manager_parse_hex_color(palette.border.c_str());
    lv_color_t text_primary = theme_manager_parse_hex_color(palette.text.c_str());
    lv_color_t text_muted = theme_manager_parse_hex_color(palette.text_muted.c_str());
    lv_color_t text_subtle = theme_manager_parse_hex_color(palette.text_subtle.c_str());
    lv_color_t primary = theme_manager_parse_hex_color(palette.primary.c_str());
    lv_color_t secondary = theme_manager_parse_hex_color(palette.secondary.c_str());
    lv_color_t tertiary = theme_manager_parse_hex_color(palette.tertiary.c_str());

    // Compute knob color: brighter of primary vs tertiary
    lv_color_t knob_color = theme_compute_more_saturated(primary, tertiary);

    // ==========================================================================
    // LABELS - Use font-based detection instead of name matching
    // ==========================================================================
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const lv_font_t* font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);

        // Skip icons (MDI font) - they use the icon variant system with shared
        // ThemeManager styles that auto-update on theme change. Setting inline
        // colors here would override variant styles (muted, secondary, etc.)
        // and HeatingIconAnimator's temperature-based colors.
        if (helix::ui::is_icon_font(font)) {
            return;
        }

        // Labels inside buttons get auto-contrast based on button background
        lv_obj_t* parent = lv_obj_get_parent(obj);
        if (parent && lv_obj_check_type(parent, &lv_button_class)) {
            // Button text - contrast is handled by apply_button_text_contrast on parent
            // Just skip, the button handler will update child labels
            return;
        }

        // Labels inside dark overlays (e.g., metadata on thumbnails) need light text
        // regardless of theme mode. Walk ancestors to find nearest opaque container.
        for (lv_obj_t* anc = parent; anc != nullptr; anc = lv_obj_get_parent(anc)) {
            lv_opa_t anc_opa = lv_obj_get_style_bg_opa(anc, LV_PART_MAIN);
            if (anc_opa >= LV_OPA_50) {
                lv_color_t anc_bg = lv_obj_get_style_bg_color(anc, LV_PART_MAIN);
                if (theme_compute_brightness(anc_bg) < 80) {
                    set_palette_color(obj, LV_STYLE_TEXT_COLOR, lv_color_white(), LV_PART_MAIN);
                    return;
                }
                break; // found opaque ancestor, not dark — fall through to normal
            }
        }

        // Small/heading fonts get muted color, body fonts get primary
        if (is_muted_text_font(font)) {
            set_palette_color(obj, LV_STYLE_TEXT_COLOR, text_muted, LV_PART_MAIN);
        } else {
            set_palette_color(obj, LV_STYLE_TEXT_COLOR, text_primary, LV_PART_MAIN);
        }
        return;
    }

    // ==========================================================================
    // BUTTONS - background, border, and text contrast
    // ==========================================================================
    if (lv_obj_check_type(obj, &lv_button_class)) {
        // Get current button background to check if it's a "neutral" button
        lv_color_t current_bg = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
        uint8_t r = current_bg.red;
        uint8_t g = current_bg.green;
        uint8_t b = current_bg.blue;

        // Check if button is "neutral" (grayscale or very desaturated)
        // Accent buttons (primary, secondary, etc.) have colorful backgrounds
        int max_rgb = std::max({(int)r, (int)g, (int)b});
        int min_rgb = std::min({(int)r, (int)g, (int)b});
        int saturation = (max_rgb > 0) ? ((max_rgb - min_rgb) * 255 / max_rgb) : 0;

        // If saturation is low (<30), this is a neutral/gray button - apply elevated_bg
        if (saturation < 30) {
            set_palette_color(obj, LV_STYLE_BG_COLOR, elevated_bg, LV_PART_MAIN);
        }

        set_palette_color(obj, LV_STYLE_BORDER_COLOR, border, LV_PART_MAIN);
        apply_button_text_contrast(obj);
        return;
    }

    // ==========================================================================
    // INTERACTIVE WIDGETS - specific styling per widget type
    // ==========================================================================

    // Checkboxes - box border, primary bg when checked, contrast checkmark
    if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        set_palette_color(obj, LV_STYLE_TEXT_COLOR, text_primary, LV_PART_MAIN);
        set_palette_color(obj, LV_STYLE_BORDER_COLOR, border, LV_PART_INDICATOR);
        set_palette_color(obj, LV_STYLE_BG_COLOR, elevated_bg, LV_PART_INDICATOR);
        // Checked state: primary background with contrasting checkmark
        set_palette_color(obj, LV_STYLE_BG_COLOR, primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        set_palette_color(obj, LV_STYLE_BORDER_COLOR, primary,
                          LV_PART_INDICATOR | LV_STATE_CHECKED);
        uint8_t lum = lv_color_luminance(primary);
        lv_color_t check_color = (lum > 140) ? lv_color_black() : lv_color_white();
        set_palette_color(obj, LV_STYLE_TEXT_COLOR, check_color,
                          LV_PART_INDICATOR | LV_STATE_CHECKED);
        return;
    }

    // Switches - track, indicator, knob
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        set_palette_color(obj, LV_STYLE_BG_COLOR, border, LV_PART_MAIN);
        set_palette_color(obj, LV_STYLE_BG_COLOR, secondary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        const bool dark = theme_manager_is_dark_mode();
        set_palette_color(obj, LV_STYLE_BG_COLOR, dark ? text_muted : card_bg, LV_PART_KNOB);
        set_palette_color(obj, LV_STYLE_BG_COLOR, knob_color, LV_PART_KNOB | LV_STATE_CHECKED);
        return;
    }

    // Sliders - track, indicator, knob
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        set_palette_color(obj, LV_STYLE_BG_COLOR, border, LV_PART_MAIN);
        set_palette_color(obj, LV_STYLE_BG_COLOR, secondary, LV_PART_INDICATOR);
        set_palette_color(obj, LV_STYLE_BG_COLOR, knob_color, LV_PART_KNOB);
        set_palette_color(obj, LV_STYLE_SHADOW_COLOR, screen_bg, LV_PART_KNOB);
        return;
    }

    // Outlined fields (dropdown, textarea, spinbox) - unfilled, outline, text
    if (lv_obj_check_type(obj, &lv_dropdown_class) || lv_obj_check_type(obj, &lv_textarea_class) ||
        lv_obj_check_type(obj, &lv_spinbox_class)) {
        lv_color_t outline =
            helix::field_outline_color(text_subtle, screen_bg, overlay_bg, card_bg, elevated_bg);
        set_palette_color(obj, LV_STYLE_BORDER_COLOR, outline, LV_PART_MAIN);
        set_palette_color(obj, LV_STYLE_TEXT_COLOR, text_primary, LV_PART_MAIN);
        return;
    }

    // Dropdown lists (popup menus)
    if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_color_t dropdown_accent = theme_compute_more_saturated(primary, secondary);
        set_palette_color(obj, LV_STYLE_BG_COLOR, elevated_bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
        set_palette_color(obj, LV_STYLE_TEXT_COLOR, text_primary, LV_PART_MAIN);
        set_palette_color(obj, LV_STYLE_BG_COLOR, dropdown_accent, LV_PART_SELECTED);
        return;
    }

    // ==========================================================================
    // DIVIDERS - detect by structure: thin lv_obj (1-2px) with visible bg, no children
    // ==========================================================================
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        int32_t w = lv_obj_get_width(obj);
        int32_t h = lv_obj_get_height(obj);
        lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
        uint32_t child_count = lv_obj_get_child_count(obj);

        // Divider: thin (<=2px in one dimension), visible bg, no children
        bool is_thin_horizontal = (h <= 2 && w > h * 10);
        bool is_thin_vertical = (w <= 2 && h > w * 10);
        bool is_divider =
            (is_thin_horizontal || is_thin_vertical) && bg_opa > 0 && child_count == 0;

        if (is_divider) {
            set_palette_color(obj, LV_STYLE_BG_COLOR, border, LV_PART_MAIN);
            return;
        }
    }

    // ==========================================================================
    // CONTAINERS - color-swap map (replaces name-based heuristics)
    // ==========================================================================
    // Swap bg_color if it matches any old semantic color
    lv_opa_t bg_opa_check = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
    if (bg_opa_check > 0 && !bg_swap_map.empty()) {
        lv_color_t current_bg = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
        for (const auto& entry : bg_swap_map) {
            if (color_eq(current_bg, entry.from)) {
                set_palette_color(obj, LV_STYLE_BG_COLOR, entry.to, LV_PART_MAIN);
                break;
            }
        }
    }

    // Swap border_color if it matches any old semantic color
    int32_t bw = lv_obj_get_style_border_width(obj, LV_PART_MAIN);
    if (bw > 0 && !border_swap_map.empty()) {
        lv_color_t current_border = lv_obj_get_style_border_color(obj, LV_PART_MAIN);
        for (const auto& entry : border_swap_map) {
            if (color_eq(current_border, entry.from)) {
                set_palette_color(obj, LV_STYLE_BORDER_COLOR, entry.to, LV_PART_MAIN);
                break;
            }
        }
    }
}

void theme_apply_palette_to_tree(lv_obj_t* root, const helix::ModePalette& palette) {
    if (!root)
        return;

    // Apply to this widget
    theme_apply_palette_to_widget(root, palette);

    // Recurse into children
    uint32_t child_count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        theme_apply_palette_to_tree(child, palette);
    }
}

void theme_apply_palette_to_screen_dropdowns(const helix::ModePalette& palette) {
    // Style any screen-level popups (dropdown lists, modals, etc.)
    // These are direct children of the screen, not part of the overlay tree
    lv_color_t elevated_bg = theme_manager_parse_hex_color(palette.elevated_bg.c_str());
    lv_color_t text_color = theme_manager_parse_hex_color(palette.text.c_str());
    lv_color_t border = theme_manager_parse_hex_color(palette.border.c_str());
    lv_color_t primary = theme_manager_parse_hex_color(palette.primary.c_str());
    lv_color_t secondary = theme_manager_parse_hex_color(palette.secondary.c_str());

    // Use more saturated of primary/secondary for highlight (avoids white/gray primaries)
    lv_color_t dropdown_accent = theme_compute_more_saturated(primary, secondary);

    // Text color for selected based on accent luminance
    uint8_t lum = lv_color_luminance(dropdown_accent);
    lv_color_t selected_text = (lum > 140) ? lv_color_black() : lv_color_white();

    lv_obj_t* screen = lv_screen_active();
    uint32_t child_count = lv_obj_get_child_count(screen);
    spdlog::debug("[Theme] Screen has {} children", child_count);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(screen, i);

        // Dropdown lists get special treatment for selection highlighting
        if (lv_obj_check_type(child, &lv_dropdownlist_class)) {
            set_palette_color(child, LV_STYLE_BG_COLOR, elevated_bg, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(child, LV_OPA_COVER, LV_PART_MAIN);
            set_palette_color(child, LV_STYLE_TEXT_COLOR, text_color, LV_PART_MAIN);
            set_palette_color(child, LV_STYLE_BORDER_COLOR, border, LV_PART_MAIN);
            set_palette_color(child, LV_STYLE_BG_COLOR, dropdown_accent, LV_PART_SELECTED);
            lv_obj_set_style_bg_opa(child, LV_OPA_COVER, LV_PART_SELECTED);
            set_palette_color(child, LV_STYLE_TEXT_COLOR, selected_text, LV_PART_SELECTED);
            continue;
        }

        // Other screen-level children (modals, etc.) - apply palette to entire tree
        // Skip the main app layout (it's handled separately by the overlay system)
        const char* name = lv_obj_get_name(child);
        if (name && strcmp(name, "app_layout") == 0) {
            continue;
        }

        // Apply palette to this popup and all its children
        spdlog::debug("[Theme] Applying palette to screen popup: {}", name ? name : "(unnamed)");
        theme_apply_palette_to_tree(child, palette);
    }
}
