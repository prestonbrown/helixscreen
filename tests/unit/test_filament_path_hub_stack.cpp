// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_path_hub_stack.cpp
 * @brief HUB topology with the bypass hidden: the hub and buffer stack down
 *        onto the toolhead, and the output run reaches the nozzle as one
 *        contiguous route.
 */

#include "ui_filament_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "settings_manager.h"
#include "src/ui/filament/ui_filament_path_internal.h"
#include "src/ui/filament/ui_filament_path_plan.h"

#include <algorithm>
#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::fpath;
using helix::ui::tray::LaneJoin;

namespace {

constexpr int32_t kCanvasW = 420;
constexpr int32_t kCanvasH = 420;

struct HubCanvas {
    lv_obj_t* obj = nullptr;
    FilamentPathData* data = nullptr;
};

class HubStackFixture : public LVGLTestFixture {
  public:
    HubCanvas make(bool buffer, bool bypass, int slots = 2) {
        HubCanvas c;
        c.obj = ui_filament_path_canvas_create(test_screen());
        REQUIRE(c.obj != nullptr);
        lv_obj_set_size(c.obj, kCanvasW, kCanvasH);
        ui_filament_path_canvas_set_topology(c.obj, 1);
        ui_filament_path_canvas_set_slot_count(c.obj, slots);
        ui_filament_path_canvas_set_slot_width(c.obj, 80);
        ui_filament_path_canvas_set_slot_overlap(c.obj, 0);
        ui_filament_path_canvas_set_show_bypass(c.obj, bypass);
        ui_filament_path_canvas_set_buffer_info(c.obj, buffer, 0);
        c.data = get_data(c.obj);
        REQUIRE(c.data != nullptr);
        return c;
    }

    void load(const HubCanvas& c, int segment, int error_segment = 0) {
        ui_filament_path_canvas_set_active_slot(c.obj, 0);
        ui_filament_path_canvas_set_filament_color(c.obj, 0x44AAFF);
        ui_filament_path_canvas_set_filament_segment(c.obj, segment);
        ui_filament_path_canvas_set_error_segment(c.obj, error_segment);
    }

    void render(const HubCanvas& c) {
        lv_obj_update_layout(test_screen());
        ui_filament_path_canvas_refresh(c.obj);
        process_lvgl(100);
    }
};

pg::PathPoint seg_start(const pg::PathSeg& s) {
    if (s.type == pg::PathSeg::LINE)
        return s.p0;
    return {s.center.x + s.radius * std::cos(s.start_angle),
            s.center.y + s.radius * std::sin(s.start_angle)};
}

pg::PathPoint seg_end(const pg::PathSeg& s) {
    if (s.type == pg::PathSeg::LINE)
        return s.p1;
    const float a = s.start_angle + s.sweep;
    return {s.center.x + s.radius * std::cos(a), s.center.y + s.radius * std::sin(a)};
}

void check_contiguous(const pg::FilamentPath& route) {
    REQUIRE(route.count > 2);
    for (int i = 1; i < route.count; ++i) {
        CAPTURE(i);
        CHECK(seg_start(route.segs[i]).x ==
              Catch::Approx(seg_end(route.segs[i - 1]).x).margin(0.01));
        CHECK(seg_start(route.segs[i]).y ==
              Catch::Approx(seg_end(route.segs[i - 1]).y).margin(0.01));
    }
}

// The index of the route segment starting at y, or -1.
int segment_starting_at(const pg::FilamentPath& route, float y) {
    for (int i = 0; i < route.count; ++i)
        if (std::fabs(seg_start(route.segs[i]).y - y) < 0.5f)
            return i;
    return -1;
}

float inlet_y(const HubCanvas& c) {
    return (float)(c.data->path_cache.nozzle_y - c.data->theme.extruder_scale * 2);
}

// The plan the canvas painted, rebuilt from its own state.
const PathPlan& plan_of(const HubCanvas& c, LinearHubFrame* frame_out) {
    static PathPlan plan;
    const BaseGeometry g = compute_base_geometry(c.obj, c.data);
    const int32_t nozzle_y = g.y_off + (int32_t)(g.height * kNozzleYRatio);
    *frame_out = compute_linear_hub_frame(*c.data, g,
                                          toolhead_top_y(nozzle_y, c.data->theme.extruder_scale));
    plan_linear_hub(*frame_out, *c.data, g, plan);
    return plan;
}

int32_t ratio_y(const HubCanvas& c, float ratio) {
    lv_area_t a;
    lv_obj_get_coords(c.obj, &a);
    return a.y1 + static_cast<int32_t>(lv_area_get_height(&a) * ratio);
}

int32_t center_y(const lv_area_t& a) {
    return (a.y1 + a.y2) / 2;
}

} // namespace

