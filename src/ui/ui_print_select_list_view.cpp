// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_list_view.h"

#include "ui_filename_utils.h"
#include "ui_panel_print_select.h" // For PrintFileData

#include "display_settings_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

using helix::gcode::strip_gcode_extension;

namespace helix::ui {

// ============================================================================
// Construction / Destruction
// ============================================================================

PrintSelectListView::PrintSelectListView() {
    spdlog::trace("[PrintSelectListView] Constructed");
}

PrintSelectListView::~PrintSelectListView() {
    cleanup();
    spdlog::trace("[PrintSelectListView] Destroyed");
}

// ============================================================================
// Setup / Cleanup
// ============================================================================

bool PrintSelectListView::setup(lv_obj_t* container, FileClickCallback on_file_click,
                                MetadataFetchCallback on_metadata_fetch) {
    if (!container) {
        spdlog::error("[PrintSelectListView] Cannot setup - null container");
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
    on_file_click_ = std::move(on_file_click);
    on_metadata_fetch_ = std::move(on_metadata_fetch);

    spdlog::trace("[PrintSelectListView] Setup complete");
    return true;
}

void PrintSelectListView::clear_cached_state() {
    // Deinitialize subjects - this properly removes all attached observers.
    // We use lv_subject_deinit() instead of lv_observer_remove() because
    // widget-bound observers can be auto-removed by LVGL when widgets are
    // deleted, leaving dangling pointers.
    if (lv_is_initialized()) {
        for (auto& data : list_data_pool_) {
            if (data) {
                lv_subject_deinit(&data->filename_subject);
                lv_subject_deinit(&data->size_subject);
                lv_subject_deinit(&data->modified_subject);
                lv_subject_deinit(&data->time_subject);
            }
        }
    }

    // Clear data structures
    list_data_pool_.clear();
    list_pool_.clear();
    list_pool_indices_.clear();

    // Clear widget references
    leading_spacer_ = nullptr;
    trailing_spacer_ = nullptr;
    visible_start_ = -1;
    visible_end_ = -1;
    last_leading_height_ = -1;
    last_trailing_height_ = -1;
}

void PrintSelectListView::on_netted_container_destroyed() {
    container_ = nullptr;
    clear_cached_state();
}

void PrintSelectListView::cleanup() {
    // Remove the delete net first when the container still outlives this view,
    // so it can never fire into a destroyed owner.
    detach_container_net();
    clear_cached_state();
    container_ = nullptr;
    spdlog::debug("[PrintSelectListView] cleanup()");
}

// ============================================================================
// Pool Initialization
// ============================================================================

void PrintSelectListView::init_pool() {
    if (!container_ || !list_pool_.empty()) {
        return;
    }

    spdlog::debug("[PrintSelectListView] Creating {} row widgets", POOL_SIZE);

    // Reserve storage
    list_pool_.reserve(POOL_SIZE);
    list_pool_indices_.resize(POOL_SIZE, -1);
    list_data_pool_.reserve(POOL_SIZE);

    // Create pool rows (initially hidden)
    for (int i = 0; i < POOL_SIZE; i++) {
        const char* attrs[] = {"filename", "",           "file_size", "",  "modified_date",
                               "",         "print_time", "",          NULL};

        lv_obj_t* row =
            static_cast<lv_obj_t*>(lv_xml_create(container_, "print_file_list_row", attrs));

        if (row) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

            // Attach click handler ONCE at pool creation
            lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, this);

            // Create per-row data with subjects
            auto data = std::make_unique<ListRowWidgetData>();

            // Initialize subjects
            lv_subject_init_string(&data->filename_subject, data->filename_buf, nullptr,
                                   sizeof(data->filename_buf), "");
            lv_subject_init_string(&data->size_subject, data->size_buf, nullptr,
                                   sizeof(data->size_buf), "--");
            lv_subject_init_string(&data->modified_subject, data->modified_buf, nullptr,
                                   sizeof(data->modified_buf), "--");
            lv_subject_init_string(&data->time_subject, data->time_buf, nullptr,
                                   sizeof(data->time_buf), "--");

            // Bind labels to subjects
            lv_obj_t* filename_label = lv_obj_find_by_name(row, "row_filename");
            if (filename_label) {
                data->filename_observer =
                    lv_label_bind_text(filename_label, &data->filename_subject, "%s");
            }

            lv_obj_t* size_label = lv_obj_find_by_name(row, "row_size");
            if (size_label) {
                data->size_observer = lv_label_bind_text(size_label, &data->size_subject, "%s");
            }

            lv_obj_t* modified_label = lv_obj_find_by_name(row, "row_modified");
            if (modified_label) {
                data->modified_observer =
                    lv_label_bind_text(modified_label, &data->modified_subject, "%s");
            }

            lv_obj_t* time_label = lv_obj_find_by_name(row, "row_print_time");
            if (time_label) {
                data->time_observer = lv_label_bind_text(time_label, &data->time_subject, "%s");
            }

            // Find status display widgets (controlled programmatically, no subject binding)
            data->status_printing_icon = lv_obj_find_by_name(row, "status_printing");
            data->status_success_container = lv_obj_find_by_name(row, "status_success_container");
            data->status_success_count = lv_obj_find_by_name(row, "status_success_count");
            data->status_failed_icon = lv_obj_find_by_name(row, "status_failed");
            data->status_cancelled_icon = lv_obj_find_by_name(row, "status_cancelled");

            list_pool_.push_back(row);
            list_data_pool_.push_back(std::move(data));
        }
    }

