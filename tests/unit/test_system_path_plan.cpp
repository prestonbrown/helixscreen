// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The multi-unit overview's route plan. Single-tool cases run on a 400x400
// frame with unit stems at x = 100 and 300 and the default glyph at extruder
// scale 10: entry 20, hub sensor 78, merge 117, hub 189 (h 48, bottom 213),
// nozzle 346 (top 326, glyph bottom 372).

#include "ui_system_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "filament_path_test_helpers.h"
#include "src/ui/ui_system_path_plan.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::syspath;
using namespace helix::ui::fpath;
using namespace fpath_test;
using helix::PathSegment;

namespace {

const lv_color_t IDLE = lv_color_hex(0x555555);
const lv_color_t BG = lv_color_hex(0x202020);
const lv_color_t ACCENT = lv_color_hex(0x3399FF);
constexpr uint32_t ACTIVE = 0xE53935;
constexpr uint32_t OWL = 0x1E88E5;
constexpr lv_area_t AREA = {0, 0, 399, 399};

// Two HUB units (a Box Turtle and a Night Owl) on one toolhead, both with hub
// sensors; unit 0 is active and loaded to the nozzle.
std::unique_ptr<SystemPathData> multi() {
    auto d = std::make_unique<SystemPathData>();
    d->unit_count = 2;
    d->unit_x_positions[0] = 100;
    d->unit_x_positions[1] = 300;
    for (int u = 0; u < 2; u++) {
        d->unit_topology[u] = 1;
        d->unit_tool_count[u] = 1;
        d->unit_has_hub_sensor[u] = true;
    }
    d->total_tools = 1;
    d->active_unit = 0;
    d->active_color = ACTIVE;
    d->filament_loaded = true;
    d->filament_segment = PathSegment::NOZZLE;
    d->unit_hub_triggered[0] = true;
    d->color_idle = IDLE;
    d->color_bg = BG;
    d->color_accent = ACCENT;
    d->color_error = lv_color_hex(0xFF0000);
    d->color_hub_bg = lv_color_hex(0x333333);
    return d;
}

PathPlan& plan_of(const SystemPathData& d) {
    static PathPlan plan;
    OverviewBoxes boxes;
    plan_overview(d, compute_sys_layout(d, AREA), plan, boxes);
    return plan;
}

// The band whose center sits on the stem at @p x.
const SensorBand* stem_band(const PathPlan& plan, float x) {
    for (int i = 0; i < plan.band_count; i++)
        if (std::fabs(plan.bands[i].at.x - x) < 0.5f && plan.bands[i].at.y < 100)
            return &plan.bands[i];
    return nullptr;
}

// A segment of the route ends exactly at @p p.
bool on_route(const Route& r, pg::PathPoint p) {
    for (int i = 0; i < r.path.count; i++)
        if (near(seg_end(r.path.segs[i]), p.x, p.y, 0.5f))
            return true;
    return false;
}

} // namespace

TEST_CASE("Overview plan: each unit's route is continuous with a clamp band on its stem",
          "[system_path][filament_path]") {
    auto d = multi();
    const PathPlan& plan = plan_of(*d);
    REQUIRE(plan.dropped == 0);
    REQUIRE(plan.route_count == 2); // the active unit owns the trunk
    CHECK(plan.active_route == 0);

    for (int u = 0; u < 2; u++) {
        CAPTURE(u);
        const Route& r = plan.routes[u];
        CHECK(contiguous(r.path));
        const SensorBand* band = stem_band(plan, (float)d->unit_x_positions[u]);
        REQUIRE(band != nullptr);
        CHECK(on_route(r, band->at));
        CHECK(near(seg_start(r.path.segs[0]), (float)d->unit_x_positions[u], 20, 0.5f));
    }
    CHECK(plan.band_count == 2);

    // The active route runs on, unbroken, to the nozzle top.
    const Route& active = plan.routes[0];
    CHECK(near(seg_end(active.path.segs[active.path.count - 1]), 200, 326, 0.5f));
    for (int i = 0; i < active.path.count; i++) {
        CAPTURE(i);
        if (!active.style[i].painted)
            continue;
        CHECK(active.style[i].wall == TubeWall::Active);
        CHECK(active.style[i].filled);
        CHECK(lv_color_eq(active.style[i].bore, lv_color_hex(ACTIVE)));
    }
    // The inactive unit is an empty tube.
    for (int i = 0; i < plan.routes[1].path.count; i++)
        CHECK_FALSE(plan.routes[1].style[i].filled);
}

TEST_CASE("Overview layout: the toolheads stand on their glyph-bottom line",
          "[system_path][filament_path]") {
    auto d = multi();
    const SysLayout single = compute_sys_layout(*d, AREA);
    CHECK(single.nozzle_y == 346);
    CHECK(single.hub_y + single.hub_h / 2 == 213);
    CHECK(single.merge_y == 117);

    d->total_tools = 3;
    const SysLayout row = compute_sys_layout(*d, AREA);
    REQUIRE(row.multi_tool);
    // The tool row's glyphs (scale 7, bottom 19 below center) stand on 320.
    CHECK(small_tool_scale(*d) == 7);
    CHECK(row.tools_y == 320 - 19);
}

