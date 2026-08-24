// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Topology renderers for the filament_path_canvas widget. Three layouts:
//
//   LINEAR/HUB (render_linear_hub) — the classic AMS detail view. Phases run
//   top-to-bottom along the physical filament path: entry lanes → prep
//   sensors → merge (fan into HUB, or butt into the LINEAR selector) →
//   hub/selector box → output sensor → buffer → bypass merge → toolhead
//   sensor → nozzle.
//
//   PARALLEL (render_parallel) — tool changers: every slot is an independent
//   column (entry → sensor → own toolhead + badge).
//
//   MIXED (render_mixed) — HTLF-style: some lanes run direct to their own
//   nozzle, others fan into a shared hub feeding one nozzle.
//
// Each renderer derives a per-draw "frame" (layout Ys, resolved colors,
// per-slot states) once, then named phase functions consume it through the
// shared RenderCtx. While drawing, the active lane records its centerline
// into FilamentPathData::path_cache and the hub/buffer/bypass boxes record
// their hit rects — see ui_filament_path_internal.h ("render → record").
//
// The DRAW_POST animation overlays (flow dots, heat glow, moving tip) live at
// the bottom of this file; they replay the recorded path each frame without
// re-running the heavyweight render.

#include "ui_filament_path_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace helix::ui::fpath {

// ============================================================================
// Shared per-draw derivations
// ============================================================================

BaseGeometry compute_base_geometry(lv_obj_t* obj, const FilamentPathData* data) {
    BaseGeometry g;
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    g.x_off = obj_coords.x1;
    g.y_off = obj_coords.y1;
    g.width = lv_area_get_width(&obj_coords);
    g.height = lv_area_get_height(&obj_coords);
    g.slot_count = data->slot_count;

    int count = LV_MIN(g.slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < count; i++) {
        g.slot_x[i] = g.x_off + get_slot_x(data, i, g.x_off);
    }

    // Center X: prefer midpoint of slot bounds so hub/selector/nozzle stay
    // aligned with the spool grid even when the grid is narrower than the
    // canvas (e.g. environment indicator present).
    if (g.slot_count >= 2) {
        g.center_x = (g.slot_x[0] + g.slot_x[g.slot_count - 1]) / 2;
    } else if (g.slot_count == 1) {
        g.center_x = g.slot_x[0];
    } else {
        g.center_x = g.x_off + g.width / 2;
    }
    return g;
}

SlotRenderStates compute_slot_render_states(const FilamentPathData* data) {
    SlotRenderStates states{};
    int count = LV_MIN(data->slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < count; i++) {
        SlotRenderState& s = states[i];

        // Per-slot installed filament (default).
        if (data->slot_filament_states[i].segment != PathSegment::NONE) {
            s.has_filament = true;
            s.color = lv_color_hex(data->slot_filament_states[i].color);
            s.segment = data->slot_filament_states[i].segment;
        }

        // Active slot overrides with current load/unload state when present.
        s.is_mounted = (i == data->active_slot);
        if (s.is_mounted && data->filament_segment > 0) {
            s.has_filament = true;
            s.color = lv_color_hex(data->filament_color);
            s.segment = static_cast<PathSegment>(data->filament_segment);
        }

        s.at_sensor = s.has_filament && (s.segment >= PathSegment::TOOLHEAD);
        s.at_nozzle = s.has_filament && (s.segment >= PathSegment::NOZZLE);
    }
    return states;
}

// Check if a segment should be drawn as "active" (filament present at or past it)
bool is_segment_active(PathSegment segment, PathSegment filament_segment) {
    return static_cast<int>(segment) <= static_cast<int>(filament_segment) &&
           filament_segment != PathSegment::NONE;
}

namespace {

// ============================================================================
// PARALLEL topology (tool changers)
// ============================================================================
// Tool changers have independent toolheads — each slot is a complete tool with
// its own extruder. Unlike hub/linear topologies where filaments converge to a
// single toolhead, parallel topology shows separate per-slot paths.

// One independent tool column: entry line → sensor dot → line → toolhead glyph
// → tool badge.
void draw_parallel_slot(const RenderCtx& ctx, const SlotRenderStates& states, int i,
                        int32_t entry_y, int32_t sensor_y, int32_t toolhead_y) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    lv_color_t idle_color = theme.color_idle;
    lv_color_t bg_color = theme.color_bg;
    int32_t line_active = theme.line_width_active;
    int32_t sensor_r = theme.sensor_radius;

    int32_t slot_x = ctx.geo.slot_x[i];
    const SlotRenderState& s = states[i];
    lv_color_t tool_color = s.has_filament ? s.color : idle_color;

    int32_t tool_scale = LV_MAX(6, theme.extruder_scale * 2 / 3);
    int32_t nozzle_top = toolhead_y - tool_scale * 2; // Top of heater block

    // Entry → sensor line: colored if filament present, hollow if idle
    {
        LaneStyle st = lane_style(s.has_filament, tool_color, idle_color, bg_color, line_active);
        draw_lane_vline(ctx.layer, slot_x, entry_y, sensor_y - sensor_r, st);
    }

    // Toolhead entry sensor dot
    lv_color_t sensor_color = s.at_sensor ? tool_color : idle_color;
    draw_sensor_dot(ctx.layer, slot_x, sensor_y, sensor_color, s.at_sensor, sensor_r);

    // Sensor → nozzle line: colored if filament reaches nozzle, hollow if idle
    {
        LaneStyle st = lane_style(s.at_nozzle, tool_color, idle_color, bg_color, line_active);
        draw_lane_vline(ctx.layer, slot_x, sensor_y + sensor_r, nozzle_top, st);
    }

    // Nozzle color — only show filament color when actually at nozzle
    lv_color_t noz_color = s.is_mounted ? theme.color_nozzle : ph_darken(theme.color_nozzle, 60);
    if (s.at_nozzle) {
        noz_color = tool_color;
    }

    // Docked toolheads rendered at reduced opacity to visually distinguish from active
    lv_opa_t toolhead_opa = s.is_mounted ? LV_OPA_COVER : LV_OPA_40;

    // Flow particles for the active slot are painted separately in
    // draw_animation_parallel (DRAW_POST) so per-frame ticks don't bust the
    // overlay canvas cache.
    draw_toolhead(ctx.layer, slot_x, toolhead_y, noz_color, tool_scale, toolhead_opa);

    // Tool badge (E0/T0, …) below nozzle — matches system_path_canvas style
    if (theme.label_font) {
        char tool_label[16];
        int tool = (data->mapped_tool[i] >= 0) ? data->mapped_tool[i] : i;
        format_tool_badge_label(data, i, tool, tool_label, sizeof(tool_label));
        lv_color_t text = s.is_mounted ? theme.color_success : theme.color_text;
        draw_tool_badge(ctx, slot_x, toolhead_y + tool_scale * 4 + 6, tool_label, text,
                        toolhead_opa);
    }
}

// Static + state-tied content for PARALLEL — painted into the overlay canvas.
// Animation (flow dots) lives separately in draw_animation_parallel (DRAW_POST).
void render_parallel(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};
    int32_t height = ctx.geo.height;
    int32_t y_off = ctx.geo.y_off;

    // Layout ratios for parallel topology (adjusted for per-slot toolheads).
    // SENSOR_Y/TOOLHEAD_Y are file-scope (PARALLEL_*_Y_RATIO) so the click
    // hit-test reads the identical values — no drift between draw and hit.
    constexpr float ENTRY_Y = -0.12f; // Top entry (connects to spool)

    int32_t entry_y = y_off + (int32_t)(height * ENTRY_Y);
    int32_t sensor_y = y_off + (int32_t)(height * PARALLEL_SENSOR_Y_RATIO);
    int32_t toolhead_y = y_off + (int32_t)(height * PARALLEL_TOOLHEAD_Y_RATIO);

    SlotRenderStates states = compute_slot_render_states(data);

    // Draw each tool as an independent column
    for (int i = 0; i < data->slot_count; i++) {
        draw_parallel_slot(ctx, states, i, entry_y, sensor_y, toolhead_y);
    }
}

