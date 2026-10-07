// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The LVGL side of the theme: the helix_theme wrapper over lv_theme_default,
// the shared widget-part styles it layers on, and the palette conversions that
// feed them.

#include "ui_breakpoint.h"
#include "ui_fonts.h"

#include "border_radius_sizes.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/themes/lv_theme_private.h"
#include "platform_capabilities.h"
#include "theme_manager.h"
#include "theme_manager_internal.h"

#include <spdlog/spdlog.h>

namespace helix::theme_detail {

// ============================================================================
// LVGL Theme Infrastructure
// ============================================================================

// Static theme instance - persists for lifetime of app
static lv_theme_t helix_theme;
static lv_theme_t* default_theme_backup = nullptr;

// Additional styles not in StyleRole enum (widget-specific parts)
static lv_style_t dropdown_indicator_style;
static lv_style_t checkbox_text_style;
static lv_style_t checkbox_box_style;
static lv_style_t checkbox_indicator_style;
static lv_style_t switch_track_style;
static lv_style_t switch_indicator_style;
static lv_style_t switch_knob_style;
static lv_style_t switch_knob_checked_style;
static lv_style_t slider_track_style;
static lv_style_t slider_indicator_style;
static lv_style_t slider_knob_style;
static lv_style_t slider_disabled_style;
static lv_color_t dropdown_accent_color;
static bool extra_styles_initialized = false;

// Forward declarations for theme infrastructure
static void init_extra_styles(const theme_palette_t* palette);
static void update_handle_styles(const theme_palette_t* palette, int border_radius, bool is_dark);
static void helix_theme_apply(lv_theme_t* theme, lv_obj_t* obj);

/**
 * @brief Build theme_palette_t from ModePalette
 *
 * Converts the C++ ModePalette struct (hex strings) to C theme_palette_t (lv_color_t).
 * Used to pass colors to theme_core functions.
 *
 * @param mode_palette ModePalette with hex color strings
 * @return theme_palette_t with parsed lv_color_t values
 */
theme_palette_t build_palette_from_mode(const helix::ModePalette& mode_palette) {
    theme_palette_t palette = {};
    palette.screen_bg = theme_manager_parse_hex_color(mode_palette.screen_bg.c_str());
    palette.overlay_bg = theme_manager_parse_hex_color(mode_palette.overlay_bg.c_str());
    palette.card_bg = theme_manager_parse_hex_color(mode_palette.card_bg.c_str());
    palette.elevated_bg = theme_manager_parse_hex_color(mode_palette.elevated_bg.c_str());
    palette.border = theme_manager_parse_hex_color(mode_palette.border.c_str());
    palette.text = theme_manager_parse_hex_color(mode_palette.text.c_str());
    palette.text_muted = theme_manager_parse_hex_color(mode_palette.text_muted.c_str());
    palette.text_subtle = theme_manager_parse_hex_color(mode_palette.text_subtle.c_str());
    palette.primary = theme_manager_parse_hex_color(mode_palette.primary.c_str());
    palette.secondary = theme_manager_parse_hex_color(mode_palette.secondary.c_str());
    palette.tertiary = theme_manager_parse_hex_color(mode_palette.tertiary.c_str());
    palette.info = theme_manager_parse_hex_color(mode_palette.info.c_str());
    palette.success = theme_manager_parse_hex_color(mode_palette.success.c_str());
    palette.warning = theme_manager_parse_hex_color(mode_palette.warning.c_str());
    palette.danger = theme_manager_parse_hex_color(mode_palette.danger.c_str());
    palette.focus = theme_manager_parse_hex_color(mode_palette.focus.c_str());
    return palette;
}

/**
 * @brief Get the current mode palette based on dark/light mode
 *
 * Returns reference to appropriate ModePalette from active_theme.
 * Falls back to the available palette if the requested mode is not supported.
 */
const helix::ModePalette& get_current_mode_palette() {
    if (runtime().dark && runtime().active_theme.supports_dark()) {
        return runtime().active_theme.dark;
    } else if (!runtime().dark && runtime().active_theme.supports_light()) {
        return runtime().active_theme.light;
    } else if (runtime().active_theme.supports_dark()) {
        return runtime().active_theme.dark;
    } else {
        return runtime().active_theme.light;
    }
}

// ============================================================================
// LVGL Theme Infrastructure - Apply Callbacks & Style Initialization
// ============================================================================

/**
 * @brief Update handle/knob styles from current theme properties
 *
 * Called on initial setup and on every theme switch to apply handle_style
 * and handle_color from the active theme. Switch knobs always stay round.
 */
static void update_handle_styles(const theme_palette_t* palette, int border_radius, bool is_dark) {
    bool bar_knob = (runtime().active_theme.properties.handle_style == "bar");
    int32_t slider_knob_radius = bar_knob ? 2 : LV_RADIUS_CIRCLE;

    // Resolve handle color token to palette color
    lv_color_t knob_color = palette->primary;
    const auto& hc = runtime().active_theme.properties.handle_color;
    if (hc == "text")
        knob_color = palette->text;
    else if (hc == "secondary")
        knob_color = palette->secondary;
    else if (hc == "tertiary")
        knob_color = palette->tertiary;

    // Switch knob: handle_color marks ON; OFF is neutral so position is not the only cue
    // (a light outlined knob in light mode, muted grey in dark). Always round (no bar style).
    lv_style_set_bg_color(&switch_knob_style, is_dark ? palette->text_muted : palette->card_bg);
    lv_style_set_border_color(&switch_knob_style, palette->text_subtle);
    lv_style_set_border_width(&switch_knob_style, is_dark ? 0 : 1);
    lv_style_set_bg_color(&switch_knob_checked_style, knob_color);
    lv_style_set_border_width(&switch_knob_checked_style, 0);

    // Slider track/indicator colors
    lv_style_set_bg_color(&slider_track_style, palette->border);
    lv_style_set_radius(&slider_track_style, border_radius);
    lv_style_set_bg_color(&slider_indicator_style, palette->primary);

    // Slider knob: both handle_color and handle_style apply
    lv_style_set_bg_color(&slider_knob_style, knob_color);
    lv_style_set_border_color(&slider_knob_style, palette->border);
    lv_style_set_border_width(&slider_knob_style, bar_knob ? 0 : 1);
    lv_style_set_radius(&slider_knob_style, slider_knob_radius);
    if (bar_knob) {
        lv_style_set_pad_left(&slider_knob_style, -4);
        lv_style_set_pad_right(&slider_knob_style, -4);
        lv_style_set_pad_top(&slider_knob_style, 8);
        lv_style_set_pad_bottom(&slider_knob_style, 8);
    } else {
        // Responsive knob padding: smaller at tiny/micro to avoid clipping in compact cards
        auto* display = lv_display_get_default();
        auto bp = display ? breakpoint_for(responsive_dimension(display)) : UiBreakpoint::Medium;
        int32_t knob_pad = (bp <= UiBreakpoint::Tiny) ? LV_DPX(4) : LV_DPX(6);
        lv_style_set_pad_left(&slider_knob_style, knob_pad);
        lv_style_set_pad_right(&slider_knob_style, knob_pad);
        lv_style_set_pad_top(&slider_knob_style, knob_pad);
        lv_style_set_pad_bottom(&slider_knob_style, knob_pad);
    }

    // Slider knob shadow: functional depth cue
    int knob_shadow_w = runtime().active_theme.properties.shadow_intensity > 0
                            ? runtime().active_theme.properties.shadow_intensity
                            : 4;
    int knob_shadow_opa = runtime().active_theme.properties.shadow_opa > 0
                              ? runtime().active_theme.properties.shadow_opa
                              : LV_OPA_30;
    lv_style_set_shadow_width(&slider_knob_style, knob_shadow_w);
    lv_style_set_shadow_color(&slider_knob_style, lv_color_black());
    lv_style_set_shadow_opa(&slider_knob_style, static_cast<lv_opa_t>(knob_shadow_opa));

    // Update dropdown accent and other palette-dependent colors
    dropdown_accent_color = palette->secondary;
    lv_style_set_text_color(&checkbox_text_style, palette->text);
    lv_style_set_bg_color(&checkbox_box_style, palette->elevated_bg);
    lv_style_set_border_color(&checkbox_box_style, palette->border);
    lv_style_set_bg_color(&checkbox_indicator_style, palette->primary);
    lv_style_set_border_color(&checkbox_indicator_style, palette->primary);
    uint8_t cb_lum = lv_color_luminance(palette->primary);
    lv_style_set_text_color(&checkbox_indicator_style,
                            (cb_lum > 140) ? lv_color_black() : lv_color_white());
    lv_style_set_bg_color(&switch_track_style, palette->border);
    lv_style_set_bg_color(&switch_indicator_style, palette->secondary);
}

/**
 * @brief Initialize the extra widget-specific styles
 *
 * These are styles for widget parts not covered by the StyleRole enum.
 */
static void init_extra_styles(const theme_palette_t* palette) {
    if (extra_styles_initialized)
        return;

    dropdown_accent_color = palette->secondary;

    // Dropdown indicator - MDI font for chevron
    lv_style_init(&dropdown_indicator_style);
    lv_style_set_text_font(&dropdown_indicator_style, &mdi_icons_24);

    // Checkbox styles
    lv_style_init(&checkbox_text_style);
    lv_style_set_text_color(&checkbox_text_style, palette->text);

    lv_style_init(&checkbox_box_style);
    lv_style_set_bg_color(&checkbox_box_style, palette->elevated_bg);
    lv_style_set_bg_opa(&checkbox_box_style, LV_OPA_COVER);
    lv_style_set_border_color(&checkbox_box_style, palette->border);
    lv_style_set_border_width(&checkbox_box_style, 2);
    lv_style_set_radius(&checkbox_box_style, 4);

    lv_style_init(&checkbox_indicator_style);
    lv_style_set_bg_color(&checkbox_indicator_style, palette->primary);
    lv_style_set_bg_opa(&checkbox_indicator_style, LV_OPA_COVER);
    lv_style_set_border_color(&checkbox_indicator_style, palette->primary);
    // Checkmark: set bg_image_src to bold check symbol, rendered via text_font
    lv_style_set_bg_image_src(&checkbox_indicator_style, LV_SYMBOL_OK);
    lv_style_set_text_font(&checkbox_indicator_style, &mdi_icons_16);
    // Contrast text color based on primary luminance (same pattern as ui_button)
    uint8_t cb_lum = lv_color_luminance(palette->primary);
    lv_style_set_text_color(&checkbox_indicator_style,
                            (cb_lum > 140) ? lv_color_black() : lv_color_white());

    // Switch styles
    lv_style_init(&switch_track_style);
    lv_style_set_bg_color(&switch_track_style, palette->border);
    lv_style_set_bg_opa(&switch_track_style, LV_OPA_COVER);

    lv_style_init(&switch_indicator_style);
    lv_style_set_bg_color(&switch_indicator_style, palette->secondary);
    lv_style_set_bg_opa(&switch_indicator_style, LV_OPA_COVER);

    lv_style_init(&switch_knob_style);
    lv_style_set_bg_opa(&switch_knob_style, LV_OPA_COVER);
    lv_style_set_radius(&switch_knob_style, LV_RADIUS_CIRCLE);
    lv_style_init(&switch_knob_checked_style);

    // Slider styles
    lv_style_init(&slider_track_style);
    lv_style_set_bg_opa(&slider_track_style, LV_OPA_COVER);

    lv_style_init(&slider_indicator_style);
    lv_style_set_bg_opa(&slider_indicator_style, LV_OPA_COVER);

    lv_style_init(&slider_knob_style);
    lv_style_set_bg_opa(&slider_knob_style, LV_OPA_COVER);

    lv_style_init(&slider_disabled_style);
    lv_style_set_opa(&slider_disabled_style, LV_OPA_50);

    extra_styles_initialized = true;
}

static bool full_style_effects_allowed_here() {
    // The tier is seeded once at startup; until then, keep the full look.
    static lv_subject_t* tier = nullptr;
    if (!tier)
        tier = lv_xml_get_subject(nullptr, "platform_tier");
    return !tier || helix::full_style_effects_allowed(
                        static_cast<helix::PlatformTier>(lv_subject_get_int(tier)));
}

/**
 * @brief HelixScreen theme apply callback - applies styles based on widget type
 *
 * This is called by LVGL for every widget created. It first applies the default
 * theme, then layers our custom styles on top.
 */
static void helix_theme_apply(lv_theme_t* theme, lv_obj_t* obj) {
    (void)theme;

    // First apply LVGL default theme (provides base padding, switch tracks, etc.)
    if (default_theme_backup && default_theme_backup->apply_cb) {
        default_theme_backup->apply_cb(default_theme_backup, obj);
    }
    if (!full_style_effects_allowed_here()) {
        lv_obj_remove_style(obj, nullptr, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);
    }

    auto& tm = ThemeManager::instance();

    // Global disabled state
    lv_obj_add_style(obj, tm.get_style(StyleRole::Disabled), LV_PART_MAIN | LV_STATE_DISABLED);

    // Plain lv_obj containers get transparent background (layout containers)
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::ObjBase), LV_PART_MAIN);
    }

