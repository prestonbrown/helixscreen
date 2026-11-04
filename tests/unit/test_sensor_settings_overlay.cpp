// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_sensor_settings_overlay.cpp
 * @brief SensorSettingsOverlay: the sensor_row sections and the chamber
 *        assignment dropdowns, driven through the widgets
 *
 * Sections: every manager with sensors gets one, its static role options equal
 * the manager's offered roles, a role pick maps through RoleSpec, a held role
 * that is not offered stays until the user picks, rows refresh live, and the
 * no-sensors placeholder shows when every count is 0.
 *
 * Chamber dropdowns: build_chamber_assignment_options() has its own tests. These
 * pin the overlay's wiring around it: which discovered objects each dropdown is
 * built from, that a saved assignment discovery did not return renders as its
 * own marked, selected option, and that the change handler maps an option index
 * back to the value it persists (#1528).
 */

#include "ui_settings_sensors.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "app_globals.h"
#include "chamber_assignment_options.h"
#include "core/data_root_resolver.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "input_sensor_manager.h"
#include "input_sensor_types.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "temperature_sensor_manager.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Re-activating the sensor overlay adds no chamber dropdown handlers",
                 "[sensors][settings][chamber]") {
    ScopedChamberAssignments restore;
    get_printer_state().set_hardware(chamber_discovery("heater_generic chamber"));

    lv_obj_t* root = activate_overlay(test_screen());
    lv_obj_t* heater_dd = lv_obj_find_by_name(root, "chamber_heater_dropdown");
    lv_obj_t* sensor_dd = lv_obj_find_by_name(root, "chamber_sensor_dropdown");
    REQUIRE(heater_dd != nullptr);
    REQUIRE(sensor_dd != nullptr);
    const uint32_t heater_handlers = lv_obj_get_event_count(heater_dd);
    const uint32_t sensor_handlers = lv_obj_get_event_count(sensor_dd);

    helix::settings::get_sensor_settings_overlay().on_activate();
    helix::settings::get_sensor_settings_overlay().on_activate();

    CHECK(lv_obj_get_event_count(heater_dd) == heater_handlers);
    CHECK(lv_obj_get_event_count(sensor_dd) == sensor_handlers);

    // The one handler still maps the rebuilt option list.
    select_option(heater_dd, 1);
    CHECK(helix::SettingsManager::instance().get_chamber_heater_assignment() ==
          "heater_generic chamber");
}

// ============================================================================
// sensor_row sections
// ============================================================================

namespace {

using helix::sensors::RoleSpec;
using helix::sensors::SensorEntry;
using helix::sensors::SensorManager;
using helix::sensors::SensorManagerTestAccess;
using helix::settings::SensorRowSection;

std::string read_text(const std::string& path) {
    std::ifstream in(helix::asset_path(path));
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// One of each sensor kind a section lists.
helix::PrinterDiscovery every_kind_discovery() {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"extruder", "heater_bed", "filament_switch_sensor runout", "gcode_button frontDoor",
         "gcode_button extruder_pos1", "probe", "hall_filament_width_sensor", "bme280 chamber",
         "load_cell", "temperature_sensor mcu_temp"}));
    hw.parse_config_keys(nlohmann::json{{"adxl345", nlohmann::json::object()}});
    return hw;
}

/// Brings every section's manager up in sync mode on @p hw, and puts the
/// managers, the discovery and the overlay back afterwards.
class ScopedSensorManagers {
  public:
    explicit ScopedSensorManagers(const helix::PrinterDiscovery& hw) {
        drop_overlay();
        for (auto* m : helix::sensors::sensor_managers()) {
            if (!m->settings_section()) {
                continue;
            }
            was_initialized_[m] = m->subjects_initialized();
            was_sync_[m] = SensorManagerTestAccess::sync_mode(*m);
            m->init_subjects();
            m->set_sync_mode(true);
            m->discover(hw);
        }
        get_printer_state().set_hardware(hw);
        drain();
    }
    ~ScopedSensorManagers() {
        drop_overlay();
        const helix::PrinterDiscovery none;
        for (auto& [m, initialized] : was_initialized_) {
            m->discover(none);
            m->set_sync_mode(was_sync_[m]);
            if (!initialized) {
                m->deinit_subjects();
            }
        }
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
        drain();
    }
    ScopedSensorManagers(const ScopedSensorManagers&) = delete;
    ScopedSensorManagers& operator=(const ScopedSensorManagers&) = delete;

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

