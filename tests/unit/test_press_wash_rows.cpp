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

#include "ui_choice_subject.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_modal_test_access.h"
#include "../test_helpers/power_device_widget_test_access.h"
#include "../test_helpers/press_wash_probe.h"
#include "device_display_name.h"
#include "moonraker_types.h"
#include "panel_widget_registry.h"
#include "power_device_state.h"
#include "power_device_widget.h"
#include "theme_manager.h"
#include "widget_resolution.h"

#include <cstring>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::has_own_click_handler;
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
                                  "choice_chip", "exclude_object_row"}) {
        INFO("component: " << component);
        lv_obj_t* row = create_row(test_screen(), component);
        REQUIRE(row != nullptr);
        CHECK(lv_obj_is_clickable(row));
        CHECK(paints_press_wash(row));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A selected picker choice is primary-tinted at rest and still washes when pressed",
                 "[press_wash][1297]") {
    const lv_color_t primary = theme_manager_get_color("primary");
    constexpr const char* kChoice = "press_wash_test_choice";
    helix::ui::set_choice(kChoice, 3);

    // The list row, and the outlined chip the power device picker passes.
    const std::vector<std::vector<const char*>> looks = {
        {"picker_option_row"},
        {"choice_chip", "idle_style", "outline_idle", "selected_style", "outline_selected"},
    };
    for (const auto& look : looks) {
        const char* component = look[0];
        INFO("component: " << component);
        auto attrs_for = [&](const char* value) {
            std::vector<const char*> attrs = {"selected_subject", kChoice, "selected_value", value};
            attrs.insert(attrs.end(), look.begin() + 1, look.end());
            attrs.push_back(nullptr);
            return attrs;
        };
        auto selected_attrs = attrs_for("3");
        lv_obj_t* selected = create_row(test_screen(), component, selected_attrs.data());
        REQUIRE(selected != nullptr);
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(selected, LV_PART_MAIN), primary));
        CHECK(lv_obj_get_style_bg_opa(selected, LV_PART_MAIN) > LV_OPA_TRANSP);
        CHECK(paints_press_wash(selected));

        auto plain_attrs = attrs_for("4");
        lv_obj_t* plain = create_row(test_screen(), component, plain_attrs.data());
        REQUIRE(plain != nullptr);
        CHECK(lv_obj_get_style_bg_opa(plain, LV_PART_MAIN) == LV_OPA_TRANSP);

        // The selection follows the subject: moving it lights the other row.
        helix::ui::set_choice(kChoice, 4);
        CHECK(lv_obj_get_style_bg_opa(selected, LV_PART_MAIN) == LV_OPA_TRANSP);
        CHECK(lv_obj_get_style_bg_opa(plain, LV_PART_MAIN) > LV_OPA_TRANSP);
        helix::ui::set_choice(kChoice, 3);
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
    CHECK(has_own_click_handler(row, start));

    // The trash icon reads its job from the row it sits directly in, and stops
    // its own tap so removing a job never also starts it.
    lv_obj_t* trash = lv_obj_find_by_name(row, "job_delete");
    REQUIRE(trash != nullptr);
    CHECK(lv_obj_get_parent(trash) == row);
    CHECK(has_own_click_handler(trash, remove));
    CHECK(lv_obj_is_clickable(trash));
    CHECK_FALSE(lv_obj_is_event_bubble(trash));
}

namespace {

class PowerPickerRowsFixture : public LVGLUITestFixture {
  public:
    PowerPickerRowsFixture() {
        // The picker rows' click callback.
        init_widget_registrations();
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
