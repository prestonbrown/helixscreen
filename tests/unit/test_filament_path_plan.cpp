// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LINEAR/HUB route plan for the filament_path_canvas detail panel.
//
// The [hits] cases render the widget and pin the hub/buffer/bypass hit rects
// the click handler reads, so the renderer records exactly the boxes it draws.
// The rest run the pure frame → plan step on a fixed 400x400 frame: slots at
// x = 50, 150, 250, 350, center 200; entry -48, prep 40, hub 120 (h 40, top
// 100), output 140, buffer 184, merge 232, toolhead 272, nozzle 328, inlet 308.

#include "ui_ams_detail.h"
#include "ui_filament_path_canvas.h"
#include "ui_fonts.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "ams_types.h"
#include "filament_path_test_helpers.h"
#include "lvgl/lvgl.h"
#include "src/ui/ui_filament_path_internal.h"
#include "src/ui/ui_filament_path_plan.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::fpath;
using namespace fpath_test;

namespace {

constexpr int32_t W = 400;
constexpr int32_t H = 400;

// A 400x400 canvas with four slots at x = 50, 150, 250, 350 (computed slot
// positions: no slot grid, 100 px slots, no overlap).
lv_obj_t* make_canvas(lv_obj_t* screen, int topology) {
    lv_obj_t* w = ui_filament_path_canvas_create(screen);
    lv_obj_set_size(w, W, H);
    ui_filament_path_canvas_set_slot_count(w, 4);
    ui_filament_path_canvas_set_slot_width(w, 100);
    ui_filament_path_canvas_set_slot_overlap(w, 0);
    ui_filament_path_canvas_set_topology(w, topology);
    return w;
}

void render(LVGLTestFixture& fx, lv_obj_t* w) {
    lv_obj_update_layout(fx.test_screen());
    fx.process_lvgl(120);
    REQUIRE(get_data(w)->layers.render_count > 0);
}

// The frame the widget renders from: where its hub box is fitted.
LinearHubFrame widget_frame(lv_obj_t* w) {
    const FilamentPathData* d = get_data(w);
    const BaseGeometry g = compute_base_geometry(w, d);
    const int32_t nozzle_y = g.y_off + (int32_t)(g.height * NOZZLE_Y_RATIO);
    return compute_linear_hub_frame(*d, g, toolhead_top_y(nozzle_y, d->theme.extruder_scale));
}

bool area_eq(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: HUB hit rects match the drawn boxes",
                 "[filament-path][plan][hits]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::HUB));
    ui_filament_path_canvas_set_buffer_info(w, true, 0);
    ui_filament_path_canvas_set_show_bypass(w, true);
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    lv_area_t c;
    lv_obj_get_coords(w, &c);
    const int32_t r = d->theme.sensor_radius;
    const int32_t hw = d->theme.hub_width;
    const int32_t cx = c.x1 + 200;
    const int32_t hub_h = (int32_t)(H * HUB_HEIGHT_RATIO);
    // Fitted to the lanes' clearance: at least 3 * 22 + 2 * 8, at most the slot span + 16.
    const LinearHubFrame f = widget_frame(w);
    const int32_t hub_y = f.hub_y;
    const int32_t hub_w = f.hub_box_w;
    CHECK(hub_w >= LV_MAX(hw, 3 * 22 + 2 * 8));
    CHECK(hub_w <= 300 + 2 * 8);
    REQUIRE(d->hits.hub_valid);
    CHECK(area_eq(d->hits.hub,
                  {cx - hub_w / 2, hub_y - hub_h / 2, cx + hub_w / 2, hub_y + hub_h / 2}));

    const int32_t buf_y = c.y1 + (int32_t)(H * BUFFER_Y_RATIO);
    const int32_t buf_w = LV_MAX(36, hw * 4 / 5);
    const int32_t buf_h = LV_MAX(16, hub_h);
    REQUIRE(d->hits.buffer_valid);
    CHECK(area_eq(d->hits.buffer,
                  {cx - buf_w / 2, buf_y - buf_h / 2, cx + buf_w / 2, buf_y + buf_h / 2}));

    const int32_t bx = c.x1 + (int32_t)(W * BYPASS_X_RATIO);
    const int32_t my = c.y1 + (int32_t)(H * BYPASS_MERGE_Y_RATIO);
    REQUIRE(d->hits.bypass_valid);
    CHECK(area_eq(d->hits.bypass, {bx - r * 3, my - r * 4, bx + r * 3, my + r * 4}));
    CHECK(d->hits.origin.x == c.x1);
    CHECK(d->hits.origin.y == c.y1);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: LINEAR selector hit rect spans the slot row",
                 "[filament-path][plan][hits]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::LINEAR));
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    lv_area_t c;
    lv_obj_get_coords(w, &c);
    const int32_t r = d->theme.sensor_radius;
    const int32_t cx = c.x1 + 200;
    const int32_t hub_h = (int32_t)(H * HUB_HEIGHT_RATIO);
    const int32_t sel_y = c.y1 + (int32_t)(H * PREP_Y_RATIO) + r + hub_h / 2;
    const int32_t sel_w = 300 + LV_MAX(100, r * 4);

    REQUIRE(d->hits.hub_valid);
    CHECK(area_eq(d->hits.hub,
                  {cx - sel_w / 2, sel_y - hub_h / 2, cx + sel_w / 2, sel_y + hub_h / 2}));
}

// ============================================================================
// Pure plan
// ============================================================================

namespace {

namespace pg = helix::ui::pathgeo;

constexpr uint32_t SLOT_COLORS[4] = {0xE53935, 0x1E88E5, 0x43A047, 0xFDD835};
constexpr uint32_t BYPASS_COLOR = 0x8E24AA;
const lv_color_t BG = lv_color_hex(0x101010);
// The default toolhead glyph's top for nozzle 328 at extruder scale 10.
constexpr int32_t GLYPH_TOP = 297;

std::unique_ptr<FilamentPathData> make_data(helix::PathTopology topo) {
    auto d = std::make_unique<FilamentPathData>();
    d->topology = static_cast<int>(topo);
    d->slot_count = 4;
    d->theme.line_width_active = 3;
    d->theme.tube_gauge = 5;
    d->theme.sensor_radius = 4;
    d->theme.hub_width = 60;
    d->theme.extruder_scale = 10;
    d->theme.color_idle = lv_color_hex(0x606060);
    d->theme.color_error = lv_color_hex(0xFF0000);
    d->theme.color_bg = BG;
    d->theme.color_accent = lv_color_hex(0x2196F3);
    for (int i = 0; i < 4; i++) {
        d->slot_has_prep_sensor[i] = true;
        d->slot_has_load_sensor[i] = true;
    }
    d->has_hub_sensor = true;
    d->show_bypass = true;
    d->has_toolhead_sensor = true;
    d->bypass_color = BYPASS_COLOR;
    return d;
}

void load_active(FilamentPathData& d, int slot, helix::PathSegment seg) {
    d.active_slot = slot;
    d.filament_segment = static_cast<int>(seg);
    d.filament_color = SLOT_COLORS[slot];
}

BaseGeometry geometry() {
    BaseGeometry g;
    g.width = 400;
    g.height = 400;
    g.slot_count = 4;
    for (int i = 0; i < 4; i++)
        g.slot_x[i] = 50 + 100 * i;
    g.center_x = 200;
    return g;
}

// Where a HUB lane's load sensor band sits: midway down its straight run from
// the prep row (y 40) to its fan's first bend.
float load_band_y(const FilamentPathData& d, int slot) {
    return (40.0f + compute_linear_hub_frame(d, geometry(), GLYPH_TOP).hub_fan[slot].pts[1].y) / 2;
}

float hub_entry_x(const FilamentPathData& d, int slot) {
    return compute_linear_hub_frame(d, geometry(), GLYPH_TOP).hub_fan[slot].pts[3].x;
}

PathPlan& plan_for(const FilamentPathData& d) {
    static PathPlan plan;
    const BaseGeometry g = geometry();
    plan_linear_hub(compute_linear_hub_frame(d, g, GLYPH_TOP), d, g, plan);
    return plan;
}

// A segment of the route ends exactly at (x, y).
bool has_boundary(const pg::FilamentPath& p, float x, float y) {
    for (int i = 0; i < p.count - 1; i++)
        if (near(seg_end(p.segs[i]), x, y))
            return true;
    return false;
}

const SensorBand* band_at(const PathPlan& plan, float x, float y) {
    for (int i = 0; i < plan.band_count; i++)
        if (near(plan.bands[i].at, x, y, 0.5f))
            return &plan.bands[i];
    return nullptr;
}

SpanStyle style_a() {
    return {TubeWall::Active, lv_color_hex(0x112233), true, true};
}
SpanStyle style_b() {
    return {TubeWall::Plain, BG, false, true};
}

Route route_of(std::initializer_list<SpanStyle> styles) {
    Route r;
    float y = 0;
    for (SpanStyle s : styles) {
        pg::FilamentPath piece;
        piece.add_line(0, y, 0, y + 10);
        route_append(r, piece, s);
        y += 10;
    }
    return r;
}

using helix::PathSegment;

} // namespace

TEST_CASE("FilamentPath plan: the frame fixture lays out as the plan states",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    const LinearHubFrame f = compute_linear_hub_frame(*d, geometry(), GLYPH_TOP);
    CHECK(f.entry_y == -48);
    CHECK(f.prep_y == 40);
    CHECK(f.hub_y == 120);
    CHECK(f.hub_h == 40);
    CHECK(f.output_y == 140);
    CHECK(f.buffer_y == 184);
    CHECK(f.bypass_merge_y == 232);
    CHECK(f.toolhead_y == 272);
    CHECK(f.nozzle_y == 328);
    CHECK(f.inlet_y == 308);
}

TEST_CASE("FilamentPath plan: coalesce joins equal painted runs", "[filament-path][plan]") {
    Stroke out[16];
    SpanStyle hidden = style_a();
    hidden.painted = false;

    Route r = route_of({style_a(), style_a(), style_b(), style_a()});
    REQUIRE(coalesce(r, out, 16) == 3);
    CHECK((out[0].first == 0 && out[0].end == 2 && out[0].style == style_a()));
    CHECK((out[1].first == 2 && out[1].end == 3 && out[1].style == style_b()));
    CHECK((out[2].first == 3 && out[2].end == 4 && out[2].style == style_a()));

    r = route_of({style_a(), hidden, style_a()});
    REQUIRE(coalesce(r, out, 16) == 2);
    CHECK((out[0].first == 0 && out[0].end == 1));
    CHECK((out[1].first == 2 && out[1].end == 3));

    r = route_of({style_a(), style_a(), style_a(), style_a(), style_a(), style_a(), style_a()});
    REQUIRE(coalesce(r, out, 16) == 1);
    CHECK((out[0].first == 0 && out[0].end == 7));

    CHECK(coalesce(Route{}, out, 16) == 0);
}

TEST_CASE("FilamentPath plan: span_style", "[filament-path][plan]") {
    const lv_color_t fil = lv_color_hex(SLOT_COLORS[1]);
    auto ss = [&](PathSegment span, PathSegment reached, bool on, PathSegment err) {
        return span_style(span, reached, on, err, fil, BG);
    };

    SpanStyle s = ss(PathSegment::LANE, PathSegment::HUB, true, PathSegment::NONE);
    CHECK(s.wall == TubeWall::Active);
    CHECK(s.filled);
    CHECK(lv_color_eq(s.bore, fil));

    s = ss(PathSegment::OUTPUT, PathSegment::HUB, true, PathSegment::NONE);
    CHECK(s.wall == TubeWall::Plain);
    CHECK_FALSE(s.filled);
    CHECK(lv_color_eq(s.bore, BG));

    s = ss(PathSegment::LANE, PathSegment::LANE, false, PathSegment::NONE);
    CHECK(s.wall == TubeWall::Plain);
    CHECK(s.filled);

    s = ss(PathSegment::OUTPUT, PathSegment::NOZZLE, true, PathSegment::OUTPUT);
    CHECK(s.wall == TubeWall::Error);
    CHECK(s.filled);

    s = ss(PathSegment::OUTPUT, PathSegment::HUB, true, PathSegment::OUTPUT);
    CHECK(s.wall == TubeWall::Error);
    CHECK_FALSE(s.filled);

    // The error marks whichever lane it is given; lane_error() decides which.
    s = ss(PathSegment::OUTPUT, PathSegment::NOZZLE, false, PathSegment::OUTPUT);
    CHECK(s.wall == TubeWall::Error);
    CHECK(s.filled);
}

TEST_CASE("FilamentPath plan: band_state", "[filament-path][plan]") {
    CHECK(band_state(PathSegment::HUB, PathSegment::LANE, true, PathSegment::NONE) ==
          BandState::Empty);
    CHECK(band_state(PathSegment::HUB, PathSegment::HUB, true, PathSegment::NONE) ==
          BandState::Active);
    CHECK(band_state(PathSegment::PREP, PathSegment::LANE, false, PathSegment::NONE) ==
          BandState::Loaded);
    CHECK(band_state(PathSegment::PREP, PathSegment::NONE, false, PathSegment::NONE) ==
          BandState::Empty);
    CHECK(band_state(PathSegment::TOOLHEAD, PathSegment::HUB, true, PathSegment::TOOLHEAD) ==
          BandState::Error);
}

TEST_CASE("FilamentPath plan: band_segment crosses the tube", "[filament-path][plan]") {
    pg::PathPoint p0, p1;
    SensorBand b{{200, 140}, {0, 1}, BandState::Empty, BG};
    band_segment(b, 5, p0, p1);
    CHECK(((near(p0, 194.5f, 140) && near(p1, 205.5f, 140)) ||
           (near(p1, 194.5f, 140) && near(p0, 205.5f, 140))));

    b.tangent = {1, 0};
    band_segment(b, 5, p0, p1);
    CHECK(((near(p0, 200, 134.5f) && near(p1, 200, 145.5f)) ||
           (near(p1, 200, 134.5f) && near(p0, 200, 145.5f))));
}

TEST_CASE("FilamentPath plan: HUB active route runs unbroken from spool to inlet",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    const PathPlan& plan = plan_for(*d);

    REQUIRE(plan.active_route >= 0);
    const Route& r = plan.routes[plan.active_route];
    REQUIRE(r.path.count > 0);
    CHECK(near(seg_start(r.path.segs[0]), 150, -48));
    CHECK(near(seg_end(r.path.segs[r.path.count - 1]), 200, 308));
    CHECK(contiguous(r.path));
    CHECK(has_boundary(r.path, 150, 40));
    CHECK(has_boundary(r.path, 200, 140));
    CHECK(has_boundary(r.path, 200, 232));
    CHECK(has_boundary(r.path, 200, 272));

    // The unpainted hub interior splits it into one stroke above and one below.
    Stroke strokes[16];
    REQUIRE(coalesce(r, strokes, 16) == 2);
    for (int i = 0; i < 2; i++) {
        CHECK(strokes[i].style.wall == TubeWall::Active);
        CHECK(strokes[i].style.filled);
    }
    CHECK(seg_end(r.path.segs[strokes[0].end - 1]).y <= 100.01f);
    CHECK(near(seg_start(r.path.segs[strokes[1].first]), 200, 140));

    REQUIRE(plan.band_count == 11);
    const SensorBand* active[] = {band_at(plan, 150, 40), band_at(plan, 150, load_band_y(*d, 1)),
                                  band_at(plan, 200, 140), band_at(plan, 200, 232),
                                  band_at(plan, 200, 272)};
    for (const SensorBand* b : active) {
        REQUIRE(b != nullptr);
        CHECK(b->state == BandState::Active);
    }
    int empty = 0;
    for (int i = 0; i < plan.band_count; i++)
        empty += plan.bands[i].state == BandState::Empty;
    CHECK(empty == 6);
}

TEST_CASE("FilamentPath plan: hub-edge bands are painted after the hub box",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    const PathPlan& plan = plan_for(*d);

    // paint_tubes paints the rest; render_linear_hub calls paint_box_bands
    // after draw_hub_section, so these clamp the tube over the box edge.
    for (int i = 0; i < 4; i++) {
        CHECK(band_at(plan, hub_entry_x(*d, i), 100) == nullptr);
        const SensorBand* load = band_at(plan, 50.0f + 100 * i, load_band_y(*d, i));
        REQUIRE(load != nullptr);
        CHECK_FALSE(load->on_box_edge);
    }
    const SensorBand* output = band_at(plan, 200, 140);
    REQUIRE(output != nullptr);
    CHECK(output->on_box_edge);

    int on_edge = 0;
    for (int i = 0; i < plan.band_count; i++)
        on_edge += plan.bands[i].on_box_edge;
    CHECK(on_edge == 1);
    CHECK_FALSE(band_at(plan, 150, 40)->on_box_edge);
    CHECK_FALSE(band_at(plan, 200, 232)->on_box_edge);
    CHECK_FALSE(band_at(plan, 200, 272)->on_box_edge);
}

TEST_CASE("FilamentPath plan: a lane loaded to the hub fills to the hub bottom",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::HUB);
    const PathPlan& plan = plan_for(*d);

