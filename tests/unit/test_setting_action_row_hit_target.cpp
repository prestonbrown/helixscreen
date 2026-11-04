// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

// A press must land on the row itself, or on the info button where it shows.
// When anything else inside takes it, PRESSED reaches the row only by bubbling,
// and LVGL clears it from the pointer's own target when a scroll starts: the
// row keeps its pressed transform for the whole drag and renders through a
// transform layer every frame.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "setting_action_row: a press hits the row, or the info button where it shows",
                 "[setting_action_row]") {
    lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
    REQUIRE(bp != nullptr);
    const int saved_bp = lv_subject_get_int(bp);
    lv_xml_register_event_cb(nullptr, "test_hit_target_noop", [](lv_event_t*) {});

    // Breakpoint 0 hides the description and shows the info button; 3 is the reverse.
    const int breakpoint = GENERATE(0, 3);
    CAPTURE(breakpoint);
    lv_subject_set_int(bp, breakpoint);

    const char* attrs[] = {"label",       "Row",       "icon",
                           "cog",         "callback",  "test_hit_target_noop",
                           "description", "Some help", nullptr};
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "setting_action_row", attrs));
    REQUIRE(row != nullptr);
    lv_obj_set_width(row, 600);
    process_lvgl(5);
    lv_obj_update_layout(test_screen());

    lv_obj_t* info = lv_obj_find_by_name(row, "info_btn");
    REQUIRE(info != nullptr);
    bool info_shown = true;
    for (lv_obj_t* o = info; o && o != row; o = lv_obj_get_parent(o))
        info_shown = info_shown && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    REQUIRE(info_shown == (breakpoint == 0));
    lv_area_t info_area;
    lv_obj_get_coords(info, &info_area);

    lv_area_t a;
    lv_obj_get_coords(row, &a);
    bool hit_info = false;
    for (int32_t x = a.x1 + 1; x < a.x2; x += 4) {
        for (int32_t y = a.y1 + 1; y < a.y2; y += 4) {
            lv_point_t p{x, y};
            lv_obj_t* hit = lv_indev_search_obj(row, &p);
            const bool over_info = info_shown && x >= info_area.x1 && x <= info_area.x2 &&
                                   y >= info_area.y1 && y <= info_area.y2;
            CAPTURE(x, y);
            if (over_info) {
                // The info button's own icon passes its press up to the button.
                REQUIRE((hit == info || lv_obj_get_parent(hit) == info));
                hit_info = true;
            } else {
                REQUIRE(hit == row);
            }
        }
    }
    CHECK(hit_info == info_shown);

    lv_obj_delete(row);
    lv_subject_set_int(bp, saved_bp);
    helix::ui::UpdateQueue::instance().drain();
}
