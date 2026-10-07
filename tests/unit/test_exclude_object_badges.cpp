// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The numbered badges that identify an exclude-object target. The side list,
// the thumbnail map and the 2D/3D render all show one; they must agree on the
// number and colour for the same object, which compute_object_badges() decides.

#include "ui_exclude_object_badges.h"
#include "ui_exclude_object_map_view.h"
#include "ui_gcode_viewer.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/gcode_layer_renderer_test_access.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "gcode_camera.h"
#ifdef ENABLE_GLES_3D
#include "../test_helpers/gcode_gles_renderer_test_access.h"
#endif
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "lvgl/src/display/lv_display_private.h"
#include "printer_excluded_objects_state.h"
#include "theme_manager.h"

#include <cmath>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrinterExcludedObjectsState;
using helix::gcode::ParsedGCodeFile;
using helix::ui::compute_object_badges;
using helix::ui::ObjectBadge;
using ObjectInfo = PrinterExcludedObjectsState::ObjectInfo;
using helix::gcode::GCodeLayerRendererTestAccess;
using helix_test::ScopedPointerIndev;

namespace {

ObjectInfo klipper_object(const std::string& name, glm::vec2 center, glm::vec2 bmin, glm::vec2 bmax,
                          bool has_center = true, bool has_bbox = true) {
    ObjectInfo o;
    o.name = name;
    o.center = center;
    o.bbox_min = bmin;
    o.bbox_max = bmax;
    o.has_center = has_center;
    o.has_bbox = has_bbox;
    return o;
}

ObjectInfo bare_object(const std::string& name) {
    return klipper_object(name, {}, {}, {}, false, false);
}

void add_parsed_object(ParsedGCodeFile& f, const std::string& name, glm::vec2 center,
                       glm::vec3 bmin, glm::vec3 bmax) {
    helix::gcode::GCodeObject o;
    o.name = name;
    o.center = center;
    o.bounding_box.expand(bmin);
    o.bounding_box.expand(bmax);
    f.objects[name] = o;
}

const ObjectBadge& by_name(const std::vector<ObjectBadge>& badges, const std::string& name) {
    for (const auto& b : badges) {
        if (b.name == name) {
            return b;
        }
    }
    FAIL("no badge for " << name);
    return badges.front();
}

} // namespace

TEST_CASE("Object badges number in defined order, not name order", "[exclude_badges]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.set_defined_objects_with_geometry({
        klipper_object("Zeta", {10, 10}, {0, 0}, {20, 20}),
        klipper_object("Alpha", {50, 50}, {40, 40}, {60, 60}),
        klipper_object("Mid", {90, 90}, {80, 80}, {100, 100}),
    });

    // The parsed map is name-sorted (Alpha, Mid, Zeta); it must not decide order.
    ParsedGCodeFile parsed;
    add_parsed_object(parsed, "Alpha", {50, 50}, {40, 40, 0}, {60, 60, 5});
    add_parsed_object(parsed, "Mid", {90, 90}, {80, 80, 0}, {100, 100, 5});
    add_parsed_object(parsed, "Zeta", {10, 10}, {0, 0, 0}, {20, 20, 5});

    for (const ParsedGCodeFile* p : {static_cast<const ParsedGCodeFile*>(nullptr),
                                     static_cast<const ParsedGCodeFile*>(&parsed)}) {
        const auto badges = compute_object_badges(state, p);
        REQUIRE(badges.size() == 3);
        CHECK(badges[0].name == "Zeta");
        CHECK(badges[1].name == "Alpha");
        CHECK(badges[2].name == "Mid");
        for (int i = 0; i < 3; ++i) {
            CHECK(badges[i].defined_index == i);
            CHECK(badges[i].number == std::to_string(i + 1));
        }
    }
    state.deinit_subjects();
}

TEST_CASE("Object badges carry the excluded and current flags", "[exclude_badges]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.set_defined_objects_with_geometry({
        klipper_object("A", {10, 10}, {0, 0}, {20, 20}),
        klipper_object("B", {50, 50}, {40, 40}, {60, 60}),
        klipper_object("C", {90, 90}, {80, 80}, {100, 100}),
    });
    state.set_excluded_objects({"B"});
    state.set_current_object("C");

    const auto badges = compute_object_badges(state, nullptr);
    REQUIRE(badges.size() == 3);
    CHECK_FALSE(badges[0].excluded);
    CHECK_FALSE(badges[0].current);
    CHECK(badges[1].excluded);
    CHECK_FALSE(badges[1].current);
    CHECK_FALSE(badges[2].excluded);
    CHECK(badges[2].current);
    state.deinit_subjects();
}

