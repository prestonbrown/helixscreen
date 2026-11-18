// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_path_view_scale.cpp
 * @brief One view scale sizes the AMS unit view's path: the tube within its bounds, the head
 *        at least three tubes wide and never shrunk to make room, the capsule never taller
 *        than the hub, and the buffer riding in the hub where its capsule would not read.
 */

#include "ui_filament_path_canvas.h"
#include "ui_toolhead_view.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "path_view_scale.h"
#include "src/ui/filament/ui_filament_path_internal.h"
#include "src/ui/filament/ui_filament_path_plan.h"

#include "../catch_amalgamated.hpp"

using helix::ui::path_view_scale;
using helix::ui::PathViewScale;
using helix::ui::TubeLimits;
using namespace helix::ui::fpath;

namespace {

int32_t real_head_width(int32_t scale) {
    const helix::ui::GlyphBounds b = helix::ui::toolhead_glyph_bounds(0, 0, scale);
    return b.right - b.left;
}

// Every column height from a 272-line screen's to past a 1280x720 one's.
constexpr int32_t kShortestColumn = 150;
constexpr int32_t kTallestColumn = 700;

} // namespace

TEST_CASE("View scale keeps the tube between its floor and ceiling", "[filament-path]") {
    const TubeLimits limits{4, 8};
    for (int32_t h = kShortestColumn; h <= kTallestColumn; ++h) {
        CAPTURE(h);
        const PathViewScale s = path_view_scale(h, limits);
        CHECK(s.tube_gauge >= limits.floor);
        CHECK(s.tube_gauge <= limits.ceil);
    }
    // The bounds bind at both ends, and the gauge follows the scale between them.
    CHECK(path_view_scale(120, limits).tube_gauge == 4);
    CHECK(path_view_scale(1200, limits).tube_gauge == 8);
    CHECK(path_view_scale(400, limits).tube_gauge > path_view_scale(250, limits).tube_gauge);
}

TEST_CASE("View scale never draws a head narrower than three tubes", "[filament-path]") {
    for (const TubeLimits limits : {TubeLimits{3, 6}, TubeLimits{4, 8}, TubeLimits{5, 10}}) {
        for (int32_t h = kShortestColumn; h <= kTallestColumn; ++h) {
            CAPTURE(limits.floor, limits.ceil, h);
            const PathViewScale s = path_view_scale(h, limits, real_head_width);
            CHECK((float)real_head_width(s.head_scale) >=
                  helix::ui::kHeadMinTubes * (float)s.tube_gauge);
        }
    }
    // A thick floor on a short column raises the head past its share of the scale.
    const PathViewScale thick = path_view_scale(kShortestColumn, {8, 8}, real_head_width);
    CHECK(thick.head_scale > (int32_t)thick.unit + 1);
}

TEST_CASE("View scale keeps the capsule no taller than the hub", "[filament-path][buffer]") {
    for (int32_t label_h : {0, 15, 30}) {
        for (int32_t h = kShortestColumn; h <= kTallestColumn; ++h) {
            CAPTURE(label_h, h);
            const PathViewScale s = path_view_scale(h, {4, 8}, nullptr, label_h);
            CHECK(s.cap_h <= s.hub_h);
            CHECK(s.hub_h >= label_h + 2);
        }
    }
}

TEST_CASE("View scale always carries the buffer in the hub", "[filament-path][buffer]") {
    for (int32_t h = kShortestColumn; h <= kTallestColumn; h += 37) {
        CAPTURE(h);
        CHECK(path_view_scale(h, {4, 8}).buffer_in_hub);
    }
}

namespace {

class ViewScaleFixture : public LVGLTestFixture {
  public:
    lv_obj_t* make_hub(int32_t height, int slots, bool buffer) {
        lv_obj_t* obj = ui_filament_path_canvas_create(test_screen());
        REQUIRE(obj != nullptr);
        lv_obj_set_size(obj, 300, height);
        ui_filament_path_canvas_set_topology(obj, 1);
        ui_filament_path_canvas_set_slot_count(obj, slots);
        ui_filament_path_canvas_set_slot_width(obj, 60);
        ui_filament_path_canvas_set_slot_overlap(obj, 0);
        ui_filament_path_canvas_set_buffer_info(obj, buffer, 2);
        return obj;
    }

    LinearHubFrame frame_of(lv_obj_t* obj) {
        lv_obj_update_layout(test_screen());
        FilamentPathData* data = get_data(obj);
        const BaseGeometry g = compute_base_geometry(obj, data);
        return compute_linear_hub_frame(
            *data, g, toolhead_top_y(linear_hub_nozzle_y(*data, g), data->theme.extruder_scale));
    }
};

} // namespace

TEST_CASE_METHOD(ViewScaleFixture, "View scale draws one head on canvases of every height",
                 "[filament-path][hub-stack]") {
    const PathViewScale s = path_view_scale(417, {4, 8}, real_head_width, 22);
    // A canvas too short for the hub and its buffer above a full head still draws the scale's
    // head, whatever the unit's lane count: the unit above gives up the room.
    for (const auto& [height, slots] : {std::pair{90, 4}, std::pair{140, 2}, std::pair{400, 8}}) {
        CAPTURE(height, slots);
        lv_obj_t* obj = make_hub(height, slots, true);
        ui_filament_path_canvas_set_view_scale(obj, s);
        const LinearHubFrame f = frame_of(obj);
        CHECK(f.tool_scale == s.head_scale);
        CHECK(f.hub_h == s.hub_h);
        CHECK(f.cap_h == s.cap_h);
        lv_obj_delete(obj);
    }
}

TEST_CASE_METHOD(ViewScaleFixture, "A buffer in the hub takes no room on the trunk",
                 "[filament-path][buffer]") {
    const PathViewScale s = path_view_scale(240, {4, 8}, real_head_width, 15);
    REQUIRE(s.buffer_in_hub);
    lv_obj_t* obj = make_hub(160, 4, true);
    ui_filament_path_canvas_set_hub_callback(obj, [](lv_point_t, void*) {}, nullptr);
    ui_filament_path_canvas_set_buffer_callback(obj, [](void*) {}, nullptr);
    ui_filament_path_canvas_set_view_scale(obj, s);
    const LinearHubFrame f = frame_of(obj);
    CHECK(f.buffer_in_hub);
    CHECK_FALSE(f.has_buffer);
    CHECK_FALSE(f.sync_on_output);

    // The buffer's tap target is the glyph's slot inside the hub box.
    ui_filament_path_canvas_refresh(obj);
    process_lvgl(100);
    FilamentPathData* data = get_data(obj);
    REQUIRE(data->hits.buffer_valid);
    REQUIRE(data->hits.hub_valid);
    CHECK(data->hits.buffer.x1 > data->hits.hub.x1);
    CHECK(data->hits.buffer.x2 < data->hits.hub.x2);
    CHECK(data->hits.buffer.y1 == data->hits.hub.y1);
    CHECK(data->hits.buffer.y2 == data->hits.hub.y2);
}
