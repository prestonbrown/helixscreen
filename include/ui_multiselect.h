// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_multiselect.h
 * @brief Reusable multi-select list widget with checkboxes
 *
 * Provides a vertical list of clickable rows, each with a label and
 * right-aligned checkbox. Items are set imperatively. The widget itself
 * doesn't scroll — consumers wrap it in a scrollable container.
 *
 * Usage:
 *   // In XML: place a ui_multiselect container
 *   // In C++:
 *   UiMultiselect multiselect;
 *   multiselect.attach(container_obj);
 *   multiselect.set_items({{"key1", "Label 1"}, {"key2", "Label 2", true}});
 *   multiselect.set_on_change([](const std::string& key, bool selected) { ... });
 */

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Item descriptor for multi-select list
 */
struct MultiSelectItem {
    std::string key;       ///< Programmatic key (defaults to label if empty)
    std::string label;     ///< Display text
    bool selected = false; ///< Initial selection state
    bool enabled = true;   ///< A disabled row shows dimmed and ignores taps
};

/**
 * @brief Callback fired when any item's selection changes
 * @param key The item key
 * @param selected Whether the item is now selected
 */
using MultiSelectCallback = std::function<void(const std::string& key, bool selected)>;

/**
 * @brief Reusable multi-select list widget
 *
 * Creates clickable rows with labels and checkboxes inside a container.
 * The entire row is the click target for better touch interaction.
 */
class UiMultiselect {
  public:
    UiMultiselect();
    ~UiMultiselect();

    // Non-copyable (owns LVGL objects)
    UiMultiselect(const UiMultiselect&) = delete;
    UiMultiselect& operator=(const UiMultiselect&) = delete;

    UiMultiselect(UiMultiselect&& other) noexcept;
    UiMultiselect& operator=(UiMultiselect&& other) noexcept;

    /**
     * @brief Attach to an LVGL container object
     * @param container The lv_obj that will hold the rows
     */
    void attach(lv_obj_t* container);

    /**
     * @brief Detach from the current container
     *
     * Clears all rows and releases the container reference.
     */
    void detach();

    /**
     * @brief Set or replace all items (recreates rows)
     * @param items Vector of items to display
     *
     * If an item's key is empty, the label is used as the key.
     */
    void set_items(const std::vector<MultiSelectItem>& items);

    /// Get all items with current selection state
    std::vector<MultiSelectItem> get_items() const;

    /// Get keys of all selected items
    std::vector<std::string> get_selected_keys() const;

    /// Get count of selected items
    size_t get_selected_count() const;

    void select_all();
    void deselect_all();

    /**
     * @brief Set selection state for a specific item
     * @param key Item key to update
     * @param selected New selection state
     * @return true if item was found
     */
    bool set_selected(const std::string& key, bool selected);

    /**
     * @brief Register callback for selection changes
     * @param cb Callback function (key, selected)
     */
    void set_on_change(MultiSelectCallback cb);

    /**
     * @brief Cap how many rows may be selected at once (0 = no cap)
     *
     * A tap past a cap of one moves the selection to the tapped row; past a
     * larger cap it is ignored.
     */
    void set_max_selected(size_t max) {
        max_selected_ = max;
    }

    /// Whether the widget is attached to a container
    bool is_attached() const {
        return container_ != nullptr;
    }

    /// Number of items
    size_t item_count() const {
        return rows_.size();
    }

  private:
    struct RowData {
        std::string key;
        std::string label;
        bool selected = false;
        bool enabled = true;
        lv_obj_t* row = nullptr;
        lv_obj_t* checkbox = nullptr;
        UiMultiselect* owner = nullptr; // Back-pointer for static callback
    };

    lv_obj_t* container_ = nullptr;
    std::vector<std::unique_ptr<RowData>> rows_;
    MultiSelectCallback on_change_;
    size_t max_selected_ = 0;

    lv_obj_t* create_row(const MultiSelectItem& item);
    void clear_rows();
    void update_checkbox_visual(RowData* data);

    static void on_row_clicked(lv_event_t* e);
};

} // namespace helix::ui
