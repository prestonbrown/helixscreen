// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thermistor_picker_commit.cpp
 * @brief The thermistor configure picker commits its checkboxes however it closes.
 *
 * The card's checkboxes ARE the edit - there is no draft state to throw away - so
 * the Done button and a tap on the backdrop have to mean the same thing. They did
 * not: Done applied the selection and the backdrop only dismissed, so a user who
 * ticked a second sensor and then tapped outside the card lost the change with no
 * indication anything had happened. Its sibling picker (print status) already
 * committed on backdrop tap.
 *
 * Selecting a second sensor flips the widget from single to carousel mode, which
 * is visible through get_component_name(), so the two paths can be compared
 * without reaching into the widget's private state.
 */

#include "ui_context_menu.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "src/ui/panel_widgets/thermistor_widget.h"
#include "temperature_sensor_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr const char* SENSOR_A = "temperature_sensor helix_test_probe_a";
constexpr const char* SENSOR_B = "temperature_sensor helix_test_probe_b";

/// Index of a klipper_name in the order the picker builds its rows.
int row_index_of(const std::string& klipper_name) {
    auto sensors = helix::sensors::TemperatureSensorManager::instance().get_sensors_sorted();
    for (size_t i = 0; i < sensors.size(); ++i) {
        if (sensors[i].klipper_name == klipper_name)
            return static_cast<int>(i);
    }
    return -1;
}

/// The card the picker put on screen, and the backdrop behind it.
struct OpenPicker {
    lv_obj_t* card = nullptr;
    lv_obj_t* backdrop = nullptr;
    lv_obj_t* sensor_list = nullptr;
};

OpenPicker find_open_picker(lv_obj_t* screen) {
    OpenPicker p;
    p.card = lv_obj_find_by_name(screen, "context_menu");
    if (p.card) {
        p.backdrop = lv_obj_get_parent(p.card);
        p.sensor_list = lv_obj_find_by_name(p.card, "sensor_list");
    }
    return p;
}

bool row_is_checked(lv_obj_t* row) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_checkbox_class))
            return lv_obj_has_state(child, LV_STATE_CHECKED);
    }
    return false;
}

/// Fixture that seeds two sensors and registers everything the picker XML needs.
class ThermistorPickerFixture : public XMLTestFixture {
  public:
    ThermistorPickerFixture() {
        helix::ui::ContextMenu::register_shared_callbacks();
        REQUIRE(register_component("thermistor_configure_picker"));
        helix::sensors::TemperatureSensorManager::instance().discover({SENSOR_A, SENSOR_B});
        REQUIRE(row_index_of(SENSOR_A) >= 0);
        REQUIRE(row_index_of(SENSOR_B) >= 0);
    }

    ~ThermistorPickerFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }
};

/// Open the configure picker on a widget bound to SENSOR_A only, tick SENSOR_B,
/// and return the picker so the caller can close it whichever way it likes.
OpenPicker open_and_tick_second_sensor(ThermistorWidget& widget, lv_obj_t* screen) {
    REQUIRE(widget.get_component_name() == "panel_widget_thermistor");

    widget.on_edit_configure();

    OpenPicker picker = find_open_picker(screen);
    REQUIRE(picker.card != nullptr);
    REQUIRE(picker.sensor_list != nullptr);

    lv_obj_t* row = lv_obj_get_child(picker.sensor_list, row_index_of(SENSOR_B));
    REQUIRE(row != nullptr);
    REQUIRE_FALSE(row_is_checked(row));

    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    REQUIRE(row_is_checked(row));

    return picker;
}

} // namespace

// The bug itself: a tap on the backdrop used to drop the tick silently.
TEST_CASE_METHOD(ThermistorPickerFixture, "thermistor configure picker: backdrop tap commits",
                 "[thermistor][context_menu]") {
    ThermistorWidget widget("thermistor");
    widget.set_config({{"sensors", std::vector<std::string>{SENSOR_A}}});

    lv_obj_t* tile = lv_obj_create(test_screen());
    widget.attach(tile, test_screen());

    OpenPicker picker = open_and_tick_second_sensor(widget, test_screen());
    lv_obj_send_event(picker.backdrop, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(widget.get_component_name() == "panel_widget_thermistor_carousel");
}

// The behaviour the backdrop is being held to.
TEST_CASE_METHOD(ThermistorPickerFixture, "thermistor configure picker: Done commits",
                 "[thermistor][context_menu]") {
    ThermistorWidget widget("thermistor");
    widget.set_config({{"sensors", std::vector<std::string>{SENSOR_A}}});

    lv_obj_t* tile = lv_obj_create(test_screen());
    widget.attach(tile, test_screen());

    OpenPicker picker = open_and_tick_second_sensor(widget, test_screen());
    lv_obj_t* done = lv_obj_find_by_name(picker.card, "btn_close");
    REQUIRE(done != nullptr);
    lv_obj_send_event(done, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(widget.get_component_name() == "panel_widget_thermistor_carousel");
}

// Both paths close the card. A commit that left the menu up would read as a
// no-op to the user even though the selection landed.
TEST_CASE_METHOD(ThermistorPickerFixture, "thermistor configure picker: both paths close the card",
                 "[thermistor][context_menu]") {
    ThermistorWidget widget("thermistor");
    widget.set_config({{"sensors", std::vector<std::string>{SENSOR_A}}});

    lv_obj_t* tile = lv_obj_create(test_screen());
    widget.attach(tile, test_screen());

    OpenPicker picker = open_and_tick_second_sensor(widget, test_screen());
    lv_obj_send_event(picker.backdrop, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(find_open_picker(test_screen()).card == nullptr);
}
