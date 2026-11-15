// SPDX-License-Identifier: GPL-3.0-or-later
//
// Temperature in °F and the date format setting (prestonbrown/helixscreen#2099):
// the one conversion and its rounding, every shared route shown in °F, entry back
// to Celsius, each date order, region defaults and the settings that persist them.

#include "ui_component_keypad.h"
#include "ui_dryer_presentation.h"
#include "ui_temp_display.h"
#include "ui_temperature_utils.h"
#include "ui_wizard_appearance.h"

#include "../../src/ui/panel_widgets/callout_chip.h"
#include "../../src/ui/panel_widgets/host_health_layout.h"
#include "../lvgl_ui_test_fixture.h"
#include "ams_error.h"
#include "config.h"
#include "core/temperature_units.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "fit_check.h"
#include "locale_formats.h"
#include "preset_materials.h"
#include "printer_state.h"
#include "region_defaults.h"
#include "settings_manager.h"
#include "system_settings_manager.h"

#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::DateFormat;
using helix::units::TempUnit;
namespace units = helix::units;
namespace temperature = helix::ui::temperature;

namespace {

constexpr TempUnit kC = TempUnit::Celsius;
constexpr TempUnit kF = TempUnit::Fahrenheit;

/// Shows °F for one scope; the fixture resets it too, this keeps a failing
/// CHECK from leaking it into the next SECTION.
struct FahrenheitScope {
    FahrenheitScope() {
        units::set_display_temp_unit(kF);
    }
    ~FahrenheitScope() {
        units::set_display_temp_unit(kC);
    }
};

std::tm date(int year, int month, int day) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_wday = 1;
    return t;
}

int this_year() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return local.tm_year + 1900;
}

void erase_display_key(const char* key) {
    auto* config = helix::Config::get_instance();
    if (config->exists("/display")) {
        config->get_json("/display").erase(key);
    }
    if (std::string(key) == "date_format") {
        helix::set_saved_date_format(std::nullopt);
    }
}

} // namespace

// ============================================================================
// The conversion and its one rounding rule
// ============================================================================

TEST_CASE("temperature units: Celsius converts to the display unit", "[temp_units]") {
    CHECK(units::celsius_to_unit(0, kF) == Catch::Approx(32.0));
    CHECK(units::celsius_to_unit(100, kF) == Catch::Approx(212.0));
    CHECK(units::celsius_to_unit(-40, kF) == Catch::Approx(-40.0));
    CHECK(units::celsius_to_unit(-10, kF) == Catch::Approx(14.0));
    CHECK(units::celsius_to_unit(37.5, kC) == Catch::Approx(37.5));

    // The largest target any keypad offers (500°C) and its whole-degree reading.
    CHECK(units::celsius_to_unit_whole(500, kF) == 932);
    CHECK(units::celsius_to_unit_whole(221, kF) == 430); // 429.8
    CHECK(units::celsius_to_unit_whole(-17.5, kF) == 1); // 0.5 rounds away from zero
    CHECK(units::celsius_to_unit_whole(-18.5, kF) == -1);

    // A difference scales without the offset.
    CHECK(units::celsius_delta_to_unit(5, kF) == Catch::Approx(9.0));
    CHECK(units::celsius_delta_to_unit(5, kC) == Catch::Approx(5.0));
    CHECK(std::string(units::temp_unit_symbol(kF)) == "°F");
    CHECK(std::string(units::temp_unit_symbol(kC)) == "°C");
}

TEST_CASE("temperature units: a typed value becomes one whole Celsius target", "[temp_units]") {
    CHECK(units::unit_to_celsius_whole(932, kF) == 500);
    CHECK(units::unit_to_celsius_whole(212, kF) == 100);
    CHECK(units::unit_to_celsius_whole(32, kF) == 0);
    CHECK(units::unit_to_celsius_whole(0, kF) == -18); // -17.8
    CHECK(units::unit_to_celsius_whole(-40, kF) == -40);
    CHECK(units::unit_to_celsius_whole(33, kF) == 1);  // 0.56
    CHECK(units::unit_to_celsius_whole(31, kF) == -1); // -0.56
    CHECK(units::unit_to_celsius_whole(2.5, kC) == 3); // half rounds away from zero
    CHECK(units::unit_to_celsius_whole(-2.5, kC) == -3);
    CHECK(units::unit_to_celsius_whole(209.6, kC) == 210);

    // Every whole Celsius target survives the trip out to °F and back.
    for (int c = -40; c <= 500; ++c) {
        INFO(c);
        REQUIRE(units::unit_to_celsius_whole(units::celsius_to_unit_whole(c, kF), kF) == c);
    }
}

