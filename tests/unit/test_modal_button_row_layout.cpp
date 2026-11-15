// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// modal_button_row's buttons hold their content inside their own box at every
// screen size: the row fixes their height at button_height, which at 480x272
// leaves no room for the theme's vertical button padding.

#include "ui_breakpoint.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/scoped_geometry.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "modal_button_row: no button's content overflows its box",
                 "[modal_button_row][ui_integration][2024]") {
    struct Size {
        int32_t w, h;
        UiBreakpoint bp;
    };
    const Size size =
        GENERATE(Size{480, 272, UiBreakpoint::Micro}, Size{800, 480, UiBreakpoint::Medium});
    CAPTURE(size.w, size.h);
    helix::test::ScopedGeometry geometry(size.w, size.h);
    helix::test::ScopedBreakpoint breakpoint(size.bp);

    const char* attrs[] = {
        "primary_text",    "Delete",        "secondary_text", "Cancel",          "tertiary_text",
        "Reset",           "hide_tertiary", "false",          "quaternary_text", "More",
        "hide_quaternary", "false",         nullptr};
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "modal_button_row", attrs));
    REQUIRE(row != nullptr);
    lv_obj_set_width(row, size.w * 7 / 10);
    lv_obj_update_layout(row);

    for (const char* name : {"btn_primary", "btn_secondary", "btn_tertiary", "btn_quaternary"}) {
        CAPTURE(name);
        lv_obj_t* btn = lv_obj_find_by_name(row, name);
        REQUIRE(btn != nullptr);
        REQUIRE_FALSE(lv_obj_is_hidden(btn));
        CAPTURE(lv_obj_get_height(btn), lv_obj_get_content_height(btn));
        CHECK(lv_obj_get_scroll_top(btn) <= 0);
        CHECK(lv_obj_get_scroll_bottom(btn) <= 0);
        lv_area_t box;
        lv_obj_get_coords(btn, &box);
        for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
            lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
            if (lv_obj_is_hidden(child)) {
                continue;
            }
            lv_area_t c;
            lv_obj_get_coords(child, &c);
            CAPTURE(i, c.y1, c.y2, box.y1, box.y2);
            CHECK(c.y1 >= box.y1);
            CHECK(c.y2 <= box.y2);
        }
    }

    lv_obj_delete(row);
}