TEST_CASE("Overview plan: hub bands take the detail view's four states",
          "[system_path][filament_path]") {
    auto d = multi();

    SECTION("Active: triggered on the unit carrying the route") {
        CHECK(stem_band(plan_of(*d), 100)->state == BandState::Active);
    }
    SECTION("Empty: not triggered") {
        CHECK(stem_band(plan_of(*d), 300)->state == BandState::Empty);
    }
    SECTION("Loaded: triggered on an inactive unit, in its loaded lane's color") {
        d->unit_hub_triggered[1] = true;
        d->unit_lane_segment[1] = PathSegment::OUTPUT;
        d->unit_lane_color[1] = OWL;
        const PathPlan& plan = plan_of(*d);
        const SensorBand* band = stem_band(plan, 300);
        REQUIRE(band != nullptr);
        CHECK(band->state == BandState::Loaded);
        CHECK(lv_color_eq(band->fill, lv_color_hex(OWL)));
        // Its tube is filled down to the combiner, in the lane color, plain walls.
        const Route& r = plan.routes[1];
        CHECK(r.style[0].filled);
        CHECK(r.style[0].wall == TubeWall::Plain);
        CHECK(lv_color_eq(r.style[r.path.count - 1].bore, lv_color_hex(OWL)));
    }
    SECTION("Loaded with no lane reporting a color takes the wall color") {
        d->unit_hub_triggered[1] = true;
        const SensorBand* band = stem_band(plan_of(*d), 300);
        REQUIRE(band != nullptr);
        CHECK(band->state == BandState::Loaded);
        CHECK(lv_color_eq(band->fill, IDLE));
    }
    SECTION("Error: the system error is at the active unit's hub output") {
        d->error_segment = PathSegment::OUTPUT;
        const PathPlan& plan = plan_of(*d);
        CHECK(stem_band(plan, 100)->state == BandState::Error);
        // An error belongs to the active route only.
        CHECK(stem_band(plan, 300)->state == BandState::Empty);
    }
}

TEST_CASE("Overview plan: a dumb hub is a continuous tube with no band",
          "[system_path][filament_path]") {
    auto d = multi();
    d->unit_has_hub_sensor[1] = false;
    const PathPlan& plan = plan_of(*d);
    CHECK(plan.band_count == 1);
    CHECK(stem_band(plan, 300) == nullptr);
    CHECK(contiguous(plan.routes[1].path));
}

TEST_CASE("Overview plan: an idle system keeps an idle trunk with its toolhead band",
          "[system_path][filament_path]") {
    auto d = multi();
    d->active_unit = -1;
    d->filament_loaded = false;
    d->filament_segment = PathSegment::NONE;
    d->unit_hub_triggered[0] = false;
    d->has_toolhead_sensor = true;
    const PathPlan& plan = plan_of(*d);
    REQUIRE(plan.trunk_route == 2);
    const Route& trunk = plan.routes[2];
    CHECK(contiguous(trunk.path));
    CHECK(near(seg_start(trunk.path.segs[0]), 200, 213, 0.5f));
    CHECK(near(seg_end(trunk.path.segs[trunk.path.count - 1]), 200, 326, 0.5f));
    REQUIRE(plan.band_count == 3);
    CHECK(plan.bands[2].state == BandState::Empty);
    CHECK(on_route(trunk, plan.bands[2].at));
}

TEST_CASE("Overview plan: an active bypass joins the trunk at the merge",
          "[system_path][filament_path]") {
    auto d = multi();
    d->has_bypass = true;
    d->bypass_active = true;
    d->bypass_color = OWL;
    d->active_unit = -1;
    d->unit_hub_triggered[0] = false;
    const PathPlan& plan = plan_of(*d);
    const SysLayout L = compute_sys_layout(*d, AREA);
    REQUIRE(plan.bypass_route >= 0);
    REQUIRE(plan.trunk_route >= 0);
    const Route& bypass = plan.routes[plan.bypass_route];
    const Route& trunk = plan.routes[plan.trunk_route];
    CHECK(contiguous(bypass.path));
    // The idle trunk stops at the merge, where the bypass turns down to the nozzle.
    const pg::PathPoint merge = seg_end(trunk.path.segs[trunk.path.count - 1]);
    CHECK(on_route(bypass, merge));
    CHECK(near(merge, (float)L.center_x, merge.y));
    CHECK(near(seg_end(bypass.path.segs[bypass.path.count - 1]), (float)L.center_x, 326, 0.5f));
    CHECK(bypass.style[bypass.path.count - 1].filled);
    CHECK(lv_color_eq(bypass.style[bypass.path.count - 1].bore, lv_color_hex(OWL)));
}

