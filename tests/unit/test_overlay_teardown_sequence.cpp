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
#include "../test_helpers/widget_in_tree.h"
#include "overlay_base.h"
#include "ui/ui_widget_helpers.h"

#include <memory>

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
    helix::ui::WidgetRef null_panel;

    CHECK_FALSE(teardown_overlay_ui(null_panel, "NullCase", TeardownDelete::Deferred, nullptr,
                                    {[&] { before_ran = true; }, [&] { after_ran = true; }}));
    CHECK_FALSE(before_ran);
    CHECK_FALSE(after_ran);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "teardown_overlay_ui: Deferred reparents to top layer, frees on async tick",
                 "[overlay_teardown][1134]") {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* parent = lv_obj_create(screen);
    helix::ui::WidgetRef root(lv_obj_create(parent));
    lv_obj_t* child = lv_obj_create(root);

    StubLifecycle lifecycle;
    NavigationManager::instance().register_overlay_instance(root, &lifecycle);
    REQUIRE(NavigationManagerTestAccess::lifecycle_of(NavigationManager::instance(), root) ==
            &lifecycle);

    lv_obj_t* cached = root;
    lv_obj_t* raw = root;

    bool after_saw_live_tree = false;
    bool after_saw_nulled_cached = false;
    CHECK(teardown_overlay_ui(root, "DeferredCase", TeardownDelete::Deferred, &cached,
                              TeardownHooks::after([&] {
                                  after_saw_live_tree = helix::test::widget_in_tree(raw) &&
                                                        helix::test::widget_in_tree(child);
                                  after_saw_nulled_cached = (cached == nullptr);
                              })));

    // Pointers null out immediately.
    CHECK(root == nullptr);
    CHECK(cached == nullptr);

    // Unregistered from NavigationManager.
    CHECK(NavigationManagerTestAccess::lifecycle_of(NavigationManager::instance(), raw) == nullptr);

    // Deferred contract: the tree is alive (hidden, reparented to the top
    // layer) until the async tick — never sync-deleted.
    CHECK(helix::test::widget_in_tree(raw));
    CHECK(lv_obj_get_parent(raw) == lv_layer_top());

    // after_delete runs after null-out but before the async free.
    CHECK(after_saw_live_tree);
    CHECK(after_saw_nulled_cached);

    process_async_timers();
    CHECK_FALSE(helix::test::widget_in_tree(raw));
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "teardown_overlay_ui: DetachSubtree detaches into a layout-less condemned container",
    "[overlay_teardown][1134]") {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* parent = lv_obj_create(screen);
    helix::ui::WidgetRef root(lv_obj_create(parent));
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
            before_saw_attached_live_tree = helix::test::widget_in_tree(raw) &&
                                            helix::test::widget_in_tree(child) &&
                                            lv_obj_get_parent(raw) == parent;
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
    CHECK(helix::test::widget_in_tree(raw));
    lv_obj_t* holder = lv_obj_get_parent(raw);
    CHECK(holder != lv_layer_top());
    CHECK(lv_obj_get_parent(holder) == lv_layer_top());
    CHECK(lv_obj_get_style_layout(raw, LV_PART_MAIN) == LV_LAYOUT_NONE);
    CHECK(lv_obj_has_flag(holder, LV_OBJ_FLAG_HIDDEN));

    // Free is still deferred — no sync delete in a close callback (#776).
    process_async_timers();
    CHECK_FALSE(helix::test::widget_in_tree(raw));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "teardown_overlay_ui: drains pending UpdateQueue work before tearing down",
                 "[overlay_teardown][1134]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    helix::ui::WidgetRef root(lv_obj_create(parent));

    bool queued_ran = false;
    helix::ui::queue_update([&] { queued_ran = true; });

    lv_obj_t* cached = root;
    CHECK(teardown_overlay_ui(root, "DrainCase", TeardownDelete::Deferred, &cached));

    // The drain inside the helper processed the queued callback while the
    // tree was still alive — this is the use-after-free guard the sequence
    // exists to provide.
    CHECK(queued_ran);
}

