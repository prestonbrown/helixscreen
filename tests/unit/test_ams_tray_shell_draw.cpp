// SPDX-License-Identifier: GPL-3.0-or-later
//
// The live tray's lid and box primitives: build_lid's caps and hull, and fill_box.

#include "../lvgl_test_fixture.h"
#include "ams_tray_shell_draw.h"
#include "ams_unit_shell.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::tray;
using Catch::Approx;

namespace {

TrayBox lid_box() {
    const float dz = box_depth(110);
    return {20, 420, 100, 140, dz, box_rise(dz), 10};
}

float turn(PointF o, PointF a, PointF b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

/// Every cap sample of @p lid lies inside its hull.
void check_hull_holds_caps(const Lid& lid) {
    REQUIRE(lid.hull_n >= 3);
    float area = 0;
    for (int i = 0; i < lid.hull_n; ++i) {
        const PointF a = lid.hull[i], b = lid.hull[(i + 1) % lid.hull_n];
        area += a.x * b.y - b.x * a.y;
    }
    const float side = area < 0 ? -1.0f : 1.0f;
    bool inside = true;
    for (const PointF* cap : {lid.left, lid.right}) {
        for (int p = 0; p < kCapPoints; ++p) {
            for (int i = 0; i < lid.hull_n; ++i) {
                const PointF a = lid.hull[i], b = lid.hull[(i + 1) % lid.hull_n];
                inside = inside && side * turn(a, b, cap[p]) >= -1e-3f;
            }
        }
    }
    CHECK(inside);
}

} // namespace

TEST_CASE("tray shell draw: a shell's lid takes both caps and its flat top from the profile",
          "[ams][tray][unit-art]") {
    const UnitShell* shell = unit_shell_by_id("ace_pro");
    REQUIRE(shell != nullptr);
    const TrayBox box = lid_box();
    const ShellView view{box, shell, 110.0f, nullptr, 0};
    Lid lid;
    build_lid(view, box, 0.0f, 60, 380, lid);

    PointF left[kCapPoints], right[kCapPoints];
    shell_cap_polyline(box, *shell, 110.0f, 60, left, kCapPoints);
    shell_cap_polyline(box, *shell, 110.0f, 380, right, kCapPoints);
    int first = -1, last = -1;
    REQUIRE(shell_lid_top_run(*shell, kCapPoints, first, last));
    bool caps_match = true;
    for (int i = 0; i < kCapPoints; ++i) {
        caps_match = caps_match && lid.left[i].x == Approx(left[i].x) &&
                     lid.left[i].y == Approx(left[i].y) && lid.right[i].x == Approx(right[i].x) &&
                     lid.right[i].y == Approx(right[i].y);
    }
    CHECK(caps_match);
    CHECK(lid.top_first == first);
    CHECK(lid.top_last == last);
    CHECK(lid.box.fl == 60);
    CHECK(lid.box.fr == 380);
    check_hull_holds_caps(lid);
}

TEST_CASE("tray shell draw: a generic lid draws a dome cap each end and no flat top",
          "[ams][tray][unit-art]") {
    const TrayBox box = lid_box();
    const ShellView view{box, nullptr, 110.0f, nullptr, 0};
    Lid lid;
    build_lid(view, box, 30.0f, 60, 380, lid);

    PointF left[kCapPoints], right[kCapPoints];
    cap_polyline(box, 30.0f, 60, left, kCapPoints);
    cap_polyline(box, 30.0f, 380, right, kCapPoints);
    bool caps_match = true;
    for (int i = 0; i < kCapPoints; ++i) {
        caps_match = caps_match && lid.left[i].x == Approx(left[i].x) &&
                     lid.left[i].y == Approx(left[i].y) && lid.right[i].x == Approx(right[i].x) &&
                     lid.right[i].y == Approx(right[i].y);
    }
    CHECK(caps_match);
    CHECK(lid.top_first == -1);
    CHECK(lid.top_last == -1);
    check_hull_holds_caps(lid);
}

TEST_CASE_METHOD(LVGLTestFixture, "tray shell draw: fill_box paints its rectangle and nothing else",
                 "[ams][tray][unit-art]") {
    lv_draw_buf_t* buf = lv_draw_buf_create(80, 60, LV_COLOR_FORMAT_ARGB8888, 0);
    REQUIRE(buf != nullptr);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    lv_canvas_set_draw_buf(canvas, buf);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    fill_box(&layer, lv_point_t{5, 5}, 10, 10, 30, 25, 0xff0000);
    lv_canvas_finish_layer(canvas, &layer);

    const auto px = [&](int x, int y) {
        const uint8_t* p = static_cast<const uint8_t*>(lv_draw_buf_goto_xy(buf, x, y));
        return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
    };
    CHECK(px(25, 25) == 0xffff0000u);
    CHECK((px(8, 8) >> 24) == 0u);
    CHECK((px(45, 25) >> 24) == 0u);
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);
}
