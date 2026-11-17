// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screen_secrets.h"

#include <algorithm>
#include <vector>

namespace helix::ui {

namespace {

std::vector<lv_obj_t*>& marked() {
    static std::vector<lv_obj_t*> objs;
    return objs;
}

void on_marked_deleted(lv_event_t* e) {
    auto* obj = lv_event_get_target_obj(e);
    auto& objs = marked();
    objs.erase(std::remove(objs.begin(), objs.end(), obj), objs.end());
}

} // namespace

void mark_screen_secret(lv_obj_t* obj) {
    if (!obj) {
        return;
    }
    auto& objs = marked();
    if (std::find(objs.begin(), objs.end(), obj) != objs.end()) {
        return;
    }
    objs.push_back(obj);
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup; a dangling pointer here would be read on
    // every flush.
    lv_obj_add_event_cb(obj, on_marked_deleted, LV_EVENT_DELETE, nullptr);
}

ScreenSecret screen_secret_state() {
    if (!lv_display_get_default()) {
        return ScreenSecret::Unknown;
    }
    for (lv_obj_t* obj : marked()) {
        if (lv_obj_is_visible(obj)) {
            return ScreenSecret::Shown;
        }
    }
    return ScreenSecret::Absent;
}

} // namespace helix::ui
