// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_usb_visibility.cpp
 * @brief Widget-level regression coverage for the Rule #2 fix in
 *        ui_print_select_usb_source.cpp (imperative lv_obj_add_flag/
 *        remove_flag on source_selector -> print_source_usb_present /
 *        print_source_moonraker_usb_access + a bind_flag_if in
 *        print_select_panel.xml).
 *
 * test_metadata_and_usb_symlink.cpp already covers the C++ side (that the
 * class writes the right subjects, mutation-verified). This file builds the
 * real print_select_panel component and reads the real widget's `hidden`
 * flag, so it's the binding itself under test, not just the subject that
 * feeds it.
 */

#include "ui_panel_print_select.h"

#include "../test_fixtures.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

lv_obj_t* source_selector(lv_obj_t* panel_root) {
    lv_obj_t* w = lv_obj_find_by_name(panel_root, "source_selector");
    REQUIRE(w != nullptr);
    return w;
}

bool is_hidden(lv_obj_t* w) {
    return lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "print_select_panel's source_selector visibility follows the "
                 "usb_present / moonraker_access binding",
                 "[ui_integration][print_select][usb]") {
    REQUIRE(register_component("print_select_panel"));

    PrintSelectPanel panel(state(), &api());
    panel.init_subjects(); // registers print_source_usb_present et al. before XML creation

    lv_obj_t* panel_obj = create_component("print_select_panel");
    REQUIRE(panel_obj != nullptr);

    panel.setup(panel_obj, test_screen());
    lv_obj_update_layout(test_screen());
    process_lvgl(20);

    lv_obj_t* selector = source_selector(panel_obj);

    // print_source_moonraker_usb_access is a process-wide static — another
    // test case earlier in this binary may have already left it non-default
    // (print_source_usb_present is reset below via the real on_drive_removed()
    // API instead, since that's the thing under test). Reset explicitly
    // rather than assume a pristine default: this test passed in isolation
    // but failed once run alongside test_metadata_and_usb_symlink.cpp's cases
    // for exactly this reason.
    //
    // check_moonraker_usb_symlink() (the real trigger for this subject) is
    // private and goes through an async Moonraker file-list call; the
    // class-level tests in test_metadata_and_usb_symlink.cpp already
    // mutation-verify that set_moonraker_has_usb_access() writes this exact
    // subject, so driving the subject directly here isolates the binding
    // itself, which is what this file is for.
    lv_subject_t* access_subject = lv_xml_get_subject(nullptr, "print_source_moonraker_usb_access");
    REQUIRE(access_subject != nullptr);
    lv_subject_set_int(access_subject, 0);

    // 1. Neither a drive present nor Moonraker access -> hidden (the default
    //    both subjects start at, and the state a fresh boot begins in).
    panel.on_usb_drive_removed();
    process_lvgl(5);
    REQUIRE(is_hidden(selector));

    // 2. A drive is inserted, no Moonraker access -> visible. Driven through
    //    the real public entry point a background USB event uses, not a
    //    direct subject write.
    panel.on_usb_drive_inserted();
    process_lvgl(5);
    REQUIRE_FALSE(is_hidden(selector));

    // 3. Moonraker gains symlink access while the drive is still present ->
    //    hidden again, even though the drive never left. This is a compound
    //    condition: a single bind_flag_if_eq on
    //    print_source_usb_present alone could never express this branch.
    lv_subject_set_int(access_subject, 1);
    process_lvgl(5);
    REQUIRE(is_hidden(selector));

    // 4. Moonraker access is revoked, drive still present -> visible again.
    //    The selector needs a path back to visible here.
    lv_subject_set_int(access_subject, 0);
    process_lvgl(5);
    REQUIRE_FALSE(is_hidden(selector));

    // 5. Drive removed while Moonraker access is also absent (already
    //    covered by state 1, repeated here to confirm removal from state 4
    //    lands on the same hidden result rather than some stuck state).
    panel.on_usb_drive_removed();
    process_lvgl(5);
    REQUIRE(is_hidden(selector));
}

TEST_CASE_METHOD(XMLTestFixture,
                 "print_select_panel shows the empty state in place of both views while no file "
                 "is listed",
                 "[ui_integration][print_select]") {
    REQUIRE(register_component("print_select_panel"));
    PrintSelectPanel panel(state(), &api());
    panel.init_subjects();
    lv_obj_t* panel_obj = create_component("print_select_panel");
    REQUIRE(panel_obj != nullptr);
    panel.setup(panel_obj, test_screen());
    auto hidden = [&](const char* name) {
        lv_obj_t* w = lv_obj_find_by_name(panel_obj, name);
        REQUIRE(w != nullptr);
        return is_hidden(w);
    };
    for (int view = 0; view < 2; ++view) { // list view, then back to cards
        panel.toggle_view();
        CHECK_FALSE(hidden("empty_state_container"));
        CHECK(hidden("card_view_container"));
        CHECK(hidden("list_view_container"));
    }
}