TEST_CASE_METHOD(HubStackFixture, "HUB output run with a buffer reaches the toolhead unbroken",
                 "[filament-path][hub-stack]") {
    auto c = make(true, false);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE),
         static_cast<int>(helix::PathSegment::OUTPUT));
    render(c);
    REQUIRE(c.data->hits.buffer_valid);
    REQUIRE(c.data->hits.hub_valid);

    LinearHubFrame f;
    const PathPlan& plan = plan_of(c, &f);
    REQUIRE(plan.active_route == 0);
    const Route& r = plan.routes[0];
    Stroke strokes[16];
    const int n = coalesce(r, strokes, 16);
    int errors = 0;
    for (int i = 0; i < n; ++i) {
        if (strokes[i].style.wall != TubeWall::Error)
            continue;
        ++errors;
        // One stroke from the hub's bottom edge, across the buffer box, to the
        // toolhead sensor, where the nozzle run carries on.
        const float y0 = seg_start(r.path.segs[strokes[i].first]).y;
        const float y1 = seg_end(r.path.segs[strokes[i].end - 1]).y;
        CHECK(y0 == Catch::Approx(c.data->hits.hub.y2).margin(0.5));
        CHECK(y0 < c.data->hits.buffer.y1);
        CHECK(y1 > c.data->hits.buffer.y2);
        CHECK(y1 == Catch::Approx(f.toolhead_y).margin(0.01));
        REQUIRE(strokes[i].end < r.path.count);
        CHECK(seg_start(r.path.segs[strokes[i].end]).y == Catch::Approx(y1).margin(0.01));
    }
    CHECK(errors == 1);
    check_contiguous(r.path);
    CHECK(seg_end(r.path.segs[r.path.count - 1]).y == Catch::Approx(inlet_y(c)).margin(0.01));
}

namespace {
/// Sets the toolhead style for one case and restores the previous one.
struct ScopedToolheadStyle {
    helix::ToolheadStyle previous;
    explicit ScopedToolheadStyle(helix::ToolheadStyle style)
        : previous(prepared().get_toolhead_style()) {
        helix::SettingsManager::instance().set_toolhead_style(style);
    }
    // The style lives in a subject; without one, set_toolhead_style() stores nothing.
    static helix::SettingsManager& prepared() {
        helix::SettingsManager::instance().init_subjects();
        return helix::SettingsManager::instance();
    }
    ~ScopedToolheadStyle() {
        helix::SettingsManager::instance().set_toolhead_style(previous);
    }
};
} // namespace

TEST_CASE_METHOD(HubStackFixture,
                 "HUB without a bypass stacks the hub and buffer onto the toolhead",
                 "[filament-path][hub-stack]") {
    const auto style = GENERATE(helix::ToolheadStyle::DEFAULT, helix::ToolheadStyle::STEALTHBURNER,
                                helix::ToolheadStyle::A4T);
    ScopedToolheadStyle scoped(style);
    CAPTURE(static_cast<int>(style));

    SECTION("with a buffer") {
        auto c = make(true, false);
        load(c, static_cast<int>(helix::PathSegment::NOZZLE));
        render(c);
        REQUIRE(c.data->hits.hub_valid);
        REQUIRE(c.data->hits.buffer_valid);
        CHECK(center_y(c.data->hits.hub) > ratio_y(c, kHubYRatio));
        CHECK(c.data->hits.hub.y2 < c.data->hits.buffer.y1);
        const int32_t gap = (c.data->hits.hub.y2 - c.data->hits.hub.y1) / 2;
        const int32_t glyph_top =
            toolhead_top_y(c.data->path_cache.nozzle_y, c.data->theme.extruder_scale);
        CHECK(c.data->hits.buffer.y2 + gap <= glyph_top);
        // The buffer box sits midway between the hub's bottom edge and the glyph.
        const int32_t above = c.data->hits.buffer.y1 - c.data->hits.hub.y2;
        const int32_t below = glyph_top - c.data->hits.buffer.y2;
        CAPTURE(above, below);
        CHECK(std::abs(above - below) <= 1);
    }
    SECTION("without a buffer") {
        auto c = make(false, false);
        load(c, static_cast<int>(helix::PathSegment::NOZZLE));
        render(c);
        REQUIRE(c.data->hits.hub_valid);
        CHECK(center_y(c.data->hits.hub) > ratio_y(c, kHubYRatio));
        const int32_t glyph_top =
            toolhead_top_y(c.data->path_cache.nozzle_y, c.data->theme.extruder_scale);
        CHECK(c.data->hits.hub.y2 < glyph_top);
    }
}

