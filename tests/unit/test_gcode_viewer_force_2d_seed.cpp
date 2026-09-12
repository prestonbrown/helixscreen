// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// What the budget-forced 2D fallback has to put into the renderer it creates.
//
// A GCodeLayerRenderer arrives with SSAO and antialiasing ON, STANDARD framing
// and no colours the viewer knows about, so everything the viewer decided has to
// be pushed in at creation. Nothing downstream repairs an omission: the load
// callback that runs after the fallback re-applies a single extrusion colour and
// nothing else, and the shading tier is never revisited for the life of the
// renderer. The device that lands here is the one whose file just exceeded its
// 3D memory budget, and antialiased rasterization is ~6x the aliased cost — the
// exact expense the constrained tier exists to refuse
// (prestonbrown/helixscreen#1555).
//
// The fallback fires when GeometryBudgetManager refuses a build, which a test on
// a machine with free memory cannot arrange, so these enter the branch body
// directly through helix::test_access.

#include "ui_gcode_viewer.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/gcode_layer_renderer_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "gcode_projection.h"

#include <cstdint>
#include <cstdlib>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ScopedEnv;
using helix::test_access::gcode_viewer_budget_force_2d;
using namespace helix::gcode;

namespace {

/// The slicer's answer and the AMS lanes that override it. Deliberately
/// unrelated values, so neither can pass for the other in a result.
constexpr uint32_t kSlicerT0 = 0x112233u;
constexpr uint32_t kSlicerT1 = 0x445566u;
constexpr uint32_t kLaneT0 = 0xED1C24u;
constexpr uint32_t kLaneT1 = 0x00A651u;

/// Neither palette, so "resolved through nothing" is a visible outcome rather
/// than something that could read as an answer.
constexpr uint32_t kFallbackColor = 0x808080u;

/// A two-tool file with one drawable segment — enough for auto_fit() to have
/// real bounds and for the colour chain to have a palette to install.
std::unique_ptr<ParsedGCodeFile> make_two_tool_file() {
    auto file = std::make_unique<ParsedGCodeFile>();
    file->filename = "budget_fallback.gcode";
    file->tool_color_palette = {"#112233", "#445566"};

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
    file->drawable_segments = 1;
    file->global_bounding_box.expand(seg.start);
    file->global_bounding_box.expand(seg.end);
    return file;
}

uint32_t rgb_of(lv_color_t c) {
    return (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
           static_cast<uint32_t>(c.blue);
}

uint32_t resolved(const GCodeLayerRenderer& renderer, int tool) {
    return rgb_of(GCodeLayerRendererTestAccess::tool_palette(renderer).resolve(
        tool, lv_color_hex(kFallbackColor)));
}

/// A viewer with a real canvas: the fallback reads the widget's coords for the
/// renderer's canvas size, and auto_fit() divides by them.
lv_obj_t* make_viewer(lv_obj_t* parent) {
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 400, 300);
    lv_obj_update_layout(viewer);
    return viewer;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the budget 2D fallback honours the viewer's shading tier",
                 "[gcode_viewer][gcode][1555]") {
    // HELIX_SSAO=0 is the only way a test can put the viewer on a reduced tier;
    // is_constrained_device() reads real RAM. It drives the same two fields the
    // constrained tier sets, and both land opposite the renderer's own defaults,
    // so a creation path that pushes neither reads back as "on".
    ScopedEnv ssao_guard("HELIX_SSAO");
    setenv("HELIX_SSAO", "0", 1);

    // Both flags start ON, so "off" is the only answer a pushed decision can
    // produce and CHECK_FALSE below cannot pass by coincidence.
    {
        GCodeLayerRenderer fresh;
        REQUIRE(fresh.get_antialias_enabled());
        REQUIRE(fresh.get_ssao_enabled());
    }

    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);
    lv_obj_t* viewer = make_viewer(parent);

    const GCodeLayerRenderer* renderer = gcode_viewer_budget_force_2d(viewer, make_two_tool_file());
    REQUIRE(renderer != nullptr);

    CHECK_FALSE(renderer->get_antialias_enabled());
    CHECK_FALSE(renderer->get_ssao_enabled());

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture, "the budget 2D fallback keeps the AMS tool colours",
                 "[gcode_viewer][gcode][colors][1555]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);
    lv_obj_t* viewer = make_viewer(parent);

    // Applied before any renderer exists, which is the ordering the fallback has
    // to survive: the viewer holds the lane colours and hands them to whatever
    // renderer it creates next.
    ui_gcode_viewer_set_tool_colors(viewer, {kLaneT0, kLaneT1});
    REQUIRE(ui_gcode_viewer_get_tool_colors(viewer) == std::vector<uint32_t>{kLaneT0, kLaneT1});

    const GCodeLayerRenderer* renderer = gcode_viewer_budget_force_2d(viewer, make_two_tool_file());
    REQUIRE(renderer != nullptr);

    // AMS-known slot colours outrank the slicer palette in the file. Installing
    // only the file's palette is the failure this pins: it repaints a
    // multi-colour print in colours the machine is not loaded with, and nothing
    // re-applies the lanes afterwards.
    CHECK(resolved(*renderer, 0) == kLaneT0);
    CHECK(resolved(*renderer, 1) == kLaneT1);
    CHECK(resolved(*renderer, 0) != kSlicerT0);
    CHECK(resolved(*renderer, 1) != kSlicerT1);

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture, "the budget 2D fallback takes the viewer's framing",
                 "[gcode_viewer][gcode][1555]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);
    lv_obj_t* viewer = make_viewer(parent);

    // Thumbnail parity is a fit shape, and the fallback's auto_fit() runs at
    // creation — a renderer created on STANDARD fits the model once at the wrong
    // scale before the draw path pushes the real framing.
    {
        GCodeLayerRenderer fresh;
        REQUIRE(GCodeLayerRendererTestAccess::framing(fresh) == FitFraming::STANDARD);
    }
    ui_gcode_viewer_set_thumbnail_parity(viewer, true);

    const GCodeLayerRenderer* renderer = gcode_viewer_budget_force_2d(viewer, make_two_tool_file());
    REQUIRE(renderer != nullptr);

    CHECK(GCodeLayerRendererTestAccess::framing(*renderer) == FitFraming::THUMBNAIL_PARITY);

    lv_obj_delete(parent);
}