    REQUIRE(plan.active_route >= 0);
    const Route& r = plan.routes[plan.active_route];
    const pg::FilamentPath filled = filled_prefix(r);
    REQUIRE(filled.count > 0);
    CHECK(near(seg_end(filled.segs[filled.count - 1]), 200, 140));
    for (int i = filled.count; i < r.path.count; i++) {
        CHECK(r.style[i].wall == TubeWall::Plain);
        CHECK_FALSE(r.style[i].filled);
    }
}

TEST_CASE("FilamentPath plan: an active bypass owns the trunk below the merge",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->buffer_present = true;
    d->bypass_active = true;
    d->active_slot = -1;
    const PathPlan& plan = plan_for(*d);

    CHECK_FALSE(plan.buffer_has_filament);
    CHECK(plan.active_route == -1);

    REQUIRE(plan.trunk_route >= 0);
    const Route& t = plan.routes[plan.trunk_route];
    REQUIRE(t.path.count > 0);
    CHECK(near(seg_end(t.path.segs[t.path.count - 1]), 200, 232));
    for (int i = 0; i < t.path.count; i++) {
        CHECK(t.style[i].wall == TubeWall::Plain);
        CHECK_FALSE(t.style[i].filled);
    }

    REQUIRE(plan.bypass_route >= 0);
    const Route& b = plan.routes[plan.bypass_route];
    REQUIRE(b.path.count > 0);
    CHECK(near(seg_start(b.path.segs[0]), 400 * (BYPASS_X_RATIO - 0.05f), 232, 1.0f));
    CHECK(near(seg_end(b.path.segs[b.path.count - 1]), 200, 308));
    CHECK(contiguous(b.path));
    for (int i = 0; i < b.path.count; i++) {
        CHECK(b.style[i].wall == TubeWall::Active);
        CHECK(lv_color_eq(b.style[i].bore, lv_color_hex(BYPASS_COLOR)));
    }
}

