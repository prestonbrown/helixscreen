// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_close_callback_timing.cpp
 * @brief An overlay's close callback never runs before the next LVGL tick
 *
 * Close callbacks delete widgets, and the paths that fire them run inside
 * UpdateQueue drains or LVGL animation callbacks, where a synchronous delete
 * corrupts LVGL's event list (prestonbrown/helixscreen#637). Every path that
 * retires an overlay has to hand the callback to the next tick.
 */

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../test_helpers/widget_in_tree.h"
#include "display_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class CloseTimingFixture : public LVGLUITestFixture {
  public:
    CloseTimingFixture() {
        home_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_;
        panels[static_cast<int>(PanelId::Controls)] = lv_obj_create(test_screen());
        NavigationManager::instance().set_panels(panels);

        lower_ = lv_obj_create(test_screen());
        upper_ = lv_obj_create(test_screen());
        nav().register_overlay_close_callback(lower_, [this] { ++lower_closes_; });
        nav().register_overlay_close_callback(upper_, [this] { ++upper_closes_; });
    }

    ~CloseTimingFixture() override {
        drain();
    }

    static NavigationManager& nav() {
        return NavigationManager::instance();
    }

    static void drain() {
        ui::UpdateQueueTestAccess::drain_all(ui::UpdateQueue::instance());
    }

    void stack_both() {
        NavigationManagerTestAccess::set_panel_stack(nav(), {home_, lower_, upper_});
    }

    lv_obj_t* home_ = nullptr;
    lv_obj_t* lower_ = nullptr;
    lv_obj_t* upper_ = nullptr;
    int lower_closes_ = 0;
    int upper_closes_ = 0;
};

} // namespace

TEST_CASE_METHOD(CloseTimingFixture, "Clearing the overlay stack defers close callbacks",
                 "[navigation][overlay][close_timing]") {
    stack_both();

    NavigationManagerTestAccess::clear_overlay_stack(nav());
    CHECK(lower_closes_ == 0);
    CHECK(upper_closes_ == 0);

    process_lvgl(50);
    CHECK(lower_closes_ == 1);
    CHECK(upper_closes_ == 1);
}

TEST_CASE_METHOD(CloseTimingFixture, "A navbar switch defers close callbacks",
                 "[navigation][overlay][close_timing]") {
    stack_both();

    NavigationManagerTestAccess::switch_to_panel(nav(), PanelId::Controls);
    CHECK(lower_closes_ == 0);
    CHECK(upper_closes_ == 0);

    process_lvgl(50);
    CHECK(lower_closes_ == 1);
    CHECK(upper_closes_ == 1);
}

TEST_CASE_METHOD(CloseTimingFixture, "A finished slide-out defers the close callback",
                 "[navigation][overlay][close_timing]") {
    NavigationManagerTestAccess::set_panel_stack(nav(), {home_});

    NavigationManagerTestAccess::slide_out_complete(upper_);
    CHECK(upper_closes_ == 0);

    process_lvgl(50);
    CHECK(upper_closes_ == 1);
}

TEST_CASE_METHOD(CloseTimingFixture, "Closing a buried overlay defers its close callback",
                 "[navigation][overlay][close_timing]") {
    stack_both();

    nav().close_overlay(lower_);
    drain();

    CHECK_FALSE(nav().is_panel_in_stack(lower_));
    CHECK(nav().is_panel_on_top(upper_));
    CHECK(lower_closes_ == 0);

    process_lvgl(50);
    CHECK(lower_closes_ == 1);
    CHECK(upper_closes_ == 0);
}

TEST_CASE_METHOD(CloseTimingFixture, "Closing a buried overlay deletes its backdrop deferred",
                 "[navigation][overlay][close_timing]") {
    stack_both();
    lv_obj_t* backdrop = lv_obj_create(test_screen());
    NavigationManagerTestAccess::set_overlay_backdrop_for(nav(), lower_, backdrop);

    nav().close_overlay(lower_);
    drain();
    CHECK(helix::test::widget_in_tree(backdrop));

    process_lvgl(50);
    CHECK_FALSE(helix::test::widget_in_tree(backdrop));
}

TEST_CASE_METHOD(CloseTimingFixture, "A close with animations off defers the close callback",
                 "[navigation][overlay][close_timing]") {
    auto& settings = DisplaySettingsManager::instance();
    const bool animations_were_enabled = settings.get_animations_enabled();
    settings.set_animations_enabled(false);

    nav().register_overlay_instance(upper_, nullptr);
    nav().push_overlay(upper_);
    drain();
    REQUIRE(nav().is_panel_on_top(upper_));

    nav().go_back();
    drain();
    CHECK_FALSE(nav().is_panel_in_stack(upper_));
    CHECK(upper_closes_ == 0);

    process_lvgl(50);
    CHECK(upper_closes_ == 1);

    nav().unregister_overlay_instance(upper_);
    settings.set_animations_enabled(animations_were_enabled);
}

TEST_CASE_METHOD(CloseTimingFixture,
                 "A close callback is dropped when its owner unregisters before the tick",
                 "[navigation][overlay][close_timing]") {
    auto& settings = DisplaySettingsManager::instance();
    const bool animations_were_enabled = settings.get_animations_enabled();
    settings.set_animations_enabled(false);

    nav().register_overlay_instance(upper_, nullptr);
    nav().push_overlay(upper_);
    drain();
    nav().go_back();
    drain();
    REQUIRE(upper_closes_ == 0);

    // The owner is destroyed in the same drain: its destructor unregisters.
    nav().unregister_overlay_instance(upper_);
    process_lvgl(50);

    CHECK(upper_closes_ == 0);

    settings.set_animations_enabled(animations_were_enabled);
}

TEST_CASE_METHOD(CloseTimingFixture, "A close callback still runs when its owner stays registered",
                 "[navigation][overlay][close_timing]") {
    nav().register_overlay_instance(upper_, nullptr);
    NavigationManagerTestAccess::set_panel_stack(nav(), {home_});

    NavigationManagerTestAccess::slide_out_complete(upper_);
    process_lvgl(50);

    CHECK(upper_closes_ == 1);
    nav().unregister_overlay_instance(upper_);
}

TEST_CASE_METHOD(CloseTimingFixture,
                 "A close callback still runs when a hot-reload rebuild rekeys the root first",
                 "[navigation][overlay][close_timing]") {
    nav().register_overlay_instance(upper_, nullptr);
    NavigationManagerTestAccess::set_panel_stack(nav(), {home_});
    NavigationManagerTestAccess::slide_out_complete(upper_);

    lv_obj_t* rebuilt = lv_obj_create(test_screen());
    nav().rekey_overlay_widget(upper_, rebuilt);
    process_lvgl(50);

    CHECK(upper_closes_ == 1);
    nav().unregister_overlay_instance(rebuilt);
}
