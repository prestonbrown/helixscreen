// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Object flags that LVGL 9.6 gives no per-flag function. Built-in flags have their own
// setter and getter (lv_obj_set_hidden(), lv_obj_is_clickable(), ...) and are called
// directly. The LV_OBJ_FLAG_USER_n flags, which carry our own markers (kDragSurfaceFlag,
// kPanelWidgetTileFlag, kEditClickSuppressedFlag), and a flag chosen at run time are
// reachable only through the generic calls LVGL deprecates, so they go through here.

#include "lvgl/lvgl.h"

namespace helix::ui {

/// Set or clear @p flags on @p obj; an OR-ed mask sets or clears every bit in it.
inline void obj_set_flags(lv_obj_t* obj, lv_obj_flag_t flags, bool on) {
    LV_DEPRECATIONS_IGNORE_BEGIN
    lv_obj_set_flag(obj, flags, on); // LV_DEPRECATED_OK: no per-flag setter for USER_n
    LV_DEPRECATIONS_IGNORE_END
}

/// Whether @p obj has every bit of @p flags set.
[[nodiscard]] inline bool obj_has_flags(const lv_obj_t* obj, lv_obj_flag_t flags) {
    LV_DEPRECATIONS_IGNORE_BEGIN
    return lv_obj_has_flag(obj, flags); // LV_DEPRECATED_OK: no per-flag getter for USER_n
    LV_DEPRECATIONS_IGNORE_END
}

} // namespace helix::ui
