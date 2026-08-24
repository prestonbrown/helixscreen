// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_header_bar_estop_slot.cpp
 * @brief Unit test for header_bar's dedicated E-Stop spacer.
 *
 * The slot reserves a right-most gutter so the title and any action buttons
 * stay clear of the floating e-stop FAB (which lives at the panel root, not in
 * the header — see prestonbrown/helixscreen#1204). The spacer holds NO button.
 * Visibility is gated by estop_visible_subject (1 = show), mirroring
 * action_button_hidden_subject. Empty default = the bind_flag no-ops and the
 * spacer stays hidden, so a header that does not opt in renders nothing.
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"

#include <fstream>
#include <lvgl.h>
#include <sstream>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

TEST_CASE_METHOD(XMLTestFixture, "header_bar estop spacer stays hidden without subject",
                 "[header_bar][xml]") {
    REQUIRE(register_component("header_bar"));

    // No estop_visible_subject passed — spacer must stay hidden (opt-in guarantee).
    const char* attrs[] = {"title", "Motion", nullptr};
    lv_obj_t* hdr = create_component("header_bar", attrs);
    REQUIRE(hdr != nullptr);
    lv_obj_t* slot = lv_obj_find_by_name(hdr, "estop_slot");
    REQUIRE(slot != nullptr);
    REQUIRE(lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(XMLTestFixture, "header_bar estop spacer toggles via subject",
                 "[header_bar][xml]") {
    REQUIRE(register_component("header_bar"));

    static lv_subject_t vis_subj;
    lv_subject_init_int(&vis_subj, 0);
    lv_xml_register_subject(nullptr, "test_estop_vis", &vis_subj);

    const char* attrs[] = {"title", "Print Status", "estop_visible_subject", "test_estop_vis",
                           nullptr};
    lv_obj_t* hdr = create_component("header_bar", attrs);
    REQUIRE(hdr != nullptr);
    lv_obj_t* slot = lv_obj_find_by_name(hdr, "estop_slot");
    REQUIRE(slot != nullptr);
    // Subject starts at 0 → hidden.
    REQUIRE(lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN));

    // The spacer must NOT contain a button — the FAB lives at the panel root.
    REQUIRE(lv_obj_get_child_count(slot) == 0);

    // Flip to 1 → printing/preparing → spacer revealed (reserves gutter).
    lv_subject_set_int(&vis_subj, 1);
    UpdateQueue::instance().drain();
    REQUIRE_FALSE(lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN));

    // Back to 0 → hidden again.
    lv_subject_set_int(&vis_subj, 0);
    UpdateQueue::instance().drain();
    REQUIRE(lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN));

    lv_subject_deinit(&vis_subj);
}

TEST_CASE_METHOD(XMLTestFixture, "header_bar estop spacer reserves x left of action button",
                 "[header_bar][xml]") {
    // The whole point of the spacer: when both an action button and the e-stop
    // gutter are visible, the action button must sit fully to the LEFT of the
    // reserved gutter — never sharing x with the panel-root FAB.
    REQUIRE(register_component("header_bar"));

    static lv_subject_t vis_subj;
    lv_subject_init_int(&vis_subj, 1); // active → spacer visible
    lv_xml_register_subject(nullptr, "test_estop_vis3", &vis_subj);

    const char* attrs[] = {"title",
                           "Tune",
                           "hide_action_button",
                           "false",
                           "action_button_text",
                           "Save",
                           "estop_visible_subject",
                           "test_estop_vis3",
                           nullptr};
    lv_obj_t* hdr = create_component("header_bar", attrs);
    REQUIRE(hdr != nullptr);
    UpdateQueue::instance().drain();
    lv_obj_update_layout(hdr);

    lv_obj_t* btn = lv_obj_find_by_name(hdr, "action_button");
    lv_obj_t* slot = lv_obj_find_by_name(hdr, "estop_slot");
    REQUIRE(btn != nullptr);
    REQUIRE(slot != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN));

    // Action button's right edge must not extend past the spacer's left edge.
    const lv_coord_t btn_right = lv_obj_get_x(btn) + lv_obj_get_width(btn);
    const lv_coord_t slot_left = lv_obj_get_x(slot);
    REQUIRE(btn_right <= slot_left);

    lv_subject_deinit(&vis_subj);
}

TEST_CASE("header_bar estop spacer markup pinned in both layout files", "[header_bar][xml]") {
    // The micro override is loaded by layout-class resolution, NOT by the
    // unit-test component loader (which reads the base file), so a typo in the
    // micro variant would not be caught by any LVGL-driving test. Read both
    // files as text and pin: (a) the spacer exists and is non-clickable, and
    // (b) NO e-stop BUTTON lives in the header — the FAB is panel-rooted to
    // avoid header clipping (#1204). A future edit that re-adds the button to
    // the header would re-introduce the clip and must fail this test.
    const std::string files[] = {"ui_xml/header_bar.xml", "ui_xml/micro/header_bar.xml"};
    for (const std::string& path : files) {
        std::ifstream file(path);
        REQUIRE(file.is_open());
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string xml = buffer.str();

        // The spacer exists and must not absorb taps (transparent gutter).
        const auto slot_needle = xml.find("name=\"estop_slot\"");
        REQUIRE(slot_needle != std::string::npos);
        const auto slot_end = xml.find('>', slot_needle);
        REQUIRE(slot_end != std::string::npos);
        const std::string slot_tag = xml.substr(slot_needle, slot_end - slot_needle);
        CHECK(slot_tag.find("clickable=\"false\"") != std::string::npos);

        // The header must NOT contain an e-stop button — it panel-rooted.
        CHECK(xml.find("name=\"estop_button\"") == std::string::npos);
        CHECK(xml.find("emergency_stop_clicked") == std::string::npos);
    }
}