    spdlog::debug("[PrintSelectListView] Pool initialized with {} rows", list_pool_.size());
}

void PrintSelectListView::create_spacers() {
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
// Row Configuration
// ============================================================================

void PrintSelectListView::configure_row(lv_obj_t* row, size_t pool_index, size_t file_index,
                                        const PrintFileData& file) {
    if (!row || pool_index >= list_data_pool_.size()) {
        return;
    }

    ListRowWidgetData* data = list_data_pool_[pool_index].get();
    if (!data) {
        return;
    }

    // Update display name
    std::string display_name =
        file.is_dir ? file.filename + "/" : strip_gcode_extension(file.filename);

    // Update labels via subjects
    lv_subject_copy_string(&data->filename_subject, display_name.c_str());
    lv_subject_copy_string(&data->size_subject, file.size_str.c_str());
    lv_subject_copy_string(&data->modified_subject, file.modified_str.c_str());
    lv_subject_copy_string(&data->time_subject, file.print_time_str.c_str());

    // Update status display based on history_status
    // Hide all status indicators first
    if (data->status_printing_icon) {
        lv_obj_add_flag(data->status_printing_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (data->status_success_container) {
        lv_obj_add_flag(data->status_success_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (data->status_failed_icon) {
        lv_obj_add_flag(data->status_failed_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (data->status_cancelled_icon) {
        lv_obj_add_flag(data->status_cancelled_icon, LV_OBJ_FLAG_HIDDEN);
    }

    // Show appropriate status indicator (directories have no history)
    if (!file.is_dir) {
        switch (file.history_status) {
        case FileHistoryStatus::CURRENTLY_PRINTING:
            if (data->status_printing_icon) {
                lv_obj_remove_flag(data->status_printing_icon, LV_OBJ_FLAG_HIDDEN);
            }
            break;

        case FileHistoryStatus::COMPLETED:
            if (data->status_success_container && data->status_success_count) {
                // Format count (e.g., "3" for 3 successful prints)
                char count_buf[8];
                snprintf(count_buf, sizeof(count_buf), "%d", file.success_count);
                lv_label_set_text(data->status_success_count, count_buf);
                lv_obj_remove_flag(data->status_success_container, LV_OBJ_FLAG_HIDDEN);
            }
            break;

        case FileHistoryStatus::FAILED:
            if (data->status_failed_icon) {
                lv_obj_remove_flag(data->status_failed_icon, LV_OBJ_FLAG_HIDDEN);
            }
            break;

        case FileHistoryStatus::CANCELLED:
            if (data->status_cancelled_icon) {
                lv_obj_remove_flag(data->status_cancelled_icon, LV_OBJ_FLAG_HIDDEN);
            }
            break;

        case FileHistoryStatus::NEVER_PRINTED:
        default:
            // All indicators already hidden
            break;
        }
    }

    // Store file index for click handler
    lv_obj_set_user_data(row, reinterpret_cast<void*>(file_index));

    // Show the row
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Population / Visibility
// ============================================================================

void PrintSelectListView::populate(const std::vector<PrintFileData>& file_list,
                                   bool preserve_scroll) {
    if (!container_) {
        return;
    }

    spdlog::debug("[PrintSelectListView] Populating with {} files (preserve_scroll={})",
                  file_list.size(), preserve_scroll);

    // Save scroll position before any changes if preserving
    int32_t saved_scroll = preserve_scroll ? lv_obj_get_scroll_y(container_) : 0;

    // Initialize pool on first call
    if (list_pool_.empty()) {
        init_pool();
    }

    // Create spacers if needed
    create_spacers();

    // Cache row dimensions on first populate (after pool exists but before hiding all rows)
    // We need a visible, laid-out row to measure correctly
    if (cached_row_height_ == 0 && !list_pool_.empty() && !file_list.empty()) {
        // Temporarily configure and show first row to measure it
        lv_obj_t* row = list_pool_[0];
        configure_row(row, 0, 0, file_list[0]);
        lv_obj_update_layout(container_);

        cached_row_height_ = lv_obj_get_height(row);
        cached_row_gap_ = lv_obj_get_style_pad_row(container_, LV_PART_MAIN);

        spdlog::debug("[PrintSelectListView] Cached row dimensions: height={} gap={}",
                      cached_row_height_, cached_row_gap_);
    }

    // Reset visible range tracking
    visible_start_ = -1;
    visible_end_ = -1;

    // Update visible rows (this also updates spacer heights)
    update_visible(file_list);

    // Restore or reset scroll position
    if (preserve_scroll && saved_scroll > 0) {
        lv_obj_update_layout(container_);
        int32_t max_scroll = lv_obj_get_scroll_bottom(container_);
        lv_obj_scroll_to_y(container_, std::min(saved_scroll, max_scroll), LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(container_, 0, LV_ANIM_OFF);
    }

    spdlog::debug("[PrintSelectListView] Populated: {} files, pool size {}", file_list.size(),
                  list_pool_.size());
}

void PrintSelectListView::update_visible(const std::vector<PrintFileData>& file_list) {
    if (!container_ || list_pool_.empty() || file_list.empty()) {
        return;
    }

    // Get scroll position and container dimensions
    int32_t scroll_y = lv_obj_get_scroll_y(container_);
    int32_t viewport_height = lv_obj_get_height(container_);

    int total_rows = static_cast<int>(file_list.size());

    // Use cached row dimensions (set in populate())
    // Fall back to defaults if not yet cached
    int row_height = cached_row_height_ > 0 ? cached_row_height_ : 44;
    int row_gap = cached_row_gap_;
    int row_stride = row_height + row_gap;

    // Calculate visible row range (with buffer)
    int first_visible = std::max(0, static_cast<int>(scroll_y / row_stride) - BUFFER_ROWS);
    int last_visible = std::min(
        total_rows, static_cast<int>((scroll_y + viewport_height) / row_stride) + 1 + BUFFER_ROWS);

    // Skip update if visible range hasn't changed
    if (first_visible == visible_start_ && last_visible == visible_end_) {
        return;
    }

    spdlog::trace("[PrintSelectListView] Scroll: y={} viewport={} visible={}-{}/{} stride={}",
                  scroll_y, viewport_height, first_visible, last_visible, total_rows, row_stride);

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

    // Assign pool rows to visible indices, skipping rows that already show correct file
    size_t pool_idx = 0;
    for (int file_idx = first_visible; file_idx < last_visible && pool_idx < list_pool_.size();
         file_idx++, pool_idx++) {
        lv_obj_t* row = list_pool_[pool_idx];

        if (list_pool_indices_[pool_idx] != file_idx) {
            configure_row(row, pool_idx, static_cast<size_t>(file_idx), file_list[file_idx]);
            list_pool_indices_[pool_idx] = file_idx;
        }

        // Ensure row is in correct position (guard to avoid redundant relayout)
        int target_index = static_cast<int>(pool_idx) + 1;
        if (lv_obj_get_index(row) != target_index) {
            lv_obj_move_to_index(row, target_index);
        }
    }

    // Hide unused pool rows
    for (; pool_idx < list_pool_.size(); pool_idx++) {
        lv_obj_add_flag(list_pool_[pool_idx], LV_OBJ_FLAG_HIDDEN);
        list_pool_indices_[pool_idx] = -1;
    }

    visible_start_ = first_visible;
    visible_end_ = last_visible;

    // Trigger metadata fetch for newly visible range
    if (on_metadata_fetch_) {
        on_metadata_fetch_(static_cast<size_t>(first_visible), static_cast<size_t>(last_visible));
    }
}

void PrintSelectListView::refresh_content(const std::vector<PrintFileData>& file_list) {
    if (!container_ || list_pool_.empty() || visible_start_ < 0) {
        return;
    }

    // Re-configure each visible pool row with latest data
    for (size_t i = 0; i < list_pool_.size(); i++) {
        ssize_t file_idx = list_pool_indices_[i];
        if (file_idx >= 0 && static_cast<size_t>(file_idx) < file_list.size()) {
            configure_row(list_pool_[i], i, static_cast<size_t>(file_idx), file_list[file_idx]);
        }
    }
}

// ============================================================================
// Animation
// ============================================================================

void PrintSelectListView::animate_entrance() {
    if (list_pool_.empty()) {
        return;
    }

    // Skip animation if disabled
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        for (lv_obj_t* row : list_pool_) {
            if (!lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_set_style_translate_y(row, 0, LV_PART_MAIN);
                lv_obj_set_style_opa(row, LV_OPA_COVER, LV_PART_MAIN);
            }
        }
        spdlog::debug("[PrintSelectListView] Animations disabled - showing rows instantly");
        return;
    }

    size_t animated_count = 0;
    for (size_t i = 0; i < list_pool_.size() && animated_count < MAX_ANIMATED_ROWS; i++) {
        lv_obj_t* row = list_pool_[i];

        if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }

        // Start row below final position and transparent
        lv_obj_set_style_translate_y(row, SLIDE_OFFSET_Y, LV_PART_MAIN);
        lv_obj_set_style_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);

        int32_t delay = static_cast<int32_t>(animated_count) * STAGGER_DELAY_MS;

        // Slide up animation
        lv_anim_t slide_anim;
        lv_anim_init(&slide_anim);
        lv_anim_set_var(&slide_anim, row);
        lv_anim_set_values(&slide_anim, SLIDE_OFFSET_Y, 0);
        lv_anim_set_duration(&slide_anim, ENTRANCE_DURATION_MS);
        lv_anim_set_delay(&slide_anim, delay);
        lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&slide_anim, [](void* obj, int32_t value) {
            lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
        });
        lv_anim_start(&slide_anim);

        // Fade in animation
        lv_anim_t fade_anim;
        lv_anim_init(&fade_anim);
        lv_anim_set_var(&fade_anim, row);
        lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&fade_anim, ENTRANCE_DURATION_MS);
        lv_anim_set_delay(&fade_anim, delay);
        lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
            lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                                 LV_PART_MAIN);
        });
        lv_anim_start(&fade_anim);

        animated_count++;
    }

    spdlog::debug("[PrintSelectListView] Entrance animation started ({} rows)", animated_count);
}

// ============================================================================
// Static Callbacks
// ============================================================================

void PrintSelectListView::on_row_clicked(lv_event_t* e) {
    auto* self = static_cast<PrintSelectListView*>(lv_event_get_user_data(e));
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    if (self && self->on_file_click_ && row) {
        auto file_index = reinterpret_cast<size_t>(lv_obj_get_user_data(row));
        self->on_file_click_(file_index);
    }
}

} // namespace helix::ui
