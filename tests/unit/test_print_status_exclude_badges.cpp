// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// While the exclude-object side list is open, the print status panel keeps the
// G-code viewer's numbered badges in step with it: the same number and colour
// per object as the list's chips, refreshed when objects are excluded or the
// printing object moves, and gone when the list closes.

#include "ui_gcode_viewer.h"

#include "../test_helpers/print_status_panel_fixture.h"
#include "../test_helpers/scoped_portrait_layout.h"
#include "printer_discovery.h"
#include "printer_excluded_objects_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using print_status_panel_test::PrintStatusPanelFixture;
using ObjectInfo = helix::PrinterExcludedObjectsState::ObjectInfo;

namespace {

ObjectInfo placed(const std::string& name, float x, float y) {
    ObjectInfo o;
    o.name = name;
    o.center = {x, y};
    o.bbox_min = {x - 10.0f, y - 10.0f};
    o.bbox_max = {x + 10.0f, y + 10.0f};
    return o;
}

std::vector<helix::ui::ObjectBadge> badges_of(lv_obj_t* viewer) {
    return helix::test_access::gcode_viewer_object_badges(viewer);
}

/// A print status panel on a printer with [exclude_object] configured.
class ExcludeObjectPanelFixture : public PrintStatusPanelFixture {
  public:
    ExcludeObjectPanelFixture() {
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json{"exclude_object", "extruder"});
        state().set_hardware(hw);
    }
    ~ExcludeObjectPanelFixture() override {
        state().set_hardware(helix::PrinterDiscovery{});
    }
};

} // namespace

TEST_CASE_METHOD(ExcludeObjectPanelFixture,
                 "Render badges follow the exclude side list open, update and close",
                 "[exclude_badges][print_status]") {
    auto& objects = state().excluded_objects_state();
    // Defined order is not name order: Zed is number 1.
    objects.set_defined_objects_with_geometry(
        {placed("Zed", 30, 30), placed("Alpha", 90, 30), placed("Mid", 150, 30)});
    UpdateQueue::instance().drain();

    lv_obj_t* viewer = PrintStatusPanelTestAccess::gcode_viewer(panel());
    REQUIRE(viewer != nullptr);
    CHECK(badges_of(viewer).empty()); // exclude mode is off

    PrintStatusPanelTestAccess::show_exclude(panel());
    UpdateQueue::instance().drain();

    auto badges = badges_of(viewer);
    REQUIRE(badges.size() == 3);
    CHECK(badges[0].name == "Zed");
    CHECK(badges[0].number == "1");

    // The side list's chips and the render badges agree for every object.
    lv_obj_t* rows = lv_obj_find_by_name(root_, "rows_container");
    REQUIRE(rows != nullptr);
    REQUIRE(lv_obj_get_child_count(rows) == 3);
    const auto fills = helix::test_access::gcode_viewer_badge_fills(viewer);
    const auto texts = helix::test_access::gcode_viewer_badge_texts(viewer);
    REQUIRE(fills.size() == 3);
    REQUIRE(texts.size() == 3);
    for (int i = 0; i < 3; ++i) {
        INFO("row " << i);
        lv_obj_t* row = lv_obj_get_child(rows, i);
        lv_obj_t* disc = lv_obj_get_child(row, 0);
        lv_obj_t* number = lv_obj_get_child(disc, 0);
        CHECK(std::string(lv_label_get_text(number)) == badges[static_cast<size_t>(i)].number);
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(disc, LV_PART_MAIN),
                          fills[static_cast<size_t>(i)]));
        CHECK(lv_color_eq(lv_obj_get_style_text_color(number, LV_PART_MAIN),
                          texts[static_cast<size_t>(i)]));
    }
    CHECK_FALSE(lv_color_eq(fills[0], fills[1]));

    objects.set_excluded_objects({"Alpha"});
    UpdateQueue::instance().drain();
    badges = badges_of(viewer);
    REQUIRE(badges.size() == 3);
    CHECK(badges[1].excluded);
    CHECK_FALSE(badges[0].excluded);

    objects.set_current_object("Mid");
    UpdateQueue::instance().drain();
    CHECK(badges_of(viewer)[2].current);

    PrintStatusPanelTestAccess::hide_exclude(panel());
    UpdateQueue::instance().drain();
    CHECK(badges_of(viewer).empty());

    // A version bump with the list closed does not bring them back.
    objects.set_current_object("Zed");
    UpdateQueue::instance().drain();
    CHECK(badges_of(viewer).empty());
}

