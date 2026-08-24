// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme_manager.h"

#include "ui_button.h"
#include "ui_error_reporting.h"
#include "ui_fonts.h"
#include "ui_gradient_canvas.h"
#include "ui_icon.h"
#include "ui_split_button.h"
#include "ui_switch.h"

#include "asset_manager.h"
#include "border_radius_sizes.h"
#include "config.h"
#include "data_root_resolver.h"
#include "helix-xml/src/libs/expat/expat.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "layout_manager.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/themes/lv_theme_private.h"
#include "settings_manager.h"
#include "theme_loader.h"
#include "theme_token_table.h"

#include <spdlog/spdlog.h>

#ifndef HELIX_MAX_FONT_TIER
#define HELIX_MAX_FONT_TIER 6 // default: all tiers (micro=0 .. xxlarge=6)
#endif

#include <cstring>

// Canonical ui_xml directory for token discovery, resolved once through the
// asset-root seam. On desktop this is byte-identical to the old "ui_xml"
// literal; on CWD-less targets (ESP-IDF VFS) it becomes an absolute path
// under the mount the firmware configured via helix::set_asset_root().
static const char* tm_ui_xml_dir() {
    static const std::string dir = helix::asset_path("ui_xml");
    return dir.c_str();
}

// Maps a value-suffix (e.g. "_large") to its tier number. Same ordering as the
// UiBreakpoint tiers and fonts.mk FONT_TIERS. Returns -1 on unknown suffix.
static int tier_num_for_suffix(const char* suffix) {
    if (strcmp(suffix, "_micro") == 0)
        return 0;
    if (strcmp(suffix, "_tiny") == 0)
        return 1;
    if (strcmp(suffix, "_small") == 0)
        return 2;
    if (strcmp(suffix, "_medium") == 0)
        return 3;
    if (strcmp(suffix, "_large") == 0)
        return 4;
    if (strcmp(suffix, "_xlarge") == 0)
        return 5;
    if (strcmp(suffix, "_xxlarge") == 0)
        return 6;
    return -1;
}

// Returns the smaller dimension of the display — used for responsive breakpoint
// selection so portrait layouts pick a breakpoint suited to the cramped axis.
// Landscape: typically the height. Portrait: the width. Either way, the short
// dimension is the one the design system has to fit content into.
int32_t responsive_dimension(lv_display_t* display) {
    lv_display_t* d = display ? display : lv_display_get_default();
    if (!d)
        return 600; // safe fallback when no display is available
    int32_t hor = lv_display_get_horizontal_resolution(d);
    int32_t ver = lv_display_get_vertical_resolution(d);
    if (hor <= 0 || ver <= 0)
        return 600;
    return hor < ver ? hor : ver;
}

// Returns the vertical resolution — the second responsive ladder (#1209).
// responsive_dimension() answers "how much room does the cramped axis have";
// this answers "how much room is there to stack things". Landscape and square
// displays have min(w,h) == h, so the two only diverge in portrait.
int32_t responsive_vertical_dimension(lv_display_t* display) {
    lv_display_t* d = display ? display : lv_display_get_default();
    if (!d)
        return 600; // safe fallback when no display is available
    int32_t ver = lv_display_get_vertical_resolution(d);
    return ver > 0 ? ver : 600;
}

// Breakpoint classification lives in ui_breakpoint.h as breakpoint_for() — the
// single canonical ladder shared with theme_manager_get_breakpoint_suffix() and
// FONT_TIERS ordering. Both axes feed the same ladder; only the scalar differs.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __ANDROID__
#include "system/android_jni.h"

#include <SDL_system.h>
#include <jni.h>

/// Push the theme's screen_bg color to Android's window background so the area
/// behind transparent system bars matches the app theme (dark or light).
static void android_set_window_bg_color(lv_color_t color) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return;

    // Cached global ref owned by helix_activity_class() — never released here.
    jclass cls = helix::android::helix_activity_class(env);
    if (!cls)
        return;

    jmethodID method = env->GetStaticMethodID(cls, "setWindowBackgroundColor", "(I)V");
    if (!method) {
        env->ExceptionClear();
        return;
    }

    // Pack as 0xFFRRGGBB (fully opaque)
    uint32_t rgb = lv_color_to_u32(color);
    jint argb = static_cast<jint>(0xFF000000u | rgb);
    env->CallStaticVoidMethod(cls, method, argb);
}
#endif // __ANDROID__

using namespace helix;

static lv_theme_t* current_theme = nullptr;
static bool use_dark_mode = true;
static lv_display_t* theme_display = nullptr;

static helix::ThemeData active_theme;

// Theme change notification subject (monotonically increasing generation counter)
static lv_subject_t theme_changed_subject;
static int32_t theme_generation = 0;
static bool theme_subject_initialized = false;

// Breakpoint index subject for reactive responsive visibility (0=MICRO..6=XXLARGE)
static lv_subject_t ui_breakpoint_subject;
static bool breakpoint_subject_initialized = false;
// Second ladder, exposed as a subject. #1209 put min(w,h) vs height behind an
// allow-list of px tokens only, so every bind_* in ui_xml/ still saw the cramped
// axis and no declarative binding could reach a tall panel's height.
static lv_subject_t ui_breakpoint_v_subject;
static bool breakpoint_v_subject_initialized = false;
// Orientation subject: 1 for any portrait class, 0 otherwise. ui_breakpoint
// classifies the cramped-axis tier and cannot answer "is this portrait" --
// 480x800 and 800x480 can land on the same tier. Lets ui_xml/*.xml branch
// layout inline with <if cond="ui_is_portrait eq 1"> instead of maintaining
// ui_xml/portrait/* variant files.
static lv_subject_t ui_is_portrait_subject;
static bool is_portrait_subject_initialized = false;

// Swatch description subjects for theme editor (file-scope for deinit access)
static constexpr size_t SWATCH_DESC_COUNT = 16;
static constexpr size_t SWATCH_DESC_BUF_SIZE = 32;
static lv_subject_t swatch_desc_subjects[SWATCH_DESC_COUNT];
static char swatch_desc_bufs[SWATCH_DESC_COUNT][SWATCH_DESC_BUF_SIZE];
static bool swatch_descs_initialized = false;

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

// ============================================================================
// LVGL Theme Infrastructure (formerly in theme_compat.cpp)
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
static lv_style_t slider_track_style;
static lv_style_t slider_indicator_style;
static lv_style_t slider_knob_style;
static lv_style_t slider_disabled_style;
static lv_color_t dropdown_accent_color;
static bool extra_styles_initialized = false;

/**
 * @brief 16-color semantic palette for theme initialization (internal use)
 */
struct theme_palette_t {
    lv_color_t screen_bg;   // 0: Main app background
    lv_color_t overlay_bg;  // 1: Sidebar/panel background
    lv_color_t card_bg;     // 2: Card surfaces
    lv_color_t elevated_bg; // 3: Elevated/control surfaces (buttons, inputs)
    lv_color_t border;      // 4: Borders and dividers
    lv_color_t text;        // 5: Primary text
    lv_color_t text_muted;  // 6: Secondary text
    lv_color_t text_subtle; // 7: Hint/tertiary text
    lv_color_t primary;     // 8: Primary accent
    lv_color_t secondary;   // 9: Secondary accent
    lv_color_t tertiary;    // 10: Tertiary accent
    lv_color_t info;        // 11: Info states
    lv_color_t success;     // 12: Success states
    lv_color_t warning;     // 13: Warning states
    lv_color_t danger;      // 14: Error/danger states
    lv_color_t focus;       // 15: Focus ring color
};

// Forward declarations for theme infrastructure
static void init_extra_styles(const theme_palette_t* palette, int border_radius);
static void update_handle_styles(const theme_palette_t* palette, int border_radius);
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
static theme_palette_t build_palette_from_mode(const helix::ModePalette& mode_palette) {
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
static const helix::ModePalette& get_current_mode_palette() {
    if (use_dark_mode && active_theme.supports_dark()) {
        return active_theme.dark;
    } else if (!use_dark_mode && active_theme.supports_light()) {
        return active_theme.light;
    } else if (active_theme.supports_dark()) {
        return active_theme.dark;
    } else {
        return active_theme.light;
    }
}

// Theme preset overrides removed - colors now come from theme JSON files via ThemeData

// Parse hex color string "#FF4444" -> lv_color_hex(0xFF4444)
lv_color_t theme_manager_parse_hex_color(const char* hex_str) {
    if (!hex_str || hex_str[0] == '\0') {
        // Unset palette field. The theme loader substitutes defaults so this
        // shouldn't happen, but a per-widget tree-walk would otherwise flood the
        // log if it did — keep it quiet (prestonbrown/helixscreen#989).
        spdlog::debug("[Theme] Empty hex color string, using black fallback");
        return lv_color_hex(0x000000);
    }
    if (hex_str[0] != '#') {
        spdlog::error("[Theme] Invalid hex color string: {}", hex_str);
        return lv_color_hex(0x000000);
    }
    uint32_t hex = static_cast<uint32_t>(strtoul(hex_str + 1, nullptr, 16));
    return lv_color_hex(hex);
}

/**
 * @brief Calculate perceived brightness of an lv_color_t
 * Uses standard luminance formula: 0.299*R + 0.587*G + 0.114*B
 * @return Brightness value 0-255
 */
int theme_compute_brightness(lv_color_t color) {
    uint32_t c = lv_color_to_u32(color);
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >> 8) & 0xFF;
    uint8_t b = c & 0xFF;
    return (299 * r + 587 * g + 114 * b) / 1000;
}

/**
 * @brief Return the brighter of two colors
 */
lv_color_t theme_compute_brighter_color(lv_color_t a, lv_color_t b) {
    return theme_compute_brightness(a) >= theme_compute_brightness(b) ? a : b;
}

/**
 * @brief Compute saturation of a color (0-255)
 *
 * Uses HSV saturation: (max - min) / max * 255
 * Returns 0 for grayscale colors, higher for more vivid colors.
 */
int theme_compute_saturation(lv_color_t c) {
    int max_val =
        c.red > c.green ? (c.red > c.blue ? c.red : c.blue) : (c.green > c.blue ? c.green : c.blue);
    int min_val =
        c.red < c.green ? (c.red < c.blue ? c.red : c.blue) : (c.green < c.blue ? c.green : c.blue);
    if (max_val == 0)
        return 0;
    return (max_val - min_val) * 255 / max_val;
}

/**
 * @brief Return the more saturated of two colors
 *
 * Useful for accent colors where you want the more vivid/colorful option
 * rather than the literally brighter one.
 */
lv_color_t theme_compute_more_saturated(lv_color_t a, lv_color_t b) {
    return theme_compute_saturation(a) >= theme_compute_saturation(b) ? a : b;
}

lv_color_t theme_get_knob_color() {
    // Knob color: more saturated of primary vs tertiary (for switch/slider handles)
    const char* primary_str = lv_xml_get_const(nullptr, "primary");
    const char* tertiary_str = lv_xml_get_const(nullptr, "tertiary");

    if (!primary_str) {
        spdlog::warn("[Theme] theme_get_knob_color: missing 'primary' constant");
        return lv_color_hex(0x5e81ac); // Fallback to Nord blue
    }

    lv_color_t primary = theme_manager_parse_hex_color(primary_str);
    lv_color_t tertiary = tertiary_str ? theme_manager_parse_hex_color(tertiary_str) : primary;

    return theme_compute_more_saturated(primary, tertiary);
}

lv_color_t theme_get_accent_color() {
    // Accent color: more saturated of primary vs secondary (for icon accents)
    const char* primary_str = lv_xml_get_const(nullptr, "primary");
    const char* secondary_str = lv_xml_get_const(nullptr, "secondary");

    if (!primary_str) {
        spdlog::warn("[Theme] theme_get_accent_color: missing 'primary' constant");
        return lv_color_hex(0x5e81ac); // Fallback to Nord blue
    }

    lv_color_t primary = theme_manager_parse_hex_color(primary_str);
    lv_color_t secondary = secondary_str ? theme_manager_parse_hex_color(secondary_str) : primary;

    return theme_compute_more_saturated(primary, secondary);
}

