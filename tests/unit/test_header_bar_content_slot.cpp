// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_header_bar_content_slot.cpp
 * @brief Unit test for header_bar's injectable content slot.
 *
 * header_content is the strip between the back/title region and the action
 * buttons where a screen can park its own readouts (the Motion panel puts
 * X/Y/Z coordinates there). Screens opt in by nesting a
 * <header_bar-header_content> tag inside their header_bar instantiation; the
 * engine re-parents that tag's children into the slot widget. A header that
 * fills nothing must render exactly as before the slot existed, so the empty
 * slot has to cost no layout and absorb no taps.
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml_component.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

TEST_CASE_METHOD(XMLTestFixture, "header_bar content slot exists and stays empty when unfilled",
                 "[header_bar][xml]") {
    REQUIRE(register_component("header_bar"));

    const char* attrs[] = {"title", "Tune", nullptr};
    lv_obj_t* hdr = create_component("header_bar", attrs);
    REQUIRE(hdr != nullptr);
    UpdateQueue::instance().drain();

    lv_obj_t* slot = lv_obj_find_by_name(hdr, "header_content");
    REQUIRE(slot != nullptr);
    // Opt-out guarantee: a header that injects nothing carries no children,
    // is visible, takes no layout width (content of nothing, no padding) and
    // does not steal taps from the back button's huge touch target.
    REQUIRE(lv_obj_get_child_count(slot) == 0);
    REQUIRE_FALSE(lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(slot, LV_OBJ_FLAG_CLICKABLE));
    lv_obj_update_layout(slot);
    REQUIRE(lv_obj_get_width(slot) == 0);

    // The slot sits between the title region and the action buttons, so
    // injected content lands next to the buttons, not next to the title.
    lv_obj_t* back = lv_obj_find_by_name(hdr, "back_button");
    lv_obj_t* action2 = lv_obj_find_by_name(hdr, "action_button_2");
    REQUIRE(back != nullptr);
    REQUIRE(action2 != nullptr);
    const int back_idx = lv_obj_get_index(back);
    const int slot_idx = lv_obj_get_index(slot);
    const int action2_idx = lv_obj_get_index(action2);
    REQUIRE(back_idx < slot_idx);
    REQUIRE(slot_idx < action2_idx);
}

TEST_CASE_METHOD(XMLTestFixture, "header_bar content slot re-parents injected children",
                 "[header_bar][xml]") {
    REQUIRE(register_component("header_bar"));

    // A screen filling the slot the way motion_panel.xml does: the
    // header_bar instantiation nests a <header_bar-header_content> tag and
    // the engine routes that tag's children into the named slot widget.
    REQUIRE(lv_xml_register_component_from_data(
                "slot_filler_screen",
                "<component>"
                "<view name=\"slot_filler_screen\" extends=\"lv_obj\" width=\"480\" height=\"320\">"
                "<header_bar title=\"Tune\">"
                "<header_bar-header_content>"
                "<lv_label name=\"injected_coord\" text=\"X 0.00\"/>"
                "</header_bar-header_content>"
                "</header_bar>"
                "</view>"
                "</component>") == LV_RESULT_OK);

    lv_obj_t* screen = create_component("slot_filler_screen");
    REQUIRE(screen != nullptr);
    UpdateQueue::instance().drain();

    lv_obj_t* slot = lv_obj_find_by_name(screen, "header_content");
    lv_obj_t* injected = lv_obj_find_by_name(screen, "injected_coord");
    REQUIRE(slot != nullptr);
    REQUIRE(injected != nullptr);
    // The whole point of the slot: injected content renders inside it, not
    // next to the header_bar instantiation.
    REQUIRE(lv_obj_get_parent(injected) == slot);
    REQUIRE(lv_obj_get_child_count(slot) == 1);
}
