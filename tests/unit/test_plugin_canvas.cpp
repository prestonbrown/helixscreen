// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "plugin_canvas.h"
#include "theme_manager.h"

#include <limits>
#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;

namespace {

bool s_canvas_registered = false;

/// One host component with a 200x100 canvas named "t__c", plus the teardown
/// every case needs: instances deleted, listener cleared, slot erased.
class CanvasRig : public XMLTestFixture {
  public:
    CanvasRig() : XMLTestFixture() {
        if (!s_canvas_registered) {
            register_plugin_canvas_widget();
            REQUIRE(lv_xml_register_component_from_data(
                        "canvas_host",
                        "<component><view name=\"canvas_host\" extends=\"lv_obj\" width=\"200\" "
                        "height=\"100\" style_pad_all=\"0\"><plugin_canvas name=\"t__c\"/>"
                        "</view></component>") == LV_RESULT_OK);
            s_canvas_registered = true;
        }
    }

    ~CanvasRig() override {
        if (host) {
            lv_obj_delete(host);
            host = nullptr;
        }
        canvas_set_size_listener("t__c", nullptr);
        canvas_commit("t__c", nullptr);
    }

    lv_obj_t* make() {
        host = static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "canvas_host", nullptr));
        REQUIRE(host != nullptr);
        return host;
    }

    lv_obj_t* canvas_of(lv_obj_t* root) {
        return lv_obj_find_by_name(root, "t__c");
    }

    lv_obj_t* host = nullptr;
};

/// RGB of the canvas center in an ARGB8888 snapshot (memory order B,G,R,A).
/// lv_color_to_u32 yields ARGB, so comparisons mask its alpha byte.
uint32_t as_rgb(lv_color_t c) {
    return lv_color_to_u32(c) & 0xFFFFFF;
}

uint32_t pixel_rgb(lv_obj_t* obj, int x, int y) {
    lv_draw_buf_t* snap = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    const uint8_t* row = snap->data + y * snap->header.stride;
    const uint32_t rgb =
        (uint32_t(row[x * 4 + 2]) << 16) | (uint32_t(row[x * 4 + 1]) << 8) | uint32_t(row[x * 4]);
    lv_draw_buf_destroy(snap);
    return rgb;
}

uint32_t center_pixel_rgb(lv_obj_t* obj) {
    return pixel_rgb(obj, 100, 50); // the rig's canvas is 200x100
}

/// A list with one of every primitive kind: Line, Polyline, Rect, Arc, Circle,
/// Text, resolving tokens "primary" and "text" as colors and "body" as a font.
std::unique_ptr<DisplayList> kitchen_sink_list() {
    auto list = std::make_unique<DisplayList>();
    list->tokens = {"primary", "text", "body"};

    CanvasPrim line;
    line.op = CanvasOp::Line;
    line.color = 0;
    line.width = 2;
    line.a = 10;
    line.b = 10;
    line.c = 50;
    line.d = 40;
    list->prims.push_back(line);

    CanvasPrim poly;
    poly.op = CanvasOp::Polyline;
    poly.color = 0;
    poly.width = 1;
    poly.first = 0;
    poly.count = 3;
    list->points = {{10, 60}, {60, 60}, {60, 90}};
    list->prims.push_back(poly);

    CanvasPrim rect;
    rect.op = CanvasOp::Rect;
    rect.color = 0;
    rect.border = 1;
    rect.width = 1;
    rect.radius = 4;
    rect.a = 100;
    rect.b = 10;
    rect.c = 40;
    rect.d = 20;
    list->prims.push_back(rect);

    CanvasPrim arc;
    arc.op = CanvasOp::Arc;
    arc.color = 0;
    arc.width = 3;
    arc.radius = 15;
    arc.a = 170;
    arc.b = 40;
    arc.c = 0;
    arc.d = 180;
    list->prims.push_back(arc);

    CanvasPrim circle;
    circle.op = CanvasOp::Circle;
    circle.color = 0;
    circle.radius = 10;
    circle.a = 40;
    circle.b = 40;
    list->prims.push_back(circle);

    CanvasPrim text;
    text.op = CanvasOp::Text;
    text.color = 1;
    text.font = 2;
    text.a = 5;
    text.b = 5;
    text.first = 0;
    text.count = 2;
    list->text = "hi";
    list->prims.push_back(text);
    return list;
}

} // namespace

