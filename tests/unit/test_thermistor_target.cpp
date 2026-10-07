// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/thermistor_test_access.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"

#include <algorithm>
#include <array>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A thermistor tile bound to one sensor, with a real controller behind it so
/// the gcode history shows which heater a target reached.
class ThermistorTargetFixture : public LVGLUITestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState printer;
    MoonrakerAPI api{client, printer};
    std::shared_ptr<TemperatureController> controller{
        std::make_shared<TemperatureController>(printer, &api)};

    ThermistorTargetFixture() {
        printer.init_subjects(false);
        printer.set_klippy_state_sync(KlippyState::READY);
        PanelWidgetManager::instance().register_shared_resource<TemperatureController>(controller);
        auto& tsm = sensors::TemperatureSensorManager::instance();
        tsm.init_subjects();
        tsm.discover({"heater_generic filament_dryer", "temperature_sensor mcu_temp"});
        std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
        client.clear_gcode_script_history();
    }

    ~ThermistorTargetFixture() override {
        // The keypad tree hangs off the screen; free it so the next case starts
        // with none.
        NavigationManager::instance().shutdown();
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(100);
        sensors::TemperatureSensorManager::instance().discover({});
        PanelWidgetManager::instance().unregister_shared_resource<TemperatureController>();
    }

    bool sent(const char* needle) const {
        const auto& hist = client.gcode_script_history();
        return std::any_of(hist.begin(), hist.end(), [needle](const std::string& g) {
            return g.find(needle) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(ThermistorTargetFixture,
                 "Thermistor tile: a tap on a heater sets that heater's target",
                 "[thermistor][heater_generic]") {
    ThermistorWidget widget("thermistor_target_test");
    widget.set_config({{"sensors", {"heater_generic filament_dryer"}}});

    ThermistorTestAccess::click(widget);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ui_keypad_is_visible());

    ThermistorTestAccess::keypad_confirm(55.0f);
    CHECK(sent("SET_HEATER_TEMPERATURE HEATER=filament_dryer TARGET=55"));
}

TEST_CASE_METHOD(ThermistorTargetFixture,
                 "Thermistor tile: a tap on a read-only sensor sets nothing",
                 "[thermistor][heater_generic]") {
    ThermistorWidget widget("thermistor_target_test");
    widget.set_config({{"sensors", {"temperature_sensor mcu_temp"}}});

    ThermistorTestAccess::click(widget);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(ui_keypad_is_visible());
    CHECK(client.gcode_script_history().empty());
}

namespace {
/// Build the tile the way the home grid does and attach @p widget to it.
lv_obj_t* build_tile(ThermistorWidget& widget, lv_obj_t* screen) {
    auto* tile = static_cast<lv_obj_t*>(
        lv_xml_create(screen, widget.get_component_name().c_str(), widget.xml_attrs()));
    REQUIRE(tile != nullptr);
    widget.attach(tile, screen);
    helix::ui::UpdateQueue::instance().drain();
    return tile;
}

bool shown(lv_obj_t* tile, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(tile, name);
    REQUIRE(obj != nullptr);
    return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
} // namespace

TEST_CASE_METHOD(ThermistorTargetFixture,
                 "Thermistor tile: a saved sensor the printer does not report reads --",
                 "[thermistor][heater_generic]") {
    ThermistorWidget widget("thermistor_missing_test");
    widget.set_config({{"sensors", {"temperature_sensor gone"}}});
    lv_obj_t* tile = build_tile(widget, test_screen());

    CHECK(shown(tile, "thermistor_unavailable"));
    CHECK_FALSE(shown(tile, "thermistor_temp"));

    widget.detach();
    lv_obj_delete(tile);
}

TEST_CASE_METHOD(ThermistorTargetFixture, "Thermistor tile: a reported sensor shows its reading",
                 "[thermistor][heater_generic]") {
    ThermistorWidget widget("thermistor_present_test");
    widget.set_config({{"sensors", {"temperature_sensor mcu_temp"}}});
    lv_obj_t* tile = build_tile(widget, test_screen());

    CHECK_FALSE(shown(tile, "thermistor_unavailable"));
    CHECK(shown(tile, "thermistor_temp"));

    widget.detach();
    lv_obj_delete(tile);
}
