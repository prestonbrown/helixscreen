// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_multiselect.h"

#include "ui_row_text.h"

#include "helix-xml/src/xml/lv_xml.h"

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
        all_row_ = build_row({"all", select_all_label_}, true);
    }

    for (size_t i = 0; i < items.size(); ++i) {
        // A hairline under every row but the last
        rows_.push_back(build_row(items[i], i + 1 < items.size()));
    }
}

std::unique_ptr<UiMultiselect::RowData> UiMultiselect::build_row(const MultiSelectItem& item,
                                                                 bool divider) {
    auto data = std::make_unique<RowData>();
    data->key = item.key.empty() ? item.label : item.key;
    data->label = item.label;
    data->selected = item.selected && item.enabled;
    data->enabled = item.enabled;
    data->owner = this;

    const std::string name = fmt::format("item_{}", data->key);
    const char* attrs[] = {
        "name",          name.c_str(),
        "checked",       data->selected ? "true" : "false",
        "disabled",      item.enabled ? "false" : "true",
        "clickable",     item.enabled ? "true" : "false",
        "text_color",    item.enabled ? "#text" : "#text_muted",
        "divider_width", divider ? "1" : "0",
        nullptr,
    };
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(container_, "multiselect_row", attrs));
    if (!row) {
        spdlog::error("[UiMultiselect] multiselect_row failed to build for '{}'", data->key);
        return data;
    }
    // A label is user text: as a prop a leading '#' would read as a const.
    set_row_label_text(row, "label", item.label.c_str());
    lv_obj_t* cb = lv_obj_find_by_name(row, "check");

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
