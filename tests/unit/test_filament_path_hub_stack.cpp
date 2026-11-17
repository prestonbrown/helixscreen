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
#include <utility>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::fpath;

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

// Whether one of @p path's segments ends within half a pixel of (@p x, @p y).
bool passes(const pg::FilamentPath& path, float x, float y) {
    for (int s = 0; s < path.count; ++s) {
        const pg::PathPoint p = seg_end(path.segs[s]);
        if (std::fabs(p.x - x) < 0.5f && std::fabs(p.y - y) < 0.5f)
            return true;
    }
    return false;
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

// Where a tube meets the toolhead: its filament inlet.
float inlet_y(const HubCanvas& c) {
    return (float)path_toolhead_inlet(c.data->path_cache.nozzle_y, c.data->path_cache.tool_scale).y;
}

// The buffer hangs just under the hub, nearer it than the toolhead.
void check_buffer_under_hub(const HubCanvas& c) {
    const lv_area_t& hub = c.data->hits.hub;
    const lv_area_t& buf = c.data->hits.buffer;
    const int32_t glyph_top =
        toolhead_top_y(c.data->path_cache.nozzle_y, c.data->theme.extruder_scale);
    CHECK((buf.y1 > hub.y2 &&
           buf.y1 - hub.y2 <= 2 * c.data->theme.sensor_radius + (buf.y2 - buf.y1) / 2 + 1));
    CHECK(buf.y1 - hub.y2 < glyph_top - buf.y2);
}

// The plan the canvas painted, rebuilt from its own state.
const PathPlan& plan_of(const HubCanvas& c, LinearHubFrame* frame_out) {
    static PathPlan plan;
    const BaseGeometry g = compute_base_geometry(c.obj, c.data);
    *frame_out = compute_linear_hub_frame(
        *c.data, g, toolhead_top_y(linear_hub_nozzle_y(*c.data, g), c.data->theme.extruder_scale));
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
                 "HUB without a bypass stacks a hub with no buffer onto the toolhead",
                 "[filament-path][hub-stack]") {
    const auto style = GENERATE(helix::ToolheadStyle::DEFAULT, helix::ToolheadStyle::STEALTHBURNER,
                                helix::ToolheadStyle::A4T);
    ScopedToolheadStyle scoped(style);
    CAPTURE(static_cast<int>(style));

    SECTION("with a buffer, which keeps the hub at its place") {
        auto c = make(true, false);
        load(c, static_cast<int>(helix::PathSegment::NOZZLE));
        render(c);
        REQUIRE((c.data->hits.hub_valid && c.data->hits.buffer_valid));
        CHECK(center_y(c.data->hits.hub) == Catch::Approx(ratio_y(c, kHubYRatio)).margin(1));
        check_buffer_under_hub(c);
    }
    SECTION("without a buffer") {
        auto c = make(false, false);
        load(c, static_cast<int>(helix::PathSegment::NOZZLE));
        render(c);
        REQUIRE(c.data->hits.hub_valid);
        CHECK(center_y(c.data->hits.hub) > ratio_y(c, kHubYRatio));
        const int32_t glyph_top =
            toolhead_top_y(c.data->path_cache.nozzle_y, c.data->path_cache.tool_scale);
        CHECK(c.data->hits.hub.y2 < glyph_top);
    }
}

// The OpenAMS unit view at 480x272: four lanes leave the spool box at y 180 into a canvas
// 66 px tall below it. Its sizes are that breakpoint's.
TEST_CASE_METHOD(HubStackFixture,
                 "HUB on a 480x272 canvas keeps room under its hub for the buffer above the head",
                 "[filament-path][hub-stack]") {
    const auto style = GENERATE(helix::ToolheadStyle::DEFAULT, helix::ToolheadStyle::STEALTHBURNER,
                                helix::ToolheadStyle::JABBERWOCKY, helix::ToolheadStyle::A4T);
    ScopedToolheadStyle scoped(style);
    const bool buffer = GENERATE(true, false);
    const bool bypass = GENERATE(false, true);
    CAPTURE(static_cast<int>(style), buffer, bypass);

    FilamentPathData d;
    d.topology = static_cast<int>(helix::PathTopology::HUB);
    d.slot_count = 4;
    d.theme.tube_gauge = 5;
    d.theme.sensor_radius = 4;
    d.theme.hub_width = 50;
    d.theme.extruder_scale = 8;
    for (int i = 0; i < 4; i++)
        d.slot_has_prep_sensor[i] = true;
    d.has_hub_sensor = true;
    d.has_toolhead_sensor = true;
    d.buffer_present = buffer;
    d.show_bypass = bypass;
    BaseGeometry g;
    g.x_off = 330;
    g.y_off = 194;
    g.width = 285;
    g.height = 66;
    g.slot_count = 4;
    for (int i = 0; i < 4; i++)
        g.slot_x[i] = 384 + 58 * i;
    g.center_x = 471;
    g.lane_entry_y = 180;

    const LinearHubFrame f = compute_linear_hub_frame(
        d, g, toolhead_top_y(linear_hub_nozzle_y(d, g), d.theme.extruder_scale));
    const int32_t r = d.theme.sensor_radius;
    const int32_t head_top = toolhead_top_y(f.nozzle_y, f.tool_scale);
    const int32_t inlet = path_toolhead_inlet(f.nozzle_y, f.tool_scale).y;
    const int32_t hub_bottom = f.hub_y + f.hub_h / 2;
    const int32_t buf_h = std::max<int32_t>(16, f.hub_h);
    CAPTURE(f.hub_y, f.hub_h, f.buffer_y, f.bypass_merge_y, f.toolhead_y, f.tool_scale, head_top,
            inlet);
    // The tube ends at the head's filament inlet.
    CHECK(f.inlet_y == inlet);
    // The buffer keeps its box, with a clear gap above and below it; the head keeps a clear gap
    // above it for the toolhead band.
    CHECK(f.has_buffer == buffer);
    CHECK(hub_bottom + (buffer ? r + buf_h : 0) + r + 2 * r <= inlet);
    if (buffer) {
        CHECK(f.buffer_y - buf_h / 2 >= hub_bottom + r);
        CHECK(f.buffer_y + buf_h / 2 + r + 2 * r <= head_top);
    }
    CHECK(f.toolhead_y + r <= head_top);
    CHECK(f.toolhead_y - r >= hub_bottom);
    // The bypass joins the trunk below the hub and buffer, clear of the head.
    if (bypass) {
        CHECK(f.bypass_merge_y <= head_top - 2 * r);
        CHECK(f.bypass_merge_y > (buffer ? f.buffer_y + buf_h / 2 : hub_bottom));
    }
    // The lanes still come down from the spool box into the hub's top.
    static PathPlan plan;
    plan_linear_hub(f, d, g, plan);
    for (int i = 0; i < 4; i++) {
        CAPTURE(i);
        const pg::FilamentPath& lane = plan.routes[i].path;
        for (int k = 1; k < lane.count; ++k)
            CHECK(std::hypot(seg_start(lane.segs[k]).x - seg_end(lane.segs[k - 1]).x,
                             seg_start(lane.segs[k]).y - seg_end(lane.segs[k - 1]).y) < 0.01);
        const pg::PathPoint end = seg_end(lane.segs[lane.count - 1]);
        CHECK(end.y == Catch::Approx(f.hub_y - f.hub_h / 2));
        CHECK(end.y >= (float)g.lane_entry_y);
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
    ui_filament_path_canvas_set_selector_rail(c.obj, ercf->id, art, true);
    render(c);
    LinearHubFrame f{};
    const PathPlan& plan = plan_of(c, &f);
    REQUIRE((art != nullptr && c.data->rail_drawn && f.rail.w > 0));
    CHECK(std::abs(f.rail.x + f.rail.w / 2 - f.center_x) <= 1);
    CHECK((f.rail.y > f.prep_y && f.rail.y + f.rail.h < ratio_y(c, kBufferYRatio)));
    CHECK((c.data->hits.hub.x1 == f.rail.x && c.data->hits.hub.y2 == f.rail.y + f.rail.h));
    const float in_y = (float)(f.rail.y + (int32_t)std::lround(art->inlet_y * f.rail.h));
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(passes(plan.routes[i].path, (float)rail_gate_x(*art, f.rail, i), in_y));
    }
    // The selected filament leaves under the carriage, standing at gate 2, clear of the art.
    CHECK((f.output_x == rail_outlet_x(*art, f.rail, 2) &&
           f.output_y >= f.rail.y + f.rail.h * 9 / 10));
    // A rail's sync sensor is at its outlet: no buffer box, the outlet band shows its state.
    ui_filament_path_canvas_set_buffer_info(c.obj, true, 0);
    ui_filament_path_canvas_set_buffer_fault_state(c.obj, 2);
    render(c);
    const PathPlan& synced = plan_of(c, &f);
    const SensorBand* sync = nullptr;
    for (int b = 0; b < synced.band_count; ++b)
        if (synced.bands[b].state == BandState::Sync && synced.bands[b].at.y == (float)f.output_y)
            sync = &synced.bands[b];
    REQUIRE(sync != nullptr);
    CHECK((!f.has_buffer && center_y(c.data->hits.buffer) == f.output_y &&
           lv_color_eq(band_color(*sync, tube_palette(*c.data)), c.data->theme.color_buffer[2])));
    // A new selection slides the exit toward that gate's outlet, before any repaint.
    ui_filament_path_canvas_set_active_slot(c.obj, 3);
    CHECK(c.data->anim.output_x_target == rail_outlet_x(*art, f.rail, 3));

    BaseGeometry short_canvas = compute_base_geometry(c.obj, c.data);
    short_canvas.height = 120; // too short for the art: the bar comes back
    CHECK(rail_art_rect(*c.data, short_canvas).w == 0);

    // Art whose images are missing gives the whole layout back to the bar.
    ui_filament_path_canvas_set_selector_rail(c.obj, "no_such_unit", art, true);
    render(c);
    plan_of(c, &f);
    CHECK((!c.data->rail_drawn && f.rail.w == 0 && f.has_buffer));
    CHECK((c.data->hits.hub.y1 == bar.y1 && c.data->hits.hub.y2 == bar.y2));
}

TEST_CASE_METHOD(HubStackFixture,
                 "A pass-through unit's art stands above the hub or the tools, a lane per outlet",
                 "[filament-path][selector-rail]") {
    auto c = make(/*buffer=*/false, /*bypass=*/false, /*slots=*/4);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    const helix::ui::tray::UnitShell* htlf = helix::ui::tray::unit_shell_by_id("htlf");
    REQUIRE((htlf != nullptr && htlf->carriage_n == 0));
    const helix::ui::tray::RailFaceArt* art = helix::ui::tray::unit_shell_face_art(*htlf, 4);
    REQUIRE(art != nullptr);
    ui_filament_path_canvas_set_selector_rail(c.obj, htlf->id, art, false);
    render(c);

    // HUB: lanes bend in to their inlets, leave through their own outlets and fan into the hub.
    LinearHubFrame f{};
    const PathPlan& plan = plan_of(c, &f);
    REQUIRE(f.through.w > 0);
    CHECK((f.rail.w == 0 && !c.data->rail_drawn));
    CHECK((f.through.y > f.prep_band_y && f.hub_y - f.hub_h / 2 > f.through.y + f.through.h));
    const float in_y = (float)(f.through.y + (int32_t)std::lround(art->inlet_y * f.through.h));
    const float out_y = (float)(f.through.y + (int32_t)std::lround(art->outlet_y * f.through.h));
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const float out_x = (float)rail_outlet_x(*art, f.through, i);
        CHECK((f.hub_fan[i].pts[0].x == out_x && f.hub_fan[i].pts[0].y == out_y));
        CHECK(passes(plan.routes[i].path, (float)rail_gate_x(*art, f.through, i), in_y));
    }

    // MIXED: the lane sensors move below the art, and each lane returns to its own column.
    ui_filament_path_canvas_set_topology(c.obj, static_cast<int>(helix::PathTopology::MIXED));
    ui_filament_path_canvas_set_slot_hub_routed(c.obj, 2, true);
    ui_filament_path_canvas_set_slot_hub_routed(c.obj, 3, true);
    const BaseGeometry g = compute_base_geometry(c.obj, c.data);
    const MixedFrame m = compute_mixed_frame(*c.data, g);
    REQUIRE(m.through.w > 0);
    CHECK((m.sensor_y > m.through.y + m.through.h && m.hub_cy > m.sensor_y));
    PathPlan mixed;
    plan_mixed(m, *c.data, g, mixed);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(passes(mixed.routes[i].path, (float)g.slot_x[i], (float)m.sensor_y));
    }

    // A carriage rides LINEAR's bar; a unit without one feeds a hub from its outlets.
    using helix::PathTopology;
    for (bool carriage : {false, true})
        CHECK((unit_art_drawn(PathTopology::LINEAR, carriage) == carriage &&
               unit_art_drawn(PathTopology::HUB, carriage) != carriage &&
               unit_art_drawn(PathTopology::MIXED, carriage) != carriage &&
               !unit_art_drawn(PathTopology::PARALLEL, carriage)));
    // Beside a buffer and the bypass the face takes the selector's slot, its outlets fan into a
    // hub below it, and the hub's outlet band carries the buffer's reading.
    ui_filament_path_canvas_set_topology(c.obj, static_cast<int>(PathTopology::HUB));
    ui_filament_path_canvas_set_buffer_info(c.obj, true, 0);
    ui_filament_path_canvas_set_show_bypass(c.obj, true);
    render(c);
    plan_of(c, &f);
    REQUIRE((f.through.w > 0 && !c.data->rail_drawn));
    CHECK((f.sync_on_output && f.through.y + f.through.h < f.hub_y - f.hub_h / 2 &&
           f.output_y < f.bypass_merge_y));
    CHECK(f.hub_fan[0].pts[0].x == (float)rail_outlet_x(*art, f.through, 0));
    // No room for the face: the hub alone.
    lv_obj_set_height(c.obj, 160);
    render(c);
    plan_of(c, &f);
    CHECK(f.through.w == 0);
}