TEST_CASE("Overview plan: toolchanger tools each get a continuous route to their nozzle",
          "[system_path][filament_path]") {
    auto d = multi();
    d->unit_count = 1;
    d->unit_x_positions[0] = 200;
    d->unit_topology[0] = 2;
    d->unit_tool_count[0] = 3;
    d->total_tools = 3;
    d->active_tool = 1;
    const PathPlan& plan = plan_of(*d);
    const SysLayout L = compute_sys_layout(*d, AREA);
    REQUIRE(plan.route_count == 3);
    const float nozzle_top = (float)(L.tools_y - small_tool_scale(*d) * 2);
    for (int i = 0; i < 3; i++) {
        CAPTURE(i);
        const Route& r = plan.routes[i];
        CHECK(contiguous(r.path));
        const pg::PathPoint end = seg_end(r.path.segs[r.path.count - 1]);
        CHECK(std::fabs(end.y - nozzle_top) < 0.5f);
        CHECK(r.style[0].filled == (i == plan.active_route));
    }
    REQUIRE(plan.active_route >= 0);
    const pg::PathPoint end = seg_end(
        plan.routes[plan.active_route].path.segs[plan.routes[plan.active_route].path.count - 1]);
    CHECK(std::fabs(end.x - (float)calc_tool_x(1, 3, L.x_off, L.width)) < 0.5f);
}

TEST_CASE_METHOD(LVGLTestFixture, "Overview canvas: the error setter reaches the hub band",
                 "[system_path][filament_path]") {
    lv_obj_t* canvas = ui_system_path_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    ui_system_path_canvas_set_unit_count(canvas, 2);
    ui_system_path_canvas_set_unit_x(canvas, 0, 100);
    ui_system_path_canvas_set_unit_x(canvas, 1, 300);
    for (int u = 0; u < 2; u++) {
        ui_system_path_canvas_set_unit_topology(canvas, u, 1);
        ui_system_path_canvas_set_unit_tools(canvas, u, 1, 0);
        ui_system_path_canvas_set_unit_hub_sensor(canvas, u, true, u == 0);
    }
    ui_system_path_canvas_set_total_tools(canvas, 1);
    ui_system_path_canvas_set_active_unit(canvas, 0);
    ui_system_path_canvas_set_filament_segment(canvas, static_cast<int>(PathSegment::NOZZLE));

    const SystemPathData* data = system_path_data(canvas);
    REQUIRE(data != nullptr);
    CHECK(stem_band(plan_of(*data), 100)->state == BandState::Active);

    ui_system_path_canvas_set_error_segment(canvas, static_cast<int>(PathSegment::OUTPUT));
    CHECK(stem_band(plan_of(*data), 100)->state == BandState::Error);

    ui_system_path_canvas_set_unit_lane(canvas, 1, static_cast<int>(PathSegment::OUTPUT), OWL);
    ui_system_path_canvas_set_unit_hub_sensor(canvas, 1, true, true);
    const SensorBand* owl = stem_band(plan_of(*data), 300);
    REQUIRE(owl != nullptr);
    CHECK(owl->state == BandState::Loaded);
    CHECK(lv_color_eq(owl->fill, lv_color_hex(OWL)));

    lv_obj_delete(canvas);
}

TEST_CASE("Overview plan: a MIXED unit's hub lanes pass through its box to the tool",
          "[system_path][filament_path]") {
    auto d = multi();
    d->unit_count = 1;
    d->unit_x_positions[0] = 200;
    d->unit_topology[0] = 3;
    d->unit_tool_count[0] = 3;
    d->total_tools = 3;
    d->active_tool = 2;
    d->unit_has_hub_sensor[0] = true;
    const SysLayout L = compute_sys_layout(*d, AREA);
    PathPlan plan;
    OverviewBoxes boxes;
    plan_overview(*d, L, plan, boxes);
    const HubInfo& box = boxes.hubs[0];
    REQUIRE(box.valid);
    const int32_t tool_x = calc_tool_x(2, 3, L.x_off, L.width);
    CHECK(box.hub_x == tool_x);
    const float top = (float)(box.mini_hub_y - box.mini_hub_h / 2);
    const float bottom = (float)(box.mini_hub_y + box.mini_hub_h / 2);
    const float nozzle_top = (float)(L.tools_y - small_tool_scale(*d) * 2);

    // One route enters the box top, crosses it unpainted, and leaves the
    // bottom for the nozzle, with the hub band at the box's output.
    const Route* through = nullptr;
    for (int i = 0; i < plan.route_count; i++)
        if (on_route(plan.routes[i], {(float)tool_x, top}))
            through = &plan.routes[i];
    REQUIRE(through != nullptr);
    CHECK(contiguous(through->path));
    CHECK(on_route(*through, {(float)tool_x, bottom}));
    CHECK(near(seg_end(through->path.segs[through->path.count - 1]), (float)tool_x, nozzle_top,
               0.5f));
    bool unpainted_inside = false;
    for (int i = 0; i < through->path.count; i++)
        unpainted_inside |= !through->style[i].painted &&
                            near(seg_end(through->path.segs[i]), (float)tool_x, bottom, 0.5f);
    CHECK(unpainted_inside);
    REQUIRE(plan.band_count == 1);
    CHECK(near(plan.bands[0].at, (float)tool_x, bottom, 0.5f));
    CHECK(plan.bands[0].on_box_edge);
    CHECK(plan.bands[0].state == BandState::Active);
}

