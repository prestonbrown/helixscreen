// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_icon_picker.h"

#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"

#include "theme_manager.h"

#include <string>

namespace helix::ui {

namespace {

constexpr int kIconCellSize = 36;

/// What each cell carries: which icon it is and what a tap does.
struct IconCell {
    std::string name;
    std::function<void(const char*)> on_pick;
};

IconCell* cell_data(lv_obj_t* cell) {
    return static_cast<IconCell*>(lv_obj_get_user_data(cell));
}

// DECLARATIVE_OK: the cells are created in C++, so their selection border has no
// XML layer to bind to.
void apply_highlight(lv_obj_t* cell, bool selected) {
    if (selected) {
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, theme_manager_get_color("primary"), 0);
        lv_obj_set_style_bg_opa(cell, 20, 0);
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("primary"), 0);
    } else {
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, 0, 0);
    }
}

} // namespace

void populate_icon_grid(lv_obj_t* grid, const char* const* icons, size_t count,
                        std::string_view selected, std::function<void(const char*)> on_pick) {
    if (!grid) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t* cell = lv_obj_create(grid);
        lv_obj_set_size(cell, kIconCellSize, kIconCellSize);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(cell, 0, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
        apply_highlight(cell, selected == icons[i]);

        if (const char* cp = icon::lookup_codepoint(icons[i])) {
            lv_obj_t* glyph = lv_label_create(cell);
            lv_label_set_text(glyph, cp);
            lv_obj_set_style_text_font(glyph, &mdi_icons_24, 0);
            lv_obj_set_style_text_color(glyph, theme_manager_get_color("text"), 0);
            lv_obj_center(glyph);
            lv_obj_remove_flag(glyph, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(glyph, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        // The DELETE hook owns the allocation through its own user_data. The
        // object's user_data slot is only the lookup the click and refresh paths
        // use, and a closing modal nulls every slot in its tree to stop dispatch.
        auto* data = new IconCell{icons[i], on_pick};
        lv_obj_set_user_data(cell, data);
        lv_obj_add_event_cb(
            cell,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[IconPicker] cell_clicked");
                if (auto* d = cell_data(lv_event_get_current_target_obj(e)); d && d->on_pick) {
                    d->on_pick(d->name.c_str());
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(
            cell,
            [](lv_event_t* e) {
                lv_obj_set_user_data(lv_event_get_current_target_obj(e), nullptr);
                delete static_cast<IconCell*>(lv_event_get_user_data(e));
            },
            LV_EVENT_DELETE, data);
    }
}

void refresh_icon_grid(lv_obj_t* grid, std::string_view selected) {
    if (!grid) {
        return;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i) {
        lv_obj_t* cell = lv_obj_get_child(grid, i);
        if (auto* d = cell_data(cell)) {
            apply_highlight(cell, d->name == selected);
        }
    }
}

} // namespace helix::ui
