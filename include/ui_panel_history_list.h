// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_history_list_view.h"
#include "ui_observer_guard.h"

#include "in_flight_guard.h"
#include "overlay_base.h"
#include "print_history_data.h"
#include "print_history_manager.h"
#include "subject_managed_panel.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct FileInfo;

/**
 * @file ui_panel_history_list.h
 * @brief Print History List Panel - Scrollable list of print jobs with filter/sort
 *
 * The History List Panel displays a scrollable list of all print history jobs
 * with metadata (filename, date, duration, filament type, status).
 *
 * ## Features (Stage 4):
 * - Search: Case-insensitive filename search with 300ms debounce
 * - Status Filter: All, Completed, Failed, Cancelled
 * - Sort: Date (newest/oldest), Duration, Filename
 * - Filters chain: search → status → sort → display
 *
 * ## Navigation:
 * - Entry: History Dashboard → "View Full History" button
 * - Back: Returns to History Dashboard
 * - Row click: Opens Detail Overlay with job metadata
 *
 * ## Features (Stage 5):
 * - Detail Overlay: Shows full print metadata when clicking a row
 * - Reprint: Start the same print again (if file still exists)
 * - Delete: Remove job from history with confirmation
 *
 * ## Data Flow:
 * 1. On activate, receives job list from HistoryDashboardPanel
 * 2. Applies search/filter/sort to create filtered_jobs_ for display
 * 3. Dynamically creates row widgets for filtered jobs
 * 4. Caches job data for row click handling (indexes into filtered_jobs_)
 *
 * @see print_history_data.h for PrintHistoryJob struct
 * @see OverlayBase for base class documentation
 */

/**
 * @brief Sort column for history list
 */
enum class HistorySortColumn {
    DATE,     ///< Sort by start_time (default)
    DURATION, ///< Sort by total_duration
    FILENAME  ///< Sort by filename alphabetically
};

/**
 * @brief Sort direction
 */
enum class HistorySortDirection {
    DESC, ///< Descending (newest first, longest first, Z-A)
    ASC   ///< Ascending (oldest first, shortest first, A-Z)
};

/**
 * @brief Status filter options (maps to dropdown indices)
 */
enum class HistoryStatusFilter {
    ALL = 0,       ///< Show all statuses
    COMPLETED = 1, ///< Only completed jobs
    FAILED = 2,    ///< Only failed/error jobs
    CANCELLED = 3  ///< Only cancelled jobs
};

class HistoryListPanel : public OverlayBase {
  public:
    /**
     * @brief Default constructor
     *
     * Dependencies are obtained from global accessors:
     * - get_printer_state()
     * - get_moonraker_api()
     * - get_print_history_manager()
     */
    HistoryListPanel();

    ~HistoryListPanel() override;

    //
    // === OverlayBase Implementation ===
    //

    /**
     * @brief Initialize subjects for reactive bindings
     *
     * Creates:
     * - history_list_panel_state: 0 = LOADING, 1 = EMPTY, 2 = HAS_JOBS
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize subjects for cleanup
     *
     * Calls lv_subject_deinit() on all subjects. Must be called
     * before destructor to avoid dangling observers.
     */
    void deinit_subjects();

    /**
     * @brief Register XML event callbacks
     */
    void register_callbacks() override;

    /**
     * @brief Create the list panel from XML
     *
     * @param parent Parent widget (screen)
     * @return Root widget of the overlay
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    [[nodiscard]] const char* get_name() const override {
        return "History List";
    }

    //
    // === Lifecycle Hooks ===
    //

    /**
     * @brief Called when panel becomes visible
     *
     * If jobs haven't been set externally, fetches history from API.
     */
    void on_activate() override;

    /**
     * @brief Called when panel is hidden
     */
    void on_deactivate() override;

    //
    // === Public API ===
    //

    /**
     * @brief Set the jobs to display (called by dashboard when navigating)
     *
     * This avoids redundant API calls since the dashboard already has the data.
     *
     * @param jobs Vector of print history jobs
     */
    void set_jobs(const std::vector<PrintHistoryJob>& jobs);

