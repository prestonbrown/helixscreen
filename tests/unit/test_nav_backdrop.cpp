// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_backdrop.cpp
 * @brief OverlayBackdrop: per-overlay backdrops, the keyboard-dismiss latch, release
 */

#include "ui_nav_backdrop.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/widget_in_tree.h"

#include "../catch_amalgamated.hpp"

using helix::ui::OverlayBackdrop;

namespace {

struct BackdropFixture : public LVGLUITestFixture {
    lv_obj_t* make() {
        return lv_obj_create(test_screen());
    }
    static void track(OverlayBackdrop& b, lv_obj_t* overlay, lv_obj_t* backdrop) {
        NavigationManagerTestAccess::track_nested_backdrop(b, overlay, backdrop);
    }
};

} // namespace

TEST_CASE_METHOD(BackdropFixture, "The keyboard-dismiss latch is consumed once",
                 "[navigation][nav_backdrop]") {
    OverlayBackdrop b;
    CHECK_FALSE(b.take_keyboard_dismiss());

    b.note_press(true);
    CHECK(b.take_keyboard_dismiss());
    CHECK_FALSE(b.take_keyboard_dismiss());

    b.note_press(false);
    CHECK_FALSE(b.take_keyboard_dismiss());
}

TEST_CASE_METHOD(BackdropFixture, "retire deletes one overlay's backdrop, deferred",
                 "[navigation][nav_backdrop]") {
    OverlayBackdrop b;
    lv_obj_t *a = make(), *a_bd = make(), *c = make(), *c_bd = make();
    track(b, a, a_bd);
    track(b, c, c_bd);

    b.retire(a);
    CHECK(helix::test::widget_in_tree(a_bd));
    process_lvgl(50);
    CHECK_FALSE(helix::test::widget_in_tree(a_bd));
    CHECK(helix::test::widget_in_tree(c_bd));

    b.retire(a); // already gone: no-op
}

TEST_CASE_METHOD(BackdropFixture, "retire_all deletes every overlay's backdrop",
                 "[navigation][nav_backdrop]") {
    OverlayBackdrop b;
    lv_obj_t *a = make(), *a_bd = make(), *c = make(), *c_bd = make();
    track(b, a, a_bd);
    track(b, c, c_bd);

    b.retire_all();
    process_lvgl(50);

    CHECK_FALSE(helix::test::widget_in_tree(a_bd));
    CHECK_FALSE(helix::test::widget_in_tree(c_bd));
}

TEST_CASE_METHOD(BackdropFixture, "scrub forgets a backdrop without deleting it",
                 "[navigation][nav_backdrop]") {
    OverlayBackdrop b;
    lv_obj_t *a = make(), *a_bd = make();
    track(b, a, a_bd);

    b.scrub(a);
    b.retire(a);
    process_lvgl(50);

    CHECK(helix::test::widget_in_tree(a_bd));
}

TEST_CASE_METHOD(BackdropFixture, "rekey moves a backdrop to the rebuilt overlay",
                 "[navigation][nav_backdrop]") {
    OverlayBackdrop b;
    lv_obj_t *old_root = make(), *new_root = make(), *bd = make();
    track(b, old_root, bd);

    b.rekey(old_root, new_root);
    b.retire(old_root);
    process_lvgl(50);
    CHECK(helix::test::widget_in_tree(bd));

    b.retire(new_root);
    process_lvgl(50);
    CHECK_FALSE(helix::test::widget_in_tree(bd));
}
