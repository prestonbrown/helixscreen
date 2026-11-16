// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include "core/update_queue.h"
#include "lvgl/lvgl.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// Press @p button_name ("btn_primary", "btn_secondary") on the top dialog.
inline void answer_top_modal(const char* button_name) {
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* button = lv_obj_find_by_name(dialog, button_name);
    REQUIRE(button != nullptr);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    ui::UpdateQueue::instance().drain();
}

/// Tap the top dialog's backdrop: the dismissal no button answers.
inline void dismiss_top_modal() {
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
    ui::UpdateQueue::instance().drain();
}

/// Every label's text under @p obj, one per line, in tree order.
inline std::string subtree_text(lv_obj_t* obj) {
    std::string text;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        text += lv_label_get_text(obj);
        text += "\n";
    }
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        text += subtree_text(lv_obj_get_child(obj, static_cast<int32_t>(i)));
    }
    return text;
}

/// The text of the top dialog, or empty when no dialog is up.
inline std::string top_modal_text() {
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    return dialog ? subtree_text(dialog) : std::string{};
}

} // namespace helix::test
