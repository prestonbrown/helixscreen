// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_breakpoint.h"
#include "ui_button.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_breakpoint.h"

#include "../catch_amalgamated.hpp"

namespace {

// The Operations card at the landscape row heights filament_panel.xml passes:
// 26 at Micro, 42 at Tiny. The card is as wide as the panel's right column.
lv_obj_t* make_operations_card(lv_obj_t* parent, const char* row_height) {
    const char* attrs[] = {"row_height", row_height, "length_height", row_height,
                           "width",      "250",      nullptr};
    lv_obj_t* card =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "filament_operations_group", attrs));
    REQUIRE(card != nullptr);
    lv_obj_update_layout(parent);
    return card;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Operations button labels fit a 480x272 row",
                 "[filament][layout]") {
    helix::test::ScopedBreakpoint micro(UiBreakpoint::Micro);
    lv_obj_t* card = make_operations_card(test_screen(), "26");
    for (const char* name : {"btn_load", "btn_unload", "btn_purge"}) {
        INFO(name);
        lv_obj_t* btn = lv_obj_find_by_name(card, name);
        REQUIRE(btn != nullptr);
        lv_obj_t* label = ui_button_get_label(btn);
        REQUIRE(label != nullptr);
        lv_area_t b;
        lv_area_t l;
        lv_obj_get_coords(btn, &b);
        lv_obj_get_coords(label, &l);
        REQUIRE(lv_area_get_height(&b) == 26);
        CHECK(l.y1 >= b.y1);
        CHECK(l.y2 <= b.y2);
    }
    lv_obj_delete(card);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Operations buttons keep their icons at 480x320",
                 "[filament][layout]") {
    helix::test::ScopedBreakpoint tiny(UiBreakpoint::Tiny);
    lv_obj_t* card = make_operations_card(test_screen(), "42");
    for (const char* name : {"btn_load", "btn_unload", "btn_purge"}) {
        INFO(name);
        lv_obj_t* icon = ui_button_get_icon(lv_obj_find_by_name(card, name));
        REQUIRE(icon != nullptr);
        CHECK_FALSE(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
    }
    lv_obj_delete(card);
}
