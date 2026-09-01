// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "gcode_projection.h"
#include "gcode_selection_style.h"
#include "gcode_streaming_controller.h"
#include "system/crash_handler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

namespace {

// Helper to build a ParsedGCodeFile with named objects and an unnamed segment.
//
// Layout (TOP_DOWN, world coords):
//   "cube1" : segment from (10,20) to (50,20)   -- y=20
//   "cube2" : segment from (10,80) to (50,80)   -- y=80
//   unnamed : segment from (10,50) to (50,50)   -- y=50
//
// All at z=0.2, single layer.
ParsedGCodeFile make_test_gcode() {
    ParsedGCodeFile gcode;
    Layer layer;
    layer.z_height = 0.2f;

    // Expand bounding box to cover all segments
    layer.bounding_box.expand(glm::vec3(10.0f, 20.0f, 0.2f));
    layer.bounding_box.expand(glm::vec3(50.0f, 80.0f, 0.2f));

    // Object "cube1" - segments at x=[10,50], y=20
    ToolpathSegment seg1;
    seg1.start = glm::vec3(10.0f, 20.0f, 0.2f);
    seg1.end = glm::vec3(50.0f, 20.0f, 0.2f);
    seg1.is_extrusion = true;
    seg1.object_name_index = gcode.intern_object_name("cube1");
    layer.segments.push_back(seg1);

    // Object "cube2" - segments at x=[10,50], y=80
    ToolpathSegment seg2;
    seg2.start = glm::vec3(10.0f, 80.0f, 0.2f);
    seg2.end = glm::vec3(50.0f, 80.0f, 0.2f);
    seg2.is_extrusion = true;
    seg2.object_name_index = gcode.intern_object_name("cube2");
    layer.segments.push_back(seg2);

    // Unnamed segment at y=50
    ToolpathSegment seg3;
    seg3.start = glm::vec3(10.0f, 50.0f, 0.2f);
    seg3.end = glm::vec3(50.0f, 50.0f, 0.2f);
    seg3.is_extrusion = true;
    // object_name_index left as -1 (default)
    layer.segments.push_back(seg3);

    layer.segment_count_extrusion = 3;
    layer.segment_count_travel = 0;

    gcode.layers.push_back(std::move(layer));
    gcode.total_segments = 3;

    // Set global bounding box
    gcode.global_bounding_box.expand(glm::vec3(10.0f, 20.0f, 0.2f));
    gcode.global_bounding_box.expand(glm::vec3(50.0f, 80.0f, 0.2f));

    return gcode;
}

// Helper to build a ParsedGCodeFile with a single named object for simple tests.
ParsedGCodeFile make_single_object_gcode(const std::string& name, float y) {
    ParsedGCodeFile gcode;
    Layer layer;
    layer.z_height = 0.2f;

    ToolpathSegment seg;
    seg.start = glm::vec3(10.0f, y, 0.2f);
    seg.end = glm::vec3(90.0f, y, 0.2f);
    seg.is_extrusion = true;
    seg.object_name_index = gcode.intern_object_name(name);
    layer.segments.push_back(seg);

    layer.bounding_box.expand(seg.start);
    layer.bounding_box.expand(seg.end);
    layer.segment_count_extrusion = 1;

    gcode.layers.push_back(std::move(layer));
    gcode.total_segments = 1;
    gcode.global_bounding_box.expand(glm::vec3(10.0f, y, 0.2f));
    gcode.global_bounding_box.expand(glm::vec3(90.0f, y, 0.2f));

    return gcode;
}

} // namespace

// =============================================================================
// Exclude Object Support
// =============================================================================

TEST_CASE("set_excluded_objects stores names and can be cleared", "[layer_renderer][exclude]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_test_gcode();
    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.auto_fit();
    renderer.set_current_layer(0);

    SECTION("setting excluded objects does not crash") {
        std::unordered_set<std::string> excluded = {"cube1"};
        REQUIRE_NOTHROW(renderer.set_excluded_objects(excluded));
    }

    SECTION("clearing excluded objects does not crash") {
        std::unordered_set<std::string> excluded = {"cube1", "cube2"};
        renderer.set_excluded_objects(excluded);

        std::unordered_set<std::string> empty;
        REQUIRE_NOTHROW(renderer.set_excluded_objects(empty));
    }

    SECTION("setting excluded objects with unknown name does not crash") {
        std::unordered_set<std::string> excluded = {"nonexistent_object"};
        REQUIRE_NOTHROW(renderer.set_excluded_objects(excluded));
    }
}

// =============================================================================
// Highlight Object Support
// =============================================================================

TEST_CASE("set_highlighted_objects stores names and can be cleared",
          "[layer_renderer][highlight]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_test_gcode();
    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.auto_fit();
    renderer.set_current_layer(0);

    SECTION("setting highlighted objects does not crash") {
        std::unordered_set<std::string> highlighted = {"cube2"};
        REQUIRE_NOTHROW(renderer.set_highlighted_objects(highlighted));
    }

    SECTION("clearing highlighted objects does not crash") {
        std::unordered_set<std::string> highlighted = {"cube1"};
        renderer.set_highlighted_objects(highlighted);

        std::unordered_set<std::string> empty;
        REQUIRE_NOTHROW(renderer.set_highlighted_objects(empty));
    }

    SECTION("setting highlighted objects with unknown name does not crash") {
        std::unordered_set<std::string> highlighted = {"nonexistent_object"};
        REQUIRE_NOTHROW(renderer.set_highlighted_objects(highlighted));
    }
}

// =============================================================================
// Pick Object At - Core algorithmic tests
// =============================================================================

TEST_CASE("pick_object_at returns object name for segment under cursor", "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_test_gcode();
    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.auto_fit();
    renderer.set_current_layer(0);

    // In TOP_DOWN mode with auto_fit on bounding box (10,20)-(50,80):
    //   center = (30, 50), range_x = 40, range_y = 60
    //   scale = min(200/(40*1.1), 200/(60*1.1)) ~= min(4.54, 3.03) ~= 3.03
    //   offset_x = 30, offset_y = 50
    //
    // world_to_screen(x, y):
    //   sx = (x - 30) * scale + 100
    //   sy = 100 - (y - 50) * scale
    //
    // For cube1 midpoint (30, 20):
    //   sx = 0 * 3.03 + 100 = 100
    //   sy = 100 - (-30) * 3.03 = 100 + 90.9 = ~191
    //
    // For cube2 midpoint (30, 80):
    //   sx = 100
    //   sy = 100 - 30 * 3.03 = 100 - 90.9 = ~9

    // Pick near cube1's midpoint (bottom of screen in top-down, since Y is flipped)
    auto result_cube1 = renderer.pick_object_at(100, 191);
    REQUIRE(result_cube1.has_value());
    CHECK(result_cube1.value() == "cube1");

    // Pick near cube2's midpoint (top of screen in top-down)
    auto result_cube2 = renderer.pick_object_at(100, 9);
    REQUIRE(result_cube2.has_value());
    CHECK(result_cube2.value() == "cube2");
}

TEST_CASE("pick_object_at returns nullopt for empty space", "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_test_gcode();
    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.auto_fit();
    renderer.set_current_layer(0);

    // Pick at far corner where no segments exist
    auto result = renderer.pick_object_at(0, 0);
    REQUIRE_FALSE(result.has_value());

    // Another empty spot
    auto result2 = renderer.pick_object_at(199, 199);
    REQUIRE_FALSE(result2.has_value());
}