lv_color_t theme_manager_get_contrast_color(lv_color_t bg_color) {
    int brightness = theme_compute_brightness(bg_color);
    auto& tm = ThemeManager::instance();
    // Dark background needs light text (dark palette has light-colored text for readability)
    // Light background needs dark text (light palette has dark-colored text for readability)
    return (brightness < 140) ? tm.dark_palette().text : tm.light_palette().text;
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
static void update_handle_styles(const theme_palette_t* palette, int border_radius) {
    bool bar_knob = (active_theme.properties.handle_style == "bar");
    int32_t slider_knob_radius = bar_knob ? 2 : LV_RADIUS_CIRCLE;

    // Resolve handle color token to palette color
    lv_color_t knob_color = palette->primary;
    const auto& hc = active_theme.properties.handle_color;
    if (hc == "text")
        knob_color = palette->text;
    else if (hc == "secondary")
        knob_color = palette->secondary;
    else if (hc == "tertiary")
        knob_color = palette->tertiary;

    // Switch knob: handle_color applies, but always round (no bar style)
    lv_style_set_bg_color(&switch_knob_style, knob_color);

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
    int knob_shadow_w =
        active_theme.properties.shadow_intensity > 0 ? active_theme.properties.shadow_intensity : 4;
    int knob_shadow_opa =
        active_theme.properties.shadow_opa > 0 ? active_theme.properties.shadow_opa : LV_OPA_30;
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
static void init_extra_styles(const theme_palette_t* palette, int border_radius) {
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

    // Apply theme-dependent handle styles (also called on theme switch)
    update_handle_styles(palette, border_radius);
}

// Forward declaration — full definition is below with palette apply functions
static bool is_on_elevated_surface(lv_obj_t* obj);

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

        // On elevated surfaces (dialogs, raised cards), override to overlay_bg for contrast
        if (is_on_elevated_surface(obj)) {
            lv_obj_set_style_bg_color(obj, tm.current_palette().overlay_bg, LV_PART_MAIN);
        }
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

        // On elevated surfaces (dialogs, raised cards), override to overlay_bg for contrast
        if (is_on_elevated_surface(obj)) {
            lv_obj_set_style_bg_color(obj, tm.current_palette().overlay_bg, LV_PART_MAIN);
        }
    }
    if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);

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

        // On elevated surfaces (dialogs, raised cards), override to overlay_bg for contrast
        if (is_on_elevated_surface(obj)) {
            lv_obj_set_style_bg_color(obj, tm.current_palette().overlay_bg, LV_PART_MAIN);
        }
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
    int32_t resp_res = responsive_dimension(theme_display);
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
    palette.border_width = props.border_width;
    palette.border_opacity = props.border_opacity;
    palette.shadow_width = props.shadow_intensity;
    palette.shadow_opa = props.shadow_opa;
    palette.shadow_offset_y = props.shadow_offset_y;
    return palette;
}

/**
 * @brief Initialize the HelixScreen LVGL theme
 *
 * Sets up ThemeManager, initializes extra widget styles, and registers
 * the helix_theme with LVGL.
 */
static lv_theme_t* theme_init_lvgl(lv_display_t* display, const theme_palette_t* palette,
                                   bool is_dark, const lv_font_t* base_font) {
    // Build palettes from active_theme for contrast calculations.
    // For single-mode themes, use the valid palette for both sides to avoid
    // parsing empty color strings from the unsupported mode.
    bool has_dark = active_theme.supports_dark();
    bool has_light = active_theme.supports_light();
    const auto& dark_src = has_dark ? active_theme.dark : active_theme.light;
    const auto& light_src = has_light ? active_theme.light : active_theme.dark;
    theme_palette_t dark_theme_pal = build_palette_from_mode(dark_src);
    theme_palette_t light_theme_pal = build_palette_from_mode(light_src);

    const auto& props = active_theme.properties;
    ThemePalette dark_pal = convert_to_theme_palette(&dark_theme_pal, props);
    ThemePalette light_pal = convert_to_theme_palette(&light_theme_pal, props);

    auto& tm = ThemeManager::instance();
    tm.set_palettes(light_pal, dark_pal);
    tm.init();
    tm.set_dark_mode(is_dark);

    // Initialize widget-specific styles not in StyleRole enum
    init_extra_styles(palette, resolve_border_radius(props));

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
static void theme_update_colors(bool is_dark) {
    auto& tm = ThemeManager::instance();

    // Build palettes, falling back to the valid mode for single-mode themes
    bool has_dark = active_theme.supports_dark();
    bool has_light = active_theme.supports_light();
    const auto& dark_src = has_dark ? active_theme.dark : active_theme.light;
    const auto& light_src = has_light ? active_theme.light : active_theme.dark;
    theme_palette_t dark_theme_pal = build_palette_from_mode(dark_src);
    theme_palette_t light_theme_pal = build_palette_from_mode(light_src);

    const auto& props = active_theme.properties;
    ThemePalette dark_pal = convert_to_theme_palette(&dark_theme_pal, props);
    ThemePalette light_pal = convert_to_theme_palette(&light_theme_pal, props);

    tm.set_palettes(light_pal, dark_pal);

    tm.set_dark_mode(is_dark);

    // Update handle/knob styles from new theme properties and palette
    const theme_palette_t& current_pal = is_dark ? dark_theme_pal : light_theme_pal;
    update_handle_styles(&current_pal, resolve_border_radius(props));

    spdlog::debug("[Theme] Updated colors, dark_mode={}", is_dark);
}

/**
 * Auto-register theme-aware color constants from all XML files
 *
 * Parses all XML files in ui_xml/ to find color pairs (xxx_light, xxx_dark) and registers
 * the base name (xxx) as a runtime constant with the appropriate value
 * based on current theme mode.
 */
static void theme_manager_register_color_pairs(lv_xml_component_scope_t* scope, bool dark_mode) {
    // Find all color tokens with _light and _dark suffixes from all XML files
    auto light_tokens = theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "color", "_light");
    auto dark_tokens = theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "color", "_dark");

    // For each _light color, check if _dark exists and register base name
    int registered = 0;
    for (const auto& [base_name, light_val] : light_tokens) {
        auto dark_it = dark_tokens.find(base_name);
        if (dark_it != dark_tokens.end()) {
            const char* selected = dark_mode ? dark_it->second.c_str() : light_val.c_str();
            spdlog::trace("[Theme] Registering color {}: selected={}", base_name, selected);
            lv_xml_register_const(scope, base_name.c_str(), selected);
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
static void theme_manager_register_static_constants(lv_xml_component_scope_t* scope) {
    const std::vector<std::string> skip_suffixes = {
        "_light", "_dark", "_micro", "_tiny", "_small", "_medium", "_large", "_xlarge", "_xxlarge"};

    auto has_dynamic_suffix = [&](const std::string& name) {
        for (const auto& suffix : skip_suffixes) {
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return true;
            }
        }
        return false;
    };

    int color_count = 0, px_count = 0, string_count = 0;

    auto color_tokens = theme_manager_parse_all_xml_for_element(tm_ui_xml_dir(), "color");

    for (const auto& [name, value] : color_tokens) {
        if (!has_dynamic_suffix(name)) {
            lv_xml_register_const(scope, name.c_str(), value.c_str());
            color_count++;
        }
    }

    for (const auto& [name, value] :
         theme_manager_parse_all_xml_for_element(tm_ui_xml_dir(), "px")) {
        if (!has_dynamic_suffix(name)) {
            lv_xml_register_const(scope, name.c_str(), value.c_str());
            px_count++;
        }
    }

    for (const auto& [name, value] :
         theme_manager_parse_all_xml_for_element(tm_ui_xml_dir(), "string")) {
        if (!has_dynamic_suffix(name)) {
            lv_xml_register_const(scope, name.c_str(), value.c_str());
            string_count++;
        }
    }

    spdlog::trace("[Theme] Registered {} static colors, {} static px, {} static strings",
                  color_count, px_count, string_count);
}

/**
 * Get the breakpoint suffix for a given resolution
 *
 * Breakpoints (in px) — ranges come from UI_BREAKPOINT_*_MAX constants:
 *   "_micro"    (≤ MICRO_MAX,   e.g. 272)
 *   "_tiny"     (≤ TINY_MAX,    e.g. 320)
 *   "_small"    (≤ SMALL_MAX,   e.g. 460)
 *   "_medium"   (≤ MEDIUM_MAX,  e.g. 540)
 *   "_large"    (≤ LARGE_MAX,   e.g. 800)
 *   "_xlarge"   (≤ XLARGE_MAX, e.g. 1280)
 *   "_xxlarge"  (> XLARGE_MAX — 1440p / 4K)
 *
 * @param resolution Screen dimension in px — typically responsive_dimension(display)
 *                   so portrait orientations pick a breakpoint matched to the
 *                   cramped axis.
 * @return One of the seven suffix strings above (valid for lv_xml_get_const lookups).
 */
const char* theme_manager_get_breakpoint_suffix(int32_t resolution) {
    return responsive_pick(breakpoint_for(resolution), "_micro", "_tiny", "_small", "_medium",
                           "_large", "_xlarge", "_xxlarge");
}

/**
 * Register responsive spacing tokens from all XML files
 *
 * Auto-discovers all <px name="xxx_small"> elements from all XML files in ui_xml/
 * and registers base tokens by matching xxx_small/xxx_medium/xxx_large triplets.
 * This makes the system fully extensible without C++ code changes.
 *
 * CRITICAL: Base tokens must NOT be pre-defined or responsive overrides will be
 * silently ignored (LVGL ignores duplicate lv_xml_register_const).
 *
 * @param display The LVGL display to get resolution from
 */
namespace helix {

const char* nav_width_suffix(int32_t hor_res, int32_t ver_res) {
    // Nav width is primarily a horizontal concern, but VERTICAL resolution
    // distinguishes micro (480x272) from tiny (480x320), which share a width.
    //
    // Ultrawide displays (e.g. 1920x480) are very wide but short. The nav bar is
    // a full-height vertical strip, so its width must track the short vertical
    // extent — not the horizontal resolution, which would otherwise select the
    // widest 'large' bar and waste the horizontal space the grid wants. Detect
    // ultrawide from the aspect ratio directly (the >2.5:1 threshold mirrors
    // LayoutManager::detect) rather than via LayoutManager, which is not yet
    // initialised when this runs at startup.
    const bool ultrawide = ver_res > 0 && hor_res > ver_res * 5 / 2;
    if (ver_res <= UI_BREAKPOINT_MICRO_MAX)
        return "_micro";
    if (ultrawide)
        // Ultrawide prioritises horizontal content space, so keep the vertical
        // nav strip slim: cap at 'small' and only go narrower on very short
        // panels. (Icons stay legible — they are centred in the strip.)
        return (ver_res <= UI_BREAKPOINT_TINY_MAX) ? "_tiny" : "_small";
    if (hor_res <= 520)
        return "_tiny";
    if (hor_res <= 900)
        return "_small";
    if (hor_res <= 1100)
        return "_medium";
    return "_large";
}

OverlayWidths compute_overlay_widths(int32_t hor_res, int32_t ver_res, int32_t nav_width,
                                     int32_t gap) {
    // Classified from raw dimensions rather than LayoutManager::type(): this
    // runs in Application phase 6 and the layout manager is not initialised
    // until phase 8b. detect_layout_type() is the same function the variant
    // chain uses, so the sizing and the choice of ui_xml/portrait/ cannot
    // disagree. A LayoutManager::set_override() forcing a portrait *layout*
    // onto landscape hardware is deliberately not honoured here — the physical
    // nav bar geometry is what the arithmetic is about.
    const bool portrait = is_portrait_layout(detect_layout_type(hor_res, ver_res));
    if (portrait) {
        // Portrait's nav bar is a bottom strip (compute_overlay_heights), not a
        // side rail, so it costs an overlay nothing horizontally — and neither
        // does the "you will return from this" gap: that gap belongs on the
        // axis the nav bar occupies. Both classes are full width; the gap is
        // carried by compute_overlay_heights instead. An override in
        // ui_set_overlay_geometry would leave this function stating something
        // false, so the rule lives here, not downstream.
        return {hor_res, hor_res};
    }
    return {hor_res - nav_width - gap, hor_res - nav_width};
}

OverlayHeights compute_overlay_heights(int32_t hor_res, int32_t ver_res, int32_t nav_height,
                                       int32_t gap) {
    // Same classification as compute_overlay_widths, and for the same reason:
    // this can run before LayoutManager::init(), and the threshold that picks
    // ui_xml/portrait/ must never disagree with the one that sizes overlays.
    //
    // Landscape reserves nothing vertically — its nav bar is a full-HEIGHT
    // strip at the leading edge, so overlays span the whole display and the
    // gap is spent horizontally instead.
    const bool portrait = is_portrait_layout(detect_layout_type(hor_res, ver_res));
    if (!portrait) {
        return {ver_res, ver_res};
    }
    return {ver_res - nav_height - gap, ver_res - nav_height};
}

} // namespace helix

// ============================================================================
// Responsive px token resolution — one implementation, two callers
// ============================================================================
// Startup (theme_manager_register_responsive_spacing) and resize
// (theme_manager_refresh_layout_constants) used to carry two copies of the tier
// selection chain. They must never disagree: a token that gets one tier at boot
// and another after a rotation is worse than no responsiveness at all.

// The tokens that size a box vertically, and so must be chosen from how much
// height there is rather than from the cramped axis (#1209). Exact base names,
// deliberately not a `*_height` convention: dialog_content_max is a vertical
// maximum that does not end in _height, and a convention would have to
// understand the _sm/_lg modifiers too.
//
// Adding to this list is how a new height token opts in — see
// docs/devel/UI_CONTRIBUTOR_GUIDE.md § "Adding New Tokens". Horizontal and
// axis-neutral tokens (space_*, widths, square icon/badge sizes) stay on the
// cramped ladder and belong nowhere near here.
static constexpr const char* VERTICAL_AXIS_TOKENS[] = {
    "button_height",
    "button_height_sm",
    "button_height_lg",
    "header_height",
    "input_height",
    "temp_card_height",
    "dialog_content_max",
    "dialog_content_pinned_max",
    "dialog_content_tall_chrome_max",
    "spinner_lg",
    "header_button_height",
};

bool theme_manager_token_uses_vertical_axis(const char* base_name) {
    if (!base_name || base_name[0] == '\0')
        return false;
    for (const char* name : VERTICAL_AXIS_TOKENS) {
        if (strcmp(base_name, name) == 0)
            return true;
    }
    return false;
}

namespace {

/// The seven per-tier value tables, parsed once per resolve.
struct PxTierTables {
    std::unordered_map<std::string, std::string> micro, tiny, small, medium, large, xlarge, xxlarge;
};

/// Pick a token's value for one tier suffix, applying the declared fallbacks:
/// the optional tiers (_micro, _tiny, _xlarge, _xxlarge) fall back inwards to
/// the required _small/_medium/_large triplet. Caller guarantees the triplet.
const std::string& pick_tier(const PxTierTables& t, const std::string& base, const char* suffix) {
    const auto& small_val = t.small.at(base);

    if (strcmp(suffix, "_micro") == 0) {
        auto it = t.micro.find(base);
        if (it != t.micro.end())
            return it->second;
        auto tiny_it = t.tiny.find(base);
        return (tiny_it != t.tiny.end()) ? tiny_it->second : small_val;
    }
    if (strcmp(suffix, "_tiny") == 0) {
        auto it = t.tiny.find(base);
        return (it != t.tiny.end()) ? it->second : small_val;
    }
    if (strcmp(suffix, "_small") == 0)
        return small_val;
    if (strcmp(suffix, "_medium") == 0)
        return t.medium.at(base);
    if (strcmp(suffix, "_large") == 0)
        return t.large.at(base);
    if (strcmp(suffix, "_xlarge") == 0) {
        auto it = t.xlarge.find(base);
        return (it != t.xlarge.end()) ? it->second : t.large.at(base);
    }
    // _xxlarge: fall back to _xlarge, then _large.
    auto it = t.xxlarge.find(base);
    if (it != t.xxlarge.end())
        return it->second;
    auto xl_it = t.xlarge.find(base);
    return (xl_it != t.xlarge.end()) ? xl_it->second : t.large.at(base);
}

} // namespace

std::unordered_map<std::string, std::string>
theme_manager_resolve_px_tokens(lv_display_t* display) {
    // tm_ui_xml_dir(), not the "ui_xml" literal: on ESP-IDF the asset root is a
    // VFS mount, and the literal would scan an empty path AND miss the
    // build-time token-table fast path (its guard string-compares the dir).
    PxTierTables t{
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "px", "_micro"),
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "px", "_tiny"),
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "px", "_small"),
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "px", "_medium"),
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "px", "_large"),
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "px", "_xlarge"),
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "px", "_xxlarge"),
    };

    // Two ladders, one classification function. Landscape and square displays
    // have min(w,h) == h, so these two suffixes are equal there and nothing
    // moves; only portrait geometry sees a difference.
    const char* cramped_suffix = theme_manager_get_breakpoint_suffix(responsive_dimension(display));
    const char* vertical_suffix =
        theme_manager_get_breakpoint_suffix(responsive_vertical_dimension(display));

    // nav_width is the one token with its own ladder: primarily horizontal, but
    // it uses the vertical resolution to separate 480x272 from 480x320 and to
    // keep the strip slim on ultrawide panels.
    lv_display_t* d = display ? display : lv_display_get_default();
    const char* nav_suffix = d ? helix::nav_width_suffix(lv_display_get_horizontal_resolution(d),
                                                         lv_display_get_vertical_resolution(d))
                               : "_medium";

    std::unordered_map<std::string, std::string> resolved;
    for (const auto& [base_name, small_val] : t.small) {
        // _small/_medium/_large is the required triplet; anything short of it is
        // an incomplete set (theme_manager_validate_constant_sets warns) and is
        // left unregistered rather than guessed at.
        if (t.medium.find(base_name) == t.medium.end() || t.large.find(base_name) == t.large.end())
            continue;

        const char* suffix = base_name == "nav_width" ? nav_suffix
                             : theme_manager_token_uses_vertical_axis(base_name.c_str())
                                 ? vertical_suffix
                                 : cramped_suffix;

        resolved[base_name] = pick_tier(t, base_name, suffix);
    }
    return resolved;
}

