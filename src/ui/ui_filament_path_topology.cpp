// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Topology renderers for the filament_path_canvas widget. Three layouts:
//
//   LINEAR/HUB (render_linear_hub) — the classic AMS detail view: entry lanes
//   → prep sensors → merge (fan into HUB, or butt into the LINEAR selector) →
//   hub/selector box → output sensor → buffer → bypass merge → toolhead
//   sensor → nozzle.
//
//   PARALLEL (render_parallel) — tool changers: every slot is an independent
//   column (entry → sensor → own toolhead + badge).
//
//   MIXED (render_mixed) — HTLF-style: some lanes run direct to their own
//   nozzle, others fan into a shared hub feeding one nozzle.
//
// All three paint one route plan (ui_filament_path_plan.cpp) with the layered
// painter, then draw their boxes and glyphs over it. Each derives a per-draw
// "frame" (layout Ys, resolved colors, per-slot states) once. LINEAR/HUB stores the active route's
// filled prefix in FilamentPathData::path_cache, and the hub/buffer/bypass boxes record their hit
// rects — see ui_filament_path_internal.h ("render → record").
//
// The DRAW_POST animation overlays (flow dots, heat glow, moving tip) live at
// the bottom of this file; they replay the recorded path each frame without
// re-running the heavyweight render.

#include "ui_filament_path_internal.h"
#include "ui_filament_path_plan.h"

#include "helix_psram_attr.h"

#include <spdlog/spdlog.h>

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

// One plan for whichever topology is rendering. About 14 KB: kept off the
// stack, which on the ESP32 is the LVGL task's, and out of internal DRAM,
// which the WiFi driver needs for its RX buffers. Rendering is single-threaded
// and not re-entrant, and nothing DMA- or ISR-side touches it.
PathPlan& plan_scratch() {
    static HELIX_PSRAM_BSS PathPlan plan;
    return plan;
}

void warn_if_dropped(const PathPlan& plan) {
    if (plan.dropped <= 0)
        return;
    // Every repaint would repeat it; one line per 10 s is enough to notice.
    static uint32_t last_warn_ms = 0;
    static bool warned = false;
    if (!warned || lv_tick_elaps(last_warn_ms) >= 10000) {
        warned = true;
        last_warn_ms = lv_tick_get();
        spdlog::warn("[FilamentPath] Route plan over budget: {} segment(s)/band(s) dropped",
                     plan.dropped);
    }
}

// ============================================================================
// PARALLEL topology (tool changers)
// ============================================================================
// Tool changers have independent toolheads: each slot is a complete tool with
// its own extruder, entry → sensor → own toolhead + badge.

// One tool's toolhead glyph and badge, drawn over its planned tube.
void draw_parallel_tool(const RenderCtx& ctx, const SlotRenderStates& states, int i,
                        int32_t toolhead_y) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    int32_t slot_x = ctx.geo.slot_x[i];
    const SlotRenderState& s = states[i];
    int32_t tool_scale = LV_MAX(6, theme.extruder_scale * 2 / 3);

    // Nozzle color only when filament actually reaches the nozzle
    std::optional<lv_color_t> noz_color;
    if (s.at_nozzle)
        noz_color = s.color;

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

void render_parallel(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};
    PathPlan& plan = plan_scratch();
    plan_parallel(*data, ctx.geo, plan);
    warn_if_dropped(plan);
    paint_tubes(layer, plan, tube_palette(*data));

    const int32_t toolhead_y =
        ctx.geo.y_off + (int32_t)(ctx.geo.height * PARALLEL_TOOLHEAD_Y_RATIO);
    const SlotRenderStates states = compute_slot_render_states(data);
    for (int i = 0; i < LV_MIN(data->slot_count, FilamentPathData::MAX_SLOTS); i++)
        draw_parallel_tool(ctx, states, i, toolhead_y);
}

