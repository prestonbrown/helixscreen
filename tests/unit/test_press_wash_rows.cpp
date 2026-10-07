// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_press_wash_rows.cpp
 * @brief List and picker rows that C++ creates per item press with the shared wash
 *
 * Each of these rows is an XML component instantiated from C++ once per item.
 * Its pressed feedback is styles.press_wash (ui_xml/styles.xml), so every row
 * here must paint exactly that while pressed and something else at rest
 * (prestonbrown/helixscreen#1297). The widget catalog row and the material
 * temperatures row are covered through their own screens, in
 * test_widget_catalog_categories.cpp and test_material_temps_chamber.cpp.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_modal_test_access.h"
#include "../test_helpers/power_device_widget_test_access.h"
#include "../test_helpers/press_wash_probe.h"
#include "device_display_name.h"
#include "moonraker_types.h"
#include "power_device_state.h"
#include "power_device_widget.h"
#include "theme_manager.h"

#include <cstring>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::test::has_clicked_handler;
using helix::test::paints_press_wash;
using helix::test::row_with_label;

namespace {

lv_obj_t* create_row(lv_obj_t* parent, const char* component, const char** attrs = nullptr) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, component, attrs));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Per-item list and picker rows press with the shared wash",
                 "[press_wash][1297]") {
    // job_queue_row resolves its event_cb names at creation.
    JobQueueModalTestAccess::register_callbacks();

    for (const char* component : {"printer_switch_row", "job_queue_row", "picker_option_row",
                                  "picker_chip", "exclude_object_row"}) {
        INFO("component: " << component);
        lv_obj_t* row = create_row(test_screen(), component);
        REQUIRE(row != nullptr);
        CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));
        CHECK(paints_press_wash(row));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A selected picker choice is primary-tinted at rest and still washes when pressed",
                 "[press_wash][1297]") {
    const lv_color_t primary = theme_manager_get_color("primary");

    for (const char* component : {"picker_option_row", "picker_chip"}) {
        INFO("component: " << component);
        const char* selected_attrs[] = {"selected", "true", nullptr};
        lv_obj_t* selected = create_row(test_screen(), component, selected_attrs);
        REQUIRE(selected != nullptr);
        REQUIRE(lv_obj_has_state(selected, LV_STATE_CHECKED));
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(selected, LV_PART_MAIN), primary));
        CHECK(lv_obj_get_style_bg_opa(selected, LV_PART_MAIN) > LV_OPA_TRANSP);
        CHECK(paints_press_wash(selected));

        const char* plain_attrs[] = {"selected", "false", nullptr};
        lv_obj_t* plain = create_row(test_screen(), component, plain_attrs);
        REQUIRE(plain != nullptr);
        CHECK_FALSE(lv_obj_has_state(plain, LV_STATE_CHECKED));
        CHECK(lv_obj_get_style_bg_opa(plain, LV_PART_MAIN) == LV_OPA_TRANSP);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "A job queue row starts its job and its trash icon removes it",
                 "[press_wash][job_queue][1297]") {
    JobQueueModalTestAccess::register_callbacks();
    lv_event_cb_t start = lv_xml_get_event_cb(nullptr, "on_jq_row_start");
    lv_event_cb_t remove = lv_xml_get_event_cb(nullptr, "on_jq_row_delete");
    REQUIRE(start != nullptr);
    REQUIRE(remove != nullptr);

    lv_obj_t* row = create_row(test_screen(), "job_queue_row");
    REQUIRE(row != nullptr);
    CHECK(has_clicked_handler(row, start));

    // The trash icon reads its job from the row it sits directly in, and stops
    // its own tap so removing a job never also starts it.
    lv_obj_t* trash = lv_obj_find_by_name(row, "job_delete");
    REQUIRE(trash != nullptr);
    CHECK(lv_obj_get_parent(trash) == row);
    CHECK(has_clicked_handler(trash, remove));
    CHECK(lv_obj_has_flag(trash, LV_OBJ_FLAG_CLICKABLE));
    CHECK_FALSE(lv_obj_has_flag(trash, LV_OBJ_FLAG_EVENT_BUBBLE));
}

namespace {

class PowerPickerRowsFixture : public LVGLUITestFixture {
  public:
    PowerPickerRowsFixture() {
        PowerDeviceState::instance().set_devices({
            PowerDevice{"printer_psu", "gpio", "off", false},
            PowerDevice{"#2_psu", "gpio", "off", false},
        });
    }

    ~PowerPickerRowsFixture() override {
        PowerDeviceState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(PowerPickerRowsFixture,
                 "Power device picker rows wash when pressed and a tap picks the device",
                 "[press_wash][power_device_widget][1297]") {
    using Access = PowerDeviceWidgetTestAccess;
    auto widget = std::make_unique<PowerDeviceWidget>("power_device:1");
    Access::set_screen(*widget, test_screen(), lv_obj_create(test_screen()));
    Access::show_picker(*widget);

    lv_obj_t* backdrop = Access::backdrop(test_screen());
    REQUIRE(backdrop != nullptr);
    lv_obj_t* row = row_with_label(backdrop, "All Devices");
    REQUIRE(row != nullptr);
    CHECK(paints_press_wash(row));

    // A device name is user text: through an XML prop a leading '#' resolves
    // as a const reference and the label comes up blank.
    const std::string hashed = get_display_name("#2_psu", DeviceType::POWER_DEVICE);
    REQUIRE(hashed.rfind('#', 0) == 0);
    CHECK(row_with_label(backdrop, hashed.c_str()) != nullptr);

    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(Access::picker_visible(*widget));
    CHECK(Access::device_name(*widget) == "__all__");
    process_lvgl(50);
}