TEST_CASE("temperature units: a Celsius axis ticks on round values of the unit shown",
          "[temp_units]") {
    const auto c = units::temp_axis_ticks(0, 300, 50, kC);
    REQUIRE(c.size() == 7);
    CHECK(c.front().label == 0);
    CHECK(c.back().label == 300);

    // 0-300°C reads 32-572°F; a 50°C step is 90°F, so the ticks fall every 100°F.
    const auto f = units::temp_axis_ticks(0, 300, 50, kF);
    REQUIRE(f.size() == 5);
    CHECK(f.front().label == 100);
    CHECK(f.front().celsius == Catch::Approx(37.78).epsilon(0.001));
    CHECK(f.back().label == 500);
    CHECK(units::temp_axis_ticks(0, 300, 0, kF).empty());
}

// ============================================================================
// Every shared route in °F
// ============================================================================

TEST_CASE("temperature units: the shared readouts show °F", "[temp_units]") {
    FahrenheitScope f;
    char buf[48];
    CHECK(std::string(temperature::format_temperature(210, buf, sizeof(buf))) == "410°F");
    CHECK(std::string(temperature::format_temperature_pair(25, 210, buf, sizeof(buf))) ==
          "77 / 410°F");
    CHECK(std::string(temperature::format_temperature_pair(25, 0, buf, sizeof(buf))) == "77 / —°F");
    CHECK(std::string(temperature::format_target_or_off(0, buf, sizeof(buf))) == "—°F");
    CHECK(std::string(temperature::format_temperature_range(200, 230, buf, sizeof(buf))) ==
          "392-446°F");
    // The compact rule applies to the shown number: 40°C is 104°F, three digits.
    CHECK(std::string(temperature::format_temp_number(40.0f, buf, sizeof(buf))) == "104");
    CHECK(std::string(temperature::format_temperature_f(37.5f, buf, sizeof(buf))) == "99.5°F");
    CHECK(temperature::heater_display(2100, 2200).temp == "410 / 428°F");
    CHECK(std::string(temperature::get_extrusion_safety_status(160, 170)) ==
          "Heating (18°F below minimum)");

    CHECK(units::format_display_temp(-10) == "14°F");
    CHECK(helix::format_cpu_temp(614, 1) == "142.5°F");
    CHECK(helix::ui::compact_callout_text("60 / 140°F") == "60 / 140°");
}

TEST_CASE("temperature units: notices and status text name °F", "[temp_units]") {
    FahrenheitScope f;
    const helix::AmsError cold = helix::AmsErrorHelper::extruder_cold(150, 170);
    CHECK(cold.suggestion.find("338°F") != std::string::npos);

    helix::FitFinding over;
    over.kind = helix::FitKind::TempOverMax;
    over.heater = "heater_bed";
    over.value = 120;
    over.limit = 110;
    CHECK(helix::fit_finding_text(over) ==
          "Heats the bed to 248°F, above this printer's 230°F limit.");

    helix::DryerInfo dryer;
    dryer.active = true;
    dryer.phase = helix::DryingPhase::Heating;
    dryer.target_temp_c = 55;
    dryer.remaining_min = 90;
    CHECK(helix::ui::drying_status_text(dryer) == "Heating to 131°F: 1:30 left");
}

