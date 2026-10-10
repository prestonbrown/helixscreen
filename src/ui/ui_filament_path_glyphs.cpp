// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glyph drawing for the filament_path_canvas widget: the hub/selector box
// (incl. its interactive gear badge),
// the buffer ("BUF") box, the animated filament tip, the nozzle heat glow,
// flow particles, tool badges, and the style-dispatched toolhead glyph.
// See ui_filament_path_internal.h for the widget architecture.

#include "ui_filament_path_internal.h"
#include "ui_filament_path_plan.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"

#include "nozzle_renderer_dispatch.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace helix::ui::fpath {

namespace {

// A shaded dot: darker shadow behind, the body, a lighter highlight offset
// toward the top right.
void draw_shaded_dot(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color, int32_t radius) {
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

    // Body: full radius in simple mode, slightly inset on full quality
    int32_t body_r = simple ? radius : LV_MAX(1, radius - 1);
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
}

} // namespace

int32_t draw_hub_box(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t width, int32_t height,
                     lv_color_t bg_color, lv_color_t border_color, const char* label,
                     lv_opa_t bg_opa, bool interactive) {
    lv_layer_t* layer = ctx.layer;
    lv_color_t text_color = ctx.data->theme.color_text;
    const lv_font_t* font = ctx.data->theme.label_font;
    int32_t radius = ctx.data->theme.border_radius;

    // Tappable affordance: a small gear glyph inside the box's right edge,
    // vertically centered with the label, signals that the box opens a context
    // menu. The box grows, about its center, until the centered label and the
    // gear both fit inside.
    const lv_font_t* icon_font = interactive ? theme_manager_get_font("icon_font_sm") : nullptr;
    constexpr int32_t GEAR_PAD = 2;
    int32_t gear_w = 0;
    int32_t gear_h = 0;
    if (icon_font) {
        gear_h = lv_font_get_line_height(icon_font);
        lv_point_t gear_sz;
        lv_text_get_size(&gear_sz, ICON_SETTINGS, icon_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        gear_w = gear_sz.x > 0 ? gear_sz.x : gear_h; // defensive fallback
        int32_t label_w = 0;
        if (label && label[0] && font) {
            lv_point_t lbl_sz;
            lv_text_get_size(&lbl_sz, label, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            label_w = lbl_sz.x;
        }
        width = LV_MAX(width, label_w + 2 * (gear_w + 2 * GEAR_PAD));
    }

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

    if (icon_font) {
        lv_draw_label_dsc_t gear_dsc;
        lv_draw_label_dsc_init(&gear_dsc);
        gear_dsc.color = text_color;
        gear_dsc.font = icon_font;
        gear_dsc.opa = LV_OPA_COVER;
        gear_dsc.text = ICON_SETTINGS;
        gear_dsc.align = LV_TEXT_ALIGN_RIGHT;
        lv_area_t gear_area = {box_area.x1, cy - gear_h / 2, box_area.x2 - GEAR_PAD,
                               cy + gear_h / 2};
        lv_draw_label(layer, &gear_dsc, &gear_area);
    }
    return width;
}

void draw_offpage_stubs(const RenderCtx& ctx, const LinearHubFrame& f) {
    const ThemeCache& theme = ctx.data->theme;
    for (const OffpageStub& stub : f.stubs) {
        if (!stub.present)
            continue;

        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = theme.color_idle;
        line.width = theme.line_width_idle;
        line.opa = LV_OPA_COVER;
        // LVGL's software renderer dashes only horizontal and vertical lines, so the dashes
        // are cut here: one phase runs the whole stub, through both bends.
        for (const pg::DashPiece& piece :
             pg::dash_polyline(stub.pts, 4, (float)(theme.line_width_idle * 3),
                               (float)(theme.line_width_idle * 2))) {
            line.p1.x = piece.a.x;
            line.p1.y = piece.a.y;
            line.p2.x = piece.b.x;
            line.p2.y = piece.b.y;
            lv_draw_line(ctx.layer, &line);
        }

        const lv_font_t* font = theme.label_font;
        if (!font || !stub.labeled)
            continue;
        int32_t x = stub.label_x;
        if (stub.glyph_w > 0) {
            const lv_font_t* icon_font = theme_manager_get_font("icon_font_sm");
            const char* glyph = helix::ui::icon::lookup_codepoint("heat_wave");
            if (icon_font && glyph) {
                lv_draw_label_dsc_t glyph_dsc;
                lv_draw_label_dsc_init(&glyph_dsc);
                glyph_dsc.color = theme.color_warning;
                glyph_dsc.font = icon_font;
                glyph_dsc.text = glyph;
                const int32_t h = lv_font_get_line_height(icon_font);
                lv_area_t area = {x, stub.label_cy - h / 2, x + stub.glyph_w,
                                  stub.label_cy + h / 2};
                lv_draw_label(ctx.layer, &glyph_dsc, &area);
            }
            x += stub.glyph_w + (f.sensor_r + 2);
        }
        const std::string text = offpage_label_text(stub.count);
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = theme.color_muted;
        label_dsc.font = font;
        label_dsc.text = text.c_str();
        label_dsc.text_local = true;
        const int32_t h = lv_font_get_line_height(font);
        lv_area_t area = {x, stub.label_cy - h / 2, stub.label_x + stub.label_w + 4,
                          stub.label_cy + h / 2};
        lv_draw_label(ctx.layer, &label_dsc, &area);
    }
}

// The buffer box's border in the buffer bands' token (neutral on target,
// warning, danger), its fill tinted by the fault or the filament.
BoxColors buffer_box_colors(const FilamentPathData& data, bool has_filament,
                            lv_color_t filament_color) {
    const ThemeCache& theme = data.theme;
    const int buffer_fault_state = data.buffer_fault_state;
    const float buffer_bias = data.buffer_bias;
    const lv_color_t bg_color = theme.color_bg;
    // Border color based on fault state and proportional bias
    lv_color_t border_color;
    lv_color_t buf_bg = bg_color;

    if (buffer_fault_state < 0) {
        border_color = theme.color_hub_border;
        buf_bg = theme.color_hub_bg;
    } else if (buffer_fault_state >= 2) {
        border_color = theme.color_buffer[2];
        buf_bg = lv_color_mix(theme.color_buffer[2], bg_color, LV_OPA_20);
    } else if (buffer_bias > -1.5f) {
        border_color = theme.color_buffer[std::clamp(buffer_fault_state, 0, 2)];
        if (has_filament) {
            buf_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    } else if (buffer_fault_state == 1) {
        border_color = theme.color_buffer[1];
        if (has_filament) {
            buf_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    } else {
        border_color = theme.color_success;
        if (has_filament) {
            buf_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    }

    return {buf_bg, border_color};
}

// Draw buffer box element: a labeled box like HUB/SELECTOR, its border in the
// buffer bands' token (neutral on target, warning, danger)
void draw_buffer_coil(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t hub_h,
                      bool has_filament, lv_color_t filament_color) {
    const ThemeCache& theme = ctx.data->theme;

    // Slightly smaller than hub box — fits "BUF" with comfortable padding
    int32_t box_w = theme.hub_width * 4 / 5;
    int32_t box_h = hub_h;
    if (box_w < 36)
        box_w = 36;
    if (box_h < 16)
        box_h = 16;

    const BoxColors colors = buffer_box_colors(*ctx.data, has_filament, filament_color);
    draw_hub_box(ctx, cx, cy, box_w, box_h, colors.bg, colors.border, ctx.data->buffer_label);
}

// Draw animated filament tip (a glowing dot that moves along the path)
void draw_filament_tip(lv_layer_t* layer, int32_t x, int32_t y, lv_color_t color, int32_t radius) {
    // Outer glow (lighter, larger)
    lv_color_t glow_color = ph_lighten(color, 60);
    draw_shaded_dot(layer, x, y, glow_color, radius + 2);

    // Inner core (bright)
    lv_color_t core_color = ph_lighten(color, 100);
    draw_shaded_dot(layer, x, y, core_color, radius);
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

// The A4T glyph is drawn 6/5 larger than the others at every call site in this
// widget, so the boost is folded in here.
namespace {

int32_t drawn_scale(helix::ToolheadStyle style, int32_t scale) {
    return style == helix::ToolheadStyle::A4T ? scale * 6 / 5 : scale;
}

} // namespace

void draw_toolhead(lv_layer_t* layer, int32_t cx, int32_t cy, std::optional<lv_color_t> filament,
                   int32_t scale, lv_opa_t opa) {
    const auto style = helix::SettingsManager::instance().get_effective_toolhead_style();
    draw_nozzle_for_style(layer, cx, cy, filament, drawn_scale(style, scale), opa);
}

helix::ui::GlyphBounds toolhead_glyph_bounds(int32_t cx, int32_t cy, int32_t scale) {
    const auto style = helix::SettingsManager::instance().get_effective_toolhead_style();
    return helix::ui::toolhead_bounds(style, cx, cy, drawn_scale(style, scale));
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

int32_t toolhead_top_y(int32_t nozzle_y, int32_t extruder_scale) {
    return toolhead_glyph_bounds(0, nozzle_y, extruder_scale).top;
}

void draw_tool_badge(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t scale,
                     const char* label) {
    helix::ui::draw_toolhead_badge(ctx.layer, helix::ui::tool_badge_look(),
                                   toolhead_glyph_bounds(cx, cy, scale), label);
}

} // namespace helix::ui::fpath
