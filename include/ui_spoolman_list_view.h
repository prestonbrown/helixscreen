// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "spoolman_types.h"

#include <lvgl.h>
#include <vector>

namespace helix::ui {

/**
 * @file ui_spoolman_list_view.h
 * @brief Virtualized list view for Spoolman spool inventory
 *
 * Manages a fixed pool of spool row widgets that are recycled as the user scrolls.
 * Follows the same spacer-based virtualization pattern as PrintSelectListView.
 *
 * ## Key Features:
 * - Fixed widget pool (POOL_SIZE rows created once)
 * - Leading/trailing spacers for smooth scroll virtualization
 * - Imperative row updates (no per-row subjects)
 * - Spool canvas color, weight, percentage, low stock, active indicator
 *
 * ## Click Handling:
 * Rows use XML event callbacks (on_spoolman_spool_row_clicked), not internal handlers.
 * configure_row() sets user_data to the spool ID for each recycled row.
 */
class SpoolmanListView {
  public:
    static constexpr int POOL_SIZE = 20;  ///< Fixed pool of spool row widgets
    static constexpr int BUFFER_ROWS = 2; ///< Extra rows above/below viewport

    /// Cached child widget pointers for a pool row (avoids lv_obj_find_by_name per scroll)
    struct RowWidgets {
        lv_obj_t* root = nullptr;
        lv_obj_t* canvas = nullptr;
        lv_obj_t* id_label = nullptr;
        lv_obj_t* name_label = nullptr;
        lv_obj_t* vendor_label = nullptr;
        lv_obj_t* weight_label = nullptr;
        lv_obj_t* percent_label = nullptr;
        lv_obj_t* low_stock_icon = nullptr;
        lv_obj_t* active_indicator = nullptr;
    };

    SpoolmanListView() = default;
    ~SpoolmanListView();

    // Non-copyable
    SpoolmanListView(const SpoolmanListView&) = delete;
    SpoolmanListView& operator=(const SpoolmanListView&) = delete;

    // === Setup / Cleanup ===

    /**
     * @brief Initialize the list view with a scrollable container
     * @param container Scrollable LVGL container for spool rows
     * @return true if setup succeeded
     */
    bool setup(lv_obj_t* container);

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
     * @brief Populate view with spool list
     * @param spools Spool data to display
     * @param active_spool_id Currently active spool ID (-1 for none)
     */
    void populate(const std::vector<SpoolInfo>& spools, int active_spool_id,
                  bool preserve_scroll = false);

    /**
     * @brief Update visible rows based on scroll position
     * @param spools Spool data vector
     * @param active_spool_id Currently active spool ID
     */
    void update_visible(const std::vector<SpoolInfo>& spools, int active_spool_id);

    /**
     * @brief Refresh content of visible rows without repositioning
     * @param spools Spool data vector
     * @param active_spool_id Currently active spool ID
     */
    void refresh_content(const std::vector<SpoolInfo>& spools, int active_spool_id);

    /**
     * @brief Update only active indicators on visible rows
     * @param spools Spool data vector (for spool ID lookup)
     * @param active_spool_id Currently active spool ID
     */
    void update_active_indicators(const std::vector<SpoolInfo>& spools, int active_spool_id);

    // === State Queries ===

    [[nodiscard]] bool is_initialized() const {
        return !pool_.empty();
    }

    [[nodiscard]] lv_obj_t* container() const {
        return container_;
    }

  private:
    // === Widget References ===
    lv_obj_t* container_ = nullptr;
    lv_obj_t* leading_spacer_ = nullptr;
    lv_obj_t* trailing_spacer_ = nullptr;

    // === Pool State ===
    std::vector<RowWidgets> pool_;
    std::vector<ssize_t> pool_indices_; ///< Maps pool slot -> spool index in data vector

    // === Visible Range ===
    int visible_start_ = -1;
    int visible_end_ = -1;
    int total_items_ = 0; ///< Track data size to detect filter changes

    // === Cached Dimensions ===
    int cached_row_height_ = 0;
    int cached_row_gap_ = 0;

    // === Cached Spacer Heights (avoid redundant lv_obj_set_height → relayout) ===
    int last_leading_height_ = -1;
    int last_trailing_height_ = -1;

    // === Internal Methods ===
    void init_pool();
    void create_spacers();
    void configure_row(RowWidgets& rw, const SpoolInfo& spool, int active_spool_id);

    /// Drop every cached pointer/counter WITHOUT touching any widget. Safe to
    /// run while the tree those pointers refer to is mid-deletion.
    void clear_cached_state();

    /// LV_EVENT_DELETE net on the container: the tree the pool was built under
    /// is being deleted (overlay rebuild, teardown, shutdown). The pool must
    /// not outlive it (prestonbrown/helixscreen#1396).
    static void on_container_delete(lv_event_t* e);
};

} // namespace helix::ui
