// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The overview's layout and route plan. See ui_system_path_plan.h.

#include "ui_system_path_plan.h"

#include "ui_bypass_spool_widget.h"
#include "ui_toolhead_badge.h"

#include "helix/ui/text_metrics.h"
#include "hub_buffer_meter.h"
#include "tube_router.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace helix::ui::syspath {

using fpath::add_band;
using fpath::add_band_at_end;
using fpath::append_line;
using fpath::BandKind;
using fpath::Lane;
using fpath::new_route;
using fpath::Route;
using fpath::route_append;
using fpath::unpainted;
namespace pg = helix::ui::pathgeo;

namespace {

// Mirrors ui_filament_path_canvas's kBypassXRatio so both canvases place the
// bypass spool at the same horizontal fraction — keeps a long visible tube
// segment instead of cramming the spool right next to the hub.
constexpr float kBypassXRatio = 0.85f;

// Merge fans: the outermost entries sit this far in from the hub-top ends, and
// the per-side slope is capped so tall hubs don't run near-vertical.
constexpr float kFanEntryMargin = 8.0f;
constexpr float kFanMaxSlope = 1.2f;
constexpr float kFanFillet = 9.0f;

constexpr int kTopoParallel = 2;
constexpr int kTopoMixed = 3;

// A box with filament passing through it is tinted toward the filament.
lv_color_t hub_fill(const SystemPathData& data, bool has_filament) {
    const lv_color_t bg = data.color_hub_bg;
    return has_filament ? fpath::ph_blend(bg, lv_color_hex(data.active_color), 0.33f) : bg;
}

// One tube's pitch where tubes run side by side: a haloed tube and a gap.
int32_t lane_pitch(const SystemPathData& data) {
    return data.tube_gauge + kHaloWidthExtra + 2;
}

// The span a unit stem may stand in: the canvas less a small margin.
struct StemSpan {
    int32_t lo, hi;
};
StemSpan stem_span(const SysLayout& L) {
    const int32_t margin = LV_MIN(8, L.width / 4);
    return {L.x_off + margin, L.x_off + L.width - margin};
}

// Which edge unit @p i's card has scrolled past: -1 left, +1 right, 0 on the canvas.
int parked_side(const SystemPathData& data, const SysLayout& L, int i) {
    if (i < 0 || i >= SystemPathData::kMaxUnits)
        return 0;
    const StemSpan span = stem_span(L);
    const int32_t x = L.x_off + data.unit_x_positions[i];
    return x < span.lo ? -1 : (x > span.hi ? 1 : 0);
}

// The stem @p rank tube pitches in from the edge on @p side.
int32_t parked_stem_x(const SystemPathData& data, const SysLayout& L, int side, int rank) {
    const StemSpan span = stem_span(L);
    const int32_t step = rank * lane_pitch(data);
    return side > 0 ? LV_MAX(span.lo, span.hi - step) : LV_MIN(span.hi, span.lo + step);
}

// Horizontal position of unit `i`'s entry stem: the center of its unit card,
// clamped into the canvas.
//
// The anchor is pushed by the panel and re-pushed on every card-row scroll -
// unit_cards_row is an independently scrollable container. Once the row
// overflows, a scrolled-off card's center lands outside the canvas; clamped,
// the stem parks at the edge it went out of, which reads as "this unit is off
// to that side". Several parked stems stand one tube pitch apart in card
// order, the farthest card at the edge, so their lines never share a stem.
int32_t unit_stem_x(const SystemPathData& data, const SysLayout& L, int i) {
    if (i < 0 || i >= SystemPathData::kMaxUnits) {
        return L.x_off + L.width / 2;
    }
    const int side = parked_side(data, L, i);
    if (side == 0)
        return L.x_off + data.unit_x_positions[i];
    const int32_t x = L.x_off + data.unit_x_positions[i];
    int farther = 0;
    for (int u = 0; u < data.unit_count && u < SystemPathData::kMaxUnits; ++u) {
        const int32_t ux = L.x_off + data.unit_x_positions[u];
        if (u == i || parked_side(data, L, u) != side)
            continue;
        const bool beyond =
            side > 0 ? (ux > x || (ux == x && u > i)) : (ux < x || (ux == x && u > i));
        farther += beyond;
    }
    return parked_stem_x(data, L, side, farther);
}

// The fan of one or more lanes into a hub top at (hub_cx, hub_top).
void merge_fan(const pg::MergeLaneIn* in, int n, int32_t hub_cx, int32_t hub_top, int32_t hub_w,
               pg::MergeLaneOut* out) {
    pg::build_merge_fan(in, n, (float)hub_cx, (float)hub_top, (float)hub_w, kFanEntryMargin,
                        kFanFillet, kFanMaxSlope, out);
}

void append_fan(Route& r, const pg::MergeLaneOut& lane, fpath::SpanStyle s) {
    pg::FilamentPath fan;
    pg::route_polyline_filleted(fan, lane.pts, 4, kFanFillet);
    route_append(r, fan, s);
}

// X where the route currently ends.
float seg_end_x(const Route& r) {
    const pg::PathSeg& s = r.path.segs[r.path.count - 1];
    if (s.type == pg::PathSeg::LINE)
        return s.p1.x;
    return s.center.x + s.radius * std::cos(s.start_angle + s.sweep);
}

// A new route, or nullptr when the plan is full.
Route* try_new_route(PathPlan& plan) {
    if (plan.route_count >= fpath::kMaxRoutes) {
        plan.dropped++;
        return nullptr;
    }
    return &new_route(plan);
}

// ----------------------------------------------------------------------------
// Multi-tool (toolchanger / mixed overview — one nozzle per tool)
// ----------------------------------------------------------------------------

// One unit→tool route.
struct GlobalRoute {
    int unit_idx;
    int tool_idx;
    int32_t start_x;
    int32_t start_y;
    int32_t end_x;
    int32_t end_y;
    int32_t dist;      // absolute horizontal distance
    bool is_hub;       // a HUB unit's one route
    bool hub_lane;     // carries its unit's hub: a HUB route, or a MIXED unit's hub lanes
    bool junction;     // enters its nozzle's hub box with the other routes there
    int32_t run_y = 0; // height of its run across to end_x
    // Where its run leaves its first drop and meets its last: the run rises from run_y toward
    // its start by the tilt it is given (tilt_runs); both run_y while level.
    int32_t run_from_y = 0, run_to_y = 0;
    // The router's trace from its start to its end, when the router laid the routes out.
    tuberoute::Trace trace;
    bool routed = false;
};

// A PARALLEL or MIXED unit: one route per tool.
bool per_tool_unit(const SystemPathData& data, int u) {
    return data.unit_topology[u] == kTopoParallel || data.unit_topology[u] == kTopoMixed;
}

// Where a tube meets a head in the multi-tool row and on the single nozzle: the
// head's filament inlet, which an art head sets from its own entry point.
int32_t tool_row_inlet_y(const SystemPathData& data, const SysLayout& L) {
    return helix::ui::toolhead_inlet(data.toolhead_style, L.tools_y, small_tool_scale(data)).y;
}
int32_t single_nozzle_inlet_y(const SystemPathData& data, const SysLayout& L) {
    return helix::ui::toolhead_inlet(data.toolhead_style, L.nozzle_y, data.extruder_scale).y;
}

// The hub box row: junction boxes stand just above their nozzles, a short
// outlet apart, leaving the run above them to the routes. A lone route's
// buffer box sits in the same row, and its hub sensor's band on the line, in
// the row or just above that buffer.
// The row grows upward to fit a line of label text, as long as the stems keep
// room for two bends above it.
struct HubRowYs {
    int32_t center, height, top, bottom;
    bool labeled;
};
HubRowYs hub_row_ys(const SystemPathData& data, const SysLayout& L) {
    const int32_t nozzle_top = tool_row_inlet_y(data, L);
    const int32_t outlet = 2 * data.tube_gauge;
    const int32_t bottom = nozzle_top - outlet;
    bool labeled = true;
    const int32_t h = labeled_box_h(data, L.hub_h * 2 / 3,
                                    bottom - L.entry_y - 2 * (int32_t)kFanFillet, &labeled);
    const int32_t y = bottom - h / 2;
    return {y, h, y - h / 2, y + h / 2, labeled};
}

// PARALLEL / MIXED unit: one route per unique tool position. For MIXED,
// tool_count already reflects unique nozzles (not lanes), so hub lanes sharing a
// mapped_tool produce a single route: the last tool's, which carries the hub.
int collect_parallel_mixed_routes(const SystemPathData& data, const SysLayout& L, int i,
                                  GlobalRoute* routes, int n) {
    int32_t unit_x = unit_stem_x(data, L, i);
    int tool_count = data.unit_tool_count[i];
    int first_tool = data.unit_first_tool[i];

    // Fan the lane start points one tube pitch apart, centered under the card:
    // close enough to read as leaving their unit, never so close they touch.
    const int32_t spread = tool_count > 1 ? (tool_count - 1) * lane_pitch(data) : 0;
    auto start_x_of = [&](int t) {
        return tool_count > 1 ? unit_x - spread / 2 + (spread * t) / (tool_count - 1) : unit_x;
    };
    const bool mixed = data.unit_topology[i] == kTopoMixed && tool_count > 1;
    for (int t = 0; t < tool_count && (first_tool + t) < data.total_tools; ++t) {
        if (n >= SystemPathData::kMaxTools)
            break;
        int tool_idx = first_tool + t;
        int32_t tool_x = calc_tool_x(tool_idx, data.total_tools, L.x_off, L.width);
        int32_t start_x = start_x_of(t);
        int32_t dist = start_x > tool_x ? (start_x - tool_x) : (tool_x - start_x);
        routes[n++] = {i,         tool_idx, start_x,
                       L.entry_y, tool_x,   L.tools_y,
                       dist,      false,    mixed && t == tool_count - 1,
                       false};
    }
    return n;
}

// HUB unit: one route from the unit toward its first tool.
int collect_hub_route(const SystemPathData& data, const SysLayout& L, int i, GlobalRoute* routes,
                      int n) {
    int tool_count = data.unit_tool_count[i];
    int first_tool = data.unit_first_tool[i];
    if (tool_count <= 0 || first_tool >= data.total_tools || n >= SystemPathData::kMaxTools)
        return n;
    const int32_t unit_x = unit_stem_x(data, L, i);
    const int32_t tool_x = calc_tool_x(first_tool, data.total_tools, L.x_off, L.width);
    const int32_t dist = unit_x > tool_x ? (unit_x - tool_x) : (tool_x - unit_x);
    routes[n++] = {i, first_tool, unit_x, L.entry_y, tool_x, tool_x, dist, true, true, false};
    return n;
}

// Clear space between neighboring hub boxes. The row solver and the boxes'
// own widths must agree on it, or a row that already fits gets moved.
int32_t hub_box_gap(const SystemPathData& data) {
    return LV_MAX(4, data.tube_gauge);
}

// The width that fits @p lanes tubes entering a box top side by side.
int32_t junction_lanes_w(const SystemPathData& data, int lanes) {
    return 2 * (int32_t)kFanEntryMargin + lanes * lane_pitch(data);
}

// The box at each junction, centered on its nozzle, and the buffer box on each
// lone route that has one: half the run from the merge height to the nozzle,
// no wider than the gap between two neighboring toolheads leaves.
void place_nozzle_boxes(const SystemPathData& data, const SysLayout& L, const NozzleFeed* feeds,
                        int tools, OverviewBoxes& boxes) {
    const HubRowYs ys = hub_row_ys(data, L);
    int32_t buffer_w = data.hub_width * 2 / 3;
    if (data.total_tools > 1) {
        const int32_t tool_pitch = calc_tool_x(1, data.total_tools, L.x_off, L.width) -
                                   calc_tool_x(0, data.total_tools, L.x_off, L.width);
        buffer_w = LV_MIN(buffer_w, tool_pitch - hub_box_gap(data));
    }
    for (int t = 0; t < tools; ++t) {
        const NozzleFeed& f = feeds[t];
        if (f.routes == 0)
            continue;
        HubInfo& h = boxes.hubs[t];
        const int32_t tool_x = calc_tool_x(t, data.total_tools, L.x_off, L.width);
        h = {tool_x, tool_x, ys.center, 0, ys.height, data.color_hub_bg, f.junction};
        h.labeled = ys.labeled;
        h.buffer_unit = f.buffer ? f.buffer_unit : -1;
        if (f.junction) {
            h.buffer_in_hub = f.buffer;
            h.mini_hub_w = LV_MAX(junction_label_w(data, ys.height, f.buffer),
                                  junction_lanes_w(data, f.routes));
        } else if (f.buffer) {
            h.buffer_h = ys.height;
            h.buffer_w = LV_MAX(1, buffer_w);
            h.buffer_y = ys.center;
        }
    }
}

// ---- Junction row ----------------------------------------------------------
// Every junction box wants to sit on its toolhead, but toolheads can sit closer
// together than a box is wide. The row is solved as one problem: boxes keep
// their order, stay inside the canvas, keep a gap from each other and stay
// clear of the vertical runs of routes that drop to their nozzles alone.

constexpr int kHubRowMax = SystemPathData::kMaxTools;

struct HubRow {
    int tool[kHubRowMax];
    int32_t pref[kHubRowMax]; // preferred center
    int32_t w[kHubRowMax];    // box width
    int32_t x[kHubRowMax];    // solved center
    int n = 0;
};

// Center-to-center distance that leaves `gap` between boxes i-1 and i.
int32_t row_pitch(const HubRow& row, int i, int32_t gap) {
    return row.w[i - 1] / 2 + row.w[i] / 2 + gap;
}

struct HubRowSpace {
    int32_t left, right; // box edges must stay inside [left, right]
    int32_t gap;         // clear space between neighbors
    // Each drop and the half width it is drawn at: a haloed tube, or the
    // sensor band across it when it carries one.
    int32_t obstacles[SystemPathData::kMaxTools];
    int32_t obstacle_half[SystemPathData::kMaxTools];
    int obstacle_count = 0;
};

// Nearest box centers on either side of an obstacle that clear it.
bool hits_obstacle(const HubRowSpace& sp, int32_t x, int32_t w, int32_t* clear_right,
                   int32_t* clear_left) {
    for (int k = 0; k < sp.obstacle_count; ++k) {
        const int32_t reach = w / 2 + sp.obstacle_half[k];
        if (LV_ABS(x - sp.obstacles[k]) < reach) {
            *clear_right = sp.obstacles[k] + reach;
            *clear_left = sp.obstacles[k] - reach;
            return true;
        }
    }
    return false;
}

// Least-squares placement of ordered boxes at their minimum pitch: each run of
// colliding boxes moves together about its members' mean preference.
void spread_row(HubRow& row, int32_t gap) {
    struct Cluster {
        int first, count;
        int64_t sum;
        int64_t mean;
    };
    Cluster stack[kHubRowMax];
    int top = 0;
    int64_t offset[kHubRowMax]; // box i's distance from box 0 when packed tight
    auto mean_of = [](const Cluster& c) { return (int64_t)std::llround((double)c.sum / c.count); };
    for (int i = 0; i < row.n; ++i) {
        offset[i] = i == 0 ? 0 : offset[i - 1] + row_pitch(row, i, gap);
        stack[top] = {i, 1, (int64_t)row.pref[i] - offset[i], 0};
        stack[top].mean = mean_of(stack[top]);
        ++top;
        while (top > 1 && stack[top - 1].mean < stack[top - 2].mean) {
            Cluster& a = stack[top - 2];
            a.count += stack[top - 1].count;
            a.sum += stack[top - 1].sum;
            a.mean = mean_of(a);
            --top;
        }
    }
    for (int c = 0; c < top; ++c)
        for (int k = 0; k < stack[c].count; ++k) {
            const int i = stack[c].first + k;
            row.x[i] = (int32_t)(stack[c].mean + offset[i]);
        }
}

// Spread, then settle against the canvas edges and the obstacles. True when the
// result is collision-free.
bool solve_row(HubRow& row, const HubRowSpace& sp) {
    spread_row(row, sp.gap);
    int32_t cr, cl;
    for (int i = 0; i < row.n; ++i) {
        const int32_t w = row.w[i];
        int32_t lo = sp.left + w / 2;
        if (i > 0)
            lo = LV_MAX(lo, row.x[i - 1] + row_pitch(row, i, sp.gap));
        row.x[i] = LV_MAX(row.x[i], lo);
        while (hits_obstacle(sp, row.x[i], w, &cr, &cl))
            row.x[i] = cr;
    }
    for (int i = row.n - 1; i >= 0; --i) {
        const int32_t w = row.w[i];
        int32_t hi = sp.right - w / 2;
        if (i + 1 < row.n)
            hi = LV_MIN(hi, row.x[i + 1] - row_pitch(row, i + 1, sp.gap));
        row.x[i] = LV_MIN(row.x[i], hi);
        while (hits_obstacle(sp, row.x[i], w, &cr, &cl))
            row.x[i] = cl;
    }
    for (int i = 0; i < row.n; ++i) {
        const int32_t w = row.w[i];
        if (row.x[i] - w / 2 < sp.left || row.x[i] + w / 2 > sp.right)
            return false;
        if (i > 0 && row.x[i] - row.x[i - 1] < row_pitch(row, i, sp.gap))
            return false;
        if (hits_obstacle(sp, row.x[i], w, &cr, &cl))
            return false;
    }
    return true;
}

// Places the junction boxes (preferred centers are their nozzles). A row that
// fits keeps its preferred positions and full widths; one that does not
// narrows the boxes, never below the width their entering lanes need, and a
// box narrowed below its label's width says "H". Lone-route buffers keep their
// own width, which already fits between two toolheads.
void resolve_hub_row(const SystemPathData& data, const SysLayout& L, const NozzleFeed* feeds,
                     const GlobalRoute* routes, int n, OverviewBoxes& boxes) {
    HubRow row;
    int32_t full_w[kHubRowMax];
    int32_t min_w[kHubRowMax];
    int32_t label_w[kHubRowMax];
    for (int t = 0; t < SystemPathData::kMaxTools; ++t) {
        if (!boxes.hubs[t].valid)
            continue;
        row.tool[row.n] = t;
        row.pref[row.n] = boxes.hubs[t].hub_x;
        full_w[row.n] = boxes.hubs[t].mini_hub_w;
        min_w[row.n] = LV_MIN(full_w[row.n], junction_lanes_w(data, feeds[t].routes));
        label_w[row.n] =
            junction_label_w(data, boxes.hubs[t].mini_hub_h, boxes.hubs[t].buffer_in_hub);
        ++row.n;
    }
    if (row.n == 0)
        return;

    HubRowSpace sp;
    sp.left = L.x_off;
    sp.right = L.x_off + L.width;
    sp.gap = hub_box_gap(data);
    // Rounded up: a box must clear the drawn edge, not the pixel before it.
    const int32_t tube_half = (data.tube_gauge + kHaloWidthExtra + 1) / 2;
    for (int r = 0; r < n && sp.obstacle_count < SystemPathData::kMaxTools; ++r) {
        if (routes[r].junction)
            continue;
        sp.obstacles[sp.obstacle_count] = routes[r].end_x;
        sp.obstacle_half[sp.obstacle_count++] = tube_half + (sp.gap + 1) / 2;
    }

    // Width tiers: full, then narrower caps; a box keeps room for its lanes.
    const int32_t tier_cap[] = {INT32_MAX, data.hub_width / 2, data.hub_width * 2 / 5};
    bool fits = false;
    for (int k = 0; k < 3 && !fits; ++k) {
        for (int i = 0; i < row.n; ++i)
            row.w[i] = LV_MAX(min_w[i], LV_MIN(full_w[i], tier_cap[k]));
        fits = solve_row(row, sp);
    }
    if (!fits) {
        // Even the narrowest boxes do not fit beside the drops: give every box
        // an equal share of the canvas and let them cover the drops. Below
        // (n - 1) * gap of canvas they still overlap.
        const int32_t share = (sp.right - sp.left - (row.n - 1) * sp.gap) / row.n;
        for (int i = 0; i < row.n; ++i)
            row.w[i] = LV_MAX(1, LV_MIN(row.w[i], share));
        sp.obstacle_count = 0;
        solve_row(row, sp);
    }

    for (int i = 0; i < row.n; ++i) {
        HubInfo& h = boxes.hubs[row.tool[i]];
        h.hub_x = row.x[i];
        h.mini_hub_w = row.w[i];
        h.short_label = row.w[i] < label_w[i];
    }
}

// Draw order: left to right by the nozzle each route ends at.
void sort_routes(GlobalRoute* routes, int n) {
    std::stable_sort(routes, routes + n,
                     [](const GlobalRoute& a, const GlobalRoute& b) { return a.end_x < b.end_x; });
}

// Clear distance two tube centerlines keep: a haloed tube's width.
int32_t route_gap(const SystemPathData& data) {
    return data.tube_gauge + kHaloWidthExtra;
}

// Cable-harness route: down from (sx, sy), across at run_y, down into
// (ex, ey). Both bends are arcs of one radius, kFanFillet or as large as the
// jog and the two drops leave room for, so a short jog reads as a smooth S
// rather than a corner. A drop too short for any bend runs straight across.
void append_harness(Route& r, int32_t sx, int32_t sy, int32_t ex, int32_t ey, int32_t run_y,
                    fpath::SpanStyle s) {
    pg::FilamentPath path;
    const float dx = (float)(ex - sx);
    if (ey - sy < 4 || dx == 0.0f) {
        path.add_line((float)sx, (float)sy, (float)ex, (float)ey);
        route_append(r, path, s);
        return;
    }
    run_y = clamp_span(sy + 2, run_y, ey - 2);
    const float dir = dx > 0 ? 1.0f : -1.0f;
    const float rad =
        std::min({kFanFillet, std::fabs(dx) / 2.0f, (float)(run_y - sy), (float)(ey - run_y)});
    constexpr float kQuarter = 1.5707963f;
    const float y = (float)run_y;
    path.add_line((float)sx, (float)sy, (float)sx, y - rad);
    path.add_arc((float)sx + dir * rad, y - rad, rad, dir > 0 ? 2 * kQuarter : 0.0f,
                 dir > 0 ? -kQuarter : kQuarter);
    path.add_line((float)sx + dir * rad, y, (float)ex - dir * rad, y);
    path.add_arc((float)ex - dir * rad, y + rad, rad, -kQuarter, dir > 0 ? kQuarter : -kQuarter);
    path.add_line((float)ex, y + rad, (float)ex, (float)ey);
    route_append(r, path, s);
}

// A run tilted down toward its end: its first drop, the run as one diagonal, its last drop,
// the bends rounded. A run with no rise left is append_harness's level run.
void append_tilted(Route& r, const GlobalRoute& g, fpath::SpanStyle s) {
    if (g.run_from_y == g.start_y && g.run_to_y == g.end_y) {
        // A straight line from under its unit to its end: no drops, no bends.
        append_line(r, (float)g.start_x, (float)g.start_y, (float)g.end_x, (float)g.end_y, s);
        return;
    }
    if (g.run_to_y <= g.run_from_y) {
        append_harness(r, g.start_x, g.start_y, g.end_x, g.end_y, g.run_y, s);
        return;
    }
    const pg::PathPoint pts[4] = {{(float)g.start_x, (float)g.start_y},
                                  {(float)g.start_x, (float)g.run_from_y},
                                  {(float)g.end_x, (float)g.run_to_y},
                                  {(float)g.end_x, (float)g.end_y}};
    pg::FilamentPath path;
    pg::route_polyline_filleted(path, pts, 4, kFanFillet);
    route_append(r, path, s);
}

// A route that runs across: its two drops are further apart than a tube.
bool runs_across(const SystemPathData& data, const GlobalRoute& r) {
    return LV_ABS(r.end_x - r.start_x) > data.tube_gauge;
}

// Run heights. Route i must turn above route j when i's first drop would pass
// through j's run, and below it when i's last drop would; a pair that must each
// be above the other has swapped ends and crosses whatever the heights, so that
// constraint is dropped. Each route runs as low as it may: above its own end
// with room for the bend, above the hub row when it passes over a hub box or a
// lone hub's drop, and one step above every route it must clear. The step is a
// tube pitch, shrunk only when the canvas is too short to stack the runs.
void assign_run_heights(const SystemPathData& data, const SysLayout& L, const OverviewBoxes& boxes,
                        GlobalRoute* routes, int n) {
    constexpr int kN = SystemPathData::kMaxTools;
    const int32_t gap = route_gap(data);
    const int32_t bend = (int32_t)kFanFillet;
    bool above[kN][kN] = {}; // above[i][j]: i's run must be higher than j's
    auto in_span = [&](int32_t lo, int32_t hi, const GlobalRoute& r) {
        return runs_across(data, r) && hi > LV_MIN(r.start_x, r.end_x) - gap &&
               lo < LV_MAX(r.start_x, r.end_x) + gap;
    };
    auto in_run = [&](int32_t x, const GlobalRoute& r) { return in_span(x, x, r); };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            if (i == j)
                continue;
            if (in_run(routes[i].start_x, routes[j]))
                above[i][j] = true;
            if (in_run(routes[i].end_x, routes[j]))
                above[j][i] = true;
        }
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (above[i][j] && above[j][i])
                above[i][j] = above[j][i] = false;

