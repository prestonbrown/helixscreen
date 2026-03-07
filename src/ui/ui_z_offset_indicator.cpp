// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_z_offset_indicator.h"

#include "ui_update_queue.h"

#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "nozzle_renderer_a4t.h"
#include "nozzle_renderer_bambu.h"
#include "nozzle_renderer_faceted.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>

using namespace helix;

// ============================================================================
// Widget Data
// ============================================================================

struct ZOffsetIndicatorData {
    int32_t current_pos = 0;    // Current animated position (0.1 micron units for smooth anim)
    int32_t target_pos = 0;     // Target position (0.1 micron units)
    int32_t arrow_progress = 0; // 0-255, draw-in progress (base to tip)
    int32_t arrow_opacity = 0;  // 0-255, overall opacity (for fade-out phase)
    int arrow_direction = 0;    // +1 (farther/up) or -1 (closer/down)

    // Cached theme colors (resolved once at creation, not per-frame)
    lv_color_t color_text_muted = {};
    lv_color_t color_text = {};
    lv_color_t color_primary = {};
};

// ============================================================================
// Auto-ranging scale
// ============================================================================

/// Predefined scale ranges in microns. Each is a symmetric ± range.
/// We pick the smallest one that fits the current value with headroom.
struct ScaleRange {
    int range_microns;  // ± this value
    int tick_step;      // Microns between ticks
    int decimal_places; // For label formatting
};

static constexpr ScaleRange SCALE_RANGES[] = {
    {100, 50, 2},     // ±0.10mm, ticks every 0.05mm
    {250, 100, 1},    // ±0.25mm, ticks every 0.1mm
    {500, 250, 2},    // ±0.50mm, ticks every 0.25mm
    {1000, 500, 1},   // ±1.0mm, ticks every 0.5mm
    {2000, 1000, 0},  // ±2.0mm, ticks every 1mm
    {5000, 2000, 0},  // ±5.0mm, ticks every 2mm
    {10000, 5000, 0}, // ±10mm, ticks every 5mm
};
static constexpr int NUM_SCALE_RANGES = sizeof(SCALE_RANGES) / sizeof(SCALE_RANGES[0]);

/// Pick the best scale range for a given value in microns.
static const ScaleRange& pick_scale_range(int microns) {
    int abs_val = std::abs(microns);
    for (int i = 0; i < NUM_SCALE_RANGES; i++) {
        // 80% of range as headroom threshold
        if (abs_val <= SCALE_RANGES[i].range_microns * 80 / 100) {
            return SCALE_RANGES[i];
        }
    }
    return SCALE_RANGES[NUM_SCALE_RANGES - 1]; // Largest range as fallback
}

/// Convert microns to Y pixel position on the vertical scale.
/// Positive values (farther from bed) map to top, negative (closer) to bottom.
static int32_t microns_to_y(int microns, int range_microns, int32_t scale_top,
                            int32_t scale_bottom) {
    // Clamp to range
    if (microns > range_microns)
        microns = range_microns;
    if (microns < -range_microns)
        microns = -range_microns;

    int32_t center = (scale_top + scale_bottom) / 2;
    int32_t half_px = (scale_bottom - scale_top) / 2;
    return center - (int32_t)((int64_t)microns * half_px / range_microns);
}

// ============================================================================
// Animation Callbacks (forward declarations)
// ============================================================================

static void position_anim_cb(void* var, int32_t value);
static void arrow_progress_anim_cb(void* var, int32_t value);
static void arrow_opacity_anim_cb(void* var, int32_t value);
static void on_draw_in_complete(lv_anim_t* anim);

// ============================================================================
// Drawing
// ============================================================================

/// Format a micron tick value as a label string into a static buffer pool.
/// Returns a pointer that remains valid until the next frame (pool of 16 slots).
static const char* format_tick_label(int microns, int decimal_places) {
    // Pool of static buffers so deferred rendering doesn't see stale pointers.
    // 16 slots is enough for any reasonable number of ticks per frame.
    static char pool[16][12];
    static int slot = 0;
    char* buf = pool[slot++ % 16];

    double mm = microns / 1000.0;
    if (decimal_places == 0) {
        lv_snprintf(buf, 12, "%d", (int)mm);
    } else if (decimal_places == 1) {
        lv_snprintf(buf, 12, "%.1f", mm);
    } else {
        lv_snprintf(buf, 12, "%.2f", mm);
    }
    return buf;
}

