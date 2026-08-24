// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_setting_toggle_row_disabled.cpp
 * @brief Pins setting_toggle_row's `disabled` prop contract: the subject
 * named by `disabled` puts the switch into LV_STATE_DISABLED while 1 and
 * leaves it operable at 0.
 *
 * Used by the AMS Management overlay's "Keep Spool Info on Eject" row:
 * when AFC's own remember_spool retention owns the behavior
 * (ams_device_ops_printer_retains_spool_info), the toggle must show disabled
 * rather than silently doing nothing.
 */

#include "../lvgl_ui_test_fixture.h"
#include "lvgl.h"

#include "../catch_amalgamated.hpp"

namespace {
void no_op_toggle_cb(lv_event_t*) {}
} // namespace

namespace {

class ToggleRowDisabledFixture : public LVGLUITestFixture {
  public:
    lv_subject_t val_subject;
    lv_subject_t dis_subject;

    ToggleRowDisabledFixture() {
        lv_xml_register_event_cb(nullptr, "test_toggle_row_noop", no_op_toggle_cb);

        lv_subject_init_int(&val_subject, 1);
        lv_xml_register_subject(nullptr, "test_toggle_row_val", &val_subject);

        lv_subject_init_int(&dis_subject, 0);
        lv_xml_register_subject(nullptr, "test_toggle_row_dis", &dis_subject);
    }

    ~ToggleRowDisabledFixture() override {
        lv_subject_deinit(&val_subject);
        lv_subject_deinit(&dis_subject);
    }
};

} // namespace

TEST_CASE_METHOD(ToggleRowDisabledFixture, "setting_toggle_row disabled prop drives switch state",
                 "[xml][settings][spool-retention]") {
    const char* attrs[] = {"name",     "row_under_test",       "label",    "Test",
                           "subject",  "test_toggle_row_val",  "disabled", "test_toggle_row_dis",
                           "callback", "test_toggle_row_noop", nullptr};
    lv_obj_t* row =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "setting_toggle_row", attrs));
    REQUIRE(row != nullptr);

    lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
    REQUIRE(toggle != nullptr);
    REQUIRE(lv_obj_has_state(toggle, LV_STATE_CHECKED)); // value subject is 1

    SECTION("disabled=0 leaves the switch operable") {
        CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_DISABLED));
    }

    SECTION("disabled=1 disables the switch; back to 0 re-enables") {
        lv_subject_set_int(&dis_subject, 1);
        CHECK(lv_obj_has_state(toggle, LV_STATE_DISABLED));

        lv_subject_set_int(&dis_subject, 0);
        CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_DISABLED));
    }
}