#if LV_USE_BUTTON
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::Button), LV_PART_MAIN);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Pressed), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Focused), LV_STATE_FOCUSED);
    }
#endif

#if LV_USE_TEXTAREA
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Focused), LV_STATE_FOCUSED);
    }
#endif

#if LV_USE_DROPDOWN
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);
        lv_obj_add_style(obj, &dropdown_indicator_style, LV_PART_INDICATOR);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Focused), LV_STATE_FOCUSED);

        // Force local radius to theme value — LVGL's default theme can set
        // a larger radius on dropdown buttons that survives our added styles.
        // Local styles always win over added styles, so this guarantees
        // dropdowns render at the theme's border_radius.
        lv_obj_set_style_radius(obj, tm.current_palette().border_radius, LV_PART_MAIN);
    }
    if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);
        // The popup floats over other content, so unlike the field it needs a fill.
        lv_obj_set_style_bg_color(obj, tm.current_palette().elevated_bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);

        // Clip highlight rectangles to rounded corners
        lv_obj_set_style_clip_corner(obj, true, LV_PART_MAIN);

        // Add responsive line spacing (1x font height) for comfortable touch targets
        const lv_font_t* list_font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        if (list_font) {
            int32_t line_space = lv_font_get_line_height(list_font);
            lv_obj_set_style_text_line_space(obj, line_space, LV_PART_MAIN);
        }

        // Compute contrast text for dropdown accent
        uint8_t lum = lv_color_luminance(dropdown_accent_color);
        lv_color_t selected_text = (lum > 140) ? lv_color_black() : lv_color_white();

        lv_obj_set_style_bg_color(obj, dropdown_accent_color, LV_PART_SELECTED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SELECTED);
        lv_obj_set_style_text_color(obj, selected_text, LV_PART_SELECTED);
        lv_obj_set_style_bg_color(obj, dropdown_accent_color, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(obj, selected_text, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(obj, dropdown_accent_color, LV_PART_SELECTED | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(obj, selected_text, LV_PART_SELECTED | LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(obj, dropdown_accent_color,
                                  LV_PART_SELECTED | LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER,
                                LV_PART_SELECTED | LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(obj, selected_text,
                                    LV_PART_SELECTED | LV_STATE_CHECKED | LV_STATE_PRESSED);
    }
#endif

#if LV_USE_ROLLER
    if (lv_obj_check_type(obj, &lv_roller_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);
    }
#endif

#if LV_USE_SPINBOX
    if (lv_obj_check_type(obj, &lv_spinbox_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);
    }
#endif

#if LV_USE_CHECKBOX
    if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_add_style(obj, &checkbox_text_style, LV_PART_MAIN);
        lv_obj_add_style(obj, &checkbox_box_style, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &checkbox_indicator_style, LV_PART_INDICATOR | LV_STATE_CHECKED);
    }
#endif

#if LV_USE_SWITCH
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &switch_track_style, LV_PART_MAIN);
        lv_obj_add_style(obj, &switch_indicator_style, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &switch_knob_style, LV_PART_KNOB);
        lv_obj_add_style(obj, &switch_knob_checked_style, LV_PART_KNOB | LV_STATE_CHECKED);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Focused), LV_STATE_FOCUSED);
    }
