// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

namespace helix::test {

/// Whether @p label is capped at one line with lv_label_set_max_lines() and
/// sized by its content, so the cap decides its height. An explicit height or a
/// max_height would make the cap do nothing, so either fails the check.
inline bool held_to_one_line(lv_obj_t* label) {
    return label && lv_label_get_max_lines(label) == 1 &&
           lv_obj_get_style_height(label, LV_PART_MAIN) == LV_SIZE_CONTENT &&
           lv_obj_get_style_max_height(label, LV_PART_MAIN) == LV_COORD_MAX;
}

/// First child of @p parent that is a label in DOTS mode, or nullptr.
inline lv_obj_t* find_dotted_label(lv_obj_t* parent) {
    if (!parent)
        return nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t* child = lv_obj_get_child(parent, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class) &&
            lv_label_get_long_mode(child) == LV_LABEL_LONG_MODE_DOTS)
            return child;
        if (lv_obj_t* nested = find_dotted_label(child))
            return nested;
    }
    return nullptr;
}

} // namespace helix::test
