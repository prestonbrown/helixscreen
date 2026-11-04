// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_tube_stroker.h"

#include "board_identity.h"
#include "memory_utils.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <cmath>

namespace helix {
namespace ui {

bool reduced_effects_for(const board::Info& board, bool constrained_memory) {
    return board.reduced_effects || constrained_memory;
}

// Installed memory does not change at runtime, so only it is cached; the board is
// asked each call so a test's board override is honoured.
bool reduced_effects() {
    static const bool constrained = helix::get_system_memory_info().is_constrained_device();
    return reduced_effects_for(board::current_info(), constrained);
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
        // (~13° between chords).
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
                    // round_joints above).
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

// One layer's passes. The halo bands are opaque, pre-blended against the
// background, so stroke_path round-joins every chord without double-blending.
int build_passes(const LaneStyle& style, TubeLayer layer, TubePass* out, bool simple) {
    switch (layer) {
    case TubeLayer::Halo:
        if (!style.halo || simple)
            return 0;
        out[0] = {tube_blend(style.bg, style.wall, kHaloOuterMix), style.width + kHaloWidthExtra,
                  LV_OPA_COVER};
        out[1] = {tube_blend(style.bg, style.wall, kHaloInnerMix), style.width + kHaloInnerExtra,
                  LV_OPA_COVER};
        return 2;
    case TubeLayer::Wall:
        out[0] = {style.wall, style.width, LV_OPA_COVER};
        return 1;
    case TubeLayer::Bore:
        out[0] = {style.bore, LV_MAX(1, style.width - 2), LV_OPA_COVER};
        return 1;
    }
    return 0;
}

LaneStyle lane_style(bool has_filament, bool active, lv_color_t fill, lv_color_t idle_wall,
                     lv_color_t accent, lv_color_t bg, int32_t gauge) {
    const bool on_route = has_filament && active;
    return {on_route ? accent : idle_wall, has_filament ? fill : bg, bg, gauge, on_route};
}

int lane_passes(const LaneStyle& style, TubePass* out, bool simple) {
    int n = build_passes(style, TubeLayer::Halo, out, simple);
    n += build_passes(style, TubeLayer::Wall, out + n, simple);
    n += build_passes(style, TubeLayer::Bore, out + n, simple);
    return n;
}

lv_color_t tube_accent() {
    lv_color_t primary = theme_manager_get_color("primary");
    return theme_manager_is_dark_mode() ? primary : tube_lighten(primary, 90);
}

} // namespace ui
} // namespace helix
