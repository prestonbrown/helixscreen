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

#include "ui_carousel.h"
#include "ui_context_menu.h"

#include "../test_fixtures.h"
#include "../test_helpers/discovery_of_objects.h"
#include "../test_helpers/one_line_label.h"
#include "../test_helpers/thermistor_test_access.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "src/ui/panel_widgets/thermistor_widget.h"
#include "temperature_sensor_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr const char* kSensorA = "temperature_sensor helix_test_probe_a";
constexpr const char* kSensorB = "temperature_sensor helix_test_probe_b";

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
        REQUIRE(register_component("components/thermistor_picker_row"));
        REQUIRE(register_component("thermistor_configure_picker"));
        lv_xml_register_event_cb(nullptr, "thermistor_picker_row_cb",
                                 ThermistorWidget::picker_row_cb);
        helix::sensors::TemperatureSensorManager::instance().discover(
            helix::test::discovery_of({kSensorA, kSensorB}));
        REQUIRE(row_index_of(kSensorA) >= 0);
        REQUIRE(row_index_of(kSensorB) >= 0);
    }

    ~ThermistorPickerFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }
};

/// Open the configure picker on a widget bound to kSensorA only, tick kSensorB,
/// and return the picker so the caller can close it whichever way it likes.
OpenPicker open_and_tick_second_sensor(ThermistorWidget& widget, lv_obj_t* screen) {
    REQUIRE(widget.get_component_name() == "panel_widget_thermistor");

    widget.on_edit_configure();

    OpenPicker picker = find_open_picker(screen);
    REQUIRE(picker.card != nullptr);
    REQUIRE(picker.sensor_list != nullptr);

    lv_obj_t* row = lv_obj_get_child(picker.sensor_list, row_index_of(kSensorB));
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
    widget.set_config({{"sensors", std::vector<std::string>{kSensorA}}});

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
    widget.set_config({{"sensors", std::vector<std::string>{kSensorA}}});

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
    widget.set_config({{"sensors", std::vector<std::string>{kSensorA}}});

    lv_obj_t* tile = lv_obj_create(test_screen());
    widget.attach(tile, test_screen());

    OpenPicker picker = open_and_tick_second_sensor(widget, test_screen());
    lv_obj_send_event(picker.backdrop, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(find_open_picker(test_screen()).card == nullptr);
}

TEST_CASE_METHOD(ThermistorPickerFixture, "thermistor picker rows hold the sensor name to one line",
                 "[thermistor][one_line]") {
    ThermistorWidget widget("thermistor");
    widget.set_config({{"sensors", std::vector<std::string>{kSensorA}}});
    lv_obj_t* tile = lv_obj_create(test_screen());
    widget.attach(tile, test_screen());
    widget.on_edit_configure();

    OpenPicker picker = find_open_picker(test_screen());
    REQUIRE(picker.sensor_list != nullptr);
    lv_obj_t* row = lv_obj_get_child(picker.sensor_list, row_index_of(kSensorA));
    lv_obj_t* name = helix::test::find_dotted_label(row);
    REQUIRE(name != nullptr);
    CHECK(helix::test::held_to_one_line(name));

    lv_obj_send_event(picker.backdrop, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
}

TEST_CASE_METHOD(ThermistorPickerFixture,
                 "thermistor carousel pages hold the sensor name to one line",
                 "[thermistor][one_line]") {
    ui_carousel_init();
    REQUIRE(register_component("components/thermistor_carousel_page"));
    REQUIRE(register_component("components/panel_widget_thermistor_carousel"));
    lv_xml_register_event_cb(nullptr, "thermistor_carousel_page_cb",
                             ThermistorWidget::carousel_page_cb);
    ThermistorWidget widget("thermistor");
    widget.set_config(
        {{"sensors", std::vector<std::string>{kSensorA, kSensorB}}, {"display_mode", "carousel"}});
    REQUIRE(widget.get_component_name() == "panel_widget_thermistor_carousel");
    auto* tile = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "panel_widget_thermistor_carousel", nullptr));
    REQUIRE(tile != nullptr);
    widget.attach(tile, test_screen());

    lv_obj_t* name = helix::test::find_dotted_label(tile);
    REQUIRE(name != nullptr);
    CHECK(helix::test::held_to_one_line(name));
    widget.detach();
}

namespace {

/// The text of the named label inside one picker row.
std::string row_text(lv_obj_t* row, const char* name) {
    lv_obj_t* label = lv_obj_find_by_name(row, name);
    return label ? lv_label_get_text(label) : std::string("<missing>");
}

} // namespace

