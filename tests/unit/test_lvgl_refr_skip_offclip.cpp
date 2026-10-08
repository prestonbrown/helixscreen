// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: the code under test is the patched LVGL refresh walk in lib/lvgl, reached through
// the LVGL API.

/**
 * @file test_lvgl_refr_skip_offclip.cpp
 * @brief The refresh walk skips a widget outside the refreshed area before its style
 *        lookups (patches/lvgl_refr_skip_offclip_children.patch), and that shortcut
 *        draws exactly what the full path would: a shadow, an outline or a transform
 *        that reaches the area from outside the widget's coords is still drawn.
 */

#include "../lvgl_ui_test_fixture.h"
#include "lvgl/src/core/lv_obj_private.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Counts the times `obj` was drawn.
int* count_draws(lv_obj_t* obj) {
    auto* n = new int(0);
    lv_obj_add_event_cb(
        obj, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_DRAW_MAIN_BEGIN, n);
    lv_obj_add_event_cb(
        obj, [](lv_event_t* e) { delete static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, n);
    return n;
}

/// A 50x50 widget at (100, 100), already rendered once.
lv_obj_t* make_widget(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 100, 100);
    lv_obj_set_size(obj, 50, 50);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_refr_now(nullptr);
    REQUIRE(obj->rendered);
    return obj;
}

/// Redraws only the rows [y1, y2] of the screen.
void refresh_rows(lv_obj_t* screen, int32_t y1, int32_t y2) {
    lv_area_t a = {0, y1, 400, y2};
    lv_obj_invalidate_area(screen, &a);
    lv_refr_now(nullptr);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Refresh walk: a widget outside the area is not drawn",
                 "[lvgl][refr]") {
    lv_obj_t* screen = test_screen();
    lv_obj_t* obj = make_widget(screen);
    int* draws = count_draws(obj);

    refresh_rows(screen, 10, 20);
    CHECK(*draws == 0);

    refresh_rows(screen, 120, 130);
    CHECK(*draws == 1);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Refresh walk: a shadow or outline reaching the area from outside is drawn",
                 "[lvgl][refr]") {
    lv_obj_t* screen = test_screen();
    lv_obj_t* obj = make_widget(screen);

    SECTION("shadow") {
        lv_obj_set_style_shadow_width(obj, 40, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    }
    SECTION("outline") {
        lv_obj_set_style_outline_width(obj, 30, LV_PART_MAIN);
        lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    }
    lv_refr_now(nullptr);
    int* draws = count_draws(obj);

    // Rows 85-90 lie above the widget's coords (100) but inside its extra draw area.
    refresh_rows(screen, 85, 90);
    CHECK(*draws == 1);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Refresh walk: a transform reaching the area from outside is drawn",
                 "[lvgl][refr]") {
    lv_obj_t* screen = test_screen();
    lv_obj_t* obj = make_widget(screen);
    // Scaled 3x about its centre (125, 125), it covers rows 50-200.
    lv_obj_set_style_transform_scale(obj, 3 * 256, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(obj, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(obj, lv_pct(50), LV_PART_MAIN);
    lv_refr_now(nullptr);
    int* draws = count_draws(obj);

    refresh_rows(screen, 60, 70);
    CHECK(*draws >= 1);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Refresh walk: a widget never rendered is marked rendered even outside the area",
                 "[lvgl][refr]") {
    lv_obj_t* screen = test_screen();
    lv_refr_now(nullptr);

    // Built with invalidation off, so the only area refreshed below misses it.
    lv_display_t* disp = lv_display_get_default();
    lv_display_enable_invalidation(disp, false);
    lv_obj_t* obj = lv_obj_create(screen);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 100, 100);
    lv_obj_set_size(obj, 50, 50);
    lv_obj_update_layout(screen);
    lv_display_enable_invalidation(disp, true);
    REQUIRE_FALSE(obj->rendered);

    // The walk reaches it as a child of the screen; animations key off the flag.
    refresh_rows(screen, 10, 20);
    CHECK(obj->rendered);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Refresh walk: an overflowing child outside its parent's area is drawn as before",
                 "[lvgl][refr]") {
    lv_obj_t* screen = test_screen();
    lv_obj_t* parent = make_widget(screen);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t* child = lv_obj_create(parent);
    lv_obj_remove_style_all(child);
    lv_obj_set_pos(child, 0, 100); // rows 200-229, below the parent's 100-149
    lv_obj_set_size(child, 30, 30);
    lv_obj_set_style_bg_opa(child, LV_OPA_COVER, LV_PART_MAIN);
    lv_refr_now(nullptr);
    REQUIRE(child->rendered);
    int* draws = count_draws(child);

    refresh_rows(screen, 210, 220);
    CHECK(*draws == 0);
    lv_obj_delete(parent);
}
