// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_owned_user_string.cpp
 * @brief Contract tests for helix::ui::set_owned_user_string / get_owned_user_string.
 *
 * Five call sites hand-rolled the "own a C string on lv_obj user_data, free it on
 * LV_EVENT_DELETE" idiom and had drifted three ways: two checked the allocation,
 * two did not (lv_malloc -> memcpy straight into NULL, a SEGV on AD5M/CC1 under
 * memory pressure), and only one nulled the slot after freeing.
 *
 * The helper collapses them into one implementation with an explicit contract:
 *   - allocation failure returns false and leaves user_data untouched
 *   - a length that would overflow `len + 1` is rejected, not wrapped
 *   - it refuses to stomp a user_data slot it does not own (lesson L069)
 *   - the getter never miscasts a foreign user_data pointer
 */

#include "ui_utils.h"

#include "../test_fixtures.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "../catch_amalgamated.hpp"

using helix::ui::get_owned_user_string;
using helix::ui::set_owned_user_string;

TEST_CASE_METHOD(LVGLTestFixture, "set_owned_user_string round-trips a string",
                 "[ui_utils][l069]") {
    lv_obj_t* obj = lv_obj_create(lv_screen_active());
    REQUIRE(obj != nullptr);

    REQUIRE(set_owned_user_string(obj, "timelapse/benchy_20260101.mp4"));

    const char* got = get_owned_user_string(obj);
    REQUIRE(got != nullptr);
    REQUIRE(std::string(got) == "timelapse/benchy_20260101.mp4");

    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_owned_user_string handles the empty string",
                 "[ui_utils][l069]") {
    lv_obj_t* obj = lv_obj_create(lv_screen_active());
    REQUIRE(set_owned_user_string(obj, ""));

    const char* got = get_owned_user_string(obj);
    REQUIRE(got != nullptr); // owned, just empty — not the same as "absent"
    REQUIRE(std::string(got).empty());

    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_owned_user_string copies from a non-terminated view",
                 "[ui_utils][l069]") {
    lv_obj_t* obj = lv_obj_create(lv_screen_active());

    // A view into the middle of a larger buffer: the helper must copy exactly
    // size() bytes and terminate itself, not read to the source's NUL.
    const std::string backing = "prefix|payload|suffix";
    std::string_view view(backing.data() + 7, 7);
    REQUIRE(set_owned_user_string(obj, view));
    REQUIRE(std::string(get_owned_user_string(obj)) == "payload");

    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "get_owned_user_string is null-safe and never miscasts",
                 "[ui_utils][l069]") {
    REQUIRE(get_owned_user_string(nullptr) == nullptr);

    lv_obj_t* bare = lv_obj_create(lv_screen_active());
    REQUIRE(get_owned_user_string(bare) == nullptr); // slot empty

    // L069: a slot owned by somebody else (here a stand-in for ui_button's
    // button_data_t*) must read back as "no owned string", not as a char*.
    int foreign = 42;
    lv_obj_t* contested = lv_obj_create(lv_screen_active());
    lv_obj_set_user_data(contested, &foreign);
    REQUIRE(get_owned_user_string(contested) == nullptr);

    lv_obj_set_user_data(contested, nullptr); // don't let teardown free a stack int
    lv_obj_delete(contested);
    lv_obj_delete(bare);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_owned_user_string refuses a contested slot",
                 "[ui_utils][l069]") {
    int foreign = 42;
    lv_obj_t* obj = lv_obj_create(lv_screen_active());
    lv_obj_set_user_data(obj, &foreign);

    REQUIRE_FALSE(set_owned_user_string(obj, "mine"));
    // The foreign pointer must survive untouched — never freed, never replaced.
    REQUIRE(lv_obj_get_user_data(obj) == &foreign);

    lv_obj_set_user_data(obj, nullptr);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_owned_user_string replaces its own previous string",
                 "[ui_utils][l069]") {
    lv_obj_t* obj = lv_obj_create(lv_screen_active());

    REQUIRE(set_owned_user_string(obj, "first"));
    const uint32_t events_after_first = lv_obj_get_event_count(obj);

    REQUIRE(set_owned_user_string(obj, "second-and-longer"));
    REQUIRE(std::string(get_owned_user_string(obj)) == "second-and-longer");

    // The cleanup handler must be registered exactly once, or teardown would
    // double-free the (already nulled) slot on the second pass.
    REQUIRE(lv_obj_get_event_count(obj) == events_after_first);

    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_owned_user_string reports allocation failure",
                 "[ui_utils][l069]") {
    lv_obj_t* obj = lv_obj_create(lv_screen_active());

    // SIZE_MAX-1: `len + 1` does not overflow, but the allocation cannot be
    // satisfied. Pre-helper code memcpy'd into the NULL result and crashed.
    // The pointer is never dereferenced because the allocation fails first.
    std::string_view unallocatable(reinterpret_cast<const char*>(0x1000),
                                   std::numeric_limits<size_t>::max() - 1);
    REQUIRE_FALSE(set_owned_user_string(obj, unallocatable));
    REQUIRE(lv_obj_get_user_data(obj) == nullptr); // slot untouched
    REQUIRE(get_owned_user_string(obj) == nullptr);

    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_owned_user_string rejects a length that would overflow",
                 "[ui_utils][l069]") {
    lv_obj_t* obj = lv_obj_create(lv_screen_active());

    // len + 1 wraps to 0 -> lv_malloc(0) can succeed -> memcpy of SIZE_MAX bytes.
    std::string_view overflowing(reinterpret_cast<const char*>(0x1000),
                                 std::numeric_limits<size_t>::max());
    REQUIRE_FALSE(set_owned_user_string(obj, overflowing));
    REQUIRE(lv_obj_get_user_data(obj) == nullptr);

    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_owned_user_string rejects a null object",
                 "[ui_utils][l069]") {
    REQUIRE_FALSE(set_owned_user_string(nullptr, "anything"));
}

TEST_CASE_METHOD(LVGLTestFixture, "owned user string is released when the object dies",
                 "[ui_utils][l069]") {
    lv_obj_t* obj = lv_obj_create(lv_screen_active());
    REQUIRE(set_owned_user_string(obj, "released-on-delete"));

    // The cleanup handler must null the slot as well as free it, so a second
    // DELETE pass (LVGL delivers DELETE to the subtree) cannot double-free.
    lv_obj_send_event(obj, LV_EVENT_DELETE, nullptr);
    REQUIRE(lv_obj_get_user_data(obj) == nullptr);
    REQUIRE(get_owned_user_string(obj) == nullptr);

    lv_obj_delete(obj); // real delete: handler runs again, must be a no-op
}
