// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gate_subject_lifetime.cpp
 * @brief gate_subject_lifetime() returns, for every hardware-gate subject name, the token of
 *        the owner that actually registers that name.
 *
 * An owner-token ObserverGuard ignores the invalidation epoch, so a token from an owner that
 * outlives the subject's real owner would leave a freed observer attached. The proof for each
 * row is behavioural: tear the registering owner down and the token must flip.
 */

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "humidity_sensor_manager.h"
#include "led/led_controller.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "temperature_sensor_manager.h"
#include "width_sensor_manager.h"

#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct Owner {
    std::function<void()> init;
    std::function<void()> deinit;
};

const std::vector<std::pair<std::string, Owner>>& owners() {
    static const std::vector<std::pair<std::string, Owner>> table = [] {
        std::vector<std::pair<std::string, Owner>> t;
        const Owner ams{[] { AmsState::instance().init_subjects(true); },
                        [] { AmsState::instance().deinit_subjects(); }};
        for (const char* n :
             {"ams_slot_count", "ams_supports_bypass", "clog_meter_mode", "buffer_present"})
            t.emplace_back(n, ams);
        t.emplace_back("filament_sensor_count",
                       Owner{[] { FilamentSensorManager::instance().init_subjects(); },
                             [] { FilamentSensorManager::instance().deinit_subjects(); }});
        t.emplace_back("humidity_sensor_count",
                       Owner{[] { sensors::HumiditySensorManager::instance().init_subjects(); },
                             [] { sensors::HumiditySensorManager::instance().deinit_subjects(); }});
        t.emplace_back(
            "temp_sensor_count",
            Owner{[] { sensors::TemperatureSensorManager::instance().init_subjects(); },
                  [] { sensors::TemperatureSensorManager::instance().deinit_subjects(); }});
        t.emplace_back("width_sensor_count",
                       Owner{[] { sensors::WidthSensorManager::instance().init_subjects(); },
                             [] { sensors::WidthSensorManager::instance().deinit_subjects(); }});
        const Owner led{[] { led::LedController::instance().init(nullptr, nullptr); },
                        [] { StaticSubjectRegistry::instance().deinit_one("LedController"); }};
        for (const char* n : {"led_controllable", "led_has_devices"})
            t.emplace_back(n, led);
        t.emplace_back(
            "platform_host_power_supported",
            Owner{[] { app_globals_init_subjects(); }, [] { app_globals_deinit_subjects(); }});
        const Owner printer{[] { get_printer_state().init_subjects(true); },
                            [] { get_printer_state().deinit_subjects(); }};
        for (const char* n : {"power_device_count", "printer_has_chamber"})
            t.emplace_back(n, printer);
        return t;
    }();
    return table;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "gate_subject_lifetime: every gate subject's token flips when its registering "
                 "owner tears down",
                 "[panel_widget][manager][observer]") {
    for (const auto& [name, owner] : owners()) {
        INFO("gate subject " << name);
        owner.deinit();
        owner.init();
        REQUIRE(lv_xml_get_subject(nullptr, name.c_str()) != nullptr);

        SubjectLifetime token = gate_subject_lifetime(name.c_str());
        REQUIRE(token != nullptr);
        REQUIRE(*token);

        owner.deinit();
        CHECK_FALSE(*token);

        owner.init();
    }
}

TEST_CASE("gate_subject_lifetime: every widget gate subject has an owner row",
          "[panel_widget][manager][observer]") {
    std::set<std::string> covered;
    for (const auto& row : owners())
        covered.insert(row.first);
    for (const auto& def : get_all_widget_defs()) {
        if (!def.hardware_gate_subject)
            continue;
        INFO("gate subject " << def.hardware_gate_subject);
        CHECK(covered.count(def.hardware_gate_subject) == 1);
        CHECK(gate_subject_lifetime(def.hardware_gate_subject) != nullptr);
    }
}