// ============================================================================
// MIXED topology (HTLF: direct + hub lanes)
// ============================================================================
// Some lanes go directly to their own nozzle (like PARALLEL), while others
// converge through a hub box to a shared nozzle. Visual layout:
//   [spool0] [spool1] [spool2] [spool3]
//      |        |        |        |       entry lines
//      o        o        o        o       sensor dots
//      |        |         \      /        direct vs angled paths
//      |        |        [HUB]            hub box (hub lanes converge)
//      |        |          |              hub output line
//     (T0)    (T2)       (T1)             nozzles + tool labels

// Per-draw layout + merge-fan plan for MIXED.
struct MixedFrame {
    int32_t entry_y = 0;
    int32_t sensor_y = 0;
    int32_t hub_cy = 0;
    int32_t hub_h = 0;
    int32_t hub_bottom = 0;
    int32_t toolhead_y = 0;
    int32_t tool_scale = 0;

    int hub_count = 0;       // lanes routed through the hub
    int first_hub_lane = -1; // first hub-routed slot index
    int32_t hub_cx = 0;      // hub box center X (mean of hub lane Xs)
    int32_t hub_w = 0;

    // Per-hub-lane merge fan via the shared builder: parallel diagonals per
    // side spread across distinct hub-top entries (no coincident or pinching
    // runs). Built once; the path phase draws each lane's tube from its
    // precomputed waypoints.
    pg::MergeLaneOut hub_fan[FilamentPathData::MAX_SLOTS];
    int slot_to_fan[FilamentPathData::MAX_SLOTS]; // slot index -> hub-lane order (-1 none)

    SlotRenderStates states;
};

// Layout + hub-lane identification + merge-fan construction.
MixedFrame compute_mixed_frame(const RenderCtx& ctx) {
    const FilamentPathData* data = ctx.data;
    const BaseGeometry& g = ctx.geo;
    MixedFrame f;

    // Layout ratios — more vertical spread than parallel to fit hub + nozzles
    constexpr float ENTRY_Y = -0.12f;   // Top entry (connects to spool)
    constexpr float SENSOR_Y = 0.15f;   // Sensor dot position
    constexpr float HUB_Y = 0.32f;      // Hub box center Y
    constexpr float HUB_H = 0.08f;      // Hub box height ratio
    constexpr float TOOLHEAD_Y = 0.62f; // Nozzle/toolhead position

    f.entry_y = g.y_off + (int32_t)(g.height * ENTRY_Y);
    f.sensor_y = g.y_off + (int32_t)(g.height * SENSOR_Y);
    f.hub_cy = g.y_off + (int32_t)(g.height * HUB_Y);
    f.hub_h = LV_MAX(16, (int32_t)(g.height * HUB_H));
    f.hub_bottom = f.hub_cy + f.hub_h / 2;
    f.toolhead_y = g.y_off + (int32_t)(g.height * TOOLHEAD_Y);
    f.tool_scale = LV_MAX(6, data->theme.extruder_scale * 2 / 3);

    f.states = compute_slot_render_states(data);

    // Identify hub lanes and compute hub center X
    int32_t hub_x_sum = 0;
    for (int i = 0; i < data->slot_count; i++) {
        if (data->slot_is_hub_routed[i]) {
            hub_x_sum += g.slot_x[i];
            f.hub_count++;
            if (f.first_hub_lane < 0)
                f.first_hub_lane = i;
        }
    }
    f.hub_cx = (f.hub_count > 0) ? (hub_x_sum / f.hub_count) : (g.x_off + 150);
    // Hub width: ~60% of full hub topology width, enough for the hub lanes
    f.hub_w = LV_MAX(40, data->theme.hub_width * 3 / 5);

    // Build the merge fan for the hub lanes.
    for (int i = 0; i < FilamentPathData::MAX_SLOTS; i++)
        f.slot_to_fan[i] = -1;
    {
        int32_t hub_top_e = f.hub_cy - f.hub_h / 2;
        int32_t sensor_r = data->theme.sensor_radius;
        pg::MergeLaneIn fan_in[FilamentPathData::MAX_SLOTS];
        int fan_n = 0;
        for (int i = 0; i < data->slot_count && i < FilamentPathData::MAX_SLOTS; i++) {
            if (!data->slot_is_hub_routed[i])
                continue;
            fan_in[fan_n] = {(float)g.slot_x[i], (float)(f.sensor_y + sensor_r)};
            f.slot_to_fan[i] = fan_n;
            fan_n++;
        }
        pg::build_merge_fan(fan_in, fan_n, (float)f.hub_cx, (float)hub_top_e, (float)f.hub_w,
                            /*entry_margin=*/8.0f, /*fillet_r=*/8.0f, /*max_slope=*/1.2f,
                            f.hub_fan);
    }
    return f;
}

// Entry lines and sensor dots for ALL lanes (direct and hub-routed alike).
void draw_mixed_entry_lanes(const RenderCtx& ctx, const MixedFrame& f) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    int32_t sensor_r = theme.sensor_radius;

    for (int i = 0; i < data->slot_count; i++) {
        int32_t slot_x = ctx.geo.slot_x[i];
        const SlotRenderState& s = f.states[i];
        lv_color_t tool_color = s.has_filament ? s.color : theme.color_idle;

        // Entry → sensor line
        {
            LaneStyle st = lane_style(s.has_filament, tool_color, theme.color_idle, theme.color_bg,
                                      theme.line_width_active);
            draw_lane_vline(ctx.layer, slot_x, f.entry_y, f.sensor_y - sensor_r, st);
        }

        // Sensor dot
        lv_color_t sensor_color = s.at_sensor ? tool_color : theme.color_idle;
        draw_sensor_dot(ctx.layer, slot_x, f.sensor_y, sensor_color, s.at_sensor, sensor_r);
    }
}

// Shared hub output: hub-bottom → nozzle line, the single shared toolhead, and
// its tool badge. Drawn once, right after the FIRST hub lane's tube (so later
// hub-lane tubes still paint on top in their original z-order).
void draw_mixed_shared_toolhead(const RenderCtx& ctx, const MixedFrame& f) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;

    // Check if any hub lane has filament at nozzle
    bool any_hub_at_nozzle = false;
    lv_color_t hub_nozzle_color = theme.color_nozzle;
    int hub_tool = (f.first_hub_lane >= 0 && data->mapped_tool[f.first_hub_lane] >= 0)
                       ? data->mapped_tool[f.first_hub_lane]
                       : (f.first_hub_lane >= 0 ? f.first_hub_lane : 0);
    // Lane whose extruder identity names this shared toolhead. Every hub-routed
    // lane feeds the same extruder, so any of them answers; the loaded one is
    // preferred only because the legacy T-label follows it.
    int hub_badge_lane = f.first_hub_lane;

    for (int j = 0; j < data->slot_count; j++) {
        if (!data->slot_is_hub_routed[j])
            continue;
        const SlotRenderState& sj = f.states[j];
        if (sj.segment >= PathSegment::NOZZLE) {
            any_hub_at_nozzle = true;
            hub_nozzle_color = sj.color;
            hub_tool = (data->mapped_tool[j] >= 0) ? data->mapped_tool[j] : j;
            hub_badge_lane = j;
            break;
        }
    }

    int32_t nozzle_top = f.toolhead_y - f.tool_scale * 2;

    // Hub bottom → nozzle top line
    {
        LaneStyle st = lane_style(any_hub_at_nozzle, hub_nozzle_color, theme.color_idle,
                                  theme.color_bg, theme.line_width_active);
        draw_lane_vline(ctx.layer, f.hub_cx, f.hub_bottom, nozzle_top, st);
    }

    // Shared hub nozzle — always "mounted" visually (it's a shared output)
    lv_color_t noz_color = any_hub_at_nozzle ? hub_nozzle_color : theme.color_nozzle;
    lv_opa_t hub_noz_opa = LV_OPA_COVER;
    draw_toolhead(ctx.layer, f.hub_cx, f.toolhead_y, noz_color, f.tool_scale, hub_noz_opa);

    // Tool label below shared hub nozzle
    if (theme.label_font) {
        char tool_label[16];
        format_tool_badge_label(data, hub_badge_lane, hub_tool, tool_label, sizeof(tool_label));
        draw_tool_badge(ctx, f.hub_cx, f.toolhead_y + f.tool_scale * 4 + 6, tool_label,
                        theme.color_text, hub_noz_opa);
    }
}