    // The lowest each run may go on its own.
    const HubRowYs ys = hub_row_ys(data, L);
    int32_t cap[kN];
    for (int i = 0; i < n; ++i) {
        const GlobalRoute& r = routes[i];
        cap[i] = r.end_y - bend;
        bool over_row = false;
        for (const HubInfo& h : boxes.hubs)
            over_row = over_row || (h.valid && in_span(h.hub_x - h.mini_hub_w / 2,
                                                       h.hub_x + h.mini_hub_w / 2, r));
        for (int j = 0; j < n; ++j)
            over_row = over_row || (j != i && !routes[j].junction && routes[j].hub_lane &&
                                    in_run(routes[j].end_x, r));
        if (over_row)
            cap[i] = LV_MIN(cap[i], ys.top - bend);
    }

    // Lay the runs out bottom up: each as low as its cap and the routes it
    // clears allow. A cycle left after dropping the swapped pairs is forced too;
    // its closing edge is ignored.
    const int32_t highest = L.entry_y + bend;
    auto layout = [&](int32_t step) {
        int state[kN] = {};
        auto visit = [&](auto&& self, int i) -> int32_t {
            if (state[i] == 2)
                return routes[i].run_y;
            if (state[i] == 1)
                return INT32_MAX / 2;
            state[i] = 1;
            int32_t y = cap[i];
            for (int j = 0; j < n; ++j)
                if (above[i][j])
                    y = LV_MIN(y, self(self, j) - step);
            state[i] = 2;
            routes[i].run_y = y;
            return y;
        };
        int32_t top = INT32_MAX;
        for (int i = 0; i < n; ++i)
            top = LV_MIN(top, visit(visit, i));
        return top;
    };
    for (int32_t step = lane_pitch(data); step >= 1; --step)
        if (layout(step) >= highest)
            return;
}

