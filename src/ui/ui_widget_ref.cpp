// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_widget_ref.h"

#include "ui_update_queue.h"

#include "lvgl/src/core/lv_obj_private.h" // obj->is_deleting: LVGL has no getter

#include <spdlog/spdlog.h>

namespace helix::ui {

void WidgetRef::reset(lv_obj_t* obj) {
    if (obj && obj->is_deleting) {
        obj = nullptr;
    }
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

struct WeakWidget::Slot {
    lv_obj_t* obj = nullptr;
    // The hook's share of the slot, dropped when the widget dies. A slot naming a
    // live widget therefore stays findable with no copy of the handle left.
    std::shared_ptr<Slot> hook_share;
};

void WeakWidget::on_deleted(lv_event_t* e) {
    auto* slot = static_cast<Slot*>(lv_event_get_user_data(e));
    slot->obj = nullptr;
    // Moved out first: this can be the last share, and it frees the slot it sits in.
    const std::shared_ptr<Slot> last_share = std::move(slot->hook_share);
}

WeakWidget::WeakWidget(lv_obj_t* obj) {
    if (!obj) {
        return;
    }
    if (!UpdateQueue::is_main_thread()) {
        spdlog::error("[WeakWidget] Made off the LVGL thread for {}; the handle is empty",
                      static_cast<void*>(obj));
        return;
    }
    if (!lv_is_initialized() || obj->is_deleting) {
        return;
    }
    const uint32_t count = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc && lv_event_dsc_get_cb(dsc) == on_deleted) {
            slot_ = static_cast<Slot*>(lv_event_dsc_get_user_data(dsc))->hook_share;
            return;
        }
    }
    slot_ = std::make_shared<Slot>();
    slot_->obj = obj;
    slot_->hook_share = slot_;
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(obj, on_deleted, LV_EVENT_DELETE, slot_.get());
}

lv_obj_t* WeakWidget::get() const {
    return slot_ ? slot_->obj : nullptr;
}

} // namespace helix::ui
