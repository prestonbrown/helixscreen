// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_component_keypad.h"

#include "core/update_queue.h"
#include "lvgl/lvgl.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// Closes any open keypad, then taps the field named @p name to open its own.
inline void keypad_open_from(const char* name) {
    ui_keypad_hide();
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* field = lv_obj_find_by_name(lv_screen_active(), name);
    REQUIRE(field != nullptr);
    lv_obj_send_event(field, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
}

/// The open keypad's reset key, which types the field's default.
inline lv_obj_t* keypad_reset_key() {
    lv_obj_t* keypad = lv_obj_find_by_name(lv_screen_active(), "keypad_panel");
    REQUIRE(keypad != nullptr);
    REQUIRE(ui_keypad_is_visible());
    lv_obj_t* key = lv_obj_find_by_name(keypad, "btn_default");
    REQUIRE(key != nullptr);
    return key;
}

/// Whether the open keypad offers a reset key.
inline bool keypad_reset_key_shown() {
    return !lv_obj_is_hidden(keypad_reset_key());
}

/// Presses the open keypad's reset key and returns what its display then reads.
inline std::string keypad_press_reset() {
    lv_obj_send_event(keypad_reset_key(), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    return lv_subject_get_string(ui_keypad_get_display_subject());
}

} // namespace helix::test
