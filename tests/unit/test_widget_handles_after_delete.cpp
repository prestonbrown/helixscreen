// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_handles_after_delete.cpp
 * @brief Long-lived holders of widgets stay usable after those widgets die
 *        (prestonbrown/helixscreen#1983)
 */

#include "ui_ams_detail.h"
#include "ui_bypass_spool_widget.h"
#include "ui_effects.h"

#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

TEST_CASE_METHOD(LVGLTestFixture, "always-on-top chrome skips a widget that died",
                 "[widget_handles]") {
    lv_obj_t* doomed = lv_obj_create(test_screen());
    lv_obj_t* kept = lv_obj_create(test_screen());
    int doomed_raises = 0;
    int kept_raises = 0;
    add_always_on_top(doomed, [&doomed_raises] { ++doomed_raises; });
    add_always_on_top(kept, [&kept_raises] { ++kept_raises; });

    SECTION("raising the chrome") {
        lv_obj_delete(doomed);
        set_chrome_covered(true);
        set_chrome_covered(false);
        CHECK(doomed_raises == 0);
        CHECK(kept_raises == 1);

        // Raising over another widget takes the parent-matching path.
        bring_to_front(lv_obj_create(test_screen()));
        CHECK(doomed_raises == 0);
        CHECK(kept_raises == 2);
    }

    SECTION("a chrome hide that spans the delete") {
        {
            ScopedHideChrome hide;
            CHECK(lv_obj_has_flag(kept, LV_OBJ_FLAG_HIDDEN));
            lv_obj_delete(doomed);
        }
        CHECK_FALSE(lv_obj_has_flag(kept, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("a hide made after the delete") {
        lv_obj_delete(doomed);
        {
            ScopedHideChrome hide;
            CHECK(lv_obj_has_flag(kept, LV_OBJ_FLAG_HIDDEN));
        }
        CHECK_FALSE(lv_obj_has_flag(kept, LV_OBJ_FLAG_HIDDEN));
    }

    remove_always_on_top(nullptr);
    remove_always_on_top(kept);
}

TEST_CASE_METHOD(LVGLTestFixture, "bypass spool visibility skips parts that died",
                 "[widget_handles][ams]") {
    BypassSpoolWidgets w = bypass_spool_create(test_screen(), nullptr, nullptr);
    REQUIRE(w.valid());

    lv_obj_delete(w.material_label);
    lv_obj_delete(w.box);

    bypass_spool_set_visible(w, false);
    CHECK(lv_obj_has_flag(w.bypass_label, LV_OBJ_FLAG_HIDDEN));
    bypass_spool_set_visible(w, true);
    CHECK_FALSE(lv_obj_has_flag(w.bypass_label, LV_OBJ_FLAG_HIDDEN));

    bypass_spool_destroy(w);
}

TEST_CASE_METHOD(LVGLTestFixture, "AMS detail teardown skips a slot that died",
                 "[widget_handles][ams]") {
    AmsDetailWidgets w;
    WidgetRef slots[2];
    slots[0] = lv_obj_create(test_screen());
    slots[1] = lv_obj_create(test_screen());
    lv_obj_t* kept = slots[1];
    int count = 2;

    lv_obj_delete(slots[0]);
    REQUIRE(slots[0].get() == nullptr);

    ams_detail_destroy_slots(w, slots, count);

    CHECK(count == 0);
    CHECK(slots[1].get() == nullptr);
    // The survivor went to the condemned container rather than staying on screen.
    CHECK(lv_obj_get_parent(kept) != test_screen());
    process_lvgl(50);
}