static void indicator_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    // Get widget dimensions
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t w = lv_area_get_width(&coords);
    int32_t h = lv_area_get_height(&coords);

    // Current value in microns
    int32_t current_microns = data->current_pos / 10;

    // Auto-range: pick scale that fits the current value
    const auto& scale = pick_scale_range(current_microns);

    // Layout: measure widest label, position scale just right of labels, nozzle in remaining space
    int32_t margin_v = h / 10;
    int32_t scale_top = coords.y1 + margin_v;
    int32_t scale_bottom = coords.y1 + h - margin_v;

    lv_color_t muted_color = data->color_text_muted;
    lv_color_t text_color = data->color_text;
    lv_color_t primary_color = data->color_primary;
    const lv_font_t* font = lv_font_get_default();
    int32_t font_h = lv_font_get_line_height(font);

    // Measure widest label to position scale dynamically
    int32_t max_label_w = 0;
    for (int tick_val = -scale.range_microns; tick_val <= scale.range_microns;
         tick_val += scale.tick_step) {
        const char* label = format_tick_label(tick_val, scale.decimal_places);
        lv_point_t txt_size;
        lv_text_get_size(&txt_size, label, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (txt_size.x > max_label_w)
            max_label_w = txt_size.x;
    }

    int32_t tick_half_w = w / 16;
    int32_t label_pad = 4;
    int32_t scale_x = coords.x1 + max_label_w + label_pad + tick_half_w + label_pad;

    // --- Vertical scale line ---
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = muted_color;
    line_dsc.width = 2;
    line_dsc.round_start = true;
    line_dsc.round_end = true;
    line_dsc.p1.x = scale_x;
    line_dsc.p1.y = scale_top;
    line_dsc.p2.x = scale_x;
    line_dsc.p2.y = scale_bottom;
    lv_draw_line(layer, &line_dsc);

    // --- Tick marks and labels ---
    // (tick_half_w already computed above for scale_x positioning)
    for (int tick_val = -scale.range_microns; tick_val <= scale.range_microns;
         tick_val += scale.tick_step) {
        int32_t y = microns_to_y(tick_val, scale.range_microns, scale_top, scale_bottom);

        // Tick mark
        lv_draw_line_dsc_t tick_dsc;
        lv_draw_line_dsc_init(&tick_dsc);
        tick_dsc.color = muted_color;
        tick_dsc.width = (tick_val == 0) ? 2 : 1;
        tick_dsc.p1.x = scale_x - tick_half_w;
        tick_dsc.p1.y = y;
        tick_dsc.p2.x = scale_x + tick_half_w;
        tick_dsc.p2.y = y;
        lv_draw_line(layer, &tick_dsc);

        // Label
        const char* label = format_tick_label(tick_val, scale.decimal_places);
        lv_draw_label_dsc_t lbl_dsc;
        lv_draw_label_dsc_init(&lbl_dsc);
        lbl_dsc.color = muted_color;
        lbl_dsc.font = font;
        lbl_dsc.align = LV_TEXT_ALIGN_RIGHT;
        lbl_dsc.text = label;
        lv_area_t lbl_area = {coords.x1 + 2, y - font_h / 2, scale_x - tick_half_w - 4,
                              y + font_h / 2};
        lv_draw_label(layer, &lbl_dsc, &lbl_area);
    }

    // --- Position marker on scale ---
    int32_t marker_y = microns_to_y(current_microns, scale.range_microns, scale_top, scale_bottom);

    // Triangular marker pointing right
    int32_t tri_size = LV_MAX(4, h / 20);
    lv_draw_triangle_dsc_t tri_dsc;
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.color = primary_color;
    tri_dsc.opa = LV_OPA_COVER;
    tri_dsc.p[0].x = scale_x + 3;
    tri_dsc.p[0].y = marker_y;
    tri_dsc.p[1].x = scale_x + 3 + tri_size;
    tri_dsc.p[1].y = marker_y - tri_size;
    tri_dsc.p[2].x = scale_x + 3 + tri_size;
    tri_dsc.p[2].y = marker_y + tri_size;
    lv_draw_triangle(layer, &tri_dsc);

    // --- Nozzle icon centered in space right of scale (fixed at vertical center) ---
    int32_t nozzle_cx = (scale_x + coords.x1 + w) / 2;
    int32_t nozzle_y = (scale_top + scale_bottom) / 2;
    int32_t nozzle_scale = LV_CLAMP(5, h / 10, 12);
    lv_color_t nozzle_color = data->color_text;

    switch (helix::SettingsManager::instance().get_effective_toolhead_style()) {
        case helix::ToolheadStyle::STEALTHBURNER:
            draw_nozzle_faceted(layer, nozzle_cx, nozzle_y, nozzle_color, nozzle_scale);
            break;
        case helix::ToolheadStyle::A4T:
            draw_nozzle_a4t(layer, nozzle_cx, nozzle_y, nozzle_color, nozzle_scale);
            break;
        default:
            draw_nozzle_bambu(layer, nozzle_cx, nozzle_y, nozzle_color, nozzle_scale);
            break;
    }

    // --- Direction arrow flash (shaft + V-head, drawn from base to tip) ---
    if (data->arrow_opacity > 0 && data->arrow_progress > 0) {
        int32_t arrow_x = nozzle_cx + nozzle_scale * 4;
        int32_t arrow_len = LV_MAX(14, h / 6);
        int32_t head_len = LV_MAX(5, arrow_len / 3);
        int32_t shaft_width = 2;
        lv_opa_t opa = static_cast<lv_opa_t>(data->arrow_opacity);

        // Arrow runs from base to tip along the Y axis
        int32_t base_y, tip_y;
        if (data->arrow_direction > 0) {
            base_y = nozzle_y + arrow_len / 2;
            tip_y = nozzle_y - arrow_len / 2;
        } else {
            base_y = nozzle_y - arrow_len / 2;
            tip_y = nozzle_y + arrow_len / 2;
        }

        // Progress determines how far the arrow has drawn from base toward tip
        int32_t progress = data->arrow_progress; // 0-255
        int32_t current_tip_y = base_y + (tip_y - base_y) * progress / 255;

        // Shaft line
        lv_draw_line_dsc_t shaft_dsc;
        lv_draw_line_dsc_init(&shaft_dsc);
        shaft_dsc.color = text_color;
        shaft_dsc.opa = opa;
        shaft_dsc.width = shaft_width;
        shaft_dsc.round_start = true;
        shaft_dsc.round_end = true;
        shaft_dsc.p1.x = arrow_x;
        shaft_dsc.p1.y = base_y;
        shaft_dsc.p2.x = arrow_x;
        shaft_dsc.p2.y = current_tip_y;
        lv_draw_line(layer, &shaft_dsc);

        // Arrowhead V at current tip (grows in as progress increases)
        if (progress > 40) {
            int32_t head_progress = (progress - 40) * 255 / 215; // 0-255 over remaining range
            int32_t head_size = head_len * head_progress / 255;
            // Head arms point back toward the base
            int32_t head_dy = (data->arrow_direction > 0) ? head_size : -head_size;

            lv_draw_line_dsc_t head_dsc;
            lv_draw_line_dsc_init(&head_dsc);
            head_dsc.color = text_color;
            head_dsc.opa = opa;
            head_dsc.width = shaft_width;
            head_dsc.round_start = true;
            head_dsc.round_end = true;

            // Left arm
            head_dsc.p1.x = arrow_x;
            head_dsc.p1.y = current_tip_y;
            head_dsc.p2.x = arrow_x - head_size;
            head_dsc.p2.y = current_tip_y + head_dy;
            lv_draw_line(layer, &head_dsc);

            // Right arm
            head_dsc.p2.x = arrow_x + head_size;
            lv_draw_line(layer, &head_dsc);
        }
    }
}

