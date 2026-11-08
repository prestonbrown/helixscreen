// tests/test_helpers/device_action_controls.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reaching the control the AMS section overlay built for a DeviceAction.
#pragma once

#include <lvgl/lvgl.h>

namespace helix::test {

/// The first object of class @p cls in @p obj's subtree, @p obj included.
inline lv_obj_t* find_first_of_class(lv_obj_t* obj, const lv_obj_class_t* cls) {
    if (!obj || lv_obj_check_type(obj, cls)) {
        return obj;
    }
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        if (lv_obj_t* found =
                find_first_of_class(lv_obj_get_child(obj, static_cast<int32_t>(i)), cls)) {
            return found;
        }
    }
    return nullptr;
}

/// The widget of class @p cls the section overlay built for action @p id, found
/// under the object named for the action.
inline lv_obj_t* find_device_action_control(lv_obj_t* root, const char* id,
                                            const lv_obj_class_t* cls) {
    return find_first_of_class(lv_obj_find_by_name(root, id), cls);
}

inline lv_obj_t* find_device_action_slider(lv_obj_t* root, const char* id) {
    return find_device_action_control(root, id, &lv_slider_class);
}

} // namespace helix::test