namespace tilt {
struct Seg {
    float x0, y0, x1, y1;
};

float seg_point_d2(const Seg& s, float px, float py) {
    const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - s.x0) * dx + (py - s.y0) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = s.x0 + t * dx - px, ey = s.y0 + t * dy - py;
    return ex * ex + ey * ey;
}

bool segs_cross(const Seg& a, const Seg& b) {
    auto side = [](const Seg& s, float x, float y) {
        return (s.x1 - s.x0) * (y - s.y0) - (s.y1 - s.y0) * (x - s.x0);
    };
    const float d1 = side(a, b.x0, b.y0), d2 = side(a, b.x1, b.y1);
    const float d3 = side(b, a.x0, a.y0), d4 = side(b, a.x1, a.y1);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// The closest two segments come, squared.
float seg_seg_d2(const Seg& a, const Seg& b) {
    if (segs_cross(a, b))
        return 0.0f;
    return std::min({seg_point_d2(a, b.x0, b.y0), seg_point_d2(a, b.x1, b.y1),
                     seg_point_d2(b, a.x0, a.y0), seg_point_d2(b, a.x1, a.y1)});
}

// A route's centerline as drop, run and drop.
int route_segs(const GlobalRoute& g, Seg* out) {
    const float sx = (float)g.start_x, ex = (float)g.end_x;
    out[0] = {sx, (float)g.start_y, sx, (float)g.run_from_y};
    out[1] = {sx, (float)g.run_from_y, ex, (float)g.run_to_y};
    out[2] = {ex, (float)g.run_to_y, ex, (float)g.end_y};
    return 3;
}

// Whether every pair of routes keeps @p gap2 (squared) between its centerlines, or as much as
// it kept level (@p level_d2) where the level layout already came closer.
bool keeps_clear(const GlobalRoute* routes, int n, const float* level_d2, float gap2) {
    for (int i = 0; i < n; ++i) {
        Seg a[3];
        route_segs(routes[i], a);
        for (int j = i + 1; j < n; ++j) {
            Seg b[3];
            route_segs(routes[j], b);
            float d2 = std::numeric_limits<float>::max();
            for (const Seg& sa : a)
                for (const Seg& sb : b)
                    d2 = std::min(d2, seg_seg_d2(sa, sb));
            if (d2 < std::min(gap2, level_d2[i * SystemPathData::kMaxTools + j]) - 0.5f)
                return false;
        }
    }
    return true;
}

void set_tilt(const SystemPathData& data, GlobalRoute* routes, int n, float slope) {
    for (int i = 0; i < n; ++i) {
        GlobalRoute& g = routes[i];
        g.run_from_y = g.run_to_y = g.run_y;
        if (!runs_across(data, g))
            continue;
        // The run keeps its end at run_y, the lowest its layout allows, and rises toward its
        // start, kept clear of the first drop's bend.
        const int32_t rise = (int32_t)(slope * (float)LV_ABS(g.end_x - g.start_x));
        const int32_t room_up = g.run_y - g.start_y - 2 * (int32_t)kFanFillet;
        g.run_from_y = g.run_y - LV_MAX(0, LV_MIN(rise, room_up));
    }
}

// Whether @p g can run as one straight line from under its unit: its end takes no painted
// line of its own after it (a junction box, or the nozzle itself). A lone hub route keeps its
// drop to its band and nozzle, so it keeps the leaning run.
bool can_run_straight(const SystemPathData& data, const GlobalRoute& g) {
    return runs_across(data, g) && (g.junction || !g.hub_lane);
}
} // namespace tilt

