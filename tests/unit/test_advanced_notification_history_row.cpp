// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_advanced_notification_history_row.cpp
 * @brief The Advanced panel's Notification History row is the way into
 *        notification history when the home screen carries no bell widget.
 *
 * The row shows only while no notifications widget holds a cell on any home
 * page; an entry that is enabled but unplaced draws no bell, so it counts as
 * absent.
 */

#include "ui_panel_advanced.h"

#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "core/static_panel_registry.h"
#include "grid_layout.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int kTpc = GridLayout::TRACKS_PER_CELL;

nlohmann::json bell(bool enabled, int col, int row) {
    return {{"id", "notifications"}, {"enabled", enabled}, {"col", col}, {"row", row},
            {"colspan", kTpc},       {"rowspan", kTpc}};
}

bool bell_placed() {
    return PanelWidgetManager::instance().get_widget_config("home").is_placed("notifications");
}

/// A two-page home layout with @p second_page_widgets on the second page.
nlohmann::json home_layout(nlohmann::json second_page_widgets) {
    return {{"main_page_index", 0},
            {"next_page_id", 2},
            {"pages",
             {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
              {{"id", "second"}, {"widgets", std::move(second_page_widgets)}}}}};
}

/// Holds the active printer's home layout for the test and puts it back after.
struct ScopedHomeLayout {
    Config* cfg = Config::get_instance();
    std::string key = cfg->df() + "panel_widgets/home";
    nlohmann::json saved = cfg->get<nlohmann::json>(key, nlohmann::json());
    ~ScopedHomeLayout() {
        set(saved);
    }
    void set(const nlohmann::json& home) {
        cfg->set<nlohmann::json>(key, home);
        PanelWidgetManager::instance().clear_panel_config("home");
        PanelWidgetManager::instance().get_widget_config("home").mark_dirty();
    }
};

struct NotificationRowFixture : LVGLUITestFixture {
    ScopedHomeLayout home;
    lv_obj_t* root = nullptr;

    NotificationRowFixture() {
        init_widget_registrations();
        init_global_advanced_panel(state(), nullptr);
        get_global_advanced_panel().init_subjects();
        root = static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "advanced_panel", nullptr));
        REQUIRE(root != nullptr);
    }

    ~NotificationRowFixture() override {
        lv_obj_delete(root);
        StaticPanelRegistry::instance().destroy_all();
    }

    bool row_hidden_after_activate() {
        get_global_advanced_panel().on_activate();
        process_lvgl(10);
        lv_obj_t* row = lv_obj_find_by_name(root, "row_notification_history");
        REQUIRE(row != nullptr);
        for (lv_obj_t* obj = row; obj && obj != root; obj = lv_obj_get_parent(obj)) {
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE_METHOD(NotificationRowFixture,
                 "Advanced Notification History row shows only without a placed bell",
                 "[advanced][notifications]") {
    SECTION("a bell on a secondary page hides the row") {
        home.set(home_layout(nlohmann::json::array({bell(true, 0, 0)})));
        REQUIRE(bell_placed());
        CHECK(row_hidden_after_activate());
    }

    SECTION("a removed bell shows the row") {
        home.set(home_layout(nlohmann::json::array({bell(false, 0, 0)})));
        REQUIRE_FALSE(bell_placed());
        CHECK_FALSE(row_hidden_after_activate());
    }

    SECTION("an enabled but unplaced bell shows the row") {
        home.set(home_layout(nlohmann::json::array({bell(true, -1, -1)})));
        CHECK_FALSE(row_hidden_after_activate());
    }

    SECTION("re-adding the bell hides the row on the next activation") {
        home.set(home_layout(nlohmann::json::array({bell(false, 0, 0)})));
        REQUIRE_FALSE(row_hidden_after_activate());
        home.set(home_layout(nlohmann::json::array({bell(true, 0, 0)})));
        CHECK(row_hidden_after_activate());
    }
}
