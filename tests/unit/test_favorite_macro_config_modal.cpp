// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: instantiates the shipped ui_xml/favorite_macro_config_modal.xml through
//                 lv_xml_create() at run time and asserts on the real widget tree.
//                 ../test_fixtures.h pulls in include/printer_state.h,
//                 include/moonraker_api.h and include/theme_manager.h.
#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

// Verifies the new XML component instantiates and tab sections toggle on the subject.
TEST_CASE_METHOD(XMLTestFixture, "favorite_macro_config_modal instantiates + tabs switch",
                 "[macro][favorite_config][xml]") {
    // Component + subjects must be registered by register_favorite_macro_widgets()
    lv_obj_t* dlg = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "favorite_macro_config_modal", nullptr));
    REQUIRE(dlg != nullptr);

    lv_obj_t* sec_macro = lv_obj_find_by_name(dlg, "section_macro");
    lv_obj_t* sec_appearance = lv_obj_find_by_name(dlg, "section_appearance");
    lv_obj_t* sec_options = lv_obj_find_by_name(dlg, "section_options");
    REQUIRE(sec_macro != nullptr);
    REQUIRE(sec_appearance != nullptr);
    REQUIRE(sec_options != nullptr);

    lv_subject_t* tab = lv_xml_get_subject(nullptr, "fav_macro_config_tab");
    REQUIRE(tab != nullptr);

    lv_subject_set_int(tab, 0);
    lv_obj_update_layout(dlg);
    REQUIRE_FALSE(lv_obj_has_flag(sec_macro, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_flag(sec_options, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(tab, 2);
    lv_obj_update_layout(dlg);
    REQUIRE(lv_obj_has_flag(sec_macro, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(sec_options, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(dlg);
}
