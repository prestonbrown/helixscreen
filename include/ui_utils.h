// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_effects.h"
#include "ui_update_queue.h"

#include "lvgl/lvgl.h"
#include "static_panel_registry.h"

#include <cstdint>
#include <string_view>

// ============================================================================
// Responsive Layout Utilities
// ============================================================================

/**
 * @brief Get responsive padding for content areas below headers
 *
 * Returns the current breakpoint's space_lg token, which the theme system
 * scales per display size — smaller on tiny/small screens for more compact
 * layouts.
 *
 * @return Padding value in pixels
 */
lv_coord_t ui_get_header_content_padding();

/**
 * @brief Get responsive header height based on screen size
 *
 * Returns smaller header on tiny/small screens for more compact layouts.
 *
 * @param screen_height Current screen height in pixels
 * @return Header height in pixels (60px for large/medium, 48px for small, 40px for tiny)
 */
lv_coord_t ui_get_responsive_header_height(lv_coord_t screen_height);

// ============================================================================
// LED Icon Utilities
// ============================================================================

/**
 * @brief Get lightbulb icon name for LED brightness level
 *
 * Maps brightness percentage (0-100) to appropriate graduated lightbulb icon.
 * Returns icons from lightbulb_outline (off) through lightbulb_on_10..90 to
 * lightbulb_on (100%).
 *
 * @param brightness LED brightness 0-100%
 * @return Icon name string for ui_icon_set_source()
 */
const char* ui_brightness_to_lightbulb_icon(int brightness);

// ============================================================================
// List/Empty State Visibility
// ============================================================================

