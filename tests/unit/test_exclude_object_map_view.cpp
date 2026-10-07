// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_exclude_object_map_view.cpp
 * @brief Tests for the exclude-object overhead map view feature
 *
 * Task 1: Verifies that object color palette tokens (object_color_1 through
 * object_color_8) are registered and return non-black colors after theme init.
 *
 * Uses XMLTestFixture because theme_manager_init() must have been called for
 * lv_xml_register_const tokens to be accessible via theme_manager_get_color().
 */

#include "../test_fixtures.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Object color palette token tests
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "object_color_1 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_1");
    // #7c8aff — periwinkle blue: red=0x7c, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_2 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_2");
    // #4ecdc4 — teal: green=0xcd, non-black
    REQUIRE(color.green != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_3 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_3");
    // #f9c74f — golden yellow: red=0xf9, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_4 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_4");
    // #a78bfa — soft purple: red=0xa7, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_5 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_5");
    // #f472b6 — pink: red=0xf4, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_6 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_6");
    // #fb923c — orange: red=0xfb, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_7 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_7");
    // #34d399 — emerald: green=0xd3, non-black
    REQUIRE(color.green != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_8 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_8");
    // #60a5fa — sky blue: blue=0xfa, non-black
    REQUIRE(color.blue != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "all 8 object color tokens are registered",
                 "[exclude_map][tokens]") {
    // Verify all 8 tokens return non-zero colors (not black fallback)
    lv_color_t black = lv_color_hex(0x000000);

    for (int i = 1; i <= 8; ++i) {
        char token[32];
        snprintf(token, sizeof(token), "object_color_%d", i);
        lv_color_t color = theme_manager_get_color(token);

        // At least one channel must be non-zero to distinguish from black fallback
        bool is_non_black =
            (color.red != black.red) || (color.green != black.green) || (color.blue != black.blue);
        INFO("Token " << token << " returned black (missing registration)");
        REQUIRE(is_non_black);
    }
}

// ============================================================================
// Coordinate mapping tests
// ============================================================================

#include "ui_exclude_object_map_view.h"

using helix::ui::ExcludeObjectMapView;
using helix::ui::ExcludeTapMode;

// ============================================================================
// Coordinate mapping tests
// ============================================================================

TEST_CASE("Coordinate mapping: mm to pixels", "[exclude_map][coords]") {
    SECTION("square bed in square viewport") {
        auto mapper = ExcludeObjectMapView::CoordMapper(235.0f, 235.0f, 400, 400);
        auto [px, py] = mapper.mm_to_px(0.0f, 0.0f);
        REQUIRE_THAT(px, Catch::Matchers::WithinAbs(0.0f, 0.5f));
        REQUIRE_THAT(py, Catch::Matchers::WithinAbs(400.0f, 0.5f));

        auto [cx, cy] = mapper.mm_to_px(117.5f, 117.5f);
        REQUIRE_THAT(cx, Catch::Matchers::WithinAbs(200.0f, 0.5f));
        REQUIRE_THAT(cy, Catch::Matchers::WithinAbs(200.0f, 0.5f));
    }

    SECTION("rectangular bed — width-limited") {
        auto mapper = ExcludeObjectMapView::CoordMapper(350.0f, 200.0f, 400, 400);
        auto [cx, cy] = mapper.mm_to_px(175.0f, 100.0f);
        REQUIRE_THAT(cx, Catch::Matchers::WithinAbs(200.0f, 0.5f));
        REQUIRE_THAT(cy, Catch::Matchers::WithinAbs(200.0f, 0.5f));
    }

    SECTION("bbox_to_rect") {
        auto mapper = ExcludeObjectMapView::CoordMapper(235.0f, 235.0f, 470, 470);
        auto rect = mapper.bbox_to_rect({10.0f, 10.0f}, {60.0f, 40.0f});
        REQUIRE_THAT(rect.w, Catch::Matchers::WithinAbs(100.0f, 0.5f));
        REQUIRE_THAT(rect.h, Catch::Matchers::WithinAbs(60.0f, 0.5f));
    }

    SECTION("minimum rect size enforced") {
        auto mapper = ExcludeObjectMapView::CoordMapper(350.0f, 350.0f, 400, 400);
        auto rect = mapper.bbox_to_rect({100.0f, 100.0f}, {101.0f, 101.0f});
        REQUIRE(rect.w >= 28.0f);
        REQUIRE(rect.h >= 28.0f);
    }
}

// ============================================================================
// Bed size fallback: derive bed extents from object bounding boxes
// ============================================================================

TEST_CASE("Bed size fallback from object extents", "[exclude_map][bed_size]") {
    // When bed dimensions are unknown (0x0), the caller derives them from the
    // union of all object bounding boxes plus a 10% padding margin.
    // Objects span 20-170mm x 30-140mm → effective extents: 170*1.1 x 140*1.1
    auto mapper = ExcludeObjectMapView::CoordMapper(170.0f * 1.1f, 140.0f * 1.1f, 400, 400);
    // Center of objects area (85, 70) should map near viewport center
    auto [cx, cy] = mapper.mm_to_px(85.0f, 70.0f);
    REQUIRE(cx > 150.0f);
    REQUIRE(cx < 250.0f);
    REQUIRE(cy > 150.0f);
    REQUIRE(cy < 250.0f);
}

// ============================================================================
// CoordMapper edge cases
// ============================================================================

TEST_CASE("CoordMapper edge cases", "[exclude_map][coords]") {
    SECTION("very narrow bed (portrait)") {
        // Height-limited: scale = 400/300 = 1.333
        auto mapper = ExcludeObjectMapView::CoordMapper(100.0f, 300.0f, 400, 400);
        auto [cx, cy] = mapper.mm_to_px(50.0f, 150.0f);
        REQUIRE_THAT(cx, Catch::Matchers::WithinAbs(200.0f, 1.0f));
        REQUIRE_THAT(cy, Catch::Matchers::WithinAbs(200.0f, 1.0f));
    }

    SECTION("overlapping minimum-size rects") {
        // Two tiny adjacent objects (1mm each) on a 235mm square bed.
        // Both must be expanded to at least MIN_TOUCH_TARGET_PX.
        auto mapper = ExcludeObjectMapView::CoordMapper(235.0f, 235.0f, 400, 400);
        auto r1 = mapper.bbox_to_rect({100.0f, 100.0f}, {101.0f, 101.0f});
        auto r2 = mapper.bbox_to_rect({102.0f, 100.0f}, {103.0f, 101.0f});
        REQUIRE(r1.w >= ExcludeObjectMapView::MIN_TOUCH_TARGET_PX);
        REQUIRE(r2.w >= ExcludeObjectMapView::MIN_TOUCH_TARGET_PX);
    }
}

// ============================================================================
// create()/destroy() lifecycle — canvas-buffer ordering + deferred deletion
// ============================================================================
//
// Regression for the LVGL event-list-corruption bug: destroy() must NOT call a
// bare synchronous lv_obj_delete(root_) (it can run inside a UpdateQueue
// process_pending batch via the memory-pressure reclaim chain, where a second
// sync deletion corrupts LVGL's global event linked list). It defers deletion
// via safe_delete_deferred() instead. The canvas widget (a child of root_)
// survives until the async delete tick, so destroy() must sever the canvas's
// image-source reference BEFORE freeing the canvas draw buffer — otherwise the
// still-live canvas would point at freed memory and a redraw in that window is
// a use-after-free. These tests exercise the full create + destroy roundtrip
// with seeded object geometry so the canvas path runs, then pump LVGL so the
// async delete completes. They crash/assert if the ordering regresses.

#include "ui_update_queue.h"

#include "printer_excluded_objects_state.h"

namespace {
// Seed a handful of objects with bounding boxes so create() allocates the
// canvas draw buffer and draws first-layer outlines into it.
void seed_objects(helix::PrinterExcludedObjectsState& st) {
    using ObjectInfo = helix::PrinterExcludedObjectsState::ObjectInfo;
    std::vector<ObjectInfo> objs;
    for (int i = 0; i < 3; ++i) {
        ObjectInfo o;
        o.name = "OBJ_" + std::to_string(i);
        float base = 20.0f + static_cast<float>(i) * 40.0f;
        o.bbox_min = {base, base};
        o.bbox_max = {base + 30.0f, base + 30.0f};
        o.center = {base + 15.0f, base + 15.0f};
        o.has_bbox = true;
        o.has_center = true;
        // Triangle polygon so draw_first_layer_outlines renders to the canvas.
        o.polygon = {{base, base}, {base + 30.0f, base}, {base + 15.0f, base + 30.0f}};
        objs.push_back(std::move(o));
    }
    st.set_defined_objects_with_geometry(objs);
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ExcludeObjectMapView create/destroy roundtrip is crash-free",
                 "[exclude_map][lifecycle]") {
    REQUIRE(register_component("components/exclude_object_map"));
    seed_objects(state().excluded_objects_state());

    auto view = std::make_unique<ExcludeObjectMapView>();
    view->create(test_screen(), &state().excluded_objects_state(), 235.0f, 235.0f, {},
                 ExcludeTapMode::ExcludeOnly, nullptr);
    REQUIRE(view->is_active());

    // Let layout settle so the canvas buffer is allocated and drawn.
    process_lvgl(50);

    view->destroy();
    // After destroy(), root_ is deferred for async deletion; the view reports
    // inactive immediately so callers can't re-enter the live tree.
    REQUIRE_FALSE(view->is_active());

    // Pump LVGL so the lv_obj_delete_async tick actually deletes the widget
    // subtree (including the now-srcless canvas). If the canvas still pointed
    // at the freed draw buffer, a redraw during this window would crash.
    process_lvgl(50);

    // Idempotent: a second destroy() on an already-destroyed view is a no-op.
    view->destroy();
    REQUIRE_FALSE(view->is_active());

    // Destructor runs here — it must not double-free or touch freed widgets.
    view.reset();
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ExcludeObjectMapView destructor cleans up without explicit destroy",
                 "[exclude_map][lifecycle]") {
    REQUIRE(register_component("components/exclude_object_map"));
    seed_objects(state().excluded_objects_state());

    const uint32_t baseline_children = lv_obj_get_child_count(test_screen());

    {
        ExcludeObjectMapView view;
        view.create(test_screen(), &state().excluded_objects_state(), 200.0f, 200.0f, {},
                    ExcludeTapMode::ExcludeOnly, nullptr);
        REQUIRE(view.is_active());
        REQUIRE(lv_obj_get_child_count(test_screen()) > baseline_children);
        process_lvgl(30);
        // No explicit destroy(): the destructor must invoke destroy() and tear
        // down the canvas buffer + widget tree safely.
    }
    process_lvgl(50);

    // The heap canvas_buf_ leaks invisibly, but the widget subtree does not: a
    // destructor that skips destroy() leaves root_ parented to the screen. Child
    // count back at baseline is the observable proof that destroy() ran and its
    // deferred deletion completed - and destroy() is the only path that frees
    // the draw buffer too.
    REQUIRE(lv_obj_get_child_count(test_screen()) == baseline_children);
}

class ExcludeObjectMapViewTestAccess {
  public:
    static lv_obj_t* rect_for(const ExcludeObjectMapView& view, const std::string& name) {
        for (const auto& r : view.object_rects_) {
            if (r.name == name) {
                return r.rect;
            }
        }
        return nullptr;
    }
    /// Strongest outline alpha drawn within 2px of the bed point (x_mm, y_mm).
    static int outline_alpha_near(const ExcludeObjectMapView& view, float x_mm, float y_mm) {
        lv_obj_t* canvas = view.canvas_;
        if (!canvas || !view.mapper_) {
            return -1;
        }
        const auto [px, py] = view.mapper_->mm_to_px(x_mm, y_mm);
        const int32_t w = lv_obj_get_width(canvas);
        const int32_t h = lv_obj_get_height(canvas);
        int best = 0;
        for (int32_t dy = -2; dy <= 2; ++dy) {
            for (int32_t dx = -2; dx <= 2; ++dx) {
                const int32_t x = static_cast<int32_t>(px) + dx;
                const int32_t y = static_cast<int32_t>(py) + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) {
                    continue;
                }
                best = std::max(best, static_cast<int>(lv_canvas_get_px(canvas, x, y).alpha));
            }
        }
        return best;
    }
};

TEST_CASE_METHOD(
    XMLTestFixture,
    "Map view in toggle mode hands taps to the callback and keeps a picked rect tappable",
    "[exclude_map][pre_start_exclude]") {
    REQUIRE(register_component("components/exclude_object_map"));
    auto& st = state().excluded_objects_state();
    seed_objects(st);
    std::vector<std::string> taps;

    ExcludeObjectMapView view;
    view.create(
        test_screen(), &st, 235.0f, 235.0f, [&](const std::string& name) { taps.push_back(name); },
        ExcludeTapMode::Toggle, nullptr);
    process_lvgl(50);
    st.set_excluded_objects({"OBJ_1"});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    lv_obj_t* rect = ExcludeObjectMapViewTestAccess::rect_for(view, "OBJ_1");
    REQUIRE(rect != nullptr);
    CHECK(lv_obj_has_flag(rect, LV_OBJ_FLAG_CLICKABLE));
    lv_obj_send_event(rect, LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "OBJ_1");

    view.destroy();
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture, "Map view in exclude-only mode drops an excluded rect's tap",
                 "[exclude_map][pre_start_exclude]") {
    REQUIRE(register_component("components/exclude_object_map"));
    auto& st = state().excluded_objects_state();
    seed_objects(st);

    ExcludeObjectMapView view;
    view.create(test_screen(), &st, 235.0f, 235.0f, {}, ExcludeTapMode::ExcludeOnly, nullptr);
    process_lvgl(50);
    st.set_excluded_objects({"OBJ_1"});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    lv_obj_t* rect = ExcludeObjectMapViewTestAccess::rect_for(view, "OBJ_1");
    REQUIRE(rect != nullptr);
    CHECK_FALSE(lv_obj_has_flag(rect, LV_OBJ_FLAG_CLICKABLE));

    view.destroy();
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture, "Each open map view's close button closes only that view",
                 "[exclude_map][pre_start_exclude]") {
    REQUIRE(register_component("components/exclude_object_map"));
    auto& st = state().excluded_objects_state();
    seed_objects(st);

    int first_closed = 0;
    int second_closed = 0;
    {
        ExcludeObjectMapView first;
        ExcludeObjectMapView second;
        first.set_close_callback([&] { ++first_closed; });
        second.set_close_callback([&] { ++second_closed; });
        first.create(test_screen(), &st, 235.0f, 235.0f, {}, ExcludeTapMode::ExcludeOnly, nullptr);
        second.create(test_screen(), &st, 235.0f, 235.0f, {}, ExcludeTapMode::ExcludeOnly, nullptr);
        process_lvgl(20);

        lv_obj_send_event(lv_obj_find_by_name(first.root(), "close_btn"), LV_EVENT_CLICKED,
                          nullptr);
        CHECK(first_closed == 1);
        CHECK(second_closed == 0);

        lv_obj_send_event(lv_obj_find_by_name(second.root(), "close_btn"), LV_EVENT_CLICKED,
                          nullptr);
        CHECK(second_closed == 1);

        first.destroy();
        second.destroy();
        process_lvgl(20);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "A picked object's outline fades on the map like an excluded one",
                 "[exclude_map][pre_start_exclude]") {
    REQUIRE(register_component("components/exclude_object_map"));
    auto& st = state().excluded_objects_state();
    seed_objects(st);

    ExcludeObjectMapView view;
    view.create(test_screen(), &st, 235.0f, 235.0f, {}, ExcludeTapMode::Toggle, nullptr);
    process_lvgl(50);

    // Midpoint of OBJ_1's bottom edge, (60,60)-(90,60).
    const int unpicked = ExcludeObjectMapViewTestAccess::outline_alpha_near(view, 75.0f, 60.0f);
    REQUIRE(unpicked > 0);

    st.set_excluded_objects({"OBJ_1"});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    const int picked = ExcludeObjectMapViewTestAccess::outline_alpha_near(view, 75.0f, 60.0f);
    CHECK(picked > 0);
    CHECK(picked <= static_cast<int>(helix::ui::object_badge_opa(true)));
    // Its neighbour keeps a full-strength outline, and the picked rect stays tappable.
    CHECK(ExcludeObjectMapViewTestAccess::outline_alpha_near(view, 115.0f, 100.0f) == unpicked);
    CHECK(lv_obj_has_flag(ExcludeObjectMapViewTestAccess::rect_for(view, "OBJ_1"),
                          LV_OBJ_FLAG_CLICKABLE));

    view.destroy();
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "Map outlines come from the map's own copy once the source parse is gone",
                 "[exclude_map][pre_start_exclude]") {
    REQUIRE(register_component("components/exclude_object_map"));
    auto& st = state().excluded_objects_state();
    seed_objects(st);
    // Only the parse knows OBJ_1's outline.
    {
        using ObjectInfo = helix::PrinterExcludedObjectsState::ObjectInfo;
        std::vector<ObjectInfo> objs;
        for (const auto& name : st.get_defined_objects()) {
            ObjectInfo o = *st.get_object_geometry(name);
            o.polygon.clear();
            objs.push_back(std::move(o));
        }
        st.set_defined_objects_with_geometry(objs);
    }

    auto parsed = std::make_unique<helix::gcode::ParsedGCodeFile>();
    helix::gcode::Layer layer0;
    const int16_t idx = parsed->intern_object_name("OBJ_1");
    const std::vector<glm::vec3> square = {
        {60, 60, 0.2f}, {90, 60, 0.2f}, {90, 90, 0.2f}, {60, 90, 0.2f}};
    for (size_t i = 0; i < square.size(); ++i) {
        helix::gcode::ToolpathSegment seg;
        seg.start = square[i];
        seg.end = square[(i + 1) % square.size()];
        seg.is_extrusion = true;
        seg.object_name_index = idx;
        layer0.segments.push_back(seg);
    }
    parsed->layers.push_back(std::move(layer0));

    ExcludeObjectMapView view;
    view.create(test_screen(), &st, 235.0f, 235.0f, {}, ExcludeTapMode::Toggle, parsed.get());
    process_lvgl(50);
    // Midpoint of the hull's bottom edge, (60,60)-(90,60).
    REQUIRE(ExcludeObjectMapViewTestAccess::outline_alpha_near(view, 75.0f, 60.0f) > 0);

    // The viewer frees its parse on clear while the map stays open.
    parsed.reset();
    st.set_excluded_objects({"OBJ_1"});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    const int picked = ExcludeObjectMapViewTestAccess::outline_alpha_near(view, 75.0f, 60.0f);
    CHECK(picked > 0);
    CHECK(picked <= static_cast<int>(helix::ui::object_badge_opa(true)));

    view.destroy();
    process_lvgl(20);
}

TEST_CASE_METHOD(XMLTestFixture, "Every object rect and its badge fit inside the map's plate",
                 "[exclude_map][fit]") {
    REQUIRE(register_component("components/exclude_object_map"));
    auto& st = state().excluded_objects_state();
    using ObjectInfo = helix::PrinterExcludedObjectsState::ObjectInfo;
    std::vector<ObjectInfo> objs;
    // Objects at the corners of their extent, long names as slicers emit them.
    const std::vector<std::pair<glm::vec2, glm::vec2>> boxes = {
        {{10, 10}, {40, 40}}, {{95, 10}, {125, 40}}, {{10, 76}, {40, 106}}, {{95, 76}, {125, 106}}};
    for (size_t i = 0; i < boxes.size(); ++i) {
        ObjectInfo o;
        o.name = "Cylinder_id_" + std::to_string(i) + "_copy_0_with_a_long_slicer_name";
        o.bbox_min = boxes[i].first;
        o.bbox_max = boxes[i].second;
        o.center = (o.bbox_min + o.bbox_max) * 0.5f;
        o.has_bbox = true;
        o.has_center = true;
        o.polygon = {o.bbox_min, {o.bbox_max.x, o.bbox_min.y}, o.bbox_max};
        objs.push_back(std::move(o));
    }
    st.set_defined_objects_with_geometry(objs);

    struct Shape {
        const char* label;
        int32_t w, h;
    };
    for (const Shape shape : {Shape{"portrait", 220, 360}, Shape{"landscape", 420, 160}}) {
        DYNAMIC_SECTION(shape.label) {
            lv_obj_t* card = lv_obj_create(test_screen());
            lv_obj_set_size(card, shape.w, shape.h);

            ExcludeObjectMapView view;
            view.create(card, &st, 235.0f, 235.0f, {}, ExcludeTapMode::Toggle, nullptr);
            REQUIRE(view.is_active());
            process_lvgl(30);

            lv_obj_update_layout(test_screen());

            lv_obj_t* plate = lv_obj_find_by_name(view.root(), "plate_area");
            REQUIRE(plate);
            lv_area_t plate_area;
            lv_obj_get_coords(plate, &plate_area);

            for (size_t i = 0; i < boxes.size(); ++i) {
                const std::string name = "obj_rect_" + std::to_string(i);
                lv_obj_t* rect = lv_obj_find_by_name(view.root(), name.c_str());
                REQUIRE(rect);
                lv_obj_t* disc = lv_obj_get_child(rect, 0);
                REQUIRE(disc);
                for (lv_obj_t* obj : {rect, disc}) {
                    lv_area_t a;
                    lv_obj_get_coords(obj, &a);
                    INFO(name << " (" << a.x1 << "," << a.y1 << ")-(" << a.x2 << "," << a.y2
                              << ") in plate (" << plate_area.x1 << "," << plate_area.y1 << ")-("
                              << plate_area.x2 << "," << plate_area.y2 << ")");
                    REQUIRE(lv_area_get_width(&a) > 0);
                    CHECK(a.x1 >= plate_area.x1);
                    CHECK(a.y1 >= plate_area.y1);
                    CHECK(a.x2 <= plate_area.x2);
                    CHECK(a.y2 <= plate_area.y2);
                }
            }

            view.destroy();
            process_lvgl(20);
            lv_obj_delete(card);
        }
    }
}
