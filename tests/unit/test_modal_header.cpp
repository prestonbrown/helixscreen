// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: drives ui_xml/components/modal_header.xml through lv_xml_create at runtime
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

// The shared modal_header component owns a trailing close button (btn_close) that is
// hidden by default and shown via hide_close="false". Consumers (and the Modal base via
// wire_cancel_button) reach it by name through the component boundary, so these tests
// guard both the visibility contract and the name-lookup contract.

TEST_CASE_METHOD(XMLTestFixture, "modal_header shows btn_close when hide_close=false",
                 "[modal_header][xml]") {
    const char* attrs[] = {"title", "Test Header",    "hide_icon", "true", "hide_close",
                           "false", "close_callback", "",          nullptr};
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "modal_header", attrs));
    REQUIRE(root != nullptr);

    // Title widget is named header_title (consumers update it by this name).
    lv_obj_t* title = lv_obj_find_by_name(root, "header_title");
    REQUIRE(title != nullptr);

    // Close button is found by name through the component boundary and is visible.
    lv_obj_t* close = lv_obj_find_by_name(root, "btn_close");
    REQUIRE(close != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(close, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(root);
}

TEST_CASE_METHOD(XMLTestFixture, "modal_header hides btn_close by default", "[modal_header][xml]") {
    const char* attrs[] = {"title", "Test Header", "hide_icon", "true", nullptr};
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "modal_header", attrs));
    REQUIRE(root != nullptr);

    lv_obj_t* close = lv_obj_find_by_name(root, "btn_close");
    REQUIRE(close != nullptr);
    REQUIRE(lv_obj_has_flag(close, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(root);
}
