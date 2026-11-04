// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_switch.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_style.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "sound_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

using namespace helix;

/**
 * Size preset bundles for ui_switch
 * Maps semantic size names to dimension values (screen-size-aware)
 */
struct SwitchSizePreset {
    int32_t width;
    int32_t height;
    int32_t knob_pad;
    int32_t vert_margin;  // Vertical margin to reserve space for knob overflow
    int32_t horiz_margin; // Horizontal margin for knob extending past track edges
};

// Presets populated by ui_switch_init_size_presets() based on screen dimensions
static SwitchSizePreset SIZE_TINY;
static SwitchSizePreset SIZE_SMALL;
static SwitchSizePreset SIZE_MEDIUM;
static SwitchSizePreset SIZE_LARGE;

/**
 * Initialize size presets based on screen dimensions
 *
 * Runs at startup from ui_switch_register() and again from
 * theme_manager_refresh_layout_constants() on every debounced resize, so the
 * presets track a breakpoint that moves at runtime (#1210). Re-entrant: the
 * presets are plain values with no widget state behind them, so a later call
 * simply overwrites them. Widgets already sized from the old presets keep their
 * old size — only switches created after this call pick up the new tier.
 */
void ui_switch_init_size_presets(lv_display_t* display) {
    // Select the preset tier from the constrained axis (min of width/height), so
    // portrait screens size switches to their narrow axis instead of their tall
    // one. Landscape is unchanged: there the min axis is already the height this
    // used before. Mirrors the responsive_dimension() convention in theme_manager.
    int32_t resp_res = responsive_dimension(display);

    // Margin calculation: knob extends ~25% beyond track on each side
    // vert_margin = height * 0.25 (rounded up)
    // horiz_margin = similar, but knob extends horizontally too
    if (resp_res <= UI_BREAKPOINT_MICRO_MAX) { // ≤272: 480x272
        SIZE_TINY = {24, 12, 1, 3, 3};
        SIZE_SMALL = {32, 16, 1, 4, 4};
        SIZE_MEDIUM = {40, 20, 1, 5, 5};
        SIZE_LARGE = {48, 24, 2, 6, 6};
        spdlog::trace("[Switch] Initialized MICRO screen presets (min_dim={}px)", resp_res);
    } else if (resp_res <= UI_BREAKPOINT_SMALL_MAX) { // 273-460: 480x320, 480x400, 1920x440
        // TINY/SMALL share presets — switch elements are too small to benefit from separate tiers
        SIZE_TINY = {32, 16, 1, 4, 4};
        SIZE_SMALL = {40, 20, 1, 5, 5};
        SIZE_MEDIUM = {48, 24, 2, 6, 6};
        SIZE_LARGE = {56, 28, 2, 7, 7};
        spdlog::trace("[Switch] Initialized TINY/SMALL screen presets (min_dim={}px)", resp_res);
    } else if (resp_res <= UI_BREAKPOINT_MEDIUM_MAX) { // 481-800: 800x480
        SIZE_TINY = {48, 24, 2, 6, 6};
        SIZE_SMALL = {64, 32, 2, 8, 8};
        SIZE_MEDIUM = {80, 40, 3, 10, 10};
        SIZE_LARGE = {88, 44, 3, 11, 11};
        spdlog::trace("[Switch] Initialized MEDIUM screen presets (min_dim={}px)", resp_res);
    } else { // >800: 1024x600+
        SIZE_TINY = {64, 32, 2, 8, 8};
        SIZE_SMALL = {88, 40, 3, 10, 10};
        SIZE_MEDIUM = {112, 48, 4, 12, 12};
        SIZE_LARGE = {128, 56, 4, 14, 14};
        spdlog::trace("[Switch] Initialized LARGE screen presets (min_dim={}px)", resp_res);
    }
}

/**
 * Parse size string to SwitchSizePreset
 * Returns true if valid size found, false otherwise
 */
static bool parse_size_preset(const char* size_str, SwitchSizePreset* out_preset) {
    if (strcmp(size_str, "tiny") == 0) {
        *out_preset = SIZE_TINY;
        return true;
    } else if (strcmp(size_str, "small") == 0) {
        *out_preset = SIZE_SMALL;
        return true;
    } else if (strcmp(size_str, "medium") == 0) {
        *out_preset = SIZE_MEDIUM;
        return true;
    } else if (strcmp(size_str, "large") == 0) {
        *out_preset = SIZE_LARGE;
        return true;
    }

    spdlog::warn("[Switch] Invalid size '{}', ignoring preset", size_str);
    return false;
}

/**
 * Apply size preset to switch widget
 * Sets width, height, knob padding, and vertical margin as a bundle
 */
