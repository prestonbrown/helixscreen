// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_language_codes.cpp
 * @brief Every row of the first-run language chooser saves its own language.
 *
 * The rows carry the Settings dropdown index as user_data, so a row's code must be
 * SystemSettingsManager's code for that index. Reads ui_xml/ from disk, so run it
 * from the repo root.
 */

#include "ui_wizard.h"
#include "ui_wizard_language_chooser.h"

#include "../lvgl_ui_test_fixture.h"
#include "system_settings_manager.h"

#include <lvgl/lvgl.h>

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// The language code a row's name spells, e.g. "lang_item_pl" -> "pl".
std::string code_of_row(const char* row_name) {
    return std::string(row_name).substr(std::string("lang_item_").size());
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Each language chooser row saves its own code",
                 "[wizard][i18n][ui][ui_integration]") {
    lv_obj_t* wizard = ui_wizard_create(test_screen());
    REQUIRE(wizard != nullptr);
    force_language_chooser_step(true);
    ui_wizard_navigate_to_step(helix::wizard::StepId::Language);
    get_wizard_language_chooser_step()->stop_cycle_timer();

    const char* rows[] = {"lang_item_en", "lang_item_de", "lang_item_fr", "lang_item_es",
                          "lang_item_ru", "lang_item_pt", "lang_item_it", "lang_item_pl",
                          "lang_item_ja", "lang_item_zh"};
    for (const char* row : rows) {
        CAPTURE(row);
        lv_obj_t* btn = lv_obj_find_by_name(wizard, row);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        CHECK(helix::SystemSettingsManager::instance().get_language() == code_of_row(row));
    }

    get_wizard_language_chooser_step()->cleanup();
}
