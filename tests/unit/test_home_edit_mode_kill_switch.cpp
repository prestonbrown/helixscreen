// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_edit_mode_kill_switch.cpp
 * @brief #1245 — Touch & Input's "home edit mode" toggle must actually stop the long press.
 *
 * The filer's complaint was that a resting finger on a tablet crosses the
 * long-press threshold and drops the home grid into edit mode by accident.
 * Raising the threshold (settings_long_press_time) helps; the toggle is the
 * hard kill switch for users who never want edit mode at all.
 *
 * It is enforced by the first branch of should_suppress_edit_mode()
 * (src/ui/ui_panel_home.cpp), a static function with no other caller — so the
 * only seam that exercises it is the LV_EVENT_LONG_PRESSED handler the XML wires
 * onto the carousel host, which is what these cases drive. Everything below the
 * event send is the production path: should_suppress_edit_mode(), the drift
 * check, GridEditMode::enter().
 *
 * Deleting the kill-switch branch turns the "disabled" cases green-to-red here.
 * The lock-screen branch that follows it is covered in
 * test_home_edit_mode_lock_guard.cpp.
 */

#include "ui_panel_home.h"

#include "home_edit_mode_test_helpers.h"
#include "input_settings_manager.h"
#include "lock_manager.h"
#include "lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

using helix_test::make_long_press_container;
using helix_test::ScopedHomePanelPage;
using helix_test::ScopedInputSettings;
using helix_test::ScopedLockState;

TEST_CASE_METHOD(LVGLTestFixture, "home edit mode toggle gates the long-press entry",
                 "[home][grid_edit][edit_mode][input_settings][1245]") {
    // Unlocked throughout: the lock branch of should_suppress_edit_mode() sits
    // right after the kill switch, and a leaked PIN would suppress every hold
    // and make the "enabled" control case pass for the wrong reason.
    ScopedLockState lock_state;
    REQUIRE_FALSE(helix::LockManager::instance().is_locked());

    HomePanel& panel = get_global_home_panel();
    lv_obj_t* container = make_long_press_container(test_screen());
    ScopedHomePanelPage page(panel, container);

    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel));

    SECTION("persisted OFF: the hold never enters edit mode") {
        ScopedInputSettings input_settings(false);

        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);
        CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel));

        // Not a one-shot debounce — a second hold is refused just the same.
        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);
        CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel));
    }

    SECTION("persisted ON: the identical hold enters edit mode (control case)") {
        ScopedInputSettings input_settings(true);

        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);
        CHECK(HomePanelTestAccess::edit_mode_active(panel));
    }

    SECTION("documented default is ON, so an untouched install still gets edit mode") {
        // The constructor asserts the key is absent and that the loaded value is
        // true; this then proves the default reaches the long-press handler.
        ScopedInputSettings input_settings;

        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);
        CHECK(HomePanelTestAccess::edit_mode_active(panel));
    }

    SECTION("the toggle applies live, in both directions, with no restart") {
        ScopedInputSettings input_settings(false);
        auto& input = helix::InputSettingsManager::instance();

        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);
        REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel));

        // Off -> on takes effect on the very next hold.
        input.set_home_edit_mode_enabled(true);
        CHECK_FALSE(input.is_restart_pending());
        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);
        CHECK(HomePanelTestAccess::edit_mode_active(panel));

        // On -> off does too. Leave edit mode first: an already-active session
        // routes to handle_long_press() and would never re-test the entry gate.
        panel.exit_grid_edit_mode();
        REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel));

        input.set_home_edit_mode_enabled(false);
        CHECK_FALSE(input.is_restart_pending());
        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);
        CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel));
    }
}