#endif

#if LV_USE_SLIDER
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &slider_track_style, LV_PART_MAIN);
        lv_obj_add_style(obj, &slider_indicator_style, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &slider_knob_style, LV_PART_KNOB);
        lv_obj_add_style(obj, &slider_disabled_style, LV_PART_MAIN | LV_STATE_DISABLED);
        lv_obj_add_style(obj, &slider_disabled_style, LV_PART_INDICATOR | LV_STATE_DISABLED);
        lv_obj_add_style(obj, &slider_disabled_style, LV_PART_KNOB | LV_STATE_DISABLED);
    }
#endif
}

/**
 * @brief Resolve border radius pixels from size index + current display breakpoint.
 */
static int resolve_border_radius(const helix::ThemeProperties& props) {
    int32_t resp_res = responsive_dimension(runtime().display);
    const char* suffix = theme_manager_get_breakpoint_suffix(resp_res);
    return helix::BorderRadiusSizes::pixels(props.border_radius_size, suffix);
}

/**
 * @brief Convert theme_palette_t to ThemePalette for ThemeManager
 */
static ThemePalette convert_to_theme_palette(const theme_palette_t* p,
                                             const helix::ThemeProperties& props) {
    ThemePalette palette;
    palette.screen_bg = p->screen_bg;
    palette.overlay_bg = p->overlay_bg;
    palette.card_bg = p->card_bg;
    palette.elevated_bg = p->elevated_bg;
    palette.border = p->border;
    palette.text = p->text;
    palette.text_muted = p->text_muted;
    palette.text_subtle = p->text_subtle;
    palette.primary = p->primary;
    palette.secondary = p->secondary;
    palette.tertiary = p->tertiary;
    palette.info = p->info;
    palette.success = p->success;
    palette.warning = p->warning;
    palette.danger = p->danger;
    palette.focus = p->focus;
    palette.border_radius = resolve_border_radius(props);
    palette.button_radius = helix::BorderRadiusSizes::button_pixels(
        props.border_radius_size,
        theme_manager_get_breakpoint_suffix(responsive_dimension(runtime().display)));
    palette.border_width = props.border_width;
    palette.border_opacity = props.border_opacity;
    palette.shadow_width = props.shadow_intensity;
    palette.shadow_opa = props.shadow_opa;
    palette.shadow_offset_y = props.shadow_offset_y;
    return palette;
}