namespace helix::ui {

/**
 * @brief Toggle visibility between a list container and its empty state
 *
 * Common pattern for panels that show either a populated list or an empty
 * state placeholder. When has_items is true, the list is shown and empty
 * state is hidden; when false, the opposite.
 *
 * @param list The list/content container widget (may be nullptr)
 * @param empty_state The empty state placeholder widget (may be nullptr)
 * @param has_items Whether the list has items to display
 */
inline void toggle_list_empty_state(lv_obj_t* list, lv_obj_t* empty_state, bool has_items) {
    if (list)
        lv_obj_set_flag(list, LV_OBJ_FLAG_HIDDEN, !has_items);
    if (empty_state)
        lv_obj_set_flag(empty_state, LV_OBJ_FLAG_HIDDEN, has_items);
}

// ============================================================================
// Object Lifecycle Utilities
// ============================================================================

/**
 * @brief Safely delete an LVGL object, guarding against shutdown race conditions
 *
 * During app shutdown, lv_is_initialized() can return true even after the display
 * has been torn down. This function checks both that LVGL is initialized AND
 * that a display still exists before attempting deletion.
 *
 * The pointer is automatically set to nullptr after deletion (or if skipped).
 *
 * WARNING: This performs SYNCHRONOUS deletion. Do NOT call from inside
 * queue_update(), async_call(), or overlay close callbacks — multiple
 * synchronous deletions in the same UpdateQueue batch corrupt LVGL's event
 * linked list (SIGSEGV in lv_event_mark_deleted). Use safe_delete_deferred()
 * in those contexts instead.
 *
 * @param obj Reference to pointer to the LVGL object (will be set to nullptr)
 * @return true if object was deleted, false if skipped (nullptr or shutdown in progress)
 */
inline bool safe_delete(lv_obj_t*& obj) {
    if (!obj)
        return false;
    if (!lv_is_initialized()) {
        obj = nullptr;
        return false;
    }
    if (!lv_display_get_next(nullptr)) {
        obj = nullptr;
        return false;
    }
    // Skip during destroy_all() - lv_deinit() will clean up all widgets
    if (StaticPanelRegistry::is_destroying_all()) {
        obj = nullptr;
        return false;
    }
    // Guard against stale pointers to already-deleted objects (e.g. children
    // auto-deleted by parent deletion before the child pointer was nulled)
    if (!lv_obj_is_valid(obj)) {
        obj = nullptr;
        return false;
    }
    // Remove entire tree from focus group before deletion to prevent LVGL from
    // auto-focusing the next element (which triggers scroll-on-focus)
    helix::ui::defocus_tree(obj);
    lv_obj_delete(obj);
    obj = nullptr;
    return true;
}

/**
 * @brief Check that an LVGL object's parent chain terminates within a sane depth
 *
 * A non-terminating parent chain indicates corruption: UAF where freed memory
 * was reused, double-reparent producing a cycle, or wild pointer mistaken as
 * valid by lv_obj_is_valid() (which walks the screen tree — a reused address
 * can pass validity while pointing at a different live object).
 *
 * When this returns false, callers should avoid operations that walk or
 * rewrite the parent chain (lv_obj_set_parent, lv_obj_get_display), since
 * they'll spin or corrupt state further. Mirrors the 128-step cap applied
 * in the lvgl_obj_get_screen_cycle_guard patch.
 */
inline bool has_sane_parent_chain(lv_obj_t* obj) {
    uint32_t depth = 0;
    lv_obj_t* p = obj;
    while (p) {
        if (++depth > 128) {
            return false;
        }
        p = lv_obj_get_parent(p);
    }
    return true;
}

/**
 * @brief True if @p obj is currently parented under the active screen of its display.
 *
 * Teardown helpers (safe_clean_children(), safe_delete_deferred()) reparent
 * condemned widgets onto lv_layer_top() before async deletion, where they linger
 * with their layout intact until the next async tick. A widget that normally lives
 * on a screen but now roots at a layer (or a different screen) has been moved out
 * from under a live layout: triggering a relayout on it (e.g. lv_image_set_src)
 * makes lv_obj_update_layout walk the whole layer and recurse into sibling
 * condemned grid subtrees whose children may already be freed — a use-after-free
 * in grid calc() (#1001). Use this to suppress relayout-triggering work on a widget
 * that may be mid-teardown.
 *
 * NOTE: legitimate overlays/modals/toasts also live on lv_layer_top(). A false
 * result means "not under the active screen", which signals teardown ONLY for
 * widgets that belong on a screen to begin with — judge per caller.
 */
inline bool is_on_active_screen(lv_obj_t* obj) {
    if (!obj || !lv_obj_is_valid(obj) || !has_sane_parent_chain(obj)) {
        return false;
    }
    lv_display_t* disp = lv_obj_get_display(obj);
    if (!disp) {
        return false;
    }
    return lv_obj_get_screen(obj) == lv_display_get_screen_active(disp);
}

/**
 * @brief Queue LVGL object deletion for the next timer tick
 *
 * Immediately nullifies the pointer to prevent further use, hides the
 * object, and defers actual deletion via lv_obj_delete_async(). This
 * avoids crashes from calling lv_obj_delete() inside UpdateQueue's
 * process_pending() batch, where multiple deletions corrupt LVGL's
 * global event linked list (SIGSEGV in lv_event_mark_deleted).
 *
 * Uses lv_obj_delete_async() (not lv_async_call with a custom lambda)
 * so that LVGL's built-in cancellation logic works — if something else
 * deletes the object first, obj_delete_core() cancels the pending async.
 *
 * @param obj Reference to pointer to the LVGL object (set to nullptr immediately)
 */
inline void safe_delete_deferred(lv_obj_t*& obj) {
    if (!obj)
        return;
    if (!lv_is_initialized() || !lv_display_get_next(nullptr) ||
        StaticPanelRegistry::is_destroying_all()) {
        obj = nullptr;
        return;
    }
    if (!lv_obj_is_valid(obj)) {
        obj = nullptr;
        return;
    }
    // Abort before touching obj if its parent chain is cyclic — lv_obj_is_valid
    // has false positives when memory is reused by a different live object, and
    // lv_obj_set_parent below would then spin in lv_obj_get_screen (#SonicPad
    // v0.99.35 grid-edit hang). Null the caller's pointer and let the corrupt
    // tree be cleaned up by lv_deinit() at shutdown.
    if (!has_sane_parent_chain(obj)) {
        obj = nullptr;
        return;
    }
    lv_obj_t* to_delete = obj;
    obj = nullptr;
    // Hide immediately so the widget isn't visible while deletion is deferred
    lv_obj_add_flag(to_delete, LV_OBJ_FLAG_HIDDEN);
    defocus_tree(to_delete);
    // Reparent to lv_layer_top() so the original parent's destruction won't
    // recursively delete this child while it's queued for async delete.
    // Without this, obj_delete_core can hit an already-freed child whose
    // event list is corrupted — SIGSEGV in lv_event_mark_deleted (ad5x).
    lv_obj_t* layer = lv_layer_top();
    if (layer) {
        lv_obj_set_parent(to_delete, layer);
    }
    // Defer deletion to next lv_timer_handler tick — outside UpdateQueue batch
    lv_obj_delete_async(to_delete);
}

/**
 * @brief Deferred-delete for raw pointers (no automatic nulling)
 *
 * Same safety as the reference overload (hide, defocus, reparent, async delete)
 * but for local variables or cases where the caller manages the pointer.
 *
 * @param obj Raw pointer to the LVGL object (caller must null their copy)
 */
inline void safe_delete_deferred_raw(lv_obj_t* obj) {
    if (!obj)
        return;
    if (!lv_is_initialized() || !lv_display_get_next(nullptr) ||
        StaticPanelRegistry::is_destroying_all()) {
        return;
    }
    if (!lv_obj_is_valid(obj)) {
        return;
    }
    if (!has_sane_parent_chain(obj)) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    defocus_tree(obj);
    lv_obj_t* layer = lv_layer_top();
    if (layer) {
        lv_obj_set_parent(obj, layer);
    }
    lv_obj_delete_async(obj);
}

/**
 * @brief Safe replacement for lv_obj_clean() inside UpdateQueue callbacks
 *
 * lv_obj_clean() synchronously deletes every child. Called from inside a
 * queue_update()/lifetime_.defer() lambda, those sync deletions share the
 * same UpdateQueue::process_pending() batch — and multiple sync deletions
 * in one batch corrupt LVGL's event linked list (SIGSEGV in
 * lv_event_mark_deleted, #776).
 *
 * This helper reparents each child to lv_layer_top() and schedules it for
 * lv_obj_delete_async(), which runs on LVGL's own async list OUTSIDE our
 * UpdateQueue batch. The container appears empty immediately, so callers
 * can add new children right after.
 *
 * @param container Parent whose children will be detached + async-deleted
 */
inline void safe_clean_children(lv_obj_t* container) {
    if (!container)
        return;
    // During shutdown safe_delete_deferred_raw early-returns without
    // detaching the child, so the count never drops and the loop spins
    // forever. Same guards as the helper itself — bail and let lv_deinit
    // handle final cleanup.
    if (!lv_is_initialized() || !lv_display_get_next(nullptr) ||
        StaticPanelRegistry::is_destroying_all()) {
        return;
    }
    lv_obj_update_layout(container);
    while (lv_obj_get_child_count(container) > 0) {
        const uint32_t before = lv_obj_get_child_count(container);
        safe_delete_deferred_raw(lv_obj_get_child(container, 0));
        // Defensive: if a child can't be detached (e.g. invalid parent
        // chain), the helper silently no-ops — stop rather than spin.
        if (lv_obj_get_child_count(container) >= before) {
            break;
        }
    }
}

/**
 * @brief Comprehensively safe deletion of a whole widget subtree
 *
 * Makes an LVGL layout pass (grid_update / flex_update) on a being-deleted
 * subtree STRUCTURALLY IMPOSSIBLE rather than merely time-shifted. This is the
 * teardown-time counterpart to the deferred grid-activation fix in
 * populate_widgets (#983): there a relayout racing the BUILD of a grid was the
 * hazard; here it is a relayout racing the TEARDOWN of a grid (modal close /
 * panel rebuild during an AMS update) that could iterate a freed container or
 * child.
 *
 * Why this is strictly stronger than safe_delete_deferred() alone:
 *  - safe_delete_deferred() reparents @p obj to lv_layer_top() and async-deletes
 *    it, but @p obj remains a child of its ORIGINAL parent only until the async
 *    fires. (It reparents to the layer, but an ancestor of the original parent
 *    could still relayout @p obj before that, and grid_update on @p obj itself
 *    could still run.) This helper closes that window deterministically.
 *  - Detach-before-delete: by moving @p obj into an off-tree, layout-less
 *    condemned container synchronously, the original parent's child list no
 *    longer contains @p obj, so any ancestor relayout of the original parent
 *    cannot iterate @p obj. The original parent is marked dirty and relayouts
 *    WITHOUT @p obj.
 *  - Layout-less destination: the condemned container has no grid/flex layout,
 *    so grid_update / flex_update never runs on it (or on @p obj via it).
 *  - LV_LAYOUT_NONE on @p obj: belt-and-suspenders so @p obj itself can never
 *    drive a layout pass over its own (doomed) children.
 *  - Deferred deletion: the condemned subtree is freed via
 *    safe_delete_deferred(), which escapes the current UpdateQueue batch and
 *    uses lv_obj_delete_async() — avoiding the multi-sync-delete event-list
 *    corruption (#776, #190, #80).
 *
 * Safe to call from inside UpdateQueue / observer / queued callbacks — that is
 * the entire point. Mirrors the proven condemned-container pattern in
 * ams_detail_destroy_slots().
 *
 * @param obj Subtree root to delete. nullptr / invalid input is a no-op.
 *            (Caller-held pointers are NOT nulled — null your own copy.)
 */
inline void safe_delete_subtree(lv_obj_t* obj) {
    if (!obj || !lv_obj_is_valid(obj))
        return;

    // Belt-and-suspenders: the object itself can no longer drive a grid/flex
    // layout pass over its own children before it is freed.
    lv_obj_set_layout(obj, LV_LAYOUT_NONE);

    // Off-tree, hidden, size-0, layout-less condemned container. No grid/flex
    // layout means grid_update never runs on it (or on obj via it).
    lv_obj_t* condemned = lv_obj_create(lv_layer_top());
    lv_obj_add_flag(condemned, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(condemned, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(condemned, 0, 0);

    // Detach obj from its original parent's child list synchronously. After
    // this, an ancestor relayout of the original parent no longer iterates obj;
    // the original parent is marked dirty and relayouts WITHOUT obj.
    lv_obj_set_parent(obj, condemned);

    // Free the whole condemned subtree (incl. obj and its descendants) off-tree
    // on the deferred path — escapes the UpdateQueue batch.
    helix::ui::defocus_tree(condemned);
    helix::ui::safe_delete_deferred(condemned);
}

/**
 * @brief Destroy every StaticPanelRegistry panel, then free the overlay
 *        widgets the panel destructors had to leave allocated
 *
 * destroy_all() forbids widget deletion inside its window (LV_EVENT_DELETE
 * would fire into the half-destroyed panel set), so an OverlayBase whose root
 * is still allocated records it with the registry instead of deleting it. This
 * wrapper runs the destroy and frees what was handed back - the soft-restart
 * (printer switch) teardown spelling of destroy_all().
 *
 * Full shutdown must NOT use this: it calls StaticPanelRegistry::destroy_all()
 * directly, ignores the recorded roots, and lets lv_deinit() free every widget
 * - deleting them mid-shutdown reopens the crash window the skip exists for.
 */
inline void destroy_static_panels() {
    for (lv_obj_t* orphan_root : StaticPanelRegistry::instance().destroy_all()) {
        safe_delete_deferred_raw(orphan_root);
    }
}

// ============================================================================
// Recursive Widget Flag Utilities
// ============================================================================

/**
 * @brief Recursively remove CLICKABLE flag from all descendants of obj
 *
 * Used by edit mode to prevent widget click handlers from firing
 * while grid rearrangement is in progress.
 *
 * @param obj Parent object whose descendants will have CLICKABLE removed
 */
inline void disable_widget_clicks_recursive(lv_obj_t* obj) {
    if (!obj)
        return;
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
        if (!child)
            continue;
        lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);
        disable_widget_clicks_recursive(child);
    }
}

/**
 * @brief Recursively remove PRESSED state from obj and all descendants
 *
 * Clears visual press feedback from deeply nested children after
 * cancelling a press (e.g., when entering edit mode via long-press).
 *
 * @param obj Root object to clear PRESSED state from
 */
inline void clear_pressed_state_recursive(lv_obj_t* obj) {
    if (!obj)
        return;
    lv_obj_remove_state(obj, LV_STATE_PRESSED);
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        clear_pressed_state_recursive(lv_obj_get_child(obj, static_cast<int32_t>(i)));
    }
}