void theme_manager_register_responsive_spacing(lv_display_t* display) {
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);

    // Logging only — the per-token axis choice lives in the resolver below. The
    // cramped axis is what most tokens follow, so it is what the summary reports.
    int32_t resp_res = responsive_dimension(display);
    const char* size_label = responsive_pick(breakpoint_for(resp_res), "MICRO", "TINY", "SMALL",
                                             "MEDIUM", "LARGE", "XLARGE", "XXLARGE");

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope) {
        spdlog::warn("[Theme] Failed to get globals scope for spacing constants");
        return;
    }

    // Auto-discover and resolve every px token — including nav_width, which the
    // resolver gives its own horizontal ladder. Shared with the resize path so
    // the two can never pick different tiers for the same token.
    int registered = 0;
    for (const auto& [base_name, value] : theme_manager_resolve_px_tokens(display)) {
        spdlog::trace("[Theme] Registering spacing {}: selected={}", base_name, value);
        lv_xml_register_const(scope, base_name.c_str(), value.c_str());
        registered++;
    }

    spdlog::trace("[Theme] Responsive spacing: {} (min_dim={}px) - auto-registered {} tokens",
                  size_label, resp_res, registered);

    // ========================================================================
    // Register computed overlay widths (derived from nav_width + gap)
    // ========================================================================
    // nav_width was registered above from its own horizontal ladder. Read it
    // back to compute overlay panel widths.
    const char* nav_width_str = lv_xml_get_const(nullptr, "nav_width");
    int32_t nav_width = nav_width_str ? std::atoi(nav_width_str) : 94; // fallback

    const char* space_lg_str = lv_xml_get_const(nullptr, "space_lg");
    int32_t gap = space_lg_str ? std::atoi(space_lg_str) : 16; // fallback to 16px

    // Two overlay widths, distinguished by what they mean rather than by how
    // much space they leave. See include/overlay_class.h and
    // prestonbrown/helixscreen#1178.
    //   transient layer — the backdrop shows at the leading edge: you opened
    //                     this over something and will return from it.
    //   destination     — occludes the backdrop: a place you park, and whose
    //                     drill-downs are part of it.
    const helix::OverlayWidths widths =
        helix::compute_overlay_widths(hor_res, ver_res, nav_width, gap);

    char transient_str[16];
    char destination_str[16];
    snprintf(transient_str, sizeof(transient_str), "%d", widths.transient);
    snprintf(destination_str, sizeof(destination_str), "%d", widths.destination);

    lv_xml_register_const(scope, "overlay_width_transient", transient_str);
    lv_xml_register_const(scope, "overlay_width_destination", destination_str);

    spdlog::trace("[Theme] Layout: nav_width={}px, gap={}px, overlay transient={}px "
                  "destination={}px",
                  nav_width, gap, widths.transient, widths.destination);
}

void theme_manager_refresh_orientation(lv_display_t* display) {
    // ui_is_portrait is consumed by XML for visual layout (flex flow, strip
    // stacking, <if cond="ui_is_portrait eq 1">). Those decisions must follow a
    // --layout override, so the source of truth is LayoutManager::type() once it
    // is initialized. Before Phase 8b LayoutManager carries only its default
    // STANDARD; detect_layout_type() gives the right physical answer, using the
    // caller's display when provided so a non-default display (tests, a
    // specific refresh target) is not confused with the default. See #1255.
    lv_subject_t* portrait_subject = lv_xml_get_subject(nullptr, "ui_is_portrait");
    if (!portrait_subject) {
        return;
    }

    int is_portrait;
    if (LayoutManager::instance().is_initialized()) {
        is_portrait = is_portrait_layout(LayoutManager::instance().type()) ? 1 : 0;
    } else {
        lv_display_t* disp = display ? display : lv_display_get_default();
        if (!disp) {
            return;
        }
        is_portrait =
            is_portrait_layout(detect_layout_type(lv_display_get_horizontal_resolution(disp),
                                                  lv_display_get_vertical_resolution(disp)))
                ? 1
                : 0;
    }
    lv_subject_set_int(portrait_subject, is_portrait);
}

void theme_manager_refresh_layout_constants(lv_display_t* display) {
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope)
        return;

    // Update every responsive px token for the new size — nav_width included.
    // Same resolver as startup, which is the point: this path used to carry its
    // own copy of the selection chain, and its nav_width write was then
    // overwritten a few lines later by a generic loop that knew nothing about
    // the ultrawide ladder. See theme_manager_resolve_px_tokens().
    int32_t resp_res = responsive_dimension(display);
    for (const auto& [base_name, value] : theme_manager_resolve_px_tokens(display)) {
        lv_xml_update_const(scope, base_name.c_str(), value.c_str());
    }

    // Recalculate overlay widths from updated nav_width and space_lg
    const char* nav_width_str = lv_xml_get_const(nullptr, "nav_width");
    int32_t nav_width = nav_width_str ? std::atoi(nav_width_str) : 94;

    const char* space_lg_str = lv_xml_get_const(nullptr, "space_lg");
    int32_t gap = space_lg_str ? std::atoi(space_lg_str) : 16;

    const helix::OverlayWidths widths =
        helix::compute_overlay_widths(hor_res, ver_res, nav_width, gap);

    char transient_str[16];
    char destination_str[16];
    snprintf(transient_str, sizeof(transient_str), "%d", widths.transient);
    snprintf(destination_str, sizeof(destination_str), "%d", widths.destination);

    lv_xml_update_const(scope, "overlay_width_transient", transient_str);
    lv_xml_update_const(scope, "overlay_width_destination", destination_str);

    // Update breakpoint subject — use shared helper so rotation never
    // downgrades XXLarge to XLarge (previous bug: missing XLARGE_MAX check).
    UiBreakpoint bp = breakpoint_for(resp_res);

    lv_subject_t* bp_subject = lv_xml_get_subject(nullptr, "ui_breakpoint");
    if (bp_subject) {
        lv_subject_set_int(bp_subject, to_int(bp));
    }

    // Rotation swaps the axes, so the vertical tier has to be recomputed too --
    // updating only ui_breakpoint would leave a panel rotated out of portrait
    // still claiming the height it no longer has.
    lv_subject_t* bp_v_subject = lv_xml_get_subject(nullptr, "ui_breakpoint_v");
    if (bp_v_subject) {
        lv_subject_set_int(bp_v_subject,
                           to_int(breakpoint_for(responsive_vertical_dimension(display))));
    }

    // Orientation follows the same axis swap. theme_manager_refresh_orientation
    // consults LayoutManager (override-aware) when it is up, falling back to
    // detect_layout_type() on the early-startup probe path; either way a
    // rotation cannot leave ui_is_portrait disagreeing with the axes it just
    // repointed (#1255).
    theme_manager_refresh_orientation(display);

    // Type has to follow the breakpoint too. The px tokens above moved the
    // boxes; without the two calls below the fonts stayed sized for the startup
    // breakpoint, so a resize rescaled layout but not type (#1210).
    //
    // Order is load-bearing: AssetManager decides which font tiers exist in
    // memory at all, and startup deliberately skips the tiers above its own. If
    // the tokens were re-pointed first, every raised one would name a face that
    // is not registered and get bounced back down to the _large tier by the
    // existence check in theme_manager_register_responsive_fonts().
    AssetManager::register_fonts_for_tier(to_int(bp));
    theme_manager_register_responsive_fonts(display);

    // Switch size presets are the same ladder expressed as plain C++ values,
    // and were likewise chosen once at startup (#1210, Notes).
    ui_switch_init_size_presets(display);

    spdlog::info("[Theme] Layout refreshed after rotation: {}x{} → nav={}px, "
                 "overlay transient={}px destination={}px (breakpoint={})",
                 hor_res, ver_res, nav_width, widths.transient, widths.destination, to_int(bp));
}

/**
 * Register responsive font tokens from all XML files
 *
 * Auto-discovers all <string name="xxx_small"> elements from all XML files in ui_xml/
 * and registers base tokens by matching xxx_small/xxx_medium/xxx_large triplets.
 * This makes the system fully extensible without C++ code changes.
 *
 * @param display The LVGL display to get resolution from
 */