  private:
    static void drop_overlay() {
        drain();
        StaticPanelRegistry::instance().destroy_all();
        drain();
    }

    std::map<SensorManager*, bool> was_initialized_;
    std::map<SensorManager*, bool> was_sync_;
};

/// Every label text in @p root's tree named @p name.
void collect_labels(lv_obj_t* root, const char* name, std::vector<std::string>& out) {
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
        const char* child_name = lv_obj_get_name(child);
        if (child_name && std::strcmp(child_name, name) == 0 &&
            lv_obj_check_type(child, &lv_label_class)) {
            out.emplace_back(lv_label_get_text(child));
        }
        collect_labels(child, name, out);
    }
}

bool hidden_up_to(lv_obj_t* obj, lv_obj_t* root) {
    for (; obj && obj != root; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            return true;
        }
    }
    return false;
}

/// The role_options a section's sensor_row carries in sensors_overlay.xml, split.
std::vector<std::string> xml_role_options(const std::string& xml, const std::string& slug) {
    const std::string anchor = "<sensor_row section=\"" + slug + "\"";
    const size_t at = xml.find(anchor);
    REQUIRE(at != std::string::npos);
    const size_t end = xml.find("/>", at);
    const std::string tag = xml.substr(at, end - at);
    std::vector<std::string> out;
    const std::string key = "role_options=\"";
    const size_t k = tag.find(key);
    if (k == std::string::npos) {
        return out;
    }
    std::string value = tag.substr(k + key.size(), tag.find('"', k + key.size()) - k - key.size());
    size_t pos = 0;
    while (true) {
        const size_t nl = value.find("&#10;", pos);
        out.push_back(value.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 5;
    }
    return out;
}

/// A manager Settings can list without PrinterDiscovery: roles out of enum
/// order and a role it holds but does not offer.
class RowTestManager : public SensorManager {
  public:
    static constexpr uint8_t kFirst = 1;
    static constexpr uint8_t kHidden = 2;
    static constexpr uint8_t kThird = 3;

    RowTestManager() : SensorManager(options_for()) {}

    void set_entries(std::vector<SensorEntry> entries) {
        SensorManagerTestAccess::reconcile(*this, std::move(entries));
    }

  protected:
    void register_subjects(SubjectManager& /*subjects*/) override {}
    void refresh_subjects() override {}
    bool parse_status(const nlohmann::json& /*status*/) override {
        return false;
    }

  private:
    static Options options_for() {
        Options o;
        o.name = "RowTestManager";
        o.count_subject = "rowtest_sensor_count";
        o.revision_subject = "rowtest_sensor_revision";
        o.from_printer_discovery = false;
        o.settings_section = "rowtest";
        o.roles = {{0, "none", "None", false, true},
                   {kThird, "third", "Third", true, true},
                   {kHidden, "hidden", "Hidden", false, false},
                   {kFirst, "first", "First", true, true}};
        return o;
    }
};

SensorEntry row_entry(const char* name, uint8_t role) {
    SensorEntry e;
    e.klipper_name = name;
    e.display_name = name;
    e.role = role;
    e.available = true;
    return e;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Sensor settings: every manager with sensors gets a section",
                 "[sensors][settings]") {
    const std::string xml = read_text("ui_xml/sensors_overlay.xml");
    for (auto* m : helix::sensors::sensor_managers()) {
        if (!m->settings_section()) {
            // Only SensorState, which has no roles and no rows, stays out.
            CHECK(std::string(m->name()) == "SensorState");
            continue;
        }
        const std::string slug = m->settings_section();
        INFO(slug);
        CHECK(xml.find("<repeat count=\"sensor_" + slug + "_row_count\">") != std::string::npos);
        CHECK(xml.find("<sensor_row section=\"" + slug + "\"") != std::string::npos);
    }

    ScopedSensorManagers sensors(every_kind_discovery());
    lv_obj_t* root = activate_overlay(test_screen());
    auto& overlay = helix::settings::get_sensor_settings_overlay();

    std::vector<std::string> names;
    collect_labels(root, "name_label", names);
    for (auto* m : helix::sensors::sensor_managers()) {
        if (!m->settings_section()) {
            continue;
        }
        INFO(m->settings_section());
        REQUIRE(m->sensor_count() > 0);
        SensorRowSection* section = overlay.section(m->settings_section());
        REQUIRE(section != nullptr);
        REQUIRE(section->row_count() > 0);
        CHECK(std::find(names.begin(), names.end(), m->row(0).name) != names.end());
    }
    CHECK(hidden_up_to(lv_obj_find_by_name(root, "no_sensors_placeholder"), root));
}

TEST_CASE("Sensor settings: section role_options match offered_roles", "[sensors][settings]") {
    const std::string xml = read_text("ui_xml/sensors_overlay.xml");
    const std::string en = read_text("translations/en.yml");
    for (auto* m : helix::sensors::sensor_managers()) {
        if (!m->settings_section()) {
            continue;
        }
        INFO(m->settings_section());
        std::vector<std::string> expected;
        for (uint8_t id : m->offered_roles()) {
            const auto& roles = SensorManagerTestAccess::roles(*m);
            const auto spec = std::find_if(roles.begin(), roles.end(),
                                           [id](const RoleSpec& r) { return r.id == id; });
            REQUIRE(spec != roles.end());
            expected.emplace_back(spec->label_key);
            // A label key is a real translation key, so options_tag translates it.
            CHECK(en.find("\n  " + std::string(spec->label_key) + ": ") != std::string::npos);
        }
        CHECK(xml_role_options(xml, m->settings_section()) == expected);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Sensor settings: editing a role saves through RoleSpec, not enum order",
                 "[sensors][settings]") {
    // Offered order None, Third, First: option 1 is role 3, option 2 is role 1.
    RowTestManager manager;
    manager.init_subjects();
    manager.set_entries({row_entry("rowtest a", 0)});
    {
        SubjectManager subjects;
        SensorRowSection section(manager, subjects);
        section.fill(manager);
        section.pick_role(0, 1);
        CHECK(manager.entries()[0].role == RowTestManager::kThird);
        section.pick_role(0, 2);
        CHECK(manager.entries()[0].role == RowTestManager::kFirst);
        section.pick_role(0, 3); // past the options: ignored
        CHECK(manager.entries()[0].role == RowTestManager::kFirst);
        subjects.deinit_all(); // the section's subjects die before the manager does
    }
    manager.deinit_subjects();

    // Through the real dropdown: the filament pick lands as the role and is saved.
    ScopedSensorManagers sensors(every_kind_discovery());
    lv_obj_t* root = activate_overlay(test_screen());
    lv_obj_t* list = lv_obj_find_by_name(root, "switch_sensors_list");
    REQUIRE(list != nullptr);
    lv_obj_t* dd = lv_obj_find_by_name(list, "role_dropdown");
    REQUIRE(dd != nullptr);
    CHECK(std::string(lv_dropdown_get_options(dd)) == "None\nRunout\nToolhead\nEntry");
    select_option(dd, 2);
    auto& filament = helix::FilamentSensorManager::instance();
    CHECK(filament.entries()[0].role == static_cast<uint8_t>(helix::FilamentSensorRole::TOOLHEAD));
    const auto saved = SensorManagerTestAccess::settings_json(filament);
    CHECK(saved["sensors"][0]["role"] == "toolhead");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Sensor settings: a held role that is not offered shows as text and stays "
                 "until changed",
                 "[sensors][settings]") {
    helix::settings::get_sensor_settings_overlay().register_callbacks();
    RowTestManager manager;
    manager.init_subjects();
    manager.set_entries({row_entry("rowtest held", RowTestManager::kHidden)});
    {
        SubjectManager subjects;
        SensorRowSection section(manager, subjects);
        section.fill(manager);

        const char* attrs[] = {
            "section", "rowtest", "index", "0", "role_options", "None\nThird\nFirst", nullptr};
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "sensor_row", attrs));
        REQUIRE(row != nullptr);
        process_lvgl(20);
        lv_obj_t* held = lv_obj_find_by_name(row, "held_role_label");
        lv_obj_t* dd = lv_obj_find_by_name(row, "role_dropdown");
        lv_obj_t* unset = lv_obj_find_by_name(row, "unset_role_dropdown");
        REQUIRE(held != nullptr);
        REQUIRE(dd != nullptr);
        REQUIRE(unset != nullptr);
        CHECK_FALSE(hidden_up_to(held, row));
        CHECK(std::string(lv_label_get_text(held)) == "Hidden");
        // No offered role reads as chosen: the placeholder after them is selected.
        CHECK(hidden_up_to(dd, row));
        CHECK_FALSE(hidden_up_to(unset, row));
        CHECK(lv_dropdown_get_selected(unset) == 3);
        CHECK(selected_text(unset) == "-");

        // A refill (any revision) leaves the held role alone.
        section.fill(manager);
        process_lvgl(10);
        CHECK(manager.entries()[0].role == RowTestManager::kHidden);
        CHECK(std::string(lv_label_get_text(held)) == "Hidden");

        // Picking the placeholder changes nothing.
        select_option(unset, 3);
        CHECK(manager.entries()[0].role == RowTestManager::kHidden);

        // Any offered option replaces it.
        select_option(unset, 1);
        CHECK(manager.entries()[0].role == RowTestManager::kThird);
        section.fill(manager);
        process_lvgl(10);
        CHECK(hidden_up_to(held, row));
        CHECK(hidden_up_to(unset, row));
        CHECK_FALSE(hidden_up_to(dd, row));
        CHECK(lv_dropdown_get_selected(dd) == 1);
        lv_obj_delete(row);
        subjects.deinit_all(); // the section's subjects die before the manager does
    }
    manager.deinit_subjects();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Sensor settings: input door state updates live while the overlay is open",
                 "[sensors][settings][input_sensors]") {
    ScopedSensorManagers sensors(every_kind_discovery());
    auto& input = helix::sensors::InputSensorManager::instance();
    input.set_role("gcode_button frontDoor",
                   static_cast<uint8_t>(helix::sensors::InputSensorRole::DOOR));
    input.update_from_status({{"gcode_button frontDoor", {{"state", "PRESSED"}}}});
    lv_obj_t* root = activate_overlay(test_screen());
    ScopedSensorManagers::drain();
    lv_obj_t* list = lv_obj_find_by_name(root, "input_sensors_list");
    REQUIRE(list != nullptr);
    lv_obj_t* state = lv_obj_find_by_name(list, "state_label");
    REQUIRE(state != nullptr);
    CHECK(std::string(lv_label_get_text(state)) == "Closed");

    input.update_from_status({{"gcode_button frontDoor", {{"state", "RELEASED"}}}});
    ScopedSensorManagers::drain();
    process_lvgl(10);
    state = lv_obj_find_by_name(list, "state_label");
    REQUIRE(state != nullptr);
    CHECK(std::string(lv_label_get_text(state)) == "Opened");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Sensor settings: the no-sensors placeholder shows when every count is 0",
                 "[sensors][settings]") {
    ScopedSensorManagers sensors{helix::PrinterDiscovery{}};
    lv_obj_t* root = activate_overlay(test_screen());
    lv_obj_t* placeholder = lv_obj_find_by_name(root, "no_sensors_placeholder");
    REQUIRE(placeholder != nullptr);
    CHECK_FALSE(lv_obj_has_flag(placeholder, LV_OBJ_FLAG_HIDDEN));

    // One sensor appearing while the overlay is open hides it.
    helix::sensors::InputSensorManager::instance().discover(every_kind_discovery());
    ScopedSensorManagers::drain();
    CHECK(lv_obj_has_flag(placeholder, LV_OBJ_FLAG_HIDDEN));
}

