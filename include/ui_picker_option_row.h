// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_choice_subject.h"
#include "ui_event_safety.h"
#include "ui_row_text.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace helix::ui {

/// Creates a picker_option_row in @p list labelled @p label, lit while the choice_subject
/// @p choice holds @p index, that runs @p on_pick when clicked. The callback is owned by
/// the row and freed with it, and the row's own user data is left for the caller.
/// @p extra_attrs are further XML attribute name/value pairs. Returns nullptr when the
/// component cannot be created.
inline lv_obj_t* create_picker_option_row(lv_obj_t* list, const char* label, const char* choice,
                                          int index, std::function<void()> on_pick,
                                          std::initializer_list<const char*> extra_attrs = {}) {
    choice_subject(choice);
    const std::string value = std::to_string(index);
    std::vector<const char*> attrs = {"selected_subject", choice, "selected_value", value.c_str()};
    attrs.insert(attrs.end(), extra_attrs.begin(), extra_attrs.end());
    attrs.push_back(nullptr);
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(list, "picker_option_row", attrs.data()));
    if (!row)
        return nullptr;
    set_row_label_text(row, "option_label", label);
    auto* payload = new std::function<void()>(std::move(on_pick));
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            delete static_cast<std::function<void()>*>(lv_event_get_user_data(e));
        },
        LV_EVENT_DELETE, payload);
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("[picker_option_row] clicked");
            // A copy: a pick that repopulates the list deletes this row and its callback.
            const std::function<void()> on_pick =
                *static_cast<std::function<void()>*>(lv_event_get_user_data(e));
            if (on_pick)
                on_pick();
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, payload);
    return row;
}

} // namespace helix::ui
