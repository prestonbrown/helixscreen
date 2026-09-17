// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_list_responsive.cpp
 * @brief The list view's Size/Modified columns must drop below MEDIUM, and the
 *        header must drop them in lockstep with the rows.
 *
 * The five columns carry 370px of fixed width. A 480px-wide canvas has ~400px
 * of row content, so on those boards the only flex-grow track (the filename)
 * is what absorbs the shortfall — and a starved track collapses to zero, at
 * which point `long_mode="dots"` can no longer clip and the label wraps
 * instead, growing a `height="content"` row to several hundred px.
 *
 * Header cells live in print_select_panel.xml and row cells in
 * print_file_list_row.xml, two files with no shared widget. Their visibility
 * is asserted to MATCH rather than merely to be correct, so editing one file
 * alone fails here instead of shipping a header whose columns name data the
 * rows no longer show.
 */

#include "ui_panel_print_select.h"

#include "../test_fixtures.h"
#include "../test_helpers/scoped_breakpoint.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

bool is_hidden(lv_obj_t* w) {
    return lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* by_name(lv_obj_t* root, const char* name) {
    lv_obj_t* w = lv_obj_find_by_name(root, name);
    REQUIRE(w != nullptr);
    return w;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "print-select list drops Size/Modified below MEDIUM",
                 "[ui_integration][print_select][responsive]") {
    REQUIRE(register_component("print_select_panel"));
    REQUIRE(register_component("print_file_list_row"));

    PrintSelectPanel panel(state(), &api());
    panel.init_subjects();

    lv_obj_t* panel_obj = create_component("print_select_panel");
    REQUIRE(panel_obj != nullptr);
    panel.setup(panel_obj, test_screen());

    lv_obj_t* row = create_component("print_file_list_row");
    REQUIRE(row != nullptr);

    lv_obj_update_layout(test_screen());
    process_lvgl(20);

    lv_obj_t* h_size = by_name(panel_obj, "header_size");
    lv_obj_t* h_modified = by_name(panel_obj, "header_modified");
    lv_obj_t* h_status = by_name(panel_obj, "header_status");
    lv_obj_t* h_time = by_name(panel_obj, "header_print_time");
    lv_obj_t* r_size = by_name(row, "row_size");
    lv_obj_t* r_modified = by_name(row, "row_modified");
    lv_obj_t* r_status = by_name(row, "row_status");
    lv_obj_t* r_time = by_name(row, "row_print_time");

    SECTION("narrow boards hide the two widest columns") {
        // MICRO = Qidi Q2 and Elegoo Centauri Carbon (480x272), TINY = Snapmaker
        // U1 (480x320). SMALL has no shipping board but sits on the same side of
        // the rule, so it is pinned here rather than left to drift.
        const auto bp = GENERATE(UiBreakpoint::Micro, UiBreakpoint::Tiny, UiBreakpoint::Small);
        helix::test::ScopedBreakpoint guard(bp);
        process_lvgl(5);

        CHECK(is_hidden(h_size));
        CHECK(is_hidden(h_modified));
        CHECK(is_hidden(r_size));
        CHECK(is_hidden(r_modified));

        // Filename, Status and Time are what a narrow board keeps: the name to
        // identify the file, the history mark, and the duration to decide on.
        CHECK_FALSE(is_hidden(h_status));
        CHECK_FALSE(is_hidden(h_time));
        CHECK_FALSE(is_hidden(r_status));
        CHECK_FALSE(is_hidden(r_time));
    }

    SECTION("boards with the width for it keep all five columns") {
        // MEDIUM = 800x480 (AD5M, Pi rig, CB1, and the rotated K1C/K2 panels),
        // LARGE = 1024x600 (SonicPad).
        const auto bp = GENERATE(UiBreakpoint::Medium, UiBreakpoint::Large);
        helix::test::ScopedBreakpoint guard(bp);
        process_lvgl(5);

        CHECK_FALSE(is_hidden(h_size));
        CHECK_FALSE(is_hidden(h_modified));
        CHECK_FALSE(is_hidden(r_size));
        CHECK_FALSE(is_hidden(r_modified));
    }

    SECTION("header and rows agree at every breakpoint") {
        const auto bp = GENERATE(UiBreakpoint::Micro, UiBreakpoint::Tiny, UiBreakpoint::Small,
                                 UiBreakpoint::Medium, UiBreakpoint::Large);
        helix::test::ScopedBreakpoint guard(bp);
        process_lvgl(5);

        CHECK(is_hidden(h_size) == is_hidden(r_size));
        CHECK(is_hidden(h_modified) == is_hidden(r_modified));
        CHECK(is_hidden(h_status) == is_hidden(r_status));
        CHECK(is_hidden(h_time) == is_hidden(r_time));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "print-select filename track has a floor",
                 "[ui_integration][print_select][responsive]") {
    REQUIRE(register_component("print_file_list_row"));

    lv_obj_t* row = create_component("print_file_list_row");
    REQUIRE(row != nullptr);

    // MEDIUM shows all five columns, so 370px of fixed width against a 300px row
    // leaves the grow track nothing to take. Without a floor LVGL clamps it to
    // zero and the label wraps, which is what turns a 28px row into a 280px one.
    helix::test::ScopedBreakpoint guard(UiBreakpoint::Medium);
    lv_obj_set_width(row, 300);
    lv_obj_update_layout(row);
    process_lvgl(5);

    lv_obj_t* filename = by_name(row, "row_filename");
    CHECK(lv_obj_get_width(filename) >= 120);
}
