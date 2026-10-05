// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Touch input on the gcode viewer: a tap names the object under the finger (or
// reports empty space), a drag is not a tap, and a held press reports through
// the long-press callback instead of the tap one.

#include "ui_gcode_viewer.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/gcode_layer_renderer_test_access.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;
using helix::GcodeViewerRenderMode;
using helix_test::ScopedPointerIndev;

namespace {

constexpr int kViewerX = 50;
constexpr int kViewerY = 40;
constexpr int kViewerSize = 200;

/// Two named objects on one layer, a flat 60 mm apart in Y.
std::unique_ptr<ParsedGCodeFile> make_two_object_file() {
    auto file = std::make_unique<ParsedGCodeFile>();
    file->filename = "two_objects.gcode";

    Layer layer;
    layer.z_height = 0.2f;
    const struct {
        const char* name;
        float y;
    } objects[] = {{"cube1", 20.0f}, {"cube2", 80.0f}};
    for (const auto& o : objects) {
        ToolpathSegment seg;
        seg.start = glm::vec3(10.0f, o.y, 0.2f);
        seg.end = glm::vec3(50.0f, o.y, 0.2f);
        seg.is_extrusion = true;
        seg.object_name_index = file->intern_object_name(o.name);
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

struct Taps {
    std::vector<std::string> taps;
    std::vector<std::string> long_presses;
};

void on_tap(lv_obj_t*, const char* name, void* user_data) {
    static_cast<Taps*>(user_data)->taps.emplace_back(name);
}

void on_long_press(lv_obj_t*, const char* name, void* user_data) {
    static_cast<Taps*>(user_data)->long_presses.emplace_back(name);
}

/// A widget-local pixel the renderer resolves to @p name (or to nothing when
/// @p name is empty), found by scanning so the test does not restate the fit.
std::optional<lv_point_t> find_local_pixel(const GCodeLayerRenderer& renderer,
                                           const std::string& name) {
    for (int y = 0; y < kViewerSize; y += 2) {
        for (int x = 0; x < kViewerSize; x += 2) {
            const auto hit = renderer.pick_object_at(x, y);
            if (name.empty() ? !hit.has_value() : (hit && *hit == name)) {
                return lv_point_t{x, y};
            }
        }
    }
    return std::nullopt;
}

struct Fixture {
    lv_obj_t* viewer = nullptr;
    const GCodeLayerRenderer* renderer = nullptr;
    Taps taps;

    Fixture() {
        viewer = ui_gcode_viewer_create(lv_screen_active());
        REQUIRE(viewer != nullptr);
        lv_obj_set_size(viewer, kViewerSize, kViewerSize);
        lv_obj_set_pos(viewer, kViewerX, kViewerY);
        lv_obj_update_layout(viewer);
        ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
        ui_gcode_viewer_set_object_tap_callback(viewer, on_tap, &taps);
        ui_gcode_viewer_set_object_long_press_callback(viewer, on_long_press, &taps);

        renderer = helix::test_access::gcode_viewer_budget_force_2d(viewer, make_two_object_file());
        REQUIRE(renderer != nullptr);
        auto& mutable_renderer = const_cast<GCodeLayerRenderer&>(*renderer);
        GCodeLayerRendererTestAccess::set_view_mode(mutable_renderer, ViewMode::TOP_DOWN);
        mutable_renderer.auto_fit();
        mutable_renderer.set_current_layer(0);
    }

    ~Fixture() {
        lv_obj_delete(viewer);
    }

    /// Screen coordinates for a widget-local pixel.
    static lv_point_t screen(lv_point_t local) {
        return {local.x + kViewerX, local.y + kViewerY};
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a tap on an object reports that object to the tap callback",
                 "[gcode_viewer][gcode][input][pick]") {
    Fixture f;
    const auto cube1 = find_local_pixel(*f.renderer, "cube1");
    const auto cube2 = find_local_pixel(*f.renderer, "cube2");
    REQUIRE(cube1.has_value());
    REQUIRE(cube2.has_value());

    ScopedPointerIndev indev;
    const lv_point_t p1 = Fixture::screen(*cube1);
    indev.press(p1.x, p1.y);
    indev.release(p1.x, p1.y);

    const lv_point_t p2 = Fixture::screen(*cube2);
    indev.press(p2.x, p2.y);
    indev.release(p2.x, p2.y);

    REQUIRE(f.taps.taps.size() == 2);
    CHECK(f.taps.taps[0] == "cube1");
    CHECK(f.taps.taps[1] == "cube2");
    CHECK(f.taps.long_presses.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a tap on empty space reports an empty name",
                 "[gcode_viewer][gcode][input][pick]") {
    Fixture f;
    const auto empty = find_local_pixel(*f.renderer, "");
    REQUIRE(empty.has_value());

    ScopedPointerIndev indev;
    const lv_point_t p = Fixture::screen(*empty);
    indev.press(p.x, p.y);
    indev.release(p.x, p.y);

    REQUIRE(f.taps.taps.size() == 1);
    CHECK(f.taps.taps[0].empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a press that travels is not a tap",
                 "[gcode_viewer][gcode][input][pick]") {
    Fixture f;
    const auto cube1 = find_local_pixel(*f.renderer, "cube1");
    REQUIRE(cube1.has_value());

    ScopedPointerIndev indev;
    const lv_point_t p = Fixture::screen(*cube1);
    indev.press(p.x, p.y);
    indev.move(p.x + 40, p.y);
    indev.release(p.x + 40, p.y);

    CHECK(f.taps.taps.empty());
    CHECK(f.taps.long_presses.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a held press reports through the long-press callback only",
                 "[gcode_viewer][gcode][input][pick]") {
    Fixture f;
    const auto cube2 = find_local_pixel(*f.renderer, "cube2");
    REQUIRE(cube2.has_value());

    ScopedPointerIndev indev;
    const lv_point_t p = Fixture::screen(*cube2);
    indev.press(p.x, p.y);
    lv_tick_inc(1100);
    lv_timer_handler_safe();
    indev.release(p.x, p.y);

    REQUIRE(f.taps.long_presses.size() == 1);
    CHECK(f.taps.long_presses[0] == "cube2");
    CHECK(f.taps.taps.empty());
}
