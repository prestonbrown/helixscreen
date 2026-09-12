// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_filament_sensor_select.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "misc/lv_timer_private.h" // timer_cb — assert the cancel neutered it

#include <memory>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Deferred refresh one-shot must not outlive the step (prestonbrown/helixscreen#1577)
// ============================================================================
// cleanup() cancels the one-shot, so the navigation path is covered. A teardown
// that destroys the step WITHOUT calling cleanup() first must not leave it
// armed on a freed `this`: StaticPanelRegistry::destroy_all() runs BEFORE
// lv_deinit() (application.cpp), so at destructor time the timer really is
// still in LVGL's list. Same shape as the connection step's auto-probe timer.

TEST_CASE_METHOD(LVGLTestFixture,
                 "Filament sensor step: destructor cancels the deferred refresh timer",
                 "[wizard][filament][timer][regression][1577]") {
    auto step = std::make_unique<WizardFilamentSensorSelectStep>();
    step->schedule_deferred_refresh();

    lv_timer_t* timer = step->refresh_timer_for_test();
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);

    // Destroy without cleanup() — the teardown path that must cancel too.
    step.reset();

    // Neutered, not deleted: lv_timer_cancel_safe() nulls the callback and lets
    // lv_timer_handler reap the timer on its next pass. Reading it here is safe
    // because the timer is LVGL-owned memory, not the step's.
    REQUIRE(timer->timer_cb == nullptr);

    // A still-armed one-shot would dispatch into the freed step here.
    process_lvgl(2000);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Filament sensor step: cleanup cancels the deferred refresh timer",
                 "[wizard][filament][timer][1577]") {
    auto step = std::make_unique<WizardFilamentSensorSelectStep>();
    step->schedule_deferred_refresh();

    lv_timer_t* timer = step->refresh_timer_for_test();
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);

    // Navigating away mid-window: the pending refresh must not fire into the
    // screen teardown that follows.
    step->cleanup();

    REQUIRE(timer->timer_cb == nullptr);
    REQUIRE(step->refresh_timer_for_test() == nullptr);

    process_lvgl(2000);
}
