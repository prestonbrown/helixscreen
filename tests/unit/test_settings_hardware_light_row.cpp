// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_hardware_light_row.cpp
 * @brief The Devices page's "Chamber light fitted" toggle
 *
 * A preset can turn the led capability off for a printer whose firmware declares
 * a light the hardware does not wire. The toggle turns it back on, so it shows
 * whenever Klipper reports a light object, whatever the override says, and its
 * change applies at once.
 */

#include "ui_settings_hardware.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "app_globals.h"
#include "capability_overrides.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_controller.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

struct LightRowFixture : public LVGLUITestFixture {
    LightRowFixture() {
        helix::settings::get_hardware_settings_overlay().register_callbacks();
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_hardware_overlay", nullptr));
    }

    ~LightRowFixture() override {
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
    }

    static lv_subject_t* subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return s;
    }

    static std::string saved_led_override() {
        Config* cfg = Config::get_instance();
        return cfg->get<std::string>(cfg->df() + "capability_overrides/led", "");
    }

    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(LightRowFixture,
                 "Devices page: the chamber light toggle needs a reported light object",
                 "[settings][led]") {
    REQUIRE(root_ != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(root_, "row_chamber_light_fitted");
    REQUIRE(row != nullptr);
    lv_subject_t* object = subject("printer_has_led_object");
    lv_subject_t* has_led = subject("printer_has_led");
    const int object_before = lv_subject_get_int(object);
    const int has_led_before = lv_subject_get_int(has_led);

    lv_subject_set_int(object, 0);
    process_lvgl(5);
    CHECK(lv_obj_is_hidden(row));

    // The override hides the lights; the toggle stays, switched off.
    lv_subject_set_int(object, 1);
    lv_subject_set_int(has_led, 0);
    process_lvgl(5);
    CHECK_FALSE(lv_obj_is_hidden(row));
    lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));

    lv_subject_set_int(object, object_before);
    lv_subject_set_int(has_led, has_led_before);
}

TEST_CASE_METHOD(LightRowFixture,
                 "Devices page: the chamber light toggle saves the override and applies it live",
                 "[settings][led]") {
    REQUIRE(root_ != nullptr);
    auto& ps = get_printer_state();
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({"led chamber_light", "extruder"}));
    ps.set_hardware(discovery);
    ps.set_led_override(OverrideState::DISABLE);

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    ctrl.set_lights_disabled(ps.get_capability_overrides().lights_disabled());
    ctrl.discover_from_hardware(discovery);
    lv_subject_t* has_led = subject("printer_has_led");
    lv_subject_t* controllable = ctrl.get_led_controllable_subject();
    REQUIRE(lv_subject_get_int(has_led) == 0);
    REQUIRE(lv_subject_get_int(controllable) == 0);
    process_lvgl(5);

    lv_obj_t* row = lv_obj_find_by_name(root_, "row_chamber_light_fitted");
    REQUIRE(row != nullptr);
    lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(row));

    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    process_lvgl(5);
    CHECK(saved_led_override() == "enable");
    CHECK(lv_subject_get_int(has_led) == 1);
    CHECK(lv_subject_get_int(controllable) == 1);

    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    process_lvgl(5);
    CHECK(saved_led_override() == "disable");
    CHECK(lv_subject_get_int(has_led) == 0);
    CHECK(lv_subject_get_int(controllable) == 0);

    ps.set_led_override(OverrideState::AUTO);
    ps.set_hardware(helix::PrinterDiscovery{});
    ctrl.deinit();
}
