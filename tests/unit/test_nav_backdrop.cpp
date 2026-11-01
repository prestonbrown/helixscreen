// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_backdrop.cpp
 * @brief OverlayBackdrop: per-overlay backdrops, the keyboard-dismiss latch, release
 */

#include "ui_nav_backdrop.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/snapshot_backdrops_mode.h"
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

TEST_CASE_METHOD(BackdropFixture, "A transition caught mid-way is re-taken once it ends",
                 "[navigation][nav_backdrop]") {
    SnapshotBackdropsMode snapshots(true);
    int retakes = 0;
    OverlayBackdrop b([&retakes] { ++retakes; });

    static const lv_style_prop_t kProps[] = {LV_STYLE_TRANSLATE_X, LV_STYLE_PROP_INV};
    lv_style_transition_dsc_t fade;
    lv_style_transition_dsc_init(&fade, kProps, lv_anim_path_linear, 100, 0, nullptr);
    lv_style_t style;
    lv_style_init(&style);
    lv_style_set_transition(&style, &fade);
    lv_obj_t* icon = make();
    lv_obj_add_style(icon, &style, LV_PART_MAIN);
    lv_obj_set_style_translate_x(icon, 20, LV_STATE_CHECKED);
    lv_refr_now(lv_display_get_default()); // LVGL skips transitions on an unrendered widget
    lv_obj_add_state(icon, LV_STATE_CHECKED);
    const auto noop = [](lv_event_t*) {};

    SECTION("mid-way") {
        process_lvgl(20);
        b.adopt(test_screen(), nullptr, noop);
        REQUIRE(b.primary() != nullptr);
        CHECK(retakes == 0);
        process_lvgl(150);
        CHECK(retakes == 1);
        process_lvgl(150);
        CHECK(retakes == 1);
    }
    SECTION("already settled") {
        process_lvgl(150);
        b.adopt(test_screen(), nullptr, noop);
        process_lvgl(300);
        CHECK(retakes == 0);
    }
    SECTION("released before it ends") {
        process_lvgl(20);
        b.adopt(test_screen(), nullptr, noop);
        b.delete_primary_now();
        process_lvgl(300);
        CHECK(retakes == 0);
    }

    b.delete_primary_now();
    lv_obj_delete(icon);
    lv_style_reset(&style);
}
