// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "overlay_base.h"

#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "system/crash_handler.h"

#include <spdlog/spdlog.h>

OverlayBase::~OverlayBase() {
    // Fallback unregister in case cleanup() wasn't called.
    // Guard against Static Destruction Order Fiasco: during shutdown,
    // NavigationManager may already be destroyed.
    if (overlay_root_ && !NavigationManager::is_destroyed()) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Guard against Static Destruction Order Fiasco: spdlog may already be
    // destroyed if this overlay wasn't registered with StaticPanelRegistry.
    if (!NavigationManager::is_destroyed()) {
        spdlog::trace("[OverlayBase] Destroyed");
    }
}

void OverlayBase::on_activate() {
    spdlog::trace("[OverlayBase] on_activate() - {}", get_name());
    visible_ = true;
}

void OverlayBase::on_view_hidden() {
    spdlog::trace("[OverlayBase] deactivated - {}", get_name());
    visible_ = false;
}

void OverlayBase::cleanup() {
    spdlog::trace("[OverlayBase] cleanup() - {}", get_name());
    lifetime_.invalidate();
    object_lifetime_.invalidate();
    cleanup_called_ = true;
    visible_ = false;
}

void OverlayBase::destroy_overlay_ui(lv_obj_t*& cached_panel) {
    helix::ui::teardown_overlay_ui(
        overlay_root_, get_name(), helix::ui::TeardownDelete::Deferred, &cached_panel,
        helix::ui::TeardownHooks::after([this]() { on_ui_destroyed(); }));
}

bool helix::ui::teardown_overlay_ui(lv_obj_t*& root, const char* owner_name, TeardownDelete how,
                                    lv_obj_t** cached_panel, TeardownHooks hooks) {
    if (!root) {
        return false;
    }

    spdlog::info("[{}] Destroying overlay UI to free memory", owner_name);

    // Drain deferred observer callbacks while all pointers are still valid.
    // observe_int_sync queues lambdas via queue_update() that capture raw
    // panel pointers. Processing them here prevents use-after-free.
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    // Unregister from NavigationManager before deleting the widget. Doing it
    // first also prevents double-invocation when destroy is called manually
    // while the panel is still in the overlay stack.
    NavigationManager::instance().unregister_overlay_close_callback(root);
    NavigationManager::instance().unregister_overlay_instance(root);

    // Breadcrumb the destroy so crashes in the close path can be pinned to
    // which overlay was being torn down. Pairs with the "overlay+" crumb on
    // push.
    crash_handler::breadcrumb::note("ovrl_dst", owner_name);

    // Owner hook while every pointer is still valid and the tree is still
    // attached — owners whose sub-objects own widgets in this subtree (AMS
    // sidebars, context menus, modals) drop them here.
    if (hooks.before_delete) {
        hooks.before_delete();
    }

    if (how == TeardownDelete::DetachSubtree) {
        // Takes a raw pointer and does not null its argument — the null-out
        // below covers both strategies.
        helix::ui::safe_delete_subtree(root);
    } else {
        // Sync `safe_delete` is banned in overlay close callbacks
        // (ui_utils.h) — multiple sync deletions in the same UpdateQueue
        // batch corrupt LVGL's global event list (#776, #190, #80, #840).
        // This runs as a close callback on memory-constrained devices via
        // register_overlay_close_callback(), so deferral is mandatory.
        helix::ui::safe_delete_deferred(root);
    }
    root = nullptr;
    if (cached_panel) {
        *cached_panel = nullptr;
    }

    // The widget tree is still alive (hidden, off-tree) until the async tick
    // frees it, so the owner can null child-widget pointers that must stay
    // dereferenceable during teardown.
    if (hooks.after_delete) {
        hooks.after_delete();
    }
    return true;
}

lv_obj_t* OverlayBase::create_overlay_from_xml(lv_obj_t* parent, const char* component_name) {
    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Creating overlay from XML", get_name());

    parent_screen_ = parent;
    cleanup_called_ = false;

    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, component_name, nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    ui_overlay_panel_setup_standard(overlay_root_, parent_screen_, "overlay_header",
                                    "overlay_content");
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    return overlay_root_;
}

bool OverlayBase::rebuild() {
    if (!overlay_root_ || !parent_screen_) {
        spdlog::debug("[OverlayBase::rebuild] {} — no widget, skipping", get_name());
        return false;
    }

    spdlog::info("[OverlayBase::rebuild] {} — tearing down and re-creating", get_name());

    bool was_visible = visible_;

    on_deactivate(DeactivateReason::Rebuild); // invalidates lifetime_, clears visible_

    lv_obj_t* old_root = overlay_root_;
    overlay_root_ = nullptr;

    // Condemn the old subtree BEFORE create(). The component was just
    // re-registered, and lv_xml_component_unregister() freed every lv_style_t
    // in the old scope — styles the old widgets still reference. Building the
    // replacement on the same screen triggers a layout pass that would walk the
    // old subtree and dereference them. safe_delete_subtree() detaches it
    // synchronously into an off-tree, layout-less container first.
    //
    // Restoring the old widget on failure is therefore not an option: its
    // styles are already gone.
    helix::ui::safe_delete_subtree(old_root);

    lv_obj_t* new_root = create(parent_screen_);
    if (!new_root) {
        spdlog::error("[OverlayBase::rebuild] {} — create() returned null; overlay is now gone, "
                      "restart to recover",
                      get_name());
        return false;
    }

    // create() sets overlay_root_ internally; some implementations also call
    // register_callbacks() — let them manage their own wiring.
    overlay_root_ = new_root;

    if (was_visible) {
        lv_obj_remove_flag(new_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(new_root, LV_OBJ_FLAG_HIDDEN);
    }

    NavigationManager::instance().rekey_overlay_widget(old_root, new_root);

    // create() reproduces the XML; anything this overlay populated into the old
    // tree from a separate entry point has to be re-applied by hand.
    repopulate();

    if (was_visible) {
        on_activate();
    }
    return true;
}
