// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cleanup_helpers.cpp
 * @brief Tests for the safe-deletion helpers (safe_delete_obj)
 *
 * These helpers eliminate the if-delete-null pattern repeated in panel destructors.
 */

#include "../lvgl_test_fixture.h"
#include "ui/ui_cleanup_helpers.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

// ============================================================================
// safe_delete_obj() tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_obj deletes valid object and nulls pointer",
                 "[cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    REQUIRE(obj != nullptr);

    safe_delete_obj(obj);

    REQUIRE(obj == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_obj is safe with nullptr", "[cleanup_helpers]") {
    lv_obj_t* obj = nullptr;

    // Should not crash
    safe_delete_obj(obj);

    REQUIRE(obj == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_obj can be called multiple times safely",
                 "[cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());

    safe_delete_obj(obj);
    REQUIRE(obj == nullptr);

    // Second call should be safe (no double-free)
    safe_delete_obj(obj);
    REQUIRE(obj == nullptr);
}
// ============================================================================
// safe_delete_deferred() tests
// ============================================================================

#include "ui_utils.h"

#include "misc/lv_timer_private.h"

#include <vector>

/// Process pending lv_async_call / lv_obj_delete_async one-shot timers.
/// Unlike lv_timer_handler(), this only fires one-shot timers and avoids
/// the infinite-loop problem with display refresh timers in the test fixture.
static void process_async_timers() {
    for (int safety = 0; safety < 100; safety++) {
        bool found = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && t->timer_cb) {
                t->timer_cb(t);
                found = true;
                break; // Restart — list may have changed
            }
            t = next;
        }
        if (!found)
            break;
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_deferred nullifies pointer immediately",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    REQUIRE(obj != nullptr);

    helix::ui::safe_delete_deferred(obj);

    REQUIRE(obj == nullptr);

    // Process timers to execute the pending lv_obj_delete_async
    process_async_timers();
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_deferred deletes object after timer tick",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_t* raw_copy = obj;
    REQUIRE(lv_obj_is_valid(raw_copy));

    helix::ui::safe_delete_deferred(obj);

    // Pointer is nullified but object still exists until timer fires
    REQUIRE(obj == nullptr);
    // Object is hidden immediately
    REQUIRE(lv_obj_has_flag(raw_copy, LV_OBJ_FLAG_HIDDEN));

    // After timer tick, the async deletion executes
    process_async_timers();
    REQUIRE_FALSE(lv_obj_is_valid(raw_copy));
}

TEST_CASE_METHOD(LVGLTestFixture, "multiple safe_delete_deferred in same batch does not crash",
                 "[cleanup][cleanup_helpers]") {
    constexpr int COUNT = 5;
    lv_obj_t* objs[COUNT];
    lv_obj_t* raw_copies[COUNT];

    for (int i = 0; i < COUNT; ++i) {
        objs[i] = lv_obj_create(test_screen());
        raw_copies[i] = objs[i];
    }

    // Delete all in quick succession (simulates the #356 crash scenario).
    // Now uses lv_obj_delete_async so multiple deletes in the same tick
    // are safe — LVGL processes them individually, not in a batch.
    for (int i = 0; i < COUNT; ++i) {
        helix::ui::safe_delete_deferred(objs[i]);
    }

    // All pointers nullified immediately
    for (int i = 0; i < COUNT; ++i) {
        REQUIRE(objs[i] == nullptr);
    }

    // Timer tick should not crash — processes all async deletions
    REQUIRE_NOTHROW(process_async_timers());

    // All objects deleted after timer tick
    for (int i = 0; i < COUNT; ++i) {
        REQUIRE_FALSE(lv_obj_is_valid(raw_copies[i]));
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_deferred hides and defers deletion",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_t* raw_copy = obj;
    REQUIRE(lv_obj_is_valid(raw_copy));

    // safe_delete_deferred hides immediately and defers via lv_obj_delete_async
    helix::ui::safe_delete_deferred(obj);

    // Pointer nullified immediately
    REQUIRE(obj == nullptr);
    // Object hidden immediately but still exists
    REQUIRE(lv_obj_is_valid(raw_copy));
    REQUIRE(lv_obj_has_flag(raw_copy, LV_OBJ_FLAG_HIDDEN));

    // After timer tick, the async deletion executes
    process_async_timers();
    REQUIRE_FALSE(lv_obj_is_valid(raw_copy));
}

// ============================================================================
// safe_delete_subtree() tests (#983 teardown-time grid safety)
// ============================================================================

/// Build a grid container with @p n children laid out in a single column.
/// The grid descriptors are static so they outlive the layout pass.
static lv_obj_t* make_grid_with_children(lv_obj_t* parent, int n,
                                         std::vector<lv_obj_t*>& out_children) {
    static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t row_dsc[16];
    for (int i = 0; i < n && i < 15; ++i)
        row_dsc[i] = 40;
    row_dsc[n < 15 ? n : 15] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_size(grid, 200, 200);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    out_children.clear();
    for (int i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_create(grid);
        lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, i, 1);
        out_children.push_back(child);
    }
    lv_obj_update_layout(grid);
    return grid;
}

