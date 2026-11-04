// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_helpers.cpp
 * @brief The shared main-panel test and overlay transform reset
 *
 * Every path that hides an overlay must leave it at the resting transform, so a
 * cached overlay reopens correctly; and every path that asks "is this widget a
 * main panel" must give the same answer.
 */

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class NavHelpersFixture : public LVGLUITestFixture {
  public:
    NavHelpersFixture() {
        home_ = lv_obj_create(test_screen());
        controls_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_;
        panels[static_cast<int>(PanelId::Controls)] = controls_;
        NavigationManager::instance().set_panels(panels);

        overlay_ = lv_obj_create(test_screen());
    }

    ~NavHelpersFixture() override {
        ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Leave the overlay mid-slide, the state a hide path has to clean up.
    void dirty_transform() {
        lv_obj_set_style_translate_x(overlay_, 40, LV_PART_MAIN);
        lv_obj_set_style_translate_y(overlay_, 7, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(overlay_, 300, LV_PART_MAIN);
        lv_obj_set_style_opa(overlay_, LV_OPA_20, LV_PART_MAIN);
    }

    void check_resting_transform() {
        CHECK(lv_obj_get_style_translate_x(overlay_, LV_PART_MAIN) == 0);
        CHECK(lv_obj_get_style_translate_y(overlay_, LV_PART_MAIN) == 0);
        CHECK(lv_obj_get_style_transform_scale_x(overlay_, LV_PART_MAIN) == 256);
        CHECK(lv_obj_get_style_opa(overlay_, LV_PART_MAIN) == LV_OPA_COVER);
    }

    lv_obj_t* home_ = nullptr;
    lv_obj_t* controls_ = nullptr;
    lv_obj_t* overlay_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(NavHelpersFixture, "A navbar switch resets the transform of the overlay it hides",
                 "[navigation][nav_helpers]") {
    dirty_transform();

    NavigationManagerTestAccess::switch_to_panel(NavigationManager::instance(), PanelId::Controls);

    CHECK(lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN));
    check_resting_transform();
}

TEST_CASE_METHOD(NavHelpersFixture,
                 "Clearing the overlay stack resets the transform of every overlay it hides",
                 "[navigation][nav_helpers]") {
    auto& nav = NavigationManager::instance();
    NavigationManagerTestAccess::set_panel_stack(nav, {home_, overlay_});
    dirty_transform();

    NavigationManagerTestAccess::clear_overlay_stack(nav);

    CHECK(lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN));
    check_resting_transform();
}

TEST_CASE_METHOD(NavHelpersFixture, "close_overlay leaves a main panel on the stack",
                 "[navigation][nav_helpers]") {
    auto& nav = NavigationManager::instance();
    NavigationManagerTestAccess::set_panel_stack(nav, {home_, controls_});

    nav.close_overlay(controls_);
    ui::UpdateQueue::instance().drain_until_empty();

    CHECK(nav.is_panel_on_top(controls_));
    CHECK(nav.is_panel_in_stack(home_));
}
