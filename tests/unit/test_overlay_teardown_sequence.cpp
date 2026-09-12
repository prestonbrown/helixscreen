// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests for helix::ui::teardown_overlay_ui() — the one overlay teardown
// sequence OverlayBase::destroy_overlay_ui() and the AMS destroy_*_panel_ui()
// sites share (issue #1134). The call sites differ only in delete strategy
// (deferred vs condemned-subtree, #983) and in which owner hook they need;
// everything else — drain, unregister, breadcrumb, pointer null-out, deferred
// free — lives here and must not be re-implemented per site.

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/process_async_timers.h"
#include "overlay_base.h"

#include "../catch_amalgamated.hpp"

namespace {

class StubLifecycle : public IPanelLifecycle {
  public:
    void on_activate() override {}
    void on_deactivate(DeactivateReason) override {}
    const char* get_name() const override {
        return "StubOverlay";
    }
};

} // namespace

using helix::ui::teardown_overlay_ui;
using helix::ui::TeardownDelete;
using helix::ui::TeardownHooks;

TEST_CASE_METHOD(LVGLTestFixture, "teardown_overlay_ui: null root runs no hooks and returns false",
                 "[overlay_teardown][1134]") {
    bool before_ran = false;
    bool after_ran = false;
    lv_obj_t* null_panel = nullptr;

    CHECK_FALSE(teardown_overlay_ui(null_panel, "NullCase", TeardownDelete::Deferred, &null_panel,
                                    {[&] { before_ran = true; }, [&] { after_ran = true; }}));
    CHECK_FALSE(before_ran);
    CHECK_FALSE(after_ran);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "teardown_overlay_ui: Deferred reparents to top layer, frees on async tick",
                 "[overlay_teardown][1134]") {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* parent = lv_obj_create(screen);
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_t* child = lv_obj_create(root);

    StubLifecycle lifecycle;
    NavigationManager::instance().register_overlay_instance(root, &lifecycle);
    REQUIRE(NavigationManagerTestAccess::lifecycle_of(NavigationManager::instance(), root) ==
            &lifecycle);

    lv_obj_t* cached = root;
    lv_obj_t* raw = root;

    bool after_saw_live_tree = false;
    bool after_saw_nulled_cached = false;
    CHECK(teardown_overlay_ui(
        root, "DeferredCase", TeardownDelete::Deferred, &cached, TeardownHooks::after([&] {
            after_saw_live_tree = lv_obj_is_valid(raw) && lv_obj_is_valid(child);
            after_saw_nulled_cached = (cached == nullptr);
        })));

    // Pointers null out immediately.
    CHECK(root == nullptr);
    CHECK(cached == nullptr);

    // Unregistered from NavigationManager.
    CHECK(NavigationManagerTestAccess::lifecycle_of(NavigationManager::instance(), raw) == nullptr);

    // Deferred contract: the tree is alive (hidden, reparented to the top
    // layer) until the async tick — never sync-deleted.
    CHECK(lv_obj_is_valid(raw));
    CHECK(lv_obj_get_parent(raw) == lv_layer_top());

    // after_delete runs after null-out but before the async free.
    CHECK(after_saw_live_tree);
    CHECK(after_saw_nulled_cached);

    process_async_timers();
    CHECK_FALSE(lv_obj_is_valid(raw));
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "teardown_overlay_ui: DetachSubtree detaches into a layout-less condemned container",
    "[overlay_teardown][1134]") {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* parent = lv_obj_create(screen);
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_t* child = lv_obj_create(root);
    lv_obj_update_layout(screen);
    REQUIRE(lv_obj_get_child_count(parent) == 1);

    StubLifecycle lifecycle;
    NavigationManager::instance().register_overlay_instance(root, &lifecycle);
    NavigationManager::instance().register_overlay_close_callback(root, [] {});
    REQUIRE(NavigationManagerTestAccess::has_close_callback(NavigationManager::instance(), root));

    lv_obj_t* cached = root;
    lv_obj_t* raw = root;

    bool before_saw_attached_live_tree = false;
    CHECK(teardown_overlay_ui(
        root, "SubtreeCase", TeardownDelete::DetachSubtree, &cached, TeardownHooks::before([&] {
            // before_delete owns the moment while every pointer is still
            // valid AND the subtree is still attached — the AMS panels drop
            // their sidebar and context-menu sub-objects here.
            before_saw_attached_live_tree =
                lv_obj_is_valid(raw) && lv_obj_is_valid(child) && lv_obj_get_parent(raw) == parent;
        })));

    CHECK(root == nullptr);
    CHECK(cached == nullptr);
    CHECK(before_saw_attached_live_tree);

    // Both NavigationManager registrations are gone.
    CHECK(NavigationManagerTestAccess::lifecycle_of(NavigationManager::instance(), raw) == nullptr);
    CHECK_FALSE(
        NavigationManagerTestAccess::has_close_callback(NavigationManager::instance(), raw));

    // The #983 structural guarantee: the root left its original parent's
    // child list SYNCHRONOUSLY, so an ancestor relayout cannot iterate it.
    CHECK(lv_obj_get_child_count(parent) == 0);

    // ...but it sits in a hidden, layout-less condemned container, not the
    // top layer directly, and cannot drive a layout pass itself.
    CHECK(lv_obj_is_valid(raw));
    lv_obj_t* holder = lv_obj_get_parent(raw);
    CHECK(holder != lv_layer_top());
    CHECK(lv_obj_get_parent(holder) == lv_layer_top());
    CHECK(lv_obj_get_style_layout(raw, LV_PART_MAIN) == LV_LAYOUT_NONE);
    CHECK(lv_obj_has_flag(holder, LV_OBJ_FLAG_HIDDEN));

    // Free is still deferred — no sync delete in a close callback (#776).
    process_async_timers();
    CHECK_FALSE(lv_obj_is_valid(raw));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "teardown_overlay_ui: drains pending UpdateQueue work before tearing down",
                 "[overlay_teardown][1134]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* root = lv_obj_create(parent);

    bool queued_ran = false;
    helix::ui::queue_update([&] { queued_ran = true; });

    lv_obj_t* cached = root;
    CHECK(teardown_overlay_ui(root, "DrainCase", TeardownDelete::Deferred, &cached));

    // The drain inside the helper processed the queued callback while the
    // tree was still alive — this is the use-after-free guard the sequence
    // exists to provide.
    CHECK(queued_ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "teardown_overlay_ui: aliased root and cached pointer",
                 "[overlay_teardown][1134]") {
    // A site may hand the same variable in twice; nulling it through both
    // references has to stay idempotent on either strategy.
    lv_obj_t* detached = lv_obj_create(lv_screen_active());
    lv_obj_t* detached_raw = detached;
    CHECK(teardown_overlay_ui(detached, "AliasDetach", TeardownDelete::DetachSubtree, &detached));
    CHECK(detached == nullptr);
    CHECK(lv_obj_is_valid(detached_raw));
    process_async_timers();
    CHECK_FALSE(lv_obj_is_valid(detached_raw));

    lv_obj_t* deferred = lv_obj_create(lv_screen_active());
    lv_obj_t* deferred_raw = deferred;
    CHECK(teardown_overlay_ui(deferred, "AliasDefer", TeardownDelete::Deferred, &deferred));
    CHECK(deferred == nullptr);
    CHECK(lv_obj_is_valid(deferred_raw));
    process_async_timers();
    CHECK_FALSE(lv_obj_is_valid(deferred_raw));
}