TEST_CASE("Object badge anchor falls back center -> parsed center -> bbox", "[exclude_badges]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.set_defined_objects_with_geometry({
        // Klipper CENTER wins over everything else.
        klipper_object("klipper_center", {11, 12}, {0, 0}, {40, 40}),
        // No Klipper CENTER: the parsed file's CENTER.
        klipper_object("parsed_center", {}, {0, 0}, {40, 40}, false, true),
        // Neither CENTER: Klipper's bbox centre.
        klipper_object("klipper_bbox", {}, {100, 200}, {120, 240}, false, true),
        // Nothing from Klipper, no parsed CENTER: the toolpath bbox centre.
        bare_object("parsed_bbox"),
        // Nothing anywhere: still numbered, no anchor.
        bare_object("nothing"),
    });

    ParsedGCodeFile parsed;
    add_parsed_object(parsed, "klipper_center", {70, 70}, {60, 60, 0}, {80, 80, 9});
    add_parsed_object(parsed, "parsed_center", {33, 34}, {0, 0, 0}, {40, 40, 3});
    add_parsed_object(parsed, "parsed_bbox", {0, 0}, {10, 20, 0}, {30, 60, 7});

    const auto badges = compute_object_badges(state, &parsed);
    REQUIRE(badges.size() == 5);

    const auto& kc = by_name(badges, "klipper_center");
    REQUIRE(kc.has_anchor);
    CHECK(kc.anchor == glm::vec2(11, 12));
    REQUIRE(kc.top_z.has_value());
    CHECK(*kc.top_z == Catch::Approx(9.0f));

    const auto& pc = by_name(badges, "parsed_center");
    REQUIRE(pc.has_anchor);
    CHECK(pc.anchor == glm::vec2(33, 34));

    const auto& kb = by_name(badges, "klipper_bbox");
    REQUIRE(kb.has_anchor);
    CHECK(kb.anchor == glm::vec2(110, 220));
    CHECK_FALSE(kb.top_z.has_value());

    const auto& pb = by_name(badges, "parsed_bbox");
    REQUIRE(pb.has_anchor);
    CHECK(pb.anchor == glm::vec2(20, 40));
    CHECK(*pb.top_z == Catch::Approx(7.0f));

    const auto& none = by_name(badges, "nothing");
    CHECK_FALSE(none.has_anchor);
    CHECK(none.defined_index == 4);
    CHECK(none.number == "5");

    // Without a parsed file the parsed-only objects lose their anchor, but
    // nobody's number moves.
    const auto unparsed = compute_object_badges(state, nullptr);
    CHECK(by_name(unparsed, "parsed_center").anchor == glm::vec2(20, 20)); // Klipper bbox
    CHECK_FALSE(by_name(unparsed, "parsed_bbox").has_anchor);
    CHECK(by_name(unparsed, "nothing").number == "5");
    state.deinit_subjects();
}

TEST_CASE("Object badges with names only (no geometry)", "[exclude_badges]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.set_defined_objects({"one", "two"});

    const auto badges = compute_object_badges(state, nullptr);
    REQUIRE(badges.size() == 2);
    CHECK_FALSE(badges[0].has_anchor);
    CHECK(badges[1].number == "2");
    state.deinit_subjects();
}

// ============================================================================
// Thumbnail map: badge numbers and colours key on the defined index
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture,
                 "Map view badge keeps defined numbering when an object has no bbox",
                 "[exclude_badges][exclude_map]") {
    REQUIRE(register_component("components/exclude_object_map"));
    state().excluded_objects_state().set_defined_objects_with_geometry({
        klipper_object("First", {35, 35}, {20, 20}, {50, 50}),
        bare_object("NoBox"),
        klipper_object("Third", {115, 115}, {100, 100}, {130, 130}),
    });

    helix::ui::ExcludeObjectMapView view;
    view.create(test_screen(), &state().excluded_objects_state(), 235.0f, 235.0f, {},
                helix::ui::ExcludeTapMode::ExcludeOnly, nullptr);
    REQUIRE(view.is_active());
    process_lvgl(30);

    // "Third" is defined third: its rect, disc number and colour all say so,
    // though only two objects have a rect.
    CHECK(lv_obj_find_by_name(view.root(), "obj_rect_1") == nullptr);
    lv_obj_t* rect = lv_obj_find_by_name(view.root(), "obj_rect_2");
    REQUIRE(rect);
    REQUIRE(lv_obj_get_child_count(rect) == 1);
    lv_obj_t* disc = lv_obj_get_child(rect, 0);
    REQUIRE(lv_obj_get_child_count(disc) == 1);
    CHECK(std::string(lv_label_get_text(lv_obj_get_child(disc, 0))) == "3");
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(disc, LV_PART_MAIN),
                      helix::ui::object_badge_color(2)));

    view.destroy();
    process_lvgl(30);
}