TEST_CASE("temperature units: the keypad takes °F and hands back whole Celsius", "[temp_units]") {
    ui_keypad_config_t config{};
    config.initial_value = 210;
    config.min_value = 0;
    config.max_value = 287;
    config.unit_label = "°C";
    config.temperature = true;

    const ui_keypad_config_t shown = helix::ui::keypad_config_in_unit(config, kF);
    CHECK(shown.initial_value == 410.0f);
    CHECK(shown.min_value == 32.0f);
    CHECK(shown.max_value == 548.0f); // 548.6, narrowed inside the Celsius range
    CHECK(std::string(shown.unit_label) == "°F");

    CHECK(helix::ui::keypad_value_to_celsius(410, 0, 287, kF) == 210.0f);
    CHECK(helix::ui::keypad_value_to_celsius(548, 0, 287, kF) == 287.0f);
    CHECK(helix::ui::keypad_value_to_celsius(999, 0, 287, kF) == 287.0f);
    CHECK(helix::ui::keypad_value_to_celsius(210, 0, 287, kC) == 210.0f);

    config.temperature = false;
    const ui_keypad_config_t plain = helix::ui::keypad_config_in_unit(config, kF);
    CHECK(plain.max_value == 287.0f);
    CHECK(std::string(plain.unit_label) == "°C");
}

// ============================================================================
// Region defaults
// ============================================================================

TEST_CASE("temperature units: region defaults follow the zone, then the language", "[temp_units]") {
    using helix::region_defaults;
    CHECK(region_defaults("en", "America/Chicago").temp_unit == kF);
    CHECK(region_defaults("en", "America/Chicago").date_format == DateFormat::MDY);
    CHECK(region_defaults("zh", "Pacific/Honolulu").temp_unit == kF);
    CHECK(region_defaults("en", "US/Eastern").temp_unit == kF);
    CHECK(region_defaults("en", "Europe/London").temp_unit == kC);
    CHECK(region_defaults("en", "Europe/London").date_format == DateFormat::DMY);
    CHECK(region_defaults("en", "America/Mexico_City").temp_unit == kC);
    CHECK(region_defaults("en", "").date_format == DateFormat::MDY);
    CHECK(region_defaults("en", "").temp_unit == kC);
    CHECK(region_defaults("de", "").date_format == DateFormat::DMY);
    CHECK(helix::default_date_format_for_language("xx") == DateFormat::MDY);
    CHECK(region_defaults("ja", "Asia/Tokyo").date_format == DateFormat::YMD);
}

// ============================================================================
// The date formatter, for each order
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "temperature units: dates follow the chosen order",
                 "[temp_units][date_format]") {
    auto& display = helix::DisplaySettingsManager::instance();
    helix::ui::locale_set_language("en");
    const std::tm now_year = date(this_year(), 3, 9);
    const std::tm old_year = date(2024, 3, 9);

    display.set_date_format(DateFormat::MDY);
    CHECK(helix::ui::format_localized_month_day(&now_year) == "Mar 9");
    CHECK(helix::ui::format_localized_short_date(&old_year) == "Mar 09 '24");
    CHECK(helix::ui::format_localized_long_date(&now_year).find("March 9, ") == 0);

    display.set_date_format(DateFormat::DMY);
    CHECK(helix::ui::format_localized_month_day(&now_year) == "9 Mar");
    CHECK(helix::ui::format_localized_short_date(&old_year) == "09 Mar '24");
    CHECK(helix::ui::format_localized_long_date(&old_year) == "9 March 2024");

    display.set_date_format(DateFormat::YMD);
    CHECK(helix::ui::format_localized_month_day(&now_year) == "3/9");
    CHECK(helix::ui::format_localized_short_date(&old_year) == "2024/3/9");
    CHECK(helix::ui::format_localized_long_date(&old_year) == "2024 March 9");
    CHECK(helix::ui::format_localized_date(&now_year) == "3/9 (Mon)");

    erase_display_key("date_format");
}