TEST_CASE_METHOD(HubStackFixture,
                 "HUB on the toolhead draws its output run from the hub's bottom edge",
                 "[filament-path][hub-stack]") {
    auto c = make(false, false);
    ui_filament_path_canvas_set_hub_on_toolhead(c.obj, true);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    const auto& route = c.data->path_cache.path;
    // The on-toolhead hub's bottom edge (its output Y) sits one stub above the
    // toolhead sensor row.
    const int32_t stub = std::max<int32_t>(10, static_cast<int32_t>(kCanvasH * 0.03f));
    const int32_t output_y = ratio_y(c, kToolheadYRatio) - stub;
    const int run = segment_starting_at(route, (float)output_y);
    // No output sensor dot sits under an on-toolhead hub: the tube leaves the
    // hub's bottom edge and runs into the glyph's inlet.
    REQUIRE(run >= 0);
    CHECK(route.segs[run].p0.x == Catch::Approx(route.segs[run].p1.x).margin(0.01));
    check_contiguous(route);
    CHECK(seg_end(route.segs[route.count - 1]).y == Catch::Approx(inlet_y(c)).margin(0.01));
}

TEST_CASE_METHOD(HubStackFixture, "A built-in selector keeps every tube and merges them at a point",
                 "[filament-path][hub-stack]") {
    auto c = make(/*buffer=*/false, /*bypass=*/false, /*slots=*/4);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    ui_filament_path_canvas_set_lane_join(c.obj, LaneJoin::Junction);
    render(c);
    CHECK_FALSE(c.data->hits.hub_valid); // no box, so nothing to tap

    LinearHubFrame f{};
    const PathPlan& plan = plan_of(c, &f);
    REQUIRE(plan.active_route == 0);
    const float top = (float)(f.hub_y - f.hub_h / 2);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const auto& route = plan.routes[i].path;
        REQUIRE(route.count > 0);
        CHECK(seg_start(route.segs[0]).y == Catch::Approx((float)f.entry_y).margin(0.01));
        bool meets = false;
        for (int s = 0; s < route.count; ++s)
            meets = meets || (std::fabs(seg_end(route.segs[s]).x - (float)f.center_x) < 0.5f &&
                              std::fabs(seg_end(route.segs[s]).y - top) < 0.5f);
        CHECK(meets);
    }
    check_contiguous(plan.routes[0].path);
    // An outer lane travels sideways on the bus, rather than along a straight diagonal.
    const float bus = (float)built_in_bus_y(f);
    bool horizontal = false;
    for (int s = 0; s < plan.routes[0].path.count; ++s) {
        const auto& seg = plan.routes[0].path.segs[s];
        horizontal =
            horizontal || (seg.type == pg::PathSeg::LINE && std::fabs(seg.p0.y - bus) < 0.5f &&
                           std::fabs(seg.p1.y - bus) < 0.5f);
    }
    CHECK(horizontal);
}

TEST_CASE("A selector rail's carriage follows the active gate and keeps its place otherwise",
          "[filament-path][selector-rail]") {
    const int32_t slots[] = {100, 160, 220, 280};
    CHECK(rail_carriage_x(slots, 4, 2, 0, false) == 220);
    CHECK(rail_carriage_x(slots, 4, 2, 175, true) == 175); // mid-slide
    CHECK(rail_carriage_x(slots, 4, -1, 160, false) == 160);
    CHECK(rail_carriage_x(slots, 4, -2, 160, false) == 160);
    CHECK(rail_carriage_x(slots, 4, -1, 0, false) == 100); // never placed: rests at the first gate
    CHECK(rail_carriage_x(slots, 4, -1, 900, false) == 100);
}

