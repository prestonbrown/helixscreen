// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_controls_overlay_ownership.cpp
 * @brief ControlsPanel opens overlays it does not own
 *
 * Bed Mesh frees its tree on every close (destroy_on_close), so any copy of
 * its root held outside the BedMeshPanel object dangles after a close. The
 * panel that opened it must keep no copy and must never delete it.
 */

#include "ui_nav_manager.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_controls.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/controls_panel_test_access.h"
#include "../test_helpers/process_async_timers.h"
#include "core/update_queue.h"
#include "static_panel_registry.h"

#include <array>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::ControlsPanelTestAccess;

namespace {

void count_delete(lv_event_t* e) {
    ++(*static_cast<int*>(lv_event_get_user_data(e)));
}

void settle() {
    for (int i = 0; i < 5; ++i) {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        lv_timer_handler();
    }
    process_async_timers();
}

void seed_nav_panels() {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ControlsPanel destroyed after Bed Mesh opened and closed touches no freed tree",
                 "[controls][bed_mesh][overlay_ownership]") {
    seed_nav_panels();
    auto panel = std::make_unique<ControlsPanel>(state(), nullptr);

    ControlsPanelTestAccess::open_bed_mesh(*panel, lv_screen_active());
    settle();
    lv_obj_t* root = get_global_bed_mesh_panel().get_root();
    REQUIRE(root != nullptr);
    int deletes = 0;
    lv_obj_add_event_cb(root, count_delete, LV_EVENT_DELETE, &deletes);

    NavigationManager::instance().go_back();
    settle();
    REQUIRE(deletes == 1); // destroy_on_close freed it
    CHECK(get_global_bed_mesh_panel().get_root() == nullptr);

    // Under ASAN, a panel that kept a copy of the root would delete freed
    // memory here.
    panel.reset();
    settle();
    CHECK(deletes == 1);

    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "destroying ControlsPanel leaves an open Bed Mesh tree to its owner",
                 "[controls][bed_mesh][overlay_ownership]") {
    seed_nav_panels();
    auto panel = std::make_unique<ControlsPanel>(state(), nullptr);

    ControlsPanelTestAccess::open_bed_mesh(*panel, lv_screen_active());
    settle();
    lv_obj_t* root = get_global_bed_mesh_panel().get_root();
    REQUIRE(root != nullptr);
    int deletes = 0;
    lv_obj_add_event_cb(root, count_delete, LV_EVENT_DELETE, &deletes);

    panel.reset();
    settle();
    CHECK(deletes == 0);
    CHECK(get_global_bed_mesh_panel().get_root() == root);

    // The owner still frees it on close.
    NavigationManager::instance().go_back();
    settle();
    CHECK(deletes == 1);
    CHECK(get_global_bed_mesh_panel().get_root() == nullptr);

    StaticPanelRegistry::instance().destroy_all();
}
