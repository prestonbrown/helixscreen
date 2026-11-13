// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// Sweep: components whose ROOT receives a click handler in C++ must not have
// decorative children that swallow the press. Plain <lv_obj> is CLICKABLE by
// default (lv_obj.c: lv_obj_constructor), and lv_indev_search_obj() returns the
// deepest hit-testing child — so an unguarded child absorbs clicks meant for the
// root. Guard decorative containers with clickable="false" event_bubble="true".
//
// Companion to test_print_file_card_hittest.cpp. Regression: prestonbrown/helixscreen#1101

#include "ui_severity_card.h"
#include "ui_swatch.h"

#include "../test_fixtures.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

lv_obj_t* hit_at(lv_obj_t* screen, int32_t x, int32_t y) {
    lv_point_t p{x, y};
    return lv_indev_search_obj(screen, &p);
}

std::string name_of(lv_obj_t* obj) {
    if (obj == nullptr)
        return "<null>";
    const char* n = lv_obj_get_name(obj);
    return n != nullptr ? n : "<unnamed>";
}

// Assert that a tap at the centre of `target` is delivered to `root`.
void check_reaches_root(lv_obj_t* screen, lv_obj_t* root, lv_obj_t* target, const char* what) {
    lv_area_t a;
    lv_obj_get_coords(target, &a);
    lv_obj_t* got = hit_at(screen, (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    INFO("tap on " << what << " resolved to: " << name_of(got));
    CHECK(got == root);
}

bool register_from(const char* path) {
    return lv_xml_register_component_from_file(path) == LV_RESULT_OK;
}

} // namespace

// ui_overlay_timelapse_videos.cpp:422-423 attaches CLICKED/LONG_PRESSED to the card root.
TEST_CASE_METHOD(XMLTestFixture, "timelapse_video_card: metadata band routes clicks to root",
                 "[xml][hittest][sweep]") {
    REQUIRE(register_component("timelapse_video_card"));
    const char* attrs[] = {"filename", "some_timelapse_video.mp4", "file_info", "12 MB · today",
                           nullptr};
    lv_obj_t* card = create_component("timelapse_video_card", attrs);
    REQUIRE(card != nullptr);
    lv_obj_update_layout(card);

    lv_obj_t* clip = lv_obj_find_by_name(card, "metadata_clip");
    lv_obj_t* overlay = lv_obj_find_by_name(card, "metadata_overlay");
    lv_obj_t* filename = lv_obj_find_by_name(card, "filename_label");
    REQUIRE(clip != nullptr);
    REQUIRE(overlay != nullptr);
    REQUIRE(filename != nullptr);

    check_reaches_root(test_screen(), card, overlay, "metadata_overlay");
    check_reaches_root(test_screen(), card, filename, "filename_label");
    check_reaches_root(test_screen(), card, clip, "metadata_clip");
}

// nozzle_temps_widget.cpp:465 attaches CLICKED to the row root.
TEST_CASE_METHOD(XMLTestFixture, "nozzle_temp_row: value area routes clicks to row root",
                 "[xml][hittest][sweep]") {
    REQUIRE(register_from("A:ui_xml/components/nozzle_temp_row.xml"));
    const char* attrs[] = {"tool_name", "T0", "tool_short", "Tool 1", "tool_number", "1", nullptr};
    lv_obj_t* root = create_component("nozzle_temp_row", attrs);
    REQUIRE(root != nullptr);
    lv_obj_set_width(root, 300);
    lv_obj_update_layout(root);

    lv_obj_t* value_group = lv_obj_find_by_name(root, "value_group");
    lv_obj_t* temp_label = lv_obj_find_by_name(root, "temp_label");
    lv_obj_t* left_group = lv_obj_find_by_name(root, "tool_left_group");
    REQUIRE(value_group != nullptr);
    REQUIRE(temp_label != nullptr);
    REQUIRE(left_group != nullptr);

    check_reaches_root(test_screen(), root, value_group, "value_group");
    check_reaches_root(test_screen(), root, temp_label, "temp_label");
    check_reaches_root(test_screen(), root, left_group, "tool_left_group");
    // The spelling labels live inside the nested group; whatever the layout
    // subjects say, a tap on the visible one must still reach the row.
    check_reaches_root(test_screen(), root, lv_obj_find_by_name(root, "tool_label_long"),
                       "tool_label_long");
}

// nozzle_temps_widget.cpp:500 attaches CLICKED to the bed row root.
TEST_CASE_METHOD(XMLTestFixture, "nozzle_temp_bed_row: value area routes clicks to row root",
                 "[xml][hittest][sweep]") {
    REQUIRE(register_from("A:ui_xml/components/nozzle_temp_bed_row.xml"));
    lv_obj_t* root = create_component("nozzle_temp_bed_row");
    REQUIRE(root != nullptr);
    lv_obj_set_width(root, 300);
    lv_obj_update_layout(root);

    lv_obj_t* left_group = lv_obj_find_by_name(root, "bed_left_group");
    lv_obj_t* value_group = lv_obj_find_by_name(root, "value_group");
    REQUIRE(left_group != nullptr);
    REQUIRE(value_group != nullptr);

    check_reaches_root(test_screen(), root, left_group, "bed_left_group");
    check_reaches_root(test_screen(), root, value_group, "value_group");
}

// ams_current_tool.xml declares <event_cb trigger="clicked"> on its own root
// (handler registered in ui_ams_current_tool.cpp:115). Rendered in
// print_status_panel.xml:330. The colour swatch is a sizeable part of this
// small indicator's tap target.
TEST_CASE_METHOD(XMLTestFixture, "ams_current_tool: colour swatch routes clicks to root",
                 "[xml][hittest][sweep]") {
    helix::ui::register_swatch_paint_widget();
    REQUIRE(register_component("components/color_swatch"));
    REQUIRE(register_component("components/color_dot"));
    REQUIRE(register_component("ams_current_tool"));
    lv_obj_t* root = create_component("ams_current_tool");
    REQUIRE(root != nullptr);
    // The component hides itself unless an AMS tool is active; clear the flag so
    // the hit test sees real geometry.
    lv_obj_set_hidden(root, false);
    lv_obj_update_layout(root);

    lv_obj_t* swatch = lv_obj_find_by_name(root, "color_swatch");
    REQUIRE(swatch != nullptr);

    check_reaches_root(test_screen(), root, swatch, "color_swatch");
}

// A history row whose entry carries an action is clickable through its
// has_action subject, and its event_cb fires on the item root.
TEST_CASE_METHOD(XMLTestFixture, "notification_history_item: body routes clicks to item root",
                 "[xml][hittest][sweep]") {
    REQUIRE(register_component("notification_history_item"));
    lv_subject_t severity{}, has_message{}, has_action{}, title_subj{}, message_subj{}, time_subj{};
    char title_buf[32], message_buf[32], time_buf[16];
    lv_subject_init_int(&severity, 0);
    lv_subject_init_int(&has_message, 1);
    lv_subject_init_int(&has_action, 1);
    lv_subject_init_string(&title_subj, title_buf, nullptr, sizeof(title_buf), "Filament runout");
    lv_subject_init_string(&message_subj, message_buf, nullptr, sizeof(message_buf), "Slot 1");
    lv_subject_init_string(&time_subj, time_buf, nullptr, sizeof(time_buf), "12:01");
    lv_xml_register_subject(nullptr, "hittest_row_severity", &severity);
    lv_xml_register_subject(nullptr, "hittest_row_has_message", &has_message);
    lv_xml_register_subject(nullptr, "hittest_row_has_action", &has_action);
    lv_xml_register_subject(nullptr, "hittest_row_title", &title_subj);
    lv_xml_register_subject(nullptr, "hittest_row_message", &message_subj);
    lv_xml_register_subject(nullptr, "hittest_row_time", &time_subj);
    const char* attrs[] = {"severity_subject",
                           "hittest_row_severity",
                           "title_subject",
                           "hittest_row_title",
                           "message_subject",
                           "hittest_row_message",
                           "has_message_subject",
                           "hittest_row_has_message",
                           "timestamp_subject",
                           "hittest_row_time",
                           "has_action_subject",
                           "hittest_row_has_action",
                           nullptr};
    lv_obj_t* item = create_component("notification_history_item", attrs);
    REQUIRE(item != nullptr);
    lv_obj_set_width(item, 400);
    lv_obj_update_layout(item);
    REQUIRE(lv_obj_is_clickable(item));

    lv_obj_t* title = lv_obj_find_by_name(item, "item_title");
    lv_obj_t* message = lv_obj_find_by_name(item, "item_message");
    REQUIRE(title != nullptr);
    REQUIRE(message != nullptr);

    check_reaches_root(test_screen(), item, title, "item_title");
    check_reaches_root(test_screen(), item, message, "item_message");

    lv_obj_delete(item);
    for (const char* name :
         {"hittest_row_severity", "hittest_row_has_message", "hittest_row_has_action",
          "hittest_row_title", "hittest_row_message", "hittest_row_time"}) {
        lv_xml_unregister_subject(nullptr, name);
    }
    for (lv_subject_t* subj :
         {&severity, &has_message, &has_action, &title_subj, &message_subj, &time_subj}) {
        lv_subject_deinit(subj);
    }
}
