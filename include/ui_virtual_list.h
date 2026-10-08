// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <lvgl.h>
#include <sys/types.h>
#include <vector>

namespace helix::ui {

/// Rows [first, last) a virtual list keeps materialised, and the spacer heights that stand in
/// for the rows outside it.
struct VirtualWindow {
    int first = 0;
    int last = 0;
    int leading_px = 0;
    int trailing_px = 0;
};

/// Which rows to materialise for a list scrolled to `scroll_y` in a `viewport_h` tall viewport.
/// `row_stride` is row height plus gap; `overscan` is the extra rows kept above and below.
///
/// A non-empty list always yields a non-empty window, even when scroll_y is past the end (a
/// shrunk list whose scroll position has not been clamped yet) or negative (overscroll). A
/// non-positive stride is treated as 1 px so a not-yet-measured row cannot divide by zero.
inline VirtualWindow compute_window(int scroll_y, int viewport_h, int row_stride, int total_rows,
                                    int overscan) {
    if (total_rows <= 0) {
        return {};
    }
    const int stride = std::max(1, row_stride);
    scroll_y = std::max(0, scroll_y);
    viewport_h = std::max(0, viewport_h);

    VirtualWindow w;
    w.first = std::min(std::max(0, scroll_y / stride - overscan), total_rows - 1);
    w.last = std::min(total_rows, (scroll_y + viewport_h) / stride + 1 + overscan);
    w.leading_px = w.first * stride;
    w.trailing_px = (total_rows - w.last) * stride;
    return w;
}

/// Rows [first, last) wholly inside a `viewport_h` tall viewport scrolled to `scroll_y`, for
/// fixed-stride rows that each sit below a `row_gap` gap: row r spans
/// [r * row_stride + row_gap, (r + 1) * row_stride). `last == first` when none fits.
inline VirtualWindow whole_rows(int scroll_y, int viewport_h, int row_stride, int row_gap,
                                int total_rows) {
    const int stride = std::max(1, row_stride);
    const int top = scroll_y - row_gap;
    const int bottom = scroll_y + std::max(0, viewport_h);
    // Division truncates toward zero: for a negative top that is already the ceiling.
    VirtualWindow w;
    w.first = std::clamp(top > 0 ? (top + stride - 1) / stride : 0, 0, std::max(0, total_rows));
    w.last = std::clamp(bottom > 0 ? bottom / stride : 0, w.first, std::max(w.first, total_rows));
    return w;
}

/// compute_window for rows of differing heights. `row_tops` holds total_rows + 1 entries:
/// row i spans [row_tops[i], row_tops[i + 1]) with its gap included, and row_tops.back() is
/// the whole list's height. Same guarantees as the fixed-stride form.
inline VirtualWindow compute_window(int scroll_y, int viewport_h, const std::vector<int>& row_tops,
                                    int overscan) {
    const int total_rows = static_cast<int>(row_tops.size()) - 1;
    if (total_rows <= 0) {
        return {};
    }
    scroll_y = std::max(0, scroll_y);
    const int bottom = scroll_y + std::max(0, viewport_h);
    const auto rows_end = row_tops.begin() + total_rows;
    // Rows whose top is at or above a y: the row containing y is the last of them.
    auto rows_starting_by = [&](int y) {
        return static_cast<int>(std::upper_bound(row_tops.begin(), rows_end, y) - row_tops.begin());
    };

    VirtualWindow w;
    w.first = std::min(std::max(0, rows_starting_by(scroll_y) - 1 - overscan), total_rows - 1);
    w.last = std::min(total_rows, rows_starting_by(bottom) + overscan);
    w.leading_px = row_tops[static_cast<size_t>(w.first)];
    w.trailing_px = row_tops.back() - row_tops[static_cast<size_t>(w.last)];
    return w;
}

/// Apply `w`'s spacer heights, touching LVGL only when a height changed (`last_*` cache the
/// previous values), and keep the leading spacer first and the trailing spacer last among
/// the container's children.
inline void sync_list_spacers(lv_obj_t* container, lv_obj_t* leading, lv_obj_t* trailing,
                              const VirtualWindow& w, int& last_leading, int& last_trailing) {
    if (leading) {
        if (w.leading_px != last_leading) {
            lv_obj_set_height(leading, w.leading_px);
            last_leading = w.leading_px;
        }
        if (lv_obj_get_index(leading) != 0) {
            lv_obj_move_to_index(leading, 0);
        }
    }
    if (trailing) {
        if (w.trailing_px != last_trailing) {
            lv_obj_set_height(trailing, w.trailing_px);
            last_trailing = w.trailing_px;
        }
        int32_t last_index = static_cast<int32_t>(lv_obj_get_child_count(container)) - 1;
        if (lv_obj_get_index(trailing) != last_index) {
            lv_obj_move_to_index(trailing, last_index);
        }
    }
}

/// Which pool slot shows each item of [first, last), at most one per slot. A slot already
/// showing one of them keeps it, so scrolling a row out and another in touches two slots,
/// not all of them. `slot_items[s]` is the item slot `s` shows (-1 for none) and is updated;
/// slots left without an item read -1. Returns the slot for each window position in order.
inline std::vector<size_t> assign_pool_slots(std::vector<ssize_t>& slot_items, int first,
                                             int last) {
    const size_t n = std::min(slot_items.size(), static_cast<size_t>(std::max(0, last - first)));
    constexpr size_t kNone = static_cast<size_t>(-1);
    std::vector<size_t> order(n, kNone);
    std::vector<bool> taken(slot_items.size(), false);
    for (size_t s = 0; s < slot_items.size(); s++) {
        const ssize_t item = slot_items[s];
        if (item >= first && item < first + static_cast<ssize_t>(n) &&
            order[static_cast<size_t>(item - first)] == kNone) {
            order[static_cast<size_t>(item - first)] = s;
            taken[s] = true;
        }
    }
    size_t free_slot = 0;
    for (size_t k = 0; k < n; k++) {
        if (order[k] != kNone)
            continue;
        while (taken[free_slot])
            free_slot++;
        order[k] = free_slot;
        taken[free_slot] = true;
    }
    for (size_t s = 0; s < slot_items.size(); s++)
        slot_items[s] = taken[s] ? slot_items[s] : -1;
    for (size_t k = 0; k < n; k++)
        slot_items[order[k]] = first + static_cast<ssize_t>(k);
    return order;
}

/// Shows items [first, last) of a virtual list through its pool of slots, placed after
/// the leading spacer in item order. `slot_obj(s)` is a slot's widget, `configure(s, item)`
/// fills a slot whose item changed (every shown slot when `refill_all`), and `park(s)` hides
/// a slot left with nothing to show.
template <typename SlotObj, typename Configure, typename Park>
void show_window(lv_obj_t* container, std::vector<ssize_t>& slot_items, int first, int last,
                 bool refill_all, SlotObj slot_obj, Configure configure, Park park) {
    const std::vector<ssize_t> before = slot_items;
    const std::vector<size_t> order = assign_pool_slots(slot_items, first, last);
    for (size_t k = 0; k < order.size(); k++) {
        const size_t s = order[k];
        if (refill_all || before[s] != slot_items[s])
            configure(s, slot_items[s]);
    }
    // Reordering moves no pixel by itself: the next layout invalidates each slot that
    // lands somewhere new. Left on, every move would redraw the whole list.
    lv_display_t* disp = lv_obj_get_display(container);
    lv_display_enable_invalidation(disp, false);
    for (size_t k = 0; k < order.size(); k++) {
        lv_obj_t* obj = slot_obj(order[k]);
        const int32_t index = static_cast<int32_t>(k) + 1;
        if (lv_obj_get_index(obj) != index)
            lv_obj_move_to_index(obj, index);
    }
    lv_display_enable_invalidation(disp, true);
    for (size_t s = 0; s < slot_items.size(); s++) {
        if (slot_items[s] < 0)
            park(s);
    }
}

} // namespace helix::ui