    /**
     * @brief Refresh the list from the API (fetches first page)
     */
    void refresh_from_api();

    /**
     * @brief Load more jobs (infinite scroll)
     *
     * Called when user scrolls near the bottom of the list.
     * Appends next page of results to existing jobs.
     */
    void load_more();

    /**
     * @brief Fetch timelapse files and associate them with print jobs
     *
     * Called after history is loaded to match timelapse videos to their
     * corresponding print jobs by filename pattern matching.
     */
    void fetch_timelapse_files();

    /**
     * @brief Associate timelapse files with print history jobs
     *
     * Matches timelapse video files to jobs by looking for the job's
     * filename (without .gcode extension) within the timelapse filename.
     *
     * @param timelapse_files List of files from the timelapse directory
     */
    void associate_timelapse_files(const std::vector<FileInfo>& timelapse_files);

  private:
    //
    // === Widget References ===
    //

    lv_obj_t* list_content_ = nullptr;  ///< Scrollable content area
    lv_obj_t* list_rows_ = nullptr;     ///< Container for row widgets
    lv_obj_t* empty_state_ = nullptr;   ///< Empty state message container
    lv_obj_t* search_box_ = nullptr;    ///< Search textarea
    lv_obj_t* filter_status_ = nullptr; ///< Status filter dropdown
    lv_obj_t* sort_dropdown_ = nullptr; ///< Sort dropdown

    //
    // === State ===
    //

    std::vector<PrintHistoryJob> jobs_;              ///< Source of truth - all jobs
    std::vector<PrintHistoryJob> filtered_jobs_;     ///< Filtered/sorted for display
    bool jobs_received_ = false;                     ///< True if jobs were set externally
    bool is_active_ = false;                         ///< True if panel is currently visible
    bool detail_overlay_open_ = false;               ///< True while detail overlay is showing
    bool history_changed_while_detail_open_ = false; ///< True if history changed while detail open

    // Connection state observer to auto-refresh when connected (ObserverGuard handles cleanup)
    ObserverGuard connection_observer_;

    //
    // === Dependencies ===
    //

    std::unique_ptr<helix::ui::HistoryListView> list_view_; ///< Virtual scroll view

    PrintHistoryManager* history_manager_ = nullptr; ///< Shared history cache (DRY)

    /// Observer callback for history manager changes
    helix::HistoryChangedCallback history_observer_;

    // Parent screen reference
    lv_obj_t* parent_screen_ = nullptr;

    // Callback registration tracking
    bool callbacks_registered_ = false;

    // Pagination state for infinite scroll
    static constexpr int PAGE_SIZE = 50; ///< Jobs per API request
    uint64_t total_job_count_ = 0;       ///< Total jobs on server (from API)
    /// Single-flight guard for the "load next page" fetch, with a 30s self-heal
    /// so a silently-lost response can't permanently block infinite scroll.
    helix::InFlightGuard load_more_guard_{std::chrono::milliseconds(30000)};
    bool has_more_data_ = true; ///< False when all jobs loaded

    // Filter/sort state
    std::string search_query_;                                         ///< Current search text
    HistoryStatusFilter status_filter_ = HistoryStatusFilter::ALL;     ///< Current status filter
    HistorySortColumn sort_column_ = HistorySortColumn::DATE;          ///< Current sort column
    HistorySortDirection sort_direction_ = HistorySortDirection::DESC; ///< Current sort direction

    // Search debounce timer
    lv_timer_t* search_timer_ = nullptr; ///< Timer for debounced search (300ms)

    //
    // === Subject Manager for RAII cleanup ===
    //

    SubjectManager subjects_; ///< RAII manager for automatic subject cleanup

    //
    // === Subject for panel state binding ===
    //

