// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_set_active_deferred_build.cpp
 * @brief set_active() activates a panel that is built on first navigation
 *
 * The ESP32 firmware builds every panel but Home on first navigation. A panel
 * reached through set_active() (the home card's Print Files / Recent Prints
 * buttons, history, startup redirects) must come up activated like one reached
 * from the navbar: an unactivated PrintSelectPanel never asks for its file list.
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class CountingPanel : public PanelBase {
  public:
    CountingPanel() : PanelBase(get_printer_state(), nullptr) {}

    void init_subjects() override {}
    const char* get_name() const override {
        return "CountingPanel";
    }
    const char* get_xml_component_name() const override {
        return "counting_panel";
    }
    void on_activate() override {
        ++activations;
    }

    int activations = 0;
};

class DeferredBuildFixture : public LVGLUITestFixture {
  public:
    DeferredBuildFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        auto& nav = NavigationManager::instance();
        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        nav.set_panels(panels);
        nav.register_panel_instance(PanelId::Home, &home_panel_);
        nav.set_active(PanelId::Home);
        drain();

        nav.set_deferred_panel_builder([this](int idx) {
            ++builds_;
            auto& n = NavigationManager::instance();
            const auto id = static_cast<PanelId>(idx);
            n.replace_panel_widget(id, lv_obj_create(test_screen()));
            n.register_panel_instance(id, &deferred_panel_);
        });
    }

    ~DeferredBuildFixture() override {
        auto& nav = NavigationManager::instance();
        nav.set_active(PanelId::Home);
        drain();
        nav.set_deferred_panel_builder({});
        nav.register_panel_instance(PanelId::PrintSelect, nullptr);
        nav.register_panel_instance(PanelId::Home, nullptr);
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        nav.set_panels(panels);
        drain();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    lv_obj_t* home_widget_ = nullptr;
    CountingPanel home_panel_;
    CountingPanel deferred_panel_;
    int builds_ = 0;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(DeferredBuildFixture,
                 "set_active builds a deferred panel and activates it on the first visit",
                 "[nav][deferred_build]") {
    auto& nav = NavigationManager::instance();

    nav.set_active(PanelId::PrintSelect);

    // Synchronously: a caller configures the panel right after set_active()
    // (Recent Prints sets the sort order), so it must already be built and live.
    REQUIRE(builds_ == 1);
    REQUIRE(deferred_panel_.activations == 1);
    REQUIRE(nav.get_panel_widget(PanelId::PrintSelect) != nullptr);

    drain();
    REQUIRE(builds_ == 1);
    REQUIRE(deferred_panel_.activations == 1);
    REQUIRE_FALSE(lv_obj_has_flag(nav.get_panel_widget(PanelId::PrintSelect), LV_OBJ_FLAG_HIDDEN));
}
