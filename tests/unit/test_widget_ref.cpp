// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_ref.cpp
 * @brief WidgetRef clears itself on the widget's own delete, and leaves no
 *        hook behind when it lets go first (prestonbrown/helixscreen#1298)
 */

#include "ui_widget_ref.h"

#include "../lvgl_test_fixture.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::WidgetRef;

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef clears itself when its widget is deleted",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    WidgetRef ref(obj);
    REQUIRE(ref.get() == obj);
    REQUIRE(ref);

    lv_obj_delete(obj);

    CHECK(ref.get() == nullptr);
    CHECK_FALSE(ref);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef clears itself when an ancestor subtree is deleted",
                 "[widget_ref]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_t* child = lv_obj_create(parent);
    WidgetRef ref;
    ref = child;
    REQUIRE(ref.get() == child);

    lv_obj_delete(parent);

    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef clears itself on a deferred delete", "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    WidgetRef ref(obj);

    lv_obj_delete_async(obj);
    REQUIRE(ref.get() == obj); // still alive until the async call runs
    process_lvgl(20);

    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef rebind unhooks the previous widget", "[widget_ref]") {
    lv_obj_t* first = lv_obj_create(test_screen());
    lv_obj_t* second = lv_obj_create(test_screen());
    const uint32_t first_events = lv_obj_get_event_count(first);

    WidgetRef ref(first);
    REQUIRE(lv_obj_get_event_count(first) == first_events + 1);

    ref = second;
    CHECK(lv_obj_get_event_count(first) == first_events);

    // The old widget dying must not touch a handle that moved on.
    lv_obj_delete(first);
    CHECK(ref.get() == second);

    lv_obj_delete(second);
    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef rebinding to the same widget keeps one hook",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);

    WidgetRef ref(obj);
    ref = obj;
    ref.reset(obj);
    CHECK(lv_obj_get_event_count(obj) == base + 1);

    lv_obj_delete(obj);
    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef cleared by hand unhooks the live widget",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);

    WidgetRef ref(obj);
    ref = nullptr;
    CHECK(ref.get() == nullptr);
    CHECK(lv_obj_get_event_count(obj) == base);

    lv_obj_delete(obj);
    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef destroyed before its widget leaves no hook behind",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);

    auto ref = std::make_unique<WidgetRef>(obj);
    REQUIRE(lv_obj_get_event_count(obj) == base + 1);
    ref.reset();

    // A hook left behind would write into the freed handle here.
    CHECK(lv_obj_get_event_count(obj) == base);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef destroyed after its widget touches nothing",
                 "[widget_ref]") {
    auto ref = std::make_unique<WidgetRef>(lv_obj_create(test_screen()));
    lv_obj_delete(ref->get());
    REQUIRE(ref->get() == nullptr);
    ref.reset(); // must not reach into the freed widget
    SUCCEED();
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef cleared from another delete handler on its widget",
                 "[widget_ref]") {
    // An owner whose own LV_EVENT_DELETE handler lets go of the handle while
    // LVGL is still walking that widget's event list.
    struct Owner {
        WidgetRef ref;
        int fired = 0;
    } owner;

    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t* e) {
            auto* o = static_cast<Owner*>(lv_event_get_user_data(e));
            o->ref = nullptr;
            ++o->fired;
        },
        LV_EVENT_DELETE, &owner);
    owner.ref = obj;

    lv_obj_delete(obj);

    CHECK(owner.fired == 1);
    CHECK(owner.ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef converts to lv_obj_t* for LVGL calls",
                 "[widget_ref]") {
    WidgetRef ref(lv_obj_create(test_screen()));
    lv_obj_add_flag(ref, LV_OBJ_FLAG_HIDDEN);
    CHECK(lv_obj_has_flag(ref.get(), LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* raw = ref;
    CHECK(raw == ref.get());
    CHECK(ref == raw);
    CHECK(ref != nullptr);
    lv_obj_delete(raw);
    CHECK(ref == nullptr);
}