void theme_manager_register_responsive_fonts(lv_display_t* display) {
    // Use the smaller dimension — the cramped axis is the design constraint
    // regardless of orientation (landscape: usually height; portrait: width).
    int32_t resp_res = responsive_dimension(display);
    const char* size_suffix = theme_manager_get_breakpoint_suffix(resp_res);
    const char* size_label = responsive_pick(breakpoint_for(resp_res), "MICRO", "TINY", "SMALL",
                                             "MEDIUM", "LARGE", "XLARGE", "XXLARGE");

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope) {
        spdlog::warn("[Theme] Failed to get globals scope for font constants");
        return;
    }

    // Auto-discover all string tokens from all XML files (including optional _micro, _tiny,
    // _xlarge, and _xxlarge)
    auto micro_tokens = theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "string", "_micro");
    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "string", "_tiny");
    auto small_tokens = theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "string", "_small");
    auto medium_tokens =
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "string", "_medium");
    auto large_tokens = theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "string", "_large");
    auto xlarge_tokens =
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "string", "_xlarge");
    auto xxlarge_tokens =
        theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "string", "_xxlarge");

    int registered = 0;
    for (const auto& [base_name, small_val] : small_tokens) {
        // Verify _small/_medium/_large triplet exists (required)
        auto medium_it = medium_tokens.find(base_name);
        auto large_it = large_tokens.find(base_name);

        if (medium_it != medium_tokens.end() && large_it != large_tokens.end()) {
            // Select appropriate variant based on breakpoint. Also track which
            // suffix actually supplied the value so we can tier-classify a
            // missing-font miss below.
            const char* value = nullptr;
            const char* selected_suffix = nullptr;
            if (strcmp(size_suffix, "_micro") == 0) {
                auto micro_it = micro_tokens.find(base_name);
                if (micro_it != micro_tokens.end()) {
                    value = micro_it->second.c_str();
                    selected_suffix = "_micro";
                } else {
                    auto tiny_it = tiny_tokens.find(base_name);
                    if (tiny_it != tiny_tokens.end()) {
                        value = tiny_it->second.c_str();
                        selected_suffix = "_tiny";
                    } else {
                        value = small_val.c_str();
                        selected_suffix = "_small";
                    }
                }
            } else if (strcmp(size_suffix, "_tiny") == 0) {
                // Use _tiny if available, otherwise fall back to _small
                auto tiny_it = tiny_tokens.find(base_name);
                if (tiny_it != tiny_tokens.end()) {
                    value = tiny_it->second.c_str();
                    selected_suffix = "_tiny";
                } else {
                    value = small_val.c_str();
                    selected_suffix = "_small";
                }
            } else if (strcmp(size_suffix, "_small") == 0) {
                value = small_val.c_str();
                selected_suffix = "_small";
            } else if (strcmp(size_suffix, "_medium") == 0) {
                value = medium_it->second.c_str();
                selected_suffix = "_medium";
            } else if (strcmp(size_suffix, "_large") == 0) {
                value = large_it->second.c_str();
                selected_suffix = "_large";
            } else if (strcmp(size_suffix, "_xlarge") == 0) {
                auto xlarge_it = xlarge_tokens.find(base_name);
                if (xlarge_it != xlarge_tokens.end()) {
                    value = xlarge_it->second.c_str();
                    selected_suffix = "_xlarge";
                } else {
                    value = large_it->second.c_str();
                    selected_suffix = "_large";
                }
            } else {
                // _xxlarge: use xxlarge if available, fall back to _xlarge, then _large
                auto xxlarge_it = xxlarge_tokens.find(base_name);
                if (xxlarge_it != xxlarge_tokens.end()) {
                    value = xxlarge_it->second.c_str();
                    selected_suffix = "_xxlarge";
                } else {
                    auto xlarge_it = xlarge_tokens.find(base_name);
                    if (xlarge_it != xlarge_tokens.end()) {
                        value = xlarge_it->second.c_str();
                        selected_suffix = "_xlarge";
                    } else {
                        value = large_it->second.c_str();
                        selected_suffix = "_large";
                    }
                }
            }

            // Only apply font existence check to actual font constants.
            // Other string constants (e.g. icon_size_xlarge = "xl") are not
            // font names and must be registered as-is.
            bool is_font_constant =
                (base_name.rfind("font_", 0) == 0) || (base_name.rfind("icon_font_", 0) == 0);

            // Verify the selected font is actually linked. If not, fall back to
            // _large (guaranteed present by the triplet check above) and emit
            // tier-aware diagnostics: warn when the miss falls within this
            // platform's compiled tier range (build bug), stay silent when it's
            // above the max tier (expected pruning).
            if (is_font_constant && lv_xml_get_font_silent(scope, value) == nullptr) {
                int tier = tier_num_for_suffix(selected_suffix);
                if (tier >= 0 && tier <= HELIX_MAX_FONT_TIER) {
                    spdlog::warn("[Theme] Font '{}' expected for tier '{}' but not linked "
                                 "(build bug?) — falling back to _large",
                                 value, selected_suffix);
                } else {
                    spdlog::trace("[Theme] Font '{}' pruned for tier '{}' (max tier {}) — "
                                  "falling back to _large",
                                  value, selected_suffix, HELIX_MAX_FONT_TIER);
                }
                const char* fallback = large_it->second.c_str();
                if (lv_xml_get_font_silent(scope, fallback) == nullptr) {
                    spdlog::error("[Theme] Fallback font '{}' for '{}' also not linked — "
                                  "skipping registration",
                                  fallback, base_name);
                    continue;
                }
                value = fallback;
                selected_suffix = "_large";
            }

            spdlog::trace("[Theme] Registering font {}: selected={} ({})", base_name, value,
                          selected_suffix);
            // update, not register: lv_xml_register_const() is first-write-wins,
            // so on the second pass (a runtime breakpoint change, or a theme
            // reload) it silently keeps the startup value. These base tokens
            // have no globals.xml declaration to protect — they exist only
            // because this function derives them — so overwriting is correct,
            // and lv_xml_update_const() falls back to registering on the first
            // pass (#1210).
            lv_xml_update_const(scope, base_name.c_str(), value);
            registered++;
        }
    }

    spdlog::trace("[Theme] Responsive fonts: {} (min_dim={}px) - auto-registered {} tokens",
                  size_label, resp_res, registered);

    // Three widgets memoize icon_font_* for the life of the process, and the
    // loop above just re-pointed those constants. Without this, the first
    // <icon size="sm"> ever built pins the face for every icon that follows, so
    // a breakpoint change resizes type everywhere except the icons (#1210).
    ui_icon_invalidate_font_cache();
    ui_button_invalidate_icon_font_cache();
    ui_split_button_invalidate_icon_font_cache();
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
static void theme_manager_register_semantic_colors(lv_xml_component_scope_t* scope,
                                                   const helix::ThemeData& theme, bool dark_mode) {
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

        // Register base name with current mode's value
        if (!current_val.empty()) {
            lv_xml_register_const(scope, name, current_val.c_str());
        }

        // Register _dark variant if dark palette is available
        if (has_dark) {
            const std::string& dark_val = theme.dark.at(index);
            if (!dark_val.empty()) {
                lv_xml_register_const(scope, dark_name, dark_val.c_str());
            }
        }

        // Register _light variant if light palette is available
        if (has_light) {
            const std::string& light_val = theme.light.at(index);
            if (!light_val.empty()) {
                lv_xml_register_const(scope, light_name, light_val.c_str());
            }
        }
    };

    // Register all 16 semantic colors from ModePalette
    auto& names = helix::ModePalette::color_names();
    for (size_t i = 0; i < 16; ++i) {
        register_color(names[i], i);
    }

    // Swatch descriptions for theme editor - registered as string subjects
    // so bind_text="swatch_N_desc" works in XML (consts don't resolve for bind_text)
    static constexpr const char* swatch_descriptions[SWATCH_DESC_COUNT] = {
        "App background",    "Panel/sidebar background", "Card surfaces",
        "Elevated surfaces", "Borders and dividers",     "Primary text",
        "Secondary text",    "Subtle/hint text",         "Primary accent",
        "Secondary accent",  "Tertiary accent",          "Info states",
        "Success states",    "Warning states",           "Danger/error states",
        "Focus ring",
    };

    if (!swatch_descs_initialized) {
        for (size_t i = 0; i < SWATCH_DESC_COUNT; ++i) {
            lv_subject_init_string(&swatch_desc_subjects[i], swatch_desc_bufs[i], nullptr,
                                   SWATCH_DESC_BUF_SIZE, swatch_descriptions[i]);
            char key[24];
            snprintf(key, sizeof(key), "swatch_%zu_desc", i);
            lv_xml_register_subject(nullptr, key, &swatch_desc_subjects[i]);
        }
        swatch_descs_initialized = true;
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
 * IMPORTANT: Must be called BEFORE theme_manager_register_static_constants() since
 * LVGL ignores duplicate lv_xml_register_const calls (first registration wins).
 *
 * @param scope LVGL XML scope to register constants in
 * @param theme Theme data with properties
 */
static void theme_manager_register_theme_properties(lv_xml_component_scope_t* scope,
                                                    const helix::ThemeData& theme, bool dark_mode) {
    char buf[32];

    // Register border_radius and button_radius from size table + current breakpoint
    int32_t resp_res = responsive_dimension(theme_display);
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
static void theme_manager_register_object_colors(lv_xml_component_scope_t* scope) {
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

/**
 * @brief Load active theme from config
 *
 * Reads /display/theme from config, loads corresponding JSON file.
 * Falls back to Nord if not found.
 *
 * HELIX_THEME env var overrides config (useful for testing/screenshots).
 */
static helix::ThemeData theme_manager_load_active_theme() {
    std::string themes_dir = helix::get_themes_directory();

    // Ensure themes directory exists with default theme
    helix::ensure_themes_directory(themes_dir);

    // Check for HELIX_THEME env var override (useful for testing/screenshots)
    std::string theme_name;
    const char* env_theme = std::getenv("HELIX_THEME");
    if (env_theme && env_theme[0] != '\0') {
        theme_name = env_theme;
        spdlog::info("[Theme] Using HELIX_THEME override: {}", theme_name);
    } else {
        // Read theme name from config
        Config* config = Config::get_instance();
        theme_name = config ? config->get<std::string>("/display/theme", helix::DEFAULT_THEME)
                            : helix::DEFAULT_THEME;
    }

    // Load theme file (supports fallback from user themes to defaults)
    auto theme = helix::load_theme_from_file(theme_name);

    if (!theme.is_valid()) {
        spdlog::warn("[Theme] Theme '{}' not found or invalid, using Nord", theme_name);
        theme = helix::get_default_nord_theme();
    }

    spdlog::info("[Theme] Loaded theme: {} ({})", theme.name, theme.filename);
    return theme;
}

void theme_manager_init(lv_display_t* display, bool use_dark_mode_param) {
    auto tm_init_start = std::chrono::steady_clock::now();
    theme_display = display;
    use_dark_mode = use_dark_mode_param;

    // Initialize theme change notification subject
    if (!theme_subject_initialized) {
        lv_subject_init_int(&theme_changed_subject, 0);
        theme_subject_initialized = true;
    }

    // Override runtime theme constants based on light/dark mode preference
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope) {
        spdlog::critical(
            "[Theme] FATAL: Failed to get globals scope for runtime constant registration");
        std::exit(EXIT_FAILURE);
    }

    // Load active theme from config/themes directory
    active_theme = theme_manager_load_active_theme();

    // Register semantic colors from dual-palette system (includes _light/_dark variants and base
    // names) NOTE: Legacy palette registration removed - was causing token collisions (text_light
    // conflict)
    theme_manager_register_semantic_colors(scope, active_theme, use_dark_mode);

    // Register theme properties (border_radius, etc.) - must be before static constants
    // so theme values override globals.xml defaults (first registration wins in LVGL)
    theme_manager_register_theme_properties(scope, active_theme, use_dark_mode);

    // Register static constants (colors, px, strings without dynamic suffixes)
    theme_manager_register_static_constants(scope);

    // Register fixed object color palette tokens (theme-invariant, hard-coded)
    theme_manager_register_object_colors(scope);

    // Auto-register all color pairs from globals.xml (xxx_light/xxx_dark -> xxx)
    // This handles screen_bg, text, header_text, elevated_bg, card_bg, etc.
    theme_manager_register_color_pairs(scope, use_dark_mode);

    // Register responsive constants (must be before theme init so fonts are available)
    theme_manager_register_responsive_spacing(display);
    theme_manager_register_responsive_fonts(display);

    // Initialize ui_breakpoint subject for reactive responsive visibility
    {
        int32_t resp_res = responsive_dimension(display);
        UiBreakpoint bp = breakpoint_for(resp_res);

        if (!breakpoint_subject_initialized) {
            lv_subject_init_int(&ui_breakpoint_subject, to_int(bp));
            breakpoint_subject_initialized = true;
        } else {
            lv_subject_set_int(&ui_breakpoint_subject, to_int(bp));
        }
        lv_xml_register_subject(nullptr, "ui_breakpoint", &ui_breakpoint_subject);
        spdlog::debug("[Theme] Registered ui_breakpoint subject: {} (min_dim={})", to_int(bp),
                      resp_res);
    }

    // Vertical companion. ui_breakpoint deliberately keeps the cramped tier --
    // every ref_value in ui_xml/ is written against it -- so height-aware layout
    // binds to this instead of changing what the old subject means.
    {
        int32_t vert_res = responsive_vertical_dimension(display);
        UiBreakpoint vbp = breakpoint_for(vert_res);

        if (!breakpoint_v_subject_initialized) {
            lv_subject_init_int(&ui_breakpoint_v_subject, to_int(vbp));
            breakpoint_v_subject_initialized = true;
        } else {
            lv_subject_set_int(&ui_breakpoint_v_subject, to_int(vbp));
        }
        lv_xml_register_subject(nullptr, "ui_breakpoint_v", &ui_breakpoint_v_subject);
        spdlog::debug("[Theme] Registered ui_breakpoint_v subject: {} (vert_dim={})", to_int(vbp),
                      vert_res);
    }

    // Startup SEED only. LayoutManager (Phase 8b) is not initialized yet, so
    // the override-aware value cannot be computed here; detect_layout_type() of
    // the live display gives the right physical answer, and Application
    // republishes via theme_manager_refresh_orientation() once LayoutManager
    // resolves any --layout override (#1255). The rotation path
    // (theme_manager_refresh_layout_constants) goes through the same helper.
    {
        int32_t hor_res = lv_display_get_horizontal_resolution(display);
        int32_t ver_res = lv_display_get_vertical_resolution(display);
        int is_portrait = is_portrait_layout(detect_layout_type(hor_res, ver_res)) ? 1 : 0;

        if (!is_portrait_subject_initialized) {
            lv_subject_init_int(&ui_is_portrait_subject, is_portrait);
            is_portrait_subject_initialized = true;
        } else {
            lv_subject_set_int(&ui_is_portrait_subject, is_portrait);
        }
        lv_xml_register_subject(nullptr, "ui_is_portrait", &ui_is_portrait_subject);
        spdlog::debug("[Theme] Registered ui_is_portrait subject: {} ({}x{})", is_portrait, hor_res,
                      ver_res);
    }

    // Validate critical color pairs were registered (fail-fast if missing)
    static const char* required_colors[] = {"screen_bg", "text", "text_muted", nullptr};
    for (const char** name = required_colors; *name != nullptr; ++name) {
        if (!lv_xml_get_const(nullptr, *name)) {
            spdlog::critical(
                "[Theme] FATAL: Missing required color pair {}_light/{}_dark in globals.xml", *name,
                *name);
            std::exit(EXIT_FAILURE);
        }
    }

    spdlog::trace("[Theme] Runtime constants set for {} mode", use_dark_mode ? "dark" : "light");

    // Read responsive font based on current breakpoint
    // NOTE: We read the variant directly because base constants are removed to enable
    // responsive overrides (LVGL ignores lv_xml_register_const for existing constants)
    int32_t resp_res = responsive_dimension(display);
    const char* size_suffix = theme_manager_get_breakpoint_suffix(resp_res);

    char font_variant_name[64];
    snprintf(font_variant_name, sizeof(font_variant_name), "font_body%s", size_suffix);
    const char* font_body_name = lv_xml_get_const(nullptr, font_variant_name);

    // Fallback chain (matches responsive spacing/font token resolution):
    //   _xxlarge → _xlarge → _large
    //   _xlarge  → _large
    //   _micro   → _tiny  → _small
    //   _tiny    → _small
    if (!font_body_name && strcmp(size_suffix, "_xxlarge") == 0) {
        font_body_name = lv_xml_get_const(nullptr, "font_body_xlarge");
        if (!font_body_name)
            font_body_name = lv_xml_get_const(nullptr, "font_body_large");
    } else if (!font_body_name && strcmp(size_suffix, "_xlarge") == 0) {
        font_body_name = lv_xml_get_const(nullptr, "font_body_large");
    } else if (!font_body_name && strcmp(size_suffix, "_micro") == 0) {
        font_body_name = lv_xml_get_const(nullptr, "font_body_tiny");
        if (!font_body_name)
            font_body_name = lv_xml_get_const(nullptr, "font_body_small");
    } else if (!font_body_name && strcmp(size_suffix, "_tiny") == 0) {
        font_body_name = lv_xml_get_const(nullptr, "font_body_small");
    }

    // The reads above resolve the suffixed variant const (e.g. font_body_medium),
    // which is supplied by globals.xml's component consts. On the firmware
    // release path that nullptr-scope read can miss it, but the responsive font
    // registrar (theme_manager_register_responsive_fonts, run just above) always
    // registers the breakpoint-selected BASE name "font_body" into this same
    // scope — so fall back to it. Desktop resolves the variant directly and
    // never reaches this line.
    if (!font_body_name) {
        font_body_name = lv_xml_get_const(nullptr, "font_body");
    }

    const lv_font_t* base_font =
        font_body_name ? lv_xml_get_font(nullptr, font_body_name) : nullptr;
    if (!base_font) {
        // Resolve the fallback through the font REGISTRY (returns NULL if the
        // face isn't linked/registered — e.g. a pruned firmware tier has no
        // "noto_sans_16"), NOT via the &noto_sans_16 symbol: on such builds that
        // symbol can resolve to a NULL/weak address and theme_init_lvgl would
        // dereference it (Guru Meditation, PC=0). If even that misses, use
        // LVGL's built-in default, which is always valid. NEVER hand a NULL font
        // to LVGL.
        const lv_font_t* fallback = lv_xml_get_font(nullptr, "noto_sans_16");
        if (fallback) {
            spdlog::warn("[Theme] Failed to get font '{}', using noto_sans_16", font_variant_name);
            base_font = fallback;
        } else {
            spdlog::error("[Theme] Failed to get font '{}' and fallback noto_sans_16 is not "
                          "registered; using lv_font_get_default()",
                          font_variant_name);
            base_font = lv_font_get_default();
        }
    }

    // Build palette from current mode
    const helix::ModePalette& mode_palette = get_current_mode_palette();
    theme_palette_t palette = build_palette_from_mode(mode_palette);

    // Initialize custom HelixScreen theme (wraps LVGL default theme)
    current_theme = theme_init_lvgl(display, &palette, use_dark_mode, base_font);

    if (current_theme) {
        lv_display_set_theme(display, current_theme);
        spdlog::debug("[Theme] Initialized HelixScreen theme: {} mode",
                      use_dark_mode ? "dark" : "light");
        spdlog::trace("[Theme] Colors: primary={}, screen={}, card={}", mode_palette.primary,
                      mode_palette.screen_bg, mode_palette.card_bg);
    } else {
        spdlog::error("[Theme] Failed to initialize HelixScreen theme");
    }

#ifdef __ANDROID__
    // Set Android window background to match theme so transparent system bars
    // don't reveal a white window background on startup
    android_set_window_bg_color(theme_manager_parse_hex_color(mode_palette.screen_bg.c_str()));
#endif

    spdlog::debug("[Theme] theme_manager_init took {} ms (token table {})",
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - tm_init_start)
                      .count(),
                  helix::theme_tokens::enabled() ? "on" : "off");
}

void theme_manager_deinit() {
    // Deinitialize subjects BEFORE lv_deinit() to prevent crash in lv_observer_remove().
    // lv_subject_deinit() removes all observers from each subject AND removes the
    // unsubscribe_on_delete_cb from widgets. Without this, lv_deinit() -> obj_delete_core()
    // fires stale callbacks that try to remove from corrupted observer linked lists.
    if (theme_subject_initialized) {
        lv_subject_deinit(&theme_changed_subject);
        theme_subject_initialized = false;
        theme_generation = 0;
    }
    if (breakpoint_subject_initialized) {
        lv_subject_deinit(&ui_breakpoint_subject);
        breakpoint_subject_initialized = false;
    }
    if (breakpoint_v_subject_initialized) {
        lv_subject_deinit(&ui_breakpoint_v_subject);
        breakpoint_v_subject_initialized = false;
    }
    if (is_portrait_subject_initialized) {
        lv_subject_deinit(&ui_is_portrait_subject);
        is_portrait_subject_initialized = false;
    }
    if (swatch_descs_initialized) {
        for (size_t i = 0; i < SWATCH_DESC_COUNT; ++i) {
            lv_subject_deinit(&swatch_desc_subjects[i]);
        }
        swatch_descs_initialized = false;
    }
    spdlog::trace("[Theme] Deinitialized theme subjects");
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

void theme_manager_refresh_widget_tree(lv_obj_t* root) {
    if (!root)
        return;

    // Walk entire tree and refresh each widget's styles
    lv_obj_tree_walk(root, refresh_style_cb, nullptr);
}

void theme_manager_apply_theme(const helix::ThemeData& theme, bool dark_mode) {
    if (!theme_display) {
        spdlog::error("[Theme] Cannot apply theme: theme not initialized");
        return;
    }

    // Respect theme mode support constraints
    bool effective_dark = dark_mode;
    auto mode_support = theme.get_mode_support();
    if (mode_support == helix::ThemeModeSupport::DARK_ONLY) {
        effective_dark = true;
    } else if (mode_support == helix::ThemeModeSupport::LIGHT_ONLY) {
        effective_dark = false;
    }

    // Capture old palette colors before overwriting, for swap map (copy, not ref!)
    const helix::ModePalette old_mp = use_dark_mode ? active_theme.dark : active_theme.light;
    bool have_old = !old_mp.screen_bg.empty();

    active_theme = theme;
    use_dark_mode = effective_dark;
    spdlog::info("[Theme] Applying theme '{}' in {} mode", theme.name,
                 effective_dark ? "dark" : "light");

    // Log palette for debugging
    const helix::ModePalette& mode_palette = get_current_mode_palette();
    spdlog::debug("[Theme] Colors: screen={}, card={}, text={}", mode_palette.screen_bg,
                  mode_palette.card_bg, mode_palette.text);

    // Build color swap map (old baked values → new values), deduplicating collisions
    bg_swap_map.clear();
    border_swap_map.clear();
    if (have_old) {
        auto p = theme_manager_parse_hex_color;
        const helix::ModePalette& new_mp = mode_palette;
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

    // Update ThemeManager stored palettes and apply current mode
    theme_update_colors(effective_dark);

    // Re-register XML constants: semantic colors, theme properties, and color pairs
    theme_manager_register_semantic_colors(nullptr, active_theme, effective_dark);
    theme_manager_register_theme_properties(nullptr, active_theme, effective_dark);

    // Update border_radius constant for live preview (register_const is first-wins,
    // so we need update_const for subsequent changes)
    {
        const char* bp_suffix =
            theme_manager_get_breakpoint_suffix(responsive_dimension(theme_display));
        int radius_px =
            helix::BorderRadiusSizes::pixels(active_theme.properties.border_radius_size, bp_suffix);
        char radius_buf[16];
        snprintf(radius_buf, sizeof(radius_buf), "%d", radius_px);
        lv_xml_update_const(nullptr, "border_radius", radius_buf);
    }

    // Same first-wins caveat as border_radius above: the shadow opacity differs
    // between light and dark, so a live mode flip has to update it. #1178
    lv_xml_update_const(nullptr, "overlay_shadow_opa", effective_dark ? "200" : "100");

    theme_manager_register_color_pairs(nullptr, effective_dark);

    // Update screen background directly (XML inline styles are baked at parse time)
    lv_color_t screen_bg = theme_manager_parse_hex_color(mode_palette.screen_bg.c_str());
    lv_obj_set_style_bg_color(lv_screen_active(), screen_bg, LV_PART_MAIN);

#ifdef __ANDROID__
    // Sync Android window background so area behind transparent system bars matches
    android_set_window_bg_color(screen_bg);
#endif

    // Refresh widget tree: shared styles + local/inline styles + palette-styled widgets
    theme_manager_refresh_widget_tree(lv_screen_active());
    theme_apply_current_palette_to_tree(lv_screen_active());

    // Refresh ui_gradient_canvas widgets for the new dark/light mode
    ui_gradient_canvas_theme_update(lv_screen_active());

    // Invalidate and notify
    lv_obj_invalidate(lv_screen_active());
    theme_manager_notify_change();

    spdlog::info("[Theme] Theme apply complete (generation={})", theme_generation);
}

void theme_manager_toggle_dark_mode() {
    theme_manager_apply_theme(active_theme, !use_dark_mode);
}

bool theme_manager_is_dark_mode() {
    return use_dark_mode;
}

const helix::ThemeData& theme_manager_get_active_theme() {
    return active_theme;
}

helix::ThemeModeSupport theme_manager_get_mode_support() {
    return active_theme.get_mode_support();
}

bool theme_manager_supports_dark_mode() {
    return active_theme.supports_dark();
}

bool theme_manager_supports_light_mode() {
    return active_theme.supports_light();
}

lv_subject_t* theme_manager_get_changed_subject() {
    return &theme_changed_subject;
}

lv_subject_t* theme_manager_get_breakpoint_subject() {
    return &ui_breakpoint_subject;
}

void theme_manager_notify_change() {
    if (!theme_subject_initialized)
        return;
    theme_generation++;
    lv_subject_set_int(&theme_changed_subject, theme_generation);
    spdlog::debug("[Theme] Notified theme change (generation={})", theme_generation);
}

void theme_manager_preview(const helix::ThemeData& theme) {
    theme_manager_apply_theme(theme, use_dark_mode);
}

void theme_manager_preview(const helix::ThemeData& theme, bool is_dark) {
    theme_manager_apply_theme(theme, is_dark);
}

// theme_manager_refresh_preview_elements() removed — was ~450 lines of
// widget-by-name updates. Replaced by theme_manager_apply_theme() which
// uses theme_apply_current_palette_to_tree() for generic palette application.

// ============================================================================
// Palette Application Functions (for DRY preview styling)
// ============================================================================

/**
 * Check if a font is one of the MDI icon fonts (forward declaration)
 */
static bool is_icon_font(const lv_font_t* font);
static bool is_muted_text_font(const lv_font_t* font);

/**
 * Helper to update button label text with contrast-aware color
 *
 * Uses theme_manager_get_contrast_color() which correctly picks dark text
 * (from light palette) for light backgrounds, and light text (from dark
 * palette) for dark backgrounds.
 */
static void apply_button_text_contrast(lv_obj_t* btn) {
    if (!btn)
        return;

    // Get button's background color and pick contrast text via theme system
    lv_color_t bg_color = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);
    lv_color_t text_color = theme_manager_get_contrast_color(bg_color);

    // Check for disabled state - use muted color
    bool btn_disabled = lv_obj_has_state(btn, LV_STATE_DISABLED);
    if (btn_disabled) {
        // Blend toward gray for disabled state
        text_color = lv_color_mix(text_color, lv_color_hex(0x888888), 128);
    }

    // Get current text colors to detect text/muted-variant icons
    lv_color_t current_text = theme_manager_get_color("text");
    lv_color_t current_muted = theme_manager_get_color("text_muted");

    // Also check contrast text from both palettes for icon detection
    auto& tm = ThemeManager::instance();
    lv_color_t dark_text = tm.dark_palette().text;
    lv_color_t light_text = tm.light_palette().text;

    // Helper lambda to check if icon color is a "text-like" color that should get contrast
    auto is_text_variant_color = [&](lv_color_t c) {
        return lv_color_eq(c, current_text) || lv_color_eq(c, current_muted) ||
               lv_color_eq(c, dark_text) || lv_color_eq(c, light_text);
    };

    // Update all label children in the button
    // For icons: only apply contrast if they're using text/muted variant
    // Skip icons with semantic colors (primary, warning, etc.)
    uint32_t count = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const lv_font_t* font = lv_obj_get_style_text_font(child, LV_PART_MAIN);
            if (is_icon_font(font)) {
                // Icon: only apply contrast if it's using text/muted variant
                lv_color_t icon_color = lv_obj_get_style_text_color(child, LV_PART_MAIN);
                if (is_text_variant_color(icon_color)) {
                    lv_obj_set_style_text_color(child, text_color, LV_PART_MAIN);
                }
            } else {
                // Regular label: use muted color for muted-style fonts, contrast for others
                const lv_font_t* font = lv_obj_get_style_text_font(child, LV_PART_MAIN);
                lv_obj_set_style_text_color(
                    child, is_muted_text_font(font) ? current_muted : text_color, LV_PART_MAIN);
            }
        }
        // Also check nested containers (some buttons have container > label structure)
        uint32_t nested_count = lv_obj_get_child_count(child);
        for (uint32_t j = 0; j < nested_count; j++) {
            lv_obj_t* nested = lv_obj_get_child(child, j);
            if (lv_obj_check_type(nested, &lv_label_class)) {
                const lv_font_t* nested_font = lv_obj_get_style_text_font(nested, LV_PART_MAIN);
                if (is_icon_font(nested_font)) {
                    lv_color_t icon_color = lv_obj_get_style_text_color(nested, LV_PART_MAIN);
                    if (is_text_variant_color(icon_color)) {
                        lv_obj_set_style_text_color(nested, text_color, LV_PART_MAIN);
                    }
                } else {
                    lv_obj_set_style_text_color(
                        nested, is_muted_text_font(nested_font) ? current_muted : text_color,
                        LV_PART_MAIN);
                }
            }
        }
    }
}