// The rows are the thermistor_picker_row component, bound to subjects the
// widget fills: one row per sensor, its name and reading as text, no checkbox
// in the single-select picker and the chosen sensor highlighted.
TEST_CASE_METHOD(ThermistorPickerFixture, "thermistor sensor picker rows bind to the row subjects",
                 "[thermistor][picker_rows]") {
    REQUIRE(register_component("thermistor_sensor_picker"));
    ThermistorWidget widget("thermistor");
    widget.set_config({{"sensors", std::vector<std::string>{kSensorA}}});
    lv_obj_t* tile = lv_obj_create(test_screen());
    widget.attach(tile, test_screen());
    ThermistorTestAccess::click(widget);

    OpenPicker picker = find_open_picker(test_screen());
    REQUIRE(picker.sensor_list != nullptr);
    auto sensors = helix::sensors::TemperatureSensorManager::instance().get_sensors_sorted();
    REQUIRE(lv_obj_get_child_count(picker.sensor_list) == sensors.size());

    const int a = row_index_of(kSensorA);
    const int b = row_index_of(kSensorB);
    lv_obj_t* row_a = lv_obj_get_child(picker.sensor_list, a);
    lv_obj_t* row_b = lv_obj_get_child(picker.sensor_list, b);
    // The name label is bound to the row's subject; DOTS may cut what it draws.
    lv_subject_t* name_subject =
        lv_xml_get_subject(nullptr, ("thermistor_sel_" + std::to_string(a) + "_name").c_str());
    REQUIRE(name_subject != nullptr);
    CHECK(std::string(lv_subject_get_string(name_subject)) ==
          sensors[static_cast<size_t>(a)].display_name);
    CHECK(row_text(row_a, "row_name").rfind("Helix Test Probe", 0) == 0);
    CHECK(row_text(row_a, "row_temp") == "0\xC2\xB0"
                                         "C");
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(row_a, "row_check"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_bg_opa(row_a, LV_PART_MAIN) == 30);
    CHECK(lv_obj_get_style_bg_opa(row_b, LV_PART_MAIN) == 0);

    // A tap on a row chooses it and closes the card.
    lv_obj_send_event(row_b, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
    CHECK(find_open_picker(test_screen()).card == nullptr);
}

// Each carousel page is the thermistor_carousel_page component: its name label
// shows the sensor it was built for and its reading label follows the sensor.
TEST_CASE_METHOD(ThermistorPickerFixture, "thermistor carousel pages bind name and reading",
                 "[thermistor][picker_rows]") {
    ui_carousel_init();
    REQUIRE(register_component("components/thermistor_carousel_page"));
    REQUIRE(register_component("components/panel_widget_thermistor_carousel"));
    ThermistorWidget widget("thermistor");
    widget.set_config(
        {{"sensors", std::vector<std::string>{kSensorA, kSensorB}}, {"display_mode", "carousel"}});
    auto* tile = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "panel_widget_thermistor_carousel", nullptr));
    REQUIRE(tile != nullptr);
    widget.attach(tile, test_screen());

    auto sensors = helix::sensors::TemperatureSensorManager::instance().get_sensors_sorted();
    // The page drops the redundant " Temperature" suffix.
    const auto name_of = [&](const char* klipper) {
        std::string name = sensors[static_cast<size_t>(row_index_of(klipper))].display_name;
        const std::string suffix = " Temperature";
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            name.erase(name.size() - suffix.size());
        return name;
    };
    lv_obj_t* name0 = ThermistorTestAccess::carousel_name_label(widget, 0);
    lv_obj_t* name1 = ThermistorTestAccess::carousel_name_label(widget, 1);
    REQUIRE(name0 != nullptr);
    REQUIRE(name1 != nullptr);
    CHECK(std::string(lv_label_get_text(name0)) == name_of(kSensorA));
    CHECK(std::string(lv_label_get_text(name1)) == name_of(kSensorB));

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    lv_subject_t* temp_a = tsm.get_temp_subject(kSensorA);
    REQUIRE(temp_a != nullptr);
    lv_subject_set_int(temp_a, 427);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* temp0 = ThermistorTestAccess::carousel_temp_label(widget, 0);
    REQUIRE(temp0 != nullptr);
    CHECK(std::string(lv_label_get_text(temp0)).find("42.7") != std::string::npos);
    widget.detach();
}

// A carousel that shows every sensor can attach before discovery lists any;
// the pages must still appear once sensors arrive.
TEST_CASE_METHOD(ThermistorPickerFixture,
                 "thermistor carousel built with no sensors fills in once sensors arrive",
                 "[thermistor][picker_rows]") {
    ui_carousel_init();
    REQUIRE(register_component("components/thermistor_carousel_page"));
    REQUIRE(register_component("components/panel_widget_thermistor_carousel"));
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    tsm.discover(helix::test::discovery_of({}));

    ThermistorWidget widget("thermistor");
    widget.set_config({{"display_mode", "carousel"}});
    auto* tile = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "panel_widget_thermistor_carousel", nullptr));
    REQUIRE(tile != nullptr);
    widget.attach(tile, test_screen());
    REQUIRE(ThermistorTestAccess::carousel_page_count(widget) == 0);

    tsm.discover(helix::test::discovery_of({kSensorA, kSensorB}));
    ThermistorTestAccess::bind_carousel(widget);
    CHECK(ThermistorTestAccess::carousel_page_count(widget) == 2);
    widget.detach();
}