// ============================================================================
// Settings persistence
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "temperature units: both settings persist",
                 "[temp_units][settings]") {
    auto& display = helix::DisplaySettingsManager::instance();
    auto* config = helix::Config::get_instance();
    lv_subject_t* symbol = lv_xml_get_subject(nullptr, "settings_temp_unit_symbol");
    REQUIRE(symbol != nullptr);

    SECTION("the temperature unit is saved and shown everywhere") {
        display.set_temp_unit(kF);
        CHECK(config->get<int>("/display/temp_unit", -1) == 1);
        CHECK(units::display_temp_unit() == kF);
        CHECK(std::string(lv_subject_get_string(symbol)) == "°F");
        display.set_temp_unit(kC);
        CHECK(config->get<int>("/display/temp_unit", -1) == 0);
        CHECK(std::string(lv_subject_get_string(symbol)) == "°C");
    }

    SECTION("a saved date order wins over the language's") {
        display.set_date_format(DateFormat::YMD);
        CHECK(config->get<int>("/display/date_format", -1) == 2);
        helix::SystemSettingsManager::instance().set_language("de");
        CHECK(display.get_date_format() == DateFormat::YMD);
        erase_display_key("date_format");
    }

    SECTION("an unsaved date order follows the language") {
        erase_display_key("date_format");
        helix::SystemSettingsManager::instance().set_language("de");
        CHECK(display.get_date_format() == DateFormat::DMY);
        CHECK(lv_subject_get_int(display.subject_date_format()) ==
              static_cast<int>(DateFormat::DMY));
        helix::SystemSettingsManager::instance().set_language("ja");
        CHECK(display.get_date_format() == DateFormat::YMD);
        CHECK_FALSE(config->exists("/display/date_format"));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "temperature units: the wizard's unit rows follow the region until picked",
                 "[temp_units][wizard]") {
    SettingsManager::instance().init_subjects();
    auto& display = helix::DisplaySettingsManager::instance();
    erase_display_key("temp_unit");
    erase_display_key("date_format");
    display.set_timezone("America/Chicago");

    auto* step = helix::wizard::get_wizard_appearance_step();
    step->register_callbacks();
    REQUIRE(step->create(test_screen()) != nullptr);
    CHECK(display.get_temp_unit() == kF);
    CHECK(display.get_date_format() == DateFormat::MDY);

    // Another zone moves both rows while nobody has picked them.
    display.set_timezone("Europe/Berlin");
    step->apply_region_defaults();
    CHECK(display.get_temp_unit() == kC);
    CHECK(display.get_date_format() == DateFormat::DMY);

    // A pick holds against the next zone.
    step->on_temp_unit_picked(static_cast<int>(kF));
    display.set_timezone("Europe/Paris");
    step->apply_region_defaults();
    CHECK(display.get_temp_unit() == kF);
    step->cleanup();

    display.set_temp_unit(kC);
    display.set_timezone("");
    erase_display_key("temp_unit");
    erase_display_key("date_format");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "temperature units: a shown reading and the presets switch unit live",
                 "[temp_units][temp_display]") {
    auto& display = helix::DisplaySettingsManager::instance();
    helix::presets::init_subjects();
    const char* attrs[] = {"show_target", "true", "size", "md", nullptr};
    auto* td = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "temp_display", attrs));
    REQUIRE(td != nullptr);
    ui_temp_display_set(td, 25, 210);
    // Children in creation order: current, separator, target, unit.
    auto text = [td](int child) {
        return std::string(lv_label_get_text(lv_obj_get_child(td, child)));
    };
    REQUIRE(text(2) == "210");
    REQUIRE(text(3) == "°C");

    display.set_temp_unit(kF);
    CHECK(text(2) == "410");
    CHECK(text(3) == "°F");
    CHECK(text(0) == "77.0");
    lv_subject_t* preset = lv_xml_get_subject(nullptr, "preset_material_0_temps");
    REQUIRE(preset != nullptr);
    REQUIRE(helix::presets::slot_temps(0).has_value());
    CHECK(std::string(lv_subject_get_string(preset)) ==
          units::format_display_temp(helix::presets::slot_temps(0)->nozzle) + " / " +
              units::format_display_temp(helix::presets::slot_temps(0)->bed));
    display.set_temp_unit(kC);
    CHECK(text(2) == "210");
    erase_display_key("temp_unit");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "temperature units: a target-only string follows a switch without a reading",
                 "[temp_units]") {
    auto& display = helix::DisplaySettingsManager::instance();
    lv_subject_t* target_c = lv_xml_get_subject(nullptr, "chamber_air_target_c");
    lv_subject_t* target_text = lv_xml_get_subject(nullptr, "chamber_air_target_text");
    REQUIRE(target_c != nullptr);
    REQUIRE(target_text != nullptr);
    lv_subject_set_int(target_c, 50);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(std::string(lv_subject_get_string(target_text)) == "50°C");

    // Every printer temperature subject, counted: a switch must not wake them.
    int raw_fires = 0;
    std::vector<lv_observer_t*> counters;
    std::vector<lv_subject_t*> raws{state().temperature_state().get_chamber_target_subject(),
                                    target_c};
    for (const char* name :
         {"extruder_temp", "extruder_target", "bed_temp", "bed_target", "chamber_temp"}) {
        raws.push_back(lv_xml_get_subject(nullptr, name));
    }
    for (lv_subject_t* raw : raws) {
        REQUIRE(raw != nullptr);
        counters.push_back(lv_subject_add_observer(
            raw,
            [](lv_observer_t* o, lv_subject_t*) {
                ++*static_cast<int*>(lv_observer_get_user_data(o));
            },
            &raw_fires));
    }
    raw_fires = 0; // registration fires each once

    display.set_temp_unit(kF);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(std::string(lv_subject_get_string(target_text)) == "122°F");
    display.set_temp_unit(kC);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(std::string(lv_subject_get_string(target_text)) == "50°C");
    CHECK(raw_fires == 0);

    for (auto* o : counters) {
        lv_observer_delete(o);
    }
    erase_display_key("temp_unit");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "temperature units: a date formats off the main thread from the saved order alone",
                 "[temp_units][date_format]") {
    helix::ui::locale_set_language("en");
    const std::tm day = date(2024, 3, 9);
    // Config alone does not change the order: formatting never reads it.
    helix::Config::get_instance()->set<int>("/display/date_format", 2);
    CHECK(helix::ui::format_localized_short_date(&day) == "Mar 09 '24");

    helix::set_saved_date_format(DateFormat::YMD);
    std::string from_thread;
    std::thread([&] { from_thread = helix::ui::format_localized_short_date(&day); }).join();
    CHECK(from_thread == "2024/3/9");

    helix::set_saved_date_format(std::nullopt);
    erase_display_key("date_format");
}