static void apply_size_preset(lv_obj_t* obj, const SwitchSizePreset& preset) {
    lv_obj_set_size(obj, preset.width, preset.height);
    lv_obj_set_style_pad_all(obj, preset.knob_pad, LV_PART_KNOB);

    // Add margins to reserve space for knob overflow
    // The knob extends beyond the track on all sides
    lv_obj_set_style_margin_top(obj, preset.vert_margin, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(obj, preset.vert_margin, LV_PART_MAIN);
    lv_obj_set_style_margin_left(obj, preset.horiz_margin, LV_PART_MAIN);
    lv_obj_set_style_margin_right(obj, preset.horiz_margin, LV_PART_MAIN);

    // Allow knob to overflow container bounds (prevents clipping)
    // NOTE: LV_OBJ_FLAG_OVERFLOW_VISIBLE when SET means "clip overflow"
    //       We need to CLEAR this flag to allow overflow
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // The track alone is too short a target for a fingertip (32px at 800x480), and
    // the knob paints past it. A flex or grid parent keeps siblings out of the
    // margins set above, so touch can extend into them without stealing a
    // neighbour's taps; under any other layout the margins reserve nothing.
    if (lv_obj_t* parent = lv_obj_get_parent(obj)) {
        const auto layout = lv_obj_get_style_layout(parent, LV_PART_MAIN);
        if (layout == LV_LAYOUT_FLEX || layout == LV_LAYOUT_GRID) {
            lv_obj_set_ext_click_area(obj, std::max(preset.vert_margin, preset.horiz_margin));
        }
    }

    spdlog::trace("[Switch] Applied size preset: {}x{}, knob_pad={}, margins=v{}/h{}", preset.width,
                  preset.height, preset.knob_pad, preset.vert_margin, preset.horiz_margin);
}

/**
 * @brief Event callback for LV_EVENT_VALUE_CHANGED — plays toggle on/off sound
 *
 * Hooked at the component level so ALL <ui_switch> instances get audio
 * feedback. Checks LV_STATE_CHECKED to determine on vs off.
 */
static void switch_value_changed_sound_cb(lv_event_t* e) {
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    SoundManager::instance().play(checked ? "toggle_on" : "toggle_off");
}

/**
 * Apply HelixScreen theme styling to a switch — colors for CHECKED indicator,
 * knob, track, and disabled state. Identical to the styling block previously
 * inlined in ui_switch_xml_apply; extracted so C++ callers can produce
 * theme-styled switches without going through the XML parser.
 */
static void apply_themed_styling(lv_obj_t* obj) {
    const char* primary_str = lv_xml_get_const(NULL, "primary");
    const char* tertiary_str = lv_xml_get_const(NULL, "tertiary");

    lv_color_t secondary = theme_manager_get_color("secondary");
    lv_obj_set_style_bg_color(obj, secondary, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(obj, 102, LV_PART_INDICATOR | LV_STATE_CHECKED);

    if (primary_str && tertiary_str) {
        lv_color_t knob_color = theme_get_knob_color();
        lv_obj_set_style_bg_color(obj, knob_color, LV_PART_KNOB | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_CHECKED);
    }

    lv_obj_set_style_bg_opa(obj, 102, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_DEFAULT);

    lv_color_t track_color = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    bool is_dark = theme_manager_is_dark_mode();
    const char* dark_color_str = lv_xml_get_const(NULL, "elevated_bg");
    const char* light_color_str = lv_xml_get_const(NULL, "text_subtle");

    if (dark_color_str && light_color_str) {
        lv_color_t dark_color = theme_manager_parse_hex_color(dark_color_str);
        lv_color_t light_color = theme_manager_parse_hex_color(light_color_str);

        lv_color_t disabled_track;
        lv_color_t disabled_knob;
        lv_opa_t track_opa;

        if (is_dark) {
            disabled_track = lv_color_mix(light_color, track_color, LV_OPA_20);
            disabled_knob = lv_color_mix(light_color, disabled_track, LV_OPA_40);
            track_opa = 77;
        } else {
            disabled_track = lv_color_mix(dark_color, track_color, LV_OPA_40);
            disabled_knob = lv_color_mix(dark_color, track_color, LV_OPA_30);
            track_opa = 128;
        }

        lv_obj_set_style_bg_color(obj, disabled_track, LV_PART_MAIN | LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(obj, track_opa, LV_PART_MAIN | LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(obj, disabled_track, LV_PART_INDICATOR | LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(obj, track_opa, LV_PART_INDICATOR | LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(obj, disabled_knob, LV_PART_KNOB | LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_DISABLED);
    }
}

extern "C" lv_obj_t* ui_switch_create_themed(lv_obj_t* parent, const char* size_str) {
    lv_obj_t* obj = lv_switch_create(parent);
    if (!obj) {
        spdlog::error("[Switch] ui_switch_create_themed: lv_switch_create failed");
        return nullptr;
    }

    // Prevent accidental click when scrolling (parity with ui_switch_xml_create).
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);

    lv_obj_add_event_cb(obj, switch_value_changed_sound_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    apply_themed_styling(obj);

    SwitchSizePreset preset{};
    if (parse_size_preset(size_str ? size_str : "small", &preset)) {
        apply_size_preset(obj, preset);
    }

    return obj;
}

/**
 * XML create handler for ui_switch
 * Creates an lv_switch widget when <ui_switch> is encountered in XML
 */
static void* ui_switch_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_switch_create((lv_obj_t*)parent);

    if (!obj) {
        spdlog::error("[Switch] Failed to create lv_switch");
        return nullptr;
    }

    // Prevent accidental click when scrolling.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);

    lv_obj_add_event_cb(obj, switch_value_changed_sound_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    return (void*)obj;
}

/**
 * XML apply handler for ui_switch
 * Applies attributes from XML to the switch widget with 3-pass size handling
 */
static void ui_switch_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = (lv_obj_t*)item;

    if (!obj) {
        spdlog::error("[Switch] NULL object in xml_apply");
        return;
    }

    // PASS 1: Extract size preset AND explicit dimension overrides
    SwitchSizePreset preset{};
    bool preset_found = false;
    int32_t explicit_width = -1;
    int32_t explicit_height = -1;
    int32_t explicit_knob_pad = -1;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "size") == 0) {
            preset_found = parse_size_preset(value, &preset);
        } else if (strcmp(name, "width") == 0) {
            explicit_width = atoi(value);
        } else if (strcmp(name, "height") == 0) {
            explicit_height = atoi(value);
        } else if (strcmp(name, "knob_pad") == 0) {
            explicit_knob_pad = atoi(value);
        }
    }

    // Apply standard lv_obj properties first (LVGL theme + XML attributes)
    lv_xml_obj_apply(state, attrs);

    // Apply HelixScreen theme styling — colors for CHECKED indicator/knob,
    // UNCHECKED track opacity, DISABLED state. Shared with C++ callers via
    // ui_switch_create_themed(). See apply_themed_styling() for the full
    // 3-layer (MAIN/INDICATOR/KNOB) anatomy reference.
    apply_themed_styling(obj);

    // PASS 2: Apply size preset (if found), then process other custom properties
    if (preset_found) {
        apply_size_preset(obj, preset);
    }

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "checked") == 0) {
            // Handle checked state
            if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                lv_obj_add_state(obj, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(obj, LV_STATE_CHECKED);
            }
        } else if (strcmp(name, "orientation") == 0) {
            // Handle orientation
            if (strcmp(value, "horizontal") == 0) {
                lv_switch_set_orientation(obj, LV_SWITCH_ORIENTATION_HORIZONTAL);
            } else if (strcmp(value, "vertical") == 0) {
                lv_switch_set_orientation(obj, LV_SWITCH_ORIENTATION_VERTICAL);
            } else if (strcmp(value, "auto") == 0) {
                lv_switch_set_orientation(obj, LV_SWITCH_ORIENTATION_AUTO);
            }
        }
    }

    // PASS 3: Apply explicit overrides AFTER preset
    // This allows size="medium" width="100" to override just width
    if (explicit_width > 0) {
        lv_obj_set_width(obj, explicit_width);
        spdlog::trace("[Switch] Explicit width override: {}px", explicit_width);
    }
    if (explicit_height > 0) {
        lv_obj_set_height(obj, explicit_height);
        spdlog::trace("[Switch] Explicit height override: {}px", explicit_height);
    }
    if (explicit_knob_pad >= 0) {
        lv_obj_set_style_pad_all(obj, explicit_knob_pad, LV_PART_KNOB);
        spdlog::trace("[Switch] Explicit knob_pad override: {}px", explicit_knob_pad);
    }

    // Log final state
    int32_t actual_w = lv_obj_get_width(obj);
    int32_t actual_h = lv_obj_get_height(obj);
    int32_t actual_knob_pad = lv_obj_get_style_pad_left(obj, LV_PART_KNOB);
    spdlog::trace("[Switch] Final size: {}x{}, knob_pad={}px", actual_w, actual_h, actual_knob_pad);
}

