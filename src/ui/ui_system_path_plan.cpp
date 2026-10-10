// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The overview's layout and route plan. See ui_system_path_plan.h.

#include "ui_system_path_plan.h"

#include "ui_toolhead_badge.h"

#include <algorithm>
#include <cmath>

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

// Mirrors ui_filament_path_canvas's BYPASS_X_RATIO so both canvases place the
// bypass spool at the same horizontal fraction — keeps a long visible tube
// segment instead of cramming the spool right next to the hub.
constexpr float BYPASS_X_RATIO = 0.85f;

// Half-extent of the panel's BypassSpoolWidgets overlay (box is 48 + 4*2 = 56px
// wide; mirror it here so the canvas can guarantee the spool clears the
// rightmost toolhead). Kept in lockstep with BOX_SIZE in ui_bypass_spool_widget.cpp.
constexpr int32_t BYPASS_SPOOL_HALF = 28;

// Merge fans: the outermost entries sit this far in from the hub-top ends, and
// the per-side slope is capped so tall hubs don't run near-vertical.
constexpr float FAN_ENTRY_MARGIN = 8.0f;
constexpr float FAN_MAX_SLOPE = 1.2f;
constexpr float FAN_FILLET = 9.0f;

constexpr int TOPO_PARALLEL = 2;
constexpr int TOPO_MIXED = 3;

// A box with filament passing through it is tinted toward the filament.
lv_color_t hub_fill(const SystemPathData& data, bool has_filament) {
    const lv_color_t bg = data.color_hub_bg;
    return has_filament ? fpath::ph_blend(bg, lv_color_hex(data.active_color), 0.33f) : bg;
}

// Horizontal position of unit `i`'s entry stem: the centre of its unit card,
// clamped into the canvas.
//
// The anchor is pushed by the panel and re-pushed on every card-row scroll -
// unit_cards_row is an independently scrollable container. Once the row
// overflows, a scrolled-off card's centre lands outside the canvas; clamped,
// the stem parks at the edge it went out of, which reads as "this unit is off
// to that side".
int32_t unit_stem_x(const SystemPathData& data, const SysLayout& L, int i) {
    if (i < 0 || i >= SystemPathData::MAX_UNITS) {
        return L.x_off + L.width / 2;
    }
    int32_t margin = LV_MIN(8, L.width / 4);
    return LV_CLAMP(L.x_off + data.unit_x_positions[i], L.x_off + margin,
                    L.x_off + L.width - margin);
}

// The fan of one or more lanes into a hub top at (hub_cx, hub_top).
void merge_fan(const pg::MergeLaneIn* in, int n, int32_t hub_cx, int32_t hub_top, int32_t hub_w,
               pg::MergeLaneOut* out) {
    pg::build_merge_fan(in, n, (float)hub_cx, (float)hub_top, (float)hub_w, FAN_ENTRY_MARGIN,
                        FAN_FILLET, FAN_MAX_SLOPE, out);
}

