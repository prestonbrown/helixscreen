// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_fan_select.h"
#include "ui_wizard_helpers.h"
#include "ui_wizard_led_select.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

TEST_CASE("wizard dropdown: a device-type namer labels each row, None stays first",
          "[wizard][dropdown]") {
    using helix::ui::wizard::build_dropdown_options;
    using helix::ui::wizard::display_name_for;
    CHECK(build_dropdown_options({"heater_fan hotend_fan", "fan"}, nullptr, true,
                                 display_name_for(helix::DeviceType::FAN)) ==
          "None\n" + helix::get_display_name("heater_fan hotend_fan", helix::DeviceType::FAN) +
              "\n" + helix::get_display_name("fan", helix::DeviceType::FAN));
    CHECK(build_dropdown_options({"heater_fan hotend_fan"}, nullptr, false) ==
          "heater_fan hotend_fan");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "wizard LED step names each light the way the LEDs overlay does",
                 "[wizard][led][dropdown]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api{client, state};
    api.hardware().parse_objects(
        nlohmann::json::array({"led caselight", "neopixel chamber_light", "extruder"}));
    set_moonraker_api(&api);
    struct ApiGuard {
        ~ApiGuard() {
            set_moonraker_api(nullptr);
        }
    } api_guard;

    auto* step = get_wizard_led_select_step();
    step->init_subjects();
    step->register_callbacks();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "led_main_dropdown");
    REQUIRE(dropdown != nullptr);

    // get_display_name(..., DeviceType::LED) would read "Caselight LED".
    CHECK(std::string(lv_dropdown_get_options(dropdown)) == "None\nCaselight\nChamber Light");

    // Picking a light stages its index for cleanup() to save.
    lv_dropdown_set_selected(dropdown, 2);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(lv_subject_get_int(step->get_led_strip_subject()) == 2);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "wizard fan step shows the optional row past two fans and flags a reused fan",
                 "[wizard][fan][dropdown]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api{client, state};
    set_moonraker_api(&api);
    struct ApiGuard {
        ~ApiGuard() {
            set_moonraker_api(nullptr);
        }
    } api_guard;
    auto* step = get_wizard_fan_select_step();
    step->register_callbacks();
    auto shown = [](lv_obj_t* root, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        return !lv_obj_is_hidden(obj);
    };

    api.hardware().parse_objects(nlohmann::json::array({"heater_fan hotend_fan", "fan"}));
    step->init_subjects();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);
    CHECK_FALSE(shown(root, "optional_fans_row"));
    step->cleanup();
    lv_obj_delete(root);

    api.hardware().parse_objects(
        nlohmann::json::array({"heater_fan hotend_fan", "fan", "fan_generic nevermore"}));
    step->init_subjects();
    root = step->create(test_screen());
    REQUIRE(root != nullptr);
    CHECK(shown(root, "optional_fans_row"));
    CHECK_FALSE(shown(root, "fan_status_text"));

    // The part fan's own fan picked again as the chamber fan.
    lv_obj_t* part = lv_obj_find_by_name(root, "part_cooling_fan_dropdown");
    lv_obj_t* chamber = lv_obj_find_by_name(root, "chamber_fan_dropdown");
    REQUIRE((part && chamber));
    char picked[64];
    lv_dropdown_get_selected_str(part, picked, sizeof(picked));
    const int32_t again = lv_dropdown_get_option_index(chamber, picked);
    REQUIRE(again > 0);
    lv_dropdown_set_selected(chamber, again);
    lv_obj_send_event(chamber, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(shown(root, "fan_status_text"));
    step->cleanup();
    lv_obj_delete(root);
}
