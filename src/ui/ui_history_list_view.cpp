// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_history_list_view.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace helix::ui {

// ============================================================================
// Helpers
// ============================================================================

/**
 * Parse a hex color string (with '#' prefix) into an lv_color_t.
 * Returns fallback_color if the string is empty or unparseable.
 */
static lv_color_t parse_hex_color(const char* color_hex, lv_color_t fallback_color) {
    if (!color_hex || color_hex[0] == '\0') {
        return fallback_color;
    }

    const char* hex = color_hex;
    if (hex[0] == '#') {
        hex++;
    }

    unsigned int color_val = 0;
    if (sscanf(hex, "%x", &color_val) == 1) {
        return lv_color_hex(color_val);
    }
    return fallback_color;
}

// ============================================================================
// Destruction
// ============================================================================

HistoryListView::~HistoryListView() {
    cleanup();
}

// ============================================================================
// Setup / Cleanup
// ============================================================================

bool HistoryListView::setup(lv_obj_t* container, lv_obj_t* scroll_parent,
                            RowClickCallback on_click) {
    if (!container) {
        spdlog::error("[HistoryListView] Cannot setup - null container");
        return false;
    }

    if (container_ != container) {
        // A different container means the tree the pool was built under is
        // gone (hot-reload rebuild deletes and re-creates the widget tree on
        // the same surviving panel). Cached pointers would dangle. The net
        // unhooks the old container itself and runs the destroyed hook (the
        // pool wipe) before this view adopts the new tree.
        retarget_container_net(container);
    }

    container_ = container;
    scroll_parent_ = scroll_parent ? scroll_parent : container;
    on_click_ = std::move(on_click);
    spdlog::trace("[HistoryListView] Setup complete");
    return true;
}

void HistoryListView::clear_cached_state() {
    pool_.clear();
    pool_indices_.clear();
    scroll_parent_ = nullptr;
    leading_spacer_ = nullptr;
    trailing_spacer_ = nullptr;
    visible_start_ = -1;
    visible_end_ = -1;
    total_items_ = 0;
    cached_row_height_ = 0;
    cached_row_gap_ = 0;
    last_leading_height_ = -1;
    last_trailing_height_ = -1;
}

void HistoryListView::on_netted_container_destroyed() {
    container_ = nullptr;
    clear_cached_state();
}

void HistoryListView::cleanup() {
    // Remove the delete net first when the container still outlives this view,
    // so it can never fire into a destroyed owner.
    detach_container_net();
    clear_cached_state();
    container_ = nullptr;
    spdlog::debug("[HistoryListView] cleanup()");
}

void HistoryListView::reset() {
    for (auto* row : pool_) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    std::fill(pool_indices_.begin(), pool_indices_.end(), static_cast<ssize_t>(-1));
    if (leading_spacer_) {
        lv_obj_set_height(leading_spacer_, 0);
    }
    if (trailing_spacer_) {
        lv_obj_set_height(trailing_spacer_, 0);
    }
    visible_start_ = -1;
    visible_end_ = -1;
    total_items_ = 0;
    last_leading_height_ = -1;
    last_trailing_height_ = -1;
    spdlog::debug("[HistoryListView] reset()");
}

// ============================================================================
// Pool Initialization
// ============================================================================

void HistoryListView::init_pool() {
    if (!container_ || !pool_.empty()) {
        return;
    }

    spdlog::debug("[HistoryListView] Creating {} row widgets", POOL_SIZE);

    pool_.reserve(POOL_SIZE);
    pool_indices_.resize(POOL_SIZE, -1);

    for (int i = 0; i < POOL_SIZE; i++) {
        lv_obj_t* row =
            static_cast<lv_obj_t*>(lv_xml_create(container_, "history_list_row", nullptr));

        if (row) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            // Attach click handler — one per pool row, reused across recycles
            lv_obj_add_event_cb(row, on_row_click_static, LV_EVENT_CLICKED, this);
            pool_.push_back(row);
        }
    }

    spdlog::debug("[HistoryListView] Pool initialized with {} rows", pool_.size());
}