/**
 * Register responsive constants for switch sizing based on screen dimensions
 * Call this BEFORE registering XML components that use switches
 */
void ui_switch_register_responsive_constants() {
    spdlog::trace("[Switch] Registering responsive constants");

    // Use custom breakpoints optimized for our hardware: max(hor_res, ver_res)
    lv_display_t* display = lv_display_get_default();
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    // Switch sizing strategy:
    // - Knob is square (width = height of switch)
    // - Knob padding (style_pad_knob_all) adds visual spacing inside switch
    // - Width = ~2x height to allow knob to slide
    // - Row height calculation CRITICAL:
    //   * XML uses style_pad_all="#space_lg" (responsive: 12/16/20px)
    //   * Total row height = switch_height + (2 * container_padding)
    //   * Container padding varies by screen size via theme_manager_register_responsive_spacing()

    const char* switch_width;
    const char* switch_height;
    const char* knob_pad;
    const char* row_height;
    const char* row_height_large;
    const char* label_font;
    const char* label_large_font;

    const char* switch_width_large;
    const char* switch_height_large;
    const char* knob_pad_large;

    if (greater_res <= UI_BREAKPOINT_MICRO_MAX) { // ≤272: 480x272
        switch_height = "16";
        switch_width = "32";
        knob_pad = "1";
        row_height = "50";
        row_height_large = "60";
        label_font = "montserrat_10";
        label_large_font = "montserrat_12";
        switch_height_large = "20";
        switch_width_large = "40";
        knob_pad_large = "1";
        spdlog::trace("[Switch] Screen: MICRO (greater_res={}px), switch: {}x{}, row: {}px",
                      greater_res, switch_width, switch_height, row_height);
    } else if (greater_res <= UI_BREAKPOINT_SMALL_MAX) { // 273-460
        switch_height = "20";
        switch_width = "40";
        knob_pad = "1";
        row_height = "60";
        row_height_large = "70";
        label_font = "montserrat_12";
        label_large_font = "montserrat_14";

        // Large variant
        switch_height_large = "28";
        switch_width_large = "56";
        knob_pad_large = "2";

        spdlog::trace("[Switch] Screen: TINY/SMALL (greater_res={}px), switch: {}x{}, row: {}px",
                      greater_res, switch_width, switch_height, row_height);
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) { // 481-800: 800x480
        switch_height = "28";
        switch_width = "56";
        knob_pad = "2";
        row_height = "64";
        row_height_large = "72";
        label_font = "montserrat_16";
        label_large_font = "montserrat_20";

        // Large variant
        switch_height_large = "44";
        switch_width_large = "88";
        knob_pad_large = "3";

        spdlog::trace("[Switch] Screen: MEDIUM (greater_res={}px), switch: {}x{}, row: {}px",
                      greater_res, switch_width, switch_height, row_height);
    } else { // >800: 1024x600+
        // Switch: 44px height, 88px width
        // Knob: 3px padding
        // Row: 44 + (2 * 20) = 84px
        switch_height = "44";
        switch_width = "88";
        knob_pad = "6";
        row_height = "84";
        row_height_large = "104";
        label_font = "montserrat_20";
        label_large_font = lv_xml_get_const(NULL, "font_heading");

        // Large variant
        switch_height_large = "56";
        switch_width_large = "112";
        knob_pad_large = "4";

        spdlog::trace("[Switch] Screen: LARGE (greater_res={}px), switch: {}x{}, row: {}px",
                      greater_res, switch_width, switch_height, row_height);
    }

    // Get globals scope for constant registration
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope) {
        spdlog::error("[Switch] Failed to get globals scope for constant registration");
        return;
    }

    // Register test panel constants
    lv_xml_register_const(scope, "test_switch_width", switch_width);
    lv_xml_register_const(scope, "test_switch_height", switch_height);
    lv_xml_register_const(scope, "test_switch_knob_pad", knob_pad);
    lv_xml_register_const(scope, "test_row_height", row_height);
    lv_xml_register_const(scope, "test_row_height_large", row_height_large);
    lv_xml_register_const(scope, "test_label_font", label_font);
    lv_xml_register_const(scope, "test_label_large_font", label_large_font);

    lv_xml_register_const(scope, "test_switch_width_large", switch_width_large);
    lv_xml_register_const(scope, "test_switch_height_large", switch_height_large);
    lv_xml_register_const(scope, "test_switch_knob_pad_large", knob_pad_large);

    spdlog::trace("[Switch] Registered constants: {}x{} (pad={}), row={}", switch_width,
                  switch_height, knob_pad, row_height);
}

/**
 * Register the ui_switch widget with LVGL's XML system
 */
void ui_switch_register() {
    ui_switch_init_size_presets(nullptr);
    lv_xml_register_widget("ui_switch", ui_switch_xml_create, ui_switch_xml_apply);
    spdlog::trace("[Switch] Registered ui_switch widget with XML system");
}
