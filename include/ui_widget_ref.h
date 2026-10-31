// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl.h>
#include <memory>

namespace helix::ui {

/**
 * @brief A cached lv_obj_t* that becomes nullptr the moment LVGL deletes the
 *        widget (prestonbrown/helixscreen#1298)
 *
 * An owner that outlives the widgets it points at (a panel, an overlay, a
 * singleton) cannot learn from its own lifetime that a widget died: a raw
 * lv_obj_delete() of the tree, a hot-reload rebuild or a backdrop-tap dismissal
 * frees the widget while the owner, and every AsyncLifetimeGuard or observer
 * token keyed on the owner, stays valid. This handle keys on the widget's own
 * LV_EVENT_DELETE instead, so a deferred callback reading it sees nullptr, not
 * freed memory.
 *
 * Nulling makes the stale case testable, not harmless: LVGL setters handed a
 * nullptr still crash, so a path that can run after the widget dies checks the
 * handle first.
 *
 * The hook's user_data is this handle's address, so a WidgetRef neither copies
 * nor moves; hold it as a member or in a fixed-size array. Assigning a new
 * widget, or nullptr, unhooks the old one; the destructor unhooks too.
 * LVGL thread only, like the widget it names.
 */
class WidgetRef {
  public:
    WidgetRef() = default;
    explicit WidgetRef(lv_obj_t* obj) {
        reset(obj);
    }
    ~WidgetRef() {
        reset(nullptr);
    }

    WidgetRef(const WidgetRef&) = delete;
    WidgetRef& operator=(const WidgetRef&) = delete;

    WidgetRef& operator=(lv_obj_t* obj) {
        reset(obj);
        return *this;
    }

    void reset(lv_obj_t* obj = nullptr) {
        if (obj == obj_) {
            return;
        }
        // obj_ is non-null only while its widget is alive: the hook clears it
        // first. After lv_deinit() nothing is left to unhook from.
        if (obj_ != nullptr && lv_is_initialized()) {
            lv_obj_remove_event_cb_with_user_data(obj_, on_deleted, this);
        }
        obj_ = obj;
        if (obj_ != nullptr) {
            lv_obj_add_event_cb(obj_, on_deleted, LV_EVENT_DELETE, this);
        }
    }

    [[nodiscard]] lv_obj_t* get() const {
        return obj_;
    }
    operator lv_obj_t*() const {
        return obj_;
    }

  private:
    static void on_deleted(lv_event_t* e) {
        auto* self = static_cast<WidgetRef*>(lv_event_get_user_data(e));
        if (self->obj_ == lv_event_get_target_obj(e)) {
            self->obj_ = nullptr;
        }
    }

    lv_obj_t* obj_ = nullptr;
};

/**
 * @brief The copyable form of WidgetRef, for lambdas, queued callbacks and
 *        containers of value types (prestonbrown/helixscreen#1983)
 *
 * Every WeakWidget naming a widget shares one slot, and one LV_EVENT_DELETE hook
 * on that widget nulls it. The slot outlives the widget for as long as any copy
 * exists, so a copy read after the delete sees nullptr. A slot and a hook cost
 * tens of bytes per widget that has a handle at all, however many copies exist.
 *
 * Threads:
 *  - Making a handle from a pointer, assigning a pointer to one, or calling get()
 *    is LVGL thread only. Making one reads the widget and may hook it, so off the
 *    LVGL thread it logs an error and yields an empty handle: the work bound to
 *    it is dropped, not raced.
 *  - Copying, moving and destroying a handle are safe on any thread. A background
 *    thread that needs a widget holds a handle made on the LVGL thread, and reads
 *    it only in the callback it queues back there.
 *
 * The pointer it is made from must be nullptr or a live widget. A widget already
 * inside its own delete yields an empty handle, because its hook could never fire.
 *
 * Prefer WidgetRef for a member of an owner that does not move: it allocates
 * nothing. Nothing unhooks when the last copy dies; the hook stays until the
 * widget is deleted, so repeatedly making handles to one widget costs one hook.
 */
class WeakWidget {
  public:
    WeakWidget() = default;
    explicit WeakWidget(lv_obj_t* obj);

    WeakWidget& operator=(lv_obj_t* obj) {
        *this = WeakWidget(obj);
        return *this;
    }

    void reset() {
        slot_.reset();
    }

    [[nodiscard]] lv_obj_t* get() const;
    operator lv_obj_t*() const {
        return get();
    }

  private:
    struct Slot;
    static void on_deleted(lv_event_t* e);

    std::shared_ptr<Slot> slot_;
};

} // namespace helix::ui