TEST_CASE("temperature units: 0 still turns a heater off on a °F keypad", "[temp_units]") {
    CHECK(helix::ui::keypad_typed_off(0, 0));
    CHECK_FALSE(helix::ui::keypad_typed_off(0, 35)); // a dryer's range has no off
    CHECK_FALSE(helix::ui::keypad_typed_off(32, 0));
    CHECK(helix::ui::keypad_value_to_celsius(0, 0, 287, kF) == 0.0f);
    CHECK(helix::ui::keypad_value_to_celsius(0, 35, 70, kF) == 35.0f);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "temperature units: a reading with a unit of its own is never converted",
                 "[temp_units][temp_display]") {
    FahrenheitScope f;
    // Registered once for the process: the XML scope keeps the name.
    static lv_subject_t raw;
    if (!lv_xml_get_subject(nullptr, "test_temp_units_raw")) {
        lv_subject_init_int(&raw, 0);
        lv_xml_register_subject(nullptr, "test_temp_units_raw", &raw);
    }
    lv_subject_set_int(&raw, 0);
    const char* attrs[] = {"show_target", "true", "bind_raw", "test_temp_units_raw", nullptr};
    auto* td = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "temp_display", attrs));
    REQUIRE(td != nullptr);
    ui_temp_display_set(td, 40, 50);
    auto text = [td](int child) {
        return std::string(lv_label_get_text(lv_obj_get_child(td, child)));
    };
    REQUIRE(text(0) == "104");
    lv_subject_set_int(&raw, 1);
    CHECK(text(0) == "40.0");
    CHECK(text(2) == "50");

    char buf[16];
    CHECK(std::string(temperature::format_number_compact(40.0, buf, sizeof(buf))) == "40.0");
    lv_obj_delete(td);
}