// ============================================================================
// Several toolheads: one hub box per hub, with its buffer between hub and nozzle
// ============================================================================

namespace {

// Three HUB units over two toolheads on a 400x400 frame: units 0 and 1 feed
// toolhead 0 (one shared hub when @p shared, else two hubs of their own) and unit 2
// feeds toolhead 1.
std::unique_ptr<SystemPathData> two_toolheads(bool shared) {
    auto d = multi();
    d->unit_count = 3;
    d->unit_x_positions[0] = 60;
    d->unit_x_positions[1] = 140;
    d->unit_x_positions[2] = 320;
    for (int u = 0; u < 3; u++) {
        d->unit_topology[u] = 1;
        d->unit_tool_count[u] = 1;
        d->unit_has_hub_sensor[u] = false;
        d->unit_first_tool[u] = u == 2 ? 1 : 0;
        d->unit_hub_group[u] = (shared && u < 2) ? 1 : 0;
    }
    d->total_tools = 2;
    d->active_unit = -1;
    d->filament_loaded = false;
    d->filament_segment = PathSegment::NONE;
    d->unit_hub_triggered[0] = false;
    return d;
}

struct Planned {
    SysLayout L;
    PathPlan plan;
    OverviewBoxes boxes;
};

std::unique_ptr<Planned> plan_boxes(const SystemPathData& d) {
    auto p = std::make_unique<Planned>();
    p->L = compute_sys_layout(d, AREA);
    plan_overview(d, p->L, p->plan, p->boxes);
    return p;
}

// Routes whose last segment ends at (x, y).
int routes_ending_at(const PathPlan& plan, float x, float y) {
    int n = 0;
    for (int i = 0; i < plan.route_count; i++) {
        const Route& r = plan.routes[i];
        n += near(seg_end(r.path.segs[r.path.count - 1]), x, y, 0.5f);
    }
    return n;
}

} // namespace

TEST_CASE("Overview plan: units on one hub draw one hub box where their lanes join",
          "[system_path][filament_path][hub_groups]") {
    auto d = two_toolheads(true);
    const auto p = plan_boxes(*d);
    REQUIRE(p->plan.dropped == 0);
    const float nozzle_top = (float)(p->L.tools_y - small_tool_scale(*d) * 2);

    // One box for units 0 and 1, recorded at the first; unit 2 keeps its own.
    REQUIRE(p->boxes.hubs[0].valid);
    CHECK_FALSE(p->boxes.hubs[1].valid);
    REQUIRE(p->boxes.hubs[2].valid);
    const HubInfo& hub = p->boxes.hubs[0];
    const int32_t tool_x = calc_tool_x(0, 2, p->L.x_off, p->L.width);
    CHECK(hub.hub_x == tool_x);
    CHECK(hub.tool_x == tool_x);
    CHECK(hub.mini_hub_w >= p->boxes.hubs[2].mini_hub_w);

    // Both lanes reach the box top, inside its width, as continuous routes.
    const float top = (float)(hub.mini_hub_y - hub.mini_hub_h / 2);
    for (int u = 0; u < 2; u++) {
        CAPTURE(u);
        bool at_top = false;
        for (int i = 0; i < p->plan.route_count; i++) {
            const Route& r = p->plan.routes[i];
            if (!near(seg_start(r.path.segs[0]), (float)d->unit_x_positions[u], p->L.entry_y, 0.5f))
                continue;
            CHECK(contiguous(r.path));
            for (int k = 0; k < r.path.count; k++) {
                const pg::PathPoint e = seg_end(r.path.segs[k]);
                at_top |= std::fabs(e.y - top) < 0.5f &&
                          std::fabs(e.x - (float)hub.hub_x) <= (float)hub.mini_hub_w / 2;
            }
        }
        CHECK(at_top);
    }

    // One outlet leaves the box for the nozzle; unit 2's own hub has its own.
    CHECK(routes_ending_at(p->plan, (float)tool_x, nozzle_top) == 1);
    CHECK(p->plan.route_count == 3);

    SECTION("separate hubs when nothing is shared") {
        auto separate = two_toolheads(false);
        const auto q = plan_boxes(*separate);
        CHECK(q->boxes.hubs[0].valid);
        CHECK(q->boxes.hubs[1].valid);
        CHECK(q->boxes.hubs[0].hub_x != q->boxes.hubs[1].hub_x);
        CHECK(routes_ending_at(q->plan, (float)q->boxes.hubs[0].hub_x, nozzle_top) +
                  routes_ending_at(q->plan, (float)q->boxes.hubs[1].hub_x, nozzle_top) ==
              0); // both hubs sit beside the nozzle and angle in
    }
}