TEST_CASE_METHOD(HubStackFixture, "Rail art replaces the bar, takes each lane to its gate",
                 "[filament-path][selector-rail]") {
    auto c = make(/*buffer=*/false, /*bypass=*/false, /*slots=*/4);
    ui_filament_path_canvas_set_topology(c.obj, 0);
    ui_filament_path_canvas_set_hub_callback(c.obj, [](lv_point_t, void*) {}, nullptr);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    ui_filament_path_canvas_set_active_slot(c.obj, -1);
    ui_filament_path_canvas_set_active_slot(c.obj, 2);
    render(c);
    REQUIRE((c.data->hits.hub_valid && !c.data->rail_drawn));
    const lv_area_t bar = c.data->hits.hub;
    const helix::ui::tray::UnitShell* ercf = helix::ui::tray::unit_shell_by_id("ercf_rail");
    REQUIRE(ercf != nullptr);
    CHECK(helix::ui::tray::unit_shell_face_art(*ercf, 5) == nullptr);
    const helix::ui::tray::RailFaceArt* art = helix::ui::tray::unit_shell_face_art(*ercf, 4);
    ui_filament_path_canvas_set_selector_rail(c.obj, ercf->id, art);
    render(c);
    LinearHubFrame f{};
    const PathPlan& plan = plan_of(c, &f);
    REQUIRE((art != nullptr && c.data->rail_drawn && f.rail.w > 0));
    CHECK(std::abs(f.rail.x + f.rail.w / 2 - f.center_x) <= 1);
    CHECK((f.rail.y > f.prep_y && f.rail.y + f.rail.h < f.buffer_y));
    CHECK((c.data->hits.hub.x1 == f.rail.x && c.data->hits.hub.y2 == f.rail.y + f.rail.h));
    const float in_y = (float)(f.rail.y + (int32_t)std::lround(art->inlet_y * f.rail.h));
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const float gate = (float)rail_gate_x(*art, f.rail, i);
        bool reaches = false;
        for (int s = 0; s < plan.routes[i].path.count; ++s) {
            const pg::PathPoint p = seg_end(plan.routes[i].path.segs[s]);
            reaches = reaches || (std::fabs(p.x - gate) < 0.5f && std::fabs(p.y - in_y) < 0.5f);
        }
        CHECK(reaches);
    }
    // The selected filament leaves under the carriage, standing at gate 2, clear of the art.
    CHECK((f.output_x == rail_outlet_x(*art, f.rail, 2) &&
           f.output_y >= f.rail.y + f.rail.h * 9 / 10));

    BaseGeometry short_canvas = compute_base_geometry(c.obj, c.data);
    short_canvas.height = 120; // too short for the art: the bar comes back
    CHECK(rail_art_rect(*c.data, short_canvas).w == 0);

    // Art whose images are missing gives the whole layout back to the bar.
    ui_filament_path_canvas_set_selector_rail(c.obj, "no_such_unit", art);
    render(c);
    plan_of(c, &f);
    CHECK((!c.data->rail_drawn && f.rail.w == 0));
    ui_filament_path_canvas_set_lane_join(c.obj, LaneJoin::FixedBlock);
    render(c);
    CHECK(
        (c.data->hits.hub_valid && c.data->hits.hub.y1 == bar.y1 && c.data->hits.hub.y2 == bar.y2));
}

TEST_CASE_METHOD(HubStackFixture, "A unit that merges inside keeps every sensor and has no box",
                 "[filament-path][hub-stack]") {
    auto c = make(/*buffer=*/false, /*bypass=*/false, /*slots=*/4);
    for (int i = 0; i < 4; ++i)
        c.data->slot_has_prep_sensor[i] = c.data->slot_has_load_sensor[i] = true;
    ui_filament_path_canvas_set_hub_sensor(c.obj, true, false);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    ui_filament_path_canvas_set_lane_join(c.obj, LaneJoin::Inside);
    ui_filament_path_canvas_set_hub_callback(c.obj, [](lv_point_t, void*) {}, nullptr);
    render(c);
    CHECK(c.data->hits.hub_valid); // no box, but the merge still opens the unit's settings
    // Routes to separate tools never merge, whatever the unit does inside.
    CHECK(drawn_lane_join(LaneJoin::Inside, helix::PathTopology::PARALLEL) == LaneJoin::Hub);
    CHECK(drawn_lane_join(LaneJoin::Junction, helix::PathTopology::MIXED) == LaneJoin::Hub);
    LinearHubFrame f{};
    const PathPlan& plan = plan_of(c, &f);
    CHECK((plan.band_count - plan.trunk_band_count == 8 && plan.trunk_band_count >= 1)); // + hub
    auto junction = make(/*buffer=*/false, /*bypass=*/false, /*slots=*/4);
    ui_filament_path_canvas_set_lane_join(junction.obj, LaneJoin::Junction);
    render(junction);
    LinearHubFrame outside{};
    plan_of(junction, &outside);
    CHECK(f.hub_y < outside.hub_y);
}

