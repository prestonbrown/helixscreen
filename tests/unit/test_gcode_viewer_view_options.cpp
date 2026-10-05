// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// What the viewer was told to show (highlighted and excluded objects, per-tool
// AMS colors) is a property of the viewer, not of whichever renderer exists when
// it is set. A renderer created later, by a load or a render-mode switch, starts
// with all of it, and repeating an unchanged value never costs the 3D renderer a
// VBO re-upload (the Pi 3B's GPU sustains ~4M triangles/s).

#include "ui_gcode_viewer.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/gcode_layer_renderer_test_access.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef ENABLE_GLES_3D
#include "../test_helpers/gcode_gles_renderer_test_access.h"
#endif

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;
using helix::GcodeViewerRenderMode;
namespace test_access = helix::test_access;

namespace {

constexpr uint32_t kLaneT0 = 0xED1C24u;
constexpr uint32_t kLaneT1 = 0x00A651u;
constexpr uint32_t kOtherT0 = 0x123456u;
constexpr uint32_t kOtherT1 = 0xFEDCBAu;
constexpr uint32_t kFallback = 0x808080u;

/// Two named objects, one per tool, on one layer.
std::unique_ptr<ParsedGCodeFile> make_two_object_file() {
    auto file = std::make_unique<ParsedGCodeFile>();
    file->filename = "two_objects.gcode";
    file->tool_color_palette = {"#112233", "#445566"};

    Layer layer;
    layer.z_height = 0.2f;
    const char* names[] = {"cube1", "cube2"};
    for (int tool = 0; tool < 2; ++tool) {
        ToolpathSegment seg;
        const float y = 10.0f + 20.0f * static_cast<float>(tool);
        seg.start = glm::vec3(10.0f, y, 0.2f);
        seg.end = glm::vec3(50.0f, y, 0.2f);
        seg.is_extrusion = true;
        seg.extrusion_amount = 1.0f;
        seg.width = 0.4f;
        seg.tool_index = static_cast<int8_t>(tool);
        seg.object_name_index = file->intern_object_name(names[tool]);
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

uint32_t rgb_of(lv_color_t c) {
    return (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
           static_cast<uint32_t>(c.blue);
}

uint32_t resolved(const GCodeLayerRenderer& renderer, int tool) {
    return rgb_of(GCodeLayerRendererTestAccess::tool_palette(renderer).resolve(
        tool, lv_color_hex(kFallback)));
}

const std::unordered_set<std::string> kHighlight{"cube1"};
const std::unordered_set<std::string> kExcluded{"cube2"};

lv_obj_t* make_viewer(lv_obj_t* parent) {
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 400, 300);
    lv_obj_update_layout(viewer);
    return viewer;
}

void set_options(lv_obj_t* viewer) {
    ui_gcode_viewer_set_highlighted_objects(viewer, kHighlight);
    ui_gcode_viewer_set_excluded_objects(viewer, kExcluded);
    ui_gcode_viewer_set_tool_colors(viewer, {kLaneT0, kLaneT1});
}

void check_2d(lv_obj_t* viewer, bool expect_lane_colors) {
    const GCodeLayerRenderer* r2d = test_access::gcode_viewer_2d_renderer(viewer);
    REQUIRE(r2d != nullptr);
    const SelectionState& sel = GCodeLayerRendererTestAccess::selection(*r2d);
    CHECK(sel.highlighted() == kHighlight);
    CHECK(sel.excluded() == kExcluded);
    if (expect_lane_colors) {
        CHECK(resolved(*r2d, 0) == kLaneT0);
        CHECK(resolved(*r2d, 1) == kLaneT1);
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a 2D renderer created after the options were set starts with them",
                 "[gcode_viewer][gcode][view_options]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* viewer = make_viewer(parent);
    set_options(viewer);

    test_access::gcode_viewer_install_loaded_file(viewer, make_two_object_file());
    ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
    check_2d(viewer, /*expect_lane_colors=*/true);

    SECTION("a cleared and reloaded viewer keeps the selection sets") {
        ui_gcode_viewer_clear(viewer);
        test_access::gcode_viewer_install_loaded_file(viewer, make_two_object_file());
        ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
        // Tool colors are per load and go with clear(); the sets belong to the
        // print, which outlives a reload of the same viewer.
        check_2d(viewer, /*expect_lane_colors=*/false);
    }

    lv_obj_delete(parent);
}

#ifdef ENABLE_GLES_3D

TEST_CASE_METHOD(LVGLTestFixture, "switching 2D, 3D, 2D leaves every renderer with the options",
                 "[gcode_viewer][gcode][view_options]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* viewer = make_viewer(parent);
    set_options(viewer);

    test_access::gcode_viewer_install_loaded_file(viewer, make_two_object_file());
    ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
    check_2d(viewer, true);

    ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Render3D);
    test_access::gcode_viewer_wait_for_build(viewer);
    helix::ui::UpdateQueue::instance().drain();

    GCodeGLESRenderer* r3d = test_access::gcode_viewer_3d_renderer(viewer);
    REQUIRE(r3d != nullptr);
    REQUIRE(r3d->has_geometry());
    const SelectionState& sel3d = GCodeGLESRendererTestAccess::selection(*r3d);
    CHECK(sel3d.highlighted() == kHighlight);
    CHECK(sel3d.excluded() == kExcluded);
    const auto palette = test_access::gcode_viewer_3d_palette(viewer);
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

    ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
    check_2d(viewer, true);

    lv_obj_delete(parent);
}

namespace {

/// A viewer whose 3D renderer holds geometry that a (GL-less) test treats as
/// uploaded.
struct UploadedViewer {
    lv_obj_t* parent;
    lv_obj_t* viewer;
    GCodeGLESRenderer* r3d;

    UploadedViewer() {
        parent = lv_obj_create(lv_screen_active());
        viewer = make_viewer(parent);
        test_access::gcode_viewer_install_loaded_file(viewer, make_two_object_file());
        ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
        ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Render3D);
        test_access::gcode_viewer_wait_for_build(viewer);
        helix::ui::UpdateQueue::instance().drain();
        r3d = test_access::gcode_viewer_3d_renderer(viewer);
        REQUIRE(r3d != nullptr);
        REQUIRE(r3d->has_geometry());
        GCodeGLESRendererTestAccess::mark_uploaded(*r3d);
    }
    ~UploadedViewer() {
        lv_obj_delete(parent);
    }

    bool uploaded() const {
        return GCodeGLESRendererTestAccess::is_uploaded(*r3d);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "repeating an unchanged option does not re-upload the 3D geometry",
                 "[gcode_viewer][gcode][view_options][gles]") {
    UploadedViewer v;

    // A changed color is what forces the repaint; the setup has to reach it or
    // the unchanged cases below prove nothing.
    ui_gcode_viewer_set_tool_colors(v.viewer, {kLaneT0, kLaneT1});
    REQUIRE_FALSE(v.uploaded());
    GCodeGLESRendererTestAccess::mark_uploaded(*v.r3d);

    ui_gcode_viewer_set_tool_colors(v.viewer, {kLaneT0, kLaneT1});
    ui_gcode_viewer_set_excluded_objects(v.viewer, kExcluded);
    ui_gcode_viewer_set_excluded_objects(v.viewer, kExcluded);
    ui_gcode_viewer_set_highlighted_objects(v.viewer, kHighlight);
    ui_gcode_viewer_set_highlighted_objects(v.viewer, kHighlight);
    CHECK(v.uploaded());

    // A render-mode round trip applies the options again.
    ui_gcode_viewer_set_render_mode(v.viewer, GcodeViewerRenderMode::Layer2D);
    ui_gcode_viewer_set_render_mode(v.viewer, GcodeViewerRenderMode::Render3D);
    CHECK(v.uploaded());

    SECTION("a different color list does") {
        ui_gcode_viewer_set_tool_colors(v.viewer, {kOtherT0, kOtherT1});
        CHECK_FALSE(v.uploaded());
    }
}

#endif // ENABLE_GLES_3D
