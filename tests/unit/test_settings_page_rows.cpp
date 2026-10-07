// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_page_rows.cpp
 * @brief Every settings row lives on the page the settings tree puts it on.
 *
 * Rows keep their name= when they move between overlays, so the census builds
 * each real overlay and looks each row up by name. A row left on its old page,
 * or dropped on the way to its new one, fails here instead of vanishing
 * silently from the UI.
 */

#include "ui_nav_manager.h"
#include "ui_panel_settings.h"
#include "ui_settings_appearance.h"
#include "ui_settings_hardware.h"
#include "ui_settings_safety.h"
#include "ui_settings_sound.h"
#include "ui_settings_touch.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "settings_manager.h"

#include <array>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct PageRowsFixture : LVGLUITestFixture {
    lv_obj_t* root_ = nullptr;

    PageRowsFixture() {
        SettingsManager::instance().init_subjects();
        get_global_settings_panel().init_subjects();
    }

    ~PageRowsFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        helix::ui::UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    void build(const char* view) {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), view, nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    bool has(const char* row) const {
        return lv_obj_find_by_name(root_, row) != nullptr;
    }
};

struct Placement {
    const char* view;
    std::vector<const char*> rows;
};

} // namespace

TEST_CASE_METHOD(PageRowsFixture, "settings pages: every row lives on its page",
                 "[settings][settings_pages]") {
    const std::vector<Placement> placements = {
        {"settings_display_overlay",
         {"row_display_rotation", "row_ui_scale", "brightness_slider", "row_display_dim",
          "row_display_sleep", "row_screensaver", "btn_test_screensaver",
          "row_sleep_while_printing"}},
        {"settings_appearance_overlay",
         {"row_dark_mode", "row_theme_settings", "row_animations", "row_widget_labels",
          "row_bed_mesh_mode"}},
        {"settings_sound_overlay",
         {"row_sounds", "row_volume", "row_ui_sounds", "row_sound_theme", "row_audio_device",
          "row_preview_sounds", "row_test_tracker"}},
        {"settings_language_time_overlay", {"row_language", "row_timezone", "row_time_format"}},
        {"settings_touch_overlay",
         {"row_system_keyboard", "row_keep_navbar", "row_page_scroll_buttons"}},
        {"settings_connection_overlay", {"row_network", "row_printer_host", "row_printers"}},
        {"settings_updates_overlay",
         {"row_update_channel", "row_update_channel_dev", "row_check_updates", "row_install_update",
          "row_updates_unavailable"}},
    };
    for (const auto& p : placements) {
        build(p.view);
        for (const char* row : p.rows) {
            CAPTURE(p.view, row);
            CHECK(has(row));
        }
    }
}

TEST_CASE_METHOD(PageRowsFixture, "settings pages: moved rows are gone from their old page",
                 "[settings][settings_pages]") {
    build("settings_appearance_overlay");
    CHECK_FALSE(has("row_language"));
    CHECK_FALSE(has("row_system_keyboard"));
    CHECK_FALSE(has("brightness_slider"));
    build("settings_display_overlay");
    CHECK_FALSE(has("row_sounds"));
    CHECK_FALSE(has("row_dark_mode"));
    CHECK_FALSE(has("row_page_scroll_buttons"));
}

TEST_CASE_METHOD(PageRowsFixture, "settings pages: connection and update rows left their old pages",
                 "[settings][settings_pages]") {
    build("settings_system_overlay");
    CHECK_FALSE(has("row_network"));
    CHECK_FALSE(has("row_printer_host"));
    CHECK_FALSE(has("row_touch_input"));
    build("settings_hardware_overlay");
    CHECK_FALSE(has("row_printers"));
    build("about_settings_overlay");
    CHECK_FALSE(has("row_check_updates"));
    CHECK(has("row_version"));
}

TEST_CASE_METHOD(PageRowsFixture, "settings pages: the Display & Sound view no longer exists",
                 "[settings][settings_pages]") {
    CHECK(lv_xml_component_get_scope("settings_display_sound_overlay") == nullptr);
}

TEST_CASE_METHOD(PageRowsFixture, "settings pages: speaker chip path opens Sound",
                 "[settings][settings_pages]") {
    // Slot 0 of the navigation stack is the active main panel; an overlay pushed
    // over nothing would sit there and read as the base.
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(test_screen());
    }
    NavigationManager::instance().set_panels(panels.data());

    // The printer manager's speaker chip opens the page this way.
    auto& sound = helix::settings::get_sound_settings_overlay();
    sound.show(test_screen());
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(5);

    const auto names = NavigationManager::instance().overlay_stack_names();
    REQUIRE_FALSE(names.empty());
    CAPTURE(names.back());
    CHECK(names.back().find("sound") != std::string::npos);

    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(5);
    lv_obj_t* cached = nullptr;
    sound.destroy_overlay_ui(cached);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(5);
}

TEST_CASE_METHOD(PageRowsFixture,
                 "settings pages: printer visuals live in Appearance, filament rows in Printing",
                 "[settings][settings_pages]") {
    build("settings_appearance_overlay");
    CHECK(has("row_toolhead_style"));
    CHECK(has("row_gcode_mode"));
    CHECK(has("row_z_movement_style"));
    build("settings_printing_overlay");
    CHECK_FALSE(has("row_toolhead_style"));
    CHECK(has("row_allow_cold_extrude"));
    CHECK(has("row_filament_auto_cooldown"));
    build("settings_safety_overlay");
    CHECK_FALSE(has("row_allow_cold_extrude"));
}

TEST_CASE_METHOD(PageRowsFixture, "Appearance fills the printer-visual dropdowns on activate",
                 "[settings][settings_pages]") {
    auto& page = helix::settings::get_appearance_settings_overlay();
    page.show(test_screen());
    process_lvgl(5);
    lv_obj_t* row = lv_obj_find_by_name(lv_screen_active(), "row_toolhead_style");
    REQUIRE(row != nullptr);
    lv_obj_t* dd = lv_obj_find_by_name(row, "dropdown");
    REQUIRE(dd != nullptr);
    CHECK(lv_dropdown_get_option_count(dd) > 1); // XML ships a lone "Auto" placeholder
    NavigationManager::instance().go_back();
    process_lvgl(5);
}

TEST_CASE("settings pages: renamed page titles", "[settings][settings_pages]") {
    CHECK(std::string(helix::settings::get_hardware_settings_overlay().get_name()) == "Devices");
    CHECK(std::string(helix::settings::get_safety_settings_overlay().get_name()) ==
          "Safety & Alerts");
}

TEST_CASE_METHOD(PageRowsFixture, "settings pages: touch calibration row shows calibration status",
                 "[settings][settings_pages]") {
    helix::settings::get_touch_settings_overlay().init_subjects();
    lv_subject_t* status = lv_xml_get_subject(nullptr, "touch_cal_status");
    REQUIRE(status != nullptr);

    build("settings_touch_overlay");
    lv_obj_t* row = lv_obj_find_by_name(root_, "row_touch_calibration");
    REQUIRE(row != nullptr);
    lv_obj_t* status_label = lv_obj_find_by_name(row, "status");
    REQUIRE(status_label != nullptr);

    lv_subject_copy_string(status, "Not calibrated");
    process_lvgl(5);
    CHECK(std::string(lv_label_get_text(status_label)) == "Not calibrated");
    lv_subject_copy_string(status, "Calibrated");
    process_lvgl(5);
    CHECK(std::string(lv_label_get_text(status_label)) == "Calibrated");

    lv_obj_delete(root_);
    root_ = nullptr;
    helix::settings::get_touch_settings_overlay().deinit_subjects();
}