TEST_CASE_METHOD(CanvasRig, "a fresh instance reports its content size", "[plugin][canvas]") {
    lv_obj_t* root = make();
    lv_obj_update_layout(root);
    lv_obj_t* canvas = canvas_of(root);
    REQUIRE(canvas != nullptr);

    auto [w, h] = canvas_size("t__c");
    CHECK(w == 200);
    CHECK(h == 100);
    CHECK(canvas_instance_count("t__c") == 1);

    lv_obj_delete(root);
    host = nullptr;
    CHECK(canvas_instance_count("t__c") == 0);
}

TEST_CASE_METHOD(CanvasRig, "the size listener sees each change once", "[plugin][canvas]") {
    int calls = 0;
    int32_t last_w = 0, last_h = 0;
    canvas_set_size_listener("t__c", [&](int32_t w, int32_t h) {
        calls++;
        last_w = w;
        last_h = h;
    });

    lv_obj_t* root = make();
    lv_obj_update_layout(root);
    lv_obj_set_size(root, 300, 100);
    lv_obj_update_layout(root);
    lv_obj_set_size(root, 300, 100);
    lv_obj_update_layout(root);

    CHECK(calls == 2);
    CHECK(last_w == 300);
    CHECK(last_h == 100);
}

TEST_CASE_METHOD(CanvasRig, "an instance created after commit draws the committed list",
                 "[plugin][canvas]") {
    auto list = std::make_unique<DisplayList>();
    CanvasPrim line;
    line.op = CanvasOp::Line;
    line.color = 0;
    line.a = 0;
    line.b = 0;
    line.c = 100;
    line.d = 50;
    list->tokens = {"primary"};
    list->prims.push_back(line);
    canvas_commit("t__c", std::move(list));

    lv_obj_t* root = make();
    lv_obj_update_layout(root);
    lv_refr_now(nullptr);

    const DisplayList* committed = canvas_committed("t__c");
    REQUIRE(committed != nullptr);
    CHECK(committed->prims.size() == 1);

    canvas_commit("t__c", nullptr);
    lv_refr_now(nullptr);
    CHECK(canvas_committed("t__c") == nullptr);
}

TEST_CASE_METHOD(CanvasRig, "every primitive kind replays", "[plugin][canvas]") {
    lv_obj_t* root = make();
    lv_obj_update_layout(root);
    canvas_commit("t__c", kitchen_sink_list());
    lv_refr_now(nullptr);
    // Completing the refresh without crashing is the assertion: the replay
    // touches every primitive's memory while a draw task consumes it.
}

TEST_CASE_METHOD(CanvasRig, "a non-finite primitive draws nothing", "[plugin][canvas]") {
    lv_obj_t* root = make();
    lv_obj_update_layout(root);
    lv_obj_t* canvas = canvas_of(root);
    REQUIRE(canvas != nullptr);

    auto list = std::make_unique<DisplayList>();
    list->tokens = {"primary"};

    const auto nan = std::numeric_limits<lv_value_precise_t>::quiet_NaN();

    CanvasPrim fill;
    fill.op = CanvasOp::Rect;
    fill.color = 0;
    fill.a = 0;
    fill.b = nan;
    fill.c = 200;
    fill.d = 100;
    list->prims.push_back(fill);

    CanvasPrim line;
    line.op = CanvasOp::Line;
    line.color = 0;
    line.a = nan;
    line.b = 0;
    line.c = 100;
    line.d = 50;
    list->prims.push_back(line);

    // A wrapped first + count must not read past the point vector.
    CanvasPrim poly;
    poly.op = CanvasOp::Polyline;
    poly.color = 0;
    poly.first = 0xFFFFFFFF;
    poly.count = 2;
    list->points = {{0, 0}, {10, 10}, {20, 20}};
    list->prims.push_back(poly);

    canvas_commit("t__c", std::move(list));
    lv_refr_now(nullptr); // completing at all is half the assertion

    const uint32_t primary = as_rgb(theme_manager_get_color("primary"));
    REQUIRE(primary != 0); // a black primary would make the check below vacuous
    CHECK(center_pixel_rgb(canvas) != primary);
}