// Each run across leans down toward its end at the steepest common slope that keeps every
// pair of routes as far apart as their level runs were (or a haloed tube's width); then each
// route that can becomes one straight line from under its unit to its end wherever that still
// holds. A rig with no room keeps its level runs.
void tilt_runs(const SystemPathData& data, GlobalRoute* routes, int n) {
    constexpr int kN = SystemPathData::kMaxTools;
    tilt::set_tilt(data, routes, n, 0.0f);
    float level_d2[kN * kN];
    for (int i = 0; i < n; ++i) {
        tilt::Seg a[3];
        tilt::route_segs(routes[i], a);
        for (int j = i + 1; j < n; ++j) {
            tilt::Seg b[3];
            tilt::route_segs(routes[j], b);
            float d2 = std::numeric_limits<float>::max();
            for (const tilt::Seg& sa : a)
                for (const tilt::Seg& sb : b)
                    d2 = std::min(d2, tilt::seg_seg_d2(sa, sb));
            level_d2[i * kN + j] = d2;
        }
    }
    const float gap = (float)route_gap(data);
    bool leaned = false;
    for (float slope : {1.0f, 0.75f, 0.55f, 0.4f, 0.28f, 0.18f}) {
        tilt::set_tilt(data, routes, n, slope);
        if (tilt::keeps_clear(routes, n, level_d2, gap * gap)) {
            leaned = true;
            break;
        }
    }
    if (!leaned)
        tilt::set_tilt(data, routes, n, 0.0f);
    // Then each route that can runs as one straight line, wherever that keeps every pair a
    // full haloed tube's width apart: a straight line takes no slack from the level layout.
    float strict_d2[kN * kN];
    for (float& v : strict_d2)
        v = std::numeric_limits<float>::max();
    for (int i = 0; i < n; ++i) {
        GlobalRoute& g = routes[i];
        if (!tilt::can_run_straight(data, g))
            continue;
        const int32_t from = g.run_from_y, to = g.run_to_y;
        g.run_from_y = g.start_y;
        g.run_to_y = g.end_y;
        if (!tilt::keeps_clear(routes, n, strict_d2, gap * gap)) {
            g.run_from_y = from;
            g.run_to_y = to;
        }
    }
}

