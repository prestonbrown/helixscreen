// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_header_bar_action_hidden.cpp
 * @brief Unit test for header_bar's action_button_hidden_subject prop.
 *
 * Mirrors the existing action_button_disabled_subject pattern: an int
 * subject drives whether the primary action button is hidden (1) or shown
 * (0), reactively, via bind_flag_if_eq.
 */

#include "../test_fixtures.h"
#include "core/update_queue.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

TEST_CASE_METHOD(XMLTestFixture, "header_bar action button hides via subject",
                 "[header_bar][xml]") {
    REQUIRE(register_component("header_bar"));

    static lv_subject_t hide_subj;
    lv_subject_init_int(&hide_subj, 0);
    lv_xml_register_subject(nullptr, "test_hdr_hidden", &hide_subj);

    const char* attrs[] = {"action_button_text", "Save", "action_button_hidden_subject",
                           "test_hdr_hidden", nullptr};
    lv_obj_t* hdr = create_component("header_bar", attrs);
    REQUIRE(hdr != nullptr);
    lv_obj_t* btn = lv_obj_find_by_name(hdr, "action_button");
    REQUIRE(btn != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(&hide_subj, 1);
    UpdateQueue::instance().drain();
    REQUIRE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(&hide_subj, 0);
    UpdateQueue::instance().drain();
    REQUIRE_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    lv_subject_deinit(&hide_subj);
}