// ============================================================================
// Animation Callbacks
// ============================================================================

static void position_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    data->current_pos = value;

    // Defer invalidation to avoid calling during render phase
    helix::ui::async_call(obj, [](void* d) { lv_obj_invalidate(static_cast<lv_obj_t*>(d)); }, obj);
}

static void arrow_progress_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    data->arrow_progress = value;

    helix::ui::async_call(obj, [](void* d) { lv_obj_invalidate(static_cast<lv_obj_t*>(d)); }, obj);
}

static void arrow_opacity_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    data->arrow_opacity = value;

    helix::ui::async_call(obj, [](void* d) { lv_obj_invalidate(static_cast<lv_obj_t*>(d)); }, obj);
}

/// Called when draw-in animation completes; starts the fade-out phase
static void on_draw_in_complete(lv_anim_t* anim) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(anim->var);
    if (!lv_obj_is_valid(obj))
        return;

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, obj);
    lv_anim_set_values(&fade, 255, 0);
    lv_anim_set_duration(&fade, 400);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade, arrow_opacity_anim_cb);
    lv_anim_start(&fade);
}

// ============================================================================
// Delete Callback
// ============================================================================

static void indicator_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    lv_anim_delete(obj, position_anim_cb);
    lv_anim_delete(obj, arrow_progress_anim_cb);
    lv_anim_delete(obj, arrow_opacity_anim_cb);
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    delete data;
    lv_obj_set_user_data(obj, nullptr);
}

