// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ams_mini_test_helpers.h
 * @brief Finding the AMS mini strip's lane bars, wherever its bar view put them
 * (in a unit box, on a line, or parked hidden in the bars container).
 */

#include "ui_ams_lane_bar.h"

#include "../ui_test_utils.h"

#include <vector>

namespace ams_mini_test {

/// The lane bar bound to @p slot under @p root, or nullptr.
inline lv_obj_t* lane_bar_for_slot(lv_obj_t* root, int slot) {
    lv_obj_t* bars = UITest::find_by_name(root, "ams_bars_container");
    if (!bars)
        return nullptr;
    std::vector<lv_obj_t*> stack{bars};
    while (!stack.empty()) {
        lv_obj_t* o = stack.back();
        stack.pop_back();
        if (o != bars && helix::ui::ams_lane_bar_slot_index(o) == slot)
            return o;
        for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i)
            stack.push_back(lv_obj_get_child(o, static_cast<int32_t>(i)));
    }
    return nullptr;
}

/// Whether @p obj has a size and it and every ancestor up to @p root are shown.
inline bool shown_within(lv_obj_t* obj, lv_obj_t* root) {
    if (lv_obj_get_width(obj) <= 0 || lv_obj_get_height(obj) <= 0)
        return false;
    for (lv_obj_t* o = obj; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_is_hidden(o))
            return false;
        if (o == root)
            return true;
    }
    return false;
}

/// The lane bars the bar view shows, in slot order.
inline std::vector<lv_obj_t*> shown_lane_bars(lv_obj_t* root, int slot_count) {
    std::vector<lv_obj_t*> out;
    for (int s = 0; s < slot_count; ++s)
        if (lv_obj_t* bar = lane_bar_for_slot(root, s); bar && shown_within(bar, root))
            out.push_back(bar);
    return out;
}

} // namespace ams_mini_test