// ============================================================================
// 2D render: a badge projected through the live transform lands on its object
// ============================================================================

namespace {

// Two hollow squares, 4 layers tall each, far apart on the bed.
ParsedGCodeFile make_two_squares() {
    ParsedGCodeFile f;
    struct Sq {
        const char* name;
        float x0, y0;
    };
    const Sq squares[] = {{"Left", 20.0f, 20.0f}, {"Right", 120.0f, 60.0f}};
    constexpr float kSide = 30.0f;
    constexpr int kLayers = 4;
    for (int li = 0; li < kLayers; ++li) {
        helix::gcode::Layer layer;
        layer.z_height = 2.0f * static_cast<float>(li + 1);
        for (const auto& s : squares) {
            const int16_t idx = f.intern_object_name(s.name);
            const glm::vec3 c[4] = {{s.x0, s.y0, layer.z_height},
                                    {s.x0 + kSide, s.y0, layer.z_height},
                                    {s.x0 + kSide, s.y0 + kSide, layer.z_height},
                                    {s.x0, s.y0 + kSide, layer.z_height}};
            for (int k = 0; k < 4; ++k) {
                helix::gcode::ToolpathSegment seg;
                seg.start = c[k];
                seg.end = c[(k + 1) % 4];
                seg.is_extrusion = true;
                seg.object_name_index = idx;
                layer.segments.push_back(seg);
                layer.bounding_box.expand(seg.start);
                f.global_bounding_box.expand(seg.start);
            }
            ++layer.segment_count_extrusion;
        }
        f.layers.push_back(std::move(layer));
    }
    for (const auto& s : squares) {
        add_parsed_object(f, s.name, {s.x0 + kSide / 2, s.y0 + kSide / 2}, {s.x0, s.y0, 2.0f},
                          {s.x0 + kSide, s.y0 + kSide, 2.0f * kLayers});
    }
    f.total_segments = 4 * 2 * kLayers;
    return f;
}

} // namespace

// ============================================================================
// The viewer: the real draw pass, the pick path, and the 3D image transform
// ============================================================================

namespace {

constexpr int kViewerX = 50;
constexpr int kViewerY = 40;
constexpr int kViewerW = 300;
constexpr int kViewerH = 240;

struct Taps {
    std::vector<std::string> names;
};

void on_tap(lv_obj_t*, const char* name, void* user_data) {
    static_cast<Taps*>(user_data)->names.emplace_back(name ? name : "");
}

/// A 2D viewer drawing make_two_squares() through the real draw callback.
struct BadgeViewer {
    lv_obj_t* viewer = nullptr;
    helix::gcode::GCodeLayerRenderer* renderer = nullptr;
    Taps taps;

    BadgeViewer() {
        viewer = ui_gcode_viewer_create(lv_screen_active());
        REQUIRE(viewer != nullptr);
        lv_obj_set_size(viewer, kViewerW, kViewerH);
        lv_obj_set_pos(viewer, kViewerX, kViewerY);
        lv_obj_update_layout(viewer);
        ui_gcode_viewer_set_render_mode(viewer, helix::GcodeViewerRenderMode::Layer2D);
        ui_gcode_viewer_set_object_tap_callback(viewer, on_tap, &taps);
        renderer = helix::test_access::gcode_viewer_show_2d(
            viewer, std::make_unique<ParsedGCodeFile>(make_two_squares()));
        REQUIRE(renderer != nullptr);
    }
    ~BadgeViewer() {
        lv_obj_delete(viewer);
    }

    void draw() {
        lv_obj_invalidate(viewer);
        lv_refr_now(nullptr);
    }

    std::vector<helix::test_access::GcodeViewerDrawnBadge> drawn() const {
        return helix::test_access::gcode_viewer_drawn_badges(viewer);
    }