// Direct lane: straight vertical from sensor to its own nozzle + badge.
void draw_mixed_direct_lane(const RenderCtx& ctx, const MixedFrame& f, int i) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    const SlotRenderState& s = f.states[i];
    int32_t slot_x = ctx.geo.slot_x[i];
    lv_color_t tool_color = s.has_filament ? s.color : theme.color_idle;
    int32_t nozzle_top = f.toolhead_y - f.tool_scale * 2;

    {
        LaneStyle st = lane_style(s.has_filament && s.at_nozzle, tool_color, theme.color_idle,
                                  theme.color_bg, theme.line_width_active);
        draw_lane_vline(ctx.layer, slot_x, f.sensor_y + theme.sensor_radius, nozzle_top, st);
    }

    // Direct nozzle
    lv_color_t noz_color = s.is_mounted ? theme.color_nozzle : ph_darken(theme.color_nozzle, 60);
    if (s.at_nozzle) {
        noz_color = tool_color;
    }
    lv_opa_t toolhead_opa = s.is_mounted ? LV_OPA_COVER : LV_OPA_40;
    draw_toolhead(ctx.layer, slot_x, f.toolhead_y, noz_color, f.tool_scale, toolhead_opa);

    // Tool label below direct nozzle
    if (theme.label_font) {
        char tool_label[16];
        int tool = (data->mapped_tool[i] >= 0) ? data->mapped_tool[i] : i;
        format_tool_badge_label(data, i, tool, tool_label, sizeof(tool_label));
        lv_color_t text = s.is_mounted ? theme.color_success : theme.color_text;
        draw_tool_badge(ctx, slot_x, f.toolhead_y + f.tool_scale * 3 + 4, tool_label, text,
                        toolhead_opa);
    }
}

// Paths from sensor to nozzle (direct or hub-routed), preserving z-order:
// each lane in slot order; the shared hub toolhead immediately after the
// first hub lane's tube.
void draw_mixed_paths(const RenderCtx& ctx, const MixedFrame& f) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    bool hub_nozzle_drawn = false;

    for (int i = 0; i < data->slot_count; i++) {
        if (data->slot_is_hub_routed[i]) {
            // Hub-routed lane: parallel-diagonal merge run from the sensor down
            // to a distinct hub-top entry, drawn from this lane's precomputed
            // fan waypoints (separation by construction — see build_merge_fan).
            const SlotRenderState& s = f.states[i];
            lv_color_t tool_color = s.has_filament ? s.color : theme.color_idle;
            int fi = (i < FilamentPathData::MAX_SLOTS) ? f.slot_to_fan[i] : -1;
            if (fi >= 0) {
                LaneStyle st = lane_style(s.has_filament, tool_color, theme.color_idle,
                                          theme.color_bg, theme.line_width_active);
                pg::FilamentPath path;
                pg::route_polyline_filleted(path, f.hub_fan[fi].pts, 4, 8.0f);
                draw_lane(ctx.layer, path, st, nullptr);
            }

            // Hub output line + shared nozzle (draw only once)
            if (!hub_nozzle_drawn) {
                hub_nozzle_drawn = true;
                draw_mixed_shared_toolhead(ctx, f);
            }
        } else {
            draw_mixed_direct_lane(ctx, f, i);
        }
    }
}

void render_mixed(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};
    MixedFrame f = compute_mixed_frame(ctx);

    // Phase 1: entry lines and sensor dots for ALL lanes
    draw_mixed_entry_lanes(ctx, f);

    // Phase 2: hub box (behind paths, so paths draw on top)
    if (f.hub_count > 0) {
        draw_hub_box(ctx, f.hub_cx, f.hub_cy, f.hub_w, f.hub_h, data->theme.color_hub_bg,
                     data->theme.color_hub_border, "HUB");
    }

    // Phase 3: paths from sensor to nozzle (direct or hub-routed)
    draw_mixed_paths(ctx, f);
}

// ============================================================================
// LINEAR / HUB topology
// ============================================================================

// Everything the LINEAR/HUB phases share for one draw: layout Ys, resolved
// colors, filament/error state, the HUB merge-fan plan, and the active-path
// recording that feeds the DRAW_POST animation pass.
struct LinearHubFrame {
    // Vertical layout (absolute display coords)
    int32_t entry_y = 0;
    int32_t prep_y = 0;
    int32_t merge_y = 0;
    int32_t hub_y = 0;
    int32_t hub_h = 0;
    int32_t output_y = 0;
    int32_t toolhead_y = 0;
    int32_t nozzle_y = 0;
    int32_t bypass_merge_y = 0;
    int32_t center_x = 0;
    int32_t output_x = 0; // LINEAR: under the active slot (possibly animating)

    // Buffer geometry — shared by the output phase and flow-dot path recording
    int32_t buffer_y = 0;
    int32_t buf_fil_top = 0;
    int32_t buf_fil_bot = 0;
    bool has_buffer = false;

    // Resolved colors
    lv_color_t idle_color, bg_color, active_color, hub_bg, hub_border, nozzle_color;
    lv_color_t error_color; // error token blended with the pulse phase

    // Sizes
    int32_t line_active = 0;
    int32_t sensor_r = 0;

    // Filament / error state
    bool has_error = false;
    PathSegment error_seg = PathSegment::NONE;
    PathSegment fil_seg = PathSegment::NONE;

    // HUB topology merge fan (parallel diagonals per side; built once, drawn
    // per slot so active-path record order is preserved)
    pg::MergeLaneOut hub_fan[FilamentPathData::MAX_SLOTS];
    int32_t hub_dot_xs[FilamentPathData::MAX_SLOTS] = {};
    int32_t hub_box_w = 0; // widened entry-spread width (HUB box drawn at this)

    // Per-slot derived state + the recorded active filament path
    SlotRenderStates states;
    pg::FilamentPath active_path;
};