TEST_CASE("Overview plan: a hub of one unit is unchanged by its neighbors sharing a hub",
          "[system_path][filament_path][hub_groups]") {
    auto grouped = two_toolheads(true);
    auto alone = two_toolheads(false);
    const auto g = plan_boxes(*grouped);
    const auto a = plan_boxes(*alone);

    const HubInfo& hg = g->boxes.hubs[2];
    const HubInfo& ha = a->boxes.hubs[2];
    CHECK(hg.hub_x == ha.hub_x);
    CHECK(hg.tool_x == ha.tool_x);
    CHECK(hg.mini_hub_y == ha.mini_hub_y);
    CHECK(hg.mini_hub_w == ha.mini_hub_w);
    CHECK(hg.mini_hub_h == ha.mini_hub_h);
    CHECK(hg.buffer_h == 0);
    CHECK(ha.buffer_h == 0);

    // Its route is the same polyline in both plans.
    const float nozzle_top = (float)(g->L.tools_y - small_tool_scale(*grouped) * 2);
    const float x = (float)hg.tool_x;
    REQUIRE(routes_ending_at(g->plan, x, nozzle_top) == 1);
    REQUIRE(routes_ending_at(a->plan, x, nozzle_top) == 1);
    const Route *rg = nullptr, *ra = nullptr;
    for (int i = 0; i < g->plan.route_count; i++)
        if (near(seg_start(g->plan.routes[i].path.segs[0]), 320.0f, g->L.entry_y, 0.5f))
            rg = &g->plan.routes[i];
    for (int i = 0; i < a->plan.route_count; i++)
        if (near(seg_start(a->plan.routes[i].path.segs[0]), 320.0f, a->L.entry_y, 0.5f))
            ra = &a->plan.routes[i];
    REQUIRE((rg && ra));
    REQUIRE(rg->path.count == ra->path.count);
    for (int k = 0; k < rg->path.count; k++) {
        const pg::PathPoint e = seg_end(ra->path.segs[k]);
        CHECK(near(seg_end(rg->path.segs[k]), e.x, e.y, 0.01f));
    }
}

TEST_CASE("Overview plan: a hub's buffer box sits between the hub and its nozzle",
          "[system_path][filament_path][hub_groups]") {
    auto d = two_toolheads(true);

    SECTION("no buffer, no box") {
        const auto p = plan_boxes(*d);
        for (int u = 0; u < 3; u++)
            CHECK(p->boxes.hubs[u].buffer_h == 0);
    }

    SECTION("the shared hub's first unit decides") {
        d->unit_has_buffer[1] = true;
        CHECK(plan_boxes(*d)->boxes.hubs[0].buffer_h == 0);
        d->unit_has_buffer[0] = true;
        const auto p = plan_boxes(*d);
        const HubInfo& hub = p->boxes.hubs[0];
        REQUIRE(hub.buffer_h > 0);
        const int32_t hub_bottom = hub.mini_hub_y + hub.mini_hub_h / 2;
        const int32_t nozzle_top = p->L.tools_y - small_tool_scale(*d) * 2;
        CHECK(hub.buffer_y - hub.buffer_h / 2 > hub_bottom);
        CHECK(hub.buffer_y + hub.buffer_h / 2 < nozzle_top);
        // The hub with none draws none.
        CHECK(p->boxes.hubs[2].buffer_h == 0);
    }

    SECTION("a hub of one unit gets its box by the same rule") {
        d->unit_has_buffer[2] = true;
        const auto p = plan_boxes(*d);
        const HubInfo& hub = p->boxes.hubs[2];
        REQUIRE(hub.buffer_h > 0);
        CHECK(hub.buffer_y > hub.mini_hub_y + hub.mini_hub_h / 2);
        CHECK(p->boxes.hubs[0].buffer_h == 0);
    }

    SECTION("a hub beside a shared nozzle drops past its buffer, then angles in") {
        auto beside = two_toolheads(false);
        beside->unit_first_tool[1] = 0;
        beside->unit_has_buffer[0] = true;
        const auto p = plan_boxes(*beside);
        const HubInfo& hub = p->boxes.hubs[0];
        REQUIRE(hub.buffer_h > 0);
        REQUIRE(hub.hub_x != hub.tool_x);
        const float buffer_bottom = (float)(hub.buffer_y + hub.buffer_h / 2);
        CHECK(routes_ending_at(p->plan, (float)hub.tool_x,
                               (float)(p->L.tools_y - small_tool_scale(*beside) * 2)) >= 1);
        bool drops_to_buffer = false;
        for (int i = 0; i < p->plan.route_count; i++) {
            const Route& r = p->plan.routes[i];
            for (int k = 0; k < r.path.count; k++)
                drops_to_buffer |=
                    near(seg_end(r.path.segs[k]), (float)hub.hub_x, buffer_bottom, 0.5f);
        }
        CHECK(drops_to_buffer);
    }
}

namespace {

// @p count HUB units over two toolheads on a 400x400 frame: the first
// @p first_group_size share a hub on toolhead 0 and the rest share one on
// toolhead 1, each with a buffer. Every card center lies far beyond the canvas
// width, as when the card row has scrolled them all out of view.
std::unique_ptr<SystemPathData> scrolled_off_fleet(int count, int first_group_size) {
    auto d = multi();
    d->unit_count = count;
    d->total_tools = 2;
    d->active_unit = -1;
    d->filament_loaded = false;
    d->filament_segment = PathSegment::NONE;
    for (int u = 0; u < count && u < SystemPathData::MAX_UNITS; u++) {
        const bool first = u < first_group_size;
        d->unit_x_positions[u] = 1000 + 150 * u;
        d->unit_topology[u] = 1;
        d->unit_tool_count[u] = 1;
        d->unit_has_hub_sensor[u] = false;
        d->unit_hub_triggered[u] = false;
        d->unit_first_tool[u] = first ? 0 : 1;
        d->unit_hub_group[u] = first ? 1 : 2;
        d->unit_has_buffer[u] = true;
    }
    return d;
}

} // namespace

