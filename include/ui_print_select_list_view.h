// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct PrintFileData;

namespace helix::ui {

/**
 * @file ui_print_select_list_view.h
 * @brief Virtualized list view for print file selection
 *
 * Manages a fixed pool of list row widgets that are recycled as the user scrolls.
 * Similar to PrintSelectCardView but for tabular list display.
 *
 * ## Key Features:
 * - Fixed widget pool (POOL_SIZE rows created once)
 * - Spacer-based virtualization for smooth scrolling
 * - Per-row subjects for declarative text binding
 * - Staggered entrance animation on population
 */

/**
 * @brief Per-row widget data for declarative text binding
 */
struct ListRowWidgetData {
    lv_subject_t filename_subject;
    char filename_buf[128] = {0};

    lv_subject_t size_subject;
    char size_buf[16] = {0};

    lv_subject_t modified_subject;
    char modified_buf[32] = {0};

    lv_subject_t time_subject;
    char time_buf[32] = {0};

    // Observer handles (saved for cleanup before DELETE)
    lv_observer_t* filename_observer = nullptr;
    lv_observer_t* size_observer = nullptr;
    lv_observer_t* modified_observer = nullptr;
    lv_observer_t* time_observer = nullptr;

    // Status display refs (no subject binding - controlled programmatically)
    lv_obj_t* status_printing_icon = nullptr;
    lv_obj_t* status_success_container = nullptr;
    lv_obj_t* status_success_count = nullptr;
    lv_obj_t* status_failed_icon = nullptr;
    lv_obj_t* status_cancelled_icon = nullptr;
};

/**
 * @brief Callback for file/directory clicks
 */
using FileClickCallback = std::function<void(size_t file_index)>;

/**
 * @brief Callback to trigger metadata fetch for visible range
 */
using MetadataFetchCallback = std::function<void(size_t start, size_t end)>;

/**
 * @brief Virtualized list view with widget pooling
 */
class PrintSelectListView {
  public:
    PrintSelectListView();
    ~PrintSelectListView();

    // Non-copyable, movable
    PrintSelectListView(const PrintSelectListView&) = delete;
    PrintSelectListView& operator=(const PrintSelectListView&) = delete;
    // Non-movable: the container's LV_EVENT_DELETE net is keyed to this
    // instance, so a move would strand it on the moved-from object. The
    // views are held by unique_ptr and never moved; the compiler enforces
    // that staying true.
    PrintSelectListView(PrintSelectListView&&) = delete;
    PrintSelectListView& operator=(PrintSelectListView&&) = delete;

    // === Configuration ===

    static constexpr int POOL_SIZE = 40;                 ///< Fixed pool of list row widgets
    static constexpr int BUFFER_ROWS = 2;                ///< Extra rows above/below viewport
    static constexpr int32_t ENTRANCE_DURATION_MS = 150; ///< Animation duration for row entrance
    static constexpr int32_t STAGGER_DELAY_MS = 40;      ///< Delay between row animations
    static constexpr int32_t SLIDE_OFFSET_Y = 15;        ///< Initial Y offset for slide animation
    static constexpr size_t MAX_ANIMATED_ROWS = 10;      ///< Max rows to animate at once

    // === Setup ===

    /**
     * @brief Initialize the list view with container and callbacks
     * @param container Scrollable container widget for rows
     * @param on_file_click Callback when row is clicked
     * @param on_metadata_fetch Callback to fetch metadata for visible range
     * @return true if setup succeeded
     */
    bool setup(lv_obj_t* container, FileClickCallback on_file_click,
               MetadataFetchCallback on_metadata_fetch);

    /**
     * @brief Clean up resources (observers, spacers)
     */
    void cleanup();

    // === Population ===

    /**
     * @brief Populate view with file list
     * @param file_list Reference to file data vector
     * @param preserve_scroll If true, preserve scroll position; otherwise reset to top
     *
     * Resets scroll position and visible range, then updates visible rows.
     */
    void populate(const std::vector<PrintFileData>& file_list, bool preserve_scroll = false);

    /**
     * @brief Update visible rows based on scroll position
     * @param file_list Reference to file data vector
     */
    void update_visible(const std::vector<PrintFileData>& file_list);

    /**
     * @brief Refresh content of visible rows without repositioning
     * @param file_list Reference to file data vector
     */
    void refresh_content(const std::vector<PrintFileData>& file_list);

    /**
     * @brief Animate visible rows with staggered entrance
     *
     * Each row slides up and fades in with a staggered delay.
     */
    void animate_entrance();

    // === State Queries ===

    [[nodiscard]] bool is_initialized() const {
        return !list_pool_.empty();
    }

    void get_visible_range(int& start, int& end) const {
        start = visible_start_;
        end = visible_end_;
    }

  private:
    // === Widget References ===
    lv_obj_t* container_ = nullptr;
    lv_obj_t* leading_spacer_ = nullptr;
    lv_obj_t* trailing_spacer_ = nullptr;

    // === Pool State ===
    std::vector<lv_obj_t*> list_pool_;
    std::vector<ssize_t> list_pool_indices_;
    std::vector<std::unique_ptr<ListRowWidgetData>> list_data_pool_;

    // === Visible Range ===
    int visible_start_ = -1;
    int visible_end_ = -1;

    // === Cached Dimensions (set once after first layout) ===
    int cached_row_height_ = 0;
    int cached_row_gap_ = 0;

    // === Cached Spacer Heights (avoid redundant lv_obj_set_height → relayout) ===
    int last_leading_height_ = -1;
    int last_trailing_height_ = -1;

    // === Callbacks ===
    FileClickCallback on_file_click_;
    MetadataFetchCallback on_metadata_fetch_;

    // === Internal Methods ===
    void init_pool();
    void configure_row(lv_obj_t* row, size_t pool_index, size_t file_index,
                       const PrintFileData& file);
    void create_spacers();

    /// Drop every cached pointer/counter WITHOUT touching any widget. Safe to
    /// run while the tree those pointers refer to is mid-deletion.
    void clear_cached_state();

    /// LV_EVENT_DELETE net on the container: the tree the pool was built under
    /// is being deleted (panel rebuild, teardown, shutdown). The pool must not
    /// outlive it (prestonbrown/helixscreen#1396).
    static void on_container_delete(lv_event_t* e);

    // === Static Callbacks ===
    static void on_row_clicked(lv_event_t* e);
};

} // namespace helix::ui