TEST_CASE("pick_object_at skips segments without object_name", "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_test_gcode();
    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.auto_fit();
    renderer.set_current_layer(0);

    // The unnamed segment is at y=50 (world), which maps to sy=100 (screen center).
    // Picking at screen center should NOT return a result since the unnamed
    // segment has an empty object_name.
    auto result = renderer.pick_object_at(100, 100);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("pick_object_at with multiple objects picks closest", "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_test_gcode();
    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.auto_fit();
    renderer.set_current_layer(0);

    // cube1 is at y=20 (screen ~191), cube2 is at y=80 (screen ~9)
    // Pick slightly closer to cube1 than cube2
    auto near_cube1 = renderer.pick_object_at(100, 180);
    if (near_cube1.has_value()) {
        CHECK(near_cube1.value() == "cube1");
    }

    // Pick slightly closer to cube2 than cube1
    auto near_cube2 = renderer.pick_object_at(100, 20);
    if (near_cube2.has_value()) {
        CHECK(near_cube2.value() == "cube2");
    }
}

TEST_CASE("pick_object_at with no gcode returns nullopt", "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);

    // No gcode set - should return nullopt
    auto result = renderer.pick_object_at(100, 100);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("pick_object_at with empty layer returns nullopt", "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    ParsedGCodeFile gcode;
    Layer empty_layer;
    empty_layer.z_height = 0.2f;
    gcode.layers.push_back(empty_layer);
    gcode.total_segments = 0;

    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.set_current_layer(0);

    auto result = renderer.pick_object_at(100, 100);
    REQUIRE_FALSE(result.has_value());
}

// =============================================================================
// Exclude + Pick interaction
// =============================================================================

TEST_CASE("excluded objects are still pickable", "[layer_renderer][exclude][pick]") {
    // Excluded objects should still be pickable (user needs to be able to
    // un-exclude them by tapping on them)
    GCodeLayerRenderer renderer;
    auto gcode = make_test_gcode();
    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(200, 200);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.auto_fit();
    renderer.set_current_layer(0);

    // Exclude cube1
    std::unordered_set<std::string> excluded = {"cube1"};
    renderer.set_excluded_objects(excluded);

    // Should still be able to pick cube1
    auto result = renderer.pick_object_at(100, 191);
    REQUIRE(result.has_value());
    CHECK(result.value() == "cube1");
}

// =============================================================================
// Exclude/Highlight with no data source
// =============================================================================

TEST_CASE("set_excluded_objects with no gcode does not crash", "[layer_renderer][exclude]") {
    GCodeLayerRenderer renderer;

    std::unordered_set<std::string> excluded = {"cube1"};
    REQUIRE_NOTHROW(renderer.set_excluded_objects(excluded));
}

TEST_CASE("set_highlighted_objects with no gcode does not crash", "[layer_renderer][highlight]") {
    GCodeLayerRenderer renderer;

    std::unordered_set<std::string> highlighted = {"cube1"};
    REQUIRE_NOTHROW(renderer.set_highlighted_objects(highlighted));
}

// =============================================================================
// Crash-diagnostics breadcrumb
// =============================================================================

namespace {
// Capture crash_handler breadcrumb ring as newline-split lines, mirroring the
// pipe+dump_to_fd helper in test_crash_handler.cpp.
std::vector<std::string> capture_breadcrumb_lines() {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    crash_handler::breadcrumb::dump_to_fd(fds[1]);
    ::close(fds[1]);
    std::string all;
    char chunk[4096];
    ssize_t n;
    while ((n = ::read(fds[0], chunk, sizeof(chunk))) > 0) {
        all.append(chunk, static_cast<size_t>(n));
    }
    ::close(fds[0]);
    std::vector<std::string> lines;
    size_t pos = 0, nl;
    while ((nl = all.find('\n', pos)) != std::string::npos) {
        lines.push_back(all.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}
} // namespace

// Verifies the breadcrumb added to GCodeLayerRenderer::render_layers_to_cache
// actually fires on the real render path. A SIGBUS was seen inside that path on
// AD5X (bundle YZQ47HQ6); the crumb gives the crash handler the gcode subsystem
// + target layer even when the stack is too corrupt to scan. render() reaches
// render_layers_to_cache only in FRONT view after warmup, so we drive several
// frames against an offscreen canvas layer with ghost mode off (deterministic,
// no background thread).
TEST_CASE_METHOD(LVGLTestFixture, "render_layers_to_cache emits gcode breadcrumb",
                 "[layer_renderer][crash][breadcrumb]") {
    auto gcode = make_test_gcode();

    GCodeLayerRenderer renderer;
    renderer.set_gcode(&gcode);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    renderer.set_ghost_mode(false); // deterministic: no background ghost thread
    renderer.set_canvas_size(200, 200);
    renderer.set_current_layer(0);

    // Offscreen canvas provides a real lv_layer_t for render() to blit into.
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t canvas_buf[200 * 200 * 4];
    lv_canvas_set_buffer(canvas, canvas_buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);
    lv_area_t clip = {0, 0, 199, 199};

    // The first WARMUP_FRAMES (2) render() calls skip heavy caching; loop past
    // them so the solid-cache path (render_layers_to_cache) runs at least once.
    for (int i = 0; i < 5; ++i) {
        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        renderer.render(&layer, &clip);
        lv_canvas_finish_layer(canvas, &layer);
    }

    auto crumbs = capture_breadcrumb_lines();
    bool found = false;
    for (const auto& line : crumbs) {
        if (line.find("gcode render_cache") != std::string::npos) {
            found = true;
            break;
        }
    }
    if (!found) {
        INFO("breadcrumb ring contents:");
        for (const auto& line : crumbs) {
            INFO(line);
        }
    }
    REQUIRE(found);
}

// =============================================================================
// Pick Object At - whole-stack hit testing
//
// The view draws every layer up to current_layer_, so picking has to match. A
// full preview parks current_layer_ on the TOP layer, whose segments are often
// a sliver in one corner, which used to make tap-to-select and
// long-press-to-exclude dead in the 2D view. The fixtures below populate
// ParsedGCodeFile::objects with real 3D bounding boxes (make_test_gcode() above
// deliberately does not), which is what the parser produces and what the
// projected-AABB stage of the picker consumes.
// =============================================================================

namespace {

constexpr int PICK_CANVAS = 200;

// Mirror of the renderer's own projection so these tests can name a click point
// in world millimetres instead of hard-coded pixels. auto_fit() feeds
// compute_auto_fit() the global bounding box with the default 5% padding, and
// content_offset_y_percent_ defaults to 0. The view mode is a parameter because
// TOP_DOWN and ISOMETRIC discard Z entirely - only FRONT maps Z to a screen
// coordinate, so only FRONT can show the drawn-Z clamp moving a footprint.
glm::ivec2 project_expected(const AABB& fit_box, float x, float y, float z = 0.0f,
                            ViewMode view = ViewMode::TOP_DOWN) {
    auto fit = compute_auto_fit(fit_box, view, PICK_CANVAS, PICK_CANVAS);
    ProjectionParams params;
    params.view_mode = view;
    params.scale = fit.scale;
    params.offset_x = fit.offset_x;
    params.offset_y = fit.offset_y;
    params.offset_z = fit.offset_z;
    params.canvas_width = PICK_CANVAS;
    params.canvas_height = PICK_CANVAS;
    return project(params, x, y, z);
}

bool xy_inside(const AABB& box, float x, float y) {
    return x >= box.min.x && x <= box.max.x && y >= box.min.y && y <= box.max.y;
}

// Topmost (smallest) screen y over a box's 8 projected corners - the same
// reduction the picker's stage 1 does. Derived rather than hand-picked because
// which corner wins is not obvious: FRONT's SIN_H is sin(-45°), so the vertical
// term peaks at MAX x and MAX y, and guessing min y silently produces a point
// that is still inside the box.
int projected_top_y(const AABB& fit_box, const AABB& box, ViewMode view) {
    int top = std::numeric_limits<int>::max();
    for (const glm::vec3& corner : box.corners()) {
        top = std::min(top, project_expected(fit_box, corner.x, corner.y, corner.z, view).y);
    }
    return top;
}

// Grow `layer` and `gcode` by one extrusion segment attributed to `object`,
// mirroring GCodeParser: the object's bounding_box is expanded over that
// object's own extrusion endpoints, so it spans the object's full Z range
// rather than a single layer.
void add_object_segment(ParsedGCodeFile& gcode, Layer& layer, const std::string& object,
                        const glm::vec3& start, const glm::vec3& end) {
    ToolpathSegment seg;
    seg.start = start;
    seg.end = end;
    seg.is_extrusion = true;
    seg.object_name_index = gcode.intern_object_name(object);
    layer.segments.push_back(seg);
    layer.segment_count_extrusion++;
    layer.bounding_box.expand(start);
    layer.bounding_box.expand(end);

    gcode.global_bounding_box.expand(start);
    gcode.global_bounding_box.expand(end);
    gcode.total_segments++;

    GCodeObject& obj = gcode.objects[object];
    obj.name = object;
    obj.bounding_box.expand(start);
    obj.bounding_box.expand(end);
}

// Object "body" spans layers 0..3 as two rails across XY [10,50]x[10,50]. The
// TOP layer (4) holds only a short patch parked at y=90, far from the body.
// That is the shipped-bug shape: the picker saw layer 4 only.
ParsedGCodeFile make_tall_object_gcode() {
    ParsedGCodeFile gcode;
    for (int i = 0; i < 4; ++i) {
        Layer layer;
        layer.z_height = 0.2f * static_cast<float>(i);
        const float z = layer.z_height;
        add_object_segment(gcode, layer, "body", {10.0f, 10.0f, z}, {50.0f, 10.0f, z});
        add_object_segment(gcode, layer, "body", {10.0f, 50.0f, z}, {50.0f, 50.0f, z});
        gcode.layers.push_back(std::move(layer));
    }
    Layer top;
    top.z_height = 0.8f;
    add_object_segment(gcode, top, "body", {10.0f, 90.0f, 0.8f}, {14.0f, 90.0f, 0.8f});
    gcode.layers.push_back(std::move(top));
    return gcode;
}

// Two L-shaped objects on one layer whose projected boxes overlap in
// XY [40,60]x[40,60], so a click there is a candidate for both:
//   "alpha": rail x=10 (y 10..60) + rail y=50 (x 10..60)  -> box [10,60]x[10,60]
//   "beta" : rail x=90 (y 40..90) + rail y=90 (x 40..90)
//            + rail x=55 (y 40..90)                       -> box [40,90]x[40,90]
// "alpha" sorts first in the std::map, so a picker that just returned the first
// containing box would always answer "alpha".
ParsedGCodeFile make_overlapping_boxes_gcode() {
    ParsedGCodeFile gcode;
    Layer layer;
    layer.z_height = 0.2f;
    const float z = 0.2f;
    add_object_segment(gcode, layer, "alpha", {10.0f, 10.0f, z}, {10.0f, 60.0f, z});
    add_object_segment(gcode, layer, "alpha", {10.0f, 50.0f, z}, {60.0f, 50.0f, z});
    add_object_segment(gcode, layer, "beta", {90.0f, 40.0f, z}, {90.0f, 90.0f, z});
    add_object_segment(gcode, layer, "beta", {40.0f, 90.0f, z}, {90.0f, 90.0f, z});
    add_object_segment(gcode, layer, "beta", {55.0f, 40.0f, z}, {55.0f, 90.0f, z});
    gcode.layers.push_back(std::move(layer));
    return gcode;
}

// 12 z-steps. The bottom layer is object "base" in the low XY corner; every
// layer above is object "tower" in the high corner. Each layer wraps its own
// EXCLUDE_OBJECT_START/END so a layer chunk parsed in isolation (streaming)
// still carries object attribution.
std::string make_streaming_pick_gcode() {
    std::ostringstream out;
    out << "; streaming pick fixture\nG28\nG90\nM82\n";
    int e = 1;
    for (int i = 0; i < 12; ++i) {
        const char* name = (i == 0) ? "base" : "tower";
        const int x0 = (i == 0) ? 10 : 70;
        const int y0 = (i == 0) ? 10 : 70;
        out << "G1 Z" << (0.2f * static_cast<float>(i + 1)) << " F1000\n";
        out << "G1 X" << x0 << " Y" << y0 << " F3000\n";
        out << "EXCLUDE_OBJECT_START NAME=" << name << "\n";
        out << "G1 X" << (x0 + 20) << " Y" << y0 << " E" << e++ << "\n";
        out << "G1 X" << (x0 + 20) << " Y" << (y0 + 20) << " E" << e++ << "\n";
        out << "EXCLUDE_OBJECT_END NAME=" << name << "\n";
    }
    return out.str();
}

class TempPickGCodeFile {
  public:
    explicit TempPickGCodeFile(const std::string& content) {
        char temp_path[] = "/tmp/gcode_pick_test_XXXXXX";
        int fd = ::mkstemp(temp_path);
        REQUIRE(fd != -1);
        ::close(fd);
        path_ = temp_path;
        std::ofstream file(path_);
        file << content;
    }
    ~TempPickGCodeFile() {
        ::remove(path_.c_str());
    }
    const std::string& path() const {
        return path_;
    }

  private:
    std::string path_;
};

// Full-file renderer wired the way the preview panel wires it.
void configure(GCodeLayerRenderer& renderer, const ParsedGCodeFile& gcode, int layer,
               GCodeLayerRenderer::ViewMode view = GCodeLayerRenderer::ViewMode::TOP_DOWN) {
    renderer.set_gcode(&gcode);
    renderer.set_canvas_size(PICK_CANVAS, PICK_CANVAS);
    renderer.set_view_mode(view);
    renderer.auto_fit();
    renderer.set_current_layer(layer);
}

// Two objects stacked in Z but disjoint in XY, so a TOP_DOWN click over one is
// never a candidate for the other:
//   "base"  : layers 0..1 (z 0.0, 0.2), rails in XY [10,50]x[10,50]
//   "riser" : layers 2..4 (z 0.4..0.8), rails in XY [10,50]x[70,110]
// With current_layer_ below 2, "riser" has not started printing: render() draws
// nothing of it, so nothing about it may be pickable.
ParsedGCodeFile make_stacked_in_z_gcode() {
    ParsedGCodeFile gcode;
    for (int i = 0; i < 5; ++i) {
        Layer layer;
        layer.z_height = 0.2f * static_cast<float>(i);
        const float z = layer.z_height;
        if (i < 2) {
            add_object_segment(gcode, layer, "base", {10.0f, 10.0f, z}, {50.0f, 10.0f, z});
            add_object_segment(gcode, layer, "base", {10.0f, 50.0f, z}, {50.0f, 50.0f, z});
        } else {
            add_object_segment(gcode, layer, "riser", {10.0f, 70.0f, z}, {50.0f, 70.0f, z});
            add_object_segment(gcode, layer, "riser", {10.0f, 110.0f, z}, {50.0f, 110.0f, z});
        }
        gcode.layers.push_back(std::move(layer));
    }
    return gcode;
}

// One tall, narrow object: rails around XY [10,20]x[10,20] repeated over ten
// layers 5mm apart, so the box is z [0,45] on a 10mm footprint. The exaggerated
// aspect ratio is deliberate - in FRONT view a 1mm Z error is only ~4px at this
// scale, so a short object could not separate the clamped top from the
// unclamped one by more than the picker's own 15px slop.
ParsedGCodeFile make_tall_column_gcode() {
    ParsedGCodeFile gcode;
    for (int i = 0; i < 10; ++i) {
        Layer layer;
        layer.z_height = 5.0f * static_cast<float>(i);
        const float z = layer.z_height;
        add_object_segment(gcode, layer, "column", {10.0f, 10.0f, z}, {20.0f, 10.0f, z});
        add_object_segment(gcode, layer, "column", {10.0f, 20.0f, z}, {20.0f, 20.0f, z});
        gcode.layers.push_back(std::move(layer));
    }
    return gcode;
}

// A part and a separate exclude-object whose name trips
// name_looks_like_support(), on one layer, disjoint in XY.
ParsedGCodeFile make_support_object_gcode() {
    ParsedGCodeFile gcode;
    Layer layer;
    layer.z_height = 0.2f;
    const float z = 0.2f;
    add_object_segment(gcode, layer, "widget", {10.0f, 10.0f, z}, {50.0f, 10.0f, z});
    add_object_segment(gcode, layer, "widget", {10.0f, 50.0f, z}, {50.0f, 50.0f, z});
    add_object_segment(gcode, layer, "widget_Support", {70.0f, 10.0f, z}, {110.0f, 10.0f, z});
    add_object_segment(gcode, layer, "widget_Support", {70.0f, 50.0f, z}, {110.0f, 50.0f, z});
    gcode.layers.push_back(std::move(layer));
    return gcode;
}

} // namespace

// THE REGRESSION. current_layer_ is the top layer, whose only segments sit at
// y=90; the click is in the middle of the object's body, 20mm clear of every
// rail on every layer. Before the projected-AABB stage existed this returned
// nullopt, which is exactly what a real tap on a 3-object plate did.
TEST_CASE("pick_object_at picks an object whose body is below the current layer",
          "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_tall_object_gcode();
    configure(renderer, gcode, 4);

    const AABB& fit_box = gcode.global_bounding_box;
    const glm::ivec2 body = project_expected(fit_box, 30.0f, 30.0f);
    const glm::ivec2 top_patch = project_expected(fit_box, 12.0f, 90.0f);
    const glm::ivec2 lower_rail = project_expected(fit_box, 30.0f, 10.0f);

    // Fixture guards: the click must be far from the top layer's segments AND
    // from the body's own rails, so a pass cannot come from segment proximity.
    REQUIRE(glm::distance(glm::vec2(body), glm::vec2(top_patch)) > 15.0f);
    REQUIRE(glm::distance(glm::vec2(body), glm::vec2(lower_rail)) > 15.0f);

    auto result = renderer.pick_object_at(body.x, body.y);
    REQUIRE(result.has_value());
    CHECK(result.value() == "body");
}

TEST_CASE("pick_object_at returns nullopt outside every object's projected box",
          "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_tall_object_gcode();
    configure(renderer, gcode, 4);

    const AABB& fit_box = gcode.global_bounding_box;
    const glm::ivec2 left_edge = project_expected(fit_box, 10.0f, 30.0f);

    // 40px left of the box's left edge - clear of the PICK_THRESHOLD_PX inflation.
    auto left_miss = renderer.pick_object_at(left_edge.x - 40, left_edge.y);
    CHECK_FALSE(left_miss.has_value());

    const glm::ivec2 right_edge = project_expected(fit_box, 50.0f, 30.0f);
    auto right_miss = renderer.pick_object_at(right_edge.x + 40, right_edge.y);
    CHECK_FALSE(right_miss.has_value());
}

TEST_CASE("pick_object_at disambiguates overlapping projected boxes by segment distance",
          "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_overlapping_boxes_gcode();
    configure(renderer, gcode, 0);

    const AABB& fit_box = gcode.global_bounding_box;
    const AABB& alpha_box = gcode.objects.at("alpha").bounding_box;
    const AABB& beta_box = gcode.objects.at("beta").bounding_box;

    // Fixture guard: this click is inside BOTH boxes, so the tie has to be
    // broken by geometry. It sits on beta's x=55 rail and 9mm (~20px) from
    // alpha's nearest rail.
    REQUIRE(xy_inside(alpha_box, 55.0f, 41.0f));
    REQUIRE(xy_inside(beta_box, 55.0f, 41.0f));

    const glm::ivec2 on_beta = project_expected(fit_box, 55.0f, 41.0f);
    auto beta_hit = renderer.pick_object_at(on_beta.x, on_beta.y);
    REQUIRE(beta_hit.has_value());
    CHECK(beta_hit.value() == "beta");

    // And a click only alpha's box contains still resolves to alpha.
    REQUIRE_FALSE(xy_inside(beta_box, 10.0f, 20.0f));
    const glm::ivec2 on_alpha = project_expected(fit_box, 10.0f, 20.0f);
    auto alpha_hit = renderer.pick_object_at(on_alpha.x, on_alpha.y);
    REQUIRE(alpha_hit.has_value());
    CHECK(alpha_hit.value() == "alpha");
}

TEST_CASE("pick_object_at skips objects with an empty bounding box", "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_tall_object_gcode();

    // EXCLUDE_OBJECT_DEFINE ran but no extrusion followed, so bounding_box is
    // still the +/-inf default. Projecting those corners must not be attempted.
    GCodeObject& ghost = gcode.objects["ghost"];
    ghost.name = "ghost";
    REQUIRE(ghost.bounding_box.is_empty());

    configure(renderer, gcode, 4);

    const AABB& fit_box = gcode.global_bounding_box;
    const glm::ivec2 body = project_expected(fit_box, 30.0f, 30.0f);
    const glm::ivec2 left_edge = project_expected(fit_box, 10.0f, 30.0f);

    std::optional<std::string> body_hit;
    REQUIRE_NOTHROW(body_hit = renderer.pick_object_at(body.x, body.y));
    REQUIRE(body_hit.has_value());
    CHECK(body_hit.value() == "body");

    // Empty space must not resolve to the never-extruded object.
    std::optional<std::string> miss;
    REQUIRE_NOTHROW(miss = renderer.pick_object_at(left_edge.x - 40, left_edge.y));
    CHECK_FALSE(miss.has_value());
}

TEST_CASE("pick_object_at in streaming mode walks past uncached layers",
          "[layer_renderer][pick][streaming]") {
    TempPickGCodeFile file(make_streaming_pick_gcode());
    GCodeStreamingController controller;
    REQUIRE(controller.open_file(file.path()));

    const int layer_count = static_cast<int>(controller.get_layer_count());
    REQUIRE(layer_count >= 12);

    GCodeLayerRenderer renderer;
    renderer.set_streaming_controller(&controller);
    renderer.set_canvas_size(PICK_CANVAS, PICK_CANVAS);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::TOP_DOWN);
    renderer.auto_fit();

    // Cache only the bottom of the file. DEFAULT_PREFETCH_RADIUS is 3, so
    // everything above layer 3 stays uncached and try_get_layer_segments()
    // returns null there - the downward walk has to skip it, never seek.
    (void)controller.get_layer_segments(0);
    controller.wait_for_prefetch_idle();
    REQUIRE_FALSE(controller.is_layer_cached(static_cast<size_t>(layer_count - 1)));

    renderer.set_current_layer(layer_count - 1);

    const auto& stats = controller.get_index_stats();
    REQUIRE(stats.has_xy_bounds());
    AABB fit_box;
    fit_box.min = {stats.min_x, stats.min_y, stats.min_z};
    fit_box.max = {stats.max_x, stats.max_y, stats.max_z};

    // On "base"'s rail, which lives only on the bottom (cached) layer.
    const glm::ivec2 on_base = project_expected(fit_box, 20.0f, 10.0f);
    std::optional<std::string> result;
    REQUIRE_NOTHROW(result = renderer.pick_object_at(on_base.x, on_base.y));
    REQUIRE(result.has_value());
    CHECK(result.value() == "base");
}

// =============================================================================
// Pick Object At - an invisible object must not be pickable
//
// Stage 1 projects each object's whole 3D bounding box, but render() only draws
// layers 0..current_layer_. Without clamping the box to the drawn Z range, an
// object that has not started printing is fully pickable while completely
// invisible, and a partly-printed one claims screen area above its drawn top.
// You cannot exclude what you cannot see, so both are UX failures.
// =============================================================================

TEST_CASE("pick_object_at ignores an object that has not started printing",
          "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_stacked_in_z_gcode();

    const AABB& riser_box = gcode.objects.at("riser").bounding_box;
    const AABB& fit_box = gcode.global_bounding_box;

    // Fixture guard: "riser" starts at z=0.4, above the z=0.2 top of layer 1.
    REQUIRE(riser_box.min.z > gcode.layers.at(1).z_height);

    // Layer 1 is the top drawn layer, so nothing of "riser" is on screen.
    configure(renderer, gcode, 1);

    const glm::ivec2 on_riser = project_expected(fit_box, 30.0f, 90.0f);

    // Fixture guard: the click IS inside riser's unclamped projected box, which
    // is what made it pickable, and is nowhere near any drawn segment.
    REQUIRE(xy_inside(riser_box, 30.0f, 90.0f));
    const glm::ivec2 nearest_drawn = project_expected(fit_box, 30.0f, 50.0f);
    REQUIRE(glm::distance(glm::vec2(on_riser), glm::vec2(nearest_drawn)) > 15.0f);

    auto miss = renderer.pick_object_at(on_riser.x, on_riser.y);
    CHECK_FALSE(miss.has_value());

    // The object that IS drawn is unaffected.
    const glm::ivec2 on_base = project_expected(fit_box, 30.0f, 30.0f);
    auto base_hit = renderer.pick_object_at(on_base.x, on_base.y);
    REQUIRE(base_hit.has_value());
    CHECK(base_hit.value() == "base");
}

TEST_CASE("pick_object_at clamps a partly-drawn object to its drawn top",
          "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_tall_column_gcode();

    // FRONT is the only view mode whose projection consumes Z, so it is the
    // only one where an overstated Z range overstates the screen footprint.
    configure(renderer, gcode, 2, GCodeLayerRenderer::ViewMode::FRONT);

    const AABB& fit_box = gcode.global_bounding_box;
    const AABB& box = gcode.objects.at("column").bounding_box;
    const float visible_top_z = gcode.layers.at(2).z_height;

    // Fixture guard: layer 2 draws only a fifth of the column's height.
    REQUIRE(visible_top_z < box.max.z);

    // Screen top of the full box versus the box clamped to the drawn Z. The
    // clamp is the only difference, and in FRONT a higher Z is higher on screen,
    // so the clamped top sits lower (larger y).
    const AABB drawn_box{box.min, glm::vec3(box.max.x, box.max.y, visible_top_z)};
    const int top_unclamped = projected_top_y(fit_box, box, ViewMode::FRONT);
    const int top_drawn = projected_top_y(fit_box, drawn_box, ViewMode::FRONT);

    // Fixture guard: the undrawn Z is worth far more than the picker's 15px
    // slop, so the two tops cannot be confused.
    REQUIRE(top_drawn - top_unclamped > 60);

    // Horizontally centred, so the click is inside both boxes' x extent - Z
    // does not enter sx, so the two share it exactly.
    const int click_x =
        project_expected(fit_box, box.center().x, box.center().y, visible_top_z, ViewMode::FRONT).x;

    // 30px above the drawn top: outside the clamped box even after inflation,
    // but comfortably inside the unclamped one.
    const int click_y = top_drawn - 30;
    REQUIRE(click_y > top_unclamped);
    auto above_drawn = renderer.pick_object_at(click_x, click_y);
    CHECK_FALSE(above_drawn.has_value());

    // The drawn body itself still picks. Mid-footprint, halfway up the drawn
    // height - unambiguously inside the clamped box.
    const glm::ivec2 on_drawn =
        project_expected(fit_box, 15.0f, 15.0f, visible_top_z * 0.5f, ViewMode::FRONT);
    auto drawn_hit = renderer.pick_object_at(on_drawn.x, on_drawn.y);
    REQUIRE(drawn_hit.has_value());
    CHECK(drawn_hit.value() == "column");
}

TEST_CASE("pick_object_at still picks the layer currently being drawn", "[layer_renderer][pick]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_stacked_in_z_gcode();
    const AABB& fit_box = gcode.global_bounding_box;
    const AABB& riser_box = gcode.objects.at("riser").bounding_box;
    const glm::ivec2 on_riser = project_expected(fit_box, 30.0f, 90.0f);

    SECTION("box.min.z exactly equals the drawn top - the object's first layer") {
        configure(renderer, gcode, 2);
        REQUIRE(riser_box.min.z == gcode.layers.at(2).z_height);
        auto hit = renderer.pick_object_at(on_riser.x, on_riser.y);
        REQUIRE(hit.has_value());
        CHECK(hit.value() == "riser");
    }

    SECTION("box.max.z exactly equals the drawn top - the object's last layer") {
        configure(renderer, gcode, 4);
        REQUIRE(riser_box.max.z == gcode.layers.at(4).z_height);
        auto hit = renderer.pick_object_at(on_riser.x, on_riser.y);
        REQUIRE(hit.has_value());
        CHECK(hit.value() == "riser");
    }
}

TEST_CASE("pick_object_at honors the support visibility toggle",
          "[layer_renderer][pick][support]") {
    GCodeLayerRenderer renderer;
    auto gcode = make_support_object_gcode();
    configure(renderer, gcode, 0);

    const AABB& fit_box = gcode.global_bounding_box;
    const glm::ivec2 on_support = project_expected(fit_box, 90.0f, 30.0f);
    const glm::ivec2 on_widget = project_expected(fit_box, 30.0f, 30.0f);

    // Fixture guard: the click is inside the support object's box only.
    REQUIRE(xy_inside(gcode.objects.at("widget_Support").bounding_box, 90.0f, 30.0f));
    REQUIRE_FALSE(xy_inside(gcode.objects.at("widget").bounding_box, 90.0f, 30.0f));

    SECTION("supports shown - the support object is pickable") {
        renderer.set_show_supports(true);
        auto hit = renderer.pick_object_at(on_support.x, on_support.y);
        REQUIRE(hit.has_value());
        CHECK(hit.value() == "widget_Support");
    }

    SECTION("supports hidden - the support object is not pickable") {
        renderer.set_show_supports(false);
        auto miss = renderer.pick_object_at(on_support.x, on_support.y);
        CHECK_FALSE(miss.has_value());

        // Hiding supports must not make the part unpickable.
        auto widget_hit = renderer.pick_object_at(on_widget.x, on_widget.y);
        REQUIRE(widget_hit.has_value());
        CHECK(widget_hit.value() == "widget");
    }
}

// ===========================================================================
// Selection index-map wiring.
//
// Selection is classified by interned object index through SelectionState, which
// only answers correctly once rebuild_index_map() has been fed the object-name
// table. Miss a wiring site and selection silently stops working while every
// unit test above stays green -- the pre-existing exclude tests assert only
// REQUIRE_NOTHROW on the setters, so they cannot catch it.
//
// These assert the rendered colour, which is the end of the chain.
// ===========================================================================

namespace {

bool is_excluded_colour(lv_color_t c) {
    // The renderer resolves its palette from the tokens; with no ui_xml loaded
    // in this fixture that is the compiled default, which is the same hue.
    const lv_color_t want = lv_color_hex(helix::gcode::selection::Palette{}.excluded);
    return c.red == want.red && c.green == want.green && c.blue == want.blue;
}

} // namespace

TEST_CASE("an excluded object renders in the excluded colour", "[layer_renderer][exclude]") {
    ParsedGCodeFile gcode = make_test_gcode();
    GCodeLayerRenderer renderer;
    renderer.set_gcode(&gcode);
    renderer.set_excluded_objects({"cube1"});

    const auto& segs = gcode.layers[0].segments;
    REQUIRE(is_excluded_colour(renderer.get_segment_color(segs[0])));       // cube1
    REQUIRE_FALSE(is_excluded_colour(renderer.get_segment_color(segs[1]))); // cube2
    REQUIRE_FALSE(is_excluded_colour(renderer.get_segment_color(segs[2]))); // unnamed
}

TEST_CASE("an exclusion set before the gcode source still classifies",
          "[layer_renderer][exclude]") {
    // Ordering matters in practice: PrinterState can deliver excluded_objects from
    // a Moonraker status update before the viewer has finished parsing the file.
    ParsedGCodeFile gcode = make_test_gcode();
    GCodeLayerRenderer renderer;
    renderer.set_excluded_objects({"cube1"});
    renderer.set_gcode(&gcode);

    REQUIRE(is_excluded_colour(renderer.get_segment_color(gcode.layers[0].segments[0])));
}

TEST_CASE("swapping to a different file with the same object count re-maps",
          "[layer_renderer][exclude]") {
    // The index map is refreshed on a size heuristic plus a force flag at source
    // swap. Without the force, two files with equal object counts would keep the
    // first file's name-to-index mapping, and index 0 would still read as
    // excluded even though this file has no object by that name.
    ParsedGCodeFile first = make_test_gcode();
    GCodeLayerRenderer renderer;
    renderer.set_gcode(&first);
    renderer.set_excluded_objects({"cube1"});
    REQUIRE(is_excluded_colour(renderer.get_segment_color(first.layers[0].segments[0])));

    ParsedGCodeFile second;
    {
        Layer layer;
        layer.z_height = 0.2f;
        layer.bounding_box.expand(glm::vec3(10.0f, 20.0f, 0.2f));
        layer.bounding_box.expand(glm::vec3(50.0f, 80.0f, 0.2f));
        ToolpathSegment a;
        a.start = glm::vec3(10.0f, 20.0f, 0.2f);
        a.end = glm::vec3(50.0f, 20.0f, 0.2f);
        a.is_extrusion = true;
        a.object_name_index = second.intern_object_name("cubeA");
        layer.segments.push_back(a);
        ToolpathSegment b;
        b.start = glm::vec3(10.0f, 80.0f, 0.2f);
        b.end = glm::vec3(50.0f, 80.0f, 0.2f);
        b.is_extrusion = true;
        b.object_name_index = second.intern_object_name("cubeB");
        layer.segments.push_back(b);
        second.layers.push_back(layer);
    }

    renderer.set_gcode(&second);
    // "cube1" is not in this file at all, so nothing here is excluded.
    REQUIRE_FALSE(is_excluded_colour(renderer.get_segment_color(second.layers[0].segments[0])));
    REQUIRE_FALSE(is_excluded_colour(renderer.get_segment_color(second.layers[0].segments[1])));
}

// ===========================================================================
// Selection halo (the white silhouette outline).
//
// A selected object keeps its filament colour and is marked by a white halo
// drawn beneath its strokes: the halo pass runs first at a wider width, then the
// normal pass paints over it, so only the object's outer boundary stays white.
// That is what traces the real toolpath contour rather than the convex hull the
// slicer's EXCLUDE_OBJECT_DEFINE POLYGON gives us.
//
// Asserted by counting white pixels rather than probing coordinates: FRONT view
// is isometric, so where a segment lands is a projection detail, but "white
// appears only when something is selected" is the actual contract.
// ===========================================================================

namespace {

// Drive enough frames to get past WARMUP_FRAMES and run the solid cache path,
// then report how many canvas pixels are near-white and how many are painted.
// Drive frames until the progressive solid cache reports it is complete.
//
// A fixed frame count is NOT deterministic here: layers_per_frame_ is adaptive
// when config_layers_per_frame_ is 0, so under machine load a frame can advance
// the cache by zero layers and a "render 6 frames" harness silently measures a
// half-built cache. That produced painted=0 for one render and painted=141 for
// an identical one. needs_more_frames() is the renderer's own completion signal.
void drive_until_cached(GCodeLayerRenderer& renderer, lv_obj_t* canvas) {
    // The canvas must have a resolved size/position before init_layer, or the
    // first layer of a test gets an unusable clip area and the blit lands
    // nowhere -- which showed up as the FIRST drive in each test case painting
    // zero pixels while later ones in the same case worked.
    lv_obj_update_layout(canvas);

    auto frame = [&]() {
        lv_layer_t layer;
        lv_area_t clip = {0, 0, 199, 199};
        lv_canvas_init_layer(canvas, &layer);
        renderer.render(&layer, &clip);
        lv_canvas_finish_layer(canvas, &layer);
        lv_timer_handler_safe();
    };
    // WARMUP_FRAMES deliberately skips heavy caching; get past it first.
    for (int i = 0; i < 3; ++i) {
        frame();
    }
    int guard = 0;
    while (renderer.needs_more_frames() && guard++ < 500) {
        frame();
    }
    REQUIRE(guard < 500); // cache never completed: harness bug, not a halo bug
    frame();              // final frame blits the completed cache to the canvas
}

struct RenderCounts {
    int white = 0;
    int painted = 0;
};

RenderCounts render_and_count(const std::unordered_set<std::string>& highlighted,
                              ParsedGCodeFile& gcode, uint8_t* buf, lv_obj_t* canvas) {
    GCodeLayerRenderer renderer;
    renderer.set_gcode(&gcode);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    renderer.set_ghost_mode(false);   // no background thread: deterministic
    renderer.set_ssao_enabled(false); // the outline pass would add white of its own
    // Antialiasing is a separate flag now, and it has to be pinned too. A tagged
    // (selected) stroke is always drawn aliased so the alpha tag survives, so
    // leaving AA on here would give the unselected render an AA fringe the
    // selected one does not have, and the footprint comparison below would be
    // measuring that rather than the rim.
    renderer.set_antialias_enabled(false);
    renderer.set_canvas_size(200, 200);
    renderer.set_current_layer(0);
    if (!highlighted.empty()) {
        renderer.set_highlighted_objects(highlighted);
    }

    std::fill(buf, buf + 200 * 200 * 4, uint8_t{0});
    drive_until_cached(renderer, canvas);

    RenderCounts c;
    for (int i = 0; i < 200 * 200; ++i) {
        const uint8_t b = buf[i * 4 + 0];
        const uint8_t g = buf[i * 4 + 1];
        const uint8_t r = buf[i * 4 + 2];
        const uint8_t a = buf[i * 4 + 3];
        if (a == 0) {
            continue;
        }
        // Alpha, not colour: the fixture sets no filament colour, so ordinary
        // segments draw black and an r|g|b test would score them as unpainted.
        ++c.painted;
        if (r >= 240 && g >= 240 && b >= 240) {
            ++c.white;
        }
    }
    return c;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "an unselected plate draws no white pixels",
                 "[layer_renderer][halo]") {
    // Baseline. The default filament colour is teal (0x26A69A) and depth shading
    // only darkens it, so nothing should read as white without a selection. If
    // this fails the white-detection threshold is wrong, not the halo.
    auto gcode = make_test_gcode();
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[200 * 200 * 4];
    lv_canvas_set_buffer(canvas, buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);

    const auto counts = render_and_count({}, gcode, buf, canvas);
    INFO("painted=" << counts.painted << " white=" << counts.white);
    REQUIRE(counts.painted > 0); // the plate did render
    REQUIRE(counts.white == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "selecting an object draws a white rim without growing it",
                 "[layer_renderer][halo]") {
    auto gcode = make_test_gcode();
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[200 * 200 * 4];
    lv_canvas_set_buffer(canvas, buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);

    const auto plain = render_and_count({}, gcode, buf, canvas);
    const auto selected = render_and_count({"cube1"}, gcode, buf, canvas);

    INFO("plain: painted=" << plain.painted << " white=" << plain.white);
    INFO("selected: painted=" << selected.painted << " white=" << selected.white);

    // The rim is the only source of white.
    REQUIRE(selected.white > 0);

    // And the object occupies exactly the same pixels it did unselected. This
    // assertion used to be the other way round - the halo was painted wider than
    // the object and the footprint grew - which is precisely why it flooded: the
    // white it laid down outside the object had to be covered by something, and
    // on a sloped wall nothing ever covered it.
    REQUIRE(selected.painted == plain.painted);

    // A rim is a boundary, so it is a minority of the object's pixels. A flood
    // still satisfies "white > 0".
    REQUIRE(selected.white * 2 < selected.painted);
}

TEST_CASE_METHOD(LVGLTestFixture, "the halo covers only the selected object",
                 "[layer_renderer][halo]") {
    // Selecting both objects must produce strictly more halo than selecting one.
    // A halo keyed on the wrong thing (say, drawn for every segment whenever any
    // selection exists) would give identical counts.
    auto gcode = make_test_gcode();
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[200 * 200 * 4];
    lv_canvas_set_buffer(canvas, buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);

    const auto one = render_and_count({"cube1"}, gcode, buf, canvas);
    const auto both = render_and_count({"cube1", "cube2"}, gcode, buf, canvas);

    INFO("one=" << one.white << " both=" << both.white);
    REQUIRE(one.white > 0);
    REQUIRE(both.white > one.white);
}

TEST_CASE_METHOD(LVGLTestFixture, "clearing the selection removes the halo",
                 "[layer_renderer][halo]") {
    auto gcode = make_test_gcode();
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[200 * 200 * 4];
    lv_canvas_set_buffer(canvas, buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer renderer;
    renderer.set_gcode(&gcode);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    renderer.set_ghost_mode(false);
    renderer.set_ssao_enabled(false);
    renderer.set_canvas_size(200, 200);
    renderer.set_current_layer(0);
    renderer.set_highlighted_objects({"cube1"});

    auto draw = [&]() {
        std::fill(buf, buf + 200 * 200 * 4, uint8_t{0});
        drive_until_cached(renderer, canvas);
        int white = 0;
        for (int i = 0; i < 200 * 200; ++i) {
            if (buf[i * 4 + 3] && buf[i * 4 + 0] >= 240 && buf[i * 4 + 1] >= 240 &&
                buf[i * 4 + 2] >= 240) {
                ++white;
            }
        }
        return white;
    };

    REQUIRE(draw() > 0);
    // Deselecting must invalidate the solid cache, or the halo would persist as a
    // stale cached image -- the exact bug the InvalidationScope split could cause
    // if SolidCache were mishandled.
    renderer.set_highlighted_objects({});
    REQUIRE(draw() == 0);
}

// ===========================================================================
// Scrubbing the layer slider backwards.
//
// The rim is not re-derived per frame: it is stamped into the solid cache as
// pixels, and selection_rim_stamped_ says it is already there. Every path that
// throws those pixels away therefore has to drop the flag with them, which is
// what makes the cache reset one shared operation rather than a rule each branch
// spells out for itself.
// ===========================================================================

namespace {

/// A stack of identical layers, each carrying both named objects, so a scrub in
/// either direction always has the selected object on screen. make_test_gcode()
/// is one layer, which cannot go backwards at all.
ParsedGCodeFile make_stacked_gcode(int layer_count) {
    ParsedGCodeFile gcode;
    const auto cube1 = gcode.intern_object_name("cube1");
    const auto cube2 = gcode.intern_object_name("cube2");

    for (int i = 0; i < layer_count; ++i) {
        Layer layer;
        const float z = 0.2f * static_cast<float>(i + 1);
        layer.z_height = z;

        auto add = [&](float y, auto name_index) {
            ToolpathSegment seg;
            seg.start = glm::vec3(10.0f, y, z);
            seg.end = glm::vec3(50.0f, y, z);
            seg.is_extrusion = true;
            seg.object_name_index = name_index;
            layer.bounding_box.expand(seg.start);
            layer.bounding_box.expand(seg.end);
            gcode.global_bounding_box.expand(seg.start);
            gcode.global_bounding_box.expand(seg.end);
            layer.segments.push_back(seg);
        };
        add(20.0f, cube1);
        add(80.0f, cube2);

        layer.segment_count_extrusion = 2;
        layer.segment_count_travel = 0;
        gcode.layers.push_back(std::move(layer));
    }

    gcode.total_segments = static_cast<size_t>(layer_count) * 2;
    return gcode;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "scrubbing back down the stack keeps the selection rim",
                 "[layer_renderer][halo]") {
    // Layer 0 is the destination that makes the loss permanent rather than
    // self-healing. The rebuild finishes inside a single frame there, so no later
    // frame takes the forward-growth branch — which has its own copy of the reset
    // and would clear a stale flag on the way past. Scrub to the middle of the
    // stack instead and the rim comes back a frame or two later, which is why
    // this went unnoticed.
    auto gcode = make_stacked_gcode(8);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[200 * 200 * 4];
    lv_canvas_set_buffer(canvas, buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer renderer;
    renderer.set_gcode(&gcode);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    renderer.set_ghost_mode(false);   // no background thread: deterministic
    renderer.set_ssao_enabled(false); // the shading pass would add white of its own
    renderer.set_antialias_enabled(false);
    renderer.set_canvas_size(200, 200);
    renderer.set_highlighted_objects({"cube1"});

    auto render_at = [&](int layer) {
        renderer.set_current_layer(layer);
        std::fill(buf, buf + 200 * 200 * 4, uint8_t{0});
        drive_until_cached(renderer, canvas);
        RenderCounts c;
        for (int i = 0; i < 200 * 200; ++i) {
            if (buf[i * 4 + 3] == 0) {
                continue;
            }
            ++c.painted;
            if (buf[i * 4 + 0] >= 240 && buf[i * 4 + 1] >= 240 && buf[i * 4 + 2] >= 240) {
                ++c.white;
            }
        }
        return c;
    };

    const auto top = render_at(7);
    INFO("top: painted=" << top.painted << " white=" << top.white);
    REQUIRE(top.painted > 0);
    REQUIRE(top.white > 0);

    const auto bottom = render_at(0);
    INFO("bottom: painted=" << bottom.painted << " white=" << bottom.white);
    // The plate still drew, so a white count of 0 below means the rim is missing
    // rather than the whole render having gone away.
    REQUIRE(bottom.painted > 0);
    REQUIRE(bottom.white > 0);
}

// =============================================================================
// First-output reveal gate
//
// The viewer fires its one-shot first-frame callback (which hides the slicer
// thumbnail) once the 2D canvas holds real content. The ghost copy is that
// moment: the ghost buffer blit is the first frame with anything on it, and
// the solid cache keeps building visibly on top of it afterwards. Waiting for
// the full build instead left the render drawing behind a mostly-transparent
// OrcaSlicer thumbnail for the whole build window.
// =============================================================================

TEST_CASE("reveal_ready_2d: the ghost copy alone is enough, a pending build alone is not",
          "[layer_renderer][reveal]") {
    // Ghost copied while the solid build still has frames to go: ready.
    REQUIRE(reveal_ready_2d(true, true, false));
    REQUIRE(reveal_ready_2d(true, false, false));
    // Completed build with no ghost involved (non-FRONT views, ghost mode
    // off): ready, same as before parity.
    REQUIRE(reveal_ready_2d(false, false, false));
    // Nothing on the canvas yet — still building, or the ghost thread has not
    // finished: wait.
    REQUIRE_FALSE(reveal_ready_2d(false, true, false));
    REQUIRE_FALSE(reveal_ready_2d(false, true, true));
    REQUIRE_FALSE(reveal_ready_2d(false, false, true));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "2D reveal fires once the ghost is on the canvas while the solid build "
                 "still needs frames",
                 "[layer_renderer][reveal]") {
    auto gcode = make_stacked_gcode(120);

    GCodeLayerRenderer renderer;
    renderer.set_gcode(&gcode);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    // Ghost mode left ON (the default): the ghost build is the first real
    // content, so the reveal gate has to key off it.
    renderer.set_canvas_size(200, 200);
    renderer.set_current_layer(119);

    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t canvas_buf[200 * 200 * 4];
    lv_canvas_set_buffer(canvas, canvas_buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_update_layout(canvas);

    auto frame = [&]() {
        lv_layer_t layer;
        lv_area_t clip = {0, 0, 199, 199};
        lv_canvas_init_layer(canvas, &layer);
        renderer.render(&layer, &clip);
        lv_canvas_finish_layer(canvas, &layer);
        lv_timer_handler_safe();
    };

    // Drive until the ghost has been copied in AND the solid cache has caught
    // up to the top layer. needs_more_frames() covers both, so this loop is
    // the deterministic completion signal.
    int guard = 0;
    while ((renderer.needs_more_frames() || renderer.is_ghost_build_running()) && guard++ < 500) {
        frame();
    }
    REQUIRE(guard < 500); // harness bug, not a reveal bug
    REQUIRE(renderer.has_ghost_output());
    REQUIRE_FALSE(renderer.needs_more_frames());

    // Scrub back down the stack: the solid cache is discarded and rebuilds
    // progressively (layers_per_frame_ never exceeds 100, so layer 100 of 120
    // stays mid-build after one frame), while the ghost stays on the canvas.
    renderer.set_current_layer(100);
    frame();

    // The setup reached the branch this test exists for: real content already
    // copied, progressive build still incomplete. The old rule (wait for
    // needs_more_frames() to clear) reported "no first frame" here.
    REQUIRE(renderer.has_ghost_output());
    REQUIRE(renderer.needs_more_frames());
    REQUIRE_FALSE(renderer.is_ghost_build_running());

    REQUIRE(renderer.has_first_output());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "invalidation discards a built-but-uncopied ghost instead of copying it stale",
                 "[layer_renderer][reveal]") {
    // The ghost worker can finish between draws. If invalidation (a palette
    // change, an occluder move) leaves that finished build pending, the next
    // render's ready-check copies it straight into the cleared buffer and
    // marks the pre-invalidate pixels valid - a cache that is never rebuilt.
    // With the early reveal keying off ghost output, the thumbnail would hide
    // onto that stale frame.
    auto gcode = make_stacked_gcode(120);

    GCodeLayerRenderer renderer;
    renderer.set_gcode(&gcode);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    renderer.set_canvas_size(200, 200);
    renderer.set_current_layer(119);

    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t canvas_buf[200 * 200 * 4];
    lv_canvas_set_buffer(canvas, canvas_buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_update_layout(canvas);

    auto frame = [&]() {
        lv_layer_t layer;
        lv_area_t clip = {0, 0, 199, 199};
        lv_canvas_init_layer(canvas, &layer);
        renderer.render(&layer, &clip);
        lv_canvas_finish_layer(canvas, &layer);
        lv_timer_handler_safe();
    };

    // One render starts the background build. Then wait for the worker to
    // finish WITHOUT rendering again, so the raw buffer sits built-but-uncopied
    // (ghost_thread_ready_ true, ghost_cache_valid_ false).
    frame();
    int guard = 0;
    while (renderer.is_ghost_build_running() && guard++ < 500) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(guard < 500);
    REQUIRE(renderer.is_ghost_build_complete());
    REQUIRE_FALSE(renderer.has_ghost_output()); // built, not yet copied

    // The production trigger from the bug: the palette settles after the load
    // and the detail view pushes tool colors mid-preview. Any non-empty set
    // invalidates the caches unconditionally.
    renderer.set_tool_color_overrides({0xFF0000u});

    frame();

    // The pending pre-invalidate build must be gone: one frame after
    // invalidation there is no ghost output. (Before the fix, this frame
    // copied the stale buffer and reported output.) Whether the replacement
    // build is still running at this instant is timing - a 120-layer ghost
    // finishes in well under a frame - so the invariant is "no stale output",
    // not "mid-build".
    REQUIRE_FALSE(renderer.has_ghost_output());

    // And the rebuild completes normally, leaving real output.
    guard = 0;
    while ((renderer.is_ghost_build_running() || renderer.needs_more_frames()) && guard++ < 500) {
        frame();
    }
    REQUIRE(guard < 500);
    REQUIRE(renderer.has_ghost_output());
}