// The drain runs queued work, which can include a close callback that tears the
// same overlay down. That inner teardown completes it; the outer call must not
// run the owner's hooks a second time against what the inner one released.
TEST_CASE_METHOD(LVGLTestFixture,
                 "teardown_overlay_ui: a teardown the drain completes runs the hooks once",
                 "[overlay_teardown][drain_reentrancy]") {
    helix::ui::WidgetRef root(lv_obj_create(lv_screen_active()));
    int before_runs = 0;
    int after_runs = 0;
    auto hooks = [&]() { return TeardownHooks{[&] { ++before_runs; }, [&] { ++after_runs; }}; };

    helix::ui::UpdateQueue::instance().queue("test::nested_overlay_teardown", [&]() {
        teardown_overlay_ui(root, "NestedCase", TeardownDelete::Deferred, nullptr, hooks());
    });
    CHECK_FALSE(
        teardown_overlay_ui(root, "NestedCase", TeardownDelete::Deferred, nullptr, hooks()));

    CHECK(root == nullptr);
    CHECK(before_runs == 1);
    CHECK(after_runs == 1);
    process_async_timers();
}

// The teardown writes through `root` and runs the owner's hooks after its
// drain, so an owner freed by the drained work stops it: a contract breach,
// fatal under strict UI checks, logged and survived without them.
TEST_CASE_METHOD(LVGLTestFixture,
                 "teardown_overlay_ui: an owner freed by its own drain stops the teardown",
                 "[overlay_teardown][drain_reentrancy]") {
    const bool strict = helix::ui::strict_ui_checks();
    helix::ui::set_strict_ui_checks(false);

    auto owner = std::make_unique<helix::AsyncLifetimeGuard>();
    helix::ui::WidgetRef root(lv_obj_create(lv_screen_active()));
    lv_obj_t* const tree = root;
    int before_runs = 0;
    int after_runs = 0;
    TeardownHooks hooks{[&] { ++before_runs; }, [&] { ++after_runs; }};
    hooks.owner = owner->token();
    helix::ui::UpdateQueue::instance().queue("test::free_owner", [&owner]() { owner.reset(); });

    CHECK_FALSE(teardown_overlay_ui(root, "FreedOwner", TeardownDelete::Deferred, nullptr,
                                    std::move(hooks)));
    CHECK(before_runs == 0);
    CHECK(after_runs == 0);

    helix::ui::set_strict_ui_checks(strict);
    lv_obj_delete(tree);
}

// The drained close can be followed, in the same drain, by an open that builds
// a fresh tree into the same pointer. The outer teardown must leave that tree.
TEST_CASE_METHOD(LVGLTestFixture,
                 "teardown_overlay_ui: a tree re-created inside the drain survives the outer call",
                 "[overlay_teardown][drain_reentrancy]") {
    helix::ui::WidgetRef root(lv_obj_create(lv_screen_active()));
    lv_obj_t* fresh = nullptr;
    int after_runs = 0;

    helix::ui::UpdateQueue::instance().queue("test::close_then_reopen", [&]() {
        teardown_overlay_ui(root, "ReopenCase", TeardownDelete::Deferred, nullptr,
                            TeardownHooks::after([&] { ++after_runs; }));
        fresh = lv_obj_create(lv_screen_active());
        root = fresh;
    });
    CHECK_FALSE(teardown_overlay_ui(root, "ReopenCase", TeardownDelete::Deferred, nullptr,
                                    TeardownHooks::after([&] { ++after_runs; })));

    CHECK(after_runs == 1);
    CHECK(root == fresh);
    process_async_timers();
    CHECK(helix::test::widget_in_tree(fresh));
    lv_obj_delete(fresh);
}
