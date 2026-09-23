// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motion_panel_switch_uaf.cpp
 * @brief Reopening Motion after a printer switch must not reuse the old overlay
 *
 * Run with: ./build/bin/helix-tests "[1707]"
 *
 * A printer switch (Application::switch_printer -> tear_down_printer_state)
 * destroys the panel objects registered with StaticPanelRegistry but deletes
 * none of their overlay widgets: NavigationManager::shutdown() clears its
 * tracking without freeing widgets, and the panel destructors skip deletion
 * entirely. An overlay widget therefore survives as a hidden screen child
 * while the panel object that created it is gone. Callers cache that widget
 * (MotionWidget::motion_panel_ is a static), and lazy_create_and_push_overlay
 * trusts a non-null cache, so the next open re-pushed the orphan - whose jog
 * pad still carried the freed MotionPanel as its callback user_data. Tapping
 * home called MotionPanel::home() on freed memory (prestonbrown/helixscreen#1707).
 *
 * These tests reproduce that sequence through the real caller path: open via
 * lazy_create_and_push_overlay, run StaticPanelRegistry::destroy_all() (the
 * switch's step that frees the panel), reopen via the same helper. The second
 * case pins the two-caller shape: a second cache holding the pre-switch widget
 * must adopt the live panel's rebuilt widget, not create a third and orphan it.
 */

#include "ui_nav_manager.h"
#include "ui_panel_motion.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "static_panel_registry.h"
#include "ui/ui_lazy_panel_helper.h"

#include <array>

#include "../catch_amalgamated.hpp"

namespace {

int g_orphan_deletes = 0;

void count_orphan_delete(lv_event_t* /*e*/) {
    ++g_orphan_deletes;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "reopening motion after a printer switch rebuilds the overlay",
                 "[motion][teardown][uaf][1707]") {
    // Seed NavigationManager the way the app does: the active root panel sits
    // at panel_stack_[0], which push_overlay() reads beneath each overlay.
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    // Open Motion through the real caller path (what MotionWidget::handle_click
    // does). The cache here stands in for MotionWidget::motion_panel_, which is
    // a static and so survives the panel object's destruction.
    lv_obj_t* cached = nullptr;
    REQUIRE(helix::ui::lazy_create_and_push_overlay<MotionPanel>(
        get_global_motion_panel, cached, lv_screen_active(), "Motion", "test"));
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* orphan = cached;
    REQUIRE(orphan != nullptr);
    // The setup reached the hazard: the pushed widget carries a jog pad whose
    // callbacks are bound to the panel that created it.
    REQUIRE(lv_obj_find_by_name(orphan, "jog_pad") != nullptr);

    g_orphan_deletes = 0;
    lv_obj_add_event_cb(orphan, count_orphan_delete, LV_EVENT_DELETE, nullptr);

    // The switch's teardown step that matters here: panel objects die while
    // their widgets stay on the screen. No widget is deleted by this call.
    StaticPanelRegistry::instance().destroy_all();
    CHECK(g_orphan_deletes == 0);

    // Reopen through the same path a user takes after the switch.
    REQUIRE(helix::ui::lazy_create_and_push_overlay<MotionPanel>(
        get_global_motion_panel, cached, lv_screen_active(), "Motion", "test"));
    helix::ui::UpdateQueue::instance().drain();

    // The orphan must not be re-pushed: pushing it is what fires its jog pad
    // callbacks on the freed panel.
    REQUIRE(cached != nullptr);
    CHECK(cached != orphan);
    // What is shown now is the live panel's own widget, with a jog pad bound
    // to that live panel.
    CHECK(get_global_motion_panel().get_root() == cached);
    REQUIRE(lv_obj_find_by_name(cached, "jog_pad") != nullptr);

    // The orphan itself is freed, not left as a hidden screen child forever.
    process_lvgl(100);
    CHECK(g_orphan_deletes == 1);

    // Leave the registry clean for the next test: the reopened panel
    // re-registered itself with StaticPanelRegistry.
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "two cached callers converge on one live overlay after a switch",
                 "[motion][teardown][uaf][1707]") {
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    // Caller A opens; caller B's cache holds the same widget, which is what a
    // second caller that opened pre-switch is left with.
    lv_obj_t* cache_a = nullptr;
    REQUIRE(helix::ui::lazy_create_and_push_overlay<MotionPanel>(
        get_global_motion_panel, cache_a, lv_screen_active(), "Motion", "test"));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* cache_b = cache_a;

    StaticPanelRegistry::instance().destroy_all();

    // Caller A reopens: the live panel has no widget, so the pre-switch one is
    // freed and a fresh widget is built for the new panel.
    REQUIRE(helix::ui::lazy_create_and_push_overlay<MotionPanel>(
        get_global_motion_panel, cache_a, lv_screen_active(), "Motion", "test"));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* rebuilt = get_global_motion_panel().get_root();
    REQUIRE(rebuilt != nullptr);
    REQUIRE(cache_a == rebuilt);

    // Caller B reopens still holding the pre-switch widget: it must adopt the
    // live root. Falling into create() here would overwrite the panel's root
    // and leave the rebuilt widget orphaned behind a third one.
    REQUIRE(helix::ui::lazy_create_and_push_overlay<MotionPanel>(
        get_global_motion_panel, cache_b, lv_screen_active(), "Motion", "test"));
    helix::ui::UpdateQueue::instance().drain();

    CHECK(cache_b == rebuilt);
    CHECK(get_global_motion_panel().get_root() == rebuilt);
    REQUIRE(lv_obj_find_by_name(rebuilt, "jog_pad") != nullptr);

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}
