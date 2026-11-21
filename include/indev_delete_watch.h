// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl.h>
#include <utility>
#include <vector>

namespace helix {

/**
 * @brief Clears a raw pointer the moment LVGL deletes the indev it names
 *
 * lv_evdev deletes its own device when a read fails, as it does when a USB
 * touch panel is unplugged, and lv_deinit() deletes every remaining indev at
 * shutdown. A caller holding a raw copy of that pointer outside the object
 * that opened it - DisplayManager's m_pointer/m_keyboard mirror whatever
 * display backend created them - has no way to learn the device is gone
 * unless something is listening for LV_EVENT_DELETE.
 *
 * The optional callback runs after the slot is cleared, so a caller with its
 * own bookkeeping tied to the device can fold that into the same delete
 * notification instead of registering a second LV_EVENT_DELETE listener.
 * Main thread only, like every indev.
 */
class IndevDeleteWatch {
  public:
    /// Runs after the matching slot (if any) has been cleared, with the
    /// device that was deleted. Never dereferenced - the indev is being
    /// freed - only used to identify which entry to drop.
    using DeleteCallback = void (*)(void* ctx, const lv_indev_t* indev);

    IndevDeleteWatch() = default;
    IndevDeleteWatch(const IndevDeleteWatch&) = delete;
    IndevDeleteWatch& operator=(const IndevDeleteWatch&) = delete;

    /// Stop watching every device still tracked.
    ~IndevDeleteWatch() {
        forget_all();
    }

    /**
     * @brief Clear *slot the moment LVGL deletes @p indev, then run @p on_delete
     *
     * @param indev      a live device, never nullptr
     * @param slot       the caller's own pointer to @p indev, cleared on delete;
     *                   nullptr when the caller only wants the callback
     * @param on_delete  called after the slot is cleared, or nullptr for none
     * @param ctx        passed back to @p on_delete unchanged
     */
    void watch(lv_indev_t* indev, lv_indev_t** slot, DeleteCallback on_delete = nullptr,
               void* ctx = nullptr) {
        lv_indev_add_event_cb(indev, on_deleted, LV_EVENT_DELETE, this);
        watches_.push_back({indev, slot, on_delete, ctx});
    }

    /// Stop watching every device and forget them, without touching any slot
    /// or running any callback.
    void forget_all() {
        for (const Watch& w : watches_) {
            lv_indev_remove_event_cb_with_user_data(w.indev, on_deleted, this);
        }
        watches_.clear();
    }

  private:
    struct Watch {
        lv_indev_t* indev;
        lv_indev_t** slot;
        DeleteCallback on_delete;
        void* ctx;
    };

    /// lv_indev_delete() sends this before it frees the device.
    static void on_deleted(lv_event_t* e) {
        auto* self = static_cast<IndevDeleteWatch*>(lv_event_get_user_data(e));
        const auto* target = static_cast<const lv_indev_t*>(lv_event_get_target(e));

        // Take every matching entry out of watches_ (and clear its slot)
        // before running any callback, so a callback that calls watch() or
        // forget_all() on this same IndevDeleteWatch - re-adopting a
        // replacement device is the natural next thing for one to do - never
        // invalidates an iterator this loop is still holding.
        std::vector<std::pair<DeleteCallback, void*>> to_notify;
        for (auto it = self->watches_.begin(); it != self->watches_.end();) {
            if (it->indev != target) {
                ++it;
                continue;
            }
            // Only while the slot still names this device - a caller may
            // already have pointed it at a fresh device before this
            // delete event reaches it.
            if (it->slot != nullptr && *it->slot == target) {
                *it->slot = nullptr;
            }
            if (it->on_delete != nullptr) {
                to_notify.emplace_back(it->on_delete, it->ctx);
            }
            it = self->watches_.erase(it);
        }
        for (const auto& [on_delete, ctx] : to_notify) {
            on_delete(ctx, target);
        }
    }

    std::vector<Watch> watches_;
};

} // namespace helix
