// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_switch_orphan_overlays.cpp
 * @brief A printer switch frees orphaned overlay widgets and the caches pointing at them
 *
 * Run with: ./build/bin/helix-tests "[switch-orphans]"
 *
 * tear_down_printer_state() destroys the panel objects registered with
 * StaticPanelRegistry, and nothing else on that path frees their overlay
 * widgets: NavigationManager::shutdown() clears tracking without freeing
 * widgets, the panel destructors skip deletion inside the destroy_all() window
 * (LV_EVENT_DELETE would fire into the half-destroyed panel set), and step 20
 * only deletes m_app_layout - overlays are parented to the SCREEN. The switch
 * therefore frees the recorded roots itself once the window closes; without
 * that, every overlay opened before a switch stays allocated as a hidden screen
 * child for the rest of the session (~400-800KB each, once per switch).
 *
 * A surviving caller cache is more dangerous than the leak: a static like
 * MotionWidget::motion_panel_ holds the orphan's address after teardown, and
 * lazy_create_and_push_overlay's stale-cache guard calls
 * safe_delete_deferred(cached) when the fresh panel has no root - a freed
 * address reused by a live widget makes that delete hit the LIVE widget. The
 * switch must drop those caches too. PrinterCacheRegistry::invalidate_all()
 * already fires from every active-printer change BEFORE teardown, so the three
 * static caches register invalidators there.
 *
 * These tests run the switch's overlay-relevant sequence end to end:
 * invalidate_all() (switch_printer's order), NavigationManager::shutdown()
 * (teardown step 2), then helix::ui::destroy_static_panels() (teardown step 14:
 * destroy the panels, free the roots the destructors hand back), then reopen
 * through the real caller path.
 */

#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_panel_console.h"
#include "ui_panel_macros.h"
#include "ui_panel_motion.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_cache_registry.h"
#include "static_panel_registry.h"
#include "test_helpers/panel_widget_overlay_cache_test_access.h"
#include "ui/ui_lazy_panel_helper.h"

#include <array>

#include "../catch_amalgamated.hpp"

namespace {

void count_delete(lv_event_t* e) {
    ++(*static_cast<int*>(lv_event_get_user_data(e)));
}

/// Seed NavigationManager the way the app does: panel_stack_[0] holds the
/// active root panel, which push_overlay() reads beneath each overlay.
void seed_nav_panels() {
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a printer switch frees the orphaned motion overlay and drops the widget cache",
                 "[overlays][teardown][switch][switch-orphans][1707]") {
    seed_nav_panels();

    // Constructing the widget registers its cache invalidator with
    // PrinterCacheRegistry; opening through the real cache storage is what
    // MotionWidget::handle_click does.
    MotionWidget widget;
    lv_obj_t*& cached = MotionWidgetTestAccess::motion_panel();
    cached = nullptr; // may hold a freed address from an earlier test

    REQUIRE(helix::ui::lazy_create_and_push_overlay<MotionPanel>(
        get_global_motion_panel, cached, lv_screen_active(), "Motion", "test"));
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* orphan = cached;
    REQUIRE(orphan != nullptr);
    REQUIRE(lv_obj_find_by_name(orphan, "jog_pad") != nullptr);

    int deletes = 0;
    lv_obj_add_event_cb(orphan, count_delete, LV_EVENT_DELETE, &deletes);

    // switch_printer fires every invalidator BEFORE teardown, while the widget
    // is still alive. A cache that survives the widget's destruction is a
    // freed pointer the stale-cache guard would later deref.
    helix::PrinterCacheRegistry::instance().invalidate_all();
    CHECK(cached == nullptr);

    // The overlay-relevant slice of tear_down_printer_state(), in order.
    NavigationManager::instance().shutdown();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100); // the deferred delete lands on the timer tick

    CHECK(deletes == 1);

    // Reopen through the same path a user takes after the switch: a fresh
    // widget bound to the live panel, not a push of the freed orphan.
    seed_nav_panels();
    REQUIRE(helix::ui::lazy_create_and_push_overlay<MotionPanel>(
        get_global_motion_panel, cached, lv_screen_active(), "Motion", "test"));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(cached != nullptr);
    CHECK(cached != orphan);
    CHECK(get_global_motion_panel().get_root() == cached);
    REQUIRE(lv_obj_find_by_name(cached, "jog_pad") != nullptr);

    // Leave the process clean for the next test: drop the caches, free the
    // reopened panel and its widget.
    helix::PrinterCacheRegistry::instance().invalidate_all();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "a printer switch frees every orphaned lazy overlay and drops the console and macros caches",
    "[overlays][teardown][switch][switch-orphans][1707]") {
    seed_nav_panels();

    GCodeConsoleWidget console_widget;
    MacrosWidget macros_widget;
    lv_obj_t*& console_cached = GCodeConsoleWidgetTestAccess::console_panel();
    lv_obj_t*& macros_cached = MacrosWidgetTestAccess::macros_panel();
    console_cached = nullptr;
    macros_cached = nullptr;

    // Macros opens destroy-on-close, console keeps its tree across closes: the
    // switch must free the orphan in both shapes.
    REQUIRE(helix::ui::lazy_create_and_push_overlay<ConsolePanel>(
        get_global_console_panel, console_cached, lv_screen_active(), "Console", "test"));
    REQUIRE(helix::ui::lazy_create_and_push_overlay<MacrosPanel>(
        get_global_macros_panel, macros_cached, lv_screen_active(), "Macros", "test", true));
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* console_orphan = console_cached;
    lv_obj_t* macros_orphan = macros_cached;
    REQUIRE(console_orphan != nullptr);
    REQUIRE(macros_orphan != nullptr);

    int console_deletes = 0;
    int macros_deletes = 0;
    lv_obj_add_event_cb(console_orphan, count_delete, LV_EVENT_DELETE, &console_deletes);
    lv_obj_add_event_cb(macros_orphan, count_delete, LV_EVENT_DELETE, &macros_deletes);

    helix::PrinterCacheRegistry::instance().invalidate_all();
    CHECK(console_cached == nullptr);
    CHECK(macros_cached == nullptr);

    NavigationManager::instance().shutdown();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100);

    CHECK(console_deletes == 1);
    CHECK(macros_deletes == 1);

    // Leave the process clean for the next test.
    helix::PrinterCacheRegistry::instance().invalidate_all();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a printer switch frees the numeric keypad widget built on first show",
                 "[overlays][teardown][switch][switch-orphans][keypad]") {
    seed_nav_panels();

    ui_keypad_init(lv_screen_active());
    ui_keypad_config_t config = {};
    config.initial_value = 100;
    config.min_value = 0;
    config.max_value = 300;
    config.title_label = "Nozzle";
    config.unit_label = "C";
    config.allow_decimal = false;
    ui_keypad_show(&config);
    helix::ui::UpdateQueue::instance().drain();

    // The keypad is a bare screen child, not an OverlayBase panel: locate its
    // tree through the XML view name the component gives its root.
    lv_obj_t* keypad = lv_obj_find_by_name(lv_screen_active(), "keypad_panel");
    REQUIRE(keypad != nullptr);
    REQUIRE(lv_obj_find_by_name(keypad, "btn_backspace") != nullptr);
    REQUIRE(ui_keypad_is_visible());

    int deletes = 0;
    lv_obj_add_event_cb(keypad, count_delete, LV_EVENT_DELETE, &deletes);

    // The overlay-relevant slice of tear_down_printer_state(), in order. Step 14
    // (destroy_static_panels) is the only place that could free the keypad tree:
    // hide merely pops the nav stack, and step 20 deletes m_app_layout, not the
    // screen the keypad hangs from.
    NavigationManager::instance().shutdown();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100); // the deferred delete lands on the timer tick

    CHECK(deletes == 1);
    CHECK_FALSE(ui_keypad_is_visible()); // the static points nowhere now

    // Reopen the way the next printer session does: init registers the parent,
    // and the first show builds a fresh tree on the same screen.
    seed_nav_panels();
    ui_keypad_init(lv_screen_active());
    ui_keypad_show(&config);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* fresh = lv_obj_find_by_name(lv_screen_active(), "keypad_panel");
    REQUIRE(fresh != nullptr);
    REQUIRE(fresh != keypad);
    REQUIRE(ui_keypad_is_visible());

    // Leave the process clean for the next test: free the reopened tree too.
    NavigationManager::instance().shutdown();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100);
}