TEST_CASE_METHOD(HubStackFixture, "LINEAR never stacks its selector or buffer",
                 "[filament-path][hub-stack]") {
    auto c = make(true, false);
    ui_filament_path_canvas_set_topology(c.obj, 0);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    REQUIRE(c.data->hits.hub_valid);
    REQUIRE(c.data->hits.buffer_valid);
    // The selector butts against the prep sensors; the buffer keeps its ratio.
    const int32_t hub_h = c.data->hits.hub.y2 - c.data->hits.hub.y1;
    CHECK(
        center_y(c.data->hits.hub) ==
        Catch::Approx(ratio_y(c, kPrepYRatio) + c.data->theme.sensor_radius + hub_h / 2).margin(1));
    CHECK(center_y(c.data->hits.buffer) == Catch::Approx(ratio_y(c, kBufferYRatio)).margin(1));
}

TEST_CASE_METHOD(HubStackFixture,
                 "HUB keeps the ratio layout with the bypass shown or a canvas too short to stack",
                 "[filament-path][hub-stack]") {
    auto shown = make(true, true);
    load(shown, static_cast<int>(helix::PathSegment::NOZZLE));
    render(shown);
    REQUIRE((shown.data->hits.hub_valid && shown.data->hits.buffer_valid));
    CHECK(center_y(shown.data->hits.hub) == Catch::Approx(ratio_y(shown, kHubYRatio)).margin(1));
    CHECK(center_y(shown.data->hits.buffer) ==
          Catch::Approx(ratio_y(shown, kBufferYRatio)).margin(1));

    auto c = make(false, false);
    lv_obj_set_size(c.obj, kCanvasW, 100);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    REQUIRE(c.data->hits.hub_valid);

    // Stacked, the hub would sit above its ratio position: the stack only moves it down.
    const int32_t hub_h = (int32_t)(100 * kHubHeightRatio);
    const int32_t gap = hub_h / 2 + 2 * c.data->theme.sensor_radius;
    const int32_t glyph_top =
        toolhead_top_y(ratio_y(c, kNozzleYRatio), c.data->theme.extruder_scale);
    REQUIRE(glyph_top - 2 * gap - hub_h / 2 <= ratio_y(c, kHubYRatio));
    CHECK(center_y(c.data->hits.hub) == Catch::Approx(ratio_y(c, kHubYRatio)).margin(1));
}

// Each expected top restates its renderer's top edge in src/rendering/nozzle_renderer_*.cpp
// at nozzle_y 300, extruder scale 10; a renderer change and its constant change together.
TEST_CASE_METHOD(HubStackFixture, "toolhead_top_y matches each renderer's top edge",
                 "[filament-path][hub-stack]") {
    struct Case {
        helix::ToolheadStyle style;
        int32_t top;
    };
    const Case c = GENERATE(
        // Bambu body: 40 tall, cap 4 over a 4 bevel, iso depth 6 -> 300 - 20 - 8 - 3
        Case{helix::ToolheadStyle::DEFAULT, 269},
        // The baked head's top edge
        Case{helix::ToolheadStyle::A4T, 256},
        // The baked head's top edge
        Case{helix::ToolheadStyle::STEALTHBURNER, 261},
        // The baked head's top edge
        Case{helix::ToolheadStyle::JABBERWOCKY, 251},
        // The baked head's top edge
        Case{helix::ToolheadStyle::ANTHEAD, 259},
        // The baked head's top edge
        Case{helix::ToolheadStyle::CREALITY_K1, 267},
        // The baked head's top edge
        Case{helix::ToolheadStyle::CREALITY_K2, 255});
    ScopedToolheadStyle scoped(c.style);
    CAPTURE(static_cast<int>(c.style));
    REQUIRE(helix::SettingsManager::instance().get_effective_toolhead_style() == c.style);
    CHECK(toolhead_top_y(300, 10) == c.top);
}
