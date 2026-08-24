// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#if defined(HELIX_PLATFORM_ESP32)
#include "esp_psram_thumbnail.h"
#endif

#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Forward declarations
struct PrintFileData;
struct CardDimensions;

namespace helix::ui {

/**
 * @file ui_print_select_card_view.h
 * @brief Virtualized card grid view for print file selection
 *
 * Manages a fixed pool of card widgets that are recycled as the user scrolls.
 * This enables displaying thousands of files without creating thousands of widgets.
 *
 * ## Key Features:
 * - Fixed widget pool (POOL_SIZE cards created once)
 * - Spacer-based virtualization for smooth scrolling
 * - Per-card subjects for declarative text binding
 * - Observer cleanup in destructor prevents crashes
 *
 * ## Usage:
 * @code
 * PrintSelectCardView card_view;
 * card_view.setup(container, file_click_callback);
 * card_view.populate(file_list, calculate_dimensions_callback);
 * // On scroll:
 * card_view.update_visible(file_list, dimensions);
 * @endcode
 */

/**
 * @brief Per-card widget data for declarative text binding
 *
 * Stored with each pooled card widget. Subjects are bound to labels once
 * at pool creation, then updated via lv_subject_copy_string() when card is recycled.
 */
struct CardWidgetData {
    lv_subject_t filename_subject;
    char filename_buf[128] = {0};

    lv_subject_t time_subject;
    char time_buf[32] = {0};

    lv_subject_t filament_subject;
    char filament_buf[32] = {0};

    /// Folder type for declarative binding: 0=file, 1=directory, 2=parent directory (..)
    lv_subject_t folder_type_subject;

    /// Thumbnail state: 0=real thumbnail, 1=placeholder (show icon), 2=directory (hide both)
    lv_subject_t thumbnail_state_subject;

    // Observer handles (saved for cleanup before DELETE)
    lv_observer_t* filename_observer = nullptr;
    lv_observer_t* time_observer = nullptr;
    lv_observer_t* filament_observer = nullptr;
    lv_observer_t* metadata_row_observer = nullptr;    ///< Hides metadata for directories
    lv_observer_t* folder_icon_observer = nullptr;     ///< Shows folder icon for directories
    lv_observer_t* parent_dir_icon_observer = nullptr; ///< Shows parent dir icon for ".."
    lv_observer_t* thumbnail_observer = nullptr;       ///< Shows thumbnail when state==0
    lv_observer_t* no_thumb_icon_observer = nullptr;   ///< Shows placeholder icon when state==1

#if defined(HELIX_PLATFORM_ESP32)
    /// Keeps the PSRAM thumbnail buffer alive for as long as this card's
    /// `thumbnail` image widget's `src` still points at its lv_image_dsc_t,
    /// independent of the source PrintFileData entry's own lifetime (which
    /// may be replaced by a list refresh/sort while this widget still shows
    /// the old image). See esp_psram_thumbnail.h.
    std::shared_ptr<helix::ui::EspPsramThumbnail> esp_thumbnail;
#endif
};

/**
 * @brief Callback for file/directory clicks
 * @param file_index Index into file list
 */
using FileClickCallback = std::function<void(size_t file_index)>;

/**
 * @brief Callback for file long-press (destructive action arming)
 * @param file_index Index into file list
 *
 * Only fires for regular files (not directories or ".."). The card view
 * plays the `button_tap` sound before invoking the callback, and suppresses
 * the next LV_EVENT_CLICKED on the same card so the file is not also
 * selected when the user lifts their finger.
 */
using FileLongPressCallback = std::function<void(size_t file_index)>;

/**
 * @brief Callback to trigger metadata fetch for visible range
 * @param start Start index (inclusive)
 * @param end End index (exclusive)
 */
using MetadataFetchCallback = std::function<void(size_t start, size_t end)>;

/**
 * @brief Virtualized card grid view with widget pooling
 */
class PrintSelectCardView {
  public:
    PrintSelectCardView();
    ~PrintSelectCardView();

    // Non-copyable, movable
    PrintSelectCardView(const PrintSelectCardView&) = delete;
    PrintSelectCardView& operator=(const PrintSelectCardView&) = delete;
    PrintSelectCardView(PrintSelectCardView&& other) noexcept;
    PrintSelectCardView& operator=(PrintSelectCardView&& other) noexcept;

    // === Configuration ===

    static constexpr int POOL_SIZE = 24;         ///< Fixed pool of card widgets
    static constexpr int BUFFER_ROWS = 1;        ///< Extra rows above/below viewport
    static constexpr int MIN_WIDTH = 150;        ///< Minimum card width
    static constexpr int MAX_WIDTH = 230;        ///< Maximum card width
    static constexpr int DEFAULT_HEIGHT = 245;   ///< Default card height
    static constexpr int ROW_3_MIN_HEIGHT = 520; ///< Min height for 3-row layout

    static constexpr const char* COMPONENT_NAME = "print_file_card";
    static constexpr const char* FOLDER_ICON = "A:assets/images/folder.png";

    /**
     * @brief Sentinel thumbnail path for files without an embedded thumbnail
     *
     * Returns an empty string. The card and detail views hide their lv_image
     * and show the cube_outline icon whenever this sentinel is used.
     */
    static std::string get_default_thumbnail();

    /**
     * @brief Check whether a path is the empty-placeholder sentinel
     *
     * @param path Thumbnail path to check
     * @return true if path is empty (no real thumbnail yet)
     */
    static bool is_placeholder_thumbnail(const std::string& path);

