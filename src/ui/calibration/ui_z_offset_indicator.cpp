// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_z_offset_indicator.h"

#include "ui_toolhead_view.h"

#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"
#include "z_offset_tape.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>

using namespace helix;

// ============================================================================
// Widget Data
// ============================================================================

struct ZOffsetIndicatorData {
    int32_t current_pos = 0;        // Current animated position (0.1 micron units for smooth anim)
    int32_t target_pos = 0;         // Target position (0.1 micron units)
    int32_t arrow_progress = 0;     // 0-255, draw-in progress (base to tip)
    int32_t arrow_opacity = 0;      // 0-255, overall opacity (for fade-out phase)
    int arrow_direction = 0;        // +1 (farther/up) or -1 (closer/down)
    bool bound_value_shown = false; // bind_microns has placed its first value

    // Tape mode: the nozzle stays put and the ruler scrolls past it.
    bool tape = false;
    int step_microns = helix::ztape::step_microns(helix::ztape::kDefaultStepIndex);
    int estimate_microns = helix::ztape::kNoEstimate;

    // Cached theme colors (resolved once at creation, not per-frame)
    lv_color_t color_text_muted = {};
    lv_color_t color_text = {};
    lv_color_t color_primary = {};
    lv_color_t color_text_subtle = {};
    lv_color_t color_elevated = {};
    lv_color_t color_overlay = {};
    lv_color_t color_border = {};
    lv_color_t color_warning = {};
    lv_color_t color_secondary = {};
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

static constexpr ScaleRange kScaleRanges[] = {
    {100, 50, 2},     // ±0.10mm, ticks every 0.05mm
    {250, 100, 1},    // ±0.25mm, ticks every 0.1mm
    {500, 250, 2},    // ±0.50mm, ticks every 0.25mm
    {1000, 500, 1},   // ±1.0mm, ticks every 0.5mm
    {2000, 1000, 0},  // ±2.0mm, ticks every 1mm
    {5000, 2000, 0},  // ±5.0mm, ticks every 2mm
    {10000, 5000, 0}, // ±10mm, ticks every 5mm
};
static constexpr int kNumScaleRanges = sizeof(kScaleRanges) / sizeof(kScaleRanges[0]);

/// Pick the best scale range for a given value in microns.
static const ScaleRange& pick_scale_range(int microns) {
    int abs_val = std::abs(microns);
    for (int i = 0; i < kNumScaleRanges; i++) {
        // 80% of range as headroom threshold
        if (abs_val <= kScaleRanges[i].range_microns * 80 / 100) {
            return kScaleRanges[i];
        }
    }
    return kScaleRanges[kNumScaleRanges - 1]; // Largest range as fallback
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

// ============================================================================
// Tape mode
// ============================================================================

namespace {

/// The estimated paper is drawn a little fainter than the paper on a known bed.
constexpr lv_opa_t kPaperOpa = 191;

/// The tape's geometry, in the canvas's pixels for a reference box; rows scale
/// with the box height, the nozzle and the bed with its width.
struct TapeLayout {
    bool compact;     ///< the micro screen's narrow column: short notes, no pill
    bool note_bottom; ///< the bed note sits at the bottom of the box
    int ref_w, ref_h;
    int ruler_x, line_top, line_bottom, tick_top, tick_bottom, tip_y;
    int tick_half, label_right, marker_w, marker_half;
    int nozzle_cx, nozzle_scale_pct, dash_end;
    int zig_x0, zig_x1, zig_y, zig_amp, zig_period;
    int bed_x, bed_right, bed_h, bed_radius;
    int paper_x, paper_w, paper_h, static_paper_top;
    int note_gap, note_bottom_margin;
};

// 800x480: the 278x214 diagram of the WizardHandsOn board.
constexpr TapeLayout kRegular{false, false, 278, 214, 46,  32,  182, 32,  146, 107, 7,
                              36,    9,     6,   160, 100, 160, 70,  270, 160, 6,   40,
                              70,    6,     14,  3,   92,  140, 4,   178, 21,  0};
// 480x800: the full-width diagram of the ZOffsetPortrait board.
constexpr TapeLayout kWide{false, true, 426, 250, 46,  30,  190, 30,  190, 110, 7,
                           36,    9,    6,   198, 100, 198, 80,  380, 202, 6,   50,
                           70,    6,    14,  3,   110, 200, 4,   214, 21,  0};
// 480x272: the 128 px column of the ZOffsetMicro board.
constexpr TapeLayout kCompact{true, true, 128, 174, 30, 18, 126, 18,  126, 70, 5,
                              24,   7,    5,   82,  50, 82, 38,  122, 134, 4,  20,
                              38,   4,    8,   2,   50, 64, 3,   140, 0,   6};

const TapeLayout& pick_tape_layout(int32_t w, int32_t h) {
    if (w < 200) {
        return kCompact;
    }
    return (w * 10 > h * 16) ? kWide : kRegular;
}

const lv_font_t* tape_font(const char* token) {
    const lv_font_t* f = theme_manager_get_font(token);
    return f ? f : lv_font_get_default();
}

/// A dashed segment in any direction: the renderer dashes only straight ones.
void dashed_segment(lv_layer_t* layer, lv_color_t color, lv_opa_t opa, int32_t width,
                    lv_point_precise_t a, lv_point_precise_t b, int32_t dash, int32_t gap) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0) {
        return;
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.opa = opa;
    d.width = width;
    for (double t = 0.0; t < len; t += dash + gap) {
        const double t1 = std::min(len, t + dash);
        d.p1.x = static_cast<lv_value_precise_t>(a.x + dx * t / len);
        d.p1.y = static_cast<lv_value_precise_t>(a.y + dy * t / len);
        d.p2.x = static_cast<lv_value_precise_t>(a.x + dx * t1 / len);
        d.p2.y = static_cast<lv_value_precise_t>(a.y + dy * t1 / len);
        lv_draw_line(layer, &d);
    }
}

void fill_rect(lv_layer_t* layer, lv_color_t color, lv_opa_t opa, int32_t radius, int32_t x1,
               int32_t y1, int32_t x2, int32_t y2) {
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = color;
    r.bg_opa = opa;
    r.radius = radius;
    const lv_area_t a{x1, y1, x2 - 1, y2 - 1};
    lv_draw_rect(layer, &r, &a);
}

void draw_text(lv_layer_t* layer, const char* text, const lv_font_t* font, lv_color_t color,
               lv_text_align_t align, int32_t x1, int32_t y1, int32_t x2) {
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.text = text;
    l.text_local = 1;
    l.font = font;
    l.color = color;
    l.align = align;
    const lv_area_t a{x1, y1, x2, y1 + lv_font_get_line_height(font)};
    lv_draw_label(layer, &l, &a);
}

/// The canvas nozzle: a heater block, a neck, a cone and an amber tip whose
/// bottom edge is the tip row.
void draw_nozzle(lv_layer_t* layer, const ZOffsetIndicatorData& d, int32_t cx, int32_t tip,
                 int pct) {
    auto s = [pct](int v) { return static_cast<int32_t>(v * pct / 100); };
    fill_rect(layer, d.color_elevated, LV_OPA_COVER, s(8), cx - s(42), tip - s(114), cx + s(42),
              tip - s(56));
    fill_rect(layer, d.color_text_subtle, LV_OPA_COVER, 0, cx - s(18), tip - s(56), cx + s(18),
              tip - s(34));
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = d.color_text_muted;
    t.opa = LV_OPA_COVER;
    // The cone: a trapezoid, as two triangles.
    t.p[0] = {static_cast<lv_value_precise_t>(cx - s(18)),
              static_cast<lv_value_precise_t>(tip - s(34))};
    t.p[1] = {static_cast<lv_value_precise_t>(cx + s(18)),
              static_cast<lv_value_precise_t>(tip - s(34))};
    t.p[2] = {static_cast<lv_value_precise_t>(cx + s(6)),
              static_cast<lv_value_precise_t>(tip - s(6))};
    lv_draw_triangle(layer, &t);
    t.p[1] = {static_cast<lv_value_precise_t>(cx - s(6)),
              static_cast<lv_value_precise_t>(tip - s(6))};
    lv_draw_triangle(layer, &t);
    fill_rect(layer, d.color_warning, LV_OPA_COVER, 0, cx - s(3), tip - s(6), cx + s(3), tip);
}

void tape_draw(lv_layer_t* layer, const ZOffsetIndicatorData& d, const lv_area_t& c) {
    using namespace helix::ztape;
    const int32_t w = lv_area_get_width(&c);
    const int32_t h = lv_area_get_height(&c);
    const TapeLayout& L = pick_tape_layout(w, h);
    const int32_t x0 = c.x1, y0 = c.y1;
    auto ry = [&](int v) { return static_cast<int32_t>(v * h / L.ref_h); };
    auto rx = [&](int v) { return static_cast<int32_t>(v * w / L.ref_w); };
    auto from_right = [&](int v) { return w - (L.ref_w - v); };

    const int value = d.current_pos / 10;
    const Zoom zoom = zoom_for_step(d.step_microns);
    const int tip = ry(L.tip_y), tick_top = ry(L.tick_top), tick_bottom = ry(L.tick_bottom);
    const bool long_notes = !L.compact;

    const lv_font_t* tick_font = tape_font("zo_tape_font_tick");
    const lv_font_t* note_font = tape_font("zo_tape_font_note");

    // The ruler and its ticks.
    fill_rect(layer, d.color_elevated, LV_OPA_COVER, 0, x0 + L.ruler_x, y0 + ry(L.line_top),
              x0 + L.ruler_x + 2, y0 + ry(L.line_bottom));
    const int32_t font_h = lv_font_get_line_height(tick_font);
    for (const Tick& t : ticks(value, zoom, tip, tick_top, tick_bottom)) {
        fill_rect(layer, d.color_text_subtle, LV_OPA_COVER, 0, x0 + L.ruler_x + 1 - L.tick_half,
                  y0 + t.y, x0 + L.ruler_x + 1 + L.tick_half, y0 + t.y + 1);
        const std::string label = tick_label(t.microns, zoom);
        draw_text(layer, label.c_str(), tick_font, d.color_text_subtle, LV_TEXT_ALIGN_RIGHT, x0,
                  y0 + t.y - font_h / 2, x0 + L.label_right);
    }

    // The bed, estimated or unknown.
    const BedPlacement bed = place_bed(value, d.estimate_microns, zoom, tip, tick_top, tick_bottom,
                                       ry(L.static_paper_top));
    const int32_t nozzle_cx = rx(L.nozzle_cx);
    const int32_t paper_w = rx(L.paper_w);
    const int32_t paper_x = nozzle_cx - paper_w / 2 + 2;
    const int32_t bed_x = L.bed_x, bed_r = w - L.bed_right;
    const char* note = nullptr;
    lv_color_t note_color = d.color_text_subtle;
    int32_t note_y = 0;
    if (bed.view == BedView::Estimated || bed.view == BedView::PastLastTime) {
        const bool past = bed.view == BedView::PastLastTime;
        const lv_color_t edge = past ? d.color_warning : d.color_secondary;
        fill_rect(layer, d.color_text, kPaperOpa, 0, x0 + paper_x, y0 + bed.paper_top,
                  x0 + paper_x + paper_w, y0 + bed.paper_top + L.paper_h);
        const int32_t by1 = y0 + bed.paper_top + L.paper_h, by2 = by1 + L.bed_h;
        fill_rect(layer, d.color_border, LV_OPA_60, L.bed_radius, x0 + bed_x, by1, x0 + bed_r, by2);
        const int32_t bw = L.compact ? 1 : 2;
        dashed_segment(layer, edge, LV_OPA_COVER, bw, {(float)(x0 + bed_x), (float)by1},
                       {(float)(x0 + bed_r), (float)by1}, 4, 3);
        dashed_segment(layer, edge, LV_OPA_COVER, bw, {(float)(x0 + bed_x), (float)by2},
                       {(float)(x0 + bed_r), (float)by2}, 4, 3);
        dashed_segment(layer, edge, LV_OPA_COVER, bw, {(float)(x0 + bed_x), (float)by1},
                       {(float)(x0 + bed_x), (float)by2}, 4, 3);
        dashed_segment(layer, edge, LV_OPA_COVER, bw, {(float)(x0 + bed_r), (float)by1},
                       {(float)(x0 + bed_r), (float)by2}, 4, 3);
        note = past ? (long_notes ? lv_tr("past last time\xE2\x80\x99s position")
                                  : lv_tr("past last time"))
                    : (long_notes ? lv_tr("estimated bed, from your last calibration")
                                  : lv_tr("estimated bed"));
        note_color = edge;
        note_y = bed.paper_top + L.note_gap;
    } else {
        // A break, then the bed at the bottom with the paper on it.
        const int32_t zy = ry(L.zig_y);
        const int32_t zx1 = from_right(L.zig_x1);
        for (int32_t x = L.zig_x0, i = 0; x < zx1; x += L.zig_period / 2, ++i) {
            const int32_t xn = std::min<int32_t>(zx1, x + L.zig_period / 2);
            const int32_t ya = zy + ((i % 2) ? L.zig_amp : -L.zig_amp);
            const int32_t yb = zy + ((i % 2) ? -L.zig_amp : L.zig_amp);
            dashed_segment(layer, d.color_elevated, LV_OPA_COVER, L.compact ? 1 : 2,
                           {(float)(x0 + x), (float)(y0 + (i == 0 ? zy : ya))},
                           {(float)(x0 + xn), (float)(y0 + yb)}, 3, 3);
        }
        const int32_t paper_top = ry(L.static_paper_top);
        const int32_t by1 = paper_top + L.paper_h;
        fill_rect(layer, d.color_border, LV_OPA_COVER, L.bed_radius, x0 + bed_x, y0 + by1,
                  x0 + bed_r, y0 + by1 + L.bed_h);
        fill_rect(layer, d.color_text, LV_OPA_90, 0, x0 + paper_x, y0 + paper_top,
                  x0 + paper_x + paper_w, y0 + paper_top + L.paper_h);
        const bool has_estimate = bed.view == BedView::FarBelow;
        note = has_estimate ? (long_notes ? lv_tr("estimated bed is further below")
                                          : lv_tr("bed further below"))
                            : (long_notes ? lv_tr("bed: distance unknown until the paper drags")
                                          : lv_tr("bed: distance unknown"));
        note_y = by1 + L.bed_h + 2;
    }
    if (note) {
        const int32_t nh = lv_font_get_line_height(note_font);
        const int32_t ny = L.note_bottom ? h - L.note_bottom_margin - nh : note_y;
        const int32_t nx1 = L.compact ? 4 : bed_x;
        const int32_t nx2 = L.compact ? w - 4 : bed_r;
        draw_text(layer, note, note_font, note_color, LV_TEXT_ALIGN_CENTER, x0 + nx1, y0 + ny,
                  x0 + nx2);
    }

    // The fixed nozzle, then the marker at its tip.
    // A box shorter than the reference shrinks the nozzle with its rows.
    const int nozzle_pct =
        static_cast<int>(L.nozzle_scale_pct * std::min<int32_t>(h, L.ref_h) / L.ref_h);
    draw_nozzle(layer, d, x0 + nozzle_cx, y0 + tip, nozzle_pct);
    dashed_segment(layer, d.color_warning, LV_OPA_COVER, 2,
                   {(float)(x0 + L.ruler_x + 2), (float)(y0 + tip)},
                   {(float)(x0 + nozzle_cx), (float)(y0 + tip)}, 3, 2);
    lv_draw_triangle_dsc_t m;
    lv_draw_triangle_dsc_init(&m);
    m.color = d.color_warning;
    m.opa = LV_OPA_COVER;
    const int32_t mx = x0 + L.ruler_x - (L.marker_w - 1);
    m.p[0] = {(float)mx, (float)(y0 + tip - L.marker_half)};
    m.p[1] = {(float)mx, (float)(y0 + tip + L.marker_half)};
    m.p[2] = {(float)(mx + L.marker_w), (float)(y0 + tip)};
    lv_draw_triangle(layer, &m);

    // Range, last so the nozzle never covers it: a pill top right, or a
    // plain note on the micro column.
    {
        const std::string range =
            L.compact ? range_text(zoom) : fmt::format("Z \xC2\xB7 {}", range_text(zoom));
        lv_point_t sz;
        lv_text_get_size(&sz, range.c_str(), note_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (L.compact) {
            draw_text(layer, range.c_str(), note_font, d.color_text_subtle, LV_TEXT_ALIGN_RIGHT, x0,
                      y0 + 3, c.x2 - 5);
        } else {
            const int32_t pw = sz.x + 16, ph = sz.y + 4;
            fill_rect(layer, d.color_overlay, LV_OPA_COVER, ph / 2, c.x2 + 1 - pw, y0 + 2, c.x2 + 1,
                      y0 + 2 + ph);
            draw_text(layer, range.c_str(), note_font, d.color_text_muted, LV_TEXT_ALIGN_CENTER,
                      c.x2 + 1 - pw, y0 + 4, c.x2);
        }
    }
}

} // namespace

static void indicator_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    if (data->tape) {
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        tape_draw(layer, *data, area);
        return;
    }

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

    // --- The configured head, fitted to the space right of the scale ---
    const lv_area_t head_box{scale_x + tick_half_w + label_pad, scale_top, coords.x1 + w - 1,
                             scale_bottom};
    const auto fit = helix::ui::fit_toolhead(
        helix::SettingsManager::instance().get_effective_toolhead_style(), head_box, 12);
    const int32_t nozzle_cx = fit.cx;
    const int32_t nozzle_y = fit.nozzle_y;
    const int32_t nozzle_scale = fit.scale;
    helix::ui::draw_toolhead(layer, nozzle_cx, nozzle_y, data->color_text, nozzle_scale);

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
    helix::ui::queue_invalidate(obj);
}

static void arrow_progress_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    data->arrow_progress = value;