// ============================================================================
// Owned user_data strings
// ============================================================================

/**
 * @brief Attach an owned copy of @p s to @p obj's user_data, freed on LV_EVENT_DELETE
 *
 * Replaces the hand-rolled `lv_malloc` + `memcpy` + per-site LV_EVENT_DELETE
 * lambda that dynamically-built rows/cards use to carry a per-instance key
 * (filename, klipper name, object name) into their click handlers.
 *
 * Ownership rules (lesson L069 — user_data is a single shared slot):
 * - The slot must be empty, or already hold a string set by this helper.
 *   Anything else (an XML widget's own payload, ui_button's `button_data_t*`)
 *   is left completely untouched and the call returns false.
 * - Calling again on the same object frees the previous copy and reuses the
 *   single registered cleanup handler.
 * - The cleanup handler nulls the slot after freeing, so a repeated DELETE
 *   delivery cannot double-free.
 *
 * @param obj Object to own the string (must be valid; nullptr returns false)
 * @param s   Bytes to copy. Copied exactly; need not be NUL-terminated.
 * @return true on success; false on allocation failure, an overlong length, a
 *         null object, or a contested slot — in every failure case user_data is
 *         left exactly as it was.
 */
bool set_owned_user_string(lv_obj_t* obj, std::string_view s);

/**
 * @brief Read back a string previously attached by set_owned_user_string()
 *
 * Returns nullptr for a null object, an empty slot, or a slot owned by anything
 * other than this helper — it never casts a foreign user_data pointer to char*.
 */
const char* get_owned_user_string(lv_obj_t* obj);

} // namespace helix::ui
