// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Glyph drawing for the filament_path_canvas widget: the hub/selector box
// (incl. its interactive gear badge),
// the buffer ("BUF") box, the animated filament tip, the nozzle heat glow,
// flow chevrons, tool badges, and the style-dispatched toolhead glyph.
// See ui_filament_path_internal.h for the widget architecture.

#include "ui_filament_path_internal.h"
#include "ui_filament_path_plan.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"

#include "helix/ui/text_metrics.h"
#include "hub_buffer_meter.h"
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

namespace {
constexpr int32_t kGearPad = 2;

// The gear for a hub box @p height tall: a short box takes the smaller one, so the gear stays
// about as tall as the box.
const lv_font_t* hub_gear_font(int32_t height) {
    const lv_font_t* font = theme_manager_get_font("icon_font_sm");
    if (font && lv_font_get_line_height(font) > height + 4) {
        if (const lv_font_t* xs = theme_manager_get_font("icon_font_xs"))
            font = xs;
    }
    return font;
}
} // namespace

HubBoxShape hub_box_shape(int32_t cx, int32_t width, int32_t label_w, int32_t gear_w,
                          const HubBoxExtras* extras) {
    constexpr int32_t kLabelPad = 4;
    const int32_t slot_w = extras ? extras->slot_w : 0;
    const int32_t max_right = extras ? extras->max_right : INT32_MAX;
    if (gear_w > 0 && extras && extras->gear_outside) {
        const int32_t w = LV_MAX(width, label_w + 2 * kLabelPad + slot_w);
        if ((int64_t)cx + w / 2 + 2 * kGearPad + gear_w <= max_right)
            return {cx, w, true};
    }
    const int32_t gear_in = gear_w > 0 ? gear_w + 2 * kGearPad : 0;
    const int32_t w = gear_w > 0 ? LV_MAX(width, label_w + 2 * gear_in + slot_w) : width;
    const int64_t over = (int64_t)cx + w / 2 - max_right;
    return {over > 0 ? cx - (int32_t)over : cx, w, false};
}

namespace {
/// The width of the gear drawn in @p icon_font; 0 for none.
int32_t hub_gear_width(const lv_font_t* icon_font) {
    if (!icon_font)
        return 0;
    const int32_t measured = helix::ui::text_width(ICON_SETTINGS, icon_font);
    return measured > 0 ? measured : lv_font_get_line_height(icon_font); // defensive fallback
}

} // namespace

const char* hub_section_label(const FilamentPathData& data) {
    return data.topology == 0 ? "SELECTOR" : "HUB";
}

bool hub_section_interactive(const FilamentPathData& data) {
    return data.hub_callback != nullptr && data.hub_has_actions && !data.fixed_selector;
}

HubBoxExtras hub_section_extras(const FilamentPathData& data, const BaseGeometry& g,
                                const LinearHubFrame& f) {
    HubBoxExtras extras;
    extras.gear_outside = data.view_hub_h > 0;
    extras.max_right = hub_box_right_limit(data, g, f);
    extras.slot_w = f.buffer_in_hub ? hub_buffer_glyph_width(f.hub_h) : 0;
    return extras;
}

lv_area_t hub_box_drawn_area(const FilamentPathData& data, const BaseGeometry& g,
                             const LinearHubFrame& f) {
    const lv_area_t box = hub_box_area(data, g, f);
    const lv_font_t* font = data.theme.label_font;
    const int32_t label_w = font ? helix::ui::text_width(hub_section_label(data), font) : 0;
    const int32_t gear_w =
        hub_section_interactive(data) ? hub_gear_width(hub_gear_font(f.hub_h)) : 0;
    const HubBoxExtras extras = hub_section_extras(data, g, f);
    const HubBoxShape shape =
        hub_box_shape((box.x1 + box.x2) / 2, box.x2 - box.x1, label_w, gear_w, &extras);
    return {shape.cx - shape.width / 2, box.y1, shape.cx + shape.width / 2, box.y2};
}