    helix::ui::queue_invalidate(obj);
}

static void arrow_opacity_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    data->arrow_opacity = value;

    helix::ui::queue_invalidate(obj);
}

/// Called when draw-in animation completes; starts the fade-out phase
static void on_draw_in_complete(lv_anim_t* anim) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(anim->var);

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

static void place_value(lv_obj_t* obj, int microns, bool animate);

void ui_z_offset_indicator_set_value(lv_obj_t* obj, int microns) {
    place_value(obj, microns, true);
}

static void place_value(lv_obj_t* obj, int microns, bool animate) {
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

    if (animate && DisplaySettingsManager::instance().get_animations_enabled()) {
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

int helix::ui::z_offset_indicator_value(lv_obj_t* obj) {
    auto* data = obj ? static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj)) : nullptr;
    return data ? data->target_pos / 10 : 0;
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
    lv_obj_set_scrollable(obj, false);

    // Remove default styles and make transparent
    lv_obj_remove_style_all(obj);

    // Allocate and attach widget data
    auto* data = new ZOffsetIndicatorData{};
    data->color_text_muted = theme_manager_get_color("text_muted");
    data->color_text = theme_manager_get_color("text");
    data->color_primary = theme_manager_get_color("primary");
    data->color_text_subtle = theme_manager_get_color("text_subtle");
    data->color_elevated = theme_manager_get_color("elevated_bg");
    data->color_overlay = theme_manager_get_color("overlay_bg");
    data->color_border = theme_manager_get_color("border");
    data->color_warning = theme_manager_get_color("warning");
    data->color_secondary = theme_manager_get_color("secondary");
    lv_obj_set_user_data(obj, data);

    // Register draw and delete callbacks
    // NOTE: lv_obj_add_event_cb() is appropriate here - this is a custom widget, not a UI button
    lv_obj_add_event_cb(obj, indicator_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, indicator_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::trace("[ZOffsetIndicator] Created widget");
    return (void*)obj;
}

// The first notification arrives as the binding is made: the indicator starts
// at the bound value rather than sliding up from zero.
static void microns_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;
    const bool first = !data->bound_value_shown;
    data->bound_value_shown = true;
    place_value(obj, lv_subject_get_int(subject), !first);
}