TEST_CASE("FilamentPath plan: a staged lane stays visible in its own color",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    d->slot_filament_states[3] = {PathSegment::LANE, SLOT_COLORS[3]};
    const PathPlan& plan = plan_for(*d);

    const Route& r = plan.routes[3];
    REQUIRE(r.path.count > 0);
    for (int i = 0; i < r.path.count; i++) {
        CHECK(r.style[i].wall == TubeWall::Plain);
        CHECK(r.style[i].filled);
    }
    const pg::PathPoint end = seg_end(r.path.segs[r.path.count - 1]);
    CHECK(end.y == Catch::Approx(100));

    const SensorBand* prep = band_at(plan, 350, 40);
    REQUIRE(prep != nullptr);
    CHECK(prep->state == BandState::Loaded);
    CHECK(lv_color_eq(prep->fill, lv_color_hex(SLOT_COLORS[3])));
    // The load sensor reads it; no sensor sits at the hub entry.
    const SensorBand* load = band_at(plan, 350, load_band_y(*d, 3));
    REQUIRE(load != nullptr);
    CHECK(load->state == BandState::Loaded);
    CHECK(band_at(plan, end.x, end.y) == nullptr);
}

TEST_CASE("FilamentPath plan: fill and error follow the segment the filament reached",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);

    // Style of the first segment of the entry run (above prep) and of the fan
    // run (below prep) of slot 1's route.
    auto entry_style = [](const Route& r) { return r.style[0]; };
    auto fan_style = [](const Route& r) {
        for (int i = 0; i < r.path.count; i++)
            if (seg_start(r.path.segs[i]).y >= 40 - 0.01f)
                return r.style[i];
        return SpanStyle{};
    };
    const float load_y = load_band_y(*d, 1);

    SECTION("loaded to PREP: the entry run fills, the fan stays empty") {
        load_active(*d, 1, PathSegment::PREP);
        const PathPlan& plan = plan_for(*d);
        const Route& r = plan.routes[1];
        CHECK(entry_style(r).wall == TubeWall::Active);
        CHECK(fan_style(r).wall == TubeWall::Plain);
        CHECK_FALSE(fan_style(r).filled);
        const SensorBand* load = band_at(plan, 150, load_y);
        REQUIRE(load != nullptr);
        CHECK(load->state == BandState::Empty);
    }
    SECTION("error at PREP with the lane loaded: only the prep band is an error") {
        load_active(*d, 1, PathSegment::LANE);
        d->error_segment = static_cast<int>(PathSegment::PREP);
        const PathPlan& plan = plan_for(*d);
        const Route& r = plan.routes[1];
        const SensorBand* prep = band_at(plan, 150, 40);
        REQUIRE(prep != nullptr);
        CHECK(prep->state == BandState::Error);
        CHECK(entry_style(r).wall == TubeWall::Active);
        CHECK(fan_style(r).wall == TubeWall::Active);
        for (int i = 0; i < r.path.count; i++)
            CHECK(r.style[i].wall != TubeWall::Error);
    }
    SECTION("error at LANE: only the fan run is an error") {
        load_active(*d, 1, PathSegment::LANE);
        d->error_segment = static_cast<int>(PathSegment::LANE);
        const PathPlan& plan = plan_for(*d);
        const Route& r = plan.routes[1];
        CHECK(entry_style(r).wall == TubeWall::Active);
        CHECK(fan_style(r).wall == TubeWall::Error);
    }
}

TEST_CASE("FilamentPath plan: the LINEAR selector passage is painted under the box",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::LINEAR);
    load_active(*d, 2, PathSegment::OUTPUT);
    const PathPlan& plan = plan_for(*d);

    REQUIRE(plan.active_route == 2);
    const Route& r = plan.routes[2];
    CHECK(contiguous(r.path));
    // Selector: top at prep + sensor_r = 44, bottom at 84, under slot 2.
    bool found = false;
    for (int i = 0; i < r.path.count; i++) {
        if (near(seg_start(r.path.segs[i]), 250, 44) && near(seg_end(r.path.segs[i]), 250, 84)) {
            found = true;
            CHECK(r.style[i].painted);
            CHECK(r.style[i].filled);
        }
    }
    CHECK(found);
}

TEST_CASE("FilamentPath plan: the on-toolhead route fits the segment budget",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->hub_on_toolhead = true;
    d->show_bypass = false;
    load_active(*d, 0, PathSegment::NOZZLE);
    const PathPlan& plan = plan_for(*d);

    REQUIRE(plan.active_route == 0);
    const Route& r = plan.routes[0];
    CHECK(r.dropped == 0);
    CHECK(plan.dropped == 0);
    CHECK(contiguous(r.path));
    CHECK(near(seg_start(r.path.segs[0]), 50, -48));
    CHECK(near(seg_end(r.path.segs[r.path.count - 1]), 200, 308));
}

TEST_CASE("FilamentPath plan: an OUTPUT error is one stroke through the buffer",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->show_bypass = false;
    d->buffer_present = true;
    load_active(*d, 1, PathSegment::NOZZLE);
    d->error_segment = static_cast<int>(PathSegment::OUTPUT);
    const PathPlan& plan = plan_for(*d);

    const Route& r = plan.routes[plan.active_route];
    Stroke strokes[16];
    const int n = coalesce(r, strokes, 16);
    int errors = 0;
    for (int i = 0; i < n; i++) {
        if (strokes[i].style.wall != TubeWall::Error)
            continue;
        errors++;
        // Stacked over the glyph: hub bottom 201, buffer 229..269, toolhead band 283.
        CHECK(near(seg_start(r.path.segs[strokes[i].first]), 200, 201));
        CHECK(near(seg_end(r.path.segs[strokes[i].end - 1]), 200, 283));
    }
    CHECK(errors == 1);
}