// Layout, colors, and state resolution. Mirrors the layout ratios at the top
// of ui_filament_path_internal.h; LINEAR butts the selector against the prep
// sensors and slides the output exit under the active slot.
LinearHubFrame compute_linear_hub_frame(const RenderCtx& ctx) {
    FilamentPathData* data = ctx.data;
    const BaseGeometry& g = ctx.geo;
    LinearHubFrame f;

    f.entry_y = g.y_off + (int32_t)(g.height * ENTRY_Y_RATIO);
    f.prep_y = g.y_off + (int32_t)(g.height * PREP_Y_RATIO);
    f.merge_y = g.y_off + (int32_t)(g.height * MERGE_Y_RATIO);
    f.hub_y = g.y_off + (int32_t)(g.height * HUB_Y_RATIO);
    f.hub_h = (int32_t)(g.height * HUB_HEIGHT_RATIO);
    // Output sensor butted directly against hub bottom (mirrors input sensors at hub top)
    f.output_y = f.hub_y + f.hub_h / 2;
    f.toolhead_y = g.y_off + (int32_t)(g.height * TOOLHEAD_Y_RATIO);
    f.nozzle_y = g.y_off + (int32_t)(g.height * NOZZLE_Y_RATIO);
    f.bypass_merge_y = g.y_off + (int32_t)(g.height * BYPASS_MERGE_Y_RATIO);
    f.center_x = g.center_x;

    // Buffer geometry — shared by drawing and flow dot paths
    f.buffer_y = g.y_off + (int32_t)(g.height * BUFFER_Y_RATIO);
    int32_t buf_box_h = f.hub_h;
    if (buf_box_h < 16)
        buf_box_h = 16;
    int32_t buf_extend = buf_box_h / 2;
    f.buf_fil_top = f.buffer_y - buf_box_h / 2 - buf_extend;
    f.buf_fil_bot = f.buffer_y + buf_box_h / 2 + buf_extend;
    f.has_buffer = data->buffer_present;

    // Colors from theme
    f.idle_color = data->theme.color_idle;
    f.bg_color = data->theme.color_bg;
    f.active_color = lv_color_hex(data->filament_color);
    f.hub_bg = data->theme.color_hub_bg;
    f.hub_border = data->theme.color_hub_border;
    f.nozzle_color = data->theme.color_nozzle;

    // Error color with pulse effect - blend toward idle based on opacity
    f.error_color = data->theme.color_error;
    if (data->anim.error_pulse_active && data->anim.error_pulse_opa < LV_OPA_COVER) {
        // Blend error color with a darker version for pulsing effect
        float blend_factor = (float)(LV_OPA_COVER - data->anim.error_pulse_opa) /
                             (float)(LV_OPA_COVER - ERROR_PULSE_OPA_MIN);
        f.error_color =
            ph_blend(data->theme.color_error, ph_darken(data->theme.color_error, 80), blend_factor);
    }

    // Sizes from theme
    f.line_active = data->theme.line_width_active;
    f.sensor_r = data->theme.sensor_radius;

    // LINEAR topology: butt SELECTOR directly against prep sensors (no gap/lines between)
    if (data->topology == 0) {
        f.hub_y = f.prep_y + f.sensor_r + f.hub_h / 2;
        f.output_y = f.hub_y + f.hub_h / 2;
    }

    // LINEAR: output exits beneath the active slot, not center
    f.output_x = f.center_x; // default for HUB
    if (data->topology == 0 && data->active_slot >= 0) {
        int32_t target_x = g.slot_x[data->active_slot];
        // Use animated position if available, otherwise snap
        if (data->anim.output_x_active) {
            f.output_x = data->anim.output_x_current;
        } else {
            f.output_x = target_x;
            data->anim.output_x_current = target_x;
            data->anim.output_x_target = target_x;
        }
    }

    f.has_error = data->error_segment > 0;
    f.error_seg = static_cast<PathSegment>(data->error_segment);
    f.fil_seg = static_cast<PathSegment>(data->filament_segment);

    f.states = compute_slot_render_states(data);
    return f;
}

// Debug override: HELIX_FLOW_SEGMENT=PREP|LANE|HUB|OUTPUT|TOOLHEAD|NOZZLE
//                 HELIX_FLOW_DIR=LOAD|UNLOAD
// Forces filament to the specified segment with flow animation for isolated
// testing, and keeps a 30ms repaint timer alive while active.
void apply_debug_flow_override(lv_obj_t* obj, const RenderCtx& ctx, LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    static const char* dbg_seg_env = getenv("HELIX_FLOW_SEGMENT");
    if (!dbg_seg_env)
        return;

    // Force active slot 0 if none set (local override only for drawing)
    if (data->active_slot < 0)
        data->active_slot = 0;

    // Map name to PathSegment value
    static const struct {
        const char* name;
        int val;
    } seg_map[] = {
        {"PREP", (int)PathSegment::PREP},         {"LANE", (int)PathSegment::LANE},
        {"HUB", (int)PathSegment::HUB},           {"OUTPUT", (int)PathSegment::OUTPUT},
        {"TOOLHEAD", (int)PathSegment::TOOLHEAD}, {"NOZZLE", (int)PathSegment::NOZZLE},
    };
    for (auto& m : seg_map) {
        if (strcasecmp(dbg_seg_env, m.name) == 0) {
            data->filament_segment = m.val;
            f.fil_seg = static_cast<PathSegment>(m.val);
            break;
        }
    }

    // Use a persistent lv_timer to keep triggering redraws
    static lv_timer_t* dbg_timer = nullptr;
    if (!dbg_timer) {
        dbg_timer = lv_timer_create(
            [](lv_timer_t* t) {
                auto* o = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
                lv_obj_invalidate(o);
            },
            30, obj);
    }
}

// HUB topology: pre-compute the merge fan via the shared builder (parallel
// diagonals per side — no overlaps or pinches by construction). Each lane's
// 4-point polyline and hub-top entry x are derived once; the lane phase draws
// each lane's tube (preserving active-path record order) and lands the hub
// sensor dot exactly on its tube.
void build_linear_hub_merge_fan(const RenderCtx& ctx, LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    const BaseGeometry& g = ctx.geo;

    // Width across which the hub-top entries spread. The nominal hub_width is
    // too narrow for many lanes (entries cluster -> tubes pinch near the
    // center), so widen the entry span toward the slot row, targeting ~22px
    // between entries (the design's separation budget). Clamped to the slot
    // span so the outermost entries never exceed the outermost slots. The hub
    // box is drawn at this same width so tubes visibly land on it.
    f.hub_box_w = data->theme.hub_width;
    if (data->topology != 1)
        return;

    int32_t hub_top = f.hub_y - f.hub_h / 2;
    pg::MergeLaneIn fan_in[FilamentPathData::MAX_SLOTS];
    int fan_n = LV_MIN(data->slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < fan_n; i++) {
        int32_t start_y = data->slot_has_prep_sensor[i] ? (f.prep_y + f.sensor_r) : f.prep_y;
        fan_in[i] = {(float)g.slot_x[i], (float)start_y};
    }
    constexpr int32_t TARGET_ENTRY_SPACING = 22;
    constexpr int32_t ENTRY_MARGIN = 8;
    int32_t want_w =
        (fan_n > 1) ? (fan_n - 1) * TARGET_ENTRY_SPACING + 2 * ENTRY_MARGIN : data->theme.hub_width;
    int32_t slot_span = (data->slot_count > 1) ? (g.slot_x[data->slot_count - 1] - g.slot_x[0])
                                               : data->theme.hub_width;
    f.hub_box_w = LV_CLAMP(want_w, data->theme.hub_width, LV_MAX(data->theme.hub_width, slot_span));
    pg::build_merge_fan(fan_in, fan_n, (float)f.center_x, (float)hub_top, (float)f.hub_box_w,
                        (float)ENTRY_MARGIN, /*fillet_r=*/8.0f, /*max_slope=*/1.2f, f.hub_fan);
    for (int i = 0; i < fan_n; i++)
        f.hub_dot_xs[i] = (int32_t)lroundf(f.hub_fan[i].pts[2].x);
}

// Per-lane derived drawing state, computed once and shared by the entry and
// merge segments of one slot's lane.
struct LaneState {
    int32_t slot_x = 0;
    bool is_active_slot = false;
    bool has_filament = false;
    PathSegment slot_segment = PathSegment::NONE;
    int32_t lane_width = 0;
    lv_color_t lane_color;       // filament color, with active-slot error override
    lv_color_t merge_line_color; // grayed past the prep sensor for non-active slots
    bool merge_is_idle = false;
};

LaneState derive_lane_state(const RenderCtx& ctx, const LinearHubFrame& f, int i) {
    const SlotRenderState& s = f.states[i];
    LaneState ls;
    ls.slot_x = ctx.geo.slot_x[i];
    ls.is_active_slot = s.is_mounted;
    ls.has_filament = s.has_filament;
    ls.slot_segment = s.segment;
    ls.lane_width = f.line_active;
    ls.lane_color = ls.has_filament ? s.color : f.idle_color;

    // Active-slot error overrides for PREP/LANE segments (must run AFTER
    // base color is set; render-state struct doesn't know about errors).
    if (ls.is_active_slot && ls.has_filament && f.has_error &&
        (f.error_seg == PathSegment::PREP || f.error_seg == PathSegment::LANE)) {
        ls.lane_color = f.error_color;
    }

    // For non-active slots with filament:
    // - Color the line FROM spool TO sensor (we know filament is here)
    // - Color the sensor dot (filament detected)
    // - Gray the line PAST sensor to merge (we don't know extent beyond sensor)
    bool is_non_active_with_filament = !ls.is_active_slot && ls.has_filament;
    bool slot_past_prep = (ls.slot_segment >= PathSegment::LANE);
    ls.merge_line_color =
        (is_non_active_with_filament && !slot_past_prep) ? f.idle_color : ls.lane_color;
    ls.merge_is_idle = !ls.has_filament || (is_non_active_with_filament && !slot_past_prep);
    if (!ls.has_filament) {
        ls.merge_line_color = f.idle_color;
    }
    return ls;
}