// Switch sensors a filament system owns are listed nowhere in Settings, so a
// printer whose every switch sensor is one shows no switch section and no
// monitoring toggle over an empty list.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "Sensor settings: switch sensors a filament system owns leave no empty section",
                 "[sensors][settings][filament_sensor]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"extruder", "filament_switch_sensor lane_1", "filament_switch_sensor slot_2"}));
    ScopedSensorManagers sensors(hw);
    REQUIRE(helix::FilamentSensorManager::instance().sensor_count() == 2);

    lv_obj_t* root = activate_overlay(test_screen());
    auto& overlay = helix::settings::get_sensor_settings_overlay();
    SensorRowSection* section = overlay.section("filament");
    REQUIRE(section != nullptr);
    REQUIRE(section->row_count() == 0);

    lv_obj_t* switches = lv_obj_find_by_name(root, "switch_sensors_section");
    REQUIRE(switches != nullptr);
    CHECK(lv_obj_has_flag(switches, LV_OBJ_FLAG_HIDDEN));
    // Nothing is listed anywhere, so the placeholder says so.
    lv_obj_t* placeholder = lv_obj_find_by_name(root, "no_sensors_placeholder");
    REQUIRE(placeholder != nullptr);
    CHECK_FALSE(lv_obj_has_flag(placeholder, LV_OBJ_FLAG_HIDDEN));
}