// ============================================================================
// MIXED topology (HTLF: direct + hub lanes)
// ============================================================================
// Some lanes go directly to their own nozzle (like PARALLEL), while others
// converge through a hub box to a shared nozzle. Visual layout:
//   [spool0] [spool1] [spool2] [spool3]
//      |        |        |        |       entry lines
//      =        =        =        =       sensor bands
//      |        |         \      /        direct vs angled paths
//      |        |        [HUB]            hub box (hub lanes converge)
//      |        |          |              hub output line
//     (T0)    (T2)       (T1)             nozzles + tool labels

// The shared hub toolhead and its tool badge.
void draw_mixed_shared_toolhead(const RenderCtx& ctx, const MixedFrame& f) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;

    // Check if any hub lane has filament at nozzle
    bool any_hub_at_nozzle = false;
    lv_color_t hub_nozzle_color = theme.color_idle;
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

    // Shared hub nozzle — always "mounted" visually (it's a shared output)
    std::optional<lv_color_t> noz_color;
    if (any_hub_at_nozzle)
        noz_color = hub_nozzle_color;
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

// A direct lane's own nozzle and badge.
void draw_mixed_direct_toolhead(const RenderCtx& ctx, const MixedFrame& f, int i) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    const SlotRenderState& s = f.states[i];
    int32_t slot_x = ctx.geo.slot_x[i];

    std::optional<lv_color_t> noz_color;
    if (s.at_nozzle)
        noz_color = s.color;
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

void render_mixed(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};
    const MixedFrame f = compute_mixed_frame(*data, ctx.geo);
    PathPlan& plan = plan_scratch();
    plan_mixed(f, *data, ctx.geo, plan);
    warn_if_dropped(plan);
    const TubePalette pal = tube_palette(*data);
    paint_tubes(layer, plan, pal);

    if (f.hub_count > 0) {
        draw_hub_box(ctx, f.hub_cx, f.hub_cy, f.hub_w, f.hub_h, data->theme.color_hub_bg,
                     data->theme.color_hub_border, "HUB");
    }
    paint_box_bands(layer, plan, pal);

    if (f.hub_count > 0)
        draw_mixed_shared_toolhead(ctx, f);
    for (int i = 0; i < LV_MIN(data->slot_count, FilamentPathData::MAX_SLOTS); i++) {
        if (!data->slot_is_hub_routed[i])
            draw_mixed_direct_toolhead(ctx, f, i);
    }
}

// ============================================================================
// LINEAR / HUB topology
// ============================================================================