/**
 * Check if a font is one of the MDI icon fonts
 */
static bool is_icon_font(const lv_font_t* font) {
    if (!font)
        return false;
    return font == &mdi_icons_14 || font == &mdi_icons_16 || font == &mdi_icons_24 ||
           font == &mdi_icons_32 || font == &mdi_icons_48 || font == &mdi_icons_64;
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

/**
 * @brief Check if an object is on an elevated background surface
 *
 * Detects two cases where inputs need overlay_bg for contrast:
 * 1. Inside a dialog (marked with LV_OBJ_FLAG_USER_1 in ui_dialog_xml_create())
 * 2. Inside any container whose opaque background matches elevated_bg
 *
 * This allows text_input, dropdowns, etc. to auto-contrast on raised cards
 * without manual style_bg_color overrides in XML.
 */
static bool is_on_elevated_surface(lv_obj_t* obj) {
    auto& tm = ThemeManager::instance();
    lv_color_t elevated = tm.current_palette().elevated_bg;
    lv_obj_t* parent = lv_obj_get_parent(obj);
    while (parent) {
        if (lv_obj_has_flag(parent, LV_OBJ_FLAG_USER_1))
            return true;
        lv_opa_t opa = lv_obj_get_style_bg_opa(parent, LV_PART_MAIN);
        if (opa > LV_OPA_50) {
            lv_color_t bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
            if (color_eq(bg, elevated))
                return true;
        }
        parent = lv_obj_get_parent(parent);
    }
    return false;
}

void theme_apply_palette_to_widget(lv_obj_t* obj, const helix::ModePalette& palette) {
    if (!obj)
        return;

    // Parse palette colors
    lv_color_t screen_bg = theme_manager_parse_hex_color(palette.screen_bg.c_str());
    lv_color_t overlay_bg = theme_manager_parse_hex_color(palette.overlay_bg.c_str());
    lv_color_t elevated_bg = theme_manager_parse_hex_color(palette.elevated_bg.c_str());
    lv_color_t border = theme_manager_parse_hex_color(palette.border.c_str());
    lv_color_t text_primary = theme_manager_parse_hex_color(palette.text.c_str());
    lv_color_t text_muted = theme_manager_parse_hex_color(palette.text_muted.c_str());
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
        if (is_icon_font(font)) {
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
                    lv_obj_set_style_text_color(obj, lv_color_white(), LV_PART_MAIN);
                    return;
                }
                break; // found opaque ancestor, not dark — fall through to normal
            }
        }

        // Small/heading fonts get muted color, body fonts get primary
        if (is_muted_text_font(font)) {
            lv_obj_set_style_text_color(obj, text_muted, LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
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
            lv_obj_set_style_bg_color(obj, elevated_bg, LV_PART_MAIN);
        }

        lv_obj_set_style_border_color(obj, border, LV_PART_MAIN);
        apply_button_text_contrast(obj);
        return;
    }

    // ==========================================================================
    // INTERACTIVE WIDGETS - specific styling per widget type
    // ==========================================================================

    // Checkboxes - box border, primary bg when checked, contrast checkmark
    if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        lv_obj_set_style_border_color(obj, border, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(obj, elevated_bg, LV_PART_INDICATOR);
        // Checked state: primary background with contrasting checkmark
        lv_obj_set_style_bg_color(obj, primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(obj, primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        uint8_t lum = lv_color_luminance(primary);
        lv_color_t check_color = (lum > 140) ? lv_color_black() : lv_color_white();
        lv_obj_set_style_text_color(obj, check_color, LV_PART_INDICATOR | LV_STATE_CHECKED);
        return;
    }

    // Switches - track, indicator, knob
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_set_style_bg_color(obj, border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, secondary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(obj, knob_color, LV_PART_KNOB);
        lv_obj_set_style_bg_color(obj, knob_color, LV_PART_KNOB | LV_STATE_CHECKED);
        return;
    }

    // Sliders - track, indicator, knob
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_set_style_bg_color(obj, border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, secondary, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(obj, knob_color, LV_PART_KNOB);
        lv_obj_set_style_shadow_color(obj, screen_bg, LV_PART_KNOB);
        return;
    }

    // Dropdowns - background, border, text
    // On elevated surfaces (dialogs, raised cards), use overlay_bg for contrast
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_color_t bg = is_on_elevated_surface(obj) ? overlay_bg : elevated_bg;
        lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
        lv_obj_set_style_border_color(obj, border, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        return;
    }

    // Textareas - background, text
    // On elevated surfaces (dialogs, raised cards), use overlay_bg for contrast
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_color_t bg = is_on_elevated_surface(obj) ? overlay_bg : elevated_bg;
        lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        return;
    }

    // Spinboxes - background, text
    // On elevated surfaces (dialogs, raised cards), use overlay_bg for contrast
    if (lv_obj_check_type(obj, &lv_spinbox_class)) {
        lv_color_t bg = is_on_elevated_surface(obj) ? overlay_bg : elevated_bg;
        lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        return;
    }

    // Dropdown lists (popup menus)
    if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_color_t dropdown_accent = theme_compute_more_saturated(primary, secondary);
        lv_obj_set_style_bg_color(obj, elevated_bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, dropdown_accent, LV_PART_SELECTED);
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
            lv_obj_set_style_bg_color(obj, border, LV_PART_MAIN);
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
                lv_obj_set_style_bg_color(obj, entry.to, LV_PART_MAIN);
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
                lv_obj_set_style_border_color(obj, entry.to, LV_PART_MAIN);
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

void theme_apply_current_palette_to_tree(lv_obj_t* root) {
    if (!root)
        return;

    // Get the active palette based on current mode
    const helix::ModePalette& palette = use_dark_mode ? active_theme.dark : active_theme.light;

    const char* root_name = lv_obj_get_name(root);
    spdlog::debug("[Theme] Applying current palette to tree root={}",
                  root_name ? root_name : "(screen)");
    theme_apply_palette_to_tree(root, palette);
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
            lv_obj_set_style_bg_color(child, elevated_bg, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(child, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(child, text_color, LV_PART_MAIN);
            lv_obj_set_style_border_color(child, border, LV_PART_MAIN);
            lv_obj_set_style_bg_color(child, dropdown_accent, LV_PART_SELECTED);
            lv_obj_set_style_bg_opa(child, LV_OPA_COVER, LV_PART_SELECTED);
            lv_obj_set_style_text_color(child, selected_text, LV_PART_SELECTED);
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

/**
 * Get theme-appropriate color variant with fallback for static colors
 *
 * First attempts to look up {base_name}_light and {base_name}_dark from globals.xml,
 * selecting the appropriate one based on current theme mode. If the theme variants
 * don't exist, falls back to {base_name} directly (for static colors like
 * warning, danger that are the same in both themes).
 *
 * @param base_name Color constant base name (e.g., "screen_bg", "warning")
 * @return Parsed color, or black (0x000000) if not found
 *
 * Example:
 *   lv_color_t bg = theme_manager_get_color("screen_bg");
 *   // Returns screen_bg_light in light mode, screen_bg_dark in dark mode
 *
 *   lv_color_t warn = theme_manager_get_color("warning");
 *   // Returns warning directly (static, no theme variants)
 */
lv_color_t theme_manager_get_color(const char* base_name) {
    if (!base_name) {
        spdlog::error("[Theme] theme_manager_get_color: NULL base_name");
        return lv_color_hex(0x000000);
    }

    // Construct variant names: {base_name}_light and {base_name}_dark
    char light_name[128];
    char dark_name[128];
    snprintf(light_name, sizeof(light_name), "%s_light", base_name);
    snprintf(dark_name, sizeof(dark_name), "%s_dark", base_name);

    // Use silent lookups to avoid LVGL warnings when probing for variants
    // Pattern 1: Theme-aware color with _light/_dark variants
    const char* light_str = lv_xml_get_const_silent(nullptr, light_name);
    const char* dark_str = lv_xml_get_const_silent(nullptr, dark_name);

    if (light_str && dark_str) {
        // Both variants exist - use theme-appropriate one
        return theme_manager_parse_hex_color(use_dark_mode ? dark_str : light_str);
    }

    // Pattern 2: Static color with just base name (no variants)
    const char* base_str = lv_xml_get_const_silent(nullptr, base_name);
    if (base_str) {
        return theme_manager_parse_hex_color(base_str);
    }

    // Pattern 3: Partial variants (error case)
    if (light_str || dark_str) {
        spdlog::error("[Theme] Color {} has only one variant (_light or _dark), need both",
                      base_name);
        return lv_color_hex(0x000000);
    }

    // Nothing found — only log error if theme is initialized (otherwise this is
    // benign, e.g. tests or early init before theme_manager_init() is called)
    if (current_theme) {
        spdlog::error("[Theme] Color not found: {} (no base, no _light/_dark variants)", base_name);
    } else {
        spdlog::trace("[Theme] Color not found (theme not initialized): {}", base_name);
    }
    return lv_color_hex(0x000000);
}

lv_color_t theme_manager_get_object_palette_color(int index) {
    constexpr int OBJECT_PALETTE_SIZE = 8;
    char token[32];
    snprintf(token, sizeof(token), "object_color_%d", (index % OBJECT_PALETTE_SIZE) + 1);
    return theme_manager_get_color(token);
}

/**
 * Apply theme-appropriate background color to object
 *
 * Convenience wrapper that gets the color variant and applies it to the object.
 *
 * @param obj LVGL object to apply color to
 * @param base_name Color constant base name (e.g., "screen_bg", "card_bg")
 * @param part Style part to apply to (default: LV_PART_MAIN)
 *
 * Example:
 *   theme_manager_apply_bg_color(screen, "screen_bg", LV_PART_MAIN);
 *   // Applies screen_bg_light/dark depending on theme mode
 */
void theme_manager_apply_bg_color(lv_obj_t* obj, const char* base_name, lv_part_t part) {
    if (!obj) {
        spdlog::error("[Theme] theme_manager_apply_bg_color: NULL object");
        return;
    }

    lv_color_t color = theme_manager_get_color(base_name);
    lv_obj_set_style_bg_color(obj, color, part);
}

/**
 * Get font line height in pixels
 *
 * Returns the total vertical space a line of text will occupy for the given font.
 * This includes ascender, descender, and line gap. Useful for calculating layout
 * heights before widgets are created.
 *
 * @param font Font to query (e.g., theme_manager_get_font("font_heading"), &noto_sans_16)
 * @return Line height in pixels, or 0 if font is NULL
 *
 * Examples:
 *   int32_t heading_h = theme_manager_get_font_height(theme_manager_get_font("font_heading"));
 *   int32_t body_h = theme_manager_get_font_height(theme_manager_get_font("font_body"));
 *   int32_t small_h = theme_manager_get_font_height(theme_manager_get_font("font_small"));
 *
 *   // Calculate total height for multi-line layout
 *   int32_t total = theme_manager_get_font_height(theme_manager_get_font("font_heading")) +
 *                   (theme_manager_get_font_height(theme_manager_get_font("font_body")) * 3) +
 *                   (4 * 8);  // 4 gaps of 8px padding
 */
int32_t theme_manager_get_font_height(const lv_font_t* font) {
    if (!font) {
        spdlog::warn("[Theme] theme_manager_get_font_height: NULL font pointer");
        return 0;
    }

    return lv_font_get_line_height(font);
}

// DECLARATIVE_OK: overlay placement is navigation chrome, resolved at push time
// from the live stack — there is no XML expression for "the class this overlay
// got depends on what pushed it". check_imperative_ui.py deliberately excludes
// geometry and layout properties for exactly this reason; see its
// APPEARANCE_PROPS comment. This is the single site that writes overlay
// geometry, which is why 17 overlay XML roots need no portrait variant.
void ui_set_overlay_geometry(lv_obj_t* obj, bool is_destination) {
    if (!obj) {
        spdlog::warn("[Theme] ui_set_overlay_geometry: NULL pointer");
        return;
    }

    lv_obj_t* screen = lv_obj_get_screen(obj);
    const lv_coord_t screen_width = screen ? lv_obj_get_width(screen) : 800;
    const lv_coord_t screen_height = screen ? lv_obj_get_height(screen) : 480;
    const bool portrait =
        helix::is_portrait_layout(helix::detect_layout_type(screen_width, screen_height));

    const char* name = is_destination ? "overlay_width_destination" : "overlay_width_transient";
    const char* width_str = lv_xml_get_const(nullptr, name);
    if (width_str) {
        lv_obj_set_width(obj, std::atoi(width_str));
    } else {
        // Theme not initialized yet — estimate from the screen. Same derivation
        // as theme_manager_register_responsive_spacing(), with medium-breakpoint
        // fallbacks for nav_width and the gap.
        const helix::OverlayWidths widths =
            helix::compute_overlay_widths(screen_width, screen_height, 94, 16);
        lv_obj_set_width(obj, is_destination ? widths.destination : widths.transient);
        spdlog::warn("[Theme] {} not registered, using fallback", name);
    }

    // Landscape leaves height and alignment to XML (height="100%"
    // align="right_mid"). Only portrait overrides them, because there the nav
    // bar is a bottom strip and a full-height overlay would cover it.
    if (!portrait) {
        return;
    }

    const char* nav_h_str = lv_xml_get_const(nullptr, "button_height_lg");
    const char* gap_str = lv_xml_get_const(nullptr, "space_lg");
    const int32_t nav_height = nav_h_str ? std::atoi(nav_h_str) : 70;
    const int32_t gap = gap_str ? std::atoi(gap_str) : 16;

    const helix::OverlayHeights heights =
        helix::compute_overlay_heights(screen_width, screen_height, nav_height, gap);
    lv_obj_set_height(obj, is_destination ? heights.destination : heights.transient);
    lv_obj_set_align(obj, LV_ALIGN_TOP_MID);
}

/**
 * Get spacing value from unified space_* system
 *
 * Reads the registered space_* constant value from LVGL's XML constant registry.
 * The value returned is responsive - it depends on what breakpoint was used
 * during theme initialization (small/medium/large).
 *
 * This function is the C++ interface to the unified spacing system, replacing
 * the old hardcoded UI_PADDING_* constants. All spacing in C++ code should now
 * use this function to stay consistent with XML layouts.
 *
 * Available tokens and their responsive values:
 *   space_xxs: 2/3/4px  (small/medium/large)
 *   space_xs:  4/5/6px
 *   space_sm:  6/7/8px
 *   space_md:  8/10/12px
 *   space_lg:  12/16/20px
 *   space_xl:  16/20/24px
 *   space_2xl: 24/32/40px
 *
 * @param token Spacing token name (e.g., "space_lg", "space_md", "space_xs")
 * @return Spacing value in pixels, or 0 if token not found
 *
 * Example:
 *   lv_obj_set_style_pad_all(obj, theme_manager_get_spacing("space_lg"), 0);
 */
int32_t theme_manager_get_spacing(const char* token) {
    if (!token) {
        spdlog::warn("[Theme] theme_manager_get_spacing: NULL token");
        return 0;
    }

    const char* value = lv_xml_get_const_silent(nullptr, token);
    if (!value) {
        if (current_theme) {
            spdlog::warn("[Theme] Spacing token '{}' not found - is theme initialized?", token);
        } else {
            spdlog::trace("[Theme] Spacing token '{}' not found (theme not initialized)", token);
        }
        return 0;
    }

    return std::atoi(value);
}

/**
 * Get responsive font by token name
 *
 * Looks up the font token (e.g., "font_small") which was registered during
 * theme init with the appropriate breakpoint variant value (e.g., "noto_sans_16"),
 * then retrieves the actual font pointer.
 *
 * @param token Font token name (e.g., "font_small", "font_body", "font_heading")
 * @return Font pointer, or nullptr if not found
 */
const lv_font_t* theme_manager_get_font(const char* token) {
    if (!token) {
        spdlog::warn("[Theme] theme_manager_get_font: NULL token, using default font");
        return lv_font_get_default();
    }

    // Get the font name from the registered constant (e.g., "font_small" -> "noto_sans_16")
    const char* font_name = lv_xml_get_const_silent(nullptr, token);
    if (!font_name) {
        if (current_theme) {
            spdlog::warn("[Theme] Font token '{}' not found - falling back to default font", token);
        } else {
            spdlog::trace("[Theme] Font token '{}' not found (theme not initialized)", token);
        }
        return lv_font_get_default();
    }

    // Get the actual font pointer
    const lv_font_t* font = lv_xml_get_font(nullptr, font_name);
    if (!font) {
        spdlog::warn("[Theme] Font '{}' (from token '{}') not registered - falling back to default",
                     font_name, token);
        return lv_font_get_default();
    }

    return font;
}

const char* theme_manager_size_to_font_token(const char* size, const char* default_size) {
    const char* effective_size = size ? size : default_size;
    if (!effective_size) {
        effective_size = "sm"; // Fallback if both are null
    }

    if (strcmp(effective_size, "xs") == 0) {
        return "font_xs";
    } else if (strcmp(effective_size, "sm") == 0) {
        return "font_small";
    } else if (strcmp(effective_size, "md") == 0) {
        return "font_body";
    } else if (strcmp(effective_size, "lg") == 0) {
        return "font_heading";
    } else if (strcmp(effective_size, "xl") == 0) {
        return "font_xl";
    }

    // Unknown size - warn and return default
    spdlog::warn("[Theme] Unknown size '{}', using default '{}'", effective_size, default_size);
    return theme_manager_size_to_font_token(default_size, "sm");
}

// ============================================================================
// Multi-File Responsive Constants
// ============================================================================
// Extension of responsive constants (_small/_medium/_large) to work with ALL
// XML files, not just globals.xml. This allows component-specific responsive
// tokens to be defined in their respective XML files.

// Expat callback data for extracting name→value pairs with a specific suffix
struct SuffixValueParserData {
    const char* element_type;                              // "color", "px", or "string"
    const char* suffix;                                    // "_light", "_small", etc.
    std::unordered_map<std::string, std::string>* results; // Output: base_name → value
};

// Helper: check if string ends with suffix
static bool ends_with_suffix(const char* str, const char* suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (str_len < suffix_len)
        return false;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

// Parser callback for ALL elements of a given type (no suffix matching)
struct AllElementParserData {
    const char* element_type;
    std::unordered_map<std::string, std::string>* token_values;
};

static void XMLCALL all_element_start(void* userData, const XML_Char* name, const XML_Char** atts) {
    auto* data = static_cast<AllElementParserData*>(userData);
    if (strcmp(name, data->element_type) != 0)
        return;

    const char* elem_name = nullptr;
    const char* elem_value = nullptr;
    for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "name") == 0)
            elem_name = atts[i + 1];
        else if (strcmp(atts[i], "value") == 0)
            elem_value = atts[i + 1];
    }
    if (elem_name && elem_value) {
        (*data->token_values)[elem_name] = elem_value;
    }
}

// Expat element start handler - extracts name and value for matching elements
static void XMLCALL suffix_value_element_start(void* user_data, const XML_Char* name,
                                               const XML_Char** attrs) {
    SuffixValueParserData* data = static_cast<SuffixValueParserData*>(user_data);

    if (strcmp(name, data->element_type) != 0)
        return;

    // Extract both name and value attributes
    const char* const_name = nullptr;
    const char* const_value = nullptr;
    for (int i = 0; attrs[i]; i += 2) {
        if (strcmp(attrs[i], "name") == 0)
            const_name = attrs[i + 1];
        if (strcmp(attrs[i], "value") == 0)
            const_value = attrs[i + 1];
    }

    // Skip if either attribute is missing
    if (!const_name || !const_value)
        return;

    // Check if name ends with the target suffix
    if (ends_with_suffix(const_name, data->suffix)) {
        // Extract base name (without suffix)
        size_t base_len = strlen(const_name) - strlen(data->suffix);
        std::string base_name(const_name, base_len);

        // Store in results (overwrites any existing value - last-wins)
        (*data->results)[base_name] = const_value;
    }
}

void theme_manager_parse_xml_file_for_all(
    const char* filepath, const char* element_type,
    std::unordered_map<std::string, std::string>& token_values) {
    if (!filepath)
        return;

    std::ifstream file(filepath);
    if (!file.is_open())
        return;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string xml_content = buffer.str();
    file.close();
    if (xml_content.empty())
        return;

    AllElementParserData parser_data = {element_type, &token_values};
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser)
        return;

    XML_SetUserData(parser, &parser_data);
    XML_SetElementHandler(parser, all_element_start, nullptr);
    XML_Parse(parser, xml_content.c_str(), static_cast<int>(xml_content.size()), XML_TRUE);
    XML_ParserFree(parser);
}

void theme_manager_parse_xml_file_for_suffix(
    const char* filepath, const char* element_type, const char* suffix,
    std::unordered_map<std::string, std::string>& token_values) {
    // Handle NULL filepath gracefully
    if (!filepath) {
        spdlog::trace("[Theme] parse_xml_file_for_suffix: NULL filepath");
        return;
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
        spdlog::trace("[Theme] Could not open {} for suffix parsing", filepath);
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string xml_content = buffer.str();
    file.close();

    // Handle empty file
    if (xml_content.empty()) {
        return;
    }

    SuffixValueParserData parser_data = {element_type, suffix, &token_values};
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser) {
        spdlog::error("[Theme] Failed to create XML parser for {}", filepath);
        return;
    }
    XML_SetUserData(parser, &parser_data);
    XML_SetElementHandler(parser, suffix_value_element_start, nullptr);

    if (XML_Parse(parser, xml_content.c_str(), static_cast<int>(xml_content.size()), XML_TRUE) ==
        XML_STATUS_ERROR) {
        spdlog::trace("[Theme] XML parse error in {} line {}: {}", filepath,
                      XML_GetCurrentLineNumber(parser), XML_ErrorString(XML_GetErrorCode(parser)));
        // Continue with partial results (don't clear token_values)
    }
    XML_ParserFree(parser);
}

std::vector<std::string> theme_manager_find_xml_files(const char* directory) {
    std::vector<std::string> result;

    // Handle NULL directory gracefully
    if (!directory) {
        spdlog::trace("[Theme] find_xml_files: NULL directory");
        return result;
    }

    DIR* dir = opendir(directory);
    if (!dir) {
        spdlog::trace("[Theme] Could not open directory: {}", directory);
        return result;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;

        // Skip directories (including . and ..)
        if (entry->d_type == DT_DIR)
            continue;

        // Skip suspicious filenames (path traversal defense)
        if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
            continue;
        }

        // Check if file ends with .xml (case-sensitive, lowercase only)
        if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".xml") {
            std::string full_path = std::string(directory) + "/" + filename;
            result.push_back(full_path);
        }
    }
    closedir(dir);

    // Sort alphabetically for deterministic ordering (needed for last-wins)
    std::sort(result.begin(), result.end());

    return result;
}