// Spool-grid entry down to the prep sensor (line + per-slot prep sensor dot).
void draw_lane_entry_segment(const RenderCtx& ctx, LinearHubFrame& f, int i, const LaneState& ls) {
    FilamentPathData* data = ctx.data;

    // Line from entry to prep sensor position.
    // When no prep sensor exists, draw continuously through the gap.
    int32_t line_end_y = data->slot_has_prep_sensor[i] ? (f.prep_y - f.sensor_r) : f.prep_y;
    {
        LaneStyle st =
            lane_style(ls.has_filament, ls.lane_color, f.idle_color, f.bg_color, ls.lane_width);
        draw_lane_vline(ctx.layer, ls.slot_x, f.entry_y, line_end_y, st,
                        (ls.has_filament && ls.is_active_slot) ? &f.active_path : nullptr);
    }

    // Draw prep sensor dot (per-slot capability flag)
    if (data->slot_has_prep_sensor[i]) {
        bool prep_active = ls.has_filament && is_segment_active(PathSegment::PREP, ls.slot_segment);
        lv_color_t prep_dot_color = prep_active ? ls.lane_color : f.idle_color;
        bool prep_dot_filled = prep_active;
        // Error on prep dot: only for the active slot when error is at PREP
        if (f.has_error && ls.is_active_slot && f.error_seg == PathSegment::PREP) {
            prep_dot_color = f.error_color;
            prep_dot_filled = true;
        }
        draw_sensor_dot(ctx.layer, ls.slot_x, f.prep_y, prep_dot_color, prep_dot_filled,
                        f.sensor_r);
    }
}

// HUB topology: parallel-diagonal merge run from the prep sensor down to this
// lane's own hub sensor dot on top of the hub box.
void draw_hub_lane_merge(const RenderCtx& ctx, LinearHubFrame& f, int i, const LaneState& ls) {
    int32_t hub_top = f.hub_y - f.hub_h / 2;
    // Hub-entry X (distinct per lane) was pre-computed by build_linear_hub_merge_fan.
    int32_t hub_dot_x = f.hub_dot_xs[i];

    // Merge run from prep to the hub sensor dot, using this lane's precomputed
    // fan waypoints (separation by construction). Drop the final hub_top
    // vertex down to the sensor-dot edge so the tube meets the dot, not the box.
    if (i < FilamentPathData::MAX_SLOTS) {
        LaneStyle st = lane_style(!ls.merge_is_idle, ls.merge_line_color, f.idle_color, f.bg_color,
                                  ls.lane_width);
        pg::PathPoint pts[4] = {f.hub_fan[i].pts[0],
                                f.hub_fan[i].pts[1],
                                f.hub_fan[i].pts[2],
                                {f.hub_fan[i].pts[3].x, (float)(hub_top - f.sensor_r)}};
        pg::FilamentPath path;
        pg::route_polyline_filleted(path, pts, 4, 8.0f);
        draw_lane(ctx.layer, path, st,
                  (!ls.merge_is_idle && ls.is_active_slot) ? &f.active_path : nullptr);
    }

    // Draw hub sensor dot - colored with filament color if loaded to hub
    bool dot_active = ls.has_filament && (ls.slot_segment >= PathSegment::HUB);
    lv_color_t dot_color = dot_active ? ls.lane_color : f.idle_color;
    bool dot_filled = dot_active;
    // Error on hub dot: only for the active slot when error is at HUB
    if (f.has_error && ls.is_active_slot && f.error_seg == PathSegment::HUB) {
        dot_color = f.error_color;
        dot_filled = true;
    }
    draw_sensor_dot(ctx.layer, hub_dot_x, hub_top, dot_color, dot_filled, f.sensor_r);

    // Record hidden hub interior segment for flow dot path
    if (ls.is_active_slot && dot_active) {
        f.active_path.add_line(hub_dot_x, hub_top - f.sensor_r, f.center_x,
                               f.output_y + f.sensor_r);
    }
}

// Prep sensor onward to the merge target. HUB lanes target their own hub
// sensor dot; LINEAR has none (the selector is butted against the prep
// sensors); other topologies converge to the center merge point.
void draw_lane_merge_segment(const RenderCtx& ctx, LinearHubFrame& f, int i, const LaneState& ls) {
    if (ctx.data->topology == 1) {
        draw_hub_lane_merge(ctx, f, i, ls);
    } else if (ctx.data->topology == 0) {
        // LINEAR topology: SELECTOR is butted against prep sensors — no lines between
    } else {
        // Other non-hub topologies: converge to center merge point.
        int32_t start_y_other = f.prep_y + f.sensor_r;
        LaneStyle st = lane_style(!ls.merge_is_idle, ls.merge_line_color, f.idle_color, f.bg_color,
                                  ls.lane_width);
        draw_lane_route(ctx.layer, ls.slot_x, start_y_other, f.center_x, f.merge_y, FILLET_RADIUS,
                        st, (!ls.merge_is_idle && ls.is_active_slot) ? &f.active_path : nullptr);
    }
}

// Entry lanes: one per slot, from the spool-grid entry down to the prep
// sensor, then onward to the merge target. Shows all installed filaments'
// colors, not just the active slot; the active lane records its centerline
// into f.active_path.
void draw_entry_lanes(const RenderCtx& ctx, LinearHubFrame& f) {
    for (int i = 0; i < ctx.data->slot_count; i++) {
        LaneState ls = derive_lane_state(ctx, f, i);
        draw_lane_entry_segment(ctx, f, i, ls);
        draw_lane_merge_segment(ctx, f, i, ls);
    }
}

// Bypass entry and path. Topology:
//   Hub Output Sensor (center_x, output_y)
//        │ (hub-to-merge segment — idle when bypass active)
//        ▼
//   Merge Point / Bypass Sensor (center_x, bypass_merge_y)
//        ▲
//        │ horizontal from bypass spool
//   Bypass Spool (bypass_x) → vertical down → horizontal to merge
//
// From merge point → toolhead → nozzle (shared downstream path)
// Skipped in hub_only mode (bypass is a system-level path)
void draw_bypass_section(const RenderCtx& ctx, LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    const BaseGeometry& g = ctx.geo;
    if (data->hub_only || !data->show_bypass)
        return;

    int32_t bypass_x = g.x_off + (int32_t)(g.width * BYPASS_X_RATIO);

    // Record the bypass spool hit region (absolute coords) for the click
    // hit-test. The spool is a sibling widget, so this rect is anchored to
    // the bypass merge geometry computed here. The click handler uses a
    // full-extent test (abs(dx) < sensor_r*3, abs(dy) < sensor_r*4), so the
    // stored rect's half-extents equal those bounds: width = 2*sensor_r*3,
    // height = 2*sensor_r*4 (read with margin 0 via hub_box_hit).
    data->hits.bypass = {bypass_x - f.sensor_r * 3, f.bypass_merge_y - f.sensor_r * 4,
                         bypass_x + f.sensor_r * 3, f.bypass_merge_y + f.sensor_r * 4};
    data->hits.bypass_valid = true;

    // Determine bypass colors
    lv_color_t bypass_line_color = f.idle_color;
    if (data->bypass_active) {
        bypass_line_color = lv_color_hex(data->bypass_color);
    }

    // Draw bypass filament path: horizontal line from merge sensor to spool area.
    // Line stops at spool's left edge (spool widget centered at bypass_x).
    // Spool is ~10% of width wide, so stop ~5% before bypass_x.
    int32_t bypass_line_end_x = g.x_off + (int32_t)(g.width * (BYPASS_X_RATIO - 0.05f));
    {
        LaneStyle st = lane_style(data->bypass_active, bypass_line_color, f.idle_color, f.bg_color,
                                  f.line_active);
        draw_lane_hline(ctx.layer, f.center_x + f.sensor_r, bypass_line_end_x, f.bypass_merge_y,
                        st);
    }

    // Draw bypass merge sensor dot (where bypass path joins center path)
    bool bypass_merge_active =
        data->bypass_active ||
        (data->active_slot >= 0 && is_segment_active(PathSegment::OUTPUT, f.fil_seg));
    lv_color_t bypass_merge_color = f.idle_color;
    if (data->bypass_active) {
        bypass_merge_color = lv_color_hex(data->bypass_color);
    } else if (data->active_slot >= 0 && is_segment_active(PathSegment::OUTPUT, f.fil_seg)) {
        bypass_merge_color = f.active_color;
    }
    draw_sensor_dot(ctx.layer, f.center_x, f.bypass_merge_y, bypass_merge_color,
                    bypass_merge_active, f.sensor_r);

    // "Bypass" label is drawn by the panel as a FLOATING sibling of the
    // bypass spool widget (see ui_panel_ams.cpp::setup_bypass_spool) so it
    // can sit below the spool without being clipped by the canvas bounds.
}