namespace {

/// Both mode palettes of the active theme, as raw colours and as ThemeManager
/// palettes. A single-mode theme fills both sides from the mode it supports, so
/// no empty colour string from the unsupported mode is ever parsed.
struct PalettePair {
    theme_palette_t dark_raw;
    theme_palette_t light_raw;
    ThemePalette dark;
    ThemePalette light;

    const theme_palette_t& raw_for(bool is_dark) const {
        return is_dark ? dark_raw : light_raw;
    }
};

PalettePair build_palette_pair() {
    const helix::ThemeData& theme = runtime().active_theme;
    const bool has_dark = theme.supports_dark();
    const bool has_light = theme.supports_light();
    const auto& dark_src = has_dark ? theme.dark : theme.light;
    const auto& light_src = has_light ? theme.light : theme.dark;

    PalettePair pair;
    pair.dark_raw = build_palette_from_mode(dark_src);
    pair.light_raw = build_palette_from_mode(light_src);
    pair.dark = convert_to_theme_palette(&pair.dark_raw, theme.properties);
    pair.light = convert_to_theme_palette(&pair.light_raw, theme.properties);
    return pair;
}

} // namespace

/**
 * @brief Sync the mutable palette manager to active_theme
 *
 * The legacy ThemeManager can be flipped in place (dark-mode toggle,
 * palette previews, tests); the next theme_manager_init() call is the
 * boundary that re-syncs it with active_theme. Pure in-memory
 * conversion - no file or XML parsing - so both the full init and the
 * repeat-skip path can afford it.
 */