std::unordered_map<std::string, std::string>
theme_manager_parse_all_xml_for_element(const char* directory, const char* element_type) {
    // Build-time token table: skip the ~28-scan boot storm when the table is
    // enabled and the caller wants the canonical ui_xml dir (tests and
    // alternate dirs always scan live).
    if (helix::theme_tokens::enabled() && directory &&
        std::strcmp(directory, tm_ui_xml_dir()) == 0) {
        return helix::theme_tokens::for_element(element_type);
    }
    std::unordered_map<std::string, std::string> token_values;
    std::vector<std::string> files = theme_manager_find_xml_files(directory);
    for (const auto& filepath : files) {
        theme_manager_parse_xml_file_for_all(filepath.c_str(), element_type, token_values);
    }
    return token_values;
}

std::unordered_map<std::string, std::string>
theme_manager_parse_all_xml_for_suffix(const char* directory, const char* element_type,
                                       const char* suffix) {
    // Build-time token table: same fast-path guard as _for_element above.
    if (helix::theme_tokens::enabled() && directory &&
        std::strcmp(directory, tm_ui_xml_dir()) == 0) {
        return helix::theme_tokens::for_suffix(element_type, suffix);
    }

    std::unordered_map<std::string, std::string> token_values;

    // Get sorted list of all XML files
    std::vector<std::string> files = theme_manager_find_xml_files(directory);

    // Parse each file in alphabetical order (last-wins via map overwrite)
    for (const auto& filepath : files) {
        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), element_type, suffix,
                                                token_values);
    }

    return token_values;
}

