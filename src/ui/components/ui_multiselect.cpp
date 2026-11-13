// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_multiselect.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

UiMultiselect::UiMultiselect() = default;

UiMultiselect::~UiMultiselect() {
    detach();
}

UiMultiselect::UiMultiselect(UiMultiselect&& other) noexcept
    : container_(other.container_), rows_(std::move(other.rows_)),
      on_change_(std::move(other.on_change_)), max_selected_(other.max_selected_) {
    other.container_ = nullptr;
    // Update back-pointers
    for (auto& row : rows_) {
        row->owner = this;
    }
}

UiMultiselect& UiMultiselect::operator=(UiMultiselect&& other) noexcept {
    if (this != &other) {
        detach();
        container_ = other.container_;
        rows_ = std::move(other.rows_);
        on_change_ = std::move(other.on_change_);
        max_selected_ = other.max_selected_;
        other.container_ = nullptr;
        for (auto& row : rows_) {
            row->owner = this;
        }
    }
    return *this;
}

void UiMultiselect::attach(lv_obj_t* container) {
    if (container_) {
        detach();
    }
    container_ = container;
}

void UiMultiselect::detach() {
    if (container_) {
        clear_rows();
        container_ = nullptr;
    }
}

void UiMultiselect::set_items(const std::vector<MultiSelectItem>& items) {
    if (!container_) {
        spdlog::warn("[UiMultiselect] set_items() called without attached container");
        return;
    }

    clear_rows();

    if (!select_all_label_.empty()) {
        all_row_ = build_row({"all", select_all_label_});
        lv_obj_set_style_border_side(all_row_->row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(all_row_->row, 1, 0);
        lv_obj_set_style_border_color(all_row_->row, theme_manager_get_color("border"), 0);
    }

    for (size_t i = 0; i < items.size(); ++i) {
        auto* row_obj = create_row(items[i]);
        // Add bottom border as divider (except last row)
        if (i < items.size() - 1) {
            lv_obj_set_style_border_side(row_obj, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row_obj, 1, 0);
            lv_obj_set_style_border_color(row_obj, theme_manager_get_color("border"), 0);
        }
    }
}

lv_obj_t* UiMultiselect::create_row(const MultiSelectItem& item) {
    rows_.push_back(build_row(item));
    return rows_.back()->row;
}

std::unique_ptr<UiMultiselect::RowData> UiMultiselect::build_row(const MultiSelectItem& item) {
    auto data = std::make_unique<RowData>();
    data->key = item.key.empty() ? item.label : item.key;
    data->label = item.label;
    data->selected = item.selected && item.enabled;
    data->enabled = item.enabled;
    data->owner = this;

    // Row container: flex row, full width, content height
    lv_obj_t* row = lv_obj_create(container_);
    lv_obj_set_name(row, fmt::format("item_{}", data->key).c_str());
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Padding from theme
    int pad = theme_manager_get_spacing("space_md");
    lv_obj_set_style_pad_left(row, pad, 0);
    lv_obj_set_style_pad_right(row, pad, 0);
    lv_obj_set_style_pad_top(row, pad, 0);
    lv_obj_set_style_pad_bottom(row, pad, 0);
    lv_obj_set_style_pad_gap(row, pad, 0);

    // Transparent background, no border by default
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_scrollable(row, false);

    // Label: flex grow to fill available space
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, item.label.c_str());
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_text_color(label,
                                theme_manager_get_color(item.enabled ? "text" : "text_muted"), 0);

    // Checkbox: right-aligned, empty text, not directly clickable
    lv_obj_t* cb = lv_checkbox_create(row);
    lv_obj_set_name(cb, "check");
    lv_checkbox_set_text(cb, "");
    lv_obj_set_clickable(cb, false);

    // Set initial state
    if (data->selected) {
        lv_obj_add_state(cb, LV_STATE_CHECKED);
    }
    if (!item.enabled) {
        lv_obj_add_state(cb, LV_STATE_DISABLED);
        lv_obj_set_clickable(row, false);
    }

    data->row = row;
    data->checkbox = cb;

    // Store data pointer in row's user_data for the click callback
    RowData* raw_ptr = data.get();
    lv_obj_set_user_data(row, raw_ptr);

    // Row click toggles selection
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, nullptr);

    return data;
}

