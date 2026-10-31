// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_root_reuse.cpp
 * @brief Overlay singletons own one widget tree, whichever caller opens them
 *
 * Run with: ./build/bin/helix-tests "[overlay-root-reuse]"
 *
 * The fan and LED control overlays, and every panel opened through
 * OverlayBase::show, hold ONE set of widget pointers. Several
 * callers (home widgets, the Controls panel, print status, printer manager)
 * open the same singleton, so a caller opening for the first time must get the
 * tree that already exists: a second create() repoints the singleton at the
 * new tree and leaves the first one pushed with nothing updating it.
 */

#include "ui_fan_control_overlay.h"
#include "ui_nav_manager.h"
#include "ui_panel_motion.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "app_globals.h"
#include "led/ui_led_control_overlay.h"
#include "printer_state.h"

#include <array>

#include "../catch_amalgamated.hpp"

class FanControlOverlayTestAccess {
  public:
    static lv_obj_t* fans_container(const FanControlOverlay& o) {
        return o.fans_container_;
    }
    static size_t dial_count(const FanControlOverlay& o) {
        return o.animated_fan_dials_.size();
    }
};

namespace {

/// Seed NavigationManager the way the app does: panel_stack_[0] holds the
/// active root panel, which push_overlay() reads beneath each overlay.
void seed_nav_panels(lv_obj_t* screen) {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(screen);
    NavigationManager::instance().set_panels(panels.data());
}

void count_delete(lv_event_t* e) {
    ++(*static_cast<int*>(lv_event_get_user_data(e)));
}

bool is_descendant(lv_obj_t* obj, lv_obj_t* root) {
    for (; obj != nullptr; obj = lv_obj_get_parent(obj)) {
        if (obj == root)
            return true;
    }
    return false;
}

/// Run the pushes' deferred work, then drop every overlay NavigationManager
/// tracks, so the tree can be deleted without a push still pending on it.
void close_all() {
    helix::ui::UpdateQueue::instance().drain();
    NavigationManager::instance().shutdown();
}

/// Start and end each case from fresh singletons: a tree left by an earlier
/// test would be reused on the wrong screen.
struct FreshOverlays {
    FreshOverlays() {
        reset();
    }
    ~FreshOverlays() {
        close_all();
        reset();
    }
    static void reset() {
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "two callers opening the fan overlay share one tree",
                 "[overlays][fan_control][overlay-root-reuse]") {
    FreshOverlays fresh;
    seed_nav_panels(test_screen());
    const uint32_t before = lv_obj_get_child_count(test_screen());

    lv_obj_t* first = helix::open_fan_control_overlay(test_screen());
    REQUIRE(first != nullptr);
    lv_obj_t* second = helix::open_fan_control_overlay(test_screen());

    CHECK(second == first);
    CHECK(get_fan_control_overlay().get_root() == first);
    CHECK(lv_obj_get_child_count(test_screen()) == before + 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "two callers opening the LED overlay share one tree",
                 "[overlays][led_control][overlay-root-reuse]") {
    FreshOverlays fresh;
    seed_nav_panels(test_screen());
    const uint32_t before = lv_obj_get_child_count(test_screen());

    lv_obj_t* first = helix::open_led_control_overlay(test_screen());
    REQUIRE(first != nullptr);
    lv_obj_t* second = helix::open_led_control_overlay(test_screen());

    CHECK(second == first);
    CHECK(get_led_control_overlay().get_root() == first);
    CHECK(lv_obj_get_child_count(test_screen()) == before + 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a fan overlay tree deleted out from under the singleton is rebuilt on next open",
                 "[overlays][fan_control][overlay-root-reuse]") {
    FreshOverlays fresh;
    state().fan_state().init_fans({"fan", "fan_generic chamber"});
    seed_nav_panels(test_screen());

    auto& overlay = get_fan_control_overlay();
    lv_obj_t* first = helix::open_fan_control_overlay(test_screen());
    REQUIRE(first != nullptr);
    const size_t dials = FanControlOverlayTestAccess::dial_count(overlay);
    REQUIRE(dials > 0);

    close_all();
    lv_obj_delete(first);

    // The dials point into the deleted tree; they must be gone before anything
    // (the next open's repopulate included) touches them.
    REQUIRE(FanControlOverlayTestAccess::dial_count(overlay) == 0);
    CHECK(overlay.get_root() == nullptr);

    seed_nav_panels(test_screen());
    lv_obj_t* reopened = helix::open_fan_control_overlay(test_screen());
    REQUIRE(reopened != nullptr);
    CHECK(helix::test::widget_in_tree(reopened));
    CHECK(overlay.get_root() == reopened);
    lv_obj_t* container = FanControlOverlayTestAccess::fans_container(overlay);
    REQUIRE(container != nullptr);
    CHECK(is_descendant(container, reopened));
    CHECK(FanControlOverlayTestAccess::dial_count(overlay) == dials);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "an LED overlay tree deleted out from under the singleton is rebuilt on next open",
                 "[overlays][led_control][overlay-root-reuse]") {
    FreshOverlays fresh;
    seed_nav_panels(test_screen());

    lv_obj_t* first = helix::open_led_control_overlay(test_screen());
    REQUIRE(first != nullptr);

    close_all();
    lv_obj_delete(first);
    CHECK(get_led_control_overlay().get_root() == nullptr);

    // New widgets can take the freed root's address; the next open must still
    // build a tree rather than push one of them.
    seed_nav_panels(test_screen());
    lv_obj_t* reopened = helix::open_led_control_overlay(test_screen());
    REQUIRE(reopened != nullptr);
    CHECK(helix::test::widget_in_tree(reopened));
    CHECK(get_led_control_overlay().get_root() == reopened);
    CHECK(lv_obj_find_by_name(reopened, "led_tab_row") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "OverlayBase::show adopts the live tree another caller created",
                 "[overlays][lazy_panel][overlay-root-reuse]") {
    FreshOverlays fresh;
    seed_nav_panels(test_screen());
    const uint32_t before = lv_obj_get_child_count(test_screen());

    lv_obj_t* first_cache = nullptr;
    REQUIRE(get_global_motion_panel().show(test_screen()));
    first_cache = get_global_motion_panel().get_root();
    REQUIRE(first_cache != nullptr);

    lv_obj_t* second_cache = nullptr;
    REQUIRE(get_global_motion_panel().show(test_screen()));
    second_cache = get_global_motion_panel().get_root();

    CHECK(second_cache == first_cache);
    CHECK(get_global_motion_panel().get_root() == first_cache);
    CHECK(lv_obj_get_child_count(test_screen()) == before + 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "OverlayBase::show does not adopt a tree deleted out from under it",
                 "[overlays][lazy_panel][overlay-root-reuse]") {
    FreshOverlays fresh;
    seed_nav_panels(test_screen());

    lv_obj_t* first_cache = nullptr;
    REQUIRE(get_global_motion_panel().show(test_screen()));
    first_cache = get_global_motion_panel().get_root();
    REQUIRE(first_cache != nullptr);

    close_all();
    lv_obj_delete(first_cache);

    // New widgets can take the freed root's address.
    seed_nav_panels(test_screen());
    lv_obj_t* second_cache = nullptr;
    REQUIRE(get_global_motion_panel().show(test_screen()));
    second_cache = get_global_motion_panel().get_root();

    REQUIRE(second_cache != nullptr);
    CHECK(get_global_motion_panel().get_root() == second_cache);
    CHECK(lv_obj_find_by_name(second_cache, "jog_pad") != nullptr);
}

// A printer switch destroys the overlay singleton and frees its old tree on a
// later tick. The re-created singleton can be opened on a new root before
// then, so the old root's delete hook must leave the new tree alone.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "a printer switch frees the old fan overlay tree without touching the new one",
                 "[overlays][fan_control][switch][overlay-root-reuse]") {
    FreshOverlays fresh;
    state().fan_state().init_fans({"fan", "fan_generic chamber"});
    seed_nav_panels(test_screen());

    lv_obj_t* old_root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(old_root != nullptr);
    int old_deletes = 0;
    lv_obj_add_event_cb(old_root, count_delete, LV_EVENT_DELETE, &old_deletes);

    close_all();
    helix::ui::destroy_static_panels();
    REQUIRE(old_deletes == 0);

    seed_nav_panels(test_screen());
    lv_obj_t* new_root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(new_root != nullptr);
    REQUIRE(new_root != old_root);

    process_lvgl(100); // the old tree's deferred delete lands here
    REQUIRE(old_deletes == 1);

    auto& overlay = get_fan_control_overlay();
    CHECK(overlay.get_root() == new_root);
    CHECK(FanControlOverlayTestAccess::dial_count(overlay) > 0);
    CHECK(is_descendant(FanControlOverlayTestAccess::fans_container(overlay), new_root));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a printer switch frees the old LED overlay tree without touching the new one",
                 "[overlays][led_control][switch][overlay-root-reuse]") {
    FreshOverlays fresh;
    seed_nav_panels(test_screen());

    lv_obj_t* old_root = helix::open_led_control_overlay(test_screen());
    REQUIRE(old_root != nullptr);
    int old_deletes = 0;
    lv_obj_add_event_cb(old_root, count_delete, LV_EVENT_DELETE, &old_deletes);

    close_all();
    helix::ui::destroy_static_panels();
    REQUIRE(old_deletes == 0);

    seed_nav_panels(test_screen());
    lv_obj_t* new_root = helix::open_led_control_overlay(test_screen());
    REQUIRE(new_root != nullptr);
    REQUIRE(new_root != old_root);

    process_lvgl(100);
    REQUIRE(old_deletes == 1);

    CHECK(get_led_control_overlay().get_root() == new_root);
}

// After a printer switch the old tree stays allocated until a later tick, but
// it belongs to the destroyed panel: a caller opening the new panel must get a
// tree of its own.
TEST_CASE_METHOD(LVGLUITestFixture, "OverlayBase::show does not adopt the destroyed panel's tree",
                 "[overlays][lazy_panel][switch][overlay-root-reuse]") {
    FreshOverlays fresh;
    seed_nav_panels(test_screen());

    lv_obj_t* old_cache = nullptr;
    REQUIRE(get_global_motion_panel().show(test_screen()));
    old_cache = get_global_motion_panel().get_root();
    REQUIRE(old_cache != nullptr);
    int old_deletes = 0;
    lv_obj_add_event_cb(old_cache, count_delete, LV_EVENT_DELETE, &old_deletes);

    close_all();
    helix::ui::destroy_static_panels();
    REQUIRE(old_deletes == 0); // the old tree is still allocated for the reopen

    seed_nav_panels(test_screen());
    lv_obj_t* cache = nullptr;
    REQUIRE(get_global_motion_panel().show(test_screen()));
    cache = get_global_motion_panel().get_root();

    CHECK(cache != old_cache);
    CHECK(get_global_motion_panel().get_root() == cache);
    CHECK(lv_obj_find_by_name(cache, "jog_pad") != nullptr);

    close_all();
    process_lvgl(100);
    CHECK(old_deletes == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan overlay cleanup runs no queued callback",
                 "[overlays][fan_control][drain_reentrancy]") {
    FreshOverlays fresh;
    state().fan_state().init_fans({"fan", "fan_generic chamber"});
    seed_nav_panels(test_screen());
    auto& overlay = get_fan_control_overlay();
    REQUIRE(helix::open_fan_control_overlay(test_screen()) != nullptr);
    REQUIRE(FanControlOverlayTestAccess::dial_count(overlay) > 0);

    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::drain_canary", [&ran]() { ran = true; });
    overlay.cleanup();
    CHECK_FALSE(ran);
    CHECK(FanControlOverlayTestAccess::dial_count(overlay) == 0);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(ran);
}
