// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_card_view.h"

#include "ui_filename_utils.h"
#include "ui_gradient_canvas.h"
#include "ui_panel_print_select.h" // For PrintFileData, CardDimensions

#include "sound_manager.h"
#include "theme_manager.h"
#include "thumbnail_processor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>

using helix::gcode::strip_gcode_extension;

namespace helix::ui {

// ============================================================================
// Static Methods
// ============================================================================

std::string PrintSelectCardView::get_default_thumbnail() {
    // Empty sentinel — views key the cube_outline icon on empty path.
    return {};
}

bool PrintSelectCardView::is_placeholder_thumbnail(const std::string& path) {
    return path.empty();
}

bool PrintSelectCardView::has_real_thumbnail(const std::string& path) {
    if (path.empty() || is_placeholder_thumbnail(path))
        return false;
    // Strip LVGL "A:" drive prefix for OS filesystem check
    std::string_view fs_path = path;
    if (fs_path.substr(0, 2) == "A:")
        fs_path.remove_prefix(2);
    return std::filesystem::exists(fs_path);
}

// ============================================================================
// Construction / Destruction
// ============================================================================

PrintSelectCardView::PrintSelectCardView() {
    spdlog::trace("[PrintSelectCardView] Constructed");
}

PrintSelectCardView::~PrintSelectCardView() {
    cleanup();
    spdlog::trace("[PrintSelectCardView] Destroyed");
}

PrintSelectCardView::PrintSelectCardView(PrintSelectCardView&& other) noexcept
    : container_(other.container_), leading_spacer_(other.leading_spacer_),
      trailing_spacer_(other.trailing_spacer_), card_pool_(std::move(other.card_pool_)),
      card_pool_indices_(std::move(other.card_pool_indices_)),
      card_data_pool_(std::move(other.card_data_pool_)), cards_per_row_(other.cards_per_row_),
      visible_start_row_(other.visible_start_row_), visible_end_row_(other.visible_end_row_),
      cached_gradient_(other.cached_gradient_), cached_gradient_w_(other.cached_gradient_w_),
      cached_gradient_h_(other.cached_gradient_h_),
      cached_gradient_dark_(other.cached_gradient_dark_),
      theme_observer_(std::move(other.theme_observer_)),
      on_file_click_(std::move(other.on_file_click_)),
      on_file_long_press_(std::move(other.on_file_long_press_)),
      on_metadata_fetch_(std::move(other.on_metadata_fetch_)),
      suppress_next_click_(other.suppress_next_click_) {
    other.container_ = nullptr;
    other.leading_spacer_ = nullptr;
    other.trailing_spacer_ = nullptr;
    other.visible_start_row_ = -1;
    other.visible_end_row_ = -1;
    other.cached_gradient_ = nullptr;
    other.suppress_next_click_ = false;
}

PrintSelectCardView& PrintSelectCardView::operator=(PrintSelectCardView&& other) noexcept {
    if (this != &other) {
        cleanup();

        container_ = other.container_;
        leading_spacer_ = other.leading_spacer_;
        trailing_spacer_ = other.trailing_spacer_;
        card_pool_ = std::move(other.card_pool_);
        card_pool_indices_ = std::move(other.card_pool_indices_);
        card_data_pool_ = std::move(other.card_data_pool_);
        cards_per_row_ = other.cards_per_row_;
        visible_start_row_ = other.visible_start_row_;
        visible_end_row_ = other.visible_end_row_;
        cached_gradient_ = other.cached_gradient_;
        cached_gradient_w_ = other.cached_gradient_w_;
        cached_gradient_h_ = other.cached_gradient_h_;
        cached_gradient_dark_ = other.cached_gradient_dark_;
        theme_observer_ = std::move(other.theme_observer_);
        on_file_click_ = std::move(other.on_file_click_);
        on_file_long_press_ = std::move(other.on_file_long_press_);
        on_metadata_fetch_ = std::move(other.on_metadata_fetch_);
        suppress_next_click_ = other.suppress_next_click_;

        other.container_ = nullptr;
        other.leading_spacer_ = nullptr;
        other.trailing_spacer_ = nullptr;
        other.visible_start_row_ = -1;
        other.visible_end_row_ = -1;
        other.cached_gradient_ = nullptr;
        other.suppress_next_click_ = false;
    }
    return *this;
}

// ============================================================================
// Setup / Cleanup
// ============================================================================

bool PrintSelectCardView::setup(lv_obj_t* container, FileClickCallback on_file_click,
                                MetadataFetchCallback on_metadata_fetch) {
    if (!container) {
        spdlog::error("[PrintSelectCardView] Cannot setup - null container");
        return false;
    }

    container_ = container;
    on_file_click_ = std::move(on_file_click);
    on_metadata_fetch_ = std::move(on_metadata_fetch);

    spdlog::trace("[PrintSelectCardView] Setup complete");
    return true;
}

void PrintSelectCardView::cleanup() {
    // Deinitialize subjects - this properly removes all attached observers.
    // We use lv_subject_deinit() instead of lv_observer_remove() because
    // widget-bound observers (from lv_label_bind_text, lv_obj_bind_flag_if_*)
    // can be auto-removed by LVGL when widgets are deleted, leaving dangling
    // pointers. Working from the subject side is always safe since we own them.
    if (lv_is_initialized()) {
        for (auto& data : card_data_pool_) {
            if (data) {
                lv_subject_deinit(&data->filename_subject);
                lv_subject_deinit(&data->time_subject);
                lv_subject_deinit(&data->filament_subject);
                lv_subject_deinit(&data->folder_type_subject);
                lv_subject_deinit(&data->thumbnail_state_subject);
            }
        }
    }

    // Release theme observer before freeing gradient buffer
    theme_observer_.reset();

    // Free cached gradient buffer
    if (cached_gradient_) {
        lv_draw_buf_destroy(cached_gradient_);
        cached_gradient_ = nullptr;
        cached_gradient_w_ = 0;
        cached_gradient_h_ = 0;
    }

    // Clear data structures
    card_data_pool_.clear();
    card_pool_.clear();
    card_pool_indices_.clear();

    // Clear widget references (owned by LVGL widget tree)
    container_ = nullptr;
    leading_spacer_ = nullptr;
    trailing_spacer_ = nullptr;
    visible_start_row_ = -1;
    visible_end_row_ = -1;
    last_leading_height_ = -1;
    last_trailing_height_ = -1;
    spdlog::debug("[PrintSelectCardView] cleanup()");
}

// ============================================================================
// Gradient Cache
// ============================================================================

void PrintSelectCardView::ensure_gradient_cache(int32_t card_width, int32_t card_height) {
    bool dark = theme_manager_is_dark_mode();

    // Already cached at this size and theme mode?
    if (cached_gradient_ && cached_gradient_w_ == card_width && cached_gradient_h_ == card_height &&
        cached_gradient_dark_ == dark) {
        return;
    }

    // Create new buffer BEFORE destroying old — lv_image_set_src triggers
    // lv_obj_update_layout which walks sibling cards. If the old buffer were
    // freed first, siblings still referencing it would hit use-after-free
    // during the layout walk (#788, #790).
    int32_t radius = theme_manager_get_spacing("border_radius");
    lv_draw_buf_t* old_gradient = cached_gradient_;
    cached_gradient_ = ui_gradient_canvas_create_buf(card_width, card_height, dark, radius);
    cached_gradient_w_ = card_width;
    cached_gradient_h_ = card_height;
    cached_gradient_dark_ = dark;

    // Update ALL pool cards to reference the new buffer immediately
    for (auto* card : card_pool_) {
        apply_gradient_to_card(card);
    }

    // Now safe to destroy old buffer — no card references it
    if (old_gradient) {
        lv_draw_buf_destroy(old_gradient);
    }
}

void PrintSelectCardView::apply_gradient_to_card(lv_obj_t* card) {
    if (!cached_gradient_ || !card || !lv_obj_is_valid(card))
        return;

    lv_obj_t* gradient_bg = lv_obj_find_by_name(card, "gradient_bg");
    if (gradient_bg) {
        lv_image_set_src(gradient_bg, cached_gradient_);
    }
}

// ============================================================================
// Pool Initialization
// ============================================================================

void PrintSelectCardView::init_pool(const CardDimensions& dims) {
    if (!container_ || !card_pool_.empty()) {
        return;
    }

    spdlog::debug("[PrintSelectCardView] Creating {} card widgets", POOL_SIZE);

    // Update layout to get accurate dimensions
    lv_obj_update_layout(container_);
    cards_per_row_ = dims.num_columns;

    // Reserve storage
    card_pool_.reserve(POOL_SIZE);
    card_pool_indices_.resize(POOL_SIZE, -1);
    card_data_pool_.reserve(POOL_SIZE);

    // Cache placeholder path for use in attrs array (needs stable pointer)
    std::string placeholder_thumb = get_default_thumbnail();

    // Create pool cards (initially hidden)
    for (int i = 0; i < POOL_SIZE; i++) {
        const char* attrs[] = {"thumbnail_src",
                               placeholder_thumb.c_str(),
                               "filename",
                               "",
                               "print_time",
                               "",
                               "filament_weight",
                               "",
                               NULL};

        lv_obj_t* card = static_cast<lv_obj_t*>(lv_xml_create(container_, COMPONENT_NAME, attrs));

        if (card) {
            lv_obj_set_width(card, dims.card_width);
            lv_obj_set_height(card, dims.card_height);
            lv_obj_set_style_flex_grow(card, 0, LV_PART_MAIN);
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

            // Attach click handler ONCE at pool creation
            lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, this);

            // Attach long-press handler ONCE at pool creation (matches click handler pattern).
            // LVGL fires LV_EVENT_LONG_PRESSED on its hold-timer BEFORE release, so releasing
            // after a long-press also fires LV_EVENT_CLICKED — we suppress it in on_card_clicked.
            lv_obj_add_event_cb(card, on_card_long_pressed, LV_EVENT_LONG_PRESSED, this);

            // If the long-press fires and then the user drags off the card without releasing,
            // LVGL sends PRESS_LOST instead of CLICKED, leaving suppress_next_click_ stuck.
            // Clearing it here keeps the state machine robust against that sequence.
            lv_obj_add_event_cb(card, on_card_press_lost, LV_EVENT_PRESS_LOST, this);

            // Create per-card data with subjects for declarative text binding
            auto data = std::make_unique<CardWidgetData>();

            // Initialize subjects
            lv_subject_init_string(&data->filename_subject, data->filename_buf, nullptr,
                                   sizeof(data->filename_buf), "");
            lv_subject_init_string(&data->time_subject, data->time_buf, nullptr,
                                   sizeof(data->time_buf), "--");
            lv_subject_init_string(&data->filament_subject, data->filament_buf, nullptr,
                                   sizeof(data->filament_buf), "--");
            // folder_type: 0=file, 1=directory, 2=parent directory (..)
            lv_subject_init_int(&data->folder_type_subject, 0);
            // thumbnail_state: 0=real thumbnail, 1=placeholder (show icon), 2=directory
            lv_subject_init_int(&data->thumbnail_state_subject, 1);

            // Bind labels to subjects
            lv_obj_t* filename_label = lv_obj_find_by_name(card, "filename_label");
            if (filename_label) {
                data->filename_observer =
                    lv_label_bind_text(filename_label, &data->filename_subject, "%s");
            }

            lv_obj_t* time_label = lv_obj_find_by_name(card, "time_label");
            if (time_label) {
                data->time_observer = lv_label_bind_text(time_label, &data->time_subject, "%s");
            }

            lv_obj_t* filament_label = lv_obj_find_by_name(card, "filament_label");
            if (filament_label) {
                data->filament_observer =
                    lv_label_bind_text(filament_label, &data->filament_subject, "%s");
            }

            // Bind directory-related visibility (declarative UI pattern)
            // folder_type: 0=file, 1=directory, 2=parent directory
            lv_obj_t* metadata_row = lv_obj_find_by_name(card, "metadata_row");
            if (metadata_row) {
                // Hide metadata row for any directory (folder_type != 0)
                data->metadata_row_observer = lv_obj_bind_flag_if_not_eq(
                    metadata_row, &data->folder_type_subject, LV_OBJ_FLAG_HIDDEN, 0);
            }

            lv_obj_t* folder_icon = lv_obj_find_by_name(card, "folder_icon");
            if (folder_icon) {
                // Show folder icon only for regular directories (folder_type == 1)
                data->folder_icon_observer = lv_obj_bind_flag_if_not_eq(
                    folder_icon, &data->folder_type_subject, LV_OBJ_FLAG_HIDDEN, 1);
            }

            lv_obj_t* parent_dir_icon = lv_obj_find_by_name(card, "parent_dir_icon");
            if (parent_dir_icon) {
                // Show parent dir icon only for ".." entries (folder_type == 2)
                data->parent_dir_icon_observer = lv_obj_bind_flag_if_not_eq(
                    parent_dir_icon, &data->folder_type_subject, LV_OBJ_FLAG_HIDDEN, 2);
            }

            // Bind thumbnail/placeholder icon visibility to thumbnail_state
            // 0=real thumbnail, 1=placeholder (show cube icon), 2=directory (hide both)
            lv_obj_t* thumbnail = lv_obj_find_by_name(card, "thumbnail");
            if (thumbnail) {
                data->thumbnail_observer = lv_obj_bind_flag_if_not_eq(
                    thumbnail, &data->thumbnail_state_subject, LV_OBJ_FLAG_HIDDEN, 0);
            }

            lv_obj_t* no_thumb_icon = lv_obj_find_by_name(card, "no_thumbnail_icon");
            if (no_thumb_icon) {
                data->no_thumb_icon_observer = lv_obj_bind_flag_if_not_eq(
                    no_thumb_icon, &data->thumbnail_state_subject, LV_OBJ_FLAG_HIDDEN, 1);
            }

            card_pool_.push_back(card);
            card_data_pool_.push_back(std::move(data));
        }
    }