// ============================================================================
// Public API
// ============================================================================

void ui_z_offset_indicator_set_value(lv_obj_t* obj, int microns) {
    if (!obj)
        return;
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    // Store in 0.1-micron units for smooth animation interpolation
    int32_t new_target = microns * 10;
    data->target_pos = new_target;

    // Stop any existing position animation
    lv_anim_delete(obj, position_anim_cb);

    if (DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, obj);
        lv_anim_set_values(&anim, data->current_pos, new_target);
        lv_anim_set_duration(&anim, 200);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, position_anim_cb);
        lv_anim_start(&anim);
    } else {
        // Jump directly to target
        data->current_pos = new_target;
        lv_obj_invalidate(obj);
    }

    spdlog::trace("[ZOffsetIndicator] Set value: {} microns", microns);
}

void ui_z_offset_indicator_flash_direction(lv_obj_t* obj, int direction) {
    if (!obj)
        return;
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    data->arrow_direction = direction;

    // Stop any existing arrow animations
    lv_anim_delete(obj, arrow_progress_anim_cb);
    lv_anim_delete(obj, arrow_opacity_anim_cb);

    if (DisplaySettingsManager::instance().get_animations_enabled()) {
        // Phase 1: draw-in (base to tip)
        data->arrow_opacity = 255;
        data->arrow_progress = 0;

        lv_anim_t draw_in;
        lv_anim_init(&draw_in);
        lv_anim_set_var(&draw_in, obj);
        lv_anim_set_values(&draw_in, 0, 255);
        lv_anim_set_duration(&draw_in, 250);
        lv_anim_set_path_cb(&draw_in, lv_anim_path_linear);
        lv_anim_set_exec_cb(&draw_in, arrow_progress_anim_cb);
        lv_anim_set_completed_cb(&draw_in, on_draw_in_complete);
        lv_anim_start(&draw_in);
    } else {
        // No animation - skip arrow entirely
        data->arrow_opacity = 0;
        data->arrow_progress = 0;
    }

    spdlog::trace("[ZOffsetIndicator] Flash direction: {}", direction > 0 ? "up" : "down");
}

// ============================================================================
// XML Widget Registration
// ============================================================================

static void* z_offset_indicator_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create((lv_obj_t*)parent);

    if (!obj) {
        spdlog::error("[ZOffsetIndicator] Failed to create lv_obj");
        return nullptr;
    }

    // Default: full width, full parent height (stretches to sibling-driven row height)
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    // Remove default styles and make transparent
    lv_obj_remove_style_all(obj);

    // Allocate and attach widget data
    auto* data = new ZOffsetIndicatorData{};
    data->color_text_muted = theme_manager_get_color("text_muted");
    data->color_text = theme_manager_get_color("text");
    data->color_primary = theme_manager_get_color("primary");
    lv_obj_set_user_data(obj, data);

    // Register draw and delete callbacks
    // NOTE: lv_obj_add_event_cb() is appropriate here - this is a custom widget, not a UI button
    lv_obj_add_event_cb(obj, indicator_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, indicator_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::trace("[ZOffsetIndicator] Created widget");
    return (void*)obj;
}

void ui_z_offset_indicator_register(void) {
    lv_xml_register_widget("z_offset_indicator", z_offset_indicator_xml_create, lv_xml_obj_apply);
    spdlog::trace("[ZOffsetIndicator] Registered <z_offset_indicator> widget");
}
