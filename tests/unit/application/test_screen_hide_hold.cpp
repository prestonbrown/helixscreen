// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl_test_fixture.h"
#include "screen_hide_hold.h"
#include "test_helpers/screen_hide_hold_test_access.h"

#include "../../catch_amalgamated.hpp"

using helix::ScreenHideHold;

namespace {

bool is_hidden(lv_obj_t* obj) {
    return lv_obj_is_hidden(obj);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "ScreenHideHold hides on the first acquire and shows on the last release",
                 "[application][display][screen_hide]") {
    ScreenHideHold hold;
    REQUIRE_FALSE(is_hidden(test_screen()));

    hold.acquire(test_screen());
    CHECK(hold.is_held());
    CHECK(is_hidden(test_screen()));

    hold.acquire(test_screen());
    hold.release();
    CHECK(hold.is_held());
    CHECK(is_hidden(test_screen()));

    hold.release();
    CHECK_FALSE(hold.is_held());
    CHECK_FALSE(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreenHideHold ignores a release with no acquire",
                 "[application][display][screen_hide]") {
    ScreenHideHold hold;
    hold.release();
    CHECK_FALSE(hold.is_held());

    // The unmatched release banked nothing: one acquire holds, one release lets go.
    hold.acquire(test_screen());
    CHECK(hold.is_held());
    CHECK(is_hidden(test_screen()));
    hold.release();
    CHECK_FALSE(hold.is_held());
    CHECK_FALSE(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreenHideHold never unhides a screen it did not hide",
                 "[application][display][screen_hide]") {
    ScreenHideHold hold;
    lv_obj_set_hidden(test_screen(), true);

    hold.acquire(test_screen());
    REQUIRE(hold.is_held());
    hold.release();

    CHECK_FALSE(hold.is_held());
    CHECK(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreenHideHold leaves a screen loaded mid-hold alone",
                 "[application][display][screen_hide]") {
    ScreenHideHold hold;
    lv_obj_t* first = test_screen();
    hold.acquire(first);
    REQUIRE(is_hidden(first));

    lv_obj_t* second = lv_obj_create(nullptr);
    lv_screen_load(second);
    REQUIRE(lv_screen_active() == second);

    // A nested acquire only counts; it does not hide the screen now active.
    hold.acquire(lv_screen_active());
    CHECK_FALSE(is_hidden(second));
    hold.release();

    // Hidden by someone else while the hold is still out: the final release is not
    // about this screen.
    lv_obj_set_hidden(second, true);
    hold.release();
    CHECK(is_hidden(second));
    CHECK_FALSE(is_hidden(first));

    lv_screen_load(first);
    lv_obj_delete(second);
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreenHideHold skips a screen deleted mid-hold",
                 "[application][display][screen_hide]") {
    ScreenHideHold hold;
    lv_obj_t* doomed = lv_obj_create(nullptr);
    hold.acquire(doomed);
    REQUIRE(is_hidden(doomed));
    lv_obj_delete(doomed);

    // Must not write to the freed screen.
    hold.release();
    CHECK_FALSE(hold.is_held());

    // And the next hold starts clean.
    hold.acquire(test_screen());
    CHECK(is_hidden(test_screen()));
    hold.release();
    CHECK_FALSE(is_hidden(test_screen()));
}

TEST_CASE("a screen hold leaked by one LVGL fixture does not reach the next",
          "[application][display][screen_hide]") {
    ScreenHideHold& hold = helix::active_screen_hide_hold();
    {
        LVGLTestFixture leaking;
        hold.acquire(leaking.test_screen());
        REQUIRE(hold.is_held());
    }

    LVGLTestFixture next;
    const bool held_on_entry = hold.is_held();
    hold.acquire(next.test_screen());
    const bool first_acquire_hid = is_hidden(next.test_screen());
    // Leave nothing held for the tests after this one, whatever the outcome.
    helix::ScreenHideHoldTestAccess::reset(hold);

    CHECK_FALSE(held_on_entry);
    CHECK(first_acquire_hid);
}
