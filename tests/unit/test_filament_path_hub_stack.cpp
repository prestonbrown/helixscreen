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
#include "src/ui/ui_filament_path_internal.h"
#include "src/ui/ui_filament_path_plan.h"

#include <algorithm>
#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::fpath;

namespace {

constexpr int32_t CANVAS_W = 420;
constexpr int32_t CANVAS_H = 420;

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
        lv_obj_set_size(c.obj, CANVAS_W, CANVAS_H);
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
    const int32_t nozzle_y = g.y_off + (int32_t)(g.height * NOZZLE_Y_RATIO);
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

TEST_CASE_METHOD(HubStackFixture, "HUB loaded to the nozzle with a buffer is one contiguous route",
                 "[filament-path][hub-stack]") {
    auto c = make(/*buffer=*/true, /*bypass=*/false);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    const auto& route = c.data->path_cache.path;
    check_contiguous(route);
    // The route passes the hub's bottom edge and ends in the glyph's inlet.
    REQUIRE(c.data->hits.hub_valid);
    CHECK(segment_starting_at(route, (float)c.data->hits.hub.y2) >= 0);
    CHECK(seg_end(route.segs[route.count - 1]).y == Catch::Approx(inlet_y(c)).margin(0.01));
}

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
        CHECK(center_y(c.data->hits.hub) > ratio_y(c, HUB_Y_RATIO));
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
        CHECK(center_y(c.data->hits.hub) > ratio_y(c, HUB_Y_RATIO));
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
    const int32_t stub = std::max<int32_t>(10, static_cast<int32_t>(CANVAS_H * 0.03f));
    const int32_t output_y = ratio_y(c, TOOLHEAD_Y_RATIO) - stub;
    const int run = segment_starting_at(route, (float)output_y);
    // No output sensor dot sits under an on-toolhead hub: the tube leaves the
    // hub's bottom edge and runs into the glyph's inlet.
    REQUIRE(run >= 0);
    CHECK(route.segs[run].p0.x == Catch::Approx(route.segs[run].p1.x).margin(0.01));
    check_contiguous(route);
    CHECK(seg_end(route.segs[route.count - 1]).y == Catch::Approx(inlet_y(c)).margin(0.01));
}

TEST_CASE_METHOD(HubStackFixture, "HUB with the bypass shown keeps the ratio layout",
                 "[filament-path][hub-stack]") {
    auto c = make(true, true);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    REQUIRE(c.data->hits.hub_valid);
    REQUIRE(c.data->hits.buffer_valid);
    CHECK(center_y(c.data->hits.hub) == Catch::Approx(ratio_y(c, HUB_Y_RATIO)).margin(1));
    CHECK(center_y(c.data->hits.buffer) == Catch::Approx(ratio_y(c, BUFFER_Y_RATIO)).margin(1));
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
    CHECK(center_y(c.data->hits.hub) ==
          Catch::Approx(ratio_y(c, PREP_Y_RATIO) + c.data->theme.sensor_radius + hub_h / 2)
              .margin(1));
    CHECK(center_y(c.data->hits.buffer) == Catch::Approx(ratio_y(c, BUFFER_Y_RATIO)).margin(1));
}

TEST_CASE_METHOD(HubStackFixture, "HUB on a canvas too short to stack keeps the ratio layout",
                 "[filament-path][hub-stack]") {
    auto c = make(false, false);
    lv_obj_set_size(c.obj, CANVAS_W, 100);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    REQUIRE(c.data->hits.hub_valid);

    // Stacked, the hub would sit above its ratio position: the stack only moves it down.
    const int32_t hub_h = (int32_t)(100 * HUB_HEIGHT_RATIO);
    const int32_t gap = hub_h / 2 + 2 * c.data->theme.sensor_radius;
    const int32_t glyph_top =
        toolhead_top_y(ratio_y(c, NOZZLE_Y_RATIO), c.data->theme.extruder_scale);
    REQUIRE(glyph_top - 2 * gap - hub_h / 2 <= ratio_y(c, HUB_Y_RATIO));
    CHECK(center_y(c.data->hits.hub) == Catch::Approx(ratio_y(c, HUB_Y_RATIO)).margin(1));
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
        // Design y 0 vs center 630, render 12*10 = 120 over 2000 -> -37.8, truncated
        Case{helix::ToolheadStyle::A4T, 263},
        // Design y 78 vs center 500, render 100 over 1000 -> -42.2, truncated
        Case{helix::ToolheadStyle::STEALTHBURNER, 258},
        // Design y 2 vs center 687, render 100 over 2400 -> -28.5, truncated
        Case{helix::ToolheadStyle::JABBERWOCKY, 272},
        // Image row 0 vs pivot 81 of 163, scaled to 65 px -> -32.3, truncated
        Case{helix::ToolheadStyle::ANTHEAD, 268},
        // Body 48 tall, iso depth 6 -> 300 - 24 - 3
        Case{helix::ToolheadStyle::CREALITY_K1, 273},
        // Body 48 tall, iso depth 5 -> 300 - 24 - 2
        Case{helix::ToolheadStyle::CREALITY_K2, 274});
    ScopedToolheadStyle scoped(c.style);
    CAPTURE(static_cast<int>(c.style));
    REQUIRE(helix::SettingsManager::instance().get_effective_toolhead_style() == c.style);
    CHECK(toolhead_top_y(300, 10) == c.top);
}