// Helper to check if a string looks like a hex color value
static bool is_hex_color_value(const std::string& value) {
    if (value.empty())
        return false;

    // Must start with a digit or be 3/6/8 hex chars
    // Hex colors: RGB (3), RRGGBB (6), or AARRGGBB (8)
    size_t len = value.length();
    if (len != 3 && len != 6 && len != 8)
        return false;

    // All characters must be hex digits
    for (char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }

    return true;
}

// Expat callback data for extracting constant references from attribute values
struct ConstantRefParserData {
    std::string current_file;
    std::vector<std::tuple<std::string, std::string, std::string>>* refs; // constant, file, attr
};

static void XMLCALL constant_ref_element_start(void* user_data, const XML_Char* name,
                                               const XML_Char** attrs) {
    (void)name;
    auto* data = static_cast<ConstantRefParserData*>(user_data);

    // Scan all attributes for constant references (pattern: ="# ... ")
    for (int i = 0; attrs[i]; i += 2) {
        const char* attr_name = attrs[i];
        const char* attr_value = attrs[i + 1];

        if (!attr_value || attr_value[0] != '#')
            continue;

        // Extract constant name (everything after # until end of string)
        std::string const_name(attr_value + 1);

        // Skip hex color values (start with digit or are 3/6/8 hex chars)
        if (is_hex_color_value(const_name))
            continue;

        data->refs->emplace_back(const_name, data->current_file, std::string(attr_name));
    }
}