int32_t draw_hub_box(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t width, int32_t height,
                     lv_color_t bg_color, lv_color_t border_color, const char* label,
                     lv_opa_t bg_opa, bool interactive, HubBoxExtras* extras) {
    lv_layer_t* layer = ctx.layer;
    lv_color_t text_color = ctx.data->theme.color_text;
    const lv_font_t* font = ctx.data->theme.label_font;
    int32_t radius = ctx.data->theme.border_radius;
    const int32_t slot_w = extras ? extras->slot_w : 0;

    // Tappable affordance: a small gear glyph, vertically centered with the label, signals
    // that the box opens a context menu (hub_box_shape() places it and sizes the box).
    const lv_font_t* icon_font = interactive ? hub_gear_font(height) : nullptr;
    const int32_t gear_w = hub_gear_width(icon_font);
    const int32_t gear_h = icon_font ? lv_font_get_line_height(icon_font) : 0;
    const int32_t label_w = label && label[0] && font ? helix::ui::text_width(label, font) : 0;
    const HubBoxShape shape = hub_box_shape(cx, width, label_w, gear_w, extras);
    cx = shape.cx;
    width = shape.width;
    const bool gear_outside = shape.gear_outside;
    const int32_t gear_in = icon_font && !gear_outside ? gear_w + 2 * kGearPad : 0;

    // Background
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = bg_color;
    fill_dsc.opa = bg_opa;
    fill_dsc.radius = radius;

    lv_area_t box_area = {cx - width / 2, cy - height / 2, cx + width / 2, cy + height / 2};
    if (extras)
        extras->box = box_area;
    lv_draw_fill(layer, &fill_dsc, &box_area);

    // Border
    lv_draw_border_dsc_t border_dsc;
    lv_draw_border_dsc_init(&border_dsc);
    border_dsc.color = border_color;
    border_dsc.width = 2;
    border_dsc.radius = radius;
    lv_draw_border(layer, &border_dsc, &box_area);

    // The slot stands at the box's right end, left of a gear inside it.
    const int32_t slot_right = box_area.x2 - gear_in;
    if (extras)
        extras->slot = {slot_right - slot_w, box_area.y1, slot_right, box_area.y2};

    // Label: centered in what the slot and a gear inside the box leave on their left; with
    // neither, in the box.
    if (label && label[0] && font) {
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = text_color;
        label_dsc.font = font;
        label_dsc.align = LV_TEXT_ALIGN_CENTER;
        label_dsc.text = label;

        int32_t font_h = lv_font_get_line_height(font);
        const int32_t right = slot_w > 0 ? slot_right - slot_w : box_area.x2;
        const int32_t left = slot_w > 0 ? box_area.x1 + gear_in : box_area.x1;
        lv_area_t label_area = {left, cy - font_h / 2, right, cy + font_h / 2};
        lv_draw_label(layer, &label_dsc, &label_area);
    }

    int32_t gear_right = box_area.x2;
    if (icon_font) {
        lv_draw_label_dsc_t gear_dsc;
        lv_draw_label_dsc_init(&gear_dsc);
        gear_dsc.color = text_color;
        gear_dsc.font = icon_font;
        gear_dsc.opa = LV_OPA_COVER;
        gear_dsc.text = ICON_SETTINGS;
        gear_dsc.align = LV_TEXT_ALIGN_RIGHT;
        if (gear_outside)
            gear_right = box_area.x2 + 2 * kGearPad + gear_w;
        lv_area_t gear_area = {box_area.x1, cy - gear_h / 2,
                               gear_outside ? gear_right : box_area.x2 - kGearPad, cy + gear_h / 2};
        lv_draw_label(layer, &gear_dsc, &gear_area);
    }
    if (extras)
        extras->gear_right = gear_right;
    return width;
}