// Whether filament has reached the hub (drives the box tint), drawing the
// single merge→hub line for non-HUB/non-LINEAR topologies along the way.
bool draw_merge_to_hub_and_check_filament(const RenderCtx& ctx, const LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;

    if (data->topology == 0) {
        // LINEAR topology: lanes go straight to hub box (no merge line needed)
        return data->active_slot >= 0 && is_segment_active(PathSegment::HUB, f.fil_seg);
    }
    if (data->topology != 1) {
        // Other non-hub topologies: draw single merge->hub line
        bool hub_line_filled =
            (data->active_slot >= 0 && is_segment_active(PathSegment::HUB, f.fil_seg));
        lv_color_t hub_line_color = f.active_color;
        if (hub_line_filled && f.has_error && f.error_seg == PathSegment::HUB) {
            hub_line_color = f.error_color;
        }
        LaneStyle st =
            lane_style(hub_line_filled, hub_line_color, f.idle_color, f.bg_color, f.line_active);
        draw_lane_vline(ctx.layer, f.center_x, f.merge_y, f.hub_y - f.hub_h / 2, st);
        return hub_line_filled;
    }
    // HUB topology: lane lines go directly to hub sensor dots (drawn in lane loop above)
    // Check if any slot has filament at hub for tinting
    if (data->active_slot >= 0 && is_segment_active(PathSegment::HUB, f.fil_seg)) {
        return true;
    }
    for (int i = 0; i < data->slot_count; i++) {
        if (f.states[i].segment >= PathSegment::HUB) {
            return true;
        }
    }
    return false;
}

// Hub box tint priority: error at hub > buffer fault > buffer warning >
// loaded-filament tint > plain theme colors.
void resolve_hub_tint(const RenderCtx& ctx, const LinearHubFrame& f, bool hub_has_filament,
                      lv_color_t* bg_out, lv_color_t* border_out) {
    FilamentPathData* data = ctx.data;
    lv_color_t hub_bg_tinted = f.hub_bg;
    lv_color_t hub_border_final = f.hub_border;
    if (f.has_error && f.error_seg == PathSegment::HUB) {
        // Error at hub — red tint with pulsing error color
        hub_bg_tinted = ph_blend(f.hub_bg, f.error_color, 0.40f);
        hub_border_final = f.error_color;
    } else if (data->buffer_fault_state == 2) {
        // Fault detected — red tint
        hub_bg_tinted = ph_blend(f.hub_bg, data->theme.color_error, 0.50f);
        hub_border_final = data->theme.color_error;
    } else if (data->buffer_fault_state == 1) {
        // Approaching fault — yellow/warning tint
        lv_color_t warning = lv_color_hex(0xFFA500);
        hub_bg_tinted = ph_blend(f.hub_bg, warning, 0.40f);
        hub_border_final = warning;
    } else if (hub_has_filament) {
        // Healthy — subtle filament color tint (use first loaded slot's color)
        lv_color_t tint_color = f.active_color;
        if (data->active_slot < 0) {
            // No active slot — find first slot loaded to hub for tint
            for (int i = 0; i < data->slot_count; i++) {
                if (f.states[i].segment >= PathSegment::HUB) {
                    tint_color = f.states[i].color;
                    break;
                }
            }
        }
        hub_bg_tinted = ph_blend(f.hub_bg, tint_color, 0.33f);
    }
    *bg_out = hub_bg_tinted;
    *border_out = hub_border_final;
}

// Filament tube through the SELECTOR box (LINEAR topology only).
void draw_selector_tube(const RenderCtx& ctx, LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    if (data->topology != 0 || data->active_slot < 0)
        return;

    int32_t sel_top = f.hub_y - f.hub_h / 2;
    int32_t sel_bot = f.hub_y + f.hub_h / 2;
    bool hub_active = is_segment_active(PathSegment::HUB, f.fil_seg);
    lv_color_t tube_color = f.active_color;
    if (hub_active && f.has_error && f.error_seg == PathSegment::HUB) {
        tube_color = f.error_color;
    }
    LaneStyle st = lane_style(hub_active, tube_color, f.idle_color, f.bg_color, f.line_active);
    draw_lane_vline(ctx.layer, f.output_x, sel_top, sel_bot, st,
                    hub_active ? &f.active_path : nullptr);
}

// Hub/selector box: state-tinted fill, label, optional gear affordance, the
// recorded hub hit rect, and (LINEAR) the filament tube through the selector.
void draw_hub_section(const RenderCtx& ctx, LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    const BaseGeometry& g = ctx.geo;

    bool hub_has_filament = draw_merge_to_hub_and_check_filament(ctx, f);

    // Hub box - tint based on error state, buffer fault state, or filament color
    lv_color_t hub_bg_tinted, hub_border_final;
    resolve_hub_tint(ctx, f, hub_has_filament, &hub_bg_tinted, &hub_border_final);

    const char* hub_label = (data->topology == 0) ? "SELECTOR" : "HUB";

    // For LINEAR topology, hub box spans the full slot area width.
    // slot_x values are slot centers, so we add half a slot width on each
    // side to cover the full visual extent of the outermost slots.
    // For HUB topology, use the widened entry-spread width computed by
    // build_linear_hub_merge_fan so the merge tubes land cleanly on the box.
    int32_t hub_w = (data->topology == 1) ? f.hub_box_w : data->theme.hub_width;
    if (data->topology == 0 && data->slot_count > 1) {
        int32_t first_slot_x = g.slot_x[0];
        int32_t last_slot_x = g.slot_x[data->slot_count - 1];
        int32_t slot_pad = LV_MAX(data->slot_width, f.sensor_r * 4);
        hub_w = (last_slot_x - first_slot_x) + slot_pad;
    }

    lv_opa_t hub_opa = (data->topology == 0) ? LV_OPA_60 : LV_OPA_COVER;
    int32_t gear_overflow =
        draw_hub_box(ctx, f.center_x, f.hub_y, hub_w, f.hub_h, hub_bg_tinted, hub_border_final,
                     hub_label, hub_opa, /*interactive=*/data->hub_callback != nullptr);

    // Single source of truth for the selector/hub hit-test: record the exact
    // box we just drew (absolute display coords). The click handler reads
    // this instead of re-deriving geometry that drifts from the render. When
    // the gear is drawn OUTSIDE the box's right edge (label too wide to fit
    // it inside), extend the hit rect rightward so the gear stays tappable.
    data->hits.hub = {f.center_x - hub_w / 2, f.hub_y - f.hub_h / 2,
                      f.center_x + hub_w / 2 + gear_overflow, f.hub_y + f.hub_h / 2};
    data->hits.hub_valid = true;

    draw_selector_tube(ctx, f);
}

