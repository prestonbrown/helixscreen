// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glyph drawing for the filament_path_canvas widget: sensor dots (push-to-
// connect fittings), the hub/selector box (incl. its interactive gear badge),
// the buffer ("BUF") box, the animated filament tip, the nozzle heat glow,
// flow particles, tool badges, and the style-dispatched toolhead glyph.
// See ui_filament_path_internal.h for the widget architecture.

#include "ui_filament_path_internal.h"
#include "ui_fonts.h"

#include "nozzle_renderer_a4t.h"
#include "nozzle_renderer_anthead.h"
#include "nozzle_renderer_bambu.h"
#include "nozzle_renderer_creality_k1.h"
#include "nozzle_renderer_creality_k2.h"
#include "nozzle_renderer_jabberwocky.h"
#include "nozzle_renderer_stealthburner.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <cmath>
#include <cstring>

namespace helix::ui::fpath {

// Draw a push-to-connect fitting at a sensor position.
// Uses same shadow/highlight language as tubes: shadow (darker) behind, highlight (lighter) offset.
// Same overall size as before — no bigger than the original radius.
void draw_sensor_dot(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color, bool filled,
                     int32_t radius) {
    const bool simple = reduced_effects();
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.center.x = cx;
    arc_dsc.center.y = cy;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    // Shadow: same darkening as tube shadow (ph_darken 35), drawn at full radius
    if (!simple) {
        arc_dsc.radius = static_cast<uint16_t>(radius);
        arc_dsc.width = static_cast<uint16_t>(radius * 2);
        arc_dsc.color = ph_darken(color, 35);
        lv_draw_arc(layer, &arc_dsc);
    }

    if (filled) {
        // Body: full radius in simple mode, slightly inset on full quality
        int32_t body_r = simple ? radius : LV_MAX(1, radius - 1);
        arc_dsc.center.x = cx;
        arc_dsc.center.y = cy;
        arc_dsc.radius = static_cast<uint16_t>(body_r);
        arc_dsc.width = static_cast<uint16_t>(body_r * 2);
        arc_dsc.color = color;
        lv_draw_arc(layer, &arc_dsc);

        // Highlight: small bright dot offset toward top-right
        if (!simple) {
            int32_t hl_r = LV_MAX(1, radius / 3);
            int32_t hl_off = LV_MAX(1, radius / 3);
            arc_dsc.center.x = cx + hl_off;
            arc_dsc.center.y = cy - hl_off;
            arc_dsc.radius = static_cast<uint16_t>(hl_r);
            arc_dsc.width = static_cast<uint16_t>(hl_r * 2);
            arc_dsc.color = ph_lighten(color, 44);
            lv_draw_arc(layer, &arc_dsc);
        }
    } else {
        // Empty fitting: outline ring only (no fill)
        arc_dsc.radius = static_cast<uint16_t>(radius - 1);
        arc_dsc.width = 2;
        arc_dsc.color = color;
        lv_draw_arc(layer, &arc_dsc);
    }
}

int32_t draw_hub_box(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t width, int32_t height,
                     lv_color_t bg_color, lv_color_t border_color, const char* label,
                     lv_opa_t bg_opa, bool interactive) {
    lv_layer_t* layer = ctx.layer;
    lv_color_t text_color = ctx.data->theme.color_text;
    const lv_font_t* font = ctx.data->theme.label_font;
    int32_t radius = ctx.data->theme.border_radius;

    // Background
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = bg_color;
    fill_dsc.opa = bg_opa;
    fill_dsc.radius = radius;

    lv_area_t box_area = {cx - width / 2, cy - height / 2, cx + width / 2, cy + height / 2};
    lv_draw_fill(layer, &fill_dsc, &box_area);

    // Border
    lv_draw_border_dsc_t border_dsc;
    lv_draw_border_dsc_init(&border_dsc);
    border_dsc.color = border_color;
    border_dsc.width = 2;
    border_dsc.radius = radius;
    lv_draw_border(layer, &border_dsc, &box_area);

    // Label
    if (label && label[0] && font) {
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = text_color;
        label_dsc.font = font;
        label_dsc.align = LV_TEXT_ALIGN_CENTER;
        label_dsc.text = label;

        int32_t font_h = lv_font_get_line_height(font);
        lv_area_t label_area = {cx - width / 2, cy - font_h / 2, cx + width / 2, cy + font_h / 2};
        lv_draw_label(layer, &label_dsc, &label_area);
    }

    // Tappable affordance: a small gear glyph signals that the box opens a
    // context menu when tapped. Preferred placement is the top-right corner
    // INSIDE the box, but when the label + gear + padding don't fit the box
    // width the gear would overlap the label — so draw it immediately OUTSIDE
    // the box's right edge, vertically centered.
    int32_t gear_overflow = 0;
    if (interactive) {
        const lv_font_t* icon_font = theme_manager_get_font("icon_font_sm");
        if (icon_font) {
            int32_t gear_h = lv_font_get_line_height(icon_font);
            lv_point_t gear_sz;
            lv_text_get_size(&gear_sz, ICON_SETTINGS, icon_font, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            int32_t gear_w = gear_sz.x > 0 ? gear_sz.x : gear_h; // defensive fallback
            const int32_t pad = 2;

            // Measure the label so we know whether the gear fits beside it.
            int32_t label_w = 0;
            if (label && label[0] && font) {
                lv_point_t lbl_sz;
                lv_text_get_size(&lbl_sz, label, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                label_w = lbl_sz.x;
            }
            // The label is centered; the gear needs the gap between the label's
            // right edge and the box's right edge: (width - label_w)/2.
            int32_t right_gap = (width - label_w) / 2;
            bool fits_inside = (right_gap >= gear_w + pad * 2);

            lv_draw_label_dsc_t gear_dsc;
            lv_draw_label_dsc_init(&gear_dsc);
            gear_dsc.color = border_color;
            gear_dsc.font = icon_font;
            gear_dsc.opa = LV_OPA_80;
            gear_dsc.text = ICON_SETTINGS;

            if (fits_inside) {
                gear_dsc.align = LV_TEXT_ALIGN_RIGHT;
                lv_area_t gear_area = {box_area.x1, box_area.y1 + pad, box_area.x2 - pad,
                                       box_area.y1 + pad + gear_h};
                lv_draw_label(layer, &gear_dsc, &gear_area);
            } else {
                // Badge style: center the gear on the box's lower-right corner
                // (half over the box, half outside), like the pencil-edit
                // badges used elsewhere — reads as part of the box instead of
                // a detached icon floating beside it. Use the label text color
                // at full opacity: border-colored at 80% reads muddy where the
                // badge overlaps the box fill.
                gear_dsc.color = text_color;
                gear_dsc.opa = LV_OPA_COVER;
                gear_dsc.align = LV_TEXT_ALIGN_LEFT;
                int32_t gx1 = box_area.x2 - gear_w / 2;
                int32_t gy1 = box_area.y2 - gear_h / 2;
                lv_area_t gear_area = {gx1, gy1, gx1 + gear_w, gy1 + gear_h};
                lv_draw_label(layer, &gear_dsc, &gear_area);
                gear_overflow = gear_w / 2;
            }
        }
    }
    return gear_overflow;
}

// Draw buffer box element — simple labeled box like HUB/SELECTOR
// Color reflects buffer state: green (OK), orange (warning), red (fault)
void draw_buffer_coil(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t hub_h,
                      bool has_filament, lv_color_t filament_color) {
    const ThemeCache& theme = ctx.data->theme;
    int buffer_fault_state = ctx.data->buffer_fault_state;
    float buffer_bias = ctx.data->buffer_bias;
    lv_color_t bg_color = theme.color_bg;

    // Slightly smaller than hub box — fits "BUF" with comfortable padding
    int32_t box_w = theme.hub_width * 4 / 5;
    int32_t box_h = hub_h;
    if (box_w < 36)
        box_w = 36;
    if (box_h < 16)
        box_h = 16;

    // Border color based on fault state and proportional bias
    lv_color_t border_color;
    lv_color_t buf_bg = bg_color;

    if (buffer_fault_state >= 2) {
        border_color = lv_color_hex(0xEF4444);
        buf_bg = lv_color_hex(0x3F1111);
    } else if (buffer_bias > -1.5f) {
        // Proportional mode: green -> orange -> red based on abs(bias)
        float abs_bias = std::fabs(buffer_bias);
        abs_bias = std::clamp(abs_bias, 0.0f, 1.0f);
        if (buffer_fault_state >= 1) {
            // Fault active — use pure orange minimum to match selector
            if (abs_bias < 0.7f) {
                border_color = lv_color_hex(0xF59E0B);
            } else {
                float t = (abs_bias - 0.7f) / 0.3f;
                border_color = ph_blend(lv_color_hex(0xF59E0B), lv_color_hex(0xEF4444), t);
            }
        } else if (abs_bias < 0.3f) {
            border_color = lv_color_hex(0x22C55E);
        } else if (abs_bias < 0.7f) {
            float t = (abs_bias - 0.3f) / 0.4f;
            border_color = ph_blend(lv_color_hex(0x22C55E), lv_color_hex(0xF59E0B), t);
        } else {
            float t = (abs_bias - 0.7f) / 0.3f;
            border_color = ph_blend(lv_color_hex(0xF59E0B), lv_color_hex(0xEF4444), t);
        }
        if (has_filament) {
            buf_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    } else if (buffer_fault_state == 1) {
        border_color = lv_color_hex(0xF59E0B);
        if (has_filament) {
            buf_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    } else {
        border_color = lv_color_hex(0x22C55E);
        if (has_filament) {
            buf_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    }

    draw_hub_box(ctx, cx, cy, box_w, box_h, buf_bg, border_color, ctx.data->buffer_label);
}

// Draw animated filament tip (a glowing dot that moves along the path)
void draw_filament_tip(lv_layer_t* layer, int32_t x, int32_t y, lv_color_t color, int32_t radius) {
    // Outer glow (lighter, larger)
    lv_color_t glow_color = ph_lighten(color, 60);
    draw_sensor_dot(layer, x, y, glow_color, true, radius + 2);

    // Inner core (bright)
    lv_color_t core_color = ph_lighten(color, 100);
    draw_sensor_dot(layer, x, y, core_color, true, radius);
}

// Draw heat glow effect around nozzle tip
// Creates a pulsing orange/red glow halo to indicate heating
void draw_heat_glow(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t radius, lv_opa_t pulse_opa) {
    // Heat glow color - warm orange (#FF6B35) at full opacity
    lv_color_t heat_color = lv_color_hex(0xFF6B35);

    // Outer soft glow (larger, more transparent)
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.center.x = cx;
    arc_dsc.center.y = cy;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    // Multiple rings for soft glow effect
    // Outer ring (widest, most transparent)
    arc_dsc.radius = static_cast<uint16_t>(radius + 8);
    arc_dsc.width = 6;
    arc_dsc.color = heat_color;
    arc_dsc.opa = static_cast<lv_opa_t>(pulse_opa / 4);
    lv_draw_arc(layer, &arc_dsc);

    // Middle ring
    arc_dsc.radius = static_cast<uint16_t>(radius + 4);
    arc_dsc.width = 4;
    arc_dsc.opa = static_cast<lv_opa_t>(pulse_opa / 2);
    lv_draw_arc(layer, &arc_dsc);

    // Inner ring (brightest)
    arc_dsc.radius = static_cast<uint16_t>(radius + 1);
    arc_dsc.width = 2;
    arc_dsc.opa = pulse_opa;
    lv_draw_arc(layer, &arc_dsc);
}

// ============================================================================
// Flow particles
// ============================================================================
// Small bright dots flowing along an active tube segment to indicate filament
// motion during load/unload. Dots are spaced at FLOW_DOT_SPACING and offset by
// flow_offset for animation.

// Draw flow dots along a straight line segment
void draw_flow_dots_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                         lv_color_t color, int32_t flow_offset, bool reverse) {
    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    float len = sqrtf((float)(dx * dx + dy * dy));
    if (len < 1.0f)
        return;

    lv_color_t dot_color = ph_lighten(color, 70);
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;
    arc_dsc.radius = static_cast<uint16_t>(FLOW_DOT_RADIUS);
    arc_dsc.width = static_cast<uint16_t>(FLOW_DOT_RADIUS * 2);
    arc_dsc.color = dot_color;
    arc_dsc.opa = FLOW_DOT_OPA;

    // Place dots along the line at FLOW_DOT_SPACING intervals
    int32_t offset = reverse ? (FLOW_DOT_SPACING - flow_offset) : flow_offset;
    for (float d = (float)offset; d < len; d += FLOW_DOT_SPACING) {
        float t = d / len;
        arc_dsc.center.x = x1 + (int32_t)(dx * t);
        arc_dsc.center.y = y1 + (int32_t)(dy * t);
        lv_draw_arc(layer, &arc_dsc);
    }
}

// Draw flow dots along an entire FilamentPath as a continuous stream.
// Dots are placed at FLOW_DOT_SPACING intervals along the total path length,
// with flow_offset providing animation. When reverse=true (unloading), dots
// flow from nozzle toward entry.
void draw_flow_dots_path(lv_layer_t* layer, const pg::FilamentPath& path, lv_color_t color,
                         int32_t flow_offset, bool reverse) {
    float total = pg::path_length(path);
    if (path.count == 0 || total < 1.0f)
        return;

    lv_color_t dot_color = ph_lighten(color, 70);
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;
    arc_dsc.radius = static_cast<uint16_t>(FLOW_DOT_RADIUS);
    arc_dsc.width = static_cast<uint16_t>(FLOW_DOT_RADIUS * 2);
    arc_dsc.color = dot_color;
    arc_dsc.opa = FLOW_DOT_OPA;

    float start_offset = (float)flow_offset;
    for (float d = start_offset; d < total; d += FLOW_DOT_SPACING) {
        float pos = reverse ? (total - d) : d;
        pg::PathPoint pt = pg::path_point_at(path, pos);
        arc_dsc.center.x = (int32_t)lroundf(pt.x);
        arc_dsc.center.y = (int32_t)lroundf(pt.y);
        lv_draw_arc(layer, &arc_dsc);
    }
}

// ============================================================================
// Toolhead glyph
// ============================================================================

// One dispatch point for the user's configured toolhead style. The A4T glyph
// is drawn 6/5 larger than the others at every call site in this widget, so
// the boost is folded in here.
void draw_toolhead(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color, int32_t scale,
                   lv_opa_t opa) {
    switch (helix::SettingsManager::instance().get_effective_toolhead_style()) {
    case helix::ToolheadStyle::A4T:
        draw_nozzle_a4t(layer, cx, cy, color, scale * 6 / 5, opa);
        break;
    case helix::ToolheadStyle::ANTHEAD:
        draw_nozzle_anthead(layer, cx, cy, color, scale, opa);
        break;
    case helix::ToolheadStyle::JABBERWOCKY:
        draw_nozzle_jabberwocky(layer, cx, cy, color, scale, opa);
        break;
    case helix::ToolheadStyle::STEALTHBURNER:
        draw_nozzle_stealthburner(layer, cx, cy, color, scale, opa);
        break;
    case helix::ToolheadStyle::CREALITY_K1:
        draw_nozzle_creality_k1(layer, cx, cy, color, scale, opa);
        break;
    case helix::ToolheadStyle::CREALITY_K2:
        draw_nozzle_creality_k2(layer, cx, cy, color, scale, opa);
        break;
    default:
        draw_nozzle_bambu(layer, cx, cy, color, scale, opa);
        break;
    }
}

// Nozzle tip Y for the configured style — anchors the heat glow halo.
int32_t toolhead_tip_y(int32_t nozzle_y, int32_t extruder_scale) {
    switch (helix::SettingsManager::instance().get_effective_toolhead_style()) {
    case helix::ToolheadStyle::A4T:
        return nozzle_y + (extruder_scale * 6 / 5 * 46) / 10 - 6;
    case helix::ToolheadStyle::STEALTHBURNER:
        return nozzle_y + (extruder_scale * 46) / 10 - 6;
    case helix::ToolheadStyle::ANTHEAD:
        return nozzle_y + (extruder_scale * 33) / 10;
    default:
        return nozzle_y + (extruder_scale * 26) / 10;
    }
}

// Tool badge (T0, T1, …) below a nozzle — matches system_path_canvas style.
void draw_tool_badge(const RenderCtx& ctx, int32_t cx, int32_t badge_top, const char* label,
                     lv_color_t text_color, lv_opa_t opa) {
    const ThemeCache& theme = ctx.data->theme;
    if (!theme.label_font || !label || !label[0])
        return;

    int32_t font_h = lv_font_get_line_height(theme.label_font);
    int32_t label_len = (int32_t)strlen(label);
    int32_t badge_w = LV_MAX(24, label_len * (font_h * 3 / 5) + 6);
    int32_t badge_h = font_h + 4;
    int32_t badge_left = cx - badge_w / 2;

    // Badge background (rounded rect)
    lv_area_t badge_area = {badge_left, badge_top, badge_left + badge_w, badge_top + badge_h};
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = theme.color_idle;
    fill_dsc.opa = (lv_opa_t)LV_MIN(200, opa);
    fill_dsc.radius = 4;
    lv_draw_fill(ctx.layer, &fill_dsc, &badge_area);

    // Badge text
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = text_color;
    label_dsc.opa = opa;
    label_dsc.font = theme.label_font;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    label_dsc.text = label;
    label_dsc.text_local = 1;

    lv_area_t text_area = {badge_left, badge_top + 2, badge_left + badge_w, badge_top + 2 + font_h};
    lv_draw_label(ctx.layer, &label_dsc, &text_area);
}

} // namespace helix::ui::fpath
