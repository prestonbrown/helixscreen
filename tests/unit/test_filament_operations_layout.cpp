// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_breakpoint.h"
#include "ui_button.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/label_fits.h"
#include "../test_helpers/scoped_geometry.h"
#include "../test_helpers/scoped_int_subject.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// One landscape size: the card as filament_panel.xml sizes it there (the
/// right column's width, and the height the column leaves under Material), the
/// least row height it passes, and where the layout puts Purge and Clean.
struct OpsSize {
    const char* what;
    int32_t screen_w;
    int32_t screen_h;
    const char* width;
    const char* height;
    const char* row_height;
    bool three_rows; ///< Purge and Clean share a row of their own
    bool icons;      ///< the action buttons show an icon above the label
};

constexpr OpsSize kSizes[] = {
    {"800x480", 800, 480, "413", "287", "52", true, true},
    {"480x320", 480, 320, "243", "192", "40", true, false},
    {"480x272", 480, 272, "250", "144", "26", false, false},
};

lv_obj_t* find(lv_obj_t* card, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(card, name);
    REQUIRE(obj != nullptr);
    return obj;
}

/// Whether @p obj sits inside the card's row named @p row.
bool in_row(lv_obj_t* obj, lv_obj_t* card, const char* row) {
    lv_obj_t* r = find(card, row);
    for (lv_obj_t* p = lv_obj_get_parent(obj); p != nullptr && p != card;
         p = lv_obj_get_parent(p)) {
        if (p == r) {
            return true;
        }
    }
    return false;
}

} // namespace

// Each landscape size has its own arrangement: three rows where the column has
// the height, and at 480x272 two, with Clean beside the speed. Every control
// stays inside the card, no shorter than the row height asked for, with its
// label whole.
TEST_CASE_METHOD(LVGLUITestFixture, "Operations controls fit the card at each landscape size",
                 "[filament][layout]") {
    helix::test::ScopedIntSubject clean_shown("filament_clean_nozzle_shown", 1);
    helix::test::ScopedIntSubject single_head("target_tool_multi_head", 0);
    for (const OpsSize& s : kSizes) {
        INFO(s.what);
        helix::test::ScopedGeometry screen(s.screen_w, s.screen_h);
        const char* attrs[] = {"row_height", s.row_height, "length_height", "40",   "width",
                               s.width,      "height",     s.height,        nullptr};
        lv_obj_t* card = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "filament_operations_group", attrs));
        REQUIRE(card != nullptr);
        lv_obj_update_layout(test_screen());

        lv_obj_t* purge = find(card, "btn_purge");
        lv_obj_t* clean = find(card, "btn_clean_nozzle");
        CHECK(in_row(purge, card, "purge_clean_row") == s.three_rows);
        CHECK(in_row(purge, card, "filament_change_row") == !s.three_rows);
        CHECK(in_row(clean, card, "purge_clean_row") == s.three_rows);
        CHECK(in_row(clean, card, "operations_heading_row") == !s.three_rows);
        CHECK(lv_obj_is_hidden(find(card, "purge_clean_row")) == !s.three_rows);

        const int min_h = std::stoi(s.row_height);
        lv_area_t c;
        lv_obj_get_content_coords(card, &c);
        for (const char* name : {"btn_load", "btn_unload", "btn_purge", "btn_clean_nozzle",
                                 "extrude_length_0", "extrude_length_1", "extrude_length_2",
                                 "btn_extrude", "btn_retract", "extrude_speed_dropdown"}) {
            INFO(name);
            lv_obj_t* obj = find(card, name);
            REQUIRE_FALSE(lv_obj_is_hidden(obj));
            lv_area_t a;
            lv_obj_get_coords(obj, &a);
            CHECK(a.x1 >= c.x1);
            CHECK(a.x2 <= c.x2);
            CHECK(a.y1 >= c.y1);
            CHECK(a.y2 <= c.y2);
            // The speed, and Clean beside it, are as tall as the heading allows.
            const bool in_heading = in_row(obj, card, "operations_heading_row");
            CHECK(lv_area_get_height(&a) >= (in_heading ? 40 : min_h));
        }
        for (const char* name : {"btn_load", "btn_unload", "btn_purge", "btn_clean_nozzle"}) {
            lv_obj_t* btn = find(card, name);
            helix::test::require_label_fits(ui_button_get_label(btn),
                                            std::string(s.what) + " " + name);
            if (std::string(name) != "btn_clean_nozzle" || s.three_rows) {
                lv_obj_t* icon = ui_button_get_icon(btn);
                REQUIRE(icon != nullptr);
                CHECK(lv_obj_is_hidden(icon) == !s.icons);
            }
        }
        lv_obj_delete(card);
    }
}

