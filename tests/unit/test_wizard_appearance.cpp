// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_appearance.h"

#include "../../src/ui/wizard/wizard_step_registry.h"
#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "display_settings_manager.h"
#include "settings_manager.h"
#include "system_settings_manager.h"
#include "theme_loader.h"

#include <lvgl/lvgl.h>

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::TimeFormat;
using helix::wizard::StepId;

TEST_CASE("Appearance step follows Language and precedes Wifi", "[wizard][appearance]") {
    CHECK(static_cast<int>(StepId::Appearance) == static_cast<int>(StepId::Language) + 1);
    CHECK(static_cast<int>(StepId::Wifi) == static_cast<int>(StepId::Appearance) + 1);

    helix::wizard::Step* step = helix::wizard::step_by_id(StepId::Appearance);
    REQUIRE(step != nullptr);
    CHECK(step->id() == StepId::Appearance);
    CHECK(std::string(step->component_name()) == "wizard_appearance");
    CHECK(std::string(helix::wizard::to_string(StepId::Appearance)) == "Appearance");
}

TEST_CASE("Appearance step shows on the first printer only", "[wizard][appearance]") {
    helix::wizard::Step* step = helix::wizard::step_by_id(StepId::Appearance);
    REQUIRE(step != nullptr);

    helix::wizard::StepContext first;
    CHECK_FALSE(step->should_skip(first));

    // A preset collapses the hardware steps, never this page.
    helix::wizard::StepContext preset_first;
    preset_first.preset = {true, true};
    CHECK_FALSE(step->should_skip(preset_first));

    helix::wizard::StepContext later;
    later.is_subsequent_printer = true;
    later.preset = {true, false};
    CHECK(step->should_skip(later));
}

TEST_CASE("Appearance: the clock format default follows the language", "[wizard][appearance]") {
    using helix::wizard::default_time_format_for_language;
    CHECK(default_time_format_for_language("en") == TimeFormat::HOUR_12);
    CHECK(default_time_format_for_language("") == TimeFormat::HOUR_12);
    CHECK(default_time_format_for_language("de") == TimeFormat::HOUR_24);
    CHECK(default_time_format_for_language("ja") == TimeFormat::HOUR_24);
}

TEST_CASE("Appearance: the timezone row draws attention only on an accidental UTC",
          "[wizard][appearance]") {
    using helix::wizard::timezone_needs_attention;
    CHECK(timezone_needs_attention(true, false));
    CHECK_FALSE(timezone_needs_attention(true, true));
    CHECK_FALSE(timezone_needs_attention(false, false));
    CHECK_FALSE(timezone_needs_attention(false, true));
}

namespace {

lv_obj_t* row_widget(lv_obj_t* root, const char* row_name, const char* widget) {
    lv_obj_t* row = lv_obj_find_by_name(root, row_name);
    REQUIRE(row != nullptr);
    lv_obj_t* w = lv_obj_find_by_name(row, widget);
    REQUIRE(w != nullptr);
    return w;
}

void pick(lv_obj_t* dropdown, uint32_t index) {
    lv_dropdown_set_selected(dropdown, index);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Appearance rows write the setting Settings reads",
                 "[wizard][appearance]") {
    SettingsManager::instance().init_subjects();
    auto& display = helix::DisplaySettingsManager::instance();

    auto* step = helix::wizard::get_wizard_appearance_step();
    step->register_callbacks();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);

    SECTION("timezone") {
        display.set_timezone_by_index(0);
        lv_obj_t* dd = row_widget(root, "row_timezone", "dropdown");
        // The wizard offers the same list as Settings.
        const std::string options = helix::DisplaySettingsManager::get_timezone_options();
        CHECK(lv_dropdown_get_option_count(dd) ==
              static_cast<uint32_t>(std::count(options.begin(), options.end(), '\n') + 1));
        pick(dd, 3);
        CHECK(display.get_timezone_index() == 3);
        CHECK_FALSE(display.get_timezone().empty());
        display.set_timezone_by_index(0);
    }

    SECTION("time format") {
        display.set_time_format(TimeFormat::HOUR_12);
        pick(row_widget(root, "row_time_format", "dropdown"), 1);
        CHECK(display.get_time_format() == TimeFormat::HOUR_24);
        pick(row_widget(root, "row_time_format", "dropdown"), 0);
        CHECK(display.get_time_format() == TimeFormat::HOUR_12);
    }

    SECTION("speed and flow units") {
        display.set_speed_flow_physical_units(false);
        lv_obj_t* sw = row_widget(root, "row_speed_flow_units", "toggle");
        lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        CHECK(display.get_speed_flow_physical_units());
        display.set_speed_flow_physical_units(false);
    }

    SECTION("theme") {
        const auto themes = helix::discover_themes(helix::get_themes_directory());
        REQUIRE(themes.size() >= 2);
        lv_obj_t* dd = row_widget(root, "row_theme", "dropdown");
        CHECK(lv_dropdown_get_option_count(dd) == themes.size());
        const std::string before = helix::Config::get_instance()->get<std::string>(
            "/display/theme", std::string(helix::kDefaultTheme));
        pick(dd, 1);
        CHECK(helix::Config::get_instance()->get<std::string>("/display/theme", "") ==
              themes[1].filename);
        helix::Config::get_instance()->set<std::string>("/display/theme", before);
    }

    step->cleanup();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Appearance: an unpicked clock format takes the language's",
                 "[wizard][appearance]") {
    SettingsManager::instance().init_subjects();
    auto& display = helix::DisplaySettingsManager::instance();
    auto* config = helix::Config::get_instance();
    auto& system = helix::SystemSettingsManager::instance();
    const std::string language = system.get_language();

    system.set_language("de");
    if (config->exists("/display")) {
        config->get_json("/display").erase("time_format");
    }
    REQUIRE_FALSE(config->exists("/display/time_format"));

    auto* step = helix::wizard::get_wizard_appearance_step();
    step->register_callbacks();
    REQUIRE(step->create(test_screen()) != nullptr);
    CHECK(display.get_time_format() == TimeFormat::HOUR_24);
    CHECK(config->exists("/display/time_format"));
    step->cleanup();

    // A format the user already picked is left alone.
    display.set_time_format(TimeFormat::HOUR_12);
    REQUIRE(step->create(test_screen()) != nullptr);
    CHECK(display.get_time_format() == TimeFormat::HOUR_12);
    step->cleanup();

    system.set_language(language);
}