// One route's state. A HUB route is its unit's; a tool route carries filament
// only when it is the active tool's, and a MIXED unit's hub lanes also when its
// hub sensor reads filament.
Lane route_lane(const SystemPathData& data, const GlobalRoute& route) {
    if (route.is_hub)
        return unit_lane(data, route.unit_idx);
    const bool tool_on = route.unit_idx == data.active_unit && route.tool_idx == data.active_tool;
    Lane lane = unit_lane(data, route.unit_idx);
    if (!tool_on) {
        const bool hub_reads = route.hub_lane && data.unit_has_hub_sensor[route.unit_idx] &&
                               data.unit_hub_triggered[route.unit_idx];
        lane = unit_lane(data, -1);
        if (hub_reads) {
            lane.reached = PathSegment::OUTPUT;
            if (data.unit_lane_segment[route.unit_idx] != PathSegment::NONE)
                lane.color = lv_color_hex(data.unit_lane_color[route.unit_idx]);
        }
    }
    lane.on = tool_on;
    return lane;
}

// The chip over the @p tubes parked tubes at the edge on @p side: how many
// units are past that edge and where their tubes stand.
void mark_parked(const SystemPathData& data, const SysLayout& L, int side, int tubes,
                 OverviewBoxes& boxes) {
    if (tubes <= 0)
        return;
    ParkedChip& chip = boxes.parked[side > 0];
    chip.present = true;
    chip.units = 0;
    for (int u = 0; u < data.unit_count && u < SystemPathData::kMaxUnits; ++u)
        chip.units += !data.unit_absent[u] && parked_side(data, L, u) == side;
    const int32_t a = parked_stem_x(data, L, side, 0);
    const int32_t b = parked_stem_x(data, L, side, tubes - 1);
    chip.stem_lo = LV_MIN(a, b);
    chip.stem_hi = LV_MAX(a, b);
}

// Units scrolled off one edge send one tube per destination between them, from
// that edge: the active one's when it is among them. A unit's lanes are one tube
// on the overview; the units past an edge are one tube more. Kept tubes stand a
// pitch apart from the edge in, the one bound for the farthest nozzle outermost,
// so their runs never cross.
int merge_parked_routes(const SystemPathData& data, const SysLayout& L, GlobalRoute* routes, int n,
                        OverviewBoxes& boxes) {
    int side[SystemPathData::kMaxTools];
    int kept = 0;
    for (int r = 0; r < n; ++r) {
        const int s = parked_side(data, L, routes[r].unit_idx);
        int same = -1;
        for (int k = 0; k < kept && s != 0; ++k)
            if (side[k] == s && routes[k].tool_idx == routes[r].tool_idx &&
                routes[k].hub_lane == routes[r].hub_lane)
                same = k;
        if (same < 0) {
            side[kept] = s;
            routes[kept++] = routes[r];
        } else if (route_lane(data, routes[r]).on) {
            routes[same] = routes[r];
        }
    }
    for (int s : {-1, 1}) {
        int members[SystemPathData::kMaxTools];
        int count = 0;
        for (int k = 0; k < kept; ++k)
            if (side[k] == s)
                members[count++] = k;
        std::sort(members, members + count, [&](int a, int b) {
            return s > 0 ? routes[a].end_x > routes[b].end_x : routes[a].end_x < routes[b].end_x;
        });
        for (int m = 0; m < count; ++m) {
            GlobalRoute& g = routes[members[m]];
            g.start_x = parked_stem_x(data, L, s, m);
            g.dist = LV_ABS(g.start_x - g.end_x);
        }
        mark_parked(data, L, s, count, boxes);
    }
    return kept;
}

// Each junction's routes enter its box top left to right, in the order of
// their stems, one tube pitch apart. The group of slots sits as near the
// routes' mean stem as the box's entry margins allow, so routes arriving from
// one side enter on that side and their runs stay short.
void slot_into_junctions(const SystemPathData& data, const OverviewBoxes& boxes,
                         GlobalRoute* routes, int n) {
    for (int t = 0; t < SystemPathData::kMaxTools; ++t) {
        const HubInfo& hub = boxes.hubs[t];
        if (!hub.valid)
            continue;
        int members[SystemPathData::kMaxTools];
        int count = 0;
        int64_t sum = 0;
        for (int r = 0; r < n; ++r)
            if (routes[r].junction && routes[r].tool_idx == t) {
                members[count++] = r;
                sum += routes[r].start_x;
            }
        if (count == 0)
            continue;
        std::sort(members, members + count,
                  [&](int a, int b) { return routes[a].start_x < routes[b].start_x; });
        const int32_t inner = LV_MAX(0, hub.mini_hub_w - 2 * (int32_t)kFanEntryMargin);
        const int32_t pitch = count > 1 ? LV_MIN(lane_pitch(data), inner / (count - 1)) : 0;
        const int32_t span = pitch * (count - 1);
        const int32_t lo = hub.hub_x - inner / 2 + span / 2;
        const int32_t hi = hub.hub_x + inner / 2 - span / 2;
        const int32_t mid = clamp_span(lo, (int32_t)(sum / count), hi);
        for (int m = 0; m < count; ++m) {
            GlobalRoute& r = routes[members[m]];
            r.end_x = mid - span / 2 + m * pitch;
            r.end_y = hub.mini_hub_y - hub.mini_hub_h / 2;
            r.dist = LV_ABS(r.start_x - r.end_x);
        }
    }
}

// The route at each junction that carries the outlet to the nozzle: the
// active one, else the one loaded furthest, else the first.
void junction_owners(const SystemPathData& data, const GlobalRoute* routes, int n, int* owner) {
    for (int t = 0; t < SystemPathData::kMaxTools; ++t)
        owner[t] = -1;
    for (int r = 0; r < n; ++r) {
        if (!routes[r].junction)
            continue;
        int& o = owner[routes[r].tool_idx];
        if (o < 0) {
            o = r;
            continue;
        }
        const Lane cur = route_lane(data, routes[o]);
        const Lane cand = route_lane(data, routes[r]);
        if (cur.on)
            continue;
        if (cand.on || cand.reached > cur.reached)
            o = r;
    }
}

// Junction outlet: through the box (unpainted) to its center and straight down
// out of its bottom edge; then on to the nozzle, angling in when the row moved
// the box.
void append_junction_outlet(Route& r, const Lane& lane, const HubInfo& hub, float from_x,
                            float nozzle_top) {
    const float top = (float)(hub.mini_hub_y - hub.mini_hub_h / 2);
    const float out_y = (float)(hub.mini_hub_y + hub.mini_hub_h / 2);
    const fpath::SpanStyle inside = unpainted(lane.style(PathSegment::HUB));
    append_line(r, from_x, top, (float)hub.hub_x, (float)hub.mini_hub_y, inside);
    append_line(r, (float)hub.hub_x, (float)hub.mini_hub_y, (float)hub.hub_x, out_y, inside);
    if (hub.hub_x == hub.tool_x) {
        append_line(r, (float)hub.hub_x, out_y, (float)hub.tool_x, nozzle_top,
                    lane.style(PathSegment::OUTPUT));
        return;
    }
    pg::MergeLaneIn out_in{(float)hub.hub_x, out_y};
    pg::MergeLaneOut outlet;
    merge_fan(&out_in, 1, hub.tool_x, (int32_t)nozzle_top, 0, &outlet);
    append_fan(r, outlet, lane.style(PathSegment::OUTPUT));
}