// Debug override: HELIX_FLOW_SEGMENT=PREP|LANE|HUB|OUTPUT|TOOLHEAD|NOZZLE
//                 HELIX_FLOW_DIR=LOAD|UNLOAD
// Forces filament to the specified segment with flow animation for isolated
// testing, and keeps a 30ms repaint timer alive while active.
void apply_debug_flow_override(lv_obj_t* obj, FilamentPathData* data) {
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

// Whether filament has reached the hub (drives the box tint).
bool hub_has_filament(const FilamentPathData* data, const LinearHubFrame& f) {
    if (data->active_slot >= 0 && is_segment_active(PathSegment::HUB, f.fil_seg))
        return true;
    if (data->topology == static_cast<int>(PathTopology::LINEAR))
        return false;
    for (int i = 0; i < data->slot_count; i++) {
        if (f.states[i].segment >= PathSegment::HUB)
            return true;
    }
    return false;
}

// Hub box tint priority: error at hub > buffer fault > buffer warning >
// loaded-filament tint > plain theme colors.
void resolve_hub_tint(const RenderCtx& ctx, const LinearHubFrame& f, bool has_filament,
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
    } else if (has_filament) {
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

// Hub/selector box: state-tinted fill, label, optional gear affordance and
// the recorded hub hit rect.
void draw_hub_section(const RenderCtx& ctx, const LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    const BaseGeometry& g = ctx.geo;

    // Hub box - tint based on error state, buffer fault state, or filament color
    lv_color_t hub_bg_tinted, hub_border_final;
    resolve_hub_tint(ctx, f, hub_has_filament(data, f), &hub_bg_tinted, &hub_border_final);

    const char* hub_label = (data->topology == 0) ? "SELECTOR" : "HUB";

    // For LINEAR topology, hub box spans the full slot area width.
    // slot_x values are slot centers, so we add half a slot width on each
    // side to cover the full visual extent of the outermost slots.
    // For HUB topology, use the widened entry-spread width from the frame so
    // the merge tubes land cleanly on the box.
    int32_t hub_w = (data->topology == 1) ? f.hub_box_w : data->theme.hub_width;
    if (data->topology == 0 && data->slot_count > 1) {
        int32_t first_slot_x = g.slot_x[0];
        int32_t last_slot_x = g.slot_x[data->slot_count - 1];
        int32_t slot_pad = LV_MAX(data->slot_width, f.sensor_r * 4);
        hub_w = (last_slot_x - first_slot_x) + slot_pad;
    }

    lv_opa_t hub_opa = (data->topology == 0) ? LV_OPA_60 : LV_OPA_COVER;

    // On-toolhead mode draws the passthrough selector first, in its classic
    // spot under the lanes, full slot width like LINEAR's selector.
    int32_t selector_gear_overflow = 0;
    if (data->hub_on_toolhead) {
        int32_t sel_w = data->theme.hub_width;
        if (data->slot_count > 1) {
            int32_t first_slot_x = g.slot_x[0];
            int32_t last_slot_x = g.slot_x[data->slot_count - 1];
            int32_t slot_pad = LV_MAX(data->slot_width, f.sensor_r * 4);
            sel_w = (last_slot_x - first_slot_x) + slot_pad;
        }
        selector_gear_overflow =
            draw_hub_box(ctx, f.center_x, f.selector_y, sel_w, f.hub_h, hub_bg_tinted,
                         hub_border_final, "SELECTOR", LV_OPA_60,
                         /*interactive=*/data->hub_callback != nullptr);
        // The unit's own box is the management target; the hub on the head is
        // not a separate control.
        data->hits.hub = {f.center_x - sel_w / 2, f.selector_y - f.hub_h / 2,
                          f.center_x + sel_w / 2 + selector_gear_overflow,
                          f.selector_y + f.hub_h / 2};
        data->hits.hub_valid = true;
    }

    int32_t gear_overflow =
        draw_hub_box(ctx, f.center_x, f.hub_y, hub_w, f.hub_h, hub_bg_tinted, hub_border_final,
                     hub_label, hub_opa, /*interactive=*/data->hub_callback != nullptr);

    // Single source of truth for the selector/hub hit-test: record the exact
    // box we just drew (absolute display coords). When the gear is drawn
    // OUTSIDE the box's right edge (label too wide to fit it inside), extend
    // the hit rect rightward so the gear stays tappable. On-toolhead mode
    // recorded the SELECTOR box above; the head hub is not a control.
    if (!data->hub_on_toolhead) {
        data->hits.hub = {f.center_x - hub_w / 2, f.hub_y - f.hub_h / 2,
                          f.center_x + hub_w / 2 + gear_overflow, f.hub_y + f.hub_h / 2};
        data->hits.hub_valid = true;
    }
}

// Buffer (TurtleNeck / eSpooler) "BUF" box over the trunk, plus its recorded
// hit rect. Mirrors draw_buffer_coil()'s internal clamping so the click
// handler never re-derives the geometry.
void draw_buffer_section(const RenderCtx& ctx, const LinearHubFrame& f, const PathPlan& plan) {
    FilamentPathData* data = ctx.data;
    draw_buffer_coil(ctx, f.center_x, f.buffer_y, f.hub_h, plan.buffer_has_filament,
                     plan.buffer_fill);

    int32_t buf_hit_w = LV_MAX(36, data->theme.hub_width * 4 / 5);
    int32_t buf_hit_h = LV_MAX(16, f.hub_h);
    data->hits.buffer = {f.center_x - buf_hit_w / 2, f.buffer_y - buf_hit_h / 2,
                         f.center_x + buf_hit_w / 2, f.buffer_y + buf_hit_h / 2};
    data->hits.buffer_valid = true;
}

// Bypass spool hit region (absolute coords). The spool is a sibling widget,
// so the rect is anchored to the bypass merge geometry: the click handler's
// full-extent test is abs(dx) < sensor_r*3, abs(dy) < sensor_r*4, read with
// margin 0 via hub_box_hit.
void record_bypass_hit(const RenderCtx& ctx, const LinearHubFrame& f) {
    const BaseGeometry& g = ctx.geo;
    int32_t bypass_x = g.x_off + (int32_t)(g.width * BYPASS_X_RATIO);
    ctx.data->hits.bypass = {bypass_x - f.sensor_r * 3, f.bypass_merge_y - f.sensor_r * 4,
                             bypass_x + f.sensor_r * 3, f.bypass_merge_y + f.sensor_r * 4};
    ctx.data->hits.bypass_valid = true;
}

// Extruder glyph, in the color of whatever filament reached the nozzle.
void draw_nozzle_glyph(const RenderCtx& ctx, const LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    std::optional<lv_color_t> noz_color;
    if (data->bypass_active) {
        noz_color = lv_color_hex(data->bypass_color);
    } else if (data->active_slot >= 0 && is_segment_active(PathSegment::NOZZLE, f.fil_seg)) {
        noz_color =
            (f.has_error && f.error_seg == PathSegment::NOZZLE) ? f.error_color : f.active_color;
    }
    draw_toolhead(ctx.layer, f.center_x, f.nozzle_y, noz_color, data->theme.extruder_scale);
}

// LINEAR/HUB renderer: frame → plan → tubes → boxes and glyphs. Hit-rect
// valid flags are reset here and re-recorded where the boxes are drawn; the
// active route's filled prefix becomes the animation pass's path.
void render_linear_hub(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    data->hits.hub_valid = false;
    data->hits.buffer_valid = false;
    data->hits.bypass_valid = false;

    apply_debug_flow_override(obj, data);
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};

    // LINEAR: the output exit snaps under the active slot unless it is sliding there.
    const int active = data->active_slot;
    if (data->topology == 0 && active >= 0 && active < ctx.geo.slot_count &&
        !data->anim.output_x_active) {
        data->anim.output_x_current = ctx.geo.slot_x[active];
        data->anim.output_x_target = ctx.geo.slot_x[active];
    }

    const LinearHubFrame f = compute_linear_hub_frame(*data, ctx.geo);
    PathPlan& plan = plan_scratch();
    plan_linear_hub(f, *data, ctx.geo, plan);
    warn_if_dropped(plan);

    const TubePalette pal = tube_palette(*data);
    paint_tubes(layer, plan, pal);

    draw_hub_section(ctx, f);
    if (!data->hub_only && f.has_buffer)
        draw_buffer_section(ctx, f, plan);
    // Bands on the hub/selector edges clamp the tube where it enters the box.
    paint_box_bands(layer, plan, pal);
    if (data->hub_only) {
        // Nothing below the hub is drawn: no nozzle to glow, no path to replay.
        data->path_cache = PathCache{};
        return;
    }
    if (data->show_bypass)
        record_bypass_hit(ctx, f);
    draw_nozzle_glyph(ctx, f);

    data->path_cache.path = (plan.active_route >= 0) ? filled_prefix(plan.routes[plan.active_route])
                                                     : pg::FilamentPath{};
    data->path_cache.center_x = f.center_x;
    data->path_cache.nozzle_y = f.nozzle_y;
    data->path_cache.sensor_r = f.sensor_r;
    data->path_cache.valid = true;
}

// ============================================================================
// DRAW_POST animation overlays
// ============================================================================

// Animation overlay for LINEAR/HUB — flow particles, heat glow, segment
// transition filament tip. Reads the active path cached by the state-tied
// renderer (populated in render_linear_hub).
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
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    data->hits.origin = {coords.x1, coords.y1};
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