void HistoryListView::create_spacers() {
    if (!container_) {
        return;
    }

    if (!leading_spacer_) {
        leading_spacer_ = lv_obj_create(container_);
        lv_obj_remove_style_all(leading_spacer_);
        lv_obj_remove_flag(leading_spacer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(leading_spacer_, lv_pct(100));
        lv_obj_set_height(leading_spacer_, 0);
    }

    if (!trailing_spacer_) {
        trailing_spacer_ = lv_obj_create(container_);
        lv_obj_remove_style_all(trailing_spacer_);
        lv_obj_remove_flag(trailing_spacer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(trailing_spacer_, lv_pct(100));
        lv_obj_set_height(trailing_spacer_, 0);
    }
}

// ============================================================================
// Static Callbacks
// ============================================================================

void HistoryListView::on_row_click_static(lv_event_t* e) {
    auto* view = static_cast<HistoryListView*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    if (!view || !target) {
        return;
    }

    // Find which pool slot was clicked
    for (size_t i = 0; i < view->pool_.size(); i++) {
        if (view->pool_[i] == target) {
            ssize_t data_idx = view->pool_indices_[i];
            if (data_idx >= 0 && view->on_click_) {
                view->on_click_(static_cast<size_t>(data_idx));
            }
            return;
        }
    }
}

// ============================================================================
// Row Configuration
// ============================================================================

// Status color/text helpers (duplicated from HistoryListPanel to keep view self-contained)
static const char* get_status_color(PrintJobStatus status) {
    switch (status) {
    case PrintJobStatus::COMPLETED:
        return "#00C853"; // Green
    case PrintJobStatus::CANCELLED:
        return "#FF9800"; // Orange
    case PrintJobStatus::ERROR:
        return "#F44336"; // Red
    case PrintJobStatus::IN_PROGRESS:
        return "#2196F3"; // Blue
    default:
        return "#9E9E9E"; // Gray
    }
}

static const char* get_status_text(PrintJobStatus status) {
    switch (status) {
    case PrintJobStatus::COMPLETED:
        return "Completed";
    case PrintJobStatus::CANCELLED:
        return "Cancelled";
    case PrintJobStatus::ERROR:
        return "Failed";
    case PrintJobStatus::IN_PROGRESS:
        return "In Progress";
    default:
        return "Unknown";
    }
}

void HistoryListView::configure_row(lv_obj_t* row, size_t data_index, const PrintHistoryJob& job) {
    if (!row) {
        return;
    }

    // Filename label
    lv_obj_t* filename_label = lv_obj_find_by_name(row, "row_filename");
    if (filename_label) {
        lv_label_set_text(filename_label, job.filename.c_str());
    }

    // Date label
    lv_obj_t* date_label = lv_obj_find_by_name(row, "row_date");
    if (date_label) {
        lv_label_set_text(date_label, job.date_str.c_str());
    }

    // Duration label
    lv_obj_t* duration_label = lv_obj_find_by_name(row, "row_duration");
    if (duration_label) {
        lv_label_set_text(duration_label, job.duration_str.c_str());
    }

    // Filament type label
    lv_obj_t* filament_label = lv_obj_find_by_name(row, "row_filament");
    if (filament_label) {
        lv_label_set_text(filament_label,
                          job.filament_type.empty() ? "Unknown" : job.filament_type.c_str());
    }

    // Status text
    const char* status_text = get_status_text(job.status);
    const char* status_color = get_status_color(job.status);

    lv_obj_t* status_label = lv_obj_find_by_name(row, "row_status");
    if (status_label) {
        lv_label_set_text(status_label, status_text);
        lv_color_t color = parse_hex_color(status_color, theme_manager_get_color("text_muted"));
        lv_obj_set_style_text_color(status_label, color, LV_PART_MAIN);
    }

    // Status bar color (left edge indicator)
    lv_obj_t* status_bar = lv_obj_find_by_name(row, "status_bar");
    if (status_bar) {
        lv_color_t color = parse_hex_color(status_color, theme_manager_get_color("text_muted"));
        lv_obj_set_style_bg_color(status_bar, color, LV_PART_MAIN);
    }

    // Show the row
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);

    (void)data_index; // Used by on_row_click_static via pool_indices_
}

// ============================================================================
// Population / Visibility
// ============================================================================

void HistoryListView::populate(const std::vector<PrintHistoryJob>& jobs, bool preserve_scroll) {
    if (!container_) {
        return;
    }

    spdlog::debug("[HistoryListView] Populating with {} jobs (preserve_scroll={})", jobs.size(),
                  preserve_scroll);

    // Initialize pool on first call
    if (pool_.empty()) {
        init_pool();
    }

    // Create spacers if needed
    create_spacers();

    // Cache row dimensions on first populate
    if (cached_row_height_ == 0 && !pool_.empty() && !jobs.empty()) {
        lv_obj_t* row = pool_[0];
        configure_row(row, 0, jobs[0]);
        lv_obj_update_layout(container_);

        cached_row_height_ = lv_obj_get_height(row);
        cached_row_gap_ = lv_obj_get_style_pad_row(container_, LV_PART_MAIN);

        spdlog::debug("[HistoryListView] Cached row dimensions: height={} gap={}",
                      cached_row_height_, cached_row_gap_);
    }

    // Reset visible range, spacer caches
    visible_start_ = -1;
    visible_end_ = -1;
    last_leading_height_ = -1;
    last_trailing_height_ = -1;

    // Invalidate pool indices to force reconfiguration on data change
    std::fill(pool_indices_.begin(), pool_indices_.end(), static_cast<ssize_t>(-1));

    if (!preserve_scroll) {
        lv_obj_scroll_to_y(scroll_parent_, 0, LV_ANIM_OFF);
    }

    // Update visible rows
    update_visible(jobs);

    spdlog::debug("[HistoryListView] Populated: {} jobs, pool size {}", jobs.size(), pool_.size());
}

void HistoryListView::update_visible(const std::vector<PrintHistoryJob>& jobs) {
    if (!container_ || pool_.empty() || jobs.empty()) {
        for (auto* row : pool_) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
        if (leading_spacer_) {
            lv_obj_set_height(leading_spacer_, 0);
            last_leading_height_ = 0;
        }
        if (trailing_spacer_) {
            lv_obj_set_height(trailing_spacer_, 0);
            last_trailing_height_ = 0;
        }
        visible_start_ = -1;
        visible_end_ = -1;
        total_items_ = 0;
        return;
    }

    int32_t scroll_y = lv_obj_get_scroll_y(scroll_parent_);
    int32_t viewport_height = lv_obj_get_height(scroll_parent_);

    int total_rows = static_cast<int>(jobs.size());

    int row_height = cached_row_height_ > 0 ? cached_row_height_ : 56;
    int row_gap = cached_row_gap_;
    int row_stride = row_height + row_gap;

    // Calculate visible range with buffer
    int first_visible = std::max(0, static_cast<int>(scroll_y / row_stride) - BUFFER_ROWS);
    int last_visible = std::min(
        total_rows, static_cast<int>((scroll_y + viewport_height) / row_stride) + 1 + BUFFER_ROWS);

    // Force re-render if total item count changed (e.g. filter applied)
    bool data_changed = (total_rows != total_items_);

    // Skip if unchanged (unless data set size changed)
    if (!data_changed && first_visible == visible_start_ && last_visible == visible_end_) {
        return;
    }

    total_items_ = total_rows;

    spdlog::debug(
        "[HistoryListView] Rendering rows {}-{} of {} (scroll_y={} viewport={} data_changed={})",
        first_visible, last_visible, total_rows, scroll_y, viewport_height, data_changed);

    // Update spacer heights (only when changed to avoid redundant relayout)
    int leading_height = first_visible * row_stride;
    if (leading_spacer_) {
        if (leading_height != last_leading_height_) {
            lv_obj_set_height(leading_spacer_, leading_height);
            last_leading_height_ = leading_height;
        }
        if (lv_obj_get_index(leading_spacer_) != 0) {
            lv_obj_move_to_index(leading_spacer_, 0);
        }
    }

    int trailing_height = std::max(0, (total_rows - last_visible) * row_stride);
    if (trailing_spacer_) {
        if (trailing_height != last_trailing_height_) {
            lv_obj_set_height(trailing_spacer_, trailing_height);
            last_trailing_height_ = trailing_height;
        }
    }

    // Assign pool rows to visible indices, skipping rows that already show correct data
    size_t pool_idx = 0;
    for (int job_idx = first_visible; job_idx < last_visible && pool_idx < pool_.size();
         job_idx++, pool_idx++) {
        lv_obj_t* row = pool_[pool_idx];

        if (data_changed || pool_indices_[pool_idx] != job_idx) {
            configure_row(row, static_cast<size_t>(job_idx), jobs[job_idx]);
            pool_indices_[pool_idx] = job_idx;
        }

        // Ensure row is in correct position (guard to avoid redundant relayout)
        int target_index = static_cast<int>(pool_idx) + 1;
        if (lv_obj_get_index(row) != target_index) {
            lv_obj_move_to_index(row, target_index);
        }
    }

    // Hide unused pool rows
    for (; pool_idx < pool_.size(); pool_idx++) {
        lv_obj_add_flag(pool_[pool_idx], LV_OBJ_FLAG_HIDDEN);
        pool_indices_[pool_idx] = -1;
    }

    // Ensure trailing spacer is always last child
    if (trailing_spacer_) {
        int32_t child_count = lv_obj_get_child_count(container_);
        if (lv_obj_get_index(trailing_spacer_) != child_count - 1) {
            lv_obj_move_to_index(trailing_spacer_, child_count - 1);
        }
    }

    spdlog::debug("[HistoryListView] Spacers: leading={}px trailing={}px, visible rows={}, "
                  "container content_h={} child_count={}",
                  leading_height, trailing_height, last_visible - first_visible,
                  lv_obj_get_content_height(container_), lv_obj_get_child_count(container_));

    visible_start_ = first_visible;
    visible_end_ = last_visible;
}

void HistoryListView::refresh_content(const std::vector<PrintHistoryJob>& jobs) {
    if (!container_ || pool_.empty() || visible_start_ < 0) {
        return;
    }

    for (size_t i = 0; i < pool_.size(); i++) {
        ssize_t job_idx = pool_indices_[i];
        if (job_idx >= 0 && static_cast<size_t>(job_idx) < jobs.size()) {
            configure_row(pool_[i], static_cast<size_t>(job_idx), jobs[job_idx]);
        }
    }
}

} // namespace helix::ui
