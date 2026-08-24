// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_panel_widget_uaf.cpp
 * @brief NavigationManager must not write through a deleted panel widget
 *
 * panel_widgets_[UI_PANEL_COUNT] is a raw lv_obj_t* array whose entries nothing
 * used to clear when the widget died. handle_active_panel_change() sweeps that
 * array and calls lv_obj_add_flag()/lv_obj_remove_flag() on every non-null
 * entry, guarded only by the null check — so a deleted panel is a dangling
 * pointer the sweep still writes to.
 *
 * It is reachable asynchronously: init() wires handle_active_panel_change to the
 * active_panel subject through observe_int_sync, which defers the apply onto the
 * UpdateQueue. A panel change queued before a teardown lands after it, against
 * panels that no longer exist. That is the shape of the real crash — a queued
 * apply left over from one test firing during the next test's fixture drain
 * (EXC_BAD_ACCESS / KERN_INVALID_ADDRESS at 0xf9 inside lv_obj_add_flag).
 *
 * The fix is the mechanism already in the file: ensure_delete_hook() on every
 * path that stores a panel pointer, and scrub_deleted_widget() nulling the
 * matching slots. These tests drive both orderings — synchronous sweep after a
 * delete, and delete between enqueue and drain.
 */

#include "ui_update_queue.h"

#include "../../include/ui_nav_manager.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "lvgl/lvgl.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Real NavigationManager over real (if plain) panel widgets.
///
/// The panels are bare lv_obj_create() containers rather than XML panel trees:
/// the defect is entirely in how nav tracks the pointer, and a plain object
/// exercises lv_obj_add_flag()/lv_obj_remove_flag() identically while keeping
/// the test free of XML component registration.
class NavPanelWidgetUafFixture : public LVGLTestFixture {
  public:
    NavPanelWidgetUafFixture() {
        NavigationManager::instance().init();

        for (int i = 0; i < UI_PANEL_COUNT; i++) {
            panels_[i] = lv_obj_create(test_screen());
            REQUIRE(panels_[i] != nullptr);
        }
        NavigationManager::instance().set_panels(panels_);

        // Every slot must actually hold a widget, or a "no crash" result would
        // be the null check passing rather than the fix working.
        for (int i = 0; i < UI_PANEL_COUNT; i++) {
            REQUIRE(NavigationManager::instance().get_panel_widget(static_cast<PanelId>(i)) ==
                    panels_[i]);
        }
    }

    ~NavPanelWidgetUafFixture() override {
        // Drain while the subjects the deferred applies read are still alive;
        // the base destructor's drain runs after teardown, too late to be the
        // only one.
        helix::ui::UpdateQueue::instance().drain();
        NavigationManager::instance().deinit_subjects();
    }

    lv_obj_t* panel(PanelId id) {
        return panels_[static_cast<int>(id)];
    }

    /// Delete one panel the way a teardown would: nav gets no call, only LVGL's
    /// own LV_EVENT_DELETE.
    void delete_panel(PanelId id) {
        int idx = static_cast<int>(id);
        REQUIRE(panels_[idx] != nullptr);
        lv_obj_delete(panels_[idx]);
        panels_[idx] = nullptr;
    }

  private:
    lv_obj_t* panels_[UI_PANEL_COUNT] = {nullptr};
};

} // namespace

TEST_CASE_METHOD(NavPanelWidgetUafFixture,
                 "NavigationManager clears the panel slot when the panel widget is deleted",
                 "[nav][navigation][teardown][uaf]") {
    lv_obj_t* doomed = panel(PanelId::Settings);
    REQUIRE(doomed != nullptr);

    delete_panel(PanelId::Settings);

    CHECK(NavigationManager::instance().get_panel_widget(PanelId::Settings) == nullptr);

    // panel_stack_ stores the same pointers. set_panels() seeded it with the
    // active panel, so this asserts the stack never holds the dead widget either.
    const auto& stack = NavigationManagerTestAccess::panel_stack(NavigationManager::instance());
    CHECK(std::find(stack.begin(), stack.end(), doomed) == stack.end());

    // The whole bug in one line: the sweep writes LV_OBJ_FLAG_HIDDEN through
    // every non-null slot. Pre-fix the Settings slot still pointed at freed
    // memory and lv_obj_add_flag() faulted on it.
    NavigationManagerTestAccess::handle_active_panel_change(NavigationManager::instance(),
                                                            PanelId::Home);

    // Surviving panels are untouched by the scrub and still get their flags.
    CHECK(lv_obj_has_flag(panel(PanelId::Home), LV_OBJ_FLAG_HIDDEN) == false);
    CHECK(lv_obj_has_flag(panel(PanelId::Controls), LV_OBJ_FLAG_HIDDEN) == true);
}