void append_fan(Route& r, const pg::MergeLaneOut& lane, fpath::SpanStyle s) {
    pg::FilamentPath fan;
    pg::route_polyline_filleted(fan, lane.pts, 4, FAN_FILLET);
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
    if (plan.route_count >= fpath::MAX_ROUTES) {
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
    int32_t dist;  // absolute horizontal distance (for stagger ordering)
    bool is_hub;   // HUB topology route (lands on a mini hub)
    bool via_box;  // MIXED hub-group route: passes through its "H" box above the tool
    int group = 0; // shared hub this route joins (SystemPathData::unit_hub_group), 0 = own hub
};

// A unit that can place a hub box on a nozzle: HUB topology, present, with a tool.
bool places_hub(const SystemPathData& data, int u) {
    return u >= 0 && u < data.unit_count && u < SystemPathData::MAX_UNITS &&
           data.unit_topology[u] != TOPO_PARALLEL && data.unit_topology[u] != TOPO_MIXED &&
           data.unit_tool_count[u] > 0;
}

// Units sharing one hub box: those carrying the same non-zero unit_hub_group on
// one nozzle. Returns the member count and the lowest member (the box's owner
// in OverviewBoxes::hubs); a unit on a hub of its own is a group of one.
int shared_hub_members(const SystemPathData& data, int unit, int* members, int* leader) {
    const int g = data.unit_hub_group[unit];
    int n = 0;
    *leader = unit;
    if (g <= 0 || !places_hub(data, unit)) {
        if (members)
            members[0] = unit;
        return 1;
    }
    for (int u = 0; u < data.unit_count && u < SystemPathData::MAX_UNITS; ++u) {
        if (data.unit_hub_group[u] != g || !places_hub(data, u) || data.unit_absent[u] ||
            data.unit_first_tool[u] != data.unit_first_tool[unit])
            continue;
        if (n == 0)
            *leader = u;
        if (members)
            members[n] = u;
        n++;
    }
    return n;
}

// The shared hub @p unit joins, or 0 when it is on a hub of its own.
int shared_hub_of(const SystemPathData& data, int unit) {
    int leader = unit;
    return shared_hub_members(data, unit, nullptr, &leader) > 1 ? data.unit_hub_group[unit] : 0;
}

// Where unit `unit_index`'s hub sits among the hubs that feed the SAME physical
// nozzle, and how many there are.
//
// compute_system_tool_layout() deliberately merges two HUB units onto one
// physical tool when they name the same extruder (a Box Turtle and a Claymore
// both wired to e0). Each unit still owns a real, separate hub, so the boxes
// fan out around the nozzle to keep every hub visible and its route
// distinguishable. Units on one shared hub count once. Returns rank 0 / count 1
// for the common unshared case.
void hub_group_position(const SystemPathData& data, int unit_index, int* rank, int* count) {
    *rank = 0;
    *count = 0;
    const int my_tool = data.unit_first_tool[unit_index];
    int my_leader = unit_index;
    shared_hub_members(data, unit_index, nullptr, &my_leader);
    for (int u = 0; u < data.unit_count && u < SystemPathData::MAX_UNITS; ++u) {
        if (!places_hub(data, u) || data.unit_first_tool[u] != my_tool)
            continue;
        int leader = u;
        shared_hub_members(data, u, nullptr, &leader);
        if (leader != u)
            continue;
        if (u < my_leader)
            (*rank)++;
        (*count)++;
    }
}

// PARALLEL / MIXED unit — one route per unique tool position. For MIXED,
// tool_count already reflects unique nozzles (not lanes), so hub lanes sharing a
// mapped_tool produce a single route; the hub group's mini-hub is recorded for
// the last tool.
int collect_parallel_mixed_routes(const SystemPathData& data, const SysLayout& L, int i,
                                  GlobalRoute* routes, int n, OverviewBoxes& boxes) {
    int32_t unit_x = unit_stem_x(data, L, i);
    int tool_count = data.unit_tool_count[i];
    int first_tool = data.unit_first_tool[i];

    // Fan the lane start points apart just enough that their tubes do not
    // overlap, and no further: keep the whole fan inside the unit's own column,
    // under its label, so each lane reads as leaving its unit.
    int32_t column_w = (data.unit_count > 1) ? (L.width / data.unit_count) : L.width;
    int32_t spread = 0;
    if (tool_count > 1) {
        const int32_t pitch = data.tube_gauge + HALO_WIDTH_EXTRA + 2;
        spread = LV_MIN(column_w - 8, (tool_count - 1) * pitch);
        spread = LV_MAX(spread, 0);
    }
    auto start_x_of = [&](int t) {
        return tool_count > 1 ? unit_x - spread / 2 + (spread * t) / (tool_count - 1) : unit_x;
    };
    for (int t = 0; t < tool_count && (first_tool + t) < data.total_tools; ++t) {
        if (n >= SystemPathData::MAX_TOOLS)
            break;
        int tool_idx = first_tool + t;
        int32_t tool_x = calc_tool_x(tool_idx, data.total_tools, L.x_off, L.width);
        int32_t start_x = start_x_of(t);
        int32_t dist = start_x > tool_x ? (start_x - tool_x) : (tool_x - start_x);
        routes[n++] = {i, tool_idx, start_x, L.entry_y, tool_x, L.tools_y, dist, false, false, 0};
    }

    // MIXED: the hub-routed lanes share the group's last tool. Its "H" box
    // stands just above that tool, where its route is alone: the route enters
    // the box top and leaves the bottom for the nozzle.
    if (data.unit_topology[i] == TOPO_MIXED && tool_count > 1 && n > 0 &&
        routes[n - 1].tool_idx == first_tool + tool_count - 1) {
        GlobalRoute& hub_route = routes[n - 1];
        hub_route.via_box = true;
        const int32_t mhw = data.hub_width * 2 / 5;
        const int32_t mhh = L.hub_h * 2 / 3;
        const int32_t outlet = LV_MAX(10, 3 * data.tube_gauge);
        const int32_t nozzle_top = L.tools_y - small_tool_scale(data) * 2;
        const int32_t mhy = nozzle_top - outlet - mhh / 2;
        const bool hub_has_filament =
            i == data.active_unit && data.filament_loaded && data.active_tool == hub_route.tool_idx;
        boxes.hubs[i] = {hub_route.end_x,
                         hub_route.end_x,
                         mhy,
                         mhw,
                         mhh,
                         hub_fill(data, hub_has_filament),
                         hub_route.tool_idx,
                         true};
        hub_route.end_y = mhy - mhh / 2;
    }
    return n;
}

// The buffer box on a hub's outlet, between the hub and its nozzle, when the
// hub's first unit has one. It takes half the run so the outlet shows above and
// below it.
void place_hub_buffer(const SystemPathData& data, const SysLayout& L, int unit, HubInfo& hub) {
    if (!data.unit_has_buffer[unit])
        return;
    const int32_t hub_bottom = hub.mini_hub_y + hub.mini_hub_h / 2;
    const int32_t nozzle_top = L.tools_y - small_tool_scale(data) * 2;
    const int32_t run = nozzle_top - hub_bottom;
    hub.buffer_h = LV_MIN(hub.mini_hub_h, run / 2);
    hub.buffer_w = data.hub_width * 2 / 3;
    hub.buffer_y = hub_bottom + run / 2;
    if (hub.buffer_h <= 0)
        hub.buffer_h = 0;
}

// HUB unit — one route from the unit to its hub: its own mini hub, or the box
// shared with the other units on its hub.
int collect_hub_route(const SystemPathData& data, const SysLayout& L, int i, GlobalRoute* routes,
                      int n, OverviewBoxes& boxes) {
    int32_t unit_x = unit_stem_x(data, L, i);
    int tool_count = data.unit_tool_count[i];
    int first_tool = data.unit_first_tool[i];
    if (tool_count <= 0 || first_tool >= data.total_tools || n >= SystemPathData::MAX_TOOLS)
        return n;

    int32_t tool_x = calc_tool_x(first_tool, data.total_tools, L.x_off, L.width);
    int32_t mini_hub_w = data.hub_width * 2 / 3;
    int32_t mini_hub_h = L.hub_h * 2 / 3;
    int32_t mini_hub_y = L.merge_y + (L.tools_y - L.merge_y) / 3;

    int members[SystemPathData::MAX_UNITS];
    int leader = i;
    const int member_count = shared_hub_members(data, i, members, &leader);
    const int group = member_count > 1 ? data.unit_hub_group[i] : 0;

    // A shared hub is wide enough for one lane per unit.
    int32_t box_w = mini_hub_w;
    if (group > 0) {
        const int32_t pitch = data.tube_gauge + HALO_WIDTH_EXTRA + 2;
        box_w = LV_MAX(mini_hub_w, 2 * (int32_t)FAN_ENTRY_MARGIN + member_count * pitch);
    }

    // Fan the box away from the nozzle center when another hub feeds the same
    // one. rank 0 / count 1 (nothing shared) leaves hub_x == tool_x.
    int hub_rank = 0;
    int hub_group = 1;
    hub_group_position(data, i, &hub_rank, &hub_group);
    int32_t hub_pitch = box_w + LV_MAX(4, data.tube_gauge);
    int32_t hub_x = tool_x + (2 * hub_rank - (hub_group - 1)) * hub_pitch / 2;

    // HUB stems drop to a shorter merge point, leaving room between hub routes
    // and the parallel routes below.
    int32_t hub_merge_y = L.entry_y + (L.merge_y - L.entry_y) * 2 / 3;
    int32_t dist = unit_x > hub_x ? (unit_x - hub_x) : (hub_x - unit_x);
    routes[n++] = {i,    first_tool, unit_x, hub_merge_y, hub_x, mini_hub_y - mini_hub_h / 2,
                   dist, true,       false,  group};

    if (i != leader)
        return n; // the shared box is recorded once, at its lowest unit

    bool hub_has_filament = false;
    for (int m = 0; m < member_count; ++m)
        hub_has_filament =
            hub_has_filament || (members[m] == data.active_unit && data.filament_loaded);
    boxes.hubs[i] = {hub_x,      tool_x,     mini_hub_y,
                     box_w,      mini_hub_h, hub_fill(data, hub_has_filament),
                     first_tool, true};
    place_hub_buffer(data, L, i, boxes.hubs[i]);
    return n;
}

// PARALLEL by end_x ascending (leftmost tool first → bottom horizontal); HUB
// after parallel, by distance descending.
void sort_routes(GlobalRoute* routes, int n) {
    std::stable_sort(routes, routes + n, [](const GlobalRoute& a, const GlobalRoute& b) {
        if (a.is_hub != b.is_hub)
            return !a.is_hub;
        return a.is_hub ? a.dist > b.dist : a.end_x < b.end_x;
    });
}

// Cable-harness route: vertical drop, bend at horiz_y, a gently descending
// diagonal across to ex, drop into (ex, ey).
void append_harness(Route& r, int32_t sx, int32_t sy, int32_t ex, int32_t ey, int32_t horiz_y,
                    fpath::SpanStyle s) {
    // Clamp the bend so it leaves room for both fillets.
    int32_t lo = sy + 4;
    int32_t hi = ey - 6;
    pg::FilamentPath path;
    if (hi < lo) {
        pg::route_orthogonal(path, (float)sx, (float)sy, (float)ex, (float)ey, FILLET_RADIUS);
        route_append(r, path, s);
        return;
    }
    horiz_y = LV_CLAMP(horiz_y, lo, hi);
    int32_t dx = (ex > sx) ? (ex - sx) : (sx - ex);
    int32_t y_approach = horiz_y + dx / 10; // ~10% downward slope along the run
    y_approach = LV_CLAMP(y_approach, horiz_y, ey - 6);
    pg::PathPoint pts[4] = {{(float)sx, (float)sy},
                            {(float)sx, (float)horiz_y},
                            {(float)ex, (float)y_approach},
                            {(float)ex, (float)ey}};
    pg::route_polyline_filleted(path, pts, 4, FAN_FILLET);
    route_append(r, path, s);
}

void plan_multi_tool(const SystemPathData& data, const SysLayout& L, PathPlan& plan,
                     OverviewBoxes& boxes) {
    GlobalRoute routes[SystemPathData::MAX_TOOLS];
    int n = 0;
    for (int i = 0; i < data.unit_count && i < SystemPathData::MAX_UNITS; i++) {
        if (data.unit_absent[i])
            continue;
        const int topology = data.unit_topology[i];
        if (topology == TOPO_PARALLEL || topology == TOPO_MIXED)
            n = collect_parallel_mixed_routes(data, L, i, routes, n, boxes);
        else
            n = collect_hub_route(data, L, i, routes, n, boxes);
    }
    sort_routes(routes, n);

    const float nozzle_top = (float)(L.tools_y - small_tool_scale(data) * 2);

    // PARALLEL horizontal levels, fixed spacing, centred at 55% between the
    // entries and the tool row. The leftmost tool takes the lowest level and
    // the rightmost the highest, so no end drop crosses another route's run.
    int parallel_count = 0;
    for (int r = 0; r < n; ++r)
        parallel_count +=
            !routes[r].is_hub && LV_ABS(routes[r].end_x - routes[r].start_x) > data.tube_gauge;
    const int32_t arc_r = LV_MAX(8, (L.tools_y - L.entry_y) / 10);
    const int32_t par_step = LV_MAX(10, data.tube_gauge + HALO_WIDTH_EXTRA + 2);
    const int32_t par_group_h = (parallel_count > 1) ? par_step * (parallel_count - 1) : 0;
    const int32_t par_center_y = L.entry_y + (L.tools_y - L.entry_y) * 55 / 100;
    const int32_t par_bot_y = par_center_y - par_group_h / 2 + par_group_h;
    int parallel_idx = 0;

    for (int k = 0; k < n; ++k) {
        const GlobalRoute& route = routes[k];
        const bool unit_on = route.unit_idx == data.active_unit;
        const bool tool_on = unit_on && route.tool_idx == data.active_tool;
        Route* r = try_new_route(plan);
        if (!r)
            break;

        if (!route.is_hub) {
            // A tool route carries filament only when it is the active tool's;
            // a MIXED hub group's also when its hub sensor reads filament.
            Lane lane = unit_lane(data, route.unit_idx);
            if (!tool_on) {
                const bool hub_reads = route.via_box && data.unit_has_hub_sensor[route.unit_idx] &&
                                       data.unit_hub_triggered[route.unit_idx];
                lane = unit_lane(data, -1);
                if (hub_reads) {
                    lane.reached = PathSegment::OUTPUT;
                    if (data.unit_lane_segment[route.unit_idx] != PathSegment::NONE)
                        lane.color = lv_color_hex(data.unit_lane_color[route.unit_idx]);
                }
            }
            lane.on = tool_on;
            const fpath::SpanStyle s = lane.style(PathSegment::LANE);
            // A hub-group route ends at its box top; the rest at the nozzle.
            const int32_t end_y = route.via_box ? route.end_y : (int32_t)nozzle_top;
            const int32_t dx = route.end_x - route.start_x;
            if (LV_ABS(dx) <= data.tube_gauge) {
                // Too close to bend into: drop straight onto the glyph.
                append_line(*r, (float)route.start_x, (float)route.start_y, (float)route.start_x,
                            (float)end_y, s);
            } else {
                int32_t horiz_y = par_bot_y - parallel_idx * par_step;
                parallel_idx++;
                horiz_y = LV_CLAMP(horiz_y, route.start_y + arc_r + 2, end_y - arc_r - 2);
                append_harness(*r, route.start_x, route.start_y, route.end_x, end_y, horiz_y, s);
            }
            if (route.via_box) {
                const HubInfo& hub = boxes.hubs[route.unit_idx];
                const float x = seg_end_x(*r);
                const float out_y = (float)(hub.mini_hub_y + hub.mini_hub_h / 2);
                append_line(*r, x, (float)end_y, x, out_y, unpainted(lane.style(PathSegment::HUB)));
                if (data.unit_has_hub_sensor[route.unit_idx]) {
                    add_band_at_end(plan, BandKind::Lane, *r, lane.band(PathSegment::OUTPUT),
                                    lane.color, /*on_box_edge=*/true);
                }
                append_line(*r, x, out_y, x, nozzle_top, lane.style(PathSegment::OUTPUT));
            }
            if (tool_on)
                plan.active_route = plan.route_count - 1;
            continue;
        }

        // HUB: stem → diagonal into its hub's top → through the box → outlet
        // (past the hub's buffer box) to the nozzle. On a shared hub every unit
        // brings its stem to the box top and one of them carries the outlet.
        const Lane lane = unit_lane(data, route.unit_idx);
        int members[SystemPathData::MAX_UNITS];
        int leader = route.unit_idx;
        const int member_count = shared_hub_members(data, route.unit_idx, members, &leader);
        const bool shared = route.group > 0 && member_count > 1;
        const HubInfo& hub = boxes.hubs[leader];
        append_line(*r, (float)route.start_x, (float)L.entry_y, (float)route.start_x,
                    (float)route.start_y, lane.style(PathSegment::LANE));

        float fan_end_x = (float)hub.hub_x;
        if (shared) {
            // Lanes enter the box top left to right, in stem order.
            std::sort(members, members + member_count, [&](int a, int b) {
                return unit_stem_x(data, L, a) < unit_stem_x(data, L, b);
            });
            pg::MergeLaneIn in[SystemPathData::MAX_UNITS];
            pg::MergeLaneOut fans[SystemPathData::MAX_UNITS];
            int mine = 0;
            for (int m = 0; m < member_count; ++m) {
                in[m] = {(float)unit_stem_x(data, L, members[m]), (float)route.start_y};
                if (members[m] == route.unit_idx)
                    mine = m;
            }
            merge_fan(in, member_count, hub.hub_x, route.end_y, hub.mini_hub_w, fans);
            append_fan(*r, fans[mine], lane.style(PathSegment::HUB));
            fan_end_x = fans[mine].pts[3].x;
        } else {
            pg::MergeLaneIn in{(float)route.start_x, (float)route.start_y};
            pg::MergeLaneOut fan;
            merge_fan(&in, 1, route.end_x, route.end_y, 0, &fan);
            append_fan(*r, fan, lane.style(PathSegment::HUB));
        }

        const int32_t out_y = hub.mini_hub_y + hub.mini_hub_h / 2;
        if (shared) {
            // One unit carries the outlet: the active one, else the one loaded
            // furthest, else the first.
            int owner = members[0];
            for (int m = 0; m < member_count; ++m) {
                const int u = members[m];
                if (u == data.active_unit) {
                    owner = u;
                    break;
                }
                if (data.unit_lane_segment[u] > data.unit_lane_segment[owner])
                    owner = u;
            }
            if (route.unit_idx != owner)
                continue;
        }

        append_line(*r, fan_end_x, (float)route.end_y, (float)hub.hub_x, (float)out_y,
                    unpainted(lane.style(PathSegment::HUB)));
        // The hub sensor reads the hub's output.
        if (data.unit_has_hub_sensor[route.unit_idx]) {
            add_band_at_end(plan, BandKind::Lane, *r, lane.band(PathSegment::OUTPUT), lane.color,
                            /*on_box_edge=*/true);
        }
        const int32_t buffer_bottom = hub.buffer_h > 0 ? hub.buffer_y + hub.buffer_h / 2 : out_y;
        if (hub.hub_x == hub.tool_x) {
            append_line(*r, (float)hub.hub_x, (float)out_y, (float)hub.tool_x, nozzle_top,
                        lane.style(PathSegment::OUTPUT));
        } else {
            // Shared nozzle: this hub sits beside it, so the outlet drops past
            // the buffer and then angles in.
            if (buffer_bottom > out_y) {
                append_line(*r, (float)hub.hub_x, (float)out_y, (float)hub.hub_x,
                            (float)buffer_bottom, lane.style(PathSegment::OUTPUT));
            }
            pg::MergeLaneIn out_in{(float)hub.hub_x, (float)buffer_bottom};
            pg::MergeLaneOut outlet;
            merge_fan(&out_in, 1, hub.tool_x, (int32_t)nozzle_top, 0, &outlet);
            append_fan(*r, outlet, lane.style(PathSegment::OUTPUT));
        }
        if (unit_on)
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
    y.nozzle_top = L.nozzle_y - data.extruder_scale * 2;
    y.bypass_merge = y.hub_bottom + (L.nozzle_y - y.hub_bottom) / 3;
    y.toolhead = y.hub_bottom + (y.nozzle_top - y.hub_bottom) * 2 / 3;
    const float cx = (float)L.center_x;
    const int32_t sensor_y = L.entry_y + (L.merge_y - L.entry_y) * 3 / 5;

    const bool bypass_owns = data.has_bypass && data.bypass_active;
    const Lane bypass_lane{data.bypass_active ? PathSegment::NOZZLE : PathSegment::NONE,
                           data.bypass_active,
                           data.bypass_active ? data.error_segment : PathSegment::NONE,
                           lv_color_hex(data.bypass_color), data.color_bg};
    const Lane* bypass_owner = bypass_owns ? &bypass_lane : nullptr;

    int units[SystemPathData::MAX_UNITS];
    pg::MergeLaneIn in[SystemPathData::MAX_UNITS];
    int count = 0;
    for (int i = 0; i < data.unit_count && i < SystemPathData::MAX_UNITS; i++) {
        if (data.unit_absent[i])
            continue;
        units[count] = i;
        in[count++] = {(float)unit_stem_x(data, L, i), (float)L.merge_y};
    }
    pg::MergeLaneOut fan[SystemPathData::MAX_UNITS];
    merge_fan(in, count, L.center_x, hub_top, data.hub_width, fan);

    bool trunk_owned = false;
    for (int k = 0; k < count; k++) {
        const int u = units[k];
        const Lane lane = unit_lane(data, u);
        const float x = in[k].slot_x;
        Route* r = try_new_route(plan);
        if (!r)
            break;
        if (data.unit_has_hub_sensor[u]) {
            append_line(*r, x, (float)L.entry_y, x, (float)sensor_y, lane.style(PathSegment::LANE));
            add_band_at_end(plan, BandKind::Lane, *r, lane.band(PathSegment::OUTPUT), lane.color);
            append_line(*r, x, (float)sensor_y, x, (float)L.merge_y,
                        lane.style(PathSegment::OUTPUT));
        } else {
            append_line(*r, x, (float)L.entry_y, x, (float)L.merge_y,
                        lane.style(PathSegment::LANE));
        }
        append_fan(*r, fan[k], lane.style(PathSegment::OUTPUT));
        if (!lane.on)
            continue;
        plan.active_route = plan.route_count - 1;
        trunk_owned = true;
        append_line(*r, fan[k].pts[3].x, (float)hub_top, cx, (float)y.hub_bottom,
                    unpainted(lane.style(PathSegment::HUB)));
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
            // Spool → merge carries the SPOOL tag: no AMS error lands on it. The
            // tube starts inside the opaque spool overlay.
            append_line(*b, (float)bg.bypass_x, (float)y.bypass_merge, cx, (float)y.bypass_merge,
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

SysLayout compute_sys_layout(const SystemPathData& data, const lv_area_t& obj_coords) {
    SysLayout L{};
    L.width = lv_area_get_width(&obj_coords);
    L.height = lv_area_get_height(&obj_coords);
    L.x_off = obj_coords.x1;
    L.y_off = obj_coords.y1;
    L.multi_tool = data.total_tools > 1;
    L.entry_y = L.y_off + (int32_t)(L.height * ENTRY_Y_RATIO);

    // The toolheads stand on their glyph-bottom line; the stems converge, and
    // the combiner sits, fixed fractions of the run down to them.
    const float bottom_ratio = L.multi_tool ? MULTI_GLYPH_BOTTOM_RATIO : SINGLE_GLYPH_BOTTOM_RATIO;
    const int32_t scale = L.multi_tool ? small_tool_scale(data) : data.extruder_scale;
    const int32_t toolhead_y = L.y_off + (int32_t)(L.height * bottom_ratio) -
                               helix::ui::toolhead_bounds(data.toolhead_style, 0, 0, scale).bottom;
    const int32_t run = toolhead_y - L.entry_y;
    const float merge = L.multi_tool ? 0.35f : 0.30f;
    const float hub = L.multi_tool ? 0.61f : 0.52f;
    const float hub_h = L.multi_tool ? 0.175f : 0.15f;
    L.merge_y = L.entry_y + (int32_t)(run * merge);
    L.hub_y = L.entry_y + (int32_t)(run * hub);
    L.hub_h = (int32_t)(run * hub_h);
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

BypassGeometry compute_bypass_geometry(const SystemPathData& data, const lv_area_t& obj_coords) {
    int32_t width = lv_area_get_width(&obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // Hub shifts ~10% left to keep its label clear of the long horizontal
    // bypass merge line (single-tool, has_bypass).
    int32_t center_x = x_off + width / 2 - width / 10;

    const SysLayout L = compute_sys_layout(data, obj_coords);
    const int32_t hub_bottom = L.hub_y + L.hub_h / 2;
    const int32_t nozzle_y = L.nozzle_y;
    int32_t merge_y = hub_bottom + (nozzle_y - hub_bottom) / 3;

    // Comfortable gap between the rightmost toolhead and the bypass spool.
    int32_t comfort = LV_MAX(data.space_md, 8);
    // Right boundary the spool centre must stay inside (leave room for its half
    // width + a small margin from the canvas edge).
    int32_t right_limit = x_off + width - BYPASS_SPOOL_HALF - 4;

    // Default horizontal position (single-tool / no toolhead row): a long
    // visible tube run from the hub.
    int32_t bypass_x = x_off + (int32_t)(width * BYPASS_X_RATIO);

    // With a toolhead row, place the bypass spool just past the rightmost
    // toolhead glyph's right edge so the two never overlap.
    if (data.total_tools > 0) {
        int32_t rightmost_tool_x =
            calc_tool_x(data.total_tools - 1, data.total_tools, x_off, width);
        int32_t scale = small_tool_scale(data);
        int32_t tool_right_edge = rightmost_tool_x + scale * 2;

        int32_t want_x = tool_right_edge + comfort + BYPASS_SPOOL_HALF;
        if (want_x > bypass_x) {
            bypass_x = want_x;
        }

        if (bypass_x > right_limit) {
            // No horizontal room even at the canvas edge — drop the spool below
            // the toolhead row, centred over the rightmost toolhead, clamped so
            // its lower edge stays on the canvas.
            bypass_x = LV_CLAMP(rightmost_tool_x, x_off + BYPASS_SPOOL_HALF + 4, right_limit);
            merge_y = nozzle_y + scale * 4 + BYPASS_SPOOL_HALF + comfort;
            merge_y = LV_MIN(merge_y, y_off + height - BYPASS_SPOOL_HALF - 4);
        }
    }

    bypass_x = LV_MIN(bypass_x, right_limit);
    return {bypass_x, merge_y, center_x};
}

Lane unit_lane(const SystemPathData& data, int unit) {
    const bool on = unit == data.active_unit;
    const bool hub_reads = unit >= 0 && unit < SystemPathData::MAX_UNITS &&
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
    } else if (unit >= 0 && unit < SystemPathData::MAX_UNITS) {
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