TEST_CASE_METHOD(ExcludeObjectPanelFixture,
                 "The objects button follows a multi-object print and never shows a pick count",
                 "[exclude_button][print_status][pre_start_exclude]") {
    lv_obj_t* btn = lv_obj_find_by_name(root_, "btn_objects");
    REQUIRE(btn != nullptr);
    auto& objects = state().excluded_objects_state();

    objects.set_defined_objects({"Solo"});
    UpdateQueue::instance().drain();
    CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    objects.set_defined_objects({"A", "B"});
    UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* count = lv_obj_find_by_name(btn, "objects_pick_count");
    REQUIRE(count != nullptr);
    CHECK(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "The objects button stays hidden on a printer without [exclude_object]",
                 "[exclude_button][print_status][pre_start_exclude]") {
    lv_obj_t* btn = lv_obj_find_by_name(root_, "btn_objects");
    REQUIRE(btn != nullptr);
    REQUIRE_FALSE(state().get_discovery().has_exclude_object());

    state().excluded_objects_state().set_defined_objects({"A", "B"});
    UpdateQueue::instance().drain();
    CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "The objects button follows the printer's [exclude_object] as hardware changes",
                 "[exclude_button][print_status][pre_start_exclude]") {
    lv_obj_t* btn = lv_obj_find_by_name(root_, "btn_objects");
    REQUIRE(btn != nullptr);
    state().excluded_objects_state().set_defined_objects({"A", "B"});
    UpdateQueue::instance().drain();
    REQUIRE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    // Discovery reports [exclude_object] after the objects are already defined.
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json{"exclude_object", "extruder"});
    state().set_hardware(hw);
    UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

    // A switch to a printer without it hides the button again.
    state().set_hardware(helix::PrinterDiscovery{});
    UpdateQueue::instance().drain();
    CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
}

namespace {

/// Print status built from its portrait layout, on a printer with [exclude_object].
class PortraitExcludePanelFixture : public PrintStatusPanelFixture {
  public:
    PortraitExcludePanelFixture()
        : PrintStatusPanelFixture([]() -> std::shared_ptr<void> {
              auto portrait = std::make_shared<ScopedPortraitLayout>(480, 1000);
              REQUIRE(lv_xml_register_component_from_file(
                          "A:ui_xml/portrait/print_status_panel.xml") == LV_RESULT_OK);
              return std::shared_ptr<void>(portrait.get(), [portrait](void*) {
                  lv_xml_register_component_from_file("A:ui_xml/print_status_panel.xml");
              });
          }) {
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json{"exclude_object", "extruder"});
        state().set_hardware(hw);
    }
    ~PortraitExcludePanelFixture() override {
        state().set_hardware(helix::PrinterDiscovery{});
    }
};

} // namespace

TEST_CASE_METHOD(PortraitExcludePanelFixture,
                 "In portrait the print status object list covers the controls and not the preview",
                 "[exclude_badges][print_status][pre_start_exclude][portrait]") {
    state().excluded_objects_state().set_defined_objects_with_geometry(
        {placed("Zed", 30, 30), placed("Alpha", 90, 30), placed("Mid", 150, 30)});
    UpdateQueue::instance().drain();
    process_lvgl(100);

    PrintStatusPanelTestAccess::show_exclude(panel());
    UpdateQueue::instance().drain();
    process_lvgl(600); // the slide-in
    lv_obj_update_layout(root_);

    lv_obj_t* rows = lv_obj_find_by_name(root_, "rows_container");
    REQUIRE(rows != nullptr);
    lv_obj_t* controls = lv_obj_find_by_name(root_, "controls_section");
    lv_obj_t* preview = lv_obj_find_by_name(root_, "thumbnail_section");
    REQUIRE(controls != nullptr);
    REQUIRE(preview != nullptr);
    lv_area_t list, ctl, card;
    lv_obj_get_coords(lv_obj_get_parent(rows), &list);
    lv_obj_get_coords(controls, &ctl);
    lv_obj_get_coords(preview, &card);
    INFO("list y1=" << list.y1 << " controls y1=" << ctl.y1 << " preview y2=" << card.y2);
    CHECK(list.y1 <= ctl.y1);
    CHECK(list.y1 > card.y2);

    PrintStatusPanelTestAccess::hide_exclude(panel());
    UpdateQueue::instance().drain();
}
