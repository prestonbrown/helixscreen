// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: exercises patches/lvgl_event_dispatch_cb_guard.patch, shipped LVGL code with
//                 no HelixScreen header to include

/**
 * @file test_event_dispatch_null_dsc.cpp
 * @brief The event dispatch loop skips a NULL descriptor instead of faulting on it.
 *
 * lv_event_send() walks an object's event list and reads dsc->cb for every
 * entry. A torn entry (a NULL descriptor pointer in the list) is a fault at
 * address 0 unless the loop checks the pointer first. The guard in
 * patches/lvgl_event_dispatch_cb_guard.patch reports the slot through
 * helix_lvgl_anomaly() and continues with the next callback.
 *
 * The test plants a NULL between two live callbacks and sends one event. A
 * regression SIGSEGVs the test process; the assertions pin the "continue"
 * half, i.e. the callback after the torn slot still runs.
 *
 * @see lib/lvgl/src/misc/lv_event.c lv_event_send()
 */

#include "../lvgl_test_fixture.h"
#include "core/lv_obj_private.h" // spec_attr->event_list: the slot a torn list leaves NULL
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

namespace {

int g_before_calls = 0;
int g_torn_calls = 0;
int g_after_calls = 0;

void before_cb(lv_event_t*) {
    ++g_before_calls;
}
void torn_cb(lv_event_t*) {
    ++g_torn_calls;
}
void after_cb(lv_event_t*) {
    ++g_after_calls;
}

lv_event_dsc_t** event_slot(lv_obj_t* obj, uint32_t index) {
    lv_event_list_t* list = &obj->spec_attr->event_list;
    return static_cast<lv_event_dsc_t**>(lv_array_at(&list->array, index));
}

} // namespace

class NullDscFixture : public LVGLTestFixture {
  public:
    lv_obj_t* obj = nullptr;
    lv_event_dsc_t* torn_dsc = nullptr;

    NullDscFixture() {
        g_before_calls = 0;
        g_torn_calls = 0;
        g_after_calls = 0;
        obj = lv_obj_create(test_screen());
        lv_obj_add_event_cb(obj, before_cb, LV_EVENT_CLICKED, nullptr);
        torn_dsc = lv_obj_add_event_cb(obj, torn_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(obj, after_cb, LV_EVENT_CLICKED, nullptr);
        REQUIRE(lv_obj_get_event_count(obj) == 3);
        REQUIRE(*event_slot(obj, 1) == torn_dsc);
    }

    // Put the real descriptor back so the object tears down through the
    // ordinary remove path and the descriptor is freed rather than leaked.
    ~NullDscFixture() override {
        if (obj && torn_dsc) {
            *event_slot(obj, 1) = torn_dsc;
        }
    }
};

TEST_CASE_METHOD(NullDscFixture, "Intact event list dispatches every callback",
                 "[lvgl][event][null_dsc]") {
    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);

    CHECK(g_before_calls == 1);
    CHECK(g_torn_calls == 1);
    CHECK(g_after_calls == 1);
}

TEST_CASE_METHOD(NullDscFixture, "NULL descriptor in the event list is skipped, not dereferenced",
                 "[lvgl][event][null_dsc]") {
    *event_slot(obj, 1) = nullptr;

    // Without the guard this reads dsc->cb through a NULL pointer.
    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);

    CHECK(g_before_calls == 1);
    CHECK(g_torn_calls == 0);
    // The loop continues past the torn slot; a `break` would leave this at 0.
    CHECK(g_after_calls == 1);
}

TEST_CASE_METHOD(NullDscFixture, "NULL descriptor is skipped on every send, not just the first",
                 "[lvgl][event][null_dsc]") {
    *event_slot(obj, 1) = nullptr;

    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);

    CHECK(g_before_calls == 2);
    CHECK(g_torn_calls == 0);
    CHECK(g_after_calls == 2);
}