TEST_CASE("FilamentPath plan: the toolhead band follows the unit's sensor, bypass or not",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->show_bypass = false;
    load_active(*d, 1, PathSegment::NOZZLE);

    // Stacked over the glyph: hub bottom 241, toolhead band midway to the glyph top.
    SECTION("a unit with a toolhead sensor gets a band on the trunk") {
        const PathPlan& plan = plan_for(*d);
        const SensorBand* b = band_at(plan, 200, 269);
        REQUIRE(b != nullptr);
        CHECK(b->state == BandState::Active);
        const Route& r = plan.routes[plan.active_route];
        CHECK(contiguous(r.path));
        CHECK(has_boundary(r.path, 200, 269));
        CHECK(near(seg_end(r.path.segs[r.path.count - 1]), 200, 308));
    }
    SECTION("a unit without one gets none") {
        d->has_toolhead_sensor = false;
        const PathPlan& plan = plan_for(*d);
        CHECK(band_at(plan, 200, 269) == nullptr);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: the animation replays one unbroken active path",
                 "[filament-path][plan]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::HUB));
    ui_filament_path_canvas_set_active_slot(w, 1);
    ui_filament_path_canvas_set_filament_segment(w, static_cast<int>(PathSegment::NOZZLE));
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    lv_area_t c;
    lv_obj_get_coords(w, &c);
    const pg::FilamentPath& p = d->path_cache.path;
    REQUIRE(d->path_cache.valid);
    REQUIRE(p.count > 0);
    CHECK(contiguous(p));
    // From slot 1's spool entry to the nozzle glyph's inlet.
    const int32_t nozzle_y = c.y1 + (int32_t)(H * NOZZLE_Y_RATIO);
    CHECK(near(seg_start(p.segs[0]), (float)(c.x1 + 150),
               (float)(c.y1 + (int32_t)(H * ENTRY_Y_RATIO))));
    CHECK(near(seg_end(p.segs[p.count - 1]), (float)(c.x1 + 200),
               (float)(nozzle_y - d->theme.extruder_scale * 2)));
}

TEST_CASE("FilamentPath plan: sixteen HUB lanes fit the segment and band budgets",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->slot_count = 16;
    for (int i = 0; i < 16; i++) {
        d->slot_has_prep_sensor[i] = true;
        d->slot_has_load_sensor[i] = true;
    }
    d->buffer_present = true;
    load_active(*d, 1, PathSegment::NOZZLE);
    BaseGeometry g;
    g.width = 800;
    g.height = 400;
    g.slot_count = 16;
    for (int i = 0; i < 16; i++)
        g.slot_x[i] = 25 + 50 * i;
    g.center_x = 400;
    static PathPlan plan;
    plan_linear_hub(compute_linear_hub_frame(*d, g, GLYPH_TOP), *d, g, plan);

    CHECK(plan.dropped == 0);
    for (int i = 0; i < plan.route_count; i++)
        CHECK(plan.routes[i].dropped == 0);
    // 16 prep + 16 load + output, merge, toolhead.
    CHECK(plan.band_count == 35);
    CHECK(contiguous(plan.routes[plan.active_route].path));
}

TEST_CASE("FilamentPath plan: route_append reports what it drops", "[filament-path][plan]") {
    Route r;
    pg::FilamentPath piece;
    for (int i = 0; i < 10; i++)
        piece.add_line(0, (float)i, 0, (float)i + 1);
    CHECK(route_append(r, piece, style_a()));
    CHECK_FALSE(route_append(r, piece, style_a()));
    CHECK(r.path.count == pg::FilamentPath::MAX_SEGS);
    CHECK(r.dropped == 20 - pg::FilamentPath::MAX_SEGS);
}

TEST_CASE("FilamentPath plan: a toolhead error under an active bypass marks the bypass route",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->bypass_active = true;
    d->active_slot = -1;
    d->error_segment = static_cast<int>(PathSegment::TOOLHEAD);
    const PathPlan& plan = plan_for(*d);

    const SensorBand* th = band_at(plan, 200, 272);
    REQUIRE(th != nullptr);
    CHECK(th->state == BandState::Error);

    REQUIRE(plan.bypass_route >= 0);
    const Route& b = plan.routes[plan.bypass_route];
    REQUIRE(b.path.count == 3);
    // Spool → merge, merge → toolhead, toolhead → inlet: the TOOLHEAD run is the
    // error, as on an AMS route; the run past the sensor stays filled.
    CHECK(b.style[0].wall == TubeWall::Active);
    CHECK(b.style[1].wall == TubeWall::Error);
    CHECK(near(seg_end(b.path.segs[1]), 200, 272));
    CHECK(b.style[2].wall == TubeWall::Active);
    for (int i = 0; i < 3; i++)
        CHECK(b.style[i].filled);

    // The same input on an AMS lane styles its trunk the same way.
    auto ams = make_data(helix::PathTopology::HUB);
    load_active(*ams, 1, PathSegment::NOZZLE);
    ams->error_segment = static_cast<int>(PathSegment::TOOLHEAD);
    const PathPlan& ap = plan_for(*ams);
    const Route& a = ap.routes[ap.active_route];
    CHECK(a.style[a.path.count - 2].wall == TubeWall::Error);
    CHECK(a.style[a.path.count - 1].wall == TubeWall::Active);
}

TEST_CASE("FilamentPath plan: an idle trunk still shows an OUTPUT error", "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->active_slot = -1;
    d->error_segment = static_cast<int>(PathSegment::OUTPUT);
    const PathPlan& plan = plan_for(*d);

    CHECK(plan.active_route == -1);
    REQUIRE(plan.trunk_route >= 0);
    const SensorBand* out = band_at(plan, 200, 140);
    REQUIRE(out != nullptr);
    CHECK(out->state == BandState::Error);
    const Route& t = plan.routes[plan.trunk_route];
    REQUIRE(t.path.count > 0);
    CHECK(t.style[0].wall == TubeWall::Error);
    CHECK_FALSE(t.style[0].filled);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: a narrow hub box grows to hold its gear",
                 "[filament-path][plan][hits]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::HUB));
    ui_filament_path_canvas_set_slot_count(w, 2);
    ui_filament_path_canvas_set_hub_callback(w, [](lv_point_t, void*) {}, nullptr);
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    lv_area_t c;
    lv_obj_get_coords(w, &c);
    const int32_t hub_w = widget_frame(w).hub_box_w;
    const int32_t cx = c.x1 + 100;

    const lv_font_t* icon = theme_manager_get_font("icon_font_sm");
    REQUIRE(icon != nullptr);
    REQUIRE(d->theme.label_font != nullptr);
    lv_point_t gear, label;
    lv_text_get_size(&gear, ICON_SETTINGS, icon, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&label, "HUB", d->theme.label_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    REQUIRE((hub_w - label.x) / 2 < gear.x + 4); // the frame's box is too narrow for it

    // The box drawn, and recorded, holds the centered label and the gear.
    const int32_t drawn_w = label.x + 2 * (gear.x + 4);
    REQUIRE(d->hits.hub_valid);
    CHECK(d->hits.hub.x1 == cx - drawn_w / 2);
    CHECK(d->hits.hub.x2 == cx + drawn_w / 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: on-toolhead records the selector as the hub hit",
                 "[filament-path][plan][hits]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::HUB));
    ui_filament_path_canvas_set_hub_on_toolhead(w, true);
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    lv_area_t c;
    lv_obj_get_coords(w, &c);
    const int32_t r = d->theme.sensor_radius;
    const int32_t cx = c.x1 + 200;
    const int32_t hub_h = (int32_t)(H * HUB_HEIGHT_RATIO);
    const int32_t sel_y =
        c.y1 + (int32_t)(H * PREP_Y_RATIO) + (int32_t)(H * (HUB_HEIGHT_RATIO / 2 + 0.02f));
    const int32_t sel_w = 300 + LV_MAX(100, r * 4);

    REQUIRE(d->hits.hub_valid);
    CHECK(area_eq(d->hits.hub,
                  {cx - sel_w / 2, sel_y - hub_h / 2, cx + sel_w / 2, sel_y + hub_h / 2}));
}

TEST_CASE("FilamentPath plan: sensor bands only where the unit reports the sensor",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);

    SECTION("no load sensors: no band between prep and hub") {
        for (int i = 0; i < 4; i++)
            d->slot_has_load_sensor[i] = false;
        const PathPlan& plan = plan_for(*d);
        for (int i = 0; i < 4; i++)
            CHECK(band_at(plan, 50.0f + 100 * i, load_band_y(*d, i)) == nullptr);
        CHECK(contiguous(plan.routes[1].path));
    }
    SECTION("no hub sensor: no band on the hub outlet") {
        d->has_hub_sensor = false;
        const PathPlan& plan = plan_for(*d);
        CHECK(band_at(plan, 200, 140) == nullptr);
        CHECK(band_at(plan, 200, 272) != nullptr); // the toolhead sensor stays
    }
    SECTION("the load band reads LANE") {
        load_active(*d, 1, PathSegment::LANE);
        CHECK(band_at(plan_for(*d), 150, load_band_y(*d, 1))->state == BandState::Active);
        load_active(*d, 1, PathSegment::PREP);
        CHECK(band_at(plan_for(*d), 150, load_band_y(*d, 1))->state == BandState::Empty);
    }
}

TEST_CASE("FilamentPath plan: filament stuck in the hub with no lane fills the idle trunk",
          "[filament-path][plan][stuck]") {
    auto d = make_data(helix::PathTopology::HUB);
    REQUIRE(d->active_slot < 0);

    SECTION("the hub sensor reads filament: filled as far as the hub output, plain walls") {
        d->hub_sensor_triggered = true;
        const PathPlan& plan = plan_for(*d);
        REQUIRE(plan.trunk_route >= 0);
        const Route& t = plan.routes[plan.trunk_route];
        CHECK(t.style[0].filled);
        CHECK(t.style[0].wall == TubeWall::Plain);
        CHECK_FALSE(t.style[t.path.count - 1].filled); // not into the toolhead
        const SensorBand* out = band_at(plan, 200, 140);
        REQUIRE(out != nullptr);
        CHECK(out->state == BandState::Loaded);
        CHECK(band_at(plan, 200, 272)->state == BandState::Empty);
    }
    SECTION("nothing read: an empty trunk") {
        const PathPlan& plan = plan_for(*d);
        CHECK_FALSE(plan.routes[plan.trunk_route].style[0].filled);
        CHECK(band_at(plan, 200, 140)->state == BandState::Empty);
    }
}

TEST_CASE("FilamentPath plan: a lane's own error shows off the active route",
          "[filament-path][plan][lane_error]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    d->slot_filament_states[3] = {PathSegment::LANE, SLOT_COLORS[3]};
    d->slot_has_error[3] = true;
    d->slot_has_error[2] = true; // no filament in it
    const PathPlan& plan = plan_for(*d);

    // Lane 3 stopped at its load sensor: that band and the LANE spans are errors.
    const SensorBand* load = band_at(plan, 350, load_band_y(*d, 3));
    REQUIRE(load != nullptr);
    CHECK(load->state == BandState::Error);
    CHECK(band_at(plan, 350, 40)->state == BandState::Loaded);
    bool lane_error_wall = false;
    for (int i = 0; i < plan.routes[3].path.count; i++)
        lane_error_wall |= plan.routes[3].style[i].wall == TubeWall::Error;
    CHECK(lane_error_wall);
    // Lane 2 has no filament: the error marks its spool run.
    CHECK(plan.routes[2].style[0].wall == TubeWall::Error);
    // The active route carries no error.
    const Route& active = plan.routes[plan.active_route];
    for (int i = 0; i < active.path.count; i++)
        CHECK(active.style[i].wall != TubeWall::Error);
}

TEST_CASE("FilamentPath: the system error belongs to the unit that owns it",
          "[filament-path][plan][lane_error]") {
    helix::AmsSystemInfo info;
    info.units.resize(2);
    info.units[0].slot_count = 4;
    info.units[0].first_slot_global_index = 0;
    info.units[1].slot_count = 2;
    info.units[1].first_slot_global_index = 4;
    for (int u = 0; u < 2; u++) {
        info.units[u].slots.resize(info.units[u].slot_count);
        for (int s = 0; s < info.units[u].slot_count; s++)
            info.units[u].slots[s].global_index = info.units[u].first_slot_global_index + s;
    }

    info.current_slot = 5;
    CHECK(helix::ui::ams_detail_error_in_view(info, 1));
    CHECK_FALSE(helix::ui::ams_detail_error_in_view(info, 0));
    CHECK(helix::ui::ams_detail_error_in_view(info, -1));

    info.current_slot = -1;
    CHECK_FALSE(helix::ui::ams_detail_error_in_view(info, 0));
    info.units[0].slots[2].error = helix::SlotError{"jam", helix::SlotError::ERROR};
    CHECK(helix::ui::ams_detail_error_in_view(info, 0));
    CHECK_FALSE(helix::ui::ams_detail_error_in_view(info, 1));
}

TEST_CASE_METHOD(XMLTestFixture, "FilamentPath: a lane of unknown color still reads as loaded",
                 "[filament-path][plan][lane_error]") {
    // The tube is filled, and its band's gray stands apart from the empty wall
    // in both themes.
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    d->slot_filament_states[3] = {PathSegment::LANE, helix::AMS_DEFAULT_SLOT_COLOR};
    const PathPlan& plan = plan_for(*d);
    CHECK(plan.routes[3].style[0].filled);
    CHECK_FALSE(lv_color_eq(plan.routes[3].style[0].bore, BG));
    CHECK_FALSE(plan.routes[2].style[0].filled);
    const SensorBand* load = band_at(plan, 350, load_band_y(*d, 3));
    REQUIRE(load != nullptr);
    CHECK(load->state == BandState::Loaded);

    const lv_color_t gray = lv_color_hex(helix::AMS_DEFAULT_SLOT_COLOR);
    for (const char* wall : {"filament_idle_dark", "filament_idle_light"}) {
        CAPTURE(wall);
        const lv_color_t w = theme_manager_get_color(wall);
        const int dist = std::abs(w.red - gray.red) + std::abs(w.green - gray.green) +
                         std::abs(w.blue - gray.blue);
        CHECK(dist >= 60);
    }
}

TEST_CASE("FilamentPath geometry: split_path cuts a path where asked", "[filament-path][plan]") {
    pg::FilamentPath p;
    pg::route_orthogonal(p, 100, 0, 200, 100, 12.0f);
    const float total = pg::path_length(p);
    for (float frac : {0.0f, 0.3f, 0.5f, 0.8f, 1.0f}) {
        CAPTURE(frac);
        pg::FilamentPath head, tail;
        pg::split_path(p, total * frac, head, tail);
        CHECK(pg::path_length(head) == Catch::Approx(total * frac).margin(0.05));
        CHECK(pg::path_length(tail) == Catch::Approx(total * (1 - frac)).margin(0.05));
        if (head.count > 0 && tail.count > 0) {
            const pg::PathPoint a = seg_end(head.segs[head.count - 1]);
            CHECK(near(seg_start(tail.segs[0]), a.x, a.y, 0.05f));
        }
    }
}

TEST_CASE("FilamentPath plan: bowden progress fills the output tube in proportion",
          "[filament-path][plan][bowden]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::OUTPUT);

    // The hub bottom (200, 140) to the bypass merge (200, 232).
    auto filled_end_y = [&](int fill) {
        d->bowden_fill = fill;
        const PathPlan& plan = plan_for(*d);
        const Route& r = plan.routes[plan.active_route];
        CHECK(contiguous(r.path));
        const pg::FilamentPath f = filled_prefix(r);
        REQUIRE(f.count > 0);
        return seg_end(f.segs[f.count - 1]).y;
    };
    CHECK(filled_end_y(-1) == Catch::Approx(232).margin(0.5));
    CHECK(filled_end_y(100) == Catch::Approx(232).margin(0.5));
    CHECK(filled_end_y(50) == Catch::Approx(186).margin(0.5));
    CHECK(filled_end_y(25) == Catch::Approx(163).margin(0.5));

    // Past the bowden the progress no longer applies.
    load_active(*d, 1, PathSegment::NOZZLE);
    d->bowden_fill = 10;
    const PathPlan& plan = plan_for(*d);
    const pg::FilamentPath f = filled_prefix(plan.routes[plan.active_route]);
    CHECK(seg_end(f.segs[f.count - 1]).y > 232);
}

TEST_CASE("FilamentPath plan: a buffer fault tints the buffer box, not the hub",
          "[filament-path][plan][hub_tint]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->theme.color_hub_bg = lv_color_hex(0x303030);
    d->theme.color_hub_border = lv_color_hex(0x707070);
    d->theme.color_buffer[2] = lv_color_hex(0xD03030);
    d->buffer_present = true;
    d->buffer_fault_state = 2;
    load_active(*d, 1, PathSegment::NOZZLE);
    const LinearHubFrame f = compute_linear_hub_frame(*d, geometry(), GLYPH_TOP);

    const BoxColors hub = resolve_hub_tint(*d, f, true);
    CHECK(lv_color_eq(hub.border, d->theme.color_hub_border));
    CHECK(lv_color_eq(hub.bg, ph_blend(f.hub_bg, lv_color_hex(SLOT_COLORS[1]), 0.33f)));
    const BoxColors buffer = buffer_box_colors(*d, true, lv_color_hex(SLOT_COLORS[1]));
    CHECK(lv_color_eq(buffer.border, d->theme.color_buffer[2]));

    // An error at the hub is the hub's.
    d->error_segment = static_cast<int>(PathSegment::HUB);
    const LinearHubFrame fe = compute_linear_hub_frame(*d, geometry(), GLYPH_TOP);
    CHECK(lv_color_eq(resolve_hub_tint(*d, fe, true).border, fe.error_color));
}

TEST_CASE("FilamentPath plan: the lanes enter at the spool box's front edge",
          "[filament-path][plan][lane_entry]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    BaseGeometry g = geometry();
    g.lane_entry_y = -20; // the box front, above the canvas top
    const LinearHubFrame f = compute_linear_hub_frame(*d, g, GLYPH_TOP);
    REQUIRE(f.prep_on_box_edge);
    static PathPlan plan;
    plan_linear_hub(f, *d, g, plan);

    for (int i = 0; i < 4; i++) {
        CAPTURE(i);
        const Route& r = plan.routes[i];
        // The tube starts at the box edge, and the prep band clamps that edge
        // after the box is drawn.
        CHECK(near(seg_start(r.path.segs[0]), 50.0f + 100 * i, -20));
        CHECK(contiguous(r.path));
        const SensorBand* prep = band_at(plan, 50.0f + 100 * i, -20);
        REQUIRE(prep != nullptr);
        CHECK(prep->on_box_edge);
        // The load band just below it.
        // The load band stays on the lane run, clear of the prep band by two
        // tube gauges.
        const float load_y = (40.0f + f.hub_fan[i].pts[1].y) / 2;
        CHECK(load_y - (-20) >= 2 * d->theme.tube_gauge);
        const SensorBand* load = band_at(plan, 50.0f + 100 * i, load_y);
        REQUIRE(load != nullptr);
        CHECK_FALSE(load->on_box_edge);
    }
}

// ============================================================================
// PARALLEL and MIXED
// ============================================================================
// Same 400x400 frame, default glyph at tool scale 6 (bottom 16 below its
// center). PARALLEL: entry -48, sensor 198, toolhead 280 (glyph bottom 296),
// nozzle top 268. MIXED: sensor 78, hub 159 (h 38, top 140, bottom 178) at
// x 300 for hub lanes 2 and 3, toolhead 304 (glyph bottom 320), nozzle top 292.

namespace {

PathPlan& parallel_plan(const FilamentPathData& d) {
    static PathPlan plan;
    plan_parallel(d, geometry(), plan);
    return plan;
}

PathPlan& mixed_plan(const FilamentPathData& d) {
    static PathPlan plan;
    const BaseGeometry g = geometry();
    plan_mixed(compute_mixed_frame(d, g), d, g, plan);
    return plan;
}

} // namespace

TEST_CASE("FilamentPath plan: idle PARALLEL tools are one stroke each through their sensor",
          "[filament-path][plan][parallel]") {
    auto d = make_data(helix::PathTopology::PARALLEL);
    const PathPlan& plan = parallel_plan(*d);

    REQUIRE(plan.route_count == 4);
    CHECK(plan.active_route == -1);
    const float sensor_y = 198;
    for (int i = 0; i < 4; i++) {
        const Route& r = plan.routes[i];
        Stroke strokes[16];
        CHECK(coalesce(r, strokes, 16) == 1);
        CHECK(near(seg_start(r.path.segs[0]), 50.0f + 100 * i, -48));
        CHECK(near(seg_end(r.path.segs[r.path.count - 1]), 50.0f + 100 * i, 268));
        CHECK(has_boundary(r.path, 50.0f + 100 * i, sensor_y));
    }
    REQUIRE(plan.band_count == 4);
    for (int i = 0; i < 4; i++) {
        const SensorBand* b = band_at(plan, 50.0f + 100 * i, sensor_y);
        REQUIRE(b != nullptr);
        CHECK(b->state == BandState::Empty);
    }
}

TEST_CASE("FilamentPath plan: the mounted PARALLEL tool is one active stroke",
          "[filament-path][plan][parallel]") {
    auto d = make_data(helix::PathTopology::PARALLEL);
    load_active(*d, 2, PathSegment::NOZZLE);
    d->slot_filament_states[0] = {PathSegment::TOOLHEAD, SLOT_COLORS[0]};
    const PathPlan& plan = parallel_plan(*d);
    const float sensor_y = 198;

    REQUIRE(plan.active_route == 2);
    Stroke strokes[16];
    REQUIRE(coalesce(plan.routes[2], strokes, 16) == 1);
    CHECK(strokes[0].style.wall == TubeWall::Active);
    CHECK(strokes[0].style.filled);
    const SensorBand* mounted = band_at(plan, 250, sensor_y);
    REQUIRE(mounted != nullptr);
    CHECK(mounted->state == BandState::Active);

    // A docked tool loaded to its sensor keeps a band in its own color.
    const SensorBand* docked = band_at(plan, 50, sensor_y);
    REQUIRE(docked != nullptr);
    CHECK(docked->state == BandState::Loaded);
    CHECK(lv_color_eq(docked->fill, lv_color_hex(SLOT_COLORS[0])));
}

TEST_CASE("FilamentPath plan: MIXED direct lanes and the hub trunk reach a nozzle",
          "[filament-path][plan][mixed]") {
    auto d = make_data(helix::PathTopology::MIXED);
    d->slot_is_hub_routed[2] = true;
    d->slot_is_hub_routed[3] = true;
    const PathPlan& plan = mixed_plan(*d);

    int at_nozzle = 0;
    for (int i = 0; i < plan.route_count; i++) {
        const Route& r = plan.routes[i];
        REQUIRE(r.path.count > 0);
        CHECK(contiguous(r.path));
        at_nozzle += std::fabs(seg_end(r.path.segs[r.path.count - 1]).y - 292) < 0.01f;
    }
    CHECK(at_nozzle == 3);
    for (int i : {2, 3}) {
        const Route& r = plan.routes[i];
        CHECK(seg_end(r.path.segs[r.path.count - 1]).y == Catch::Approx(140));
    }
    REQUIRE(plan.trunk_route >= 0);
    const Route& t = plan.routes[plan.trunk_route];
    CHECK(near(seg_start(t.path.segs[0]), 300, 178));
    CHECK(near(seg_end(t.path.segs[t.path.count - 1]), 300, 292));
    CHECK(plan.band_count == 4);
}

TEST_CASE("FilamentPath plan: the MIXED trunk fills from the first hub lane at the nozzle",
          "[filament-path][plan][mixed]") {
    auto d = make_data(helix::PathTopology::MIXED);
    d->slot_is_hub_routed[2] = true;
    d->slot_is_hub_routed[3] = true;

    SECTION("no hub lane at the nozzle: empty") {
        d->slot_filament_states[2] = {PathSegment::TOOLHEAD, SLOT_COLORS[2]};
        const PathPlan& plan = mixed_plan(*d);
        CHECK_FALSE(plan.routes[plan.trunk_route].style[0].filled);
    }
    SECTION("the lane at the nozzle colors it, not an earlier staged lane") {
        d->slot_filament_states[2] = {PathSegment::TOOLHEAD, SLOT_COLORS[2]};
        d->slot_filament_states[3] = {PathSegment::NOZZLE, SLOT_COLORS[3]};
        const PathPlan& plan = mixed_plan(*d);
        const SpanStyle t = plan.routes[plan.trunk_route].style[0];
        CHECK(t.filled);
        CHECK(lv_color_eq(t.bore, lv_color_hex(SLOT_COLORS[3])));
    }
    SECTION("two lanes at the nozzle: the first wins") {
        d->slot_filament_states[2] = {PathSegment::NOZZLE, SLOT_COLORS[2]};
        d->slot_filament_states[3] = {PathSegment::NOZZLE, SLOT_COLORS[3]};
        const PathPlan& plan = mixed_plan(*d);
        CHECK(
            lv_color_eq(plan.routes[plan.trunk_route].style[0].bore, lv_color_hex(SLOT_COLORS[2])));
    }
}

TEST_CASE("FilamentPath plan: a MIXED direct lane short of the nozzle fills to its sensor",
          "[filament-path][plan][mixed]") {
    auto d = make_data(helix::PathTopology::MIXED);
    d->slot_is_hub_routed[2] = true;
    d->slot_is_hub_routed[3] = true;
    d->slot_filament_states[0] = {PathSegment::TOOLHEAD, SLOT_COLORS[0]};
    const PathPlan& plan = mixed_plan(*d);

    const Route& r = plan.routes[0];
    REQUIRE(r.path.count == 2);
    CHECK(r.style[0].filled);
    CHECK_FALSE(r.style[1].filled);
    CHECK(near(seg_end(r.path.segs[1]), 50, 292));
}

TEST_CASE("FilamentPath plan: PARALLEL and MIXED show an error on the mounted lane",
          "[filament-path][plan]") {
    SECTION("PARALLEL") {
        auto d = make_data(helix::PathTopology::PARALLEL);
        load_active(*d, 1, PathSegment::NOZZLE);
        d->error_segment = static_cast<int>(PathSegment::NOZZLE);
        const PathPlan& plan = parallel_plan(*d);
        const Route& r = plan.routes[1];
        CHECK(r.style[0].wall == TubeWall::Active);
        CHECK(r.style[1].wall == TubeWall::Error);
        CHECK(plan.routes[0].style[1].wall == TubeWall::Plain);
    }
    SECTION("MIXED direct lane") {
        auto d = make_data(helix::PathTopology::MIXED);
        d->slot_is_hub_routed[2] = true;
        d->slot_is_hub_routed[3] = true;
        load_active(*d, 0, PathSegment::NOZZLE);
        d->error_segment = static_cast<int>(PathSegment::NOZZLE);
        const PathPlan& plan = mixed_plan(*d);
        CHECK(plan.routes[0].style[1].wall == TubeWall::Error);
        // A direct lane is mounted: the idle hub trunk is not its route.
        CHECK(plan.routes[plan.trunk_route].style[0].wall == TubeWall::Plain);
    }
    SECTION("MIXED idle trunk with nothing mounted") {
        auto d = make_data(helix::PathTopology::MIXED);
        d->slot_is_hub_routed[2] = true;
        d->slot_is_hub_routed[3] = true;
        d->error_segment = static_cast<int>(PathSegment::NOZZLE);
        const PathPlan& plan = mixed_plan(*d);
        CHECK(plan.active_route == -1);
        CHECK(plan.routes[plan.trunk_route].style[0].wall == TubeWall::Error);
    }
}

TEST_CASE("FilamentPath plan: PARALLEL and MIXED bands do not wait on a toolhead sensor",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::PARALLEL);
    d->has_toolhead_sensor = false;
    CHECK(parallel_plan(*d).band_count == 4);

    d->topology = static_cast<int>(helix::PathTopology::MIXED);
    d->slot_is_hub_routed[2] = true;
    d->slot_is_hub_routed[3] = true;
    CHECK(mixed_plan(*d).band_count == 4);
}

TEST_CASE("FilamentPath plan: the fitted hub stays on the canvas and catches every lane",
          "[filament-path][plan][fan-clearance]") {
    struct Canvas {
        const char* name;
        int32_t w, h;
    };
    const Canvas canvas =
        GENERATE(Canvas{"micro 285x138", 285, 138}, Canvas{"800x480 470x294", 470, 294});
    const int count = GENERATE(2, 4, 8);
    CAPTURE(canvas.name, count);

    auto d = make_data(helix::PathTopology::HUB);
    d->slot_count = count;
    for (int i = 0; i < count; i++)
        d->slot_has_prep_sensor[i] = true;
    BaseGeometry g;
    g.width = canvas.w;
    g.height = canvas.h;
    g.slot_count = count;
    for (int i = 0; i < count; i++)
        g.slot_x[i] = (int32_t)(canvas.w * (i + 0.5f) / count);
    g.center_x = (g.slot_x[0] + g.slot_x[count - 1]) / 2;
    const int32_t nozzle_y = (int32_t)(canvas.h * NOZZLE_Y_RATIO);
    const LinearHubFrame f = compute_linear_hub_frame(*d, g, nozzle_y - 31);
    static PathPlan plan;
    plan_linear_hub(f, *d, g, plan);

    const float left = f.center_x - f.hub_box_w / 2.0f;
    const float right = f.center_x + f.hub_box_w / 2.0f;
    CAPTURE(f.hub_box_w, left, right);
    CHECK(left >= 0);
    CHECK(right <= canvas.w);
    const float hub_top = (float)(f.hub_y - f.hub_h / 2);
    for (int i = 0; i < count; i++) {
        const Route& r = plan.routes[i];
        const pg::PathPoint end = seg_end(r.path.segs[r.path.count - 1]);
        CAPTURE(i, end.x, end.y);
        CHECK(end.x >= left);
        CHECK(end.x <= right);
        CHECK(end.y == Catch::Approx(hub_top));
    }
}

TEST_CASE("FilamentPath plan: a 4-lane micro hub with the bypass hidden keeps its lanes apart",
          "[filament-path][plan][fan-clearance]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->show_bypass = false;
    BaseGeometry g;
    g.width = 285;
    g.height = 138;
    g.slot_count = 4;
    for (int i = 0; i < 4; i++)
        g.slot_x[i] = (int32_t)(285 * (i + 0.5f) / 4);
    g.center_x = (g.slot_x[0] + g.slot_x[3]) / 2;
    const int32_t nozzle_y = (int32_t)(138 * NOZZLE_Y_RATIO);
    const LinearHubFrame f = compute_linear_hub_frame(*d, g, nozzle_y - 31);
    static PathPlan plan;
    plan_linear_hub(f, *d, g, plan);

    const int32_t hub_top = f.hub_y - f.hub_h / 2;
    const int32_t fan_start = f.prep_y + f.sensor_r;
    INFO("micro 4-lane fan zone " << (hub_top - fan_start) << " px (prep+r " << fan_start
                                  << " -> hub top " << hub_top << "), hub width " << f.hub_box_w
                                  << " of slot row + 16 = " << (g.slot_x[3] - g.slot_x[0] + 16)
                                  << ", stacked " << f.hub_stacked);

    // Separation: gauge 5 + halo 6 + 2 px.
    float nearest = 10000;
    std::vector<pg::PathPoint> previous;
    for (int i = 0; i < 4; i++) {
        const pg::FilamentPath& p = plan.routes[i].path;
        const float len = pg::path_length(p);
        std::vector<pg::PathPoint> samples;
        for (int s = 0; s <= 500; s++)
            samples.push_back(pg::path_point_at(p, len * s / 500));
        for (auto a : previous)
            for (auto b : samples)
                nearest = std::min(nearest, std::hypot(a.x - b.x, a.y - b.y));
        previous = std::move(samples);
    }
    CHECK(nearest >= 13 - 0.1f);
}

// ============================================================================
// Off-page stubs
// ============================================================================

namespace {

LinearHubFrame frame_with_offpage(FilamentPathData& d, int before, bool before_drying, int after,
                                  bool after_drying, const BaseGeometry& g = geometry()) {
    d.offpage_before = before;
    d.offpage_before_drying = before_drying;
    d.offpage_after = after;
    d.offpage_after_drying = after_drying;
    return compute_linear_hub_frame(d, g, GLYPH_TOP);
}

// dy/dx of a polyline leg, with x taken as a positive run.
float slope_of(pg::PathPoint a, pg::PathPoint b) {
    return (b.y - a.y) / std::fabs(b.x - a.x);
}

} // namespace

TEST_CASE("FilamentPath offpage: a stub exists only on a side that has units",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    const LinearHubFrame none = frame_with_offpage(*d, 0, false, 0, false);
    CHECK_FALSE(none.stubs[0].present);
    CHECK_FALSE(none.stubs[1].present);

    const LinearHubFrame left = frame_with_offpage(*d, 2, false, 0, false);
    CHECK(left.stubs[0].present);
    CHECK_FALSE(left.stubs[1].present);
    CHECK(left.stubs[0].count == 2);

    const LinearHubFrame right = frame_with_offpage(*d, 0, false, 7, true);
    CHECK_FALSE(right.stubs[0].present);
    CHECK(right.stubs[1].present);
    CHECK(right.stubs[1].count == 7);

    const LinearHubFrame both = frame_with_offpage(*d, 1, false, 3, false);
    CHECK(both.stubs[0].present);
    CHECK(both.stubs[1].present);
}

TEST_CASE("FilamentPath offpage: the drying flag belongs to the side that holds the drying unit",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    const LinearHubFrame f = frame_with_offpage(*d, 2, false, 7, true);
    CHECK_FALSE(f.stubs[0].drying);
    CHECK(f.stubs[1].drying);
    const LinearHubFrame g = frame_with_offpage(*d, 2, true, 7, false);
    CHECK(g.stubs[0].drying);
    CHECK_FALSE(g.stubs[1].drying);
}

TEST_CASE("FilamentPath offpage: a stub is a parallel diagonal outside the outermost lane",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    const LinearHubFrame f = frame_with_offpage(*d, 2, false, 3, false);
    const pg::PathPoint* left_lane = f.hub_fan[0].pts;
    const pg::PathPoint* right_lane = f.hub_fan[3].pts;
    const OffpageStub& left = f.stubs[0];
    const OffpageStub& right = f.stubs[1];
    REQUIRE(left.present);
    REQUIRE(right.present);

    // Same slope as the outermost lane of its side: the diagonals are parallel.
    CHECK(slope_of(left.pts[1], left.pts[2]) ==
          Catch::Approx(slope_of(left_lane[1], left_lane[2])));
    CHECK(slope_of(right.pts[1], right.pts[2]) ==
          Catch::Approx(slope_of(right_lane[1], right_lane[2])));
    // The left stub leans the lane's way: down and toward the hub.
    CHECK(left.pts[2].x > left.pts[1].x);
    CHECK(right.pts[2].x < right.pts[1].x);

    // Its entry sits beyond the outermost lane's entry, on the outer side.
    CHECK(left.pts[3].x < left_lane[3].x);
    CHECK(right.pts[3].x > right_lane[3].x);
    // It drops straight into the hub's top edge from the same height every lane does.
    const float hub_top = (float)(f.hub_y - f.hub_h / 2);
    CHECK(left.pts[3].y == Catch::Approx(hub_top));
    CHECK(left.pts[2].x == Catch::Approx(left.pts[3].x));
    CHECK(left.pts[2].y == Catch::Approx(left_lane[2].y));
    CHECK(right.pts[3].y == Catch::Approx(hub_top));
    CHECK(right.pts[2].y == Catch::Approx(left_lane[2].y));
}

TEST_CASE("FilamentPath offpage: both stubs' runs sit at one height and keep their lanes' slopes",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    // A hub off the middle of the lane row gives the two sides different slopes.
    BaseGeometry g = geometry();
    g.center_x = 130;
    const LinearHubFrame f = frame_with_offpage(*d, 2, false, 3, false, g);
    REQUIRE(f.stubs[0].present);
    REQUIRE(f.stubs[1].present);

    const float left_lane = slope_of(f.hub_fan[0].pts[1], f.hub_fan[0].pts[2]);
    const float right_lane = slope_of(f.hub_fan[3].pts[1], f.hub_fan[3].pts[2]);
    REQUIRE(std::fabs(left_lane - right_lane) > 0.1f);

    CHECK(f.stubs[0].pts[1].y == Catch::Approx(f.stubs[1].pts[1].y));
    CHECK(f.stubs[0].pts[0].y == Catch::Approx(f.stubs[1].pts[0].y));
    CHECK(f.stubs[0].label_cy == f.stubs[1].label_cy);
    CHECK(slope_of(f.stubs[0].pts[1], f.stubs[0].pts[2]) == Catch::Approx(left_lane));
    CHECK(slope_of(f.stubs[1].pts[1], f.stubs[1].pts[2]) == Catch::Approx(right_lane));
    // The run never rises above the lanes' bends.
    const float highest_bend = std::min(f.hub_fan[0].pts[1].y, f.hub_fan[3].pts[1].y);
    CHECK(f.stubs[0].pts[1].y >= highest_bend - 0.01f);

    // The same rise whichever sides a page shows.
    const LinearHubFrame left_only = frame_with_offpage(*d, 2, false, 0, false, g);
    CHECK(left_only.stubs[0].pts[1].y == Catch::Approx(f.stubs[0].pts[1].y));
}

TEST_CASE("FilamentPath offpage: the drops land inside the widened hub box",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    const LinearHubFrame plain = frame_with_offpage(*d, 0, false, 0, false);
    const LinearHubFrame f = frame_with_offpage(*d, 1, false, 1, false);
    REQUIRE(f.stubs[0].present);
    REQUIRE(f.stubs[1].present);

    CHECK(f.hub_box_w > plain.hub_box_w);
    const float left_edge = (float)f.center_x - f.hub_box_w / 2.0f;
    const float right_edge = (float)f.center_x + f.hub_box_w / 2.0f;
    CHECK(f.stubs[0].pts[3].x > left_edge);
    CHECK(f.stubs[1].pts[3].x < right_edge);
    // Every real lane still lands inside, between the two stubs.
    for (int i = 0; i < 4; i++) {
        CAPTURE(i);
        CHECK(f.hub_fan[i].pts[3].x > f.stubs[0].pts[3].x);
        CHECK(f.hub_fan[i].pts[3].x < f.stubs[1].pts[3].x);
    }
}

TEST_CASE("FilamentPath offpage: the lanes and the hub keep their places from page to page",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    const LinearHubFrame left_only = frame_with_offpage(*d, 3, false, 0, false);
    const LinearHubFrame right_only = frame_with_offpage(*d, 0, false, 3, false);
    const LinearHubFrame both = frame_with_offpage(*d, 2, false, 2, false);

    CHECK(left_only.hub_box_w == right_only.hub_box_w);
    CHECK(left_only.hub_box_w == both.hub_box_w);
    CHECK(left_only.center_x == both.center_x);
    CHECK(left_only.hub_y == both.hub_y);
    for (int i = 0; i < 4; i++) {
        CAPTURE(i);
        for (int k = 0; k < 4; k++) {
            CHECK(left_only.hub_fan[i].pts[k].x == Catch::Approx(both.hub_fan[i].pts[k].x));
            CHECK(left_only.hub_fan[i].pts[k].y == Catch::Approx(both.hub_fan[i].pts[k].y));
            CHECK(right_only.hub_fan[i].pts[k].x == Catch::Approx(both.hub_fan[i].pts[k].x));
        }
    }
}

TEST_CASE("FilamentPath offpage: the horizontal run is short and stays out of the edge columns",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    const BaseGeometry g = geometry();

    const LinearHubFrame f = frame_with_offpage(*d, 2, false, 2, false);
    for (int side = 0; side < 2; side++) {
        CAPTURE(side);
        const OffpageStub& s = f.stubs[side];
        REQUIRE(s.present);
        // Level, outward of the diagonal, and no longer than about 5/6 of the hub width.
        CHECK(s.pts[0].y == Catch::Approx(s.pts[1].y));
        const float run = std::fabs(s.pts[0].x - s.pts[1].x);
        CHECK(run <= d->theme.hub_width * 5 / 6 + 0.5f);
        CHECK(run >= 10.0f - 0.5f);
        CHECK(((side == 0) ? (s.pts[0].x < s.pts[1].x) : (s.pts[0].x > s.pts[1].x)));
        CHECK(s.pts[0].x >= 0);
        CHECK(s.pts[0].x <= g.width);
    }

    // An overlaid control on each edge pulls the run back from it.
    d->edge_reserve = 12;
    const LinearHubFrame narrow = frame_with_offpage(*d, 2, false, 2, false);
    CHECK(narrow.stubs[0].pts[0].x >= 12 - 0.5f);
    CHECK(narrow.stubs[1].pts[0].x <= g.width - 12 + 0.5f);
    CHECK(std::fabs(narrow.stubs[0].pts[0].x - narrow.stubs[0].pts[1].x) <
          std::fabs(f.stubs[0].pts[0].x - f.stubs[0].pts[1].x));
}

TEST_CASE("FilamentPath offpage: no hub box, no stub", "[filament-path][plan][offpage]") {
    auto linear = make_data(helix::PathTopology::LINEAR);
    const LinearHubFrame lf = frame_with_offpage(*linear, 2, true, 2, true);
    CHECK_FALSE(lf.stubs[0].present);
    CHECK_FALSE(lf.stubs[1].present);

    auto on_head = make_data(helix::PathTopology::HUB);
    on_head->hub_on_toolhead = true;
    const LinearHubFrame hf = frame_with_offpage(*on_head, 2, false, 2, false);
    CHECK_FALSE(hf.stubs[0].present);
    CHECK_FALSE(hf.stubs[1].present);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath offpage: the label reads N units, 1 unit for one",
                 "[filament-path][plan][offpage]") {
    CHECK(offpage_label_text(1) == "1 unit");
    CHECK(offpage_label_text(2) == "2 units");
    CHECK(offpage_label_text(11) == "11 units");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FilamentPath offpage: the hub box getter is the box the renderer draws",
                 "[filament-path][plan][offpage]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::HUB));
    ui_filament_path_canvas_set_offpage_units(w, 2, false, 3, true);
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    REQUIRE(d->hits.hub_valid);
    lv_area_t box;
    REQUIRE(ui_filament_path_canvas_get_hub_box(w, &box));
    CHECK(area_eq(box, d->hits.hub));
    // The stubs widened it past the plain hub.
    ui_filament_path_canvas_set_offpage_units(w, 0, false, 0, false);
    lv_area_t plain;
    REQUIRE(ui_filament_path_canvas_get_hub_box(w, &plain));
    CHECK(lv_area_get_width(&box) > lv_area_get_width(&plain));
    // Same center either way.
    CHECK((box.x1 + box.x2) / 2 == (plain.x1 + plain.x2) / 2);
    CHECK(box.y1 == plain.y1);
}

TEST_CASE("FilamentPath offpage: a label with no room is dropped and the stub drawn bare",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    const LinearHubFrame roomy = frame_with_offpage(*d, 2, false, 2, false);
    CHECK(roomy.stubs[0].labeled);
    CHECK(roomy.stubs[1].labeled);

    // The edge columns leave less than a run and a gap beside the diagonal.
    d->edge_reserve = 180;
    const LinearHubFrame tight = frame_with_offpage(*d, 2, true, 2, true);
    for (const OffpageStub& s : tight.stubs) {
        REQUIRE(s.present);
        CHECK_FALSE(s.labeled);
        CHECK(s.label_w == 0);
        CHECK(s.glyph_w == 0);
    }
}

TEST_CASE("FilamentPath offpage: a fixed hub is sized for its widest unit",
          "[filament-path][plan][offpage]") {
    auto narrow = make_data(helix::PathTopology::HUB);
    narrow->slot_count = 1;
    BaseGeometry g;
    g.width = 400;
    g.height = 400;
    g.slot_count = 1;
    g.slot_x[0] = 200;
    g.center_x = 200;

    const LinearHubFrame natural = compute_linear_hub_frame(*narrow, g, GLYPH_TOP);
    narrow->fixed_hub_lanes = 4;
    const LinearHubFrame fixed = compute_linear_hub_frame(*narrow, g, GLYPH_TOP);
    CHECK(fixed.hub_box_w > natural.hub_box_w);
    // Four lanes at the target entry spacing, plus the margins.
    CHECK(fixed.hub_box_w >= 3 * 22 + 2 * 8);

    // Stubs land at the ends of the row either way: the same x as a four-lane unit's.
    auto wide = make_data(helix::PathTopology::HUB);
    wide->fixed_hub_lanes = 4;
    narrow->offpage_before = 1;
    wide->offpage_before = 1;
    const LinearHubFrame narrow_stub = compute_linear_hub_frame(*narrow, g, GLYPH_TOP);
    const LinearHubFrame wide_stub = compute_linear_hub_frame(*wide, geometry(), GLYPH_TOP);
    REQUIRE(narrow_stub.stubs[0].present);
    REQUIRE(wide_stub.stubs[0].present);
    CHECK(narrow_stub.hub_box_w >= 3 * 22 + 2 * 8 + 2 * 22);
    CHECK(narrow_stub.stubs[0].pts[3].x - (200 - narrow_stub.hub_box_w / 2.0f) ==
          Catch::Approx(8.0f));
}

TEST_CASE("FilamentPath offpage: the hub box is identical on every page of one hub",
          "[filament-path][plan][offpage]") {
    // A four-lane unit, a two-lane unit and a one-lane unit of one hub, on a fixed hub.
    auto frame_of = [](int lanes, int fixed, int before, int after) {
        auto d = make_data(helix::PathTopology::HUB);
        d->slot_count = lanes;
        d->fixed_hub_lanes = fixed;
        d->fixed_hub_pitch = 100;
        d->show_bypass = true; // not stacked above the glyph, so the hub's row is free to move
        BaseGeometry g;
        g.width = 400;
        g.height = 240; // short enough that a hub of three or more lanes borrows height
        g.slot_count = lanes;
        g.center_x = 200;
        for (int i = 0; i < lanes; i++)
            g.slot_x[i] = 200 + (int)((i - (lanes - 1) / 2.0f) * 100);
        return frame_with_offpage(*d, before, false, after, false, g);
    };
    const LinearHubFrame four = frame_of(4, 4, 1, 1);
    const LinearHubFrame two = frame_of(2, 4, 1, 1);
    const LinearHubFrame one = frame_of(1, 4, 1, 0);
    const LinearHubFrame last = frame_of(4, 4, 0, 1);

    for (const LinearHubFrame* f : {&two, &one, &last}) {
        CHECK(f->hub_box_w == four.hub_box_w);
        CHECK(f->center_x == four.center_x);
        CHECK(f->hub_y == four.hub_y);
        CHECK(f->hub_h == four.hub_h);
    }
    // The lanes of a narrower unit enter inside the box, and the stubs drop at its ends.
    const float left = (float)four.center_x - four.hub_box_w / 2.0f;
    const float right = (float)four.center_x + four.hub_box_w / 2.0f;
    CHECK(one.hub_fan[0].pts[3].x > left);
    CHECK(one.hub_fan[0].pts[3].x < right);
    CHECK(one.stubs[0].pts[3].x == Catch::Approx(four.stubs[0].pts[3].x));

    // A hub of fewer lanes is its own box.
    const LinearHubFrame small = frame_of(2, 2, 1, 1);
    CHECK(small.hub_box_w != four.hub_box_w);

    // The box follows the widest unit's lane pitch, not the shown unit's.
    auto with_pitch = [](int pitch) {
        auto d = make_data(helix::PathTopology::HUB);
        d->slot_count = 1;
        d->fixed_hub_lanes = 4;
        d->fixed_hub_pitch = pitch;
        d->show_bypass = true;
        BaseGeometry g;
        g.width = 400;
        g.height = 240;
        g.slot_count = 1;
        g.center_x = 200;
        g.slot_x[0] = 200;
        return frame_with_offpage(*d, 1, false, 1, false, g).hub_box_w;
    };
    CHECK(with_pitch(120) != with_pitch(40));
}

TEST_CASE("FilamentPath offpage: a fixed hub's stubs lie the same on every page of one hub",
          "[filament-path][plan][offpage]") {
    auto frame_of = [](int lanes) {
        auto d = make_data(helix::PathTopology::HUB);
        d->slot_count = lanes;
        d->fixed_hub_lanes = 4;
        d->fixed_hub_pitch = 100;
        d->show_bypass = true;
        BaseGeometry g;
        g.width = 400;
        g.height = 240;
        g.slot_count = lanes;
        g.center_x = 200;
        for (int i = 0; i < lanes; i++)
            g.slot_x[i] = 200 + (int)((i - (lanes - 1) / 2.0f) * 100);
        return frame_with_offpage(*d, 1, false, 1, false, g);
    };
    // A one-lane unit has no lane on one side of the hub at all; its stubs must still lean
    // the way the four-lane unit's lanes do, not at the steepest slope the rules allow.
    const LinearHubFrame four = frame_of(4);
    const LinearHubFrame one = frame_of(1);
    for (int side = 0; side < 2; side++) {
        CAPTURE(side);
        for (int k = 0; k < 4; k++) {
            CAPTURE(k);
            CHECK(one.stubs[side].pts[k].x == Catch::Approx(four.stubs[side].pts[k].x));
            CHECK(one.stubs[side].pts[k].y == Catch::Approx(four.stubs[side].pts[k].y));
        }
    }
}

TEST_CASE("FilamentPath offpage: each stub leans like the shown unit's outermost lane",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->fixed_hub_lanes = 4;
    d->fixed_hub_pitch = 100;
    d->show_bypass = true;
    // The spools sit left of the hub's center line, as with an env chip on their right.
    BaseGeometry g = geometry();
    g.height = 240;
    g.center_x = 200;
    for (int i = 0; i < 4; i++)
        g.slot_x[i] = 50 + 100 * i - 40;
    const LinearHubFrame f = frame_with_offpage(*d, 1, false, 1, false, g);
    REQUIRE(f.stubs[0].present);
    REQUIRE(f.stubs[1].present);

    const pg::MergeLaneOut& left_lane = f.hub_fan[0];
    const pg::MergeLaneOut& right_lane = f.hub_fan[3];
    const float left_slope = slope_of(left_lane.pts[1], left_lane.pts[2]);
    const float right_slope = slope_of(right_lane.pts[1], right_lane.pts[2]);
    REQUIRE(std::fabs(left_slope - right_slope) > 0.05f);

    auto min_gap = [](const OffpageStub& stub, const pg::MergeLaneOut& lane) {
        float best = 1e9f;
        for (int i = 0; i <= 20; i++) {
            const float t = i / 20.0f;
            const float sx = stub.pts[1].x + (stub.pts[2].x - stub.pts[1].x) * t;
            const float sy = stub.pts[1].y + (stub.pts[2].y - stub.pts[1].y) * t;
            for (int k = 0; k <= 200; k++) {
                const float u = k / 200.0f;
                const float lx = lane.pts[1].x + (lane.pts[2].x - lane.pts[1].x) * u;
                const float ly = lane.pts[1].y + (lane.pts[2].y - lane.pts[1].y) * u;
                best = std::min(best, std::hypot(sx - lx, sy - ly));
            }
        }
        return best;
    };
    CHECK(slope_of(f.stubs[0].pts[1], f.stubs[0].pts[2]) == Catch::Approx(left_slope));
    CHECK(slope_of(f.stubs[1].pts[1], f.stubs[1].pts[2]) == Catch::Approx(right_slope));
    CHECK(min_gap(f.stubs[0], left_lane) > 4.0f);
    CHECK(min_gap(f.stubs[1], right_lane) > 4.0f);
    // Level, as always.
    CHECK(f.stubs[0].pts[1].y == Catch::Approx(f.stubs[1].pts[1].y));
}

TEST_CASE("FilamentPath offpage: the edge columns bind only on the rows a control shares",
          "[filament-path][plan][offpage]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->edge_reserve = 180;
    const LinearHubFrame everywhere = frame_with_offpage(*d, 1, false, 1, false);
    REQUIRE(everywhere.stubs[0].present);
    // No rows named: the columns hold on every row and leave no room for a label.
    CHECK_FALSE(everywhere.stubs[0].labeled);

    const float run_y = everywhere.stubs[0].pts[1].y;
    // The control sits well below the stub's rows: the label has the whole width.
    d->keepout_y0 = (int32_t)run_y + 60;
    d->keepout_y1 = (int32_t)run_y + 120;
    const LinearHubFrame apart = frame_with_offpage(*d, 1, false, 1, false);
    CHECK(apart.stubs[0].labeled);
    CHECK(apart.stubs[1].labeled);

    // Moved onto the stub's rows, it binds again.
    d->keepout_y0 = (int32_t)run_y - 10;
    d->keepout_y1 = (int32_t)run_y + 10;
    const LinearHubFrame level = frame_with_offpage(*d, 1, false, 1, false);
    CHECK_FALSE(level.stubs[0].labeled);
    CHECK_FALSE(level.stubs[1].labeled);
}
