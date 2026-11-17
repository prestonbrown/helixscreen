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
#include "src/ui/filament/ui_system_path_plan.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::syspath;
using namespace helix::ui::fpath;
using namespace fpath_test;
using helix::PathSegment;

namespace {

const lv_color_t IDLE = lv_color_hex(0x555555);
const lv_color_t kBg = lv_color_hex(0x202020);
const lv_color_t kAccent = lv_color_hex(0x3399FF);
constexpr uint32_t ACTIVE = 0xE53935;
constexpr uint32_t kOwl = 0x1E88E5;
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
    d->color_bg = kBg;
    d->color_accent = kAccent;
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

TEST_CASE("Overview plan: a unit stem placed past the canvas edge is drawn on the canvas",
          "[system_path][filament_path]") {
    // A card laid out wider than the canvas reports a center the canvas cannot reach.
    auto d = multi();
    d->unit_x_positions[1] = 1000;
    const PathPlan& plan = plan_of(*d);
    const SysLayout L = compute_sys_layout(*d, AREA);
    REQUIRE(plan.route_count == 2);
    const pg::PathPoint stem = seg_start(plan.routes[1].path.segs[0]);
    CHECK(stem.x <= (float)(L.x_off + L.width));
    CHECK(stem.x > (float)d->unit_x_positions[0]);
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
        d->unit_lane_color[1] = kOwl;
        const PathPlan& plan = plan_of(*d);
        const SensorBand* band = stem_band(plan, 300);
        REQUIRE(band != nullptr);
        CHECK(band->state == BandState::Loaded);
        CHECK(lv_color_eq(band->fill, lv_color_hex(kOwl)));
        // Its tube is filled down to the combiner, in the lane color, plain walls.
        const Route& r = plan.routes[1];
        CHECK(r.style[0].filled);
        CHECK(r.style[0].wall == TubeWall::Plain);
        CHECK(lv_color_eq(r.style[r.path.count - 1].bore, lv_color_hex(kOwl)));
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
    d->bypass_color = kOwl;
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
    CHECK(lv_color_eq(bypass.style[bypass.path.count - 1].bore, lv_color_hex(kOwl)));
}

TEST_CASE("Overview plan: a span too short to clamp into yields its midpoint",
          "[system_path][filament_path]") {
    CHECK(clamp_span(10, 5, 20) == 10);
    CHECK(clamp_span(10, 15, 20) == 15);
    CHECK(clamp_span(10, 25, 20) == 20);
    CHECK(clamp_span(30, 0, 10) == 20);
    CHECK(clamp_span(30, 50, 10) == 20);
}

TEST_CASE("Overview plan: a bypass spool on a canvas too narrow for it stays centred",
          "[system_path][filament_path]") {
    auto d = multi();
    d->unit_count = 1;
    d->unit_x_positions[0] = 20;
    d->has_bypass = true;
    constexpr lv_area_t kNarrow = {0, 0, 39, 399};
    const BypassGeometry g = compute_bypass_geometry(*d, kNarrow);
    CHECK(g.bypass_x == 20);
}

TEST_CASE("Overview plan: tool routes on a short canvas stay between their ends",
          "[system_path][filament_path]") {
    auto d = multi();
    d->unit_count = 1;
    d->unit_x_positions[0] = 200;
    d->unit_topology[0] = 2;
    d->unit_tool_count[0] = 3;
    d->total_tools = 3;
    d->active_tool = 1;
    constexpr lv_area_t kShort = {0, 0, 399, 59};
    PathPlan plan;
    OverviewBoxes boxes;
    plan_overview(*d, compute_sys_layout(*d, kShort), plan, boxes);
    REQUIRE(plan.route_count == 3);
    for (int i = 0; i < plan.route_count; i++) {
        CAPTURE(i);
        const Route& r = plan.routes[i];
        CHECK(contiguous(r.path));
        const float top = seg_start(r.path.segs[0]).y;
        const float bottom = seg_end(r.path.segs[r.path.count - 1]).y;
        for (int k = 0; k < r.path.count; k++) {
            CAPTURE(k);
            CHECK(seg_end(r.path.segs[k]).y >= top - 0.5f);
            CHECK(seg_end(r.path.segs[k]).y <= bottom + 0.5f);
        }
    }
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

    ui_system_path_canvas_set_unit_lane(canvas, 1, static_cast<int>(PathSegment::OUTPUT), kOwl);
    ui_system_path_canvas_set_unit_hub_sensor(canvas, 1, true, true);
    const SensorBand* owl = stem_band(plan_of(*data), 300);
    REQUIRE(owl != nullptr);
    CHECK(owl->state == BandState::Loaded);
    CHECK(lv_color_eq(owl->fill, lv_color_hex(kOwl)));

    lv_obj_delete(canvas);
}

// ============================================================================
// Hubs: a box only where two or more routes enter one nozzle
// ============================================================================

namespace {

// Three HUB units over two toolheads on a 400x400 frame: units 0 and 1 feed
// toolhead 0 (one shared hub when @p shared, else two hubs of their own) and unit 2
// feeds toolhead 1 alone.
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

// The route that starts at (x, entry_y), or nullptr.
const Route* route_from(const Planned& p, float x) {
    for (int i = 0; i < p.plan.route_count; i++)
        if (near(seg_start(p.plan.routes[i].path.segs[0]), x, (float)p.L.entry_y, 0.5f))
            return &p.plan.routes[i];
    return nullptr;
}

int hub_boxes(const OverviewBoxes& boxes) {
    int n = 0;
    for (const HubInfo& h : boxes.hubs)
        n += h.valid;
    return n;
}

float nozzle_top_of(const SystemPathData& d, const SysLayout& L) {
    return (float)(L.tools_y - small_tool_scale(d) * 2);
}

// Some segment of @p r ends on the top edge of @p hub, inside its width.
bool enters_top(const Route& r, const HubInfo& hub) {
    const float top = (float)(hub.mini_hub_y - hub.mini_hub_h / 2);
    for (int k = 0; k < r.path.count; k++) {
        const pg::PathPoint e = seg_end(r.path.segs[k]);
        if (std::fabs(e.y - top) < 0.5f &&
            std::fabs(e.x - (float)hub.hub_x) <= (float)hub.mini_hub_w / 2)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("Overview plan: the nozzle feed counts the routes into each nozzle",
          "[system_path][hub_groups]") {
    SECTION("several toolheads: a HUB unit feeds its first tool") {
        auto d = two_toolheads(false);
        const NozzleFeed t0 = nozzle_feed(*d, 0);
        CHECK(t0.routes == 2);
        CHECK(t0.junction);
        CHECK_FALSE(t0.hub_sensor);
        CHECK_FALSE(t0.buffer);
        const NozzleFeed t1 = nozzle_feed(*d, 1);
        CHECK(t1.routes == 1);
        CHECK_FALSE(t1.junction);

        d->unit_has_hub_sensor[1] = true;
        d->unit_has_buffer[1] = true;
        const NozzleFeed sensed = nozzle_feed(*d, 0);
        CHECK(sensed.hub_sensor);
        CHECK(sensed.buffer);
        CHECK(sensed.buffer_unit == 1);

        d->unit_absent[1] = true;
        CHECK(nozzle_feed(*d, 0).routes == 1);
        CHECK_FALSE(nozzle_feed(*d, 0).junction);
    }
    SECTION("a PARALLEL unit feeds each of its tools, with no hub of its own") {
        auto d = two_toolheads(false);
        d->unit_count = 2;
        d->unit_topology[0] = 2;
        d->unit_tool_count[0] = 2;
        d->unit_has_hub_sensor[0] = true;
        d->unit_first_tool[1] = 1;
        CHECK(nozzle_feed(*d, 0).routes == 1);
        const NozzleFeed t1 = nozzle_feed(*d, 1);
        CHECK(t1.routes == 2);
        CHECK(t1.junction);
        CHECK_FALSE(t1.hub_sensor);
    }
    SECTION("a MIXED unit's hub sits on its last tool") {
        auto d = two_toolheads(false);
        d->unit_count = 1;
        d->unit_topology[0] = 3;
        d->unit_tool_count[0] = 2;
        d->unit_has_hub_sensor[0] = true;
        CHECK_FALSE(nozzle_feed(*d, 0).hub_sensor);
        CHECK(nozzle_feed(*d, 1).hub_sensor);
        CHECK(nozzle_feed(*d, 1).routes == 1);
    }
    SECTION("one toolhead: every present unit feeds it") {
        auto d = multi();
        CHECK(nozzle_feed(*d, 0).routes == 2);
        CHECK(nozzle_feed(*d, 0).junction);
        CHECK(nozzle_feed(*d, 0).hub_sensor);
        d->has_buffer = true;
        CHECK(nozzle_feed(*d, 0).buffer);
        CHECK(nozzle_feed(*d, 0).buffer_unit == -1);
        d->unit_count = 1;
        CHECK(nozzle_feed(*d, 0).routes == 1);
        CHECK_FALSE(nozzle_feed(*d, 0).junction);
    }
}

TEST_CASE("Overview plan: a single route into a nozzle draws no hub box",
          "[system_path][filament_path][hub_groups]") {
    SECTION("several toolheads") {
        auto d = two_toolheads(false);
        const auto p = plan_boxes(*d);
        CHECK_FALSE(p->boxes.hubs[1].valid);
        CHECK(p->boxes.hubs[1].buffer_h == 0);
        // Unit 2's route runs unbroken and painted to its nozzle.
        const Route* r = route_from(*p, 320);
        REQUIRE(r != nullptr);
        CHECK(contiguous(r->path));
        const float tool_x = (float)calc_tool_x(1, 2, p->L.x_off, p->L.width);
        CHECK(
            near(seg_end(r->path.segs[r->path.count - 1]), tool_x, nozzle_top_of(*d, p->L), 0.5f));
        for (int k = 0; k < r->path.count; k++) {
            CAPTURE(k);
            CHECK(r->style[k].painted);
        }
    }
    SECTION("one toolhead, one unit") {
        auto d = multi();
        d->unit_count = 1;
        d->unit_x_positions[0] = 200;
        const auto p = plan_boxes(*d);
        CHECK_FALSE(p->boxes.combiner);
        REQUIRE(p->plan.active_route >= 0);
        const Route& r = p->plan.routes[p->plan.active_route];
        CHECK(contiguous(r.path));
        CHECK(near(seg_end(r.path.segs[r.path.count - 1]), 200, 326, 0.5f));
        for (int k = 0; k < r.path.count; k++) {
            CAPTURE(k);
            CHECK(r.style[k].painted);
        }
    }
    SECTION("one toolhead, two units: the combiner") {
        CHECK(plan_boxes(*multi())->boxes.combiner);
    }
}

TEST_CASE("Overview plan: two units into one nozzle meet in exactly one hub box",
          "[system_path][filament_path][hub_groups]") {
    // Two hubs of their own, or one shared hub: either way one box.
    const bool shared = GENERATE(false, true);
    CAPTURE(shared);
    auto d = two_toolheads(shared);
    const auto p = plan_boxes(*d);
    REQUIRE(p->plan.dropped == 0);
    CHECK(hub_boxes(p->boxes) == 1);
    const HubInfo& hub = p->boxes.hubs[0];
    REQUIRE(hub.valid);
    const int32_t tool_x = calc_tool_x(0, 2, p->L.x_off, p->L.width);
    CHECK(hub.hub_x == tool_x);
    CHECK(hub.tool_x == tool_x);

    // Both lanes enter its top as continuous routes; one outlet reaches the nozzle.
    for (int u = 0; u < 2; u++) {
        CAPTURE(u);
        const Route* r = route_from(*p, (float)d->unit_x_positions[u]);
        REQUIRE(r != nullptr);
        CHECK(contiguous(r->path));
        CHECK(enters_top(*r, hub));
    }
    CHECK(routes_ending_at(p->plan, (float)tool_x, nozzle_top_of(*d, p->L)) == 1);
    CHECK(p->plan.route_count == 3);
}

TEST_CASE("Overview plan: a direct tool route into a hub's nozzle enters the same box",
          "[system_path][filament_path][hub_groups]") {
    // A PARALLEL unit on tools 0 and 1, and a HUB unit on tool 1.
    auto d = two_toolheads(false);
    d->unit_count = 2;
    d->unit_x_positions[0] = 100;
    d->unit_x_positions[1] = 300;
    d->unit_topology[0] = 2;
    d->unit_tool_count[0] = 2;
    d->unit_first_tool[1] = 1;
    const auto p = plan_boxes(*d);
    CHECK(hub_boxes(p->boxes) == 1);
    CHECK_FALSE(p->boxes.hubs[0].valid);
    const HubInfo& hub = p->boxes.hubs[1];
    REQUIRE(hub.valid);
    int entering = 0;
    for (int i = 0; i < p->plan.route_count; i++)
        entering += enters_top(p->plan.routes[i], hub);
    CHECK(entering == 2);
    const float nozzle_top = nozzle_top_of(*d, p->L);
    CHECK(routes_ending_at(p->plan, (float)calc_tool_x(1, 2, p->L.x_off, p->L.width), nozzle_top) ==
          1);
    CHECK(routes_ending_at(p->plan, (float)calc_tool_x(0, 2, p->L.x_off, p->L.width), nozzle_top) ==
          1);
}

TEST_CASE("Overview plan: a junction's hub with a buffer is one box",
          "[system_path][filament_path][hub_groups]") {
    SECTION("several toolheads") {
        auto d = two_toolheads(false);
        d->unit_has_buffer[0] = true;
        const auto p = plan_boxes(*d);
        const HubInfo& hub = p->boxes.hubs[0];
        REQUIRE(hub.valid);
        CHECK(hub.buffer_in_hub);
        CHECK(hub.buffer_unit == 0);
        CHECK(hub.buffer_h == 0); // no separate buffer box
        CHECK(hub_boxes(p->boxes) == 1);
    }
    SECTION("one toolhead") {
        auto d = multi();
        d->has_buffer = true;
        const auto p = plan_boxes(*d);
        CHECK(p->boxes.combiner);
        CHECK(p->boxes.combiner_buffer);
    }
}

TEST_CASE("Overview plan: a buffer on a single route stays an inline box with no hub",
          "[system_path][filament_path][hub_groups]") {
    SECTION("several toolheads") {
        auto d = two_toolheads(false);
        d->unit_has_buffer[2] = true;
        const auto p = plan_boxes(*d);
        const HubInfo& line = p->boxes.hubs[1];
        CHECK_FALSE(line.valid);
        CHECK_FALSE(line.buffer_in_hub);
        REQUIRE(line.buffer_h > 0);
        CHECK(line.buffer_unit == 2);
        const int32_t tool_x = calc_tool_x(1, 2, p->L.x_off, p->L.width);
        CHECK(line.hub_x == tool_x);
        CHECK(line.buffer_y - line.buffer_h / 2 > p->L.merge_y);
        CHECK(line.buffer_y + line.buffer_h / 2 < nozzle_top_of(*d, p->L));
        // The route passes through the buffer on its way down.
        const Route* r = route_from(*p, 320);
        REQUIRE(r != nullptr);
        bool through = false;
        for (int k = 0; k < r->path.count; k++) {
            const pg::PathPoint a = seg_start(r->path.segs[k]);
            const pg::PathPoint b = seg_end(r->path.segs[k]);
            through |= std::fabs(a.x - (float)tool_x) < 0.5f &&
                       std::fabs(b.x - (float)tool_x) < 0.5f && a.y <= (float)line.buffer_y &&
                       b.y >= (float)line.buffer_y;
        }
        CHECK(through);
        // The junction on tool 0 has none.
        CHECK_FALSE(p->boxes.hubs[0].buffer_in_hub);
    }
    SECTION("one toolhead, one unit") {
        auto d = multi();
        d->unit_count = 1;
        d->has_buffer = true;
        const auto p = plan_boxes(*d);
        CHECK_FALSE(p->boxes.combiner);
        CHECK_FALSE(p->boxes.combiner_buffer);
    }
}

TEST_CASE("Overview plan: a junction shows a hub sensor only when one of its hubs has one",
          "[system_path][filament_path][hub_groups]") {
    auto d = two_toolheads(false);
    {
        const auto p = plan_boxes(*d);
        CHECK(p->plan.band_count == 0);
    }
    d->unit_has_hub_sensor[1] = true;
    d->unit_hub_triggered[1] = true;
    d->unit_lane_segment[1] = PathSegment::OUTPUT;
    d->unit_lane_color[1] = kOwl;
    const auto p = plan_boxes(*d);
    const HubInfo& hub = p->boxes.hubs[0];
    REQUIRE(p->plan.band_count == 1);
    const SensorBand& band = p->plan.bands[0];
    CHECK(band.on_box_edge);
    CHECK(near(band.at, (float)hub.hub_x, (float)(hub.mini_hub_y + hub.mini_hub_h / 2), 0.5f));
    CHECK(band.state == BandState::Loaded);
    CHECK(lv_color_eq(band.fill, lv_color_hex(kOwl)));
}

TEST_CASE("Overview plan: a lone hub's sensor sits on its line",
          "[system_path][filament_path][hub_groups]") {
    auto d = two_toolheads(false);
    d->unit_has_hub_sensor[2] = true;
    const auto p = plan_boxes(*d);
    REQUIRE(p->plan.band_count == 1);
    const SensorBand& band = p->plan.bands[0];
    CHECK_FALSE(band.on_box_edge);
    CHECK(std::fabs(band.at.x - (float)calc_tool_x(1, 2, p->L.x_off, p->L.width)) < 0.5f);
    const Route* r = route_from(*p, 320);
    REQUIRE(r != nullptr);
    CHECK(on_route(*r, band.at));
}

TEST_CASE("Overview plan: a MIXED unit alone on its hub tool draws no box",
          "[system_path][filament_path]") {
    auto d = multi();
    d->unit_count = 1;
    d->unit_x_positions[0] = 200;
    d->unit_topology[0] = 3;
    d->unit_tool_count[0] = 3;
    d->total_tools = 3;
    d->active_tool = 2;
    d->unit_has_hub_sensor[0] = true;
    const auto p = plan_boxes(*d);
    CHECK(hub_boxes(p->boxes) == 0);
    const int32_t tool_x = calc_tool_x(2, 3, p->L.x_off, p->L.width);
    // Its hub sensor reads the hub-routed lanes on their own line.
    REQUIRE(p->plan.band_count == 1);
    CHECK_FALSE(p->plan.bands[0].on_box_edge);
    CHECK(std::fabs(p->plan.bands[0].at.x - (float)tool_x) < 0.5f);
    CHECK(p->plan.bands[0].state == BandState::Active);
    REQUIRE(p->plan.active_route >= 0);
    const Route& r = p->plan.routes[p->plan.active_route];
    CHECK(contiguous(r.path));
    CHECK(on_route(r, p->plan.bands[0].at));
    CHECK(
        near(seg_end(r.path.segs[r.path.count - 1]), (float)tool_x, nozzle_top_of(*d, p->L), 0.5f));
}

TEST_CASE("Overview plan: the active unit at a junction carries the outlet",
          "[system_path][filament_path][hub_groups]") {
    auto d = two_toolheads(false);
    d->active_unit = 1;
    d->active_tool = 0;
    d->filament_loaded = true;
    d->filament_segment = PathSegment::NOZZLE;
    const auto p = plan_boxes(*d);
    REQUIRE(p->plan.active_route >= 0);
    const Route& r = p->plan.routes[p->plan.active_route];
    CHECK(near(seg_start(r.path.segs[0]), 140, (float)p->L.entry_y, 0.5f));
    CHECK(near(seg_end(r.path.segs[r.path.count - 1]),
               (float)calc_tool_x(0, 2, p->L.x_off, p->L.width), nozzle_top_of(*d, p->L), 0.5f));
    // The box tints toward the filament passing through it.
    CHECK_FALSE(lv_color_eq(p->boxes.hubs[0].hub_bg_color, d->color_hub_bg));
    const auto idle = plan_boxes(*two_toolheads(false));
    CHECK(lv_color_eq(idle->boxes.hubs[0].hub_bg_color, d->color_hub_bg));
}

TEST_CASE("Overview plan: a lone route is the same polyline whatever its neighbors share",
          "[system_path][filament_path][hub_groups]") {
    auto grouped = two_toolheads(true);
    auto alone = two_toolheads(false);
    const auto g = plan_boxes(*grouped);
    const auto a = plan_boxes(*alone);
    const Route* rg = route_from(*g, 320);
    const Route* ra = route_from(*a, 320);
    REQUIRE((rg && ra));
    REQUIRE(rg->path.count == ra->path.count);
    for (int k = 0; k < rg->path.count; k++) {
        const pg::PathPoint e = seg_end(ra->path.segs[k]);
        CHECK(near(seg_end(rg->path.segs[k]), e.x, e.y, 0.01f));
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
    for (int u = 0; u < count && u < SystemPathData::kMaxUnits; u++) {
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

TEST_CASE("Overview plan: a junction with every card scrolled off still draws its hub",
          "[system_path][filament_path][hub_groups]") {
    // The second group's units are the last cards of the row: four units split
    // 2 + 2, and twelve split 10 + 2.
    const auto [count, split] = GENERATE(std::pair<int, int>{4, 2}, std::pair<int, int>{12, 10});
    CAPTURE(count, split);
    auto d = scrolled_off_fleet(count, split);
    REQUIRE(count <= SystemPathData::kMaxUnits);
    const auto p = plan_boxes(*d);
    REQUIRE(p->plan.dropped == 0);
    const float nozzle_top = nozzle_top_of(*d, p->L);

    for (int tool : {0, 1}) {
        CAPTURE(tool);
        const HubInfo& hub = p->boxes.hubs[tool];
        REQUIRE(hub.valid);
        const int32_t half_w = hub.mini_hub_w / 2;
        CHECK(hub.hub_x - half_w >= p->L.x_off);
        CHECK(hub.hub_x + half_w < p->L.x_off + p->L.width);
        CHECK(hub.mini_hub_y + hub.mini_hub_h / 2 < nozzle_top);
        CHECK(hub.buffer_in_hub);
    }

    // The second junction's two lanes join its top, and one outlet reaches its
    // nozzle. The parked stems stand apart, one tube pitch or more.
    const HubInfo& hub = p->boxes.hubs[1];
    int joined = 0;
    std::vector<float> stems;
    for (int i = 0; i < p->plan.route_count; i++) {
        const Route& r = p->plan.routes[i];
        CHECK(contiguous(r.path));
        joined += enters_top(r, hub);
        stems.push_back(seg_start(r.path.segs[0]).x);
    }
    CHECK(joined == count - split);
    std::sort(stems.begin(), stems.end());
    for (size_t i = 1; i < stems.size(); i++)
        CHECK(stems[i] - stems[i - 1] >= (float)(d->tube_gauge + helix::ui::kHaloWidthExtra));
    CHECK(routes_ending_at(p->plan, (float)hub.tool_x, nozzle_top) == 1);
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
// The junction row: hub boxes never overlap each other or a direct drop.
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

// Every drawn hub box lies inside the canvas, @p gap clear of the next.
void check_row_clear(const SystemPathData& d, const HubRowPlan& p) {
    const int32_t gap = LV_MAX(4, d.tube_gauge);
    const HubInfo* prev = nullptr;
    for (int t = 0; t < SystemPathData::kMaxTools; ++t) {
        const HubInfo& h = p.boxes.hubs[t];
        if (!h.valid)
            continue;
        CAPTURE(t);
        CHECK(box_left(h) >= p.L.x_off);
        CHECK(box_right(h) <= p.L.x_off + p.L.width);
        if (prev)
            CHECK(box_left(h) - box_right(*prev) >= gap);
        prev = &h;
    }
}

} // namespace

TEST_CASE("Overview plan: the StealthChanger row has one hub, where two units meet",
          "[system_path][hub_row]") {
    auto d = stealth_row();
    auto p = plan_hub_row(*d, 580);
    CHECK(hub_boxes(p->boxes) == 1);
    const HubInfo& hub = p->boxes.hubs[6];
    REQUIRE(hub.valid);
    check_row_clear(*d, *p);
    // It covers no other tool's vertical drop.
    for (int t = 0; t < 6; ++t) {
        const int32_t tx = calc_tool_x(t, 7, p->L.x_off, p->L.width);
        CAPTURE(t);
        CHECK((tx < box_left(hub) || tx > box_right(hub)));
    }
    int entering = 0;
    for (int r = 0; r < p->plan.route_count; ++r)
        entering += enters_top(p->plan.routes[r], hub);
    CHECK(entering == 2);
}

TEST_CASE("Overview plan: a crowded junction row narrows its boxes, never below their lanes",
          "[system_path][hub_row]") {
    // Eight toolheads, two hub units on each.
    auto d = hub_row_data(16, 8);
    for (int u = 0; u < 16; ++u)
        hub_unit(*d, u, u / 2, 20 + u * 20);
    const int32_t lanes_w = 2 * 8 + 2 * (d->tube_gauge + helix::ui::kHaloWidthExtra + 2);

    auto roomy = plan_hub_row(*d, 1200);
    check_row_clear(*d, *roomy);
    for (int t = 0; t < 8; ++t) {
        CAPTURE(t);
        const HubInfo& h = roomy->boxes.hubs[t];
        REQUIRE(h.valid);
        CHECK(h.hub_x == h.tool_x);
        CHECK(h.mini_hub_w == LV_MAX(d->hub_width * 2 / 3, lanes_w));
        CHECK_FALSE(h.short_label);
    }

    auto tight = plan_hub_row(*d, 400);
    check_row_clear(*d, *tight);
    for (int t = 0; t < 8; ++t) {
        CAPTURE(t);
        const HubInfo& h = tight->boxes.hubs[t];
        REQUIRE(h.valid);
        CHECK(h.mini_hub_w < roomy->boxes.hubs[t].mini_hub_w);
        CHECK(h.mini_hub_w >= lanes_w);
        CHECK(h.short_label);
    }
}

// ----------------------------------------------------------------------------
// Route spacing: on the rigs the mocks model, no two routes cross or run closer
// than a haloed tube's width, except where they deliberately meet inside a box.
// ----------------------------------------------------------------------------

namespace {

struct MockUnit {
    int32_t x;
    int topology, tool_count, first_tool;
};

// The overview canvas as the panel fills it for a mock: card centres, tools,
// the theme's sizes at that breakpoint, unit 0 active on tool 0.
std::unique_ptr<SystemPathData> mock_rig(std::initializer_list<MockUnit> units, int tools,
                                         int32_t extruder_scale) {
    auto d = multi();
    d->unit_count = (int)units.size();
    int u = 0;
    for (const MockUnit& m : units) {
        d->unit_x_positions[u] = m.x;
        d->unit_topology[u] = m.topology;
        d->unit_tool_count[u] = m.tool_count;
        d->unit_first_tool[u] = m.first_tool;
        d->unit_has_hub_sensor[u] = m.topology == 1;
        d->unit_hub_triggered[u] = false;
        ++u;
    }
    d->total_tools = tools;
    d->active_unit = 0;
    d->active_tool = 0;
    d->hub_width = 70;
    d->tube_gauge = 5;
    d->extruder_scale = extruder_scale;
    d->toolhead_style = helix::ToolheadStyle::DEFAULT;
    return d;
}

struct Rig {
    const char* name;
    std::unique_ptr<SystemPathData> data;
    lv_area_t area;
};

constexpr lv_area_t kPanel800 = {0, 0, 429, 283};   // the canvas at 800x480
constexpr lv_area_t kPanelMicro = {0, 0, 284, 148}; // the canvas at 480x272

// Card centres at 800x480 sit 110 px apart, at 480x272 86 px apart.
std::vector<Rig> mock_rigs() {
    std::vector<Rig> rigs;
    for (const bool micro : {false, true}) {
        const int32_t x0 = micro ? 39 : 49, pitch = micro ? 86 : 110;
        const int32_t scale = micro ? 8 : 10;
        const lv_area_t area = micro ? kPanelMicro : kPanel800;
        auto at = [&](int card) { return x0 + pitch * card; };
        rigs.push_back({micro ? "afc_direct micro" : "afc_direct",
                        mock_rig({{at(0), 1, 1, 0},
                                  {at(1), 2, 1, 1},
                                  {at(2), 1, 1, 2},
                                  {at(3), 1, 1, 2},
                                  {at(4), 1, 1, 3},
                                  {at(5), 1, 1, 3}},
                                 4, scale),
                        area});
        rigs.push_back(
            {micro ? "multi micro" : "multi",
             mock_rig({{micro ? 80 : 99, 1, 1, 0}, {micro ? 230 : 309, 1, 1, 0}}, 1, scale), area});
        rigs.push_back({micro ? "mixed micro" : "mixed",
                        mock_rig({{micro ? 55 : 68, 2, 4, 0},
                                  {micro ? 170 : 215, 1, 1, 4},
                                  {micro ? 285 : 361, 1, 1, 5}},
                                 6, scale),
                        area});
        rigs.push_back({micro ? "stealth micro" : "stealth",
                        mock_rig({{at(0), 2, 4, 0},
                                  {at(1), 1, 1, 4},
                                  {at(2), 1, 1, 5},
                                  {at(3), 1, 1, 6},
                                  {at(4), 1, 1, 6}},
                                 7, scale),
                        area});
        rigs.push_back({micro ? "torture micro" : "torture",
                        mock_rig({{at(0), 1, 1, 0},
                                  {at(2), 2, 2, 1},
                                  {at(3), 1, 1, 3},
                                  {at(4), 1, 1, 3},
                                  {at(1), 1, 1, 0}},
                                 4, scale),
                        area});
    }
    return rigs;
}

// A route's painted centreline, sampled about every pixel.
std::vector<pg::PathPoint> sample_route(const Route& r) {
    std::vector<pg::PathPoint> pts;
    for (int k = 0; k < r.path.count; k++) {
        if (!r.style[k].painted)
            continue;
        const pg::PathSeg& s = r.path.segs[k];
        float len = s.type == pg::PathSeg::LINE ? std::hypot(s.p1.x - s.p0.x, s.p1.y - s.p0.y)
                                                : std::fabs(s.sweep) * s.radius;
        const int steps = LV_MAX(1, (int)std::ceil(len));
        for (int i = 0; i <= steps; i++) {
            const float t = (float)i / (float)steps;
            if (s.type == pg::PathSeg::LINE) {
                pts.push_back({s.p0.x + (s.p1.x - s.p0.x) * t, s.p0.y + (s.p1.y - s.p0.y) * t});
            } else {
                const float a = s.start_angle + s.sweep * t;
                pts.push_back(
                    {s.center.x + s.radius * std::cos(a), s.center.y + s.radius * std::sin(a)});
            }
        }
    }
    return pts;
}

struct Box {
    float x1, y1, x2, y2;
    bool contains(pg::PathPoint p, float pad) const {
        return p.x >= x1 - pad && p.x <= x2 + pad && p.y >= y1 - pad && p.y <= y2 + pad;
    }
};

// Where routes meet on purpose: inside a hub box, and at a nozzle.
std::vector<Box> meeting_places(const SystemPathData& d, const SysLayout& L,
                                const OverviewBoxes& boxes) {
    std::vector<Box> out;
    for (const HubInfo& h : boxes.hubs)
        if (h.valid)
            out.push_back(
                {(float)(h.hub_x - h.mini_hub_w / 2), (float)(h.mini_hub_y - h.mini_hub_h / 2),
                 (float)(h.hub_x + h.mini_hub_w / 2), (float)(h.mini_hub_y + h.mini_hub_h / 2)});
    if (boxes.combiner)
        out.push_back({(float)(L.center_x - d.hub_width / 2), (float)(L.hub_y - L.hub_h / 2),
                       (float)(L.center_x + d.hub_width / 2), (float)(L.hub_y + L.hub_h / 2)});
    return out;
}

struct Clash {
    int a, b;
    float distance;
    pg::PathPoint at;
};

// Every pair of routes whose painted centrelines come closer than @p min_gap
// outside the meeting places, with the closest point.
std::vector<Clash> route_clashes(const PathPlan& plan, const std::vector<Box>& meet,
                                 float min_gap) {
    std::vector<std::vector<pg::PathPoint>> pts(plan.route_count);
    for (int i = 0; i < plan.route_count; i++)
        pts[i] = sample_route(plan.routes[i]);
    auto excluded = [&](pg::PathPoint p) {
        for (const Box& b : meet)
            if (b.contains(p, min_gap))
                return true;
        return false;
    };
    std::vector<Clash> out;
    for (int a = 0; a < plan.route_count; a++)
        for (int b = a + 1; b < plan.route_count; b++) {
            Clash c{a, b, 1e9f, {}};
            for (const pg::PathPoint& p : pts[a]) {
                if (excluded(p))
                    continue;
                for (const pg::PathPoint& q : pts[b]) {
                    const float dist = std::hypot(p.x - q.x, p.y - q.y);
                    if (dist < c.distance && !excluded(q))
                        c = {a, b, dist, p};
                }
            }
            if (c.distance < min_gap)
                out.push_back(c);
        }
    return out;
}

} // namespace

TEST_CASE("Overview plan: routes on the mock rigs keep apart outside their boxes",
          "[system_path][route_spacing]") {
    for (const Rig& rig : mock_rigs()) {
        CAPTURE(rig.name);
        const SystemPathData& d = *rig.data;
        const SysLayout L = compute_sys_layout(d, rig.area);
        PathPlan plan;
        OverviewBoxes boxes;
        plan_overview(d, L, plan, boxes);
        REQUIRE(plan.dropped == 0);
        const float min_gap = (float)(d.tube_gauge + helix::ui::kHaloWidthExtra);
        for (const Clash& c : route_clashes(plan, meeting_places(d, L, boxes), min_gap)) {
            const pg::PathPoint sa = seg_start(plan.routes[c.a].path.segs[0]);
            const pg::PathPoint sb = seg_start(plan.routes[c.b].path.segs[0]);
            UNSCOPED_INFO(rig.name << ": routes from x=" << sa.x << " and x=" << sb.x << " come "
                                   << c.distance << " px apart at (" << c.at.x << ", " << c.at.y
                                   << ")");
            CHECK(c.distance >= min_gap);
        }
    }
}
