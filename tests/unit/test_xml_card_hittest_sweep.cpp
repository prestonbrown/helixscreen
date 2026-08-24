// SPDX-License-Identifier: GPL-3.0-or-later
// Sweep: components whose ROOT receives a click handler in C++ must not have
// decorative children that swallow the press. Plain <lv_obj> is CLICKABLE by
// default (lv_obj.c: lv_obj_constructor), and lv_indev_search_obj() returns the
// deepest hit-testing child — so an unguarded child absorbs clicks meant for the
// root. Guard decorative containers with clickable="false" event_bubble="true".
//
// Companion to test_print_file_card_hittest.cpp. Regression: prestonbrown/helixscreen#1101

#include "ui_severity_card.h"

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
    const char* attrs[] = {"tool_name", "T0", nullptr};
    lv_obj_t* root = create_component("nozzle_temp_row", attrs);
    REQUIRE(root != nullptr);
    lv_obj_set_width(root, 300);
    lv_obj_update_layout(root);

    lv_obj_t* value_group = lv_obj_find_by_name(root, "value_group");
    lv_obj_t* temp_label = lv_obj_find_by_name(root, "temp_label");
    REQUIRE(value_group != nullptr);
    REQUIRE(temp_label != nullptr);

    check_reaches_root(test_screen(), root, value_group, "value_group");
    check_reaches_root(test_screen(), root, temp_label, "temp_label");
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
    REQUIRE(register_component("ams_current_tool"));
    lv_obj_t* root = create_component("ams_current_tool");
    REQUIRE(root != nullptr);
    // The component hides itself unless an AMS tool is active; clear the flag so
    // the hit test sees real geometry.
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(root);

    lv_obj_t* swatch = lv_obj_find_by_name(root, "color_swatch");
    REQUIRE(swatch != nullptr);

    check_reaches_root(test_screen(), root, swatch, "color_swatch");
}

// ui_panel_notification_history.cpp:163 attaches CLICKED to the item root
// (and explicitly re-adds LV_OBJ_FLAG_CLICKABLE) for entries carrying an action.
TEST_CASE_METHOD(XMLTestFixture, "notification_history_item: body routes clicks to item root",
                 "[xml][hittest][sweep]") {
    ui_severity_card_register();
    REQUIRE(register_component("notification_history_item"));
    const char* attrs[] = {"severity",  "info",  "title", "Filament runout", "message", "Slot 1",
                           "timestamp", "12:01", nullptr};
    lv_obj_t* item = create_component("notification_history_item", attrs);
    REQUIRE(item != nullptr);
    lv_obj_set_width(item, 400);
    lv_obj_update_layout(item);

    // The panel makes the root clickable when the entry has an action.
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_obj_find_by_name(item, "item_title");
    lv_obj_t* message = lv_obj_find_by_name(item, "item_message");
    REQUIRE(title != nullptr);
    REQUIRE(message != nullptr);

    check_reaches_root(test_screen(), item, title, "item_title");
    check_reaches_root(test_screen(), item, message, "item_message");
}