TEST_CASE_METHOD(HubStackFixture,
                 "A unit's hub box reads presence only when the backend reports it",
                 "[filament-path][hub-stack]") {
    auto c = make(/*buffer=*/false, /*bypass=*/false, /*slots=*/4);
    render(c);
    LinearHubFrame f{};
    const int plain_bands = plan_of(c, &f).trunk_band_count;
    ui_filament_path_canvas_set_hub_sensor(c.obj, true, false);
    render(c);
    CHECK(plan_of(c, &f).trunk_band_count == plain_bands + 1);
}

TEST_CASE("A unit's combiner decides how its lanes reach one extruder", "[filament-path]") {
    using helix::PathTopology;
    using helix::ui::tray::Combiner;
    for (PathTopology one : {PathTopology::LINEAR, PathTopology::HUB}) {
        // Inside the unit or below it, the lanes meet in a hub box.
        CHECK((combined_topology(Combiner::Internal, false, one) == PathTopology::HUB &&
               combined_topology(Combiner::External, false, one) == PathTopology::HUB));
        // A sliding selector keeps its rail; one lane has nothing to merge.
        CHECK((combined_topology(Combiner::Internal, true, one) == one &&
               combined_topology(Combiner::Absent, false, one) == one));
    }
    // Lanes bound for separate tools keep their own drawing.
    for (PathTopology split : {PathTopology::PARALLEL, PathTopology::MIXED})
        CHECK((combined_topology(Combiner::External, false, split) == split &&
               combined_topology(Combiner::Internal, false, split) == split));
}