/// Returns true if @p needle is currently a direct child of @p container.
static bool is_child_of(lv_obj_t* container, lv_obj_t* needle) {
    uint32_t count = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < count; ++i) {
        if (lv_obj_get_child(container, static_cast<int32_t>(i)) == needle)
            return true;
    }
    return false;
}

// Property 1: detach is SYNCHRONOUS into an off-tree, layout-less condemned
// container — the structural guarantee that distinguishes safe_delete_subtree
// from a plain safe_delete_deferred. Both helpers drop the child out of the
// grid synchronously (deferred reparents to lv_layer_top()), so a bare
// child-count assertion does NOT distinguish them. The distinguishing property
// is the NEW parent: safe_delete_subtree moves the child into a hidden, size-0,
// non-scrollable, layout-less condemned container — NOT lv_layer_top() (a
// real, full-screen, layout-capable layer). The parent-property assertions
// below FAIL for safe_delete_deferred, proving this test exercises the stronger
// behavior.
TEST_CASE_METHOD(LVGLTestFixture,
                 "safe_delete_subtree detaches child into off-tree condemned container",
                 "[cleanup][cleanup_helpers][grid]") {
    constexpr int N = 4;
    std::vector<lv_obj_t*> children;
    lv_obj_t* grid = make_grid_with_children(test_screen(), N, children);
    REQUIRE(lv_obj_get_child_count(grid) == N);

    lv_obj_t* victim = children[1];
    REQUIRE(is_child_of(grid, victim));

    helix::ui::safe_delete_subtree(victim);

    // Immediately — BEFORE any async drain — the grid no longer lists the child.
    REQUIRE(lv_obj_get_child_count(grid) == N - 1);
    REQUIRE_FALSE(is_child_of(grid, victim));

    // The child's NEW parent is the condemned container, not lv_layer_top().
    // (safe_delete_deferred would put it on lv_layer_top(), failing these.)
    lv_obj_t* new_parent = lv_obj_get_parent(victim);
    REQUIRE(new_parent != nullptr);
    REQUIRE(new_parent != lv_layer_top());
    REQUIRE(new_parent != grid);
    // The condemned container is hidden, size-0, and non-scrollable.
    REQUIRE(lv_obj_has_flag(new_parent, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(new_parent, LV_OBJ_FLAG_SCROLLABLE));
    REQUIRE(lv_obj_get_width(new_parent) == 0);
    REQUIRE(lv_obj_get_height(new_parent) == 0);

    process_async_timers();
}

// Contrast test: documents that a plain safe_delete_deferred reparents to
// lv_layer_top() (a live, full-screen, layout-capable layer) rather than an
// off-tree condemned container. This is the behavior test #1 above guards
// against — if safe_delete_subtree degraded to a plain deferred delete, the
// new-parent assertions in test #1 would fail.
TEST_CASE_METHOD(LVGLTestFixture,
                 "safe_delete_deferred reparents to lv_layer_top not a condemned container",
                 "[cleanup][cleanup_helpers][grid]") {
    constexpr int N = 4;
    std::vector<lv_obj_t*> children;
    lv_obj_t* grid = make_grid_with_children(test_screen(), N, children);
    REQUIRE(lv_obj_get_child_count(grid) == N);

    lv_obj_t* victim = children[1];
    lv_obj_t* victim_copy = victim;
    helix::ui::safe_delete_deferred(victim);

    REQUIRE(victim == nullptr);            // reference overload nulls the caller pointer
    REQUIRE(lv_obj_is_valid(victim_copy)); // not freed yet (deferred)
    // The defining difference: deferred puts the child directly on lv_layer_top.
    REQUIRE(lv_obj_get_parent(victim_copy) == lv_layer_top());

    process_async_timers();
    REQUIRE_FALSE(lv_obj_is_valid(victim_copy));
}

// Property 2: relayout of the original grid after teardown is safe and never
// lays out / re-lists the deleted child.
TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_subtree relayout after teardown is safe",
                 "[cleanup][cleanup_helpers][grid]") {
    constexpr int N = 5;
    std::vector<lv_obj_t*> children;
    lv_obj_t* grid = make_grid_with_children(test_screen(), N, children);

    lv_obj_t* victim = children[2];
    helix::ui::safe_delete_subtree(victim);

    // Force an ancestor relayout immediately — must not crash and must not
    // iterate / re-list the detached child.
    REQUIRE_NOTHROW(lv_obj_update_layout(grid));
    REQUIRE_FALSE(is_child_of(grid, victim));
    REQUIRE(lv_obj_get_child_count(grid) == N - 1);

    process_async_timers();
    REQUIRE_NOTHROW(lv_obj_update_layout(grid));
}

