// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_icon_picker.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_icon_codepoints.h"

#include <string>
#include <vector>

namespace helix::ui {

namespace {

/// What a grid carries: the icon each cell index names and what a tap does.
struct IconGrid {
    std::vector<std::string> names;
    std::function<void(const char*)> on_pick;
};

IconGrid* grid_data(lv_obj_t* grid) {
    return grid ? static_cast<IconGrid*>(lv_obj_get_user_data(grid)) : nullptr;
}

void cell_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[IconPicker] cell_clicked");
    auto* d = grid_data(lv_obj_get_parent(lv_event_get_current_target_obj(e)));
    const auto index = event_user_int(e);
    if (d && d->on_pick && index && *index >= 0 && static_cast<size_t>(*index) < d->names.size()) {
        d->on_pick(d->names[*index].c_str());
    }
    LVGL_SAFE_EVENT_CB_END();
}

/// The grid's lookup record, made on first use. The DELETE hook owns the
/// allocation through its own user_data. The grid's user_data slot is only the
/// lookup the click and refresh paths use, and a closing modal nulls every slot
/// in its tree to stop dispatch.
IconGrid* ensure_grid_data(lv_obj_t* grid) {
    if (auto* d = grid_data(grid)) {
        return d;
    }
    auto* d = new IconGrid{};
    lv_obj_set_user_data(grid, d);
    lv_obj_add_event_cb(
        grid,
        [](lv_event_t* e) {
            lv_obj_set_user_data(lv_event_get_current_target_obj(e), nullptr);
            delete static_cast<IconGrid*>(lv_event_get_user_data(e));
        },
        LV_EVENT_DELETE, d);
    return d;
}

} // namespace

void populate_icon_grid(lv_obj_t* grid, const char* const* icons, size_t count,
                        std::string_view selected, std::function<void(const char*)> on_pick) {
    if (!grid) {
        return;
    }
    lv_xml_register_event_cb(nullptr, "icon_picker_cell_cb", cell_clicked);
    IconGrid* d = ensure_grid_data(grid);
    d->on_pick = std::move(on_pick);
    // Cell i names names[i]: a grid cleaned for a refill starts the list over.
    if (lv_obj_get_child_count(grid) == 0) {
        d->names.clear();
    }
    for (size_t i = 0; i < count; ++i) {
        const std::string index = std::to_string(d->names.size());
        const char* glyph = icon::lookup_codepoint(icons[i]);
        const char* attrs[] = {"index",    index.c_str(),
                               "glyph",    glyph ? glyph : "",
                               "selected", selected == icons[i] ? "true" : "false",
                               nullptr};
        if (lv_xml_create(grid, "icon_picker_cell", attrs)) {
            d->names.emplace_back(icons[i]);
        }
    }
}

void refresh_icon_grid(lv_obj_t* grid, std::string_view selected) {
    const IconGrid* d = grid_data(grid);
    if (!d) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(grid);
    for (uint32_t i = 0; i < n && i < d->names.size(); ++i) {
        lv_obj_set_state(lv_obj_get_child(grid, static_cast<int32_t>(i)), LV_STATE_CHECKED,
                         d->names[i] == selected);
    }
}

} // namespace helix::ui
