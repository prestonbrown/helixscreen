// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_edit_mode_lock_guard.cpp
 * @brief #1245 — a long press must not open home-grid edit mode behind the lock screen.
 *
 * On Android the wake touch reaches the home panel (the sleep overlay is not
 * clickable and the sleep-aware input wrapper is compiled out under
 * HELIX_DISPLAY_SDL). wake_display() shows the lock screen, but nothing on that
 * path stopped a LONG_PRESSED already in flight from reaching
 * HomePanel::on_home_grid_long_press — so edit mode activated underneath the
 * PIN pad and the user found it once the PIN cleared.
 *
 * DisplayManager::disable_input_briefly() is the root-cause fix
 * (tests/unit/application/test_display_wake_input_gate.cpp). This is the second
 * layer: while the screen is locked, edit mode is never a legitimate outcome of
 * a hold, whatever produced the event.
 *
 * The handler only does anything once the panel owns a page container, so the
 * container list is seeded through HomePanelTestAccess rather than standing up
 * the whole carousel; everything below that — should_suppress_edit_mode(), the
 * drift check, GridEditMode::enter() — is the production path.
 *
 * The kill switch that sits ahead of the lock check in should_suppress_edit_mode()
 * is covered separately in test_home_edit_mode_kill_switch.cpp; ScopedInputSettings
 * pins it enabled here so this file is testing the lock and nothing else.
 */

#include "ui_panel_home.h"

#include "home_edit_mode_test_helpers.h"
#include "lock_manager.h"
#include "lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

using helix_test::make_long_press_container;
using helix_test::ScopedHomePanelPage;
using helix_test::ScopedInputSettings;
using helix_test::ScopedLockState;

TEST_CASE_METHOD(LVGLTestFixture, "home-grid long press is ignored while the screen is locked",
                 "[home][grid_edit][edit_mode][lock][1245]") {
    // Default-flavoured: asserts the persisted key is absent AND that the
    // documented default (enabled) is what the manager actually loaded, so this
    // test can never quietly run against a co-tenant's disabled kill switch and
    // "pass" for the wrong reason.
    ScopedInputSettings input_settings;
    ScopedLockState lock_state;
    auto& lock = helix::LockManager::instance();

    HomePanel& panel = get_global_home_panel();

    lv_obj_t* container = make_long_press_container(test_screen());
    ScopedHomePanelPage page(panel, container);

    REQUIRE_FALSE(HomePanelTestAccess::edit_mode_active(panel));

    SECTION("unlocked: the hold enters edit mode (control case)") {
        REQUIRE_FALSE(lock.is_locked());

        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);

        CHECK(HomePanelTestAccess::edit_mode_active(panel));
    }

    SECTION("locked: the identical hold is suppressed") {
        REQUIRE(lock.set_pin("1234"));
        lock.lock();
        REQUIRE(lock.is_locked());

        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);

        CHECK_FALSE(HomePanelTestAccess::edit_mode_active(panel));

        // And once the PIN clears, the same hold works again — the guard keys
        // off lock state, not some sticky one-way latch.
        REQUIRE(lock.try_unlock("1234"));
        lv_obj_send_event(container, LV_EVENT_LONG_PRESSED, nullptr);
        CHECK(HomePanelTestAccess::edit_mode_active(panel));
    }
}