// Parse XML file for constant references in attribute values
static void theme_manager_parse_xml_file_for_refs(
    const char* filepath, std::vector<std::tuple<std::string, std::string, std::string>>& refs) {
    if (!filepath)
        return;

    std::ifstream file(filepath);
    if (!file.is_open())
        return;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string xml_content = buffer.str();
    file.close();

    if (xml_content.empty())
        return;

    // Extract just the filename for error messages
    std::string filepath_str(filepath);
    std::string filename = filepath_str;
    size_t slash_pos = filepath_str.rfind('/');
    if (slash_pos != std::string::npos) {
        filename = filepath_str.substr(slash_pos + 1);
    }

    ConstantRefParserData parser_data = {filename, &refs};
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser)
        return;

    XML_SetUserData(parser, &parser_data);
    XML_SetElementHandler(parser, constant_ref_element_start, nullptr);

    XML_Parse(parser, xml_content.c_str(), static_cast<int>(xml_content.size()), XML_TRUE);
    XML_ParserFree(parser);
}

std::vector<std::string> theme_manager_validate_constant_sets(const char* directory) {
    std::vector<std::string> warnings;

    if (!directory) {
        return warnings;
    }

    // Validate responsive px sets (_small/_medium/_large required, _tiny optional)
    {
        auto tiny_tokens = theme_manager_parse_all_xml_for_suffix(directory, "px", "_tiny");
        auto small_tokens = theme_manager_parse_all_xml_for_suffix(directory, "px", "_small");
        auto medium_tokens = theme_manager_parse_all_xml_for_suffix(directory, "px", "_medium");
        auto large_tokens = theme_manager_parse_all_xml_for_suffix(directory, "px", "_large");

        // Collect all base names that have at least one responsive suffix
        // _tiny is optional — only _small/_medium/_large are required for a complete set
        std::unordered_map<std::string, int> base_names;
        for (const auto& [name, _] : small_tokens) {
            base_names[name] |= 1; // bit 0 = _small
        }
        for (const auto& [name, _] : medium_tokens) {
            base_names[name] |= 2; // bit 1 = _medium
        }
        for (const auto& [name, _] : large_tokens) {
            base_names[name] |= 4; // bit 2 = _large
        }

        // Check for incomplete sets (_small/_medium/_large must be complete)
        for (const auto& [base_name, flags] : base_names) {
            if (flags != 7) { // Not all three present (111 in binary)
                std::vector<std::string> found;
                std::vector<std::string> missing;

                if (flags & 1)
                    found.push_back("_small");
                else
                    missing.push_back("_small");

                if (flags & 2)
                    found.push_back("_medium");
                else
                    missing.push_back("_medium");

                if (flags & 4)
                    found.push_back("_large");
                else
                    missing.push_back("_large");

                std::string found_str;
                for (size_t i = 0; i < found.size(); ++i) {
                    if (i > 0)
                        found_str += ", ";
                    found_str += found[i];
                }

                std::string missing_str;
                for (size_t i = 0; i < missing.size(); ++i) {
                    if (i > 0)
                        missing_str += ", ";
                    missing_str += missing[i];
                }

                warnings.push_back("Incomplete responsive set for '" + base_name + "': found " +
                                   found_str + " but missing " + missing_str);
            }
        }

        // Warn about _tiny tokens without corresponding _small (likely a typo)
        for (const auto& [name, _] : tiny_tokens) {
            if (small_tokens.find(name) == small_tokens.end()) {
                warnings.push_back("Token '" + name +
                                   "' has _tiny but no _small (tiny falls back to small)");
            }
        }
    }

    // Validate themed color pairs (_light/_dark)
    {
        auto light_tokens = theme_manager_parse_all_xml_for_suffix(directory, "color", "_light");
        auto dark_tokens = theme_manager_parse_all_xml_for_suffix(directory, "color", "_dark");

        // Collect all base names that have at least one theme suffix
        std::unordered_map<std::string, int> base_names;
        for (const auto& [name, _] : light_tokens) {
            base_names[name] |= 1; // bit 0 = _light
        }
        for (const auto& [name, _] : dark_tokens) {
            base_names[name] |= 2; // bit 1 = _dark
        }

        // Check for incomplete pairs
        for (const auto& [base_name, flags] : base_names) {
            if (flags != 3) { // Not both present (11 in binary)
                if (flags == 1) {
                    warnings.push_back("Incomplete theme pair for '" + base_name +
                                       "': found _light but missing _dark");
                } else if (flags == 2) {
                    warnings.push_back("Incomplete theme pair for '" + base_name +
                                       "': found _dark but missing _light");
                }
            }
        }
    }

    // ========================================================================
    // Validate undefined constant references
    // ========================================================================
    {
        // Whitelist of constants registered in C++ (not XML) or work-in-progress
        static const std::unordered_set<std::string> cpp_registered_constants = {
            // Registered dynamically in theme_manager_register_responsive_spacing()
            "nav_width",
            "overlay_width_transient",
            "overlay_width_destination",
            // WIP wizard constants (user actively working on these)
            "wizard_footer_height",
            "wizard_button_width",
        };

        // Step 1: Collect all defined constants from all element types
        std::unordered_set<std::string> defined_constants;

        // Direct definitions (px, color, string, str, percentage)
        for (const auto& [name, _] : theme_manager_parse_all_xml_for_element(directory, "px")) {
            defined_constants.insert(name);
        }
        for (const auto& [name, _] : theme_manager_parse_all_xml_for_element(directory, "color")) {
            defined_constants.insert(name);
        }
        for (const auto& [name, _] : theme_manager_parse_all_xml_for_element(directory, "string")) {
            defined_constants.insert(name);
        }
        for (const auto& [name, _] : theme_manager_parse_all_xml_for_element(directory, "str")) {
            defined_constants.insert(name);
        }
        for (const auto& [name, _] :
             theme_manager_parse_all_xml_for_element(directory, "percentage")) {
            defined_constants.insert(name);
        }
        for (const auto& [name, _] : theme_manager_parse_all_xml_for_element(directory, "int")) {
            defined_constants.insert(name);
        }

        // Step 2: Add base names for responsive constants (_small/_medium/_large -> base)
        // These get registered at runtime as the base name
        // Also include _tiny and _xlarge tokens — they are optional but valid definitions
        auto small_px = theme_manager_parse_all_xml_for_suffix(directory, "px", "_small");
        auto medium_px = theme_manager_parse_all_xml_for_suffix(directory, "px", "_medium");
        auto large_px = theme_manager_parse_all_xml_for_suffix(directory, "px", "_large");
        auto tiny_px = theme_manager_parse_all_xml_for_suffix(directory, "px", "_tiny");
        auto xlarge_px = theme_manager_parse_all_xml_for_suffix(directory, "px", "_xlarge");
        for (const auto& [base_name, _] : small_px) {
            if (medium_px.count(base_name) && large_px.count(base_name)) {
                defined_constants.insert(base_name);
            }
        }
        // _tiny tokens with a valid _small/_medium/_large set are also defined
        for (const auto& [base_name, _] : tiny_px) {
            if (small_px.count(base_name) && medium_px.count(base_name) &&
                large_px.count(base_name)) {
                defined_constants.insert(base_name);
            }
        }
        // _xlarge tokens with a valid _small/_medium/_large set are also defined
        for (const auto& [base_name, _] : xlarge_px) {
            if (small_px.count(base_name) && medium_px.count(base_name) &&
                large_px.count(base_name)) {
                defined_constants.insert(base_name);
            }
        }

        auto small_str = theme_manager_parse_all_xml_for_suffix(directory, "string", "_small");
        auto medium_str = theme_manager_parse_all_xml_for_suffix(directory, "string", "_medium");
        auto large_str = theme_manager_parse_all_xml_for_suffix(directory, "string", "_large");
        auto tiny_str = theme_manager_parse_all_xml_for_suffix(directory, "string", "_tiny");
        auto xlarge_str = theme_manager_parse_all_xml_for_suffix(directory, "string", "_xlarge");
        for (const auto& [base_name, _] : small_str) {
            if (medium_str.count(base_name) && large_str.count(base_name)) {
                defined_constants.insert(base_name);
            }
        }
        for (const auto& [base_name, _] : tiny_str) {
            if (small_str.count(base_name) && medium_str.count(base_name) &&
                large_str.count(base_name)) {
                defined_constants.insert(base_name);
            }
        }
        // _xlarge string tokens with valid triplet are also defined
        for (const auto& [base_name, _] : xlarge_str) {
            if (small_str.count(base_name) && medium_str.count(base_name) &&
                large_str.count(base_name)) {
                defined_constants.insert(base_name);
            }
        }

        // Step 3: Add base names for themed colors (_light/_dark -> base)
        auto light_colors = theme_manager_parse_all_xml_for_suffix(directory, "color", "_light");
        auto dark_colors = theme_manager_parse_all_xml_for_suffix(directory, "color", "_dark");
        for (const auto& [base_name, _] : light_colors) {
            if (dark_colors.count(base_name)) {
                defined_constants.insert(base_name);
            }
        }

        // Step 4: Scan all XML files for constant references
        std::vector<std::tuple<std::string, std::string, std::string>> refs;
        auto files = theme_manager_find_xml_files(directory);
        for (const auto& filepath : files) {
            theme_manager_parse_xml_file_for_refs(filepath.c_str(), refs);
        }

        // Step 5: Check each reference against defined constants
        for (const auto& [const_name, filename, attr_name] : refs) {
            // Skip whitelisted constants (registered in C++ or WIP)
            if (cpp_registered_constants.count(const_name)) {
                continue;
            }
            if (defined_constants.find(const_name) == defined_constants.end()) {
                warnings.push_back("Undefined constant '#" + const_name + "' in " + filename +
                                   " (attribute: " + attr_name + ")");
            }
        }
    }

    return warnings;
}