// Temperature rows show no reading, so a status frame refills nothing; a change
// to the entries still does.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "Sensor settings: a temperature status update does not refill the section",
                 "[sensors][settings][temperature]") {
    ScopedSensorManagers sensors(every_kind_discovery());
    activate_overlay(test_screen());
    ScopedSensorManagers::drain();
    lv_subject_t* name0 = lv_xml_get_subject(nullptr, "sensor_temp_row_name_0");
    REQUIRE(name0 != nullptr);
    const std::string filled = lv_subject_get_string(name0);
    REQUIRE_FALSE(filled.empty());
    lv_subject_copy_string(name0, "untouched");

    auto& temps = helix::sensors::TemperatureSensorManager::instance();
    temps.update_from_status({{"temperature_sensor mcu_temp", {{"temperature", 41.5}}}});
    ScopedSensorManagers::drain();
    CHECK(std::string(lv_subject_get_string(name0)) == "untouched");

    temps.set_enabled("temperature_sensor mcu_temp", false);
    ScopedSensorManagers::drain();
    CHECK(std::string(lv_subject_get_string(name0)) == filled);
}

// Rows follow get_sensors_sorted(): chamber, MCU, host, drivers, then auxiliary.
TEST_CASE_METHOD(LVGLUITestFixture, "Sensor settings: temperature rows are in priority order",
                 "[sensors][settings][temperature]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(
        nlohmann::json::array({"temperature_sensor zz_aux", "tmc2240 stepper_x",
                               "temperature_sensor raspberry_pi", "temperature_sensor mcu_temp"}));
    ScopedSensorManagers sensors(hw);
    auto& temps = helix::sensors::TemperatureSensorManager::instance();
    const auto sorted = temps.get_sensors_sorted();
    std::vector<std::string> expected;
    for (const auto& s : sorted) {
        expected.push_back(s.klipper_name);
    }
    REQUIRE(expected.front() != "temperature_sensor zz_aux");

    const auto all = temps.entries();
    std::vector<std::string> rows;
    for (const size_t i : temps.settings_rows()) {
        rows.push_back(all[i].klipper_name);
    }
    CHECK(rows == expected);
}

// No card ends in a divider with nothing below it.
TEST_CASE_METHOD(LVGLUITestFixture, "Sensor settings: no section's last row draws a divider",
                 "[sensors][settings]") {
    ScopedSensorManagers sensors(every_kind_discovery());
    lv_obj_t* root = activate_overlay(test_screen());
    ScopedSensorManagers::drain();
    process_lvgl(20);
    for (const char* list : {"switch_sensors_list", "input_sensors_list", "temp_sensors_list"}) {
        INFO(list);
        lv_obj_t* container = lv_obj_find_by_name(root, list);
        REQUIRE(container != nullptr);
        const uint32_t n = lv_obj_get_child_count(container);
        REQUIRE(n > 0);
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_t* divider = lv_obj_find_by_name(
                lv_obj_get_child(container, static_cast<int32_t>(i)), "row_divider");
            REQUIRE(divider != nullptr);
            CHECK(hidden_up_to(divider, root) == (i + 1 == n));
        }
    }
}