void draw_count_pill(lv_layer_t* layer, const lv_area_t& chip, const CountPillStyle& style,
                     const char* text, lv_text_align_t align, int32_t text_x1, int32_t text_x2) {
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = style.bg;
    fill.opa = LV_OPA_COVER;
    fill.radius = LV_RADIUS_CIRCLE;
    lv_draw_fill(layer, &fill, &chip);
    lv_draw_border_dsc_t border;
    lv_draw_border_dsc_init(&border);
    border.color = style.border;
    border.width = 1;
    border.radius = LV_RADIUS_CIRCLE;
    lv_draw_border(layer, &border, &chip);
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.color = style.text;
    label.font = style.font;
    label.align = align;
    label.text = text;
    label.text_local = 1;
    const int32_t cy = (chip.y1 + chip.y2) / 2;
    const lv_area_t area = {text_x1, cy - style.line_h / 2, text_x2, cy + style.line_h / 2};
    lv_draw_label(layer, &label, &area);
}

void draw_offpage_chips(const RenderCtx& ctx, const LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    data->hits.offpage_valid[0] = data->hits.offpage_valid[1] = false;
    const lv_font_t* font = theme.label_font;
    if (!font || !(data->hits.hub_anchor_valid || data->hits.hub_valid))
        return;
    const lv_area_t hub = data->hits.hub_anchor_valid ? data->hits.hub_anchor : data->hits.hub;
    const int32_t pad = LV_MAX(4, f.hub_h / 4);
    const int32_t gap = f.sensor_r + 2;
    const int32_t h = f.hub_h;
    const lv_font_t* icon_font = theme_manager_get_font("icon_font_sm");
    const char* glyph = helix::ui::icon::lookup_codepoint("heat_wave");
    const CountPillStyle style{theme.color_hub_bg, theme.color_hub_border, theme.color_text, font,
                               lv_font_get_line_height(font)};
    for (int side = 0; side < 2; ++side) {
        const OffpageStub& stub = f.stubs[side];
        if (!stub.present)
            continue;
        const int32_t drying_w =
            stub.drying && icon_font && glyph ? helix::ui::text_width(glyph, icon_font) : 0;
        // "+2 units" where it fits beside the hub, inside the canvas and clear of the edge
        // controls on the hub's rows; "+2" where it does not; "+2" without its glyph, or no
        // chip at all, before it covers the hub.
        const bool shares_rows = data->keepout_y1 <= data->keepout_y0 ||
                                 (hub.y2 >= data->keepout_y0 && hub.y1 <= data->keepout_y1);
        const int32_t keep = shares_rows ? data->edge_reserve : 0;
        const int32_t min_x = ctx.geo.x_off + keep;
        const int32_t max_x = ctx.geo.x_off + ctx.geo.width - keep;
        const std::string long_text = "+" + offpage_label_text(stub.count);
        const std::string short_text = "+" + std::to_string(stub.count);
        const OffpageChipFit fit =
            fit_offpage_chip(side, hub, helix::ui::text_width(long_text.c_str(), font),
                             helix::ui::text_width(short_text.c_str(), font),
                             drying_w > 0 ? drying_w + pad / 2 : 0, pad, h, gap, min_x, max_x);
        if (!fit.shown)
            continue;
        const std::string& text = fit.long_text ? long_text : short_text;
        const lv_area_t& chip = fit.rect;
        const int32_t glyph_w = fit.glyph ? drying_w : 0;
        const int32_t extra = glyph_w > 0 ? glyph_w + pad / 2 : 0;

        // A drying unit among them leads the text with a heat-wave glyph.
        const int32_t x = chip.x1 + pad;
        draw_count_pill(ctx.layer, chip, style, text.c_str(), LV_TEXT_ALIGN_AUTO, x + extra,
                        chip.x2 - pad + 1);
        if (glyph_w > 0) {
            lv_draw_label_dsc_t glyph_dsc;
            lv_draw_label_dsc_init(&glyph_dsc);
            glyph_dsc.color = theme.color_warning;
            glyph_dsc.font = icon_font;
            glyph_dsc.text = glyph;
            const int32_t gh = lv_font_get_line_height(icon_font);
            const int32_t cy = (chip.y1 + chip.y2) / 2;
            const lv_area_t area = {x, cy - gh / 2, x + glyph_w, cy + gh / 2};
            lv_draw_label(ctx.layer, &glyph_dsc, &area);
        }

        data->hits.offpage[side] = chip;
        data->hits.offpage_valid[side] = true;
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

namespace {
void tension_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                  lv_color_t color, int32_t width, bool round = true) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = width;
    line.opa = LV_OPA_COVER;
    line.round_start = round;
    line.round_end = round;
    line.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
    line.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
    lv_draw_line(layer, &line);
}

void tension_fill(lv_layer_t* layer, const lv_area_t& a, lv_color_t color, int32_t radius,
                  lv_opa_t opa) {
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = color;
    fill.opa = opa;
    fill.radius = radius;
    lv_draw_fill(layer, &fill, &a);
}

void tension_border(lv_layer_t* layer, const lv_area_t& a, lv_color_t color, int32_t radius) {
    lv_draw_border_dsc_t border;
    lv_draw_border_dsc_init(&border);
    border.color = color;
    border.width = 1;
    border.radius = radius;
    lv_draw_border(layer, &border, &a);
}

// A chevron of half-height @p s with its tip at (x, y), pointing right (+1) or left (-1).
void tension_chevron(lv_layer_t* layer, int32_t x, int32_t y, int32_t s, int dir, lv_color_t c,
                     int32_t w) {
    tension_line(layer, x - dir * s, y - s, x, y, c, w);
    tension_line(layer, x - dir * s, y + s, x, y, c, w);
}

// Two chevrons about a bar at @p x: pointing in at it (compressed) or away from it (expanded).
void tension_squeeze(lv_layer_t* layer, int32_t x, int32_t y, int32_t s, bool inward, lv_color_t c,
                     int32_t w) {
    tension_line(layer, x, y - s, x, y + s, c, w, false);
    if (inward) {
        tension_chevron(layer, x - 2, y, s, +1, c, w);
        tension_chevron(layer, x + 2, y, s, -1, c, w);
    } else {
        tension_chevron(layer, x - 2 - s, y, s, -1, c, w);
        tension_chevron(layer, x + 2 + s, y, s, +1, c, w);
    }
}

// A spring on its side: short leads at each end, @p teeth zigzag peaks between.
void tension_spring(lv_layer_t* layer, int32_t x1, int32_t x2, int32_t y, int32_t amp, int teeth,
                    lv_color_t c, int32_t w) {
    const int32_t lead = LV_MAX(1, (x2 - x1) / 8);
    const int32_t a = x1 + lead;
    const int32_t b = x2 - lead;
    tension_line(layer, x1, y, a, y, c, w);
    tension_line(layer, b, y, x2, y, c, w);
    const int segs = 2 * teeth;
    int32_t px = a;
    int32_t py = y;
    for (int i = 0; i < segs; ++i) {
        const int32_t nx = a + (b - a) * (2 * i + 1) / (2 * segs);
        const int32_t ny = y + ((i % 2) ? amp : -amp);
        tension_line(layer, px, py, nx, ny, c, w);
        px = nx;
        py = ny;
    }
    tension_line(layer, px, py, b, y, c, w);
}
} // namespace