TEST_CASE("Overview plan: a hub group with every card scrolled off still draws its hub and buffer",
          "[system_path][filament_path][hub_groups]") {
    // The second group's units are the last cards of the row: four units split
    // 2 + 2, and twelve split 10 + 2.
    const auto [count, split] = GENERATE(std::pair<int, int>{4, 2}, std::pair<int, int>{12, 10});
    CAPTURE(count, split);
    auto d = scrolled_off_fleet(count, split);
    // Every unit has a slot in the canvas's per-unit arrays.
    REQUIRE(count <= SystemPathData::MAX_UNITS);
    const auto p = plan_boxes(*d);
    REQUIRE(p->plan.dropped == 0);

    for (int leader : {0, split}) {
        CAPTURE(leader);
        REQUIRE(p->boxes.hubs[leader].valid);
        const HubInfo& hub = p->boxes.hubs[leader];
        const int32_t nozzle_top = p->L.tools_y - small_tool_scale(*d) * 2;
        const int32_t half_w = hub.mini_hub_w / 2;
        CHECK(hub.hub_x - half_w >= p->L.x_off);
        CHECK(hub.hub_x + half_w < p->L.x_off + p->L.width);
        CHECK(hub.mini_hub_y + hub.mini_hub_h / 2 < nozzle_top);
        REQUIRE(hub.buffer_h > 0);
        CHECK(hub.buffer_y - hub.buffer_h / 2 > hub.mini_hub_y + hub.mini_hub_h / 2);
        CHECK(hub.buffer_y + hub.buffer_h / 2 < nozzle_top);
    }

    // The second group's lanes enter from the canvas edge, join its hub's top,
    // and one outlet reaches its nozzle.
    const HubInfo& hub = p->boxes.hubs[split];
    const float top = (float)(hub.mini_hub_y - hub.mini_hub_h / 2);
    const float edge = (float)(p->L.x_off + p->L.width);
    int joined = 0;
    for (int i = 0; i < p->plan.route_count; i++) {
        const Route& r = p->plan.routes[i];
        const pg::PathPoint start = seg_start(r.path.segs[0]);
        if (start.x < edge - 20.0f)
            continue;
        CHECK(contiguous(r.path));
        for (int k = 0; k < r.path.count; k++) {
            const pg::PathPoint e = seg_end(r.path.segs[k]);
            if (std::fabs(e.y - top) < 0.5f && std::fabs(e.x - (float)hub.hub_x) <= hub.mini_hub_w)
                joined++;
        }
    }
    CHECK(joined >= 2);
    CHECK(routes_ending_at(p->plan, (float)hub.tool_x,
                           (float)(p->L.tools_y - small_tool_scale(*d) * 2)) >= 1);
}

