// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

/**
 * @file ui_container_delete_net.h
 * @brief The LV_EVENT_DELETE net a virtualized list view watches its
 *        container with.
 *
 * A view's widget pool caches pointers into its container's subtree. Hot
 * reload deletes and re-creates that subtree on the DEFERRED path while the
 * view itself lives on inside the surviving panel, so the pool must be wiped
 * the moment its tree dies — and a LATE deferred fire must not wipe a pool
 * already rebuilt under a replacement tree (prestonbrown/helixscreen#1396;
 * previously four hand-written copies of this rule across the list views).
 *
 * The net owns both halves: the callback acts only when the dying object is
 * the exact container it attached to, and retarget unhooks the old net
 * before the view moves on.
 */
class ContainerDeleteNet {
  public:
    ContainerDeleteNet() = default;
    ContainerDeleteNet(const ContainerDeleteNet&) = delete;
    ContainerDeleteNet& operator=(const ContainerDeleteNet&) = delete;

  protected:
    /// Views are held as members of their panels and never deleted through
    /// this base — hence non-virtual.
    ///
    /// The detach is the base's own guarantee, not a per-derived obligation:
    /// every current view calls cleanup() from its destructor, but a view
    /// added later that forgets would leave the container holding a callback
    /// into a half-destroyed object, and net_on_container_delete() would then
    /// dispatch on_netted_container_destroyed() through a vtable whose
    /// override is already gone — a pure-virtual call, or a plain UAF.
    /// detach_container_net() touches no virtual, so running it from here is
    /// legal. Same guarantee ~PanelWidget() gives its delete hook.
    ~ContainerDeleteNet() {
        detach_container_net();
    }

    /// The container the net currently watches, or nullptr when detached.
    lv_obj_t* netted_container() const {
        return netted_;
    }

    /// Point the net at @p container. When the net watched a different
    /// container before, that tree is gone (or about to die on the deferred
    /// path): unhook the old net and run on_netted_container_destroyed() so
    /// the owner drops its cached pointers before adopting the new tree.
    /// Re-pointing at the container already watched is a no-op.
    void retarget_container_net(lv_obj_t* container) {
        if (netted_ == container) {
            return;
        }
        if (netted_ != nullptr) {
            detach_container_net();
            on_netted_container_destroyed();
        }
        netted_ = container;
        if (netted_ != nullptr) {
            lv_obj_add_event_cb(netted_, net_on_container_delete, LV_EVENT_DELETE, this);
        }
    }

    /// Remove the net without running the destroyed hook — owner teardown
    /// (cleanup() clears its own state; the net must simply never fire
    /// again into an owner about to die).
    void detach_container_net() {
        // Guarded like every sibling uninstall (PanelWidget::uninstall_delete_hook(),
        // ~PrintStatusPanel(), ToolSwitcherWidget::detach()): this runs on
        // teardown paths that can execute after lv_deinit() — cleanup() calls it
        // ahead of a clear_cached_state() that guards its own LVGL calls for
        // exactly that reason, and the destructor above calls it later still.
        if (netted_ != nullptr && lv_is_initialized()) {
            lv_obj_remove_event_cb_with_user_data(netted_, net_on_container_delete, this);
        }
        netted_ = nullptr;
    }

    /// The watched tree died — its real LV_EVENT_DELETE, or a retarget away
    /// from it. Drop every pointer cached into that tree.
    virtual void on_netted_container_destroyed() = 0;

  private:
    static void net_on_container_delete(lv_event_t* e) {
        auto* self = static_cast<ContainerDeleteNet*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        // Guarded: a deferred subtree deletion can land after retarget()
        // has already re-pointed the net at a replacement tree, and that
        // late fire must not wipe the pool built under the new one.
        if (self->netted_ != lv_event_get_target(e)) {
            return;
        }
        self->netted_ = nullptr; // the callback dies with the container
        self->on_netted_container_destroyed();
    }

    lv_obj_t* netted_ = nullptr;
};