// Output section: hub output sensor + the hub-to-merge/toolhead segment, with
// the optional buffer ("BUF") element in the middle. The output sensor is
// butted against the hub bottom (mirrors input sensors at hub top). When the
// bypass is shown the segment runs output → bypass merge point; when hidden it
// runs output → toolhead directly.
// Buffer (TurtleNeck / eSpooler) element: straight filament (no caps) + the
// "BUF" box on top + the continuation run down to the merge/toolhead, plus
// the recorded buffer hit rect.
void draw_buffer_element(const RenderCtx& ctx, LinearHubFrame& f, int32_t output_end_y) {
    FilamentPathData* data = ctx.data;
    if (!f.has_buffer)
        return;

    bool buffer_has_filament =
        (data->active_slot >= 0 && is_segment_active(PathSegment::OUTPUT, f.fil_seg)) ||
        data->bypass_active;
    lv_color_t buf_fil_color =
        data->bypass_active ? lv_color_hex(data->bypass_color) : f.active_color;

    // Straight filament through buffer
    {
        LaneStyle st =
            lane_style(buffer_has_filament, buf_fil_color, f.idle_color, f.bg_color, f.line_active);
        draw_lane_vline(ctx.layer, f.center_x, f.buf_fil_top, f.buf_fil_bot, st,
                        (buffer_has_filament && !data->bypass_active && data->active_slot >= 0)
                            ? &f.active_path
                            : nullptr);
    }

    // Buffer box on top
    draw_buffer_coil(ctx, f.center_x, f.buffer_y, f.hub_h, buffer_has_filament, buf_fil_color);

    // Record the exact drawn box (absolute coords) for the click
    // hit-test. Mirrors draw_buffer_coil()'s internal clamping so the
    // click handler never re-derives the geometry.
    int32_t buf_hit_w = data->theme.hub_width * 4 / 5;
    int32_t buf_hit_h = f.hub_h;
    if (buf_hit_w < 36)
        buf_hit_w = 36;
    if (buf_hit_h < 16)
        buf_hit_h = 16;
    data->hits.buffer = {f.center_x - buf_hit_w / 2, f.buffer_y - buf_hit_h / 2,
                         f.center_x + buf_hit_w / 2, f.buffer_y + buf_hit_h / 2};
    data->hits.buffer_valid = true;

    // Continuation: buffer bottom → merge/toolhead
    {
        LaneStyle st =
            lane_style(buffer_has_filament, buf_fil_color, f.idle_color, f.bg_color, f.line_active);
        draw_lane_vline(ctx.layer, f.center_x, f.buf_fil_bot, output_end_y - f.sensor_r, st,
                        (buffer_has_filament && !data->bypass_active && data->active_slot >= 0)
                            ? &f.active_path
                            : nullptr);
    }
}

void draw_output_section(const RenderCtx& ctx, LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    if (data->hub_only)
        return;

    // Hub output sensor — determine if AMS filament is passing through
    bool ams_output_active = !data->bypass_active && data->active_slot >= 0 &&
                             is_segment_active(PathSegment::OUTPUT, f.fil_seg);
    lv_color_t output_dot_color = f.idle_color;
    bool output_dot_filled = false;
    if (ams_output_active) {
        output_dot_color = f.active_color;
        output_dot_filled = true;
        if (f.has_error && f.error_seg == PathSegment::OUTPUT) {
            output_dot_color = f.error_color;
        }
    } else if (f.has_error && f.error_seg == PathSegment::OUTPUT) {
        output_dot_color = f.error_color;
        output_dot_filled = true;
    }
    draw_sensor_dot(ctx.layer, f.output_x, f.output_y, output_dot_color, output_dot_filled,
                    f.sensor_r);

    // When bypass is hidden, output connects directly to toolhead (no merge point gap)
    int32_t output_end_y = data->show_bypass ? f.bypass_merge_y : f.toolhead_y;

    // No-cap endpoints where buffer segments meet
    int32_t seg_end_y = f.has_buffer ? f.buf_fil_top : (output_end_y - f.sensor_r);

    // Segment: output sensor → buffer top (or merge/toolhead when no buffer)
    // LINEAR: S-curve from output_x down to center_x
    // HUB: straight vertical at center_x
    lv_color_t seg_color = f.active_color;
    if (ams_output_active && f.has_error && f.error_seg == PathSegment::OUTPUT) {
        seg_color = f.error_color;
    }
    if (data->topology == 0 && f.output_x != f.center_x) {
        // LINEAR with off-center output: orthogonal route from output_x to center_x.
        int32_t oc_start_y = f.output_y + f.sensor_r;
        LaneStyle st =
            lane_style(ams_output_active, seg_color, f.idle_color, f.bg_color, f.line_active);
        draw_lane_route(ctx.layer, f.output_x, oc_start_y, f.center_x, seg_end_y, FILLET_RADIUS, st,
                        ams_output_active ? &f.active_path : nullptr);
    } else {
        // HUB or LINEAR with output at center: straight vertical.
        LaneStyle st =
            lane_style(ams_output_active, seg_color, f.idle_color, f.bg_color, f.line_active);
        draw_lane_vline(ctx.layer, f.center_x, f.output_y + f.sensor_r, seg_end_y, st,
                        ams_output_active ? &f.active_path : nullptr);
    }

    draw_buffer_element(ctx, f, output_end_y);
}

// Toolhead section: bypass merge → toolhead sensor. Active when ANY filament
// is flowing (AMS or bypass). Skipped when the bypass is hidden — the output
// section then draws directly to the toolhead.
void draw_toolhead_section(const RenderCtx& ctx, LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    if (data->hub_only || !data->show_bypass)
        return;

    lv_color_t toolhead_color = f.idle_color;
    bool toolhead_active = false;
    if (data->bypass_active) {
        toolhead_color = lv_color_hex(data->bypass_color);
        toolhead_active = true;
    } else if (data->active_slot >= 0 && is_segment_active(PathSegment::TOOLHEAD, f.fil_seg)) {
        toolhead_color = f.active_color;
        toolhead_active = true;
        if (f.has_error && f.error_seg == PathSegment::TOOLHEAD) {
            toolhead_color = f.error_color;
        }
    }

    // Line from bypass merge point to toolhead sensor
    {
        LaneStyle st =
            lane_style(toolhead_active, toolhead_color, f.idle_color, f.bg_color, f.line_active);
        draw_lane_vline(ctx.layer, f.center_x, f.bypass_merge_y + f.sensor_r,
                        f.toolhead_y - f.sensor_r, st,
                        (toolhead_active && !data->bypass_active) ? &f.active_path : nullptr);
    }

    // Toolhead sensor
    lv_color_t toolhead_dot_color = toolhead_active ? toolhead_color : f.idle_color;
    bool toolhead_dot_filled = toolhead_active;
    if (f.has_error && f.error_seg == PathSegment::TOOLHEAD) {
        toolhead_dot_color = f.error_color;
        toolhead_dot_filled = true;
    }
    draw_sensor_dot(ctx.layer, f.center_x, f.toolhead_y, toolhead_dot_color, toolhead_dot_filled,
                    f.sensor_r);
}