// A lone route's last stretch from above its buffer box (@p from_y) to the
// nozzle: the output tube, past the box drawn over it.
void append_lone_outlet(Route& r, const Lane& lane, float x, float from_y, float nozzle_top) {
    append_line(r, x, from_y, x, nozzle_top, lane.style(PathSegment::OUTPUT));
}

// Lays every route out with the tube router: each one a drop, one jog and a drop,
// in source order, never crossing and a haloed tube apart. A PARALLEL or MIXED
// unit's routes are one bus, so they jog as a ribbon; every other unit's tube is
// a bus of its own. Returns false, leaving the routes untouched, when the router
// cannot fit them (ends out of order, too little height or clearance): the
// caller lays them out with run heights instead. Sorts @p routes by start.
bool route_with_router(const SystemPathData& data, GlobalRoute* routes, int n) {
    if (n <= 0)
        return false;
    std::stable_sort(routes, routes + n, [](const GlobalRoute& a, const GlobalRoute& b) {
        return a.start_x < b.start_x;
    });
    pg::PathPoint sources[SystemPathData::kMaxTools];
    pg::PathPoint sinks[SystemPathData::kMaxTools];
    tuberoute::Bus buses[SystemPathData::kMaxTools];
    int bus_count = 0;
    for (int r = 0; r < n; ++r) {
        sources[r] = {(float)routes[r].start_x, (float)routes[r].start_y};
        sinks[r] = {(float)routes[r].end_x, (float)routes[r].end_y};
        const bool joins = r > 0 && !routes[r].is_hub && !routes[r - 1].is_hub &&
                           routes[r].unit_idx == routes[r - 1].unit_idx;
        if (joins) {
            buses[bus_count - 1].count++;
            continue;
        }
        buses[bus_count++] = {&sources[r], &sinks[r], 1};
    }
    tuberoute::RouterConfig cfg;
    cfg.clearance = (float)route_gap(data);
    cfg.bus_pitch = (float)lane_pitch(data);
    cfg.group_gap = (float)(2 * lane_pitch(data));
    cfg.fillet_r = kFanFillet;
    // The steepest jog that fits the height, in steps fine enough that a tight
    // rig leans as far as its height allows; a level jog only when no lean fits.
    tuberoute::Trace traces[SystemPathData::kMaxTools];
    bool fits = false;
    cfg.allow_horizontal = false;
    for (float slope : {1.0f, 0.75f, 0.6f, 0.5f, 0.4f, 0.35f, 0.3f, 0.26f, 0.22f, 0.18f, 0.0f}) {
        cfg.slope = slope > 0.0f ? slope : 1.0f;
        cfg.allow_horizontal = slope == 0.0f;
        if (tuberoute::plan_routes(buses, bus_count, cfg, traces, n) ==
            tuberoute::RouteStatus::Ok) {
            fits = true;
            break;
        }
    }
    if (!fits)
        return false;
    for (int r = 0; r < n; ++r) {
        routes[r].trace = traces[r];
        routes[r].routed = true;
    }
    return true;
}

void plan_multi_tool(const SystemPathData& data, const SysLayout& L, PathPlan& plan,
                     OverviewBoxes& boxes) {
    GlobalRoute routes[SystemPathData::kMaxTools];
    int n = 0;
    for (int i = 0; i < data.unit_count && i < SystemPathData::kMaxUnits; i++) {
        if (data.unit_absent[i])
            continue;
        if (per_tool_unit(data, i))
            n = collect_parallel_mixed_routes(data, L, i, routes, n);
        else
            n = collect_hub_route(data, L, i, routes, n);
    }
    n = merge_parked_routes(data, L, routes, n, boxes);

    const int tools = LV_MIN(data.total_tools, SystemPathData::kMaxTools);
    NozzleFeed feeds[SystemPathData::kMaxTools];
    for (int t = 0; t < tools; ++t) {
        // A box stands only where two or more tubes visibly meet, as wide as the
        // tubes that enter it once parked units share theirs. Units that all
        // share one parked tube merge off-screen.
        feeds[t] = nozzle_feed(data, t);
        int tubes = 0;
        for (int r = 0; r < n; ++r)
            tubes += routes[r].tool_idx == t;
        if (feeds[t].routes > 0)
            feeds[t].routes = LV_MAX(1, tubes);
        feeds[t].junction = feeds[t].junction && tubes >= 2;
    }
    const HubRowYs ys = hub_row_ys(data, L);
    const int32_t nozzle_top = tool_row_inlet_y(data, L);
    for (int r = 0; r < n; ++r) {
        GlobalRoute& route = routes[r];
        const NozzleFeed& feed = feeds[route.tool_idx];
        route.junction = feed.junction;
        // A lone route with a buffer stops its run just above the buffer box,
        // which its outlet then passes through; every other lone route runs on
        // to its nozzle.
        if (route.junction)
            continue;
        route.end_y = route.hub_lane && feed.buffer ? ys.top - hub_box_gap(data) : nozzle_top;
    }
    place_nozzle_boxes(data, L, feeds, tools, boxes);
    resolve_hub_row(data, L, feeds, routes, n, boxes);
    slot_into_junctions(data, boxes, routes, n);
    if (!route_with_router(data, routes, n)) {
        assign_run_heights(data, L, boxes, routes, n);
        tilt_runs(data, routes, n);
    }
    sort_routes(routes, n);
    int owner[SystemPathData::kMaxTools];
    junction_owners(data, routes, n, owner);

    for (int k = 0; k < n; ++k) {
        const GlobalRoute& route = routes[k];
        const Lane lane = route_lane(data, route);
        Route* r = try_new_route(plan);
        if (!r)
            break;

        // Down from the unit, across (leaning down) at its run height, down to its end.
        const fpath::SpanStyle s = lane.style(PathSegment::LANE);
        if (route.routed) {
            pg::FilamentPath path;
            tuberoute::trace_to_path(route.trace, kFanFillet, path);
            route_append(*r, path, s);
        } else if (!runs_across(data, route)) {
            append_line(*r, (float)route.start_x, (float)route.start_y, (float)route.start_x,
                        (float)route.end_y, s);
        } else {
            append_tilted(*r, route, s);
        }
        const float end_x = seg_end_x(*r);

        if (route.junction) {
            // The owner carries the outlet on through the box to the nozzle.
            HubInfo& hub = boxes.hubs[route.tool_idx];
            if (lane.on && data.filament_loaded)
                hub.hub_bg_color = hub_fill(data, true);
            if (owner[route.tool_idx] != k)
                continue;
            append_junction_outlet(*r, lane, hub, end_x, (float)nozzle_top);
        } else if (route.end_y < nozzle_top) {
            append_lone_outlet(*r, lane, end_x, (float)route.end_y, (float)nozzle_top);
        }
        if (lane.on)
            plan.active_route = plan.route_count - 1;
    }
}

// ----------------------------------------------------------------------------
// Single-tool (all units converge through the combiner hub on one nozzle)
// ----------------------------------------------------------------------------

struct TrunkYs {
    int32_t hub_bottom, bypass_merge, toolhead, nozzle_top;
};