float buffer_tension_position(const FilamentPathData& d) {
    return helix::ui::buffer_meter_position(d.buffer_bias, d.buffer_state);
}

lv_color_t buffer_thumb_color(const FilamentPathData& d) {
    const ThemeCache& t = d.theme;
    if (d.buffer_fault_state >= 2)
        return t.color_buffer[2];
    if (d.buffer_fault_state == 1)
        return t.color_buffer[1];
    // Off center is normal operation: a lighter tint of the accent, not a warning.
    if (std::fabs(buffer_tension_position(d)) > kBufferOffCenter)
        return lv_color_mix(t.color_text, t.color_accent, LV_OPA_40);
    return t.color_accent;
}

void draw_buffer_tension(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t cap_h,
                         lv_area_t* hit, int32_t max_w) {
    const FilamentPathData& d = *ctx.data;
    const ThemeCache& t = d.theme;
    lv_layer_t* layer = ctx.layer;
    const float pos = buffer_tension_position(d);
    const lv_color_t thumb_color = buffer_thumb_color(d);

    const int32_t stroke = cap_h >= 16 ? 2 : 1;
    const BufferControlGeometry geo = buffer_control_geometry(cap_h, max_w);
    const int32_t half = geo.half;
    const int32_t cell = geo.cell;
    const int32_t radius = cap_h / 2;
    const lv_area_t cap = {cx - half, cy - cap_h / 2, cx + half + cell, cy + cap_h / 2};

    // The capsule, in the hub chip's colors, half see-through so the tube shows through it.
    tension_fill(layer, cap, t.color_hub_bg, radius, LV_OPA_50);
    tension_border(layer, cap, t.color_hub_border, radius);

    // The groove, and a notch at its center where the tube crosses: neutral.
    const int32_t inset = LV_MAX(2, cap_h / 5);
    tension_line(layer, cx - half + radius + 2, cy, cx + half - radius - 2, cy, t.color_hub_border,
                 stroke);
    tension_line(layer, cx, cap.y1 + 1, cx, cap.y1 + inset, t.color_muted, 1, false);
    tension_line(layer, cx, cap.y2 - inset, cx, cap.y2 - 1, t.color_muted, 1, false);

    // The ends: chevrons squeezing in on the compressed (left) end, pulling apart on the
    // expanded (right) end.
    const int32_t gs = LV_MAX(2, cap_h / 5);
    tension_squeeze(layer, cx - half + radius + gs + 2, cy, gs, true, t.color_muted, stroke);
    tension_squeeze(layer, cx + half - radius - gs - 2, cy, gs, false, t.color_muted, stroke);

    // The thumb, a little taller than the capsule, ringed by a soft glow in its color.
    const int32_t travel = half - radius - 2 * gs - 4;
    const int32_t tx = cx - static_cast<int32_t>(pos * travel);
    const int32_t tw = geo.thumb_w;
    const int32_t th = geo.thumb_h;
    const lv_area_t thumb = {tx - tw / 2, cy - th / 2, tx + tw / 2, cy + th / 2};
    const lv_area_t glow = {thumb.x1 - 3, thumb.y1 - 3, thumb.x2 + 3, thumb.y2 + 3};
    tension_fill(layer, glow, thumb_color, tw, LV_OPA_30);
    tension_fill(layer, {thumb.x1 - 1, thumb.y1 - 1, thumb.x2 + 1, thumb.y2 + 1}, t.color_bg, tw,
                 LV_OPA_COVER);
    tension_fill(layer, thumb, thumb_color, tw, LV_OPA_COVER);

    // The detail button: a divider, then a spring where the hub chip carries its gear.
    const int32_t div = cx + half;
    tension_line(layer, div, cap.y1 + 3, div, cap.y2 - 3, t.color_hub_border, 1, false);
    const lv_font_t* icon_font = theme_manager_get_font("icon_font_sm");
    const int32_t gear = icon_font ? lv_font_get_line_height(icon_font) : cap_h;
    const int32_t glyph_w = LV_MIN(gear, cap_h);
    const int32_t bx = div + cell / 2;
    tension_spring(layer, bx - glyph_w / 2 + 1, bx + glyph_w / 2 - 1, cy, LV_MAX(2, glyph_w / 4), 3,
                   t.color_text, stroke);

    *hit = buffer_control_hit(cx, cy, cap_h, max_w);
}

