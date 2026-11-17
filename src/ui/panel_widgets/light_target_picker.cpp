// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "light_target_picker.h"

#include "ui_event_safety.h"

#include "core/subject.h"
#include "core/text_io.h"
#include "helix/xml/indexed_subject_pool.h"
#include "led/led_controller.h"
#include "led/led_devices.h"
#include "src/ui/panel_widgets/led_widget.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

/// The picker's rows. One picker is open at a time, so these are process-wide.
struct PickerSubjects {
    xml::IndexedSubjectPool names{"led_picker_name", xml::IndexedSubjectPool::Type::String};
    helix::Subject count{};
    helix::Subject selected{};
};

helix::WidgetSubjects s_picker_subjects{"LedPicker"};

/// Created on the first open, before the picker's XML binds them.
PickerSubjects& picker_subjects() {
    static PickerSubjects s_picker;
    s_picker_subjects.ensure_init(
        [] {
            UI_MANAGED_SUBJECT_INT(s_picker.count, 0, "led_picker_count", s_picker_subjects);
            UI_MANAGED_SUBJECT_INT(s_picker.selected, -1, "led_picker_selected", s_picker_subjects);
        },
        [] { s_picker.names.reclaim(); });
    return s_picker;
}

} // namespace

void register_light_target_picker() {
    lv_xml_register_event_cb(nullptr, "led_picker_row_cb", LightTargetPicker::row_cb);
}

void LightTargetPicker::show_for(lv_obj_t* parent, lv_obj_t* anchor, const std::string& key,
                                 OnPick on_pick) {
    if (is_visible() || parent == nullptr || anchor == nullptr) {
        return;
    }

    auto& subs = picker_subjects();
    const auto devices = light_picker_devices();
    const auto targets = led::LedController::instance().light_targets(key);
    subs.names.ensure_size(devices.size());
    int selected = -1;
    row_ids_.clear();
    for (size_t i = 0; i < devices.size(); ++i) {
        row_ids_.push_back(devices[i].id);
        subs.names.set_string(i, led::device_display_name(devices[i]));
        if (key != led::kLightButtonAll && !targets.empty() && devices[i].id == targets.front()) {
            selected = static_cast<int>(i);
        }
    }
    lv_subject_set_int(&subs.selected, selected);
    lv_subject_set_int(&subs.count, static_cast<int>(devices.size()));

    on_pick_ = std::move(on_pick);
    show_below_widget(parent, anchor, helix::ui::ContextMenu::AnchorAlign::Center);
}

void LightTargetPicker::on_created(lv_obj_t* menu_obj) {
    // DECLARATIVE_OK: measured cap. A share of the screen, so a printer with a
    // dozen lights scrolls the list instead of growing the card past the panel.
    if (lv_obj_t* list = lv_obj_find_by_name(menu_obj, "led_picker_list")) {
        lv_obj_set_style_max_height(list, screen_height_pct(66), 0);
    }
}

void LightTargetPicker::row_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LightTargetPicker] row_cb");
    auto* picker = helix::ui::ContextMenu::active_as<LightTargetPicker>();
    const char* ud = static_cast<const char*>(lv_event_get_user_data(e));
    if (picker == nullptr || ud == nullptr) {
        return;
    }
    const int index = helix::text_io::parse_leading<int>(ud).value_or(-1);
    std::string key = led::kLightButtonAll;
    if (index >= 0 && static_cast<size_t>(index) < picker->row_ids_.size()) {
        key = picker->row_ids_[static_cast<size_t>(index)];
    }
    // hide() may run the owner's teardown; the callback is taken out first.
    OnPick on_pick = std::move(picker->on_pick_);
    picker->hide();
    if (on_pick) {
        on_pick(key);
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
