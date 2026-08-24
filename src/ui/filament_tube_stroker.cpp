// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_tube_stroker.h"

#include "memory_utils.h"
#include "ui/ams_drawing_utils.h"

#include <cmath>

namespace helix {
namespace ui {

// Detect low-performance platforms at compile time or runtime.
// Returns true on K1/K2/MIPS (weak CPUs) or constrained-memory devices.
bool reduced_effects() {
#if defined(HELIX_PLATFORM_K2) || defined(HELIX_PLATFORM_MIPS)
    return true;
#else
    static const bool cached = helix::get_system_memory_info().is_constrained_device();
    return cached;
#endif
}

// Color helpers delegate to ams_draw so there is one definition of the
// darken/lighten/blend math across the AMS drawing code.
lv_color_t tube_darken(lv_color_t c, uint8_t amt) {
    return ams_draw::darken_color(c, amt);
}

lv_color_t tube_lighten(lv_color_t c, uint8_t amt) {
    return ams_draw::lighten_color(c, amt);
}

lv_color_t tube_blend(lv_color_t c1, lv_color_t c2, float factor) {
    return ams_draw::blend_color(c1, c2, factor);
}

// Get a suitable glow color from a filament color.
lv_color_t get_glow_color(lv_color_t color) {
    // If the filament is very dark, use a contrasting blue tint
    int brightness = color.red + color.green + color.blue;
    if (brightness < 120) {
        return lv_color_hex(0x4466AA); // Dark blue glow for black/dark filaments
    }
    return tube_lighten(color, 60);
}

// Stroke a path with N concentric passes.
void stroke_path(lv_layer_t* layer, const pg::FilamentPath& path, const TubePass* passes,
                 int n_passes) {
    if (path.count <= 0)
        return;

    for (int pi = 0; pi < n_passes; pi++) {
        const TubePass& pass = passes[pi];
        if (pass.width <= 0)
            continue;

        // Opaque passes use round caps at EVERY joint: same-color opaque
        // overdraw is invisible, and round ends close the wedge notches that
        // butt caps leave wherever adjacent segments/chords meet at an angle
        // (~13° between chords). Translucent passes (glow) keep butt caps at
        // interior joints — round caps would double-blend at the overlap.
        const bool round_joints = (pass.opa >= LV_OPA_COVER);

        for (int i = 0; i < path.count; i++) {
            const pg::PathSeg& s = path.segs[i];
            bool first = (i == 0);
            bool last = (i == path.count - 1);

            if (s.type == pg::PathSeg::LINE) {
                lv_draw_line_dsc_t dsc;
                lv_draw_line_dsc_init(&dsc);
                dsc.color = pass.color;
                dsc.width = pass.width;
                dsc.opa = pass.opa;
                // Float coords passed directly (LV_USE_FLOAT) — no rounding.
                dsc.p1.x = s.p0.x;
                dsc.p1.y = s.p0.y;
                dsc.p2.x = s.p1.x;
                dsc.p2.y = s.p1.y;
                dsc.round_start = first || round_joints;
                dsc.round_end = last || round_joints;
                lv_draw_line(layer, &dsc);
            } else {
                // ARC: rendered as a fan of short straight chords at FLOAT
                // precision (lv_draw_line), NOT lv_draw_arc.
                //
                // Why not lv_draw_arc: it takes an INTEGER center (lv_point_t)
                // and an INTEGER OUTER radius (uint16_t), while lines draw at
                // float precision. For odd pass widths the true outer edge is at
                // a half-pixel (centerline_r + w/2 = x.5) and rounds, shifting the
                // arc band ~0.5px radially versus the adjoining line band —
                // visible misalignment at every fillet joint. Chords inherit the
                // exact float arc parametrization, so line and arc bands stay
                // flush.
                //
                // Angle convention (unchanged from pathgeo and the geometry
                // module): angle 0 = +x, positive angle rotates toward +y (screen
                // down), positive sweep = CLOCKWISE on screen. A point at
                // parameter t in [0, |sweep|] is
                //   angle = start_angle + sign(sweep) * t
                //   (cx + r*cos angle, cy + r*sin angle).
                const float r = s.radius;
                const float sweep = s.sweep;
                const float a0 = s.start_angle;
                const float abs_sweep = std::fabs(sweep);
                const float sgn = (sweep >= 0.0f) ? 1.0f : -1.0f;

                // Subdivide so the chord sagitta error stays < ~0.1px:
                //   sagitta = r * (1 - cos(half_step)) < 0.1
                //   => half_step < acos(1 - 0.1/r), step < 2*half_step
                //   => n = ceil(|sweep| / step). Clamp n to [4, 16].
                int n_chords = 4;
                if (r > 0.0f) {
                    float arg = 1.0f - 0.1f / r;
                    if (arg < -1.0f)
                        arg = -1.0f; // tiny r: just clamp to the floor below
                    float step = 2.0f * std::acos(arg);
                    if (step > 1e-6f)
                        n_chords = (int)std::ceil(abs_sweep / step);
                }
                if (n_chords < 4)
                    n_chords = 4;
                if (n_chords > 16)
                    n_chords = 16;

                const float dt = abs_sweep / (float)n_chords;
                float prev_a = a0;
                float prev_x = s.center.x + r * std::cos(prev_a);
                float prev_y = s.center.y + r * std::sin(prev_a);

                for (int c = 0; c < n_chords; c++) {
                    float cur_a = a0 + sgn * dt * (float)(c + 1);
                    float cur_x = s.center.x + r * std::cos(cur_a);
                    float cur_y = s.center.y + r * std::sin(cur_a);

                    lv_draw_line_dsc_t dsc;
                    lv_draw_line_dsc_init(&dsc);
                    dsc.color = pass.color;
                    dsc.width = pass.width;
                    dsc.opa = pass.opa;
                    dsc.p1.x = prev_x;
                    dsc.p1.y = prev_y;
                    dsc.p2.x = cur_x;
                    dsc.p2.y = cur_y;
                    // Opaque passes: round caps at every chord joint (see
                    // round_joints above). Translucent passes: butt interior
                    // joints, terminal round caps only at true path ends.
                    dsc.round_start = (first && (c == 0)) || round_joints;
                    dsc.round_end = (last && (c == n_chords - 1)) || round_joints;
                    lv_draw_line(layer, &dsc);

                    prev_a = cur_a;
                    prev_x = cur_x;
                    prev_y = cur_y;
                }
            }
        }
    }
}

// Build the concentric pass list for a LaneStyle. Keeps the darken/lighten
// numbers consistent with the legacy tube look. Returns the number of passes
// written into `out` (caller sizes out for at least 4).
int build_passes(const LaneStyle& style, TubePass* out) {
    const bool simple = reduced_effects();
    int n = 0;
    if (style.solid) {
        // Solid (loaded) lanes read the SAME outer gauge as the idle hollow
        // tubes: the body is drawn at style.width with NO darker +2 outline
        // pass. The hollow tube's visible wall spans width-2..width+2 (≈ width
        // gauge), so dropping the outline here matches the two. The "loaded"
        // emphasis is carried by the fill color, the bright centered core, and
        // the wide glow backdrop — not by bulk.
        if (style.glow && !simple) {
            out[n++] = {get_glow_color(style.color), style.width + GLOW_WIDTH_EXTRA, GLOW_OPA};
        }
        // Body (always).
        out[n++] = {style.color, style.width, LV_OPA_COVER};
        if (!simple) {
            // Core highlight (lighter, narrower) — concentric, no offset.
            out[n++] = {tube_lighten(style.color, 44), LV_MAX(1, style.width / 2), LV_OPA_COVER};
        }
    } else {
        if (!simple) {
            out[n++] = {tube_darken(style.color, 25), style.width + 2, LV_OPA_COVER};
        }
        // Wall.
        out[n++] = {style.color, style.width, LV_OPA_COVER};
        // Bore (background show-through).
        out[n++] = {style.bg, LV_MAX(1, style.width - 2), LV_OPA_COVER};
    }
    return n;
}

// Build a LaneStyle from slot state in ONE place.
LaneStyle lane_style(bool has_filament, lv_color_t tool_color, lv_color_t idle_color, lv_color_t bg,
                     int32_t active_w) {
    LaneStyle st{};
    st.solid = has_filament;
    st.color = has_filament ? tool_color : idle_color;
    st.bg = bg;
    st.width = active_w;
    st.glow = has_filament; // active lanes get the glow backdrop
    return st;
}

// Draw a path with a style; optionally record (append) the path's segments into
// the active-path cache so the flow-dot / tip animation walks exactly what was
// drawn. Geometry is computed once.
void draw_lane(lv_layer_t* layer, const pg::FilamentPath& path, const LaneStyle& style,
               pg::FilamentPath* record) {
    TubePass passes[4];
    int n = build_passes(style, passes);
    stroke_path(layer, path, passes, n);
    if (record) {
        for (int i = 0; i < path.count && record->count < pg::FilamentPath::MAX_SEGS; i++) {
            record->segs[record->count++] = path.segs[i];
        }
    }
}

// Convenience: vertical tube run from (x,y0) to (x,y1).
void draw_lane_vline(lv_layer_t* layer, int32_t x, int32_t y0, int32_t y1, const LaneStyle& style,
                     pg::FilamentPath* record) {
    pg::FilamentPath path;
    path.add_line((float)x, (float)y0, (float)x, (float)y1);
    TubePass passes[4];
    int n = build_passes(style, passes);
    stroke_path(layer, path, passes, n);
    // Preserve the old guard: only record forward (downward) verticals.
    if (record && y1 > y0) {
        for (int i = 0; i < path.count && record->count < pg::FilamentPath::MAX_SEGS; i++)
            record->segs[record->count++] = path.segs[i];
    }
}

// Convenience: orthogonal route (vertical -> fillet -> horizontal -> fillet ->
// vertical) from (x0,y0) down to (x1,y1).
void draw_lane_route(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     float fillet_r, const LaneStyle& style, pg::FilamentPath* record) {
    pg::FilamentPath path;
    pg::route_orthogonal(path, (float)x0, (float)y0, (float)x1, (float)y1, fillet_r);
    draw_lane(layer, path, style, record);
}

// Convenience: horizontal tube run from (x0,y) to (x1,y) (bypass horizontal).
void draw_lane_hline(lv_layer_t* layer, int32_t x0, int32_t x1, int32_t y, const LaneStyle& style,
                     pg::FilamentPath* record) {
    pg::FilamentPath path;
    path.add_line((float)x0, (float)y, (float)x1, (float)y);
    draw_lane(layer, path, style, record);
}

void draw_merge_fan(lv_layer_t* layer, const MergeFanLane* lanes, int n, int32_t hub_cx,
                    int32_t hub_top, int32_t hub_w, float fillet_r, int32_t* entry_x_out) {
    if (!lanes || n <= 0)
        return;

    // Inset the outermost entries ~8px from each hub-top end (per the design),
    // and cap the per-side slope at 1.2 so tall hubs don't run near-vertical.
    constexpr float ENTRY_MARGIN = 8.0f;
    constexpr float MAX_SLOPE = 1.2f;

    pg::MergeLaneIn in[pg::FilamentPath::MAX_SEGS];
    pg::MergeLaneOut fan[pg::FilamentPath::MAX_SEGS];
    int count = (n < pg::FilamentPath::MAX_SEGS) ? n : pg::FilamentPath::MAX_SEGS;
    for (int i = 0; i < count; ++i)
        in[i] = {(float)lanes[i].slot_x, (float)lanes[i].start_y};

    pg::build_merge_fan(in, count, (float)hub_cx, (float)hub_top, (float)hub_w, ENTRY_MARGIN,
                        fillet_r, MAX_SLOPE, fan);

    for (int i = 0; i < count; ++i) {
        if (entry_x_out)
            entry_x_out[i] = (int32_t)lroundf(fan[i].pts[2].x);
        pg::FilamentPath path;
        pg::route_polyline_filleted(path, fan[i].pts, 4, fillet_r);
        draw_lane(layer, path, lanes[i].style, lanes[i].record);
    }
}

} // namespace ui
} // namespace helix