int32_t hub_buffer_glyph_width(int32_t hub_h) {
    return helix::ui::buffer_meter_slot_width(hub_h);
}

void draw_hub_buffer_glyph(const RenderCtx& ctx, const lv_area_t& slot) {
    const FilamentPathData& d = *ctx.data;
    const ThemeCache& t = d.theme;
    helix::ui::draw_buffer_meter(ctx.layer, slot, buffer_tension_position(d), d.buffer_fault_state,
                                 {t.color_bg, t.color_hub_border, t.color_text, t.color_error,
                                  t.color_accent, t.color_warning, t.color_success});
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
// Flow chevrons
// ============================================================================
// Chevrons streaming along an active tube to show filament moving: down toward
// the nozzle on a load or extrude, back up on an unload or retract. Sized from
// the tube gauge and phased by flow_offset for animation.

pg::ChevronSize flow_chevron_size(const FilamentPathData& data) {
    return pg::chevron_size_for_gauge(static_cast<float>(data.theme.tube_gauge));
}

bool flow_visible(const FilamentPathData& data) {
    return data.anim.flow_active || data.anim.manual_motion != helix::FilamentMotion::Idle;
}

bool flow_reversed(const FilamentPathData& data) {
    if (data.anim.manual_motion != helix::FilamentMotion::Idle)
        return data.anim.manual_motion == helix::FilamentMotion::Retract;
    return data.anim.direction == AnimDirection::UNLOADING;
}

void draw_flow(lv_layer_t* layer, const FilamentPathData& data, const pg::FilamentPath& path,
               lv_color_t filament, const bool* seg_shown, const pg::PathBox* avoid, int n_avoid) {
    // Ink that reads against the filament the chevrons ride on, whatever its color.
    const lv_color_t ink = theme_manager_get_contrast_color(filament);
    helix::ui::draw_flow_chevrons(layer, path, flow_chevron_size(data),
                                  static_cast<float>(data.anim.flow_offset), flow_reversed(data),
                                  ink, kFlowChevronOpa, seg_shown, avoid, n_avoid);
}

// ============================================================================
// Toolhead glyph
// ============================================================================

// Nozzle tip Y for the configured style — anchors the heat glow halo.
int32_t toolhead_tip_y(int32_t nozzle_y, int32_t extruder_scale) {
    const auto style = helix::SettingsManager::instance().get_effective_toolhead_style();
    if (const helix::ui::ToolheadArt* art = helix::ui::toolhead_art_for(style)) {
        const helix::ui::ToolheadArtRect r = helix::ui::toolhead_art_rect(
            *art, 0, nozzle_y, helix::ui::toolhead_drawn_scale(style, extruder_scale));
        return r.y + static_cast<int32_t>(r.h * art->tip_y);
    }
    switch (style) {
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

helix::ui::ToolheadInlet path_toolhead_inlet(int32_t nozzle_y, int32_t extruder_scale) {
    const auto style = helix::SettingsManager::instance().get_effective_toolhead_style();
    return helix::ui::toolhead_inlet(style, nozzle_y,
                                     helix::ui::toolhead_drawn_scale(style, extruder_scale));
}

int32_t toolhead_top_y(int32_t nozzle_y, int32_t extruder_scale) {
    return toolhead_glyph_bounds(0, nozzle_y, extruder_scale).top;
}

int32_t toolhead_standing_on(int32_t bottom_y, int32_t scale) {
    const auto style = helix::SettingsManager::instance().get_effective_toolhead_style();
    return helix::ui::toolhead_nozzle_standing_on(style, bottom_y,
                                                  helix::ui::toolhead_drawn_scale(style, scale));
}

void draw_tool_badge(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t scale,
                     const char* label) {
    helix::ui::draw_toolhead_badge(ctx.layer, helix::ui::tool_badge_look(),
                                   toolhead_glyph_bounds(cx, cy, scale), label);
}

} // namespace helix::ui::fpath