    // Render shared gradient at exact card dimensions (applies to all pool cards)
    ensure_gradient_cache(dims.card_width, dims.card_height);

    // Observe theme changes to re-render gradient for dark/light switch
    lv_subject_t* theme_subject = theme_manager_get_changed_subject();
    if (theme_subject) {
        theme_observer_ = ObserverGuard(
            theme_subject,
            [](lv_observer_t* observer, lv_subject_t*) {
                auto* self = static_cast<PrintSelectCardView*>(lv_observer_get_user_data(observer));
                if (self && self->cached_gradient_) {
                    // Force cache invalidation by resetting dark mode flag
                    self->cached_gradient_dark_ = !theme_manager_is_dark_mode();
                    self->ensure_gradient_cache(self->cached_gradient_w_, self->cached_gradient_h_);
                }
            },
            this);
    }

    spdlog::debug("[PrintSelectCardView] Pool initialized with {} cards", card_pool_.size());
}

void PrintSelectCardView::create_spacers() {
    if (!container_) {
        return;
    }

    // Leading spacer - fills space before visible cards
    if (!leading_spacer_) {
        leading_spacer_ = lv_obj_create(container_);
        lv_obj_remove_style_all(leading_spacer_);
        lv_obj_remove_flag(leading_spacer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(leading_spacer_, lv_pct(100));
        lv_obj_set_height(leading_spacer_, 0);
    }

    // Trailing spacer - enables scrolling to end
    if (!trailing_spacer_) {
        trailing_spacer_ = lv_obj_create(container_);
        lv_obj_remove_style_all(trailing_spacer_);
        lv_obj_remove_flag(trailing_spacer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(trailing_spacer_, lv_pct(100));
        lv_obj_set_height(trailing_spacer_, 0);
    }
}

// ============================================================================
// Card Configuration
// ============================================================================

void PrintSelectCardView::configure_card(lv_obj_t* card, size_t pool_index, size_t file_index,
                                         const PrintFileData& file, const CardDimensions& dims) {
    if (!card || pool_index >= card_data_pool_.size()) {
        return;
    }

    CardWidgetData* data = card_data_pool_[pool_index].get();
    if (!data) {
        return;
    }

    // Determine folder type: 0=file, 1=directory, 2=parent directory (..)
    int folder_type = 0;
    if (file.is_dir) {
        folder_type = (file.filename == "..") ? 2 : 1;
    }

    // Update display name (parent dir shows as "..", others append "/" for dirs)
    std::string display_name;
    if (file.filename == "..") {
        display_name = "..";
    } else if (file.is_dir) {
        display_name = file.filename + "/";
    } else {
        display_name = strip_gcode_extension(file.filename);
    }

    // Update subjects (declarative pattern - bindings react automatically)
    lv_subject_copy_string(&data->filename_subject, display_name.c_str());
    lv_subject_copy_string(&data->time_subject, file.print_time_str.c_str());
    lv_subject_copy_string(&data->filament_subject, file.filament_str.c_str());
    lv_subject_set_int(&data->folder_type_subject, folder_type);

    // Update thumbnail state (observers handle visibility declaratively)
    // 0=real thumbnail, 1=placeholder (show cube icon), 2=directory (hide both)
    if (file.is_dir) {
        lv_subject_set_int(&data->thumbnail_state_subject, 2);
    } else {
        bool has_real_thumb = has_real_thumbnail(file.thumbnail_path);
#if defined(HELIX_PLATFORM_ESP32)
        // No disk thumbnail cache on this platform (Task 10 R6) — a fetched
        // thumbnail lives in file.esp_thumbnail (PSRAM) instead of a file at
        // file.thumbnail_path, so has_real_thumbnail()'s std::filesystem::exists
        // check alone would always report false here.
        bool has_psram_thumb = static_cast<bool>(file.esp_thumbnail);
        has_real_thumb = has_real_thumb || has_psram_thumb;
#endif
        if (has_real_thumb) {
            lv_obj_t* thumb_img = lv_obj_find_by_name(card, "thumbnail");
            if (thumb_img) {
#if defined(HELIX_PLATFORM_ESP32)
                if (has_psram_thumb) {
                    // Keep the buffer alive in this pool slot for as long as
                    // the widget's `src` references it (see CardWidgetData
                    // comment) — assigning here also drops the previous
                    // slot's thumbnail, if any.
                    data->esp_thumbnail = file.esp_thumbnail;
                    lv_image_set_src(thumb_img, data->esp_thumbnail->dsc());
                } else
#endif
                {
                    lv_image_set_src(thumb_img, file.thumbnail_path.c_str());
                }
                // Size widget to match pre-scaled .bin target so LVGL uses 1:1 blit
                // instead of the scaled transform path (avoids per-frame bilinear scaling).
                // Re-read per update rather than caching in a function-local static: the
                // target tracks the measured card size, which changes on resize.
                auto target = helix::ThumbnailProcessor::get_target_for_display();
                lv_obj_set_size(thumb_img, target.width, target.height);
            }
            lv_subject_set_int(&data->thumbnail_state_subject, 0);
        } else {
            lv_subject_set_int(&data->thumbnail_state_subject, 1);
        }
    }

    // Note: metadata_row visibility, folder_icon, and thumbnail visibility
    // are handled declaratively via folder_type_subject bindings.
    // Overlay is content-sized so it adapts automatically.

    // Update card sizing (ensure_gradient_cache updates all cards if dims changed)
    lv_obj_set_width(card, dims.card_width);
    lv_obj_set_height(card, dims.card_height);

    ensure_gradient_cache(dims.card_width, dims.card_height);

    // Store file index for click handler
    lv_obj_set_user_data(card, reinterpret_cast<void*>(file_index));

    // Show the card
    lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Population / Visibility
// ============================================================================

void PrintSelectCardView::populate(const std::vector<PrintFileData>& file_list,
                                   const CardDimensions& dims, bool preserve_scroll) {
    if (!container_) {
        return;
    }

    spdlog::debug("[PrintSelectCardView] Populating with {} files (preserve_scroll={})",
                  file_list.size(), preserve_scroll);

    // Save scroll position before any changes if preserving
    int32_t saved_scroll = preserve_scroll ? lv_obj_get_scroll_y(container_) : 0;

    // Initialize pool on first call
    if (card_pool_.empty()) {
        init_pool(dims);
    }

    // Create spacers if needed
    create_spacers();

    // Update cards per row
    cards_per_row_ = dims.num_columns;

    // Reset visible range tracking
    visible_start_row_ = -1;
    visible_end_row_ = -1;

    // Invalidate card pool indices to force reconfiguration
    // This is critical when the file list content changes (e.g., directory navigation)
    // even though indices might match, the underlying data is different
    std::fill(card_pool_indices_.begin(), card_pool_indices_.end(), -1);

    // Update visible cards (this also updates spacer heights)
    update_visible(file_list, dims);

    // Restore or reset scroll position.
    //
    // Note on the clamp: lv_obj_get_scroll_bottom() returns the *remaining* scrollable
    // distance below the current position, NOT the total max scroll offset. The total
    // range is (current_y + scroll_bottom). Clamping against scroll_bottom alone would
    // pull a scrolled-down user back toward the top every refresh tick — see #scroll-bug.
    if (preserve_scroll && saved_scroll > 0) {
        lv_obj_update_layout(container_);
        int32_t current_y = lv_obj_get_scroll_y(container_);
        int32_t max_scroll = current_y + lv_obj_get_scroll_bottom(container_);
        lv_obj_scroll_to_y(container_, std::min(saved_scroll, max_scroll), LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(container_, 0, LV_ANIM_OFF);
    }

    int total_rows = (static_cast<int>(file_list.size()) + cards_per_row_ - 1) / cards_per_row_;
    spdlog::debug("[PrintSelectCardView] Populated: {} files, {} rows, pool size {}",
                  file_list.size(), total_rows, card_pool_.size());
}

void PrintSelectCardView::update_visible(const std::vector<PrintFileData>& file_list,
                                         const CardDimensions& dims) {
    if (!container_ || card_pool_.empty() || file_list.empty()) {
        return;
    }

    // Get scroll position and container dimensions
    int32_t scroll_y = lv_obj_get_scroll_y(container_);
    int32_t viewport_height = lv_obj_get_height(container_);

    cards_per_row_ = dims.num_columns;

    int card_gap = lv_obj_get_style_pad_row(container_, LV_PART_MAIN);
    int row_height = dims.card_height + card_gap;
    int total_rows = (static_cast<int>(file_list.size()) + cards_per_row_ - 1) / cards_per_row_;

    // Calculate visible row range (with buffer)
    int first_visible_row = std::max(0, static_cast<int>(scroll_y / row_height) - BUFFER_ROWS);
    int last_visible_row = std::min(
        total_rows, static_cast<int>((scroll_y + viewport_height) / row_height) + 1 + BUFFER_ROWS);

    // Skip update if visible range hasn't changed
    if (first_visible_row == visible_start_row_ && last_visible_row == visible_end_row_) {
        return;
    }

    // Calculate file index range
    int first_visible_idx = first_visible_row * cards_per_row_;
    int last_visible_idx =
        std::min(static_cast<int>(file_list.size()), last_visible_row * cards_per_row_);

    spdlog::trace("[PrintSelectCardView] Scroll: {} viewport: {} rows: {}-{} indices: {}-{}",
                  scroll_y, viewport_height, first_visible_row, last_visible_row, first_visible_idx,
                  last_visible_idx);

    // Update spacer heights (only when changed to avoid redundant relayout)
    int leading_height = first_visible_row * row_height;
    if (leading_spacer_) {
        if (leading_height != last_leading_height_) {
            lv_obj_set_height(leading_spacer_, leading_height);
            last_leading_height_ = leading_height;
        }
        if (lv_obj_get_index(leading_spacer_) != 0) {
            lv_obj_move_to_index(leading_spacer_, 0);
        }
    }

    int trailing_height = std::max(0, (total_rows - last_visible_row) * row_height);
    if (trailing_spacer_) {
        if (trailing_height != last_trailing_height_) {
            lv_obj_set_height(trailing_spacer_, trailing_height);
            last_trailing_height_ = trailing_height;
        }
    }

    // Assign pool cards to visible indices, skipping cards that already show correct file
    size_t pool_idx = 0;
    for (int file_idx = first_visible_idx;
         file_idx < last_visible_idx && pool_idx < card_pool_.size(); file_idx++, pool_idx++) {
        lv_obj_t* card = card_pool_[pool_idx];

        // Skip reconfiguration if this card already shows this file
        if (card_pool_indices_[pool_idx] != file_idx) {
            configure_card(card, pool_idx, static_cast<size_t>(file_idx), file_list[file_idx],
                           dims);
            card_pool_indices_[pool_idx] = file_idx;
        }

        // Ensure card is in correct position (guard to avoid redundant relayout)
        int target_index = static_cast<int>(pool_idx) + 1;
        if (lv_obj_get_index(card) != target_index) {
            lv_obj_move_to_index(card, target_index);
        }
    }

    // Hide unused pool cards
    for (; pool_idx < card_pool_.size(); pool_idx++) {
        lv_obj_add_flag(card_pool_[pool_idx], LV_OBJ_FLAG_HIDDEN);
        card_pool_indices_[pool_idx] = -1;
    }

    visible_start_row_ = first_visible_row;
    visible_end_row_ = last_visible_row;

    // Trigger metadata fetch for newly visible range
    if (on_metadata_fetch_) {
        on_metadata_fetch_(static_cast<size_t>(first_visible_idx),
                           static_cast<size_t>(last_visible_idx));
    }
}

void PrintSelectCardView::refresh_content(const std::vector<PrintFileData>& file_list,
                                          const CardDimensions& dims) {
    if (!container_ || !lv_obj_is_valid(container_) || card_pool_.empty() ||
        visible_start_row_ < 0) {
        return;
    }

    // Re-configure each visible pool card with latest data
    for (size_t i = 0; i < card_pool_.size(); i++) {
        ssize_t file_idx = card_pool_indices_[i];
        if (file_idx >= 0 && static_cast<size_t>(file_idx) < file_list.size() &&
            lv_obj_is_valid(card_pool_[i])) {
            configure_card(card_pool_[i], i, static_cast<size_t>(file_idx), file_list[file_idx],
                           dims);
        }
    }
}

// ============================================================================
// Static Callbacks
// ============================================================================

void PrintSelectCardView::on_card_clicked(lv_event_t* e) {
    auto* self = static_cast<PrintSelectCardView*>(lv_event_get_user_data(e));
    auto* card = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // If a long-press just fired on this (or any) card, LVGL will still deliver a
    // CLICKED event when the user releases their finger. Swallow it so the file is
    // not also selected/navigated behind the confirmation modal.
    if (self && self->suppress_next_click_) {
        self->suppress_next_click_ = false;
        spdlog::trace("[PrintSelectCardView] suppressed click after long-press");
        return;
    }

    if (self && self->on_file_click_ && card) {
        auto file_index = reinterpret_cast<size_t>(lv_obj_get_user_data(card));
        self->on_file_click_(file_index);
    }
}

void PrintSelectCardView::on_card_long_pressed(lv_event_t* e) {
    auto* self = static_cast<PrintSelectCardView*>(lv_event_get_user_data(e));
    auto* card = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!self || !card) {
        return;
    }

    // Resolve pool slot so we can check folder_type_subject for this card.
    // Only files (folder_type == 0) trigger long-press delete; directories and ".." are skipped.
    size_t pool_index = self->card_pool_.size();
    for (size_t i = 0; i < self->card_pool_.size(); ++i) {
        if (self->card_pool_[i] == card) {
            pool_index = i;
            break;
        }
    }
    if (pool_index >= self->card_pool_.size()) {
        spdlog::warn("[PrintSelectCardView] long-press on card not in pool");
        return;
    }

    auto& data = self->card_data_pool_[pool_index];
    if (!data) {
        return;
    }
    int32_t folder_type = lv_subject_get_int(&data->folder_type_subject);
    if (folder_type != 0) {
        // Directory or parent-dir card — ignore long-press.
        spdlog::trace("[PrintSelectCardView] long-press ignored for non-file card (folder_type={})",
                      folder_type);
        return;
    }

    // Arm the click-suppression guard BEFORE invoking the callback so that the
    // inevitable CLICKED event on release is swallowed even if the callback shows
    // a modal that takes the next touch frame.
    self->suppress_next_click_ = true;

    // Audible feedback. SoundManager::play() internally gates on the sound-enabled
    // and UI-sounds-enabled settings, so no guard needed here.
    SoundManager::instance().play("button_tap");

    if (self->on_file_long_press_) {
        auto file_index = reinterpret_cast<size_t>(lv_obj_get_user_data(card));
        spdlog::debug("[PrintSelectCardView] long-press fired for file_index={}", file_index);
        self->on_file_long_press_(file_index);
    }
}

void PrintSelectCardView::on_card_press_lost(lv_event_t* e) {
    auto* self = static_cast<PrintSelectCardView*>(lv_event_get_user_data(e));
    if (self && self->suppress_next_click_) {
        self->suppress_next_click_ = false;
        spdlog::trace("[PrintSelectCardView] suppress flag cleared on PRESS_LOST");
    }
}

} // namespace helix::ui