    lv_subject_t subject_panel_state_; ///< 0 = LOADING, 1 = EMPTY, 2 = HAS_JOBS
    lv_subject_t subject_filters_expanded_; ///< 0 = filter dropdowns collapsed, 1 = expanded
    lv_subject_t subject_filter_active_;    ///< 1 when a status/search/sort filter is active (drives funnel accent)

    // Empty state message subjects (for dynamic text based on filter state)
    lv_subject_t subject_empty_message_; ///< Empty state message text
    lv_subject_t subject_empty_hint_;    ///< Empty state hint text

    // Buffers for empty state string subjects
    char empty_message_buf_[64] = {};
    char empty_hint_buf_[64] = {};

    //
    // === Detail Overlay State ===
    //

    lv_obj_t* detail_overlay_ = nullptr;     ///< Detail overlay widget (created on first use)
    size_t selected_job_index_ = 0;          ///< Index of currently selected job in filtered_jobs_
    uint64_t detail_overlay_generation_ = 0; ///< Generation counter for async callback safety

    // Detail overlay subjects (string subjects for reactive binding)
    lv_subject_t detail_filename_;
    lv_subject_t detail_status_;
    lv_subject_t detail_status_icon_;
    lv_subject_t detail_status_variant_;
    lv_subject_t detail_start_time_;
    lv_subject_t detail_end_time_;
    lv_subject_t detail_duration_;
    lv_subject_t detail_layers_;
    lv_subject_t detail_layer_height_;
    lv_subject_t detail_nozzle_temp_;
    lv_subject_t detail_bed_temp_;
    lv_subject_t detail_filament_;
    lv_subject_t detail_filament_type_;
    lv_subject_t detail_can_reprint_;   ///< 1 if file exists, 0 otherwise
    lv_subject_t detail_status_code_;   ///< 0=completed, 1=cancelled, 2=error, 3=in_progress
    lv_subject_t detail_has_timelapse_; ///< 1 if timelapse available, 0 otherwise

    // Buffers for string subjects (LVGL 9.4 requires pre-allocated buffers)
    static constexpr size_t DETAIL_BUF_SIZE = 128;
    static constexpr size_t SMALL_BUF_SIZE = 32;
    char detail_filename_buf_[256] = {};
    char detail_status_buf_[SMALL_BUF_SIZE] = {};
    char detail_status_icon_buf_[SMALL_BUF_SIZE] = {};
    char detail_status_variant_buf_[SMALL_BUF_SIZE] = {};
    char detail_start_time_buf_[SMALL_BUF_SIZE] = {};
    char detail_end_time_buf_[SMALL_BUF_SIZE] = {};
    char detail_duration_buf_[SMALL_BUF_SIZE] = {};
    char detail_layers_buf_[SMALL_BUF_SIZE] = {};
    char detail_layer_height_buf_[SMALL_BUF_SIZE] = {};
    char detail_nozzle_temp_buf_[SMALL_BUF_SIZE] = {};
    char detail_bed_temp_buf_[SMALL_BUF_SIZE] = {};
    char detail_filament_buf_[SMALL_BUF_SIZE] = {};
    char detail_filament_type_buf_[SMALL_BUF_SIZE] = {};

    //
    // === Internal Methods ===
    //

    /**
     * @brief Populate the list with row widgets from filtered_jobs_
     *
     * Clears existing rows and creates new ones from filtered_jobs_ vector.
     */
    void populate_list();

    /**
     * @brief Clear all row widgets from the list
     */
    void clear_list();

    /**
     * @brief Update the empty state visibility and message
     *
     * Shows appropriate message based on whether filters are active.
     */
    void update_empty_state();

    /**
     * @brief Apply all filters and sort, then populate list
     *
     * Chain: search → status filter → sort → populate_list()
     */
    void apply_filters_and_sort();

    /**
     * @brief Apply search filter to jobs
     *
     * Case-insensitive substring match on filename.
     *
     * @param source Source job list
     * @return Filtered job list
     */
    std::vector<PrintHistoryJob> apply_search_filter(const std::vector<PrintHistoryJob>& source);

