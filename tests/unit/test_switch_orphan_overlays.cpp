// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_switch_orphan_overlays.cpp
 * @brief A printer switch frees orphaned overlay widgets
 *
 * Run with: ./build/bin/helix-tests "[switch-orphans]"
 *
 * tear_down_printer_state() destroys the panel objects registered with
 * StaticPanelRegistry, and nothing else on that path frees their overlay
 * widgets: NavigationManager::shutdown() clears tracking without freeing
 * widgets, the panel destructors skip deletion inside the destroy_all() window
 * (LV_EVENT_DELETE would fire into the half-destroyed panel set), and the tree delete
 * only deletes m_app_layout - overlays are parented to the SCREEN. The switch
 * therefore frees the recorded roots itself once the window closes; without
 * that, every overlay opened before a switch stays allocated as a hidden screen
 * child for the rest of the session (~400-800KB each, once per switch).
 *
 * Callers keep no copy of an overlay's root (the overlay object owns it), so
 * a reopen after the switch reaches the live panel's own tree.
 *
 * These tests run the switch's overlay-relevant sequence end to end:
 * invalidate_all() (switch_printer's order), NavigationManager::shutdown()
 * (the first step of teardown_printer_scope()), then helix::ui::destroy_static_panels()
 * (its panel-destroy step: destroy the panels, free the roots the destructors hand
 * back), then reopen through the real caller path.
 */

#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_panel_console.h"
#include "ui_panel_macros.h"
#include "ui_panel_motion.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "overlay_base.h"
#include "printer_cache_registry.h"
#include "static_panel_registry.h"

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

// Root for the DetachSubtree-shape case: the registry callback is a captureless
// lambda, so it reaches the widget through file scope (how AmsPanel's
// s_ams_panel_obj works).
lv_obj_t* s_detach_shape_root = nullptr;

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "a printer switch frees the orphaned motion overlay",
                 "[overlays][teardown][switch][switch-orphans][1707]") {
    seed_nav_panels();

    // What MotionWidget::handle_click does.
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* orphan = get_global_motion_panel().get_root();
    REQUIRE(orphan != nullptr);
    REQUIRE(lv_obj_find_by_name(orphan, "jog_pad") != nullptr);

    int deletes = 0;
    lv_obj_add_event_cb(orphan, count_delete, LV_EVENT_DELETE, &deletes);

    // switch_printer fires every invalidator BEFORE teardown.
    helix::PrinterCacheRegistry::instance().invalidate_all();

    // The overlay-relevant slice of tear_down_printer_state(), in order.
    NavigationManager::instance().shutdown();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100); // the deferred delete lands on the timer tick

    CHECK(deletes == 1);

    // Reopen through the same path a user takes after the switch: a fresh
    // widget bound to the live panel, not a push of the freed orphan.
    seed_nav_panels();
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* reopened = get_global_motion_panel().get_root();
    REQUIRE(reopened != nullptr);
    // The orphan was freed exactly once and the reopen did not resurrect it. (Pointer
    // inequality would be allocator-dependent: a freed address can be reused.)
    CHECK(deletes == 1);
    REQUIRE(lv_obj_find_by_name(reopened, "jog_pad") != nullptr);

    // Leave the process clean for the next test: free the reopened panel and
    // its widget.
    helix::PrinterCacheRegistry::instance().invalidate_all();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a printer switch frees every orphaned lazy overlay",
                 "[overlays][teardown][switch][switch-orphans][1707]") {
    seed_nav_panels();

    // Both are destroy-on-close; still open (on the stack) when the switch
    // runs, so the switch must free them.
    REQUIRE(get_global_console_panel().show(lv_screen_active()));
    REQUIRE(get_global_macros_panel().show(lv_screen_active()));
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* console_orphan = get_global_console_panel().get_root();
    lv_obj_t* macros_orphan = get_global_macros_panel().get_root();
    REQUIRE(console_orphan != nullptr);
    REQUIRE(macros_orphan != nullptr);

    int console_deletes = 0;
    int macros_deletes = 0;
    lv_obj_add_event_cb(console_orphan, count_delete, LV_EVENT_DELETE, &console_deletes);
    lv_obj_add_event_cb(macros_orphan, count_delete, LV_EVENT_DELETE, &macros_deletes);

    helix::PrinterCacheRegistry::instance().invalidate_all();

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
    // hide merely pops the nav stack, and the tree delete in teardown_printer_scope() deletes
    // m_app_layout, not the screen the keypad hangs from.
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

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a printer switch frees a DetachSubtree-torn-down panel (AMS shape)",
                 "[overlays][teardown][switch][switch-orphans][ams-orphans]") {
    seed_nav_panels();

    // AmsPanel and AmsOverviewPanel are bare screen children whose registry
    // callbacks tear down via teardown_overlay_ui(DetachSubtree): the root is
    // detached into a hidden layout-less condemned container (the #983 grid/
    // flex guarantee), which the switch must then free.
    s_detach_shape_root = lv_obj_create(lv_screen_active());
    lv_obj_t* child = lv_obj_create(s_detach_shape_root);
    (void)child;
    StaticPanelRegistry::instance().register_destroy("DetachShapePanel", []() {
        helix::ui::teardown_overlay_ui(s_detach_shape_root, "DetachShapePanel",
                                       helix::ui::TeardownDelete::DetachSubtree);
    });

    int deletes = 0;
    lv_obj_add_event_cb(s_detach_shape_root, count_delete, LV_EVENT_DELETE, &deletes);

    NavigationManager::instance().shutdown();
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100); // the deferred delete lands on the timer tick

    CHECK(deletes == 1);
    CHECK(s_detach_shape_root == nullptr);
}