    /**
     * @brief Check if a thumbnail path points to a real, existing file
     *
     * Validates that the path is non-empty, not a placeholder, and the
     * underlying file exists on disk. Handles LVGL "A:" drive prefix
     * stripping for the filesystem check.
     *
     * @param path Thumbnail path (may include LVGL "A:" prefix)
     * @return true if path points to an existing non-placeholder file
     */
    static bool has_real_thumbnail(const std::string& path);

    // === Setup ===

    /**
     * @brief Initialize the card view with container and callbacks
     * @param container Scrollable container widget for cards
     * @param on_file_click Callback when card is clicked
     * @param on_metadata_fetch Callback to fetch metadata for visible range
     * @return true if setup succeeded
     */
    bool setup(lv_obj_t* container, FileClickCallback on_file_click,
               MetadataFetchCallback on_metadata_fetch);

    /**
     * @brief Set the long-press callback (optional; no long-press handling if unset)
     * @param on_file_long_press Callback invoked when a file card is long-pressed
     */
    void set_on_file_long_press(FileLongPressCallback on_file_long_press) {
        on_file_long_press_ = std::move(on_file_long_press);
    }

    /**
     * @brief Clean up resources (observers, spacers)
     *
     * Called automatically by destructor. Safe to call multiple times.
     */
    void cleanup();

    // === Population ===

    /**
     * @brief Populate view with file list
     * @param file_list Reference to file data vector
     * @param dims Card dimensions for layout
     * @param preserve_scroll If true, preserve scroll position; otherwise reset to top
     *
     * Resets scroll position and visible range, then updates visible cards.
     */
    void populate(const std::vector<PrintFileData>& file_list, const CardDimensions& dims,
                  bool preserve_scroll = false);

    /**
     * @brief Update visible cards based on scroll position
     * @param file_list Reference to file data vector
     * @param dims Card dimensions for layout
     *
     * Called on scroll events. Recycles cards that scrolled out of view.
     */
    void update_visible(const std::vector<PrintFileData>& file_list, const CardDimensions& dims);

    /**
     * @brief Refresh content of visible cards without repositioning
     * @param file_list Reference to file data vector
     * @param dims Card dimensions for layout
     *
     * Called when metadata/thumbnails update asynchronously.
     */
    void refresh_content(const std::vector<PrintFileData>& file_list, const CardDimensions& dims);

    // === State Queries ===

    /**
     * @brief Check if pool has been initialized
     */
    [[nodiscard]] bool is_initialized() const {
        return !card_pool_.empty();
    }

    /**
     * @brief Get current visible row range
     * @param start_row Output: first visible row (-1 if uninitialized)
     * @param end_row Output: last visible row (exclusive)
     */
    void get_visible_range(int& start_row, int& end_row) const {
        start_row = visible_start_row_;
        end_row = visible_end_row_;
    }

    /**
     * @brief Get cards per row for current layout
     */
    [[nodiscard]] int get_cards_per_row() const {
        return cards_per_row_;
    }

  private:
    // === Widget References ===
    lv_obj_t* container_ = nullptr;
    lv_obj_t* leading_spacer_ = nullptr;
    lv_obj_t* trailing_spacer_ = nullptr;

    // === Pool State ===
    std::vector<lv_obj_t*> card_pool_;
    std::vector<ssize_t> card_pool_indices_;
    std::vector<std::unique_ptr<CardWidgetData>> card_data_pool_;

    // === Visible Range ===
    int cards_per_row_ = 3;
    int visible_start_row_ = -1;
    int visible_end_row_ = -1;

    // === Cached Spacer Heights (avoid redundant lv_obj_set_height → relayout) ===
    int last_leading_height_ = -1;
    int last_trailing_height_ = -1;

    // === Cached Gradient Buffer (shared across all cards) ===
    lv_draw_buf_t* cached_gradient_ = nullptr;
    int32_t cached_gradient_w_ = 0;
    int32_t cached_gradient_h_ = 0;
    bool cached_gradient_dark_ = true;

    /// Ensure gradient buffer matches current card dimensions and theme
    void ensure_gradient_cache(int32_t card_width, int32_t card_height);

    /// Apply cached gradient to a card's gradient_bg image
    void apply_gradient_to_card(lv_obj_t* card);

    // === Theme Observer (re-renders gradient on dark/light switch) ===
    ObserverGuard theme_observer_;

    // === Callbacks ===
    FileClickCallback on_file_click_;
    FileLongPressCallback on_file_long_press_;
    MetadataFetchCallback on_metadata_fetch_;

    /// Set to true when a long-press fires; next CLICKED event on any card is skipped.
    /// Single-shot: cleared the next time on_card_clicked() runs.
    bool suppress_next_click_ = false;

    // === Internal Methods ===

    /**
     * @brief Initialize the fixed card pool
     * @param dims Initial card dimensions
     */
    void init_pool(const CardDimensions& dims);

    /**
     * @brief Configure a pool card to display a specific file
     * @param card Pool card widget
     * @param pool_index Index into card_pool_
     * @param file_index Index into file_list
     * @param file File data to display
     * @param dims Card dimensions
     */
    void configure_card(lv_obj_t* card, size_t pool_index, size_t file_index,
                        const PrintFileData& file, const CardDimensions& dims);

    /**
     * @brief Create spacers for virtualization
     */
    void create_spacers();

    // === Static Callbacks ===
    static void on_card_clicked(lv_event_t* e);
    static void on_card_long_pressed(lv_event_t* e);
    static void on_card_press_lost(lv_event_t* e);
};

} // namespace helix::ui