static void step_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;
    data->step_microns = helix::ztape::step_microns(lv_subject_get_int(subject));
    lv_obj_invalidate(obj);
}

static void estimate_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;
    data->estimate_microns = lv_subject_get_int(subject);
    lv_obj_invalidate(obj);
}

// bind_microns follows an int subject in microns, so an indicator a layout
// <if> rebuilds shows the current offset from its first frame.
static void z_offset_indicator_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    auto* data = static_cast<ZOffsetIndicatorData*>(lv_obj_get_user_data(obj));
    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        if (strcmp(name, "mode") == 0) {
            if (data) {
                data->tape = strcmp(attrs[i + 1], "tape") == 0;
            }
            continue;
        }
        lv_observer_cb_t cb = nullptr;
        if (strcmp(name, "bind_microns") == 0) {
            cb = microns_observer_cb;
        } else if (strcmp(name, "bind_step") == 0) {
            cb = step_observer_cb;
        } else if (strcmp(name, "bind_estimate") == 0) {
            cb = estimate_observer_cb;
        } else {
            continue;
        }
        if (lv_subject_t* subject = lv_xml_get_subject(nullptr, attrs[i + 1])) {
            lv_subject_add_observer_obj(subject, cb, obj, nullptr);
        } else {
            spdlog::warn("[ZOffsetIndicator] Subject '{}' not found for {}", attrs[i + 1], name);
        }
    }
    lv_xml_obj_apply(state, attrs);
}

void ui_z_offset_indicator_register(void) {
    lv_xml_register_widget("z_offset_indicator", z_offset_indicator_xml_create,
                           z_offset_indicator_xml_apply);
    spdlog::trace("[ZOffsetIndicator] Registered <z_offset_indicator> widget");
}