// Combiner bottom → nozzle, appended to the route that owns the trunk. Ends at
// the bypass merge when an active bypass (@p bypass_owner) owns the rest.
void append_trunk(PathPlan& plan, Route& r, const Lane& lane, const SystemPathData& data,
                  const SysLayout& L, const TrunkYs& y, const Lane* bypass_owner) {
    const float cx = (float)L.center_x;
    if (data.has_bypass) {
        append_line(r, cx, (float)y.hub_bottom, cx, (float)y.bypass_merge,
                    lane.style(PathSegment::OUTPUT));
        if (bypass_owner) {
            add_band_at_end(plan, BandKind::Trunk, r, bypass_owner->band(PathSegment::OUTPUT),
                            bypass_owner->color);
            return;
        }
        add_band_at_end(plan, BandKind::Trunk, r, lane.band(PathSegment::OUTPUT), lane.color);
        append_line(r, cx, (float)y.bypass_merge, cx, (float)y.toolhead,
                    lane.style(PathSegment::TOOLHEAD));
    } else {
        append_line(r, cx, (float)y.hub_bottom, cx, (float)y.toolhead,
                    lane.style(PathSegment::OUTPUT));
    }
    if (data.has_toolhead_sensor)
        add_band_at_end(plan, BandKind::Trunk, r, lane.band(PathSegment::TOOLHEAD), lane.color);
    append_line(r, cx, (float)y.toolhead, cx, (float)y.nozzle_top, lane.style(PathSegment::NOZZLE));
}

void plan_single_tool(const SystemPathData& data, const SysLayout& L, PathPlan& plan,
                      OverviewBoxes& boxes) {
    const int32_t hub_top = L.hub_y - L.hub_h / 2;
    TrunkYs y;
    y.hub_bottom = L.hub_y + L.hub_h / 2;
    y.nozzle_top = single_nozzle_inlet_y(data, L);
    y.bypass_merge = y.hub_bottom + (L.nozzle_y - y.hub_bottom) / 3;
    y.toolhead = y.hub_bottom + (y.nozzle_top - y.hub_bottom) * 2 / 3;
    const float cx = (float)L.center_x;

    const bool bypass_owns = data.has_bypass && data.bypass_active;
    const Lane bypass_lane{data.bypass_active ? PathSegment::NOZZLE : PathSegment::NONE,
                           data.bypass_active,
                           data.bypass_active ? data.error_segment : PathSegment::NONE,
                           lv_color_hex(data.bypass_color), data.color_bg};
    const Lane* bypass_owner = bypass_owns ? &bypass_lane : nullptr;

    int units[SystemPathData::kMaxUnits];
    pg::MergeLaneIn in[SystemPathData::kMaxUnits];
    int count = 0;
    // Units scrolled off one edge share one line from that edge: the active one's
    // when it is among them.
    int parked_at[2] = {-1, -1};
    for (int i = 0; i < data.unit_count && i < SystemPathData::kMaxUnits; i++) {
        if (data.unit_absent[i])
            continue;
        const int side = parked_side(data, L, i);
        if (side != 0) {
            int& slot = parked_at[side > 0];
            if (slot >= 0) {
                if (i == data.active_unit)
                    units[slot] = i;
                continue;
            }
            slot = count;
            units[count] = i;
            in[count++] = {(float)parked_stem_x(data, L, side, 0), (float)L.merge_y};
            mark_parked(data, L, side, 1, boxes);
            continue;
        }
        units[count] = i;
        in[count++] = {(float)unit_stem_x(data, L, i), (float)L.merge_y};
    }
    pg::MergeLaneOut fan[SystemPathData::kMaxUnits];
    merge_fan(in, count, L.center_x, hub_top, data.hub_width, fan);

    // Two or more units meet in the combiner; one unit's line runs on through
    // where the combiner would stand.
    const NozzleFeed feed = nozzle_feed(data, 0);
    boxes.combiner = feed.junction;
    boxes.combiner_buffer = feed.junction && feed.buffer;
    boxes.combiner_labeled = L.hub_labeled;

    bool trunk_owned = false;
    for (int k = 0; k < count; k++) {
        const int u = units[k];
        const Lane lane = unit_lane(data, u);
        const float x = in[k].slot_x;
        Route* r = try_new_route(plan);
        if (!r)
            break;
        // One straight line from under the unit to its entry on the hub's top, with no bend:
        // the lines converge on the hub as diagonals. A unit's lanes and hub are its card, so
        // the line carries the unit's hub output state from end to end.
        const float sx = (float)L.entry_y;
        const float ex = fan[k].pts[3].x;
        const float ey = (float)hub_top;
        append_line(*r, x, sx, ex, ey, lane.style(PathSegment::OUTPUT));
        if (!feed.junction) {
            append_line(*r, fan[k].pts[3].x, (float)hub_top, cx, (float)y.hub_bottom,
                        lane.style(PathSegment::HUB));
        }
        if (!lane.on)
            continue;
        plan.active_route = plan.route_count - 1;
        trunk_owned = true;
        if (feed.junction) {
            append_line(*r, fan[k].pts[3].x, (float)hub_top, cx, (float)y.hub_bottom,
                        unpainted(lane.style(PathSegment::HUB)));
        }
        append_trunk(plan, *r, lane, data, L, y, bypass_owner);
    }

    if (!trunk_owned) {
        // Nothing loaded owns the trunk. It still shows an error at its sensors.
        const Lane idle{PathSegment::NONE, true, data.error_segment, data.color_idle,
                        data.color_bg};
        if (Route* r = try_new_route(plan)) {
            plan.trunk_route = plan.route_count - 1;
            append_trunk(plan, *r, idle, data, L, y, bypass_owner);
        }
    }

    if (data.has_bypass) {
        const BypassGeometry bg = compute_bypass_geometry(
            data, {L.x_off, L.y_off, L.x_off + L.width - 1, L.y_off + L.height - 1});
        if (Route* b = try_new_route(plan)) {
            plan.bypass_route = plan.route_count - 1;
            // Spool → merge carries the SPOOL tag: no AMS error lands on it.
            fpath::append_bypass_feed(*b, (float)bg.bypass_x, (float)y.bypass_merge, cx,
                                      bypass_lane.style(PathSegment::SPOOL));
            if (bypass_owns) {
                append_line(*b, cx, (float)y.bypass_merge, cx, (float)y.toolhead,
                            bypass_lane.style(PathSegment::TOOLHEAD));
                if (data.has_toolhead_sensor)
                    add_band_at_end(plan, BandKind::Trunk, *b,
                                    bypass_lane.band(PathSegment::TOOLHEAD), bypass_lane.color);
                append_line(*b, cx, (float)y.toolhead, cx, (float)y.nozzle_top,
                            bypass_lane.style(PathSegment::NOZZLE));
            }
        }
    }

    boxes.combiner_bg = hub_fill(data, data.active_unit >= 0 && data.filament_loaded);
}

} // namespace

NozzleFeed nozzle_feed(const SystemPathData& data, int tool) {
    NozzleFeed f;
    const int units = LV_MIN(data.unit_count, SystemPathData::kMaxUnits);
    if (data.total_tools <= 1) {
        if (tool != 0)
            return f;
        for (int u = 0; u < units; ++u) {
            if (data.unit_absent[u])
                continue;
            f.routes++;
        }
        f.buffer = data.has_buffer;
    } else {
        if (tool < 0 || tool >= data.total_tools)
            return f;
        for (int u = 0; u < units; ++u) {
            if (data.unit_absent[u])
                continue;
            const int first = data.unit_first_tool[u];
            const int count = data.unit_tool_count[u];
            if (per_tool_unit(data, u)) {
                if (tool < first || tool >= first + count)
                    continue;
                f.routes++;
                continue;
            }
            if (count <= 0 || first != tool)
                continue;
            f.routes++;
            if (!f.buffer && data.unit_has_buffer[u]) {
                f.buffer = true;
                f.buffer_unit = u;
            }
        }
    }
    f.junction = f.routes >= 2;
    return f;
}

int32_t junction_label_w(const SystemPathData& data, int32_t box_h, bool with_buffer) {
    const int32_t meter = with_buffer ? helix::ui::buffer_meter_slot_width(box_h) : 0;
    if (!data.label_font)
        return LV_MAX(data.hub_width * 2 / 3, meter);
    return LV_MAX(data.hub_width * 2 / 3,
                  helix::ui::text_width(kHubLabel, data.label_font, 0) + 2 * data.space_xs + meter);
}

