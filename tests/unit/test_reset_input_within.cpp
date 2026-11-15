// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_reset_input_within.cpp
 * @brief helix::ui::reset_input_within, the input reset a toast's teardown runs.
 *
 * ToastManager::detach_from_input resets through it before the toast's objects
 * are retired. Each case builds a toast's shape (a root with an action button
 * inside it) and calls the helper as the teardown does, with pointer input from
 * real indev reads.
 */

#include "ui_utils.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_pointer_indev.h"

#include "../catch_amalgamated.hpp"

using helix_test::ScopedPointerIndev;

namespace {

void count_event(lv_event_t* e) {
    ++*static_cast<int*>(lv_event_get_user_data(e));
}

lv_point_t center_of(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
}

/// A toast's shape in the bottom-right corner of its parent, clear of the top
/// left where a case holds a press elsewhere.
struct ToastShape {
    lv_obj_t* root = nullptr;
    lv_obj_t* action_btn = nullptr;
};

ToastShape make_toast(lv_obj_t* parent) {
    ToastShape toast;
    toast.root = lv_obj_create(parent);
    lv_obj_set_size(toast.root, 240, 100);
    lv_obj_align(toast.root, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_scrollable(toast.root, false);
    toast.action_btn = lv_button_create(toast.root);
    lv_obj_set_size(toast.action_btn, 60, 40);
    lv_obj_align(toast.action_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_update_layout(parent);
    return toast;
}

/// An object in the top-left corner of @p parent, clear of the toast, that
/// counts the RELEASED and CLICKED a press on it receives.
struct HeldTarget {
    lv_obj_t* obj = nullptr;
    int released = 0;
    int clicked = 0;
};

void make_held_target(lv_obj_t* parent, HeldTarget& target) {
    target.obj = lv_obj_create(parent);
    lv_obj_set_size(target.obj, 100, 100);
    lv_obj_align(target.obj, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollable(target.obj, false);
    lv_obj_add_event_cb(target.obj, count_event, LV_EVENT_RELEASED, &target.released);
    lv_obj_add_event_cb(target.obj, count_event, LV_EVENT_CLICKED, &target.clicked);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a toast's input reset leaves a press held elsewhere to finish",
                 "[toast][indev][ui_utils]") {
    ScopedPointerIndev indev;
    HeldTarget held;
    make_held_target(test_screen(), held);
    const ToastShape toast = make_toast(test_screen());

    const lv_point_t p = center_of(held.obj);
    indev.press(p.x, p.y);
    REQUIRE(indev.indev()->pointer.act_obj == held.obj);

    helix::ui::reset_input_within(toast.root);
    CHECK(indev.indev()->pointer.act_obj == held.obj);

    indev.release(p.x, p.y);
    CHECK(held.released == 1);
    CHECK(held.clicked == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "a toast's input reset clears a press held on its action button",
                 "[toast][indev][ui_utils]") {
    ScopedPointerIndev indev;
    const ToastShape toast = make_toast(test_screen());
    int clicked = 0;
    lv_obj_add_event_cb(toast.action_btn, count_event, LV_EVENT_CLICKED, &clicked);

    // A second pointer holds a press outside the toast through the reset.
    ScopedPointerIndev other;
    HeldTarget held;
    make_held_target(test_screen(), held);
    const lv_point_t h = center_of(held.obj);
    other.press(h.x, h.y);
    REQUIRE(other.indev()->pointer.act_obj == held.obj);

    const lv_point_t p = center_of(toast.action_btn);
    indev.press(p.x, p.y);
    REQUIRE(indev.indev()->pointer.act_obj == toast.action_btn);

    helix::ui::reset_input_within(toast.root);
    // Required before any release: in a real toast, CLICKED on the button reaches
    // a ToastInstance the teardown erases.
    REQUIRE(indev.indev()->pointer.act_obj == nullptr);
    CHECK(other.indev()->pointer.act_obj == held.obj);

    indev.release(p.x, p.y);
    CHECK(clicked == 0);
    other.release(h.x, h.y);
    CHECK(held.clicked == 1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a toast's input reset clears a scroll throw still running inside it",
                 "[toast][indev][ui_utils]") {
    ScopedPointerIndev indev;
    const ToastShape toast = make_toast(test_screen());
    lv_obj_t* detail = lv_obj_create(toast.root);
    lv_obj_set_size(detail, 80, 60);
    lv_obj_align(detail, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* content = lv_obj_create(detail);
    lv_obj_set_size(content, 60, 240);
    lv_obj_update_layout(test_screen());
    REQUIRE(lv_obj_get_scroll_bottom(detail) > 0);

    // A flick up inside the detail. The lift ends the press, and the throw it
    // starts keeps the detail as the pointer's scroll target with no press
    // target left to name the toast.
    const int step = static_cast<int>(indev.indev()->scroll_limit) + 5;
    lv_point_t p = center_of(detail);
    indev.press(p.x, p.y);
    for (int i = 0; i < 3; ++i) {
        p.y -= step;
        indev.move(p.x, p.y);
    }
    indev.release(p.x, p.y);
    REQUIRE(indev.indev()->pointer.act_obj == nullptr);
    REQUIRE(lv_indev_get_scroll_obj(indev.indev()) == detail);

    helix::ui::reset_input_within(toast.root);
    CHECK(lv_indev_get_scroll_obj(indev.indev()) == nullptr);
}