TEST_CASE_METHOD(NavPanelWidgetUafFixture,
                 "NavigationManager panel change queued before a teardown survives the drain",
                 "[nav][navigation][teardown][uaf]") {
    // set_active() publishes to active_panel_subject_; observe_int_sync defers
    // handle_active_panel_change onto the UpdateQueue rather than running it now.
    NavigationManager::instance().set_active(PanelId::Settings);

    // The panels die while that apply is still sitting in the queue — the exact
    // ordering in the crash report (a queued apply from one test firing during
    // the next fixture's drain).
    delete_panel(PanelId::Settings);
    delete_panel(PanelId::Controls);

    CHECK(NavigationManager::instance().get_panel_widget(PanelId::Settings) == nullptr);
    CHECK(NavigationManager::instance().get_panel_widget(PanelId::Controls) == nullptr);

    // Pre-fix this drain ran the sweep over two freed widgets: SIGSEGV inside
    // lv_obj_add_flag().
    helix::ui::UpdateQueue::instance().drain();

    CHECK(lv_obj_has_flag(panel(PanelId::Home), LV_OBJ_FLAG_HIDDEN) == true);
}

TEST_CASE_METHOD(NavPanelWidgetUafFixture,
                 "NavigationManager clears a swapped-in panel widget when it dies",
                 "[nav][navigation][teardown][uaf]") {
    lv_obj_t* replacement = lv_obj_create(test_screen());
    REQUIRE(replacement != nullptr);

    NavigationManager::instance().replace_panel_widget(PanelId::Filament, replacement);
    REQUIRE(NavigationManager::instance().get_panel_widget(PanelId::Filament) == replacement);

    // The widget the swap displaced is still alive and still hooked. Its delete
    // must not blank the successor's slot — the hook scrubs by identity, so only
    // slots that still point at the dying widget are cleared.
    delete_panel(PanelId::Filament);
    CHECK(NavigationManager::instance().get_panel_widget(PanelId::Filament) == replacement);

    NavigationManagerTestAccess::handle_active_panel_change(NavigationManager::instance(),
                                                            PanelId::Filament);
    CHECK(lv_obj_has_flag(replacement, LV_OBJ_FLAG_HIDDEN) == false);

    // A widget registered only through replace_panel_widget() needs the same
    // protection as one registered through set_panels().
    lv_obj_delete(replacement);
    CHECK(NavigationManager::instance().get_panel_widget(PanelId::Filament) == nullptr);

    NavigationManagerTestAccess::handle_active_panel_change(NavigationManager::instance(),
                                                            PanelId::Home);
    CHECK(lv_obj_has_flag(panel(PanelId::Home), LV_OBJ_FLAG_HIDDEN) == false);
}

TEST_CASE_METHOD(NavPanelWidgetUafFixture,
                 "NavigationManager drops the app layout pointer when that widget dies",
                 "[nav][navigation][teardown][uaf]") {
    lv_obj_t* app_layout = lv_obj_create(test_screen());
    REQUIRE(app_layout != nullptr);

    NavigationManager::instance().set_app_layout(app_layout);
    REQUIRE(NavigationManagerTestAccess::app_layout_widget(NavigationManager::instance()) ==
            app_layout);

    lv_obj_delete(app_layout);

    // Stale, this pointer misidentifies whichever screen child LVGL later parks
    // at the same address as the app layout, in refresh_overlay_backdrop()'s and
    // go_back()'s screen sweeps.
    CHECK(NavigationManagerTestAccess::app_layout_widget(NavigationManager::instance()) == nullptr);
}
