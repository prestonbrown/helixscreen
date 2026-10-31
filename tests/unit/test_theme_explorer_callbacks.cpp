// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_explorer_callbacks.cpp
 * @brief The theme explorer's preset dropdown keeps its own event callback
 *
 * XML event callbacks are one global name -> function table, and a dropdown
 * binds whatever the name resolves to when it is created. The explorer is
 * rebuilt every time it opens, so any later registration of its callback name
 * silently rewires the next explorer.
 */

#include "ui_nav_manager.h"
#include "ui_settings_appearance.h"
#include "ui_theme_editor_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "theme_loader.h"
#include "theme_manager.h"

#include <array>
#include <string>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ThemeExplorer: opening the editor does not rewire the preset dropdown",
                 "[theme][ui]") {
    helix::settings::get_appearance_settings_overlay().register_callbacks();
    lv_event_cb_t explorer_cb = lv_xml_get_event_cb(nullptr, "on_theme_preset_changed");
    REQUIRE(explorer_cb != nullptr);

    // The editor registers its callbacks the first time Edit is pressed.
    get_theme_editor_overlay().register_callbacks();

    // A different handler here previews nothing and never enables Apply, so a
    // reopened explorer can no longer change theme.
    REQUIRE(lv_xml_get_event_cb(nullptr, "on_theme_preset_changed") == explorer_cb);
}

namespace {

int theme_apply_generation() {
    return lv_subject_get_int(theme_manager_get_changed_subject());
}

int theme_index(const std::string& filename) {
    auto themes = helix::discover_themes(helix::get_themes_directory());
    for (size_t i = 0; i < themes.size(); ++i) {
        if (themes[i].filename == filename) {
            return static_cast<int>(i);
        }
    }
    FAIL("theme '" << filename << "' not found");
    return -1;
}

/// Opens Appearance, then the explorer from its Theme row, and closes both after.
class ThemeExplorerFixture : public LVGLUITestFixture {
  protected:
    ThemeExplorerFixture() {
        for (auto& p : panels_)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels_.data());

        REQUIRE(overlay().show(lv_screen_active()));
        settle();
        overlay().handle_theme_settings_clicked();
        settle();
        dropdown_ = lv_obj_find_by_name(lv_screen_active(), "theme_preset_dropdown");
        REQUIRE(dropdown_ != nullptr);
    }

    ~ThemeExplorerFixture() override {
        for (int i = 0; i < 4 && NavigationManager::instance().has_open_overlays(); ++i) {
            NavigationManager::instance().go_back();
            settle();
        }
    }

    static helix::settings::AppearanceSettingsOverlay& overlay() {
        return helix::settings::get_appearance_settings_overlay();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    /// The dropdown's value_changed event, as a tap on a row delivers it.
    void pick(const std::string& filename) {
        int index = theme_index(filename);
        lv_dropdown_set_selected(dropdown_, static_cast<uint32_t>(index));
        lv_obj_send_event(dropdown_, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    /// The explorer's root: the overlay the preset dropdown lives in.
    lv_obj_t* explorer() const {
        lv_obj_t* obj = dropdown_;
        while (obj && !NavigationManager::instance().is_panel_in_stack(obj)) {
            obj = lv_obj_get_parent(obj);
        }
        return obj;
    }

    /// Back out of the explorer the way its header's back button does.
    void close_explorer() {
        REQUIRE(explorer() != nullptr);
        NavigationManager::instance().go_back();
        settle();
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
    lv_obj_t* dropdown_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(ThemeExplorerFixture, "ThemeExplorer: a tree deleted with its screen is recreated",
                 "[theme][ui][widget_handles]") {
    lv_obj_delete(explorer());
    settle();
    overlay().handle_theme_settings_clicked();
    settle();
    dropdown_ = lv_obj_find_by_name(lv_screen_active(), "theme_preset_dropdown");
    REQUIRE(dropdown_ != nullptr);
    CHECK(explorer() != nullptr);
}

TEST_CASE_METHOD(ThemeExplorerFixture, "ThemeExplorer: one pick applies the theme once",
                 "[theme][ui]") {
    SECTION("a theme with both modes") {
        int before = theme_apply_generation();
        pick("nord");
        REQUIRE(theme_apply_generation() - before == 1);
        REQUIRE(theme_manager_get_active_theme().filename == "nord");
    }

    SECTION("a dark-only theme picked while previewing light lands in dark mode") {
        pick("nord");
        overlay().handle_preview_dark_mode_toggled(false);
        REQUIRE_FALSE(theme_manager_is_dark_mode());

        int before = theme_apply_generation();
        pick("dracula");
        REQUIRE(theme_apply_generation() - before == 1);
        REQUIRE(theme_manager_get_active_theme().filename == "dracula");
        REQUIRE(theme_manager_is_dark_mode());
    }

    SECTION("Apply commits the previewed theme with no further apply, counting the close") {
        pick("nord");
        lv_obj_t* root = explorer();
        REQUIRE(root != nullptr);

        int before = theme_apply_generation();
        overlay().handle_apply_theme_clicked();
        settle();
        // Pointer comparison only: the explorer is deleted once it closes.
        REQUIRE_FALSE(NavigationManager::instance().is_panel_in_stack(root));
        REQUIRE(theme_apply_generation() - before == 0);
        REQUIRE(theme_manager_get_active_theme().filename == "nord");
    }

    SECTION("closing without a change applies nothing") {
        int before = theme_apply_generation();
        close_explorer();
        REQUIRE(theme_apply_generation() - before == 0);
    }
}

TEST_CASE_METHOD(ThemeExplorerFixture,
                 "ThemeExplorer: cancelling restores the original theme in its original mode",
                 "[theme][ui]") {
    const std::string original = theme_manager_get_active_theme().filename;
    const bool original_dark = theme_manager_is_dark_mode();
    REQUIRE(theme_manager_supports_dark_mode());
    REQUIRE(theme_manager_supports_light_mode());

    pick("nord");
    overlay().handle_preview_dark_mode_toggled(!original_dark);
    REQUIRE(theme_manager_is_dark_mode() != original_dark);

    int before = theme_apply_generation();
    close_explorer();

    CHECK(theme_apply_generation() - before == 1);
    CHECK(theme_manager_get_active_theme().filename == original);
    CHECK(theme_manager_is_dark_mode() == original_dark);
}