    void tap_local(glm::vec2 local) {
        ScopedPointerIndev indev;
        const int x = kViewerX + static_cast<int>(std::lround(local.x));
        const int y = kViewerY + static_cast<int>(std::lround(local.y));
        indev.press(x, y);
        indev.release(x, y);
    }
};

ObjectBadge make_badge(int index, const std::string& name, glm::vec2 anchor,
                       std::optional<float> top_z = std::nullopt, bool excluded = false) {
    ObjectBadge b;
    b.defined_index = index;
    b.name = name;
    b.number = std::to_string(index + 1);
    b.has_anchor = true;
    b.anchor = anchor;
    b.top_z = top_z;
    b.excluded = excluded;
    return b;
}

// Squares from make_two_squares(): "Left" spans (20..50, 20..50), "Right"
// spans (120..150, 60..90), layers at z 2, 4, 6, 8.
const glm::vec2 kLeftCenter{35.0f, 35.0f};
const glm::vec2 kRightCenter{135.0f, 75.0f};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "2D badges sit on the drawn top of their object",
                 "[exclude_badges][gcode_viewer]") {
    BadgeViewer v;
    ui_gcode_viewer_set_print_progress(v.viewer, 1); // drawn top: z 4
    ui_gcode_viewer_set_object_badges(v.viewer,
                                      {
                                          // Taller than what is drawn: rides the current layer.
                                          make_badge(0, "Left", kLeftCenter, 8.0f),
                                          // Already finished below it: sits on its own top.
                                          make_badge(1, "Right", kRightCenter, 2.0f),
                                      });
    v.draw();

    const auto drawn = v.drawn();
    REQUIRE(drawn.size() == 2);
    REQUIRE(v.renderer->current_layer_z() == Catch::Approx(4.0f));
    const glm::vec2 left_at_layer(v.renderer->project_to_screen(35.0f, 35.0f, 4.0f));
    const glm::vec2 left_at_top(v.renderer->project_to_screen(35.0f, 35.0f, 8.0f));
    const glm::vec2 right_at_top(v.renderer->project_to_screen(135.0f, 75.0f, 2.0f));
    REQUIRE(left_at_layer != left_at_top); // the view shows Z, so the choice is visible
    CHECK(drawn[0].name == "Left");
    CHECK(drawn[0].center == left_at_layer);
    CHECK(drawn[1].name == "Right");
    CHECK(drawn[1].center == right_at_top);

    // And the badge lands on its object: a tap there picks it even without
    // the badge (geometry alone), which is what makes the anchor right.
    for (const auto& d : drawn) {
        const auto picked = v.renderer->pick_object_at(static_cast<int>(std::lround(d.center.x)),
                                                       static_cast<int>(std::lround(d.center.y)));
        REQUIRE(picked.has_value());
        CHECK(*picked == d.name);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "A badge outside the widget is not drawn",
                 "[exclude_badges][gcode_viewer]") {
    BadgeViewer v;
    ui_gcode_viewer_set_object_badges(v.viewer, {
                                                    make_badge(0, "Left", kLeftCenter),
                                                    make_badge(1, "Far", {5000.0f, 5000.0f}),
                                                });
    v.draw();
    const auto drawn = v.drawn();
    REQUIRE(drawn.size() == 1);
    CHECK(drawn[0].name == "Left");
}

TEST_CASE_METHOD(LVGLTestFixture, "A tap on a badge picks the badge's object over the geometry",
                 "[exclude_badges][gcode_viewer][pick]") {
    BadgeViewer v;
    // "Right"'s badge drawn over Left's geometry: the badge must win.
    ui_gcode_viewer_set_object_badges(v.viewer, {make_badge(1, "Right", kLeftCenter)});
    v.draw();
    REQUIRE(v.drawn().size() == 1);
    v.tap_local(v.drawn()[0].center);
    REQUIRE(v.taps.names.size() == 1);
    CHECK(v.taps.names[0] == "Right");
}

TEST_CASE_METHOD(LVGLTestFixture, "Overlapping badges: the one drawn on top is picked",
                 "[exclude_badges][gcode_viewer][pick]") {
    BadgeViewer v;
    ui_gcode_viewer_set_object_badges(v.viewer, {
                                                    make_badge(0, "Under", kLeftCenter),
                                                    make_badge(1, "Over", kLeftCenter),
                                                });
    v.draw();
    REQUIRE(v.drawn().size() == 2);
    v.tap_local(v.drawn()[1].center);
    REQUIRE(v.taps.names.size() == 1);
    CHECK(v.taps.names[0] == "Over");
}

TEST_CASE_METHOD(LVGLTestFixture, "An excluded badge is drawn but the tap goes to the geometry",
                 "[exclude_badges][gcode_viewer][pick]") {
    BadgeViewer v;
    ui_gcode_viewer_set_object_badges(
        v.viewer, {make_badge(1, "Right", kLeftCenter, std::nullopt, /*excluded=*/true)});
    v.draw();
    REQUIRE(v.drawn().size() == 1);
    v.tap_local(v.drawn()[0].center);
    REQUIRE(v.taps.names.size() == 1);
    CHECK(v.taps.names[0] == "Left");
}

TEST_CASE_METHOD(LVGLTestFixture, "With excluded badges pickable, a tap on one picks its object",
                 "[exclude_badges][gcode_viewer][pick][pre_start_exclude]") {
    BadgeViewer v;
    ui_gcode_viewer_set_excluded_badges_pickable(v.viewer, true);
    ui_gcode_viewer_set_object_badges(
        v.viewer, {make_badge(1, "Right", kLeftCenter, std::nullopt, /*excluded=*/true)});
    v.draw();
    REQUIRE(v.drawn().size() == 1);
    v.tap_local(v.drawn()[0].center);
    REQUIRE(v.taps.names.size() == 1);
    CHECK(v.taps.names[0] == "Right");
}

TEST_CASE_METHOD(LVGLTestFixture, "Clearing the badges removes them and their pick targets",
                 "[exclude_badges][gcode_viewer][pick]") {
    BadgeViewer v;
    ui_gcode_viewer_set_object_badges(v.viewer, {make_badge(1, "Right", kLeftCenter)});
    v.draw();
    const auto drawn = v.drawn();
    REQUIRE(drawn.size() == 1);

    ui_gcode_viewer_set_object_badges(v.viewer, {});
    CHECK(v.drawn().empty()); // before any redraw
    v.tap_local(drawn[0].center);
    REQUIRE(v.taps.names.size() == 1);
    CHECK(v.taps.names[0] == "Left");
    v.draw();
    CHECK(v.drawn().empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "A frame that draws nothing leaves no stale pick targets",
                 "[exclude_badges][gcode_viewer][pick]") {
    BadgeViewer v;
    ui_gcode_viewer_set_object_badges(v.viewer, {make_badge(1, "Right", kLeftCenter)});
    v.draw();
    const auto drawn = v.drawn();
    REQUIRE(drawn.size() == 1);

    ui_gcode_viewer_set_paused(v.viewer, true);
    v.draw();
    CHECK(v.drawn().empty());
    v.tap_local(drawn[0].center);
    REQUIRE(v.taps.names.size() == 1);
    CHECK(v.taps.names[0] == "Left");
}

TEST_CASE_METHOD(LVGLTestFixture, "Setting the same badges again does not invalidate the viewer",
                 "[exclude_badges][gcode_viewer]") {
    BadgeViewer v;
    const std::vector<ObjectBadge> badges = {make_badge(0, "Left", kLeftCenter)};
    ui_gcode_viewer_set_object_badges(v.viewer, badges);
    v.draw();
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp->inv_p == 0);

    ui_gcode_viewer_set_object_badges(v.viewer, badges);
    CHECK(disp->inv_p == 0);

    auto changed = badges;
    changed[0].excluded = true;
    ui_gcode_viewer_set_object_badges(v.viewer, changed);
    CHECK(disp->inv_p > 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Excluding a selected object drops its selection",
                 "[exclude_badges][gcode_viewer]") {
    BadgeViewer v;
    ui_gcode_viewer_set_highlighted_objects(v.viewer, {"Left", "Right"});
    REQUIRE(GCodeLayerRendererTestAccess::selection(*v.renderer).highlighted().count("Left") == 1);

    ui_gcode_viewer_set_excluded_objects(v.viewer, {"Left"});
    const auto& sel = GCodeLayerRendererTestAccess::selection(*v.renderer);
    CHECK(sel.highlighted().count("Left") == 0);
    CHECK(sel.highlighted().count("Right") == 1);
}

#ifdef ENABLE_GLES_3D
TEST_CASE("3D badges project through the image on screen, not the live camera",
          "[exclude_badges][gles]") {
    helix::gcode::GCodeGLESRenderer renderer;
    renderer.set_viewport_size(400, 300);
    helix::gcode::GCodeCamera camera;
    camera.set_viewport_size(400, 300);
    helix::gcode::AABB box;
    box.expand({0.0f, 0.0f, 0.0f});
    box.expand({100.0f, 100.0f, 20.0f});
    camera.fit_to_bounds(box);

    const glm::vec3 centre = box.center();
    const glm::vec3 corner{100.0f, 0.0f, 20.0f};
    CHECK_FALSE(renderer.project_to_shown_image(centre).has_value()); // no image yet

    helix::gcode::GCodeGLESRendererTestAccess::show_frame(renderer, camera, 400, 300);
    const auto c = renderer.project_to_shown_image(centre);
    REQUIRE(c.has_value());
    // Fitted to the box, the model centre lands near the middle of the image.
    CHECK(c->x == Catch::Approx(200.0f).margin(40.0f));
    CHECK(c->y == Catch::Approx(150.0f).margin(60.0f));
    const auto before = renderer.project_to_shown_image(corner);
    REQUIRE(before.has_value());

    // The camera moves and a refinement starts, but the image on screen is
    // still the old frame until the new one is finished and blitted.
    camera.rotate(60.0f, 0.0f);
    helix::gcode::GCodeGLESRendererTestAccess::start_frame(renderer, camera);
    const auto lagging = renderer.project_to_shown_image(corner);
    REQUIRE(lagging.has_value());
    CHECK(*lagging == *before);

    // A new frame lands: the badges move with it.
    helix::gcode::GCodeGLESRendererTestAccess::show_frame(renderer, camera, 400, 300);
    const auto after = renderer.project_to_shown_image(corner);
    REQUIRE(after.has_value());
    CHECK(glm::distance(*after, *before) > 5.0f);

    renderer.clear_cached_frame();
    CHECK_FALSE(renderer.project_to_shown_image(corner).has_value());
}

TEST_CASE("Releasing 3D geometry drops the shown image's transform", "[exclude_badges][gles]") {
    helix::gcode::GCodeGLESRenderer renderer;
    renderer.set_viewport_size(400, 300);
    helix::gcode::GCodeCamera camera;
    camera.set_viewport_size(400, 300);
    helix::gcode::AABB box;
    box.expand({0.0f, 0.0f, 0.0f});
    box.expand({100.0f, 100.0f, 20.0f});
    camera.fit_to_bounds(box);
    helix::gcode::GCodeGLESRendererTestAccess::show_frame(renderer, camera, 400, 300);
    REQUIRE(renderer.project_to_shown_image(box.center()).has_value());

    // A reload releases the old file: its image must not place the new file's badges.
    renderer.release_geometry();
    CHECK_FALSE(renderer.project_to_shown_image(box.center()).has_value());
}
#endif

TEST_CASE_METHOD(LVGLUITestFixture, "A theme switch re-resolves the badge colours",
                 "[exclude_badges][gcode_viewer]") {
    BadgeViewer v;
    std::vector<ObjectBadge> badges;
    for (int i = 0; i < 8; ++i) {
        badges.push_back(make_badge(i, "obj_" + std::to_string(i), kLeftCenter));
    }
    ui_gcode_viewer_set_object_badges(v.viewer, badges);
    v.draw();
    const auto before = helix::test_access::gcode_viewer_badge_texts(v.viewer);
    REQUIRE(before.size() == 8);

    theme_manager_toggle_dark_mode();
    v.draw(); // same badge list, new theme
    const auto fills = helix::test_access::gcode_viewer_badge_fills(v.viewer);
    const auto after = helix::test_access::gcode_viewer_badge_texts(v.viewer);
    REQUIRE(after.size() == 8);
    bool any_changed = false;
    for (size_t i = 0; i < 8; ++i) {
        INFO("badge " << i);
        CHECK(lv_color_eq(fills[i], helix::ui::object_badge_color(static_cast<int>(i))));
        CHECK(lv_color_eq(after[i], helix::ui::object_badge_text_color(fills[i])));
        any_changed = any_changed || !lv_color_eq(after[i], before[i]);
    }
    theme_manager_toggle_dark_mode();
    // The theme moved at least one number colour, or this test proves nothing.
    REQUIRE(any_changed);
}