// Nozzle section: toolhead sensor → extruder line, the toolhead glyph, and the
// active-path cache handoff to the DRAW_POST animation pass.
void draw_nozzle_section(const RenderCtx& ctx, LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    if (data->hub_only)
        return;

    int32_t extruder_half_height = data->theme.extruder_scale * 2; // Half of body_height
    lv_color_t noz_color = f.nozzle_color;

    // Bypass or normal slot active?
    if (data->bypass_active) {
        // Bypass active - use bypass color for nozzle
        noz_color = lv_color_hex(data->bypass_color);
    } else if (data->active_slot >= 0 && is_segment_active(PathSegment::NOZZLE, f.fil_seg)) {
        noz_color = f.active_color;
        if (f.has_error && f.error_seg == PathSegment::NOZZLE) {
            noz_color = f.error_color;
        }
    }

    // Line from toolhead sensor to extruder (adjust gap for tall extruder body)
    // Use toolhead color (idle gray when no filament) for the connecting line,
    // not nozzle color which is always tinted
    bool nozzle_has_filament =
        data->bypass_active ||
        (data->active_slot >= 0 && is_segment_active(PathSegment::NOZZLE, f.fil_seg));
    {
        LaneStyle st =
            lane_style(nozzle_has_filament, noz_color, f.idle_color, f.bg_color, f.line_active);
        draw_lane_vline(ctx.layer, f.center_x, f.toolhead_y + f.sensor_r,
                        f.nozzle_y - extruder_half_height, st,
                        (nozzle_has_filament && !data->bypass_active) ? &f.active_path : nullptr);
    }

    // Cache the recorded active path + geometry locals so the animation
    // DRAW_POST pass can paint flow dots / heat glow / segment tip
    // without re-running the state-tied draw on each animation tick.
    //
    // Worst-case segment budget: the longest active HUB path records the
    // entry vertical (1) + merge fan polyline (3 lines + 2 arcs = 5) + hidden
    // hub interior (1) + output/toolhead/nozzle run (~2) = 11 segs, safely
    // under FilamentPath::MAX_SEGS (16). add_line/add_arc silently no-op past
    // the cap, so an overrun would only truncate the tail, never corrupt.
    data->path_cache.path = f.active_path;
    data->path_cache.center_x = f.center_x;
    data->path_cache.nozzle_y = f.nozzle_y;
    data->path_cache.sensor_r = f.sensor_r;
    data->path_cache.valid = true;

    // Flow particles, heat glow, and segment-transition tip live in the
    // animation DRAW_POST pass (see draw_animation_linear_hub) — not here.

    // Extruder/print head icon
    draw_toolhead(ctx.layer, f.center_x, f.nozzle_y, noz_color, data->theme.extruder_scale);
}

// LINEAR/HUB renderer: one frame computation, then the phases in physical
// path order. Hit-rect valid flags are reset here and re-recorded by the
// phases that actually draw the boxes.
void render_linear_hub(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};

    // Invalidated until a selector/hub box is actually drawn this pass. The
    // draw sites below record the real rects.
    data->hits.hub_valid = false;
    data->hits.buffer_valid = false;
    data->hits.bypass_valid = false;

    LinearHubFrame f = compute_linear_hub_frame(ctx);
    apply_debug_flow_override(obj, ctx, f);
    build_linear_hub_merge_fan(ctx, f);

    draw_entry_lanes(ctx, f);
    draw_bypass_section(ctx, f);
    draw_hub_section(ctx, f);
    draw_output_section(ctx, f);
    draw_toolhead_section(ctx, f);
    draw_nozzle_section(ctx, f);
}

// ============================================================================
// DRAW_POST animation overlays
// ============================================================================

// Animation overlay for LINEAR/HUB — flow particles, heat glow, segment
// transition filament tip. Reads the active path cached by the state-tied
// renderer (populated in draw_nozzle_section).
void draw_animation_linear_hub(lv_layer_t* layer, FilamentPathData* data) {
    if (!data->path_cache.valid)
        return;

    lv_color_t active_color = lv_color_hex(data->filament_color);
    int32_t sensor_r = data->path_cache.sensor_r;
    int32_t center_x = data->path_cache.center_x;
    int32_t nozzle_y = data->path_cache.nozzle_y;
    auto& path = data->path_cache.path;

    // Flow particles along the active filament path.
    if (data->anim.flow_active && data->active_slot >= 0 && !data->hub_only) {
        bool reverse = (data->anim.direction == AnimDirection::UNLOADING);
        draw_flow_dots_path(layer, path, active_color, data->anim.flow_offset, reverse);
    }

    // Heat glow halo around the nozzle tip.
    if (data->anim.heat_active) {
        int32_t tip_y = toolhead_tip_y(nozzle_y, data->theme.extruder_scale);
        draw_heat_glow(layer, center_x, tip_y, sensor_r, data->anim.heat_pulse_opa);
    }

    // Segment transition tip — interpolated along the path.
    if (data->anim.segment_active && data->active_slot >= 0 && !data->hub_only && path.count > 0) {
        PathSegment prev_seg = static_cast<PathSegment>(data->anim.prev_segment);
        PathSegment fil_seg = static_cast<PathSegment>(data->filament_segment);
        float progress_factor = data->anim.progress / 100.0f;
        const float NUM_INTERVALS = static_cast<float>(static_cast<int>(PathSegment::NOZZLE) -
                                                       static_cast<int>(PathSegment::SPOOL));
        float base = static_cast<float>(static_cast<int>(prev_seg) - 1);
        float target = static_cast<float>(static_cast<int>(fil_seg) - 1);
        float tip_fraction = (base + (target - base) * progress_factor) / NUM_INTERVALS;
        tip_fraction = LV_CLAMP(tip_fraction, 0.0f, 1.0f);
        float tip_distance = tip_fraction * pg::path_length(path);
        pg::PathPoint tip = pg::path_point_at(path, tip_distance);
        int32_t tip_x = (int32_t)lroundf(tip.x);
        int32_t tip_y = (int32_t)lroundf(tip.y);
        bool in_nozzle_body =
            (prev_seg == PathSegment::TOOLHEAD && fil_seg == PathSegment::NOZZLE) ||
            (prev_seg == PathSegment::NOZZLE && fil_seg == PathSegment::TOOLHEAD);
        if (!in_nozzle_body) {
            draw_filament_tip(layer, tip_x, tip_y, active_color, sensor_r);
        }
    }
}

// Animation overlay for PARALLEL — flow particles on the mounted slot's entry
// run. Painted via DRAW_POST on top of the overlay canvas.
void draw_animation_parallel(lv_layer_t* layer, const BaseGeometry& g,
                             const SlotRenderStates& states, const FilamentPathData* data) {
    if (!data->anim.flow_active)
        return;
    int32_t entry_y = g.y_off + static_cast<int32_t>(g.height * -0.12f);
    int32_t sensor_y = g.y_off + static_cast<int32_t>(g.height * PARALLEL_SENSOR_Y_RATIO);
    int32_t sensor_r = data->theme.sensor_radius;
    bool reverse = (data->anim.direction == AnimDirection::UNLOADING);
    int count = LV_MIN(data->slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < count; i++) {
        const SlotRenderState& s = states[i];
        if (!s.is_mounted || !s.has_filament)
            continue;
        draw_flow_dots_line(layer, g.slot_x[i], entry_y, g.slot_x[i], sensor_y - sensor_r, s.color,
                            data->anim.flow_offset, reverse);
    }
}

} // namespace

// ============================================================================
// Entry points (called by the layers module and the widget's DRAW_POST event)
// ============================================================================

void render_overlay_content(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    if (data->topology == static_cast<int>(PathTopology::MIXED)) {
        render_mixed(obj, layer, data);
    } else if (data->topology == static_cast<int>(PathTopology::PARALLEL)) {
        render_parallel(obj, layer, data);
    } else {
        render_linear_hub(obj, layer, data);
    }
}

void render_animation_overlay(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    int topo = data->topology;
    if (topo == static_cast<int>(PathTopology::PARALLEL)) {
        BaseGeometry g = compute_base_geometry(obj, data);
        SlotRenderStates states = compute_slot_render_states(data);
        draw_animation_parallel(layer, g, states, data);
    } else if (topo == static_cast<int>(PathTopology::LINEAR) ||
               topo == static_cast<int>(PathTopology::HUB)) {
        draw_animation_linear_hub(layer, data);
    }
    // MIXED has no per-frame animation — nothing to paint here.
}

} // namespace helix::ui::fpath