TEST_CASE_METHOD(HubStackFixture, "LINEAR never stacks its selector or buffer",
                 "[filament-path][hub-stack]") {
    auto c = make(true, false);
    ui_filament_path_canvas_set_topology(c.obj, 0);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    REQUIRE(c.data->hits.hub_valid);
    REQUIRE(c.data->hits.buffer_valid);
    // The selector butts against the prep sensors; the buffer hangs under it.
    const int32_t hub_h = c.data->hits.hub.y2 - c.data->hits.hub.y1;
    CHECK(
        center_y(c.data->hits.hub) ==
        Catch::Approx(ratio_y(c, kPrepYRatio) + c.data->theme.sensor_radius + hub_h / 2).margin(1));
    check_buffer_under_hub(c);
}

TEST_CASE_METHOD(HubStackFixture,
                 "HUB keeps the ratio layout with the bypass shown or a canvas too short to stack",
                 "[filament-path][hub-stack]") {
    auto shown = make(true, true);
    load(shown, static_cast<int>(helix::PathSegment::NOZZLE));
    render(shown);
    REQUIRE((shown.data->hits.hub_valid && shown.data->hits.buffer_valid));
    CHECK(center_y(shown.data->hits.hub) == Catch::Approx(ratio_y(shown, kHubYRatio)).margin(1));
    check_buffer_under_hub(shown);

    auto c = make(false, false);
    lv_obj_set_size(c.obj, kCanvasW, 100);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    REQUIRE(c.data->hits.hub_valid);

    // Stacked, the hub would sit above its ratio position: the stack only moves it down.
    const int32_t hub_h = (int32_t)(100 * kHubHeightRatio);
    const int32_t gap = hub_h / 2 + 2 * c.data->theme.sensor_radius;
    const int32_t glyph_top =
        toolhead_top_y(linear_hub_nozzle_y(*c.data, compute_base_geometry(c.obj, c.data)),
                       c.data->theme.extruder_scale);
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

// The AMS panel's path canvas height (ctl geom) and the theme's extruder scale
// there, LV_MAX(8, space_md), at 480x272 and 800x480.
TEST_CASE_METHOD(HubStackFixture,
                 "Every toolhead's glyph stays on the path canvas at 480x272 and 800x480",
                 "[filament-path][hub-stack][toolhead-fit]") {
    const auto bp =
        GENERATE(std::pair<int32_t, int32_t>{119, 8}, std::pair<int32_t, int32_t>{262, 10});
    const int style_id = GENERATE(range(1, helix::kLastToolheadStyle + 1));
    ScopedToolheadStyle scoped(static_cast<helix::ToolheadStyle>(style_id));
    REQUIRE(static_cast<int>(helix::SettingsManager::instance().get_effective_toolhead_style()) ==
            style_id);
    FilamentPathData d;
    d.theme.extruder_scale = bp.second;
    BaseGeometry g;
    g.height = bp.first;
    const helix::ui::GlyphBounds b = toolhead_glyph_bounds(0, linear_hub_nozzle_y(d, g), bp.second);
    CAPTURE(style_id, bp.first, b.top, b.bottom);
    CHECK(b.top >= 0);
    CHECK(b.bottom < bp.first);
}
