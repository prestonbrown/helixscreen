// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_sensor_settings_overlay.cpp
 * @brief SensorSettingsOverlay's chamber assignment dropdowns, driven through the widget
 *
 * build_chamber_assignment_options() has its own tests. These pin the overlay's
 * wiring around it: which discovered objects each dropdown is built from, that a
 * saved assignment discovery did not return renders as its own marked, selected
 * option, and that the change handler maps an option index back to the value it
 * persists (#1528).
 */

#include "ui_settings_sensors.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "chamber_assignment_options.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::settings::build_chamber_assignment_options;
using helix::settings::chamber_assignment_labels;

namespace {

constexpr const char* kMissingHeater = "heater_generic ghost_heater";
constexpr const char* kMissingSensor = "temperature_sensor ghost_probe";

/// Puts both chamber assignments and the discovered hardware back, and drops
/// the singleton overlay so the next case builds against its own screen.
class ScopedChamberAssignments {
  public:
    ScopedChamberAssignments()
        : heater_(helix::SettingsManager::instance().get_chamber_heater_assignment()),
          sensor_(helix::SettingsManager::instance().get_chamber_sensor_assignment()) {
        drop_overlay();
    }
    ~ScopedChamberAssignments() {
        drop_overlay();
        helix::SettingsManager::instance().set_chamber_heater_assignment(heater_);
        helix::SettingsManager::instance().set_chamber_sensor_assignment(sensor_);
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
    }
    ScopedChamberAssignments(const ScopedChamberAssignments&) = delete;
    ScopedChamberAssignments& operator=(const ScopedChamberAssignments&) = delete;

  private:
    static void drop_overlay() {
        helix::ui::UpdateQueue::instance().drain();
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::UpdateQueue::instance().drain();
    }

    std::string heater_;
    std::string sensor_;
};

/// A chamber heater leaves discovery's chamber sensor pick empty, so each
/// dropdown's case seeds only the chamber object its role detects.
helix::PrinterDiscovery chamber_discovery(const char* chamber_object) {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"extruder", "heater_bed", chamber_object, "temperature_sensor mcu_temp"}));
    return hw;
}

/// Build the overlay against the test screen and populate it.
lv_obj_t* activate_overlay(lv_obj_t* screen) {
    auto& overlay = helix::settings::get_sensor_settings_overlay();
    overlay.register_callbacks();
    lv_obj_t* root = overlay.create(screen);
    REQUIRE(root != nullptr);
    overlay.on_activate();
    return root;
}

void select_option(lv_obj_t* dropdown, uint32_t index) {
    lv_dropdown_set_selected(dropdown, index);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
}

std::string selected_text(lv_obj_t* dropdown) {
    char buf[128] = {};
    lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
    return buf;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Chamber heater dropdown shows an undiscovered saved heater and maps indices",
                 "[sensors][settings][chamber]") {
    ScopedChamberAssignments restore;
    auto& settings = helix::SettingsManager::instance();
    const auto hw = chamber_discovery("heater_generic chamber");
    get_printer_state().set_hardware(hw);
    settings.set_chamber_heater_assignment(kMissingHeater);

    lv_obj_t* root = activate_overlay(test_screen());
    lv_obj_t* dd = lv_obj_find_by_name(root, "chamber_heater_dropdown");
    REQUIRE(dd != nullptr);

    // Only the generic heater is assignable; the bed and extruder are filtered.
    const auto expected = build_chamber_assignment_options(
        {"heater_generic chamber"}, hw.chamber_heater_name(), kMissingHeater, "heater_generic ",
        chamber_assignment_labels());
    REQUIRE(expected.names.size() == 2);
    CHECK(std::string(lv_dropdown_get_options(dd)) == expected.options);
    CHECK(lv_dropdown_get_selected(dd) == expected.selected);
    CHECK(selected_text(dd).find("ghost_heater") != std::string::npos);
    CHECK(selected_text(dd).find(chamber_assignment_labels().not_detected) != std::string::npos);

    select_option(dd, static_cast<uint32_t>(expected.names.size() + 1));
    CHECK(settings.get_chamber_heater_assignment() == "none");

    select_option(dd, 0);
    CHECK(settings.get_chamber_heater_assignment() == "auto");

    select_option(dd, 1);
    CHECK(settings.get_chamber_heater_assignment() == "heater_generic chamber");

    select_option(dd, expected.selected);
    CHECK(settings.get_chamber_heater_assignment() == kMissingHeater);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Chamber sensor dropdown shows an undiscovered saved sensor and maps indices",
                 "[sensors][settings][chamber]") {
    ScopedChamberAssignments restore;
    auto& settings = helix::SettingsManager::instance();
    const auto hw = chamber_discovery("temperature_sensor chamber");
    get_printer_state().set_hardware(hw);
    settings.set_chamber_sensor_assignment(kMissingSensor);

    lv_obj_t* root = activate_overlay(test_screen());
    lv_obj_t* dd = lv_obj_find_by_name(root, "chamber_sensor_dropdown");
    REQUIRE(dd != nullptr);

    REQUIRE_FALSE(hw.chamber_sensor_name().empty());
    const auto expected =
        build_chamber_assignment_options(hw.sensors(), hw.chamber_sensor_name(), kMissingSensor,
                                         "temperature_sensor ", chamber_assignment_labels());
    REQUIRE(expected.names.size() == hw.sensors().size() + 1);
    CHECK(std::string(lv_dropdown_get_options(dd)) == expected.options);
    CHECK(lv_dropdown_get_selected(dd) == expected.selected);
    CHECK(selected_text(dd).find("ghost_probe") != std::string::npos);
    CHECK(selected_text(dd).find(chamber_assignment_labels().not_detected) != std::string::npos);

    select_option(dd, static_cast<uint32_t>(expected.names.size() + 1));
    CHECK(settings.get_chamber_sensor_assignment() == "none");

    select_option(dd, 0);
    CHECK(settings.get_chamber_sensor_assignment() == "auto");

    select_option(dd, 1);
    CHECK(settings.get_chamber_sensor_assignment() == expected.names[0]);

    select_option(dd, expected.selected);
    CHECK(settings.get_chamber_sensor_assignment() == kMissingSensor);
}
