// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_container_delete_net.h"

#include "print_history_data.h"

#include <functional>
#include <lvgl.h>
#include <vector>

namespace helix::ui {

/**
 * @file ui_history_list_view.h
 * @brief Virtualized list view for print history jobs
 *
 * Manages a fixed pool of history row widgets that are recycled as the user scrolls.
 * Follows the same spacer-based virtualization pattern as SpoolmanListView.
 *
 * ## Key Features:
 * - Fixed widget pool (POOL_SIZE rows created once)
 * - Leading/trailing spacers for smooth scroll virtualization
 * - Imperative row updates (no per-row subjects)
 * - Click callback with data index for detail overlay navigation
 */
class HistoryListView : public ContainerDeleteNet {
  public:
    static constexpr int POOL_SIZE = 30;  ///< Fixed pool of history row widgets
    static constexpr int BUFFER_ROWS = 2; ///< Extra rows above/below viewport

    using RowClickCallback = std::function<void(size_t index)>;

    HistoryListView() = default;
    ~HistoryListView();

    // Non-copyable
    HistoryListView(const HistoryListView&) = delete;
    HistoryListView& operator=(const HistoryListView&) = delete;

    // === Setup / Cleanup ===

    /**
     * @brief Initialize the list view
     * @param container LVGL container where row widgets and spacers are placed
     * @param scroll_parent Scrollable parent to read scroll_y from (may differ from container
     *                      when container is a non-scrollable child of a scrollable parent).
     *                      Pass nullptr to use container as the scroll source.
     * @param on_click Callback invoked when a row is clicked, with data index
     * @return true if setup succeeded
     */
    bool setup(lv_obj_t* container, lv_obj_t* scroll_parent, RowClickCallback on_click);

    /**
     * @brief Clean up pool and spacers
     */
    void cleanup();

    /**
     * @brief Reset visible state without destroying pool/container
     *
     * Used during deactivation to hide rows while keeping the pool intact
     * so reactivation can repopulate without recreating widgets.
     */
    void reset();

    // === Population ===

    /**
     * @brief Populate view with job list
     * @param jobs Job data to display
     * @param preserve_scroll If true, keep current scroll position (for infinite scroll append)
     */
    void populate(const std::vector<PrintHistoryJob>& jobs, bool preserve_scroll = false);

    /**
     * @brief Update visible rows based on scroll position
     * @param jobs Job data vector
     */
    void update_visible(const std::vector<PrintHistoryJob>& jobs);

    /**
     * @brief Refresh content of visible rows without repositioning
     * @param jobs Job data vector
     */
    void refresh_content(const std::vector<PrintHistoryJob>& jobs);

    // === State Queries ===

    [[nodiscard]] bool is_initialized() const {
        return !pool_.empty();
    }

    [[nodiscard]] lv_obj_t* container() const {
        return container_;
    }

  private:
    // === Widget References ===
    lv_obj_t* container_ = nullptr;     ///< Where rows and spacers live
    lv_obj_t* scroll_parent_ = nullptr; ///< Scrollable parent (for reading scroll_y)
    lv_obj_t* leading_spacer_ = nullptr;
    lv_obj_t* trailing_spacer_ = nullptr;

    // === Pool State ===
    std::vector<lv_obj_t*> pool_;
    std::vector<ssize_t> pool_indices_; ///< Maps pool slot -> job index in data vector
    RowClickCallback on_click_;

    // === Visible Range ===
    int visible_start_ = -1;
    int visible_end_ = -1;
    int total_items_ = 0; ///< Track data size to detect filter changes

    // === Cached Dimensions ===
    int cached_row_height_ = 0;
    int cached_row_gap_ = 0;

    // === Cached Spacer Heights (avoid redundant lv_obj_set_height -> relayout) ===
    int last_leading_height_ = -1;
    int last_trailing_height_ = -1;

    // === Internal Methods ===
    void init_pool();
    void create_spacers();
    void configure_row(lv_obj_t* row, size_t data_index, const PrintHistoryJob& job);

    /// Drop every cached pointer/counter WITHOUT touching any widget. Safe to
    /// run while the tree those pointers refer to is mid-deletion.
    void clear_cached_state();

    /// ContainerDeleteNet: the watched tree died — drop the pool
    /// before its pointers dangle.
    void on_netted_container_destroyed() override;

    /// LV_EVENT_DELETE net on the container: the tree the pool was built under
    /// is being deleted (panel rebuild, teardown, shutdown). The pool must not
    /// outlive it (prestonbrown/helixscreen#1396).

    // === Static Callbacks ===
    static void on_row_click_static(lv_event_t* e);
};

} // namespace helix::ui