int32_t labeled_box_h(const SystemPathData& data, int32_t base_h, int32_t room, bool* labeled) {
    const int32_t need = data.label_line_h > 0 ? data.label_line_h + data.space_xs : 0;
    *labeled = need <= base_h || need <= room;
    return *labeled ? LV_MAX(base_h, need) : base_h;
}

TrunkBuffer trunk_buffer(const SystemPathData& data, const SysLayout& L) {
    const int32_t hub_bottom = L.hub_y + L.hub_h / 2;
    const int32_t nozzle_top = single_nozzle_inlet_y(data, L);
    const int32_t run = nozzle_top - hub_bottom;
    TrunkBuffer b{hub_bottom + run / 2, 0, true};
    b.h = labeled_box_h(data, L.hub_h * 2 / 3, run - 2 * data.tube_gauge, &b.labeled);
    return b;
}

SysLayout compute_sys_layout(const SystemPathData& data, const lv_area_t& obj_coords) {
    SysLayout L{};
    L.width = lv_area_get_width(&obj_coords);
    L.height = lv_area_get_height(&obj_coords);
    L.x_off = obj_coords.x1;
    L.y_off = obj_coords.y1;
    L.multi_tool = data.total_tools > 1;
    L.entry_y = L.y_off + (int32_t)(L.height * kEntryYRatio);

    // The toolheads stand on their glyph-bottom line; the stems converge, and
    // the combiner sits, fixed fractions of the run down to them.
    const float bottom_ratio = L.multi_tool ? kMultiGlyphBottomRatio : kSingleGlyphBottomRatio;
    const int32_t scale = L.multi_tool ? small_tool_scale(data) : data.extruder_scale;
    const int32_t toolhead_y = L.y_off + (int32_t)(L.height * bottom_ratio) -
                               helix::ui::toolhead_bounds(data.toolhead_style, 0, 0, scale).bottom;
    const int32_t run = toolhead_y - L.entry_y;
    // One toolhead: the lines start their diagonals just under the cards and meet in a hub
    // nearer the units than the toolhead, so the long straight run is the hub's to the head.
    const float merge = L.multi_tool ? 0.35f : 0.04f;
    const float hub = L.multi_tool ? 0.61f : 0.44f;
    const float hub_h = L.multi_tool ? 0.175f : 0.13f;
    L.merge_y = L.entry_y + (int32_t)(run * merge);
    L.hub_y = L.entry_y + (int32_t)(run * hub);
    L.hub_h = (int32_t)(run * hub_h);
    if (!L.multi_tool) {
        // The combiner grows to fit its label while the fan still has room to
        // bend into its top.
        L.hub_h = labeled_box_h(data, L.hub_h, 2 * (L.hub_y - L.merge_y - (int32_t)kFanFillet),
                                &L.hub_labeled);
    }
    L.tools_y = toolhead_y;
    L.nozzle_y = toolhead_y;
    L.center_x = L.x_off + L.width / 2;
    // The hub and toolhead shift ~10% left to make room for the bypass path.
    if (data.has_bypass && !L.multi_tool)
        L.center_x -= L.width / 10;
    return L;
}

int32_t calc_tool_x(int tool_index, int total_tools, int32_t x_off, int32_t width) {
    if (total_tools <= 1) {
        return x_off + width / 2;
    }
    // Distribute tools evenly across 20%-80% of widget width
    int32_t margin = width / 5;
    int32_t usable = width - 2 * margin;
    return x_off + margin + (usable * tool_index) / (total_tools - 1);
}

int32_t small_tool_scale(const SystemPathData& data) {
    return LV_MAX(6, data.extruder_scale * 3 / 4);
}

int32_t clamp_span(int32_t lo, int32_t v, int32_t hi) {
    return lo <= hi ? LV_CLAMP(lo, v, hi) : lo + (hi - lo) / 2;
}

BypassGeometry compute_bypass_geometry(const SystemPathData& data, const lv_area_t& obj_coords) {
    int32_t width = lv_area_get_width(&obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // Hub shifts ~10% left to keep its label clear of the long horizontal
    // bypass merge line (single-tool, has_bypass).
    int32_t center_x = x_off + width / 2 - width / 10;

    const SysLayout L = compute_sys_layout(data, obj_coords);
    // Half the bypass card the panel floats over this point: the spool must clear
    // the rightmost toolhead and the canvas edges by that much.
    const int32_t spool_half = helix::ui::bypass_spool_box_size() / 2;
    const int32_t hub_bottom = L.hub_y + L.hub_h / 2;
    const int32_t nozzle_y = L.nozzle_y;
    int32_t merge_y = hub_bottom + (nozzle_y - hub_bottom) / 3;

    // Comfortable gap between the rightmost toolhead and the bypass spool.
    int32_t comfort = LV_MAX(data.space_md, 8);
    // Right boundary the spool center must stay inside (leave room for its half
    // width + a small margin from the canvas edge).
    int32_t right_limit = x_off + width - spool_half - 4;
    int32_t left_limit = x_off + spool_half + 4;

    // Default horizontal position (single-tool / no toolhead row): a long
    // visible tube run from the hub.
    int32_t bypass_x = x_off + (int32_t)(width * kBypassXRatio);

    // With a toolhead row, place the bypass spool just past the rightmost
    // toolhead glyph's right edge so the two never overlap.
    if (data.total_tools > 0) {
        int32_t rightmost_tool_x =
            calc_tool_x(data.total_tools - 1, data.total_tools, x_off, width);
        int32_t scale = small_tool_scale(data);
        int32_t tool_right_edge = rightmost_tool_x + scale * 2;

        int32_t want_x = tool_right_edge + comfort + spool_half;
        if (want_x > bypass_x) {
            bypass_x = want_x;
        }

        if (bypass_x > right_limit) {
            // No horizontal room even at the canvas edge — drop the spool below
            // the toolhead row, centered over the rightmost toolhead, clamped so
            // its lower edge stays on the canvas.
            bypass_x = clamp_span(left_limit, rightmost_tool_x, right_limit);
            merge_y = nozzle_y + scale * 4 + spool_half + comfort;
            merge_y = LV_MIN(merge_y, y_off + height - spool_half - 4);
        }
    }

    bypass_x = clamp_span(left_limit, bypass_x, right_limit);
    return {bypass_x, merge_y, center_x};
}

Lane unit_lane(const SystemPathData& data, int unit) {
    const bool on = unit == data.active_unit;
    const bool hub_reads = unit >= 0 && unit < SystemPathData::kMaxUnits &&
                           data.unit_has_hub_sensor[unit] && data.unit_hub_triggered[unit];
    PathSegment reached = PathSegment::NONE;
    lv_color_t color = data.color_idle;
    if (on) {
        reached = data.filament_segment;
        if (reached == PathSegment::NONE && data.filament_loaded)
            reached = PathSegment::NOZZLE;
        if (hub_reads && reached < PathSegment::OUTPUT)
            reached = PathSegment::OUTPUT;
        color = lv_color_hex(data.active_color);
    } else if (unit >= 0 && unit < SystemPathData::kMaxUnits) {
        // Past its own hub output, an inactive unit's filament would be in the
        // shared path, which belongs to the active unit.
        const PathSegment lane = data.unit_lane_segment[unit];
        reached = LV_MIN(lane, PathSegment::OUTPUT);
        if (hub_reads)
            reached = PathSegment::OUTPUT;
        if (lane != PathSegment::NONE)
            color = lv_color_hex(data.unit_lane_color[unit]);
    }
    return {reached, on, on ? data.error_segment : PathSegment::NONE, color, data.color_bg};
}

void plan_overview(const SystemPathData& data, const SysLayout& L, PathPlan& plan,
                   OverviewBoxes& boxes) {
    fpath::reset_plan(plan);
    boxes = OverviewBoxes{};
    if (L.multi_tool)
        plan_multi_tool(data, L, plan, boxes);
    else
        plan_single_tool(data, L, plan, boxes);
    fpath::total_dropped(plan);
}

fpath::TubePalette overview_palette(const SystemPathData& data) {
    return {data.color_idle, data.color_accent, data.color_error, data.color_bg, data.tube_gauge};
}

} // namespace helix::ui::syspath