void UiMultiselect::sync_all_row() {
    if (!all_row_) {
        return;
    }
    bool any_enabled = false;
    bool every = true;
    for (const auto& row : rows_) {
        if (row->enabled) {
            any_enabled = true;
            every = every && row->selected;
        }
    }
    all_row_->selected = any_enabled && every;
    update_checkbox_visual(all_row_.get());
}

void UiMultiselect::on_row_clicked(lv_event_t* e) {
    lv_obj_t* row = lv_event_get_target_obj(e);
    auto* data = static_cast<RowData*>(lv_obj_get_user_data(row));
    if (!data || !data->owner)
        return;

    if (!data->enabled)
        return;
    UiMultiselect* owner = data->owner;
    if (data == owner->all_row_.get()) {
        if (data->selected) {
            owner->deselect_all();
        } else {
            owner->select_all();
        }
        return;
    }
    if (!data->selected && owner->max_selected_ > 0 &&
        owner->get_selected_count() >= owner->max_selected_) {
        if (owner->max_selected_ > 1)
            return;
        for (auto& other : owner->rows_) {
            if (other.get() != data && other->selected) {
                other->selected = false;
                owner->update_checkbox_visual(other.get());
                if (owner->on_change_)
                    owner->on_change_(other->key, false);
            }
        }
    }

    data->selected = !data->selected;
    data->owner->update_checkbox_visual(data);

    owner->sync_all_row();

    if (data->owner->on_change_) {
        data->owner->on_change_(data->key, data->selected);
    }
}

void UiMultiselect::update_checkbox_visual(RowData* data) {
    if (!data || !data->checkbox)
        return;
    if (data->selected) {
        lv_obj_add_state(data->checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(data->checkbox, LV_STATE_CHECKED);
    }
}

void UiMultiselect::clear_rows() {
    // Clear the vector first (frees RowData), then clean LVGL children
    rows_.clear();
    all_row_.reset();
    if (container_) {
        lv_obj_clean(container_);
    }
}

std::vector<MultiSelectItem> UiMultiselect::get_items() const {
    std::vector<MultiSelectItem> items;
    items.reserve(rows_.size());
    for (const auto& row : rows_) {
        items.push_back({row->key, row->label, row->selected, row->enabled});
    }
    return items;
}

std::vector<std::string> UiMultiselect::get_selected_keys() const {
    std::vector<std::string> keys;
    for (const auto& row : rows_) {
        if (row->selected) {
            keys.push_back(row->key);
        }
    }
    return keys;
}

size_t UiMultiselect::get_selected_count() const {
    size_t count = 0;
    for (const auto& row : rows_) {
        if (row->selected)
            ++count;
    }
    return count;
}

void UiMultiselect::select_all() {
    for (auto& row : rows_) {
        if (!row->selected && row->enabled) {
            row->selected = true;
            update_checkbox_visual(row.get());
            if (on_change_)
                on_change_(row->key, true);
        }
    }
    sync_all_row();
}

void UiMultiselect::deselect_all() {
    for (auto& row : rows_) {
        if (row->selected) {
            row->selected = false;
            update_checkbox_visual(row.get());
            if (on_change_)
                on_change_(row->key, false);
        }
    }
    sync_all_row();
}

bool UiMultiselect::set_selected(const std::string& key, bool selected) {
    for (auto& row : rows_) {
        if (row->key == key) {
            if (row->selected != selected) {
                row->selected = selected;
                update_checkbox_visual(row.get());
                if (on_change_)
                    on_change_(key, selected);
                sync_all_row();
            }
            return true;
        }
    }
    return false;
}

void UiMultiselect::set_on_change(MultiSelectCallback cb) {
    on_change_ = std::move(cb);
}

} // namespace helix::ui
