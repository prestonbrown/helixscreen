// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_power_device_picker.cpp
 * @brief The power device picker acts on the widget that raised it, and on no
 *        widget once that one is gone
 *
 * Several power_device tiles can share a home page, and they share one XML
 * component and one active-menu registry. A row tap has to reach the tile whose
 * picker is open, and a tile torn down with its picker up (PanelWidget instances
 * are detached and recycled on every rebuild) must take the card with it.
 */

#include "ui_context_menu.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/power_device_widget_test_access.h"
#include "../test_helpers/press_wash_probe.h"
#include "device_display_name.h"
#include "moonraker_types.h"
#include "power_device_state.h"
#include "power_device_widget.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = helix::PowerDeviceWidgetTestAccess;
using helix::test::row_with_label;
using helix::ui::ContextMenu;

namespace {

class PowerPickerFixture : public LVGLUITestFixture {
  public:
    PowerPickerFixture() {
        // show_device_picker() returns early when no power devices exist.
        PowerDeviceState::instance().set_devices({
            PowerDevice{"printer_psu", "gpio", "off", false},
            PowerDevice{"chamber_light", "gpio", "on", false},
        });
    }

    ~PowerPickerFixture() override {
        PowerDeviceState::instance().deinit_subjects();
    }

    std::unique_ptr<PowerDeviceWidget> make_widget(const char* id) {
        auto widget = std::make_unique<PowerDeviceWidget>(id);
        Access::set_screen(*widget, test_screen(), lv_obj_create(test_screen()));
        return widget;
    }

    /// Tap the open picker's row labelled @p label.
    void tap_row(const char* label) {
        lv_obj_t* backdrop = Access::backdrop(test_screen());
        REQUIRE(backdrop != nullptr);
        lv_obj_t* row = row_with_label(backdrop, label);
        REQUIRE(row != nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(PowerPickerFixture,
                 "Power device picker: a row tap configures the widget that "
                 "opened it",
                 "[power_device_widget][picker]") {
    auto first = make_widget("power_device:1");
    auto second = make_widget("power_device:2");

    // Opening the second tile's picker closes the first's: one card at a time.
    Access::show_picker(*first);
    REQUIRE(Access::picker_visible(*first));
    Access::show_picker(*second);
    CHECK_FALSE(Access::picker_visible(*first));
    REQUIRE(Access::picker_visible(*second));
    REQUIRE(ContextMenu::active() == Access::picker(*second));
    process_lvgl(50); // the first card's deferred delete

    tap_row(get_display_name("printer_psu", DeviceType::POWER_DEVICE).c_str());

    CHECK(Access::device_name(*second) == "printer_psu");
    CHECK(Access::device_name(*first).empty());
    CHECK_FALSE(Access::picker_visible(*second));
    CHECK(ContextMenu::active() == nullptr);
    process_lvgl(50);
}

TEST_CASE_METHOD(PowerPickerFixture, "Power device picker: a tap outside picks nothing",
                 "[power_device_widget][picker]") {
    auto widget = make_widget("power_device:1");
    Access::show_picker(*widget);
    lv_obj_t* backdrop = Access::backdrop(test_screen());
    REQUIRE(backdrop != nullptr);

    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);

    CHECK_FALSE(Access::picker_visible(*widget));
    CHECK(Access::device_name(*widget).empty());
    process_lvgl(50);
}

TEST_CASE_METHOD(PowerPickerFixture, "Power device picker: no devices, no picker",
                 "[power_device_widget][picker]") {
    PowerDeviceState::instance().set_devices({});
    auto widget = make_widget("power_device:1");

    Access::show_picker(*widget);

    CHECK_FALSE(Access::picker_visible(*widget));
    CHECK(Access::backdrop(test_screen()) == nullptr);
}

TEST_CASE_METHOD(PowerPickerFixture, "Power device picker: detach closes an open picker",
                 "[power_device_widget][picker][teardown]") {
    auto widget = make_widget("power_device:1");
    Access::show_picker(*widget);
    REQUIRE(Access::picker_visible(*widget));

    // The manager detaches and recycles instances on every rebuild; a card left
    // up would route its taps into a widget with no tile.
    widget->detach();

    CHECK_FALSE(Access::picker_visible(*widget));
    CHECK(ContextMenu::active() == nullptr);
    process_lvgl(50);
    CHECK(Access::backdrop(test_screen()) == nullptr);
}

TEST_CASE_METHOD(PowerPickerFixture,
                 "Power device picker: the widget dying with its picker open is safe",
                 "[power_device_widget][picker][teardown][uaf]") {
    auto widget = make_widget("power_device:1");
    Access::show_picker(*widget);
    lv_obj_t* backdrop = Access::backdrop(test_screen());
    REQUIRE(backdrop != nullptr);

    widget.reset();

    // Nothing may still name the dead widget's picker, and the card it left
    // behind dies on LVGL's async pass without reaching back into it.
    CHECK(ContextMenu::active() == nullptr);
    process_lvgl(50);
    CHECK_FALSE(lv_obj_is_valid(backdrop));
}

TEST_CASE_METHOD(PowerPickerFixture,
                 "Power device picker: a backdrop killed with its screen clears the picker",
                 "[power_device_widget][picker][teardown]") {
    auto widget = make_widget("power_device:1");
    Access::show_picker(*widget);
    lv_obj_t* backdrop = Access::backdrop(test_screen());
    REQUIRE(backdrop != nullptr);

    lv_obj_delete(backdrop);

    CHECK_FALSE(Access::picker_visible(*widget));
    CHECK(ContextMenu::active() == nullptr);

    // And it opens again afterwards.
    Access::show_picker(*widget);
    CHECK(Access::picker_visible(*widget));
    widget.reset();
    process_lvgl(50);
}

TEST_CASE_METHOD(PowerPickerFixture,
                 "Power device picker: a dismissed card dying later leaves a reopened one alone",
                 "[power_device_widget][picker][teardown]") {
    auto widget = make_widget("power_device:1");
    Access::show_picker(*widget);
    lv_obj_t* first = Access::backdrop(test_screen());
    REQUIRE(first != nullptr);

    // Reopen inside one async tick: the first card's deferred delete lands while
    // the second is already on screen.
    Access::hide_picker(*widget);
    REQUIRE(lv_obj_is_valid(first));
    Access::show_picker(*widget);
    REQUIRE(Access::picker_visible(*widget));

    process_lvgl(50);

    CHECK_FALSE(lv_obj_is_valid(first));
    CHECK(Access::picker_visible(*widget));
    CHECK(ContextMenu::active() == Access::picker(*widget));
    widget.reset();
    process_lvgl(50);
}