TEST_CASE_METHOD(CanvasRig, "a committed fill paints its pixels", "[plugin][canvas]") {
    lv_obj_t* root = make();
    lv_obj_update_layout(root);
    lv_obj_t* canvas = canvas_of(root);
    REQUIRE(canvas != nullptr);

    auto list = std::make_unique<DisplayList>();
    CanvasPrim fill;
    fill.op = CanvasOp::Rect;
    fill.color = 0;
    fill.a = 0;
    fill.b = 0;
    fill.c = 200;
    fill.d = 100;
    list->tokens = {"primary"};
    list->prims.push_back(fill);
    canvas_commit("t__c", std::move(list));
    lv_refr_now(nullptr);

    const uint32_t primary = as_rgb(theme_manager_get_color("primary"));
    REQUIRE(primary != 0); // a black primary would make the blank check below vacuous
    CHECK(center_pixel_rgb(canvas) == primary);

    canvas_commit("t__c", nullptr);
    lv_refr_now(nullptr);
    CHECK(center_pixel_rgb(canvas) != primary);
}

TEST_CASE_METHOD(CanvasRig, "a polyline area fill is continuous under a diagonal",
                 "[plugin][canvas]") {
    lv_obj_t* root = make();
    lv_obj_update_layout(root);
    lv_obj_t* canvas = canvas_of(root);
    REQUIRE(canvas != nullptr);

    auto list = std::make_unique<DisplayList>();
    list->tokens = {"primary"};
    CanvasPrim poly;
    poly.op = CanvasOp::Polyline;
    poly.color = 0; // stroke, transparent so only the fill can paint
    poly.opa = 0;
    poly.border = 0; // the area fill
    poly.fill_opa = LV_OPA_COVER;
    poly.width = 1;
    poly.a = 98; // baseline y
    poly.first = 0;
    poly.count = 2;
    list->points = {{2, 2}, {198, 98}};
    list->prims.push_back(poly);
    canvas_commit("t__c", std::move(list));
    lv_refr_now(nullptr);

    // The unit-test snapshot rasterizes masked primitives over their whole
    // bounding box, so pixels above the line cannot be asserted here; the
    // running app is where the fill's upper edge is verified by eye. What this
    // fixture can pin: with the stroke transparent, every column under the
    // diagonal is painted, so a fill that stepped at the samples (leaving the
    // just-under-line pixels bare) or was removed entirely goes red.
    const uint32_t primary = as_rgb(theme_manager_get_color("primary"));
    REQUIRE(primary != 0); // a black primary would make the checks below vacuous
    for (int x = 10; x <= 190; x += 8) {
        const double ideal = 2.0 + (x - 2) * 96.0 / 196.0;
        const int y_in = static_cast<int>(ideal) + 2;
        CAPTURE(x, y_in);
        CHECK(pixel_rgb(canvas, x, y_in) == primary);
    }
    // The baseline row is painted across the span: the fill reaches the
    // baseline at every column, not only under the samples.
    for (int x = 10; x <= 190; x += 8)
        CHECK(pixel_rgb(canvas, x, 97) == primary);
}

TEST_CASE_METHOD(CanvasRig, "tokens resolve at draw time", "[plugin][canvas]") {
    lv_obj_t* root = make();
    lv_obj_update_layout(root);
    lv_obj_t* canvas = canvas_of(root);
    REQUIRE(canvas != nullptr);

    auto list = std::make_unique<DisplayList>();
    CanvasPrim fill;
    fill.op = CanvasOp::Rect;
    fill.color = 0;
    fill.a = 0;
    fill.b = 0;
    fill.c = 200;
    fill.d = 100;
    list->tokens = {"card_bg"};
    list->prims.push_back(fill);
    canvas_commit("t__c", std::move(list));
    lv_refr_now(nullptr);

    const helix::ThemeData saved_theme = theme_manager_get_active_theme();
    const bool saved_dark = theme_manager_is_dark_mode();

    const uint32_t first = center_pixel_rgb(canvas);
    CHECK(first == as_rgb(theme_manager_get_color("card_bg")));

    theme_manager_apply_theme(saved_theme, !saved_dark);
    lv_obj_invalidate(canvas);
    lv_refr_now(nullptr);

    const uint32_t second = center_pixel_rgb(canvas);
    CHECK(second == as_rgb(theme_manager_get_color("card_bg")));
    CHECK(second != first);

    theme_manager_apply_theme(saved_theme, saved_dark);
}

TEST_CASE_METHOD(CanvasRig, "theme_manager_has_color answers token existence", "[plugin][canvas]") {
    CHECK(theme_manager_has_color("primary"));
    CHECK(theme_manager_has_color("text"));
    CHECK_FALSE(theme_manager_has_color("no_such_token"));
}

#endif // HELIX_HAS_PLUGINS