    /**
     * @brief Apply status filter to jobs
     *
     * @param source Source job list
     * @return Filtered job list
     */
    std::vector<PrintHistoryJob> apply_status_filter(const std::vector<PrintHistoryJob>& source);

    /**
     * @brief Sort jobs in place
     *
     * @param jobs Jobs to sort (modified in place)
     */
    void apply_sort(std::vector<PrintHistoryJob>& jobs);

    /**
     * @brief Get status color for a job status
     *
     * @param status Job status enum
     * @return Hex color string (e.g., "#00C853")
     */
    static const char* get_status_color(PrintJobStatus status);

    /**
     * @brief Get display text for a job status
     *
     * @param status Job status enum
     * @return Display string (e.g., "Completed", "Failed")
     */
    static const char* get_status_text(PrintJobStatus status);

    //
    // === Click Handlers ===
    //

    /**
     * @brief Attach click handler to a row widget
     *
     * @param row Row widget
     * @param index Index into filtered_jobs_ vector
     */
    void attach_row_click_handler(lv_obj_t* row, size_t index);

    /**
     * @brief Handle row click - opens detail overlay
     *
     * @param index Index of clicked row in filtered_jobs_
     */
    void handle_row_click(size_t index);

    // Static callback wrapper for row clicks
    static void on_row_clicked_static(lv_event_t* e);

    //
    // === Detail Overlay Methods ===
    //

    /**
     * @brief Initialize subjects for the detail overlay
     *
     * Called during init_subjects() to set up all binding subjects.
     */
    void init_detail_subjects();

    /**
     * @brief Show the detail overlay for a job
     *
     * Updates all detail subjects with job data and pushes the overlay.
     *
     * @param job The job to display
     */
    void show_detail_overlay(const PrintHistoryJob& job);

    /**
     * @brief Update detail subjects with job data
     *
     * @param job The job to display
     */
    void update_detail_subjects(const PrintHistoryJob& job);

    /**
     * @brief Handle reprint button click
     */
    void handle_reprint();

    /**
     * @brief Handle delete button click
     */
    void handle_delete();

    /**
     * @brief Actually delete the job after confirmation
     */
    void confirm_delete();

    /**
     * @brief Handle view timelapse button click
     */
    void handle_view_timelapse();

    //
    // === Filter/Sort Event Handlers ===
    //

    /**
     * @brief Handle search text change (debounced)
     */
    void on_search_changed();
    void on_search_clear();

    /**
     * @brief Debounced search callback (called after 300ms)
     */
    void do_debounced_search();

    /**
     * @brief Handle status filter dropdown change
     *
     * @param index Selected dropdown index (maps to HistoryStatusFilter)
     */
    void on_status_filter_changed(int index);

    /**
     * @brief Handle sort dropdown change
     *
     * @param index Selected dropdown index
     */
    void on_sort_changed(int index);

    /** @brief Toggle the collapsible filter dropdowns (funnel button). */
    void toggle_filters();

    /** @brief Accent the funnel icon when a status/search filter is active. */
    void refresh_filter_indicator();

    // Search timer callback (private - not used for XML registration)
    static void on_search_timer_static(lv_timer_t* timer);

    //
    // === Infinite Scroll ===
    //

    /**
     * @brief Check if scroll is near bottom and load more if needed
     */
    void check_scroll_position();

    /**
     * @brief Static callback for scroll events (infinite scroll load-more)
     */
    static void on_scroll_static(lv_event_t* e);

    /**
     * @brief Static callback for scroll events (virtual scroll update)
     */
    static void on_scroll_update_visible(lv_event_t* e);

    /**
     * @brief Append rows for newly loaded jobs (without clearing existing rows)
     *
     * @param start_index Index in filtered_jobs_ to start appending from
     */
    void append_rows(size_t start_index);
};

/**
 * @brief Get global HistoryListPanel instance
 *
 * Creates instance on first call. Used by static callbacks.
 */
HistoryListPanel& get_global_history_list_panel();