TEST_CASE("Overview plan: a shared hub is tinted when a member carries filament to it",
          "[system_path][filament_path][hub_groups]") {
    auto d = two_toolheads(true);
    const auto idle = plan_boxes(*d);
    CHECK(lv_color_eq(idle->boxes.hubs[0].hub_bg_color, d->color_hub_bg));

    d->active_unit = 1;
    d->filament_loaded = true;
    d->active_tool = 0;
    d->filament_segment = PathSegment::NOZZLE;
    const auto loaded = plan_boxes(*d);
    CHECK_FALSE(lv_color_eq(loaded->boxes.hubs[0].hub_bg_color, d->color_hub_bg));
    CHECK(lv_color_eq(loaded->boxes.hubs[2].hub_bg_color, d->color_hub_bg));
    CHECK(loaded->plan.active_route >= 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Overview canvas: the buffer setters carry label and severity",
                 "[system_path][filament_path][hub_groups]") {
    lv_obj_t* canvas = ui_system_path_canvas_create(lv_screen_active());
    REQUIRE(canvas != nullptr);
    const SystemPathData* data = system_path_data(canvas);
    REQUIRE(data != nullptr);

    helix::ui::ui_system_path_canvas_set_buffer(canvas, true, 2, "BUF");
    CHECK(data->has_buffer);
    CHECK(data->buffer_fault == 2);
    CHECK(std::string(data->buffer_label) == "BUF");
    helix::ui::ui_system_path_canvas_set_buffer(canvas, false, 0, "");
    CHECK_FALSE(data->has_buffer);

    helix::ui::ui_system_path_canvas_set_unit_hub(canvas, 3, 2, true, 1, "FPS");
    CHECK(data->unit_hub_group[3] == 2);
    CHECK(data->unit_has_buffer[3]);
    CHECK(data->unit_buffer_fault[3] == 1);
    CHECK(std::string(data->unit_buffer_label[3]) == "FPS");
    CHECK_FALSE(data->unit_has_buffer[2]);
    lv_obj_delete(canvas);
}

// ----------------------------------------------------------------------------
// The mini-hub row: boxes of HUB units never overlap each other or a route.
// ----------------------------------------------------------------------------

namespace {

// A HUB unit feeding toolhead `tool`.
void hub_unit(SystemPathData& d, int u, int tool, int32_t stem_x) {
    d.unit_x_positions[u] = stem_x;
    d.unit_topology[u] = 1;
    d.unit_tool_count[u] = 1;
    d.unit_first_tool[u] = tool;
}

std::unique_ptr<SystemPathData> hub_row_data(int units, int tools) {
    auto d = multi();
    d->unit_count = units;
    d->total_tools = tools;
    d->active_unit = -1;
    d->filament_loaded = false;
    d->hub_width = 70;
    d->tube_gauge = 5;
    return d;
}

struct HubRowPlan {
    SysLayout L;
    PathPlan plan;
    OverviewBoxes boxes;
};

std::unique_ptr<HubRowPlan> plan_hub_row(const SystemPathData& d, int32_t width) {
    auto out = std::make_unique<HubRowPlan>();
    const lv_area_t area = {0, 0, width - 1, 399};
    out->L = compute_sys_layout(d, area);
    plan_overview(d, out->L, out->plan, out->boxes);
    return out;
}

// The StealthChanger overview: an ACE straight to T0-T3, then hub units on T4,
// T5 and (two of them) T6.
std::unique_ptr<SystemPathData> stealth_row() {
    auto d = hub_row_data(5, 7);
    d->unit_x_positions[0] = 60;
    d->unit_topology[0] = 2;
    d->unit_tool_count[0] = 4;
    d->unit_first_tool[0] = 0;
    hub_unit(*d, 1, 4, 200);
    hub_unit(*d, 2, 5, 330);
    hub_unit(*d, 3, 6, 460);
    hub_unit(*d, 4, 6, 560);
    return d;
}

int32_t box_left(const HubInfo& h) {
    return h.hub_x - h.mini_hub_w / 2;
}
int32_t box_right(const HubInfo& h) {
    return h.hub_x + h.mini_hub_w / 2;
}

// Hub boxes of units [first, last], in unit order.
void check_row_clear(const SystemPathData& d, const HubRowPlan& p, int first, int last) {
    const int32_t gap = LV_MAX(4, d.tube_gauge);
    for (int u = first; u <= last; ++u) {
        CAPTURE(u);
        const HubInfo& a = p.boxes.hubs[u];
        REQUIRE(a.valid);
        CHECK(box_left(a) >= p.L.x_off);
        CHECK(box_right(a) <= p.L.x_off + p.L.width);
        for (int v = u + 1; v <= last; ++v) {
            CAPTURE(v);
            const HubInfo& b = p.boxes.hubs[v];
            const bool a_first = a.hub_x <= b.hub_x;
            const int32_t clear = a_first ? box_left(b) - box_right(a) : box_left(a) - box_right(b);
            CHECK(clear >= gap);
        }
    }
}

} // namespace

TEST_CASE("Overview plan: the StealthChanger hub row has no overlapping boxes",
          "[system_path][hub_row]") {
    auto d = stealth_row();
    auto p = plan_hub_row(*d, 580);
    check_row_clear(*d, *p, 1, 4);

    // None of the boxes covers the vertical drop of a direct route to T0-T3.
    for (int t = 0; t < 4; ++t) {
        const int32_t tx = calc_tool_x(t, 7, p->L.x_off, p->L.width);
        for (int u = 1; u <= 4; ++u) {
            CAPTURE(t, u);
            CHECK((tx < box_left(p->boxes.hubs[u]) || tx > box_right(p->boxes.hubs[u])));
        }
    }
    // Every hub route lands on its box.
    for (int u = 1; u <= 4; ++u) {
        bool landed = false;
        for (int r = 0; r < p->plan.route_count; ++r)
            landed |=
                on_route(p->plan.routes[r],
                         {(float)p->boxes.hubs[u].hub_x,
                          (float)(p->boxes.hubs[u].mini_hub_y - p->boxes.hubs[u].mini_hub_h / 2)});
        CHECK(landed);
    }
}

TEST_CASE("Overview plan: two units sharing a toolhead clear the neighbor's hub",
          "[system_path][hub_row]") {
    auto d = hub_row_data(4, 3);
    hub_unit(*d, 0, 0, 60);
    hub_unit(*d, 1, 1, 150);
    hub_unit(*d, 2, 1, 200);
    hub_unit(*d, 3, 2, 260);
    auto p = plan_hub_row(*d, 300);
    check_row_clear(*d, *p, 0, 3);
    // The pair still straddles its nozzle: the rank order is kept.
    CHECK(p->boxes.hubs[1].hub_x < p->boxes.hubs[2].hub_x);
}

TEST_CASE("Overview plan: a hub row too wide for the canvas narrows, then says H",
          "[system_path][hub_row]") {
    auto d = hub_row_data(8, 8);
    for (int u = 0; u < 8; ++u)
        hub_unit(*d, u, u, 40 + u * 40);
    auto narrow = plan_hub_row(*d, 400);
    check_row_clear(*d, *narrow, 0, 7);
    CHECK(narrow->boxes.hubs[0].mini_hub_w < d->hub_width * 2 / 3);
    CHECK(narrow->boxes.hubs[0].mini_hub_w >= d->hub_width / 2);
    CHECK_FALSE(narrow->boxes.hubs[0].short_label);

    auto tight = plan_hub_row(*d, 300);
    check_row_clear(*d, *tight, 0, 7);
    CHECK(tight->boxes.hubs[0].short_label);
    CHECK(tight->boxes.hubs[0].mini_hub_w == d->hub_width * 2 / 5);
}

TEST_CASE("Overview plan: a hub row that fits keeps its boxes on their toolheads",
          "[system_path][hub_row]") {
    auto d = hub_row_data(2, 2);
    hub_unit(*d, 0, 0, 100);
    hub_unit(*d, 1, 1, 400);
    auto p = plan_hub_row(*d, 580);
    for (int u = 0; u < 2; ++u) {
        CAPTURE(u);
        const HubInfo& h = p->boxes.hubs[u];
        CHECK(h.hub_x == h.tool_x);
        CHECK(h.mini_hub_w == d->hub_width * 2 / 3);
        CHECK_FALSE(h.short_label);
    }

    // A shared nozzle fans its pair by one box pitch.
    auto s = hub_row_data(2, 2);
    hub_unit(*s, 0, 0, 100);
    hub_unit(*s, 1, 0, 400);
    auto sp = plan_hub_row(*s, 580);
    const int32_t pitch = s->hub_width * 2 / 3 + 5;
    CHECK(sp->boxes.hubs[1].hub_x - sp->boxes.hubs[0].hub_x == pitch);
    CHECK(sp->boxes.hubs[0].mini_hub_w == s->hub_width * 2 / 3);
}

TEST_CASE("Overview plan: a crowded hub row steps around a direct route's drop",
          "[system_path][hub_row]") {
    // Four hubs on the left nozzle fan across the middle nozzle's vertical run.
    auto d = hub_row_data(5, 3);
    d->unit_x_positions[0] = 150;
    d->unit_topology[0] = 2;
    d->unit_tool_count[0] = 1;
    d->unit_first_tool[0] = 1;
    for (int u = 1; u < 5; ++u)
        hub_unit(*d, u, 0, 40 + u * 20);
    auto p = plan_hub_row(*d, 300);
    check_row_clear(*d, *p, 1, 4);
    const int32_t drop_x = calc_tool_x(1, 3, p->L.x_off, p->L.width);
    for (int u = 1; u <= 4; ++u) {
        CAPTURE(u);
        CHECK((drop_x < box_left(p->boxes.hubs[u]) - d->tube_gauge / 2 ||
               drop_x > box_right(p->boxes.hubs[u]) + d->tube_gauge / 2));
    }
}

TEST_CASE("Overview plan: a crowded hub row keeps a shared hub wide and narrows the buffers",
          "[system_path][hub_row][hub_groups]") {
    // Three single-unit hubs, then units 3-6 on one shared hub (wider than a
    // single-unit box, for its four lanes), every hub with a buffer box.
    auto d = hub_row_data(7, 4);
    for (int u = 0; u < 7; ++u) {
        hub_unit(*d, u, LV_MIN(u, 3), 60 + u * 80);
        d->unit_hub_group[u] = u >= 3 ? 1 : 0;
        d->unit_has_buffer[u] = true;
    }
    const auto roomy = plan_hub_row(*d, 1200);
    REQUIRE(roomy->boxes.hubs[3].valid);
    REQUIRE_FALSE(roomy->boxes.hubs[4].valid);
    REQUIRE_FALSE(roomy->boxes.hubs[6].valid);
    const int32_t shared_w = roomy->boxes.hubs[3].mini_hub_w;
    REQUIRE(shared_w > roomy->boxes.hubs[0].mini_hub_w);

    const auto tight = plan_hub_row(*d, 200);
    // The single-unit boxes had to narrow; the shared one did not.
    REQUIRE(tight->boxes.hubs[0].mini_hub_w < roomy->boxes.hubs[0].mini_hub_w);
    CHECK(tight->boxes.hubs[3].mini_hub_w == shared_w);
    CHECK_FALSE(tight->boxes.hubs[3].short_label);

    const int32_t gap = LV_MAX(4, d->tube_gauge);
    const int boxes[] = {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i) {
        const HubInfo& a = tight->boxes.hubs[boxes[i]];
        CAPTURE(i);
        REQUIRE(a.buffer_h > 0);
        // A buffer is drawn centered on its hub, so no wider than the hub keeps
        // the buffer row as clear as the hub row.
        CHECK(a.buffer_w <= a.mini_hub_w);
        CHECK(box_left(a) >= tight->L.x_off);
        CHECK(box_right(a) <= tight->L.x_off + tight->L.width);
        if (i > 0)
            CHECK(box_left(a) - box_right(tight->boxes.hubs[boxes[i - 1]]) >= gap);
    }
}