// A 480x272 toolchanger has no heading (Pick up / Park and the speed sit beside
// the heads), so Clean stays in the first row.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "Operations keeps Clean in the first row on a 480x272 toolchanger",
                 "[filament][layout]") {
    helix::test::ScopedIntSubject clean_shown("filament_clean_nozzle_shown", 1);
    helix::test::ScopedIntSubject multi_head("target_tool_multi_head", 1);
    helix::test::ScopedGeometry screen(480, 272);
    const char* attrs[] = {"row_height", "26",     "length_height", "40",   "width",
                           "250",        "height", "144",           nullptr};
    lv_obj_t* card =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "filament_operations_group", attrs));
    REQUIRE(card != nullptr);
    lv_obj_update_layout(test_screen());
    CHECK(in_row(find(card, "btn_clean_nozzle"), card, "filament_change_row"));
    CHECK(in_row(find(card, "extrude_speed_dropdown"), card, "tool_card_row"));
    lv_obj_delete(card);
}

// Purge and Clean each have a declaration per place. Whichever copy a layout
// builds carries the same disabled rules: Purge while the nozzle is too cold,
// while an operation runs, and with no filament to purge; Clean while an
// operation runs.
TEST_CASE_METHOD(LVGLUITestFixture, "Operations Purge and Clean disable on every copy",
                 "[filament][layout]") {
    helix::test::ScopedIntSubject clean_shown("filament_clean_nozzle_shown", 1);
    helix::test::ScopedIntSubject cold("filament_safety_warning_visible", 0);
    helix::test::ScopedIntSubject busy("filament_operation_in_progress", 0);
    helix::test::ScopedIntSubject nothing_loaded("filament_unload_disabled", 0);

    struct Layout {
        const char* what;
        int32_t w, h;
        const char* card_w;
        const char* card_h;
        int multi_head;
        const char* purge_row;
        const char* clean_row;
    };
    const Layout layouts[] = {
        {"800x480", 800, 480, "413", "287", 0, "purge_clean_row", "purge_clean_row"},
        {"480x272", 480, 272, "250", "144", 0, "filament_change_row", "operations_heading_row"},
        {"480x272 toolchanger", 480, 272, "250", "144", 1, "filament_change_row",
         "filament_change_row"},
    };
    for (const Layout& l : layouts) {
        INFO(l.what);
        helix::test::ScopedGeometry screen(l.w, l.h);
        helix::test::ScopedIntSubject heads("target_tool_multi_head", l.multi_head);
        const char* attrs[] = {"row_height", "26",     "length_height", "40",   "width",
                               l.card_w,     "height", l.card_h,        nullptr};
        lv_obj_t* card = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "filament_operations_group", attrs));
        REQUIRE(card != nullptr);
        lv_obj_t* purge = find(card, "btn_purge");
        lv_obj_t* clean = find(card, "btn_clean_nozzle");
        REQUIRE(in_row(purge, card, l.purge_row));
        REQUIRE(in_row(clean, card, l.clean_row));

        auto disabled = [](lv_obj_t* o) { return lv_obj_has_state(o, LV_STATE_DISABLED); };
        CHECK_FALSE(disabled(purge));
        CHECK_FALSE(disabled(clean));
        for (const char* gate : {"filament_safety_warning_visible",
                                 "filament_operation_in_progress", "filament_unload_disabled"}) {
            INFO(gate);
            lv_subject_t* s = lv_xml_get_subject(nullptr, gate);
            REQUIRE(s != nullptr);
            lv_subject_set_int(s, 1);
            CHECK(disabled(purge));
            CHECK(disabled(clean) == (std::string(gate) == "filament_operation_in_progress"));
            lv_subject_set_int(s, 0);
            CHECK_FALSE(disabled(purge));
        }

        // Cleaning a cold nozzle is valid: with the cold-extrusion cover up,
        // a tap on Clean still reaches it.
        lv_subject_set_int(lv_xml_get_subject(nullptr, "filament_safety_warning_visible"), 1);
        lv_obj_update_layout(test_screen());
        REQUIRE_FALSE(lv_obj_is_hidden(find(card, "safety_warning")));
        lv_area_t a;
        lv_obj_get_coords(clean, &a);
        lv_point_t center{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        lv_obj_t* hit = lv_indev_search_obj(test_screen(), &center);
        bool reaches_clean = false;
        for (lv_obj_t* o = hit; o != nullptr; o = lv_obj_get_parent(o)) {
            reaches_clean = reaches_clean || o == clean;
        }
        CHECK(reaches_clean);
        CHECK_FALSE(disabled(clean));
        lv_subject_set_int(lv_xml_get_subject(nullptr, "filament_safety_warning_visible"), 0);
        lv_obj_delete(card);
    }
}
