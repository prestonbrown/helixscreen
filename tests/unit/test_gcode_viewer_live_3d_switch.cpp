// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A live 2D -> 3D switch on a file that was loaded in 2D builds its geometry on
// the viewer's build thread, and a budget refusal falls back to 2D for that
// file only: the 3D mode the user picked stays set for the next file.

#include "ui_gcode_viewer.h"

#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "gcode_parser.h"

#include <glm/glm.hpp>
#include <limits>
#include <memory>

#include "../catch_amalgamated.hpp"

#ifdef ENABLE_GLES_3D

using namespace helix::gcode;
using helix::GcodeViewerRenderMode;

namespace {

/// One drawable segment, but a drawable count no memory budget can cover, so
/// GeometryBudgetManager refuses the build on any host.
std::unique_ptr<ParsedGCodeFile> make_over_budget_file() {
    auto file = std::make_unique<ParsedGCodeFile>();
    file->filename = "over_budget.gcode";

    Layer layer;
    layer.z_height = 0.2f;
    ToolpathSegment seg;
    seg.start = glm::vec3(10.0f, 10.0f, 0.2f);
    seg.end = glm::vec3(50.0f, 50.0f, 0.2f);
    seg.is_extrusion = true;
    layer.segments.push_back(seg);
    layer.segment_count_extrusion = 1;
    layer.bounding_box.expand(seg.start);
    layer.bounding_box.expand(seg.end);

    file->layers.push_back(std::move(layer));
    file->total_segments = 1;
    file->drawable_segments = std::numeric_limits<size_t>::max() / 2;
    file->global_bounding_box.expand(seg.start);
    file->global_bounding_box.expand(seg.end);
    return file;
}

/// Two tools on two segments, small enough for any budget.
std::unique_ptr<ParsedGCodeFile> make_two_tool_file() {
    auto file = std::make_unique<ParsedGCodeFile>();
    file->filename = "two_tool.gcode";
    file->tool_color_palette = {"#112233", "#445566"};

    Layer layer;
    layer.z_height = 0.2f;
    for (int tool = 0; tool < 2; ++tool) {
        ToolpathSegment seg;
        const float y = 10.0f + 20.0f * static_cast<float>(tool);
        seg.start = glm::vec3(10.0f, y, 0.2f);
        seg.end = glm::vec3(50.0f, y, 0.2f);
        seg.is_extrusion = true;
        seg.extrusion_amount = 1.0f;
        seg.width = 0.4f;
        seg.tool_index = static_cast<int8_t>(tool);
        layer.segments.push_back(seg);
        layer.bounding_box.expand(seg.start);
        layer.bounding_box.expand(seg.end);
        file->global_bounding_box.expand(seg.start);
        file->global_bounding_box.expand(seg.end);
    }
    layer.segment_count_extrusion = 2;
    file->layers.push_back(std::move(layer));
    file->total_segments = 2;
    file->drawable_segments = 2;
    return file;
}

lv_obj_t* make_2d_viewer_with(lv_obj_t* parent, std::unique_ptr<ParsedGCodeFile> file) {
    lv_obj_t* viewer = helix::ui::gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 400, 300);
    lv_obj_update_layout(viewer);
    helix::ui::gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
    helix::test_access::gcode_viewer_install_loaded_file(viewer, std::move(file));
    REQUIRE(helix::ui::gcode_viewer_is_using_2d_mode(viewer));
    return viewer;
}

constexpr uint32_t kLaneT0 = 0xED1C24u;
constexpr uint32_t kLaneT1 = 0x00A651u;

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a live 3D switch installs the geometry with the AMS tool colors already applied",
                 "[gcode_viewer][gcode][render_mode][colors]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* viewer = make_2d_viewer_with(parent, make_two_tool_file());

    // Applied while the 3D renderer has no mesh to write them into.
    helix::ui::gcode_viewer_set_tool_colors(viewer, {kLaneT0, kLaneT1});

    helix::ui::gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Render3D);
    helix::test_access::gcode_viewer_wait_for_build(viewer);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(helix::ui::gcode_viewer_is_using_2d_mode(viewer));
    const auto palette = helix::test_access::gcode_viewer_3d_palette(viewer);
    REQUIRE_FALSE(palette.empty());
    auto has = [&palette](uint32_t rgb) {
        for (uint32_t c : palette) {
            if ((c & 0xFFFFFFu) == rgb) {
                return true;
            }
        }
        return false;
    };
    CHECK(has(kLaneT0));
    CHECK(has(kLaneT1));
    CHECK_FALSE(has(0x112233u));
    CHECK_FALSE(has(0x445566u));

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a refused live 3D switch falls back to 2D for this file without the LVGL "
                 "thread building",
                 "[gcode_viewer][gcode][render_mode]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* viewer = helix::ui::gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 400, 300);
    lv_obj_update_layout(viewer);

    helix::ui::gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
    helix::test_access::gcode_viewer_install_loaded_file(viewer, make_over_budget_file());
    REQUIRE(helix::ui::gcode_viewer_is_using_2d_mode(viewer));
    const uint32_t children_before = lv_obj_get_child_count(viewer);

    helix::ui::gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Render3D);

    // The build has not been answered on the calling thread: the viewer is in
    // 3D, waiting on geometry, with the loading spinner up meanwhile.
    CHECK_FALSE(helix::ui::gcode_viewer_is_using_2d_mode(viewer));
    CHECK(lv_obj_get_child_count(viewer) == children_before + 1);

    helix::test_access::gcode_viewer_wait_for_build(viewer);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK(lv_obj_get_child_count(viewer) == children_before);

    // Refused: this file draws in 2D, and the user's choice of 3D survives it.
    CHECK(helix::ui::gcode_viewer_is_using_2d_mode(viewer));
    CHECK(helix::test_access::gcode_viewer_render_mode(viewer) == GcodeViewerRenderMode::Render3D);

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture, "a live 3D switch installs the geometry it built",
                 "[gcode_viewer][gcode][render_mode]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* viewer = make_2d_viewer_with(parent, make_two_tool_file());

    helix::ui::gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Render3D);
    helix::test_access::gcode_viewer_wait_for_build(viewer);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(helix::ui::gcode_viewer_is_using_2d_mode(viewer));
    CHECK_FALSE(helix::test_access::gcode_viewer_3d_palette(viewer).empty());

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture, "a live 3D build result for a replaced file is dropped",
                 "[gcode_viewer][gcode][render_mode]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* viewer = make_2d_viewer_with(parent, make_two_tool_file());

    helix::ui::gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Render3D);
    // The result is built and queued before the file it describes goes away.
    helix::test_access::gcode_viewer_wait_for_build(viewer);

    SECTION("by a clear") {
        helix::ui::gcode_viewer_clear(viewer);
    }
    SECTION("by another file") {
        helix::test_access::gcode_viewer_install_loaded_file(viewer, make_two_tool_file());
    }
    helix::ui::UpdateQueue::instance().drain();

    CHECK(helix::test_access::gcode_viewer_3d_palette(viewer).empty());

    lv_obj_delete(parent);
}

#endif // ENABLE_GLES_3D
