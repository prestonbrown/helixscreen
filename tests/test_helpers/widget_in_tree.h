// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "lvgl/src/display/lv_display_private.h"

namespace helix::test {

inline bool subtree_holds(const lv_obj_t* root, const lv_obj_t* obj) {
    if (root == obj) {
        return true;
    }
    const uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        if (subtree_holds(lv_obj_get_child(root, static_cast<int32_t>(i)), obj)) {
            return true;
        }
    }
    return false;
}

/// Whether some display's tree holds @p obj at this address, for a test asserting
/// a pointer it kept was or was not freed. It never reads through @p obj, so a freed
/// pointer is safe to pass. App code holds a helix::ui::WidgetRef or WeakWidget instead:
/// a new widget at a freed address answers true here.
inline bool widget_in_tree(const lv_obj_t* obj) {
    if (!obj) {
        return false;
    }
    for (lv_display_t* d = lv_display_get_next(nullptr); d; d = lv_display_get_next(d)) {
        for (uint32_t i = 0; i < d->screen_cnt; ++i) {
            if (subtree_holds(d->screens[i], obj)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace helix::test