void resync_palette_manager(bool is_dark) {
    const PalettePair pair = build_palette_pair();

    auto& tm = ThemeManager::instance();
    tm.set_palettes(pair.light, pair.dark);
    tm.init();
    tm.set_dark_mode(is_dark);
}

/**
 * @brief Initialize the HelixScreen LVGL theme
 *
 * Sets up ThemeManager, initializes extra widget styles, and registers
 * the helix_theme with LVGL.
 */
lv_theme_t* theme_init_lvgl(lv_display_t* display, const theme_palette_t* palette, bool is_dark,
                            const lv_font_t* base_font) {
    resync_palette_manager(is_dark);

    // Initialize widget-specific styles not in StyleRole enum
    const auto& props = runtime().active_theme.properties;
    init_extra_styles(palette);
    // Theme- and mode-dependent handle styles refresh on every init, not only the first.
    update_handle_styles(palette, resolve_border_radius(props), is_dark);

    // Create LVGL default theme as base (we'll layer on top)
    default_theme_backup =
        lv_theme_default_init(display, palette->primary, palette->secondary, is_dark, base_font);

    // Initialize our custom theme
    lv_theme_set_apply_cb(&helix_theme, helix_theme_apply);
    helix_theme.font_small = base_font;
    helix_theme.font_normal = base_font;
    helix_theme.font_large = base_font;
    helix_theme.color_primary = palette->primary;
    helix_theme.color_secondary = palette->secondary;

    spdlog::trace("[Theme] Initialized HelixScreen theme via ThemeManager");
    return &helix_theme;
}

/**
 * @brief Update theme colors without full re-initialization
 */
void theme_update_colors(bool is_dark) {
    auto& tm = ThemeManager::instance();

    const PalettePair pair = build_palette_pair();
    tm.set_palettes(pair.light, pair.dark);

    tm.set_dark_mode(is_dark);

    // Update handle/knob styles from new theme properties and palette
    update_handle_styles(&pair.raw_for(is_dark),
                         resolve_border_radius(runtime().active_theme.properties), is_dark);

    spdlog::debug("[Theme] Updated colors, dark_mode={}", is_dark);
}

} // namespace helix::theme_detail
