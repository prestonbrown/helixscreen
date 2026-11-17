// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_display.h"
#include "ui_wizard_screen_power.h"

#include "../../src/ui/wizard/wizard_step_registry.h"
#include "../lvgl_ui_test_fixture.h"
#include "display_metrics.h"
#include "display_settings_manager.h"
#include "screensaver_registry.h"
#include "settings_manager.h"

#include <lvgl/lvgl.h>

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::DisplaySettingsManager;
using helix::settings::fitting_ui_scale_percents;
using helix::settings::ui_scale_has_choice;
using helix::wizard::StepId;

TEST_CASE("Screen & Power step follows Appearance and precedes Wifi", "[wizard][screen_power]") {
    CHECK(static_cast<int>(StepId::ScreenPower) == static_cast<int>(StepId::Appearance) + 1);
    CHECK(static_cast<int>(StepId::Wifi) == static_cast<int>(StepId::ScreenPower) + 1);

    helix::wizard::Step* step = helix::wizard::step_by_id(StepId::ScreenPower);
    REQUIRE(step != nullptr);
    CHECK(step->id() == StepId::ScreenPower);
    CHECK(std::string(step->component_name()) == "wizard_screen_power");
    CHECK(std::string(helix::wizard::to_string(StepId::ScreenPower)) == "ScreenPower");
}

TEST_CASE("Screen & Power step shows on the first printer only", "[wizard][screen_power]") {
    helix::wizard::Step* step = helix::wizard::step_by_id(StepId::ScreenPower);
    REQUIRE(step != nullptr);

    helix::wizard::StepContext first;
    CHECK_FALSE(step->should_skip(first));

    helix::wizard::StepContext preset_first;
    preset_first.preset = {true, true};
    CHECK_FALSE(step->should_skip(preset_first));

    helix::wizard::StepContext later;
    later.is_subsequent_printer = true;
    CHECK(step->should_skip(later));
}

TEST_CASE("UI scale: a size is offered only where the smallest layout still fits",
          "[wizard][screen_power][display_settings]") {
    CHECK(fitting_ui_scale_percents(480, 272) == std::vector<int>{100});
    CHECK(fitting_ui_scale_percents(800, 480) == std::vector<int>{100, 125, 150});
    // Orientation does not matter.
    CHECK(fitting_ui_scale_percents(480, 800) == std::vector<int>{100, 125, 150});
    CHECK(fitting_ui_scale_percents(1024, 600) == std::vector<int>{100, 125, 150, 175, 200});
    // Exactly at the edge fits.
    CHECK(fitting_ui_scale_percents(600, 340) == std::vector<int>{100, 125});
}

TEST_CASE("UI scale: the row is a choice only with a size other than Automatic's",
          "[wizard][screen_power][display_settings]") {
    CHECK_FALSE(ui_scale_has_choice({100}, 100));
    CHECK_FALSE(ui_scale_has_choice({}, 100));
    CHECK(ui_scale_has_choice({100, 125}, 100));
    // A high-DPI panel's Automatic is not 100%, so 100% is a different size.
    CHECK(ui_scale_has_choice({100}, 158));
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

/// Puts back every display setting the page can write, however the test ends.
struct DisplaySettingsRestore {
    DisplaySettingsManager& mgr = DisplaySettingsManager::instance();
    int dim = mgr.get_display_dim_sec();
    int sleep = mgr.get_display_sleep_sec();
    int brightness = mgr.get_brightness();
    int scale = mgr.get_ui_scale_percent();
    int saver = mgr.get_screensaver_type();
    ~DisplaySettingsRestore() {
        mgr.set_screensaver_type(saver);
        mgr.set_display_dim_sec(dim);
        mgr.set_display_sleep_sec(sleep);
        mgr.set_brightness(brightness);
        mgr.set_ui_scale_percent(scale);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Screen & Power rows write the setting Settings reads",
                 "[wizard][screen_power][display_settings]") {
    SettingsManager::instance().init_subjects();
    DisplaySettingsRestore restore;
    auto& display = DisplaySettingsManager::instance();
    display.set_screensaver_type(static_cast<int>(ScreensaverType::OFF));

    auto* step = helix::wizard::get_wizard_screen_power_step();
    step->register_callbacks();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);

    SECTION("dim") {
        pick(row_widget(root, "row_display_dim", "dropdown"),
             static_cast<uint32_t>(DisplaySettingsManager::dim_seconds_to_index(60)));
        CHECK(display.get_display_dim_sec() == 60);
    }

    SECTION("sleep") {
        pick(row_widget(root, "row_display_sleep", "dropdown"),
             static_cast<uint32_t>(DisplaySettingsManager::sleep_seconds_to_index(1800)));
        CHECK(display.get_display_sleep_sec() == 1800);
    }

    SECTION("brightness previews on drag and saves on release") {
        display.set_brightness(80);
        lv_obj_t* slider = row_widget(root, "row_brightness", "slider");
        lv_slider_set_value(slider, 40, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "settings_brightness")) == 40);
        lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
        CHECK(display.get_brightness() == 40);
    }

    SECTION("UI scale offers only the sizes that fit, and stores the one picked") {
        const std::vector<int> fitting =
            helix::settings::ui_scale_percents(helix::settings::UiScaleOffer::Fitting);
        REQUIRE_FALSE(fitting.empty());
        lv_obj_t* dd = row_widget(root, "row_ui_scale", "dropdown");
        CHECK(lv_dropdown_get_option_count(dd) == fitting.size() + 1);
        pick(dd, static_cast<uint32_t>(fitting.size()));
        CHECK(display.get_ui_scale_percent() == fitting.back());
        pick(dd, 0);
        CHECK(display.get_ui_scale_percent() == helix::DisplayMetrics::kScaleSettingAutomatic);
    }

#if HELIX_ENABLE_SCREENSAVER
    SECTION("a screensaver turns the dim timeout into the one that starts it") {
        display.set_display_dim_sec(0);
        pick(row_widget(root, "row_screensaver", "dropdown"), 1); // the first saver after Off
        CHECK(display.get_screensaver_type() != static_cast<int>(ScreensaverType::OFF));
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "settings_dim_row_kind")) ==
              static_cast<int>(DisplaySettingsManager::DimRowKind::ScreensaverAfter));
        // The seeded timeout shows on the page's own "Screensaver after" row.
        CHECK(lv_dropdown_get_selected(row_widget(root, "row_screensaver_after", "dropdown")) ==
              static_cast<uint32_t>(DisplaySettingsManager::dim_seconds_to_index(
                  DisplaySettingsManager::kDefaultDimSec)));
    }
#endif

    step->cleanup();
    lv_obj_delete(root);
}