// Property 3: the subtree (root + descendants) is eventually freed after the
// deferred queue drains. Uses an LV_EVENT_DELETE sentinel on a descendant.
static bool s_subtree_child_deleted = false;
static void subtree_delete_sentinel(lv_event_t*) {
    s_subtree_child_deleted = true;
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_subtree eventually frees the whole subtree",
                 "[cleanup][cleanup_helpers][grid]") {
    s_subtree_child_deleted = false;

    std::vector<lv_obj_t*> children;
    lv_obj_t* grid = make_grid_with_children(test_screen(), 3, children);
    lv_obj_t* root = children[0];
    lv_obj_t* root_copy = root;

    // A nested descendant under the subtree root carries the delete sentinel.
    lv_obj_t* descendant = lv_obj_create(root);
    lv_obj_add_event_cb(descendant, subtree_delete_sentinel, LV_EVENT_DELETE, nullptr);

    helix::ui::safe_delete_subtree(root);

    // Synchronously detached from the grid already.
    REQUIRE_FALSE(is_child_of(grid, root_copy));
    REQUIRE_FALSE(s_subtree_child_deleted); // not freed yet (deferred)

    process_async_timers();

    // Whole subtree freed: descendant's DELETE fired and the root is invalid.
    REQUIRE(s_subtree_child_deleted);
    REQUIRE_FALSE(lv_obj_is_valid(root_copy));
}

// Property 4: null / invalid input is a safe no-op.
TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_subtree is a no-op on null/invalid input",
                 "[cleanup][cleanup_helpers][grid]") {
    REQUIRE_NOTHROW(helix::ui::safe_delete_subtree(nullptr));

    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_delete(obj); // obj is now stale/invalid
    REQUIRE_NOTHROW(helix::ui::safe_delete_subtree(obj));
}
