// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "overlay_base.h"
#include "print_history_data.h"
#include "print_history_manager.h"
#include "subject_managed_panel.h"

#include <vector>

/**
 * @file ui_panel_history_dashboard.h
 * @brief Print History Dashboard Panel - Statistics overview with time filtering
 *
 * The History Dashboard Panel displays aggregated print statistics including:
 * - Total prints, print time, filament used
 * - Success rate, longest print, failed/cancelled count
 *
 * ## Navigation:
 * - Entry: Advanced Panel -> "Print History" action row
 * - Back: Returns to Advanced Panel
 * - "View Full History": Opens HistoryListPanel (Stage 3)
 *
 * ## Time Filtering:
 * The panel supports 5 time filters (Day/Week/Month/Year/All) that update
 * all displayed statistics. Filter selection is maintained across panel activations.
 *
 * ## Data Flow:
 * 1. On activate, calls IMoonrakerAPI::get_history_list() with time filter
 * 2. Parses response to calculate statistics client-side
 * 3. Updates stat labels via direct widget manipulation
 *
 * Note: Moonraker's server.history.totals doesn't provide breakdown counts,
 * so we calculate success/fail/cancelled from the job list.
 *
 * @see print_history_data.h for data structures
 * @see OverlayBase for base class documentation
 */
class HistoryDashboardPanel : public OverlayBase {
  public:
    /**
     * @brief Default constructor
     *
     * Dependencies are obtained from global accessors:
     * - get_printer_state()
     * - get_moonraker_api()
     * - get_print_history_manager()
     */
    HistoryDashboardPanel();

    ~HistoryDashboardPanel() override;

    //
    // === OverlayBase Implementation ===
    //

    /**
     * @brief Initialize subjects for reactive bindings
     *
     * Creates:
     * - history_has_jobs: 0 = no history, 1 = has history (for empty state)
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize all subjects
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
     * @brief Create the dashboard panel from XML
     *
     * @param parent Parent widget (screen)
     * @return Root widget of the overlay
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    [[nodiscard]] const char* get_name() const override {
        return "History Dashboard";
    }

    //
    // === Lifecycle Hooks ===
    //

    /**
     * @brief Refresh statistics when panel becomes visible
     *
     * Fetches history data with current time filter and updates display.
     */
    void on_activate() override;

    /**
     * @brief Called when panel becomes invisible (navigated away)
     */
    void on_deactivate() override;

    //
    // === Public API ===
    //

    /**
     * @brief Set the time filter and refresh statistics
     *
     * @param filter The time filter to apply
     */
    void set_time_filter(HistoryTimeFilter filter);

    /**
     * @brief Get the current time filter
     */
    HistoryTimeFilter get_time_filter() const {
        return current_filter_;
    }

    /**
     * @brief Get the cached jobs from the last fetch
     *
     * Returns the jobs retrieved from the API during the last refresh.
     * Used by HistoryListPanel to avoid redundant API calls.
     */
    const std::vector<PrintHistoryJob>& get_cached_jobs() const {
        return cached_jobs_;
    }

    //
    // === Static Event Callbacks (registered with lv_xml_register_event_cb) ===
    // Must be public for LVGL XML system registration
    //

    static void on_filter_day_clicked(lv_event_t* e);
    static void on_filter_week_clicked(lv_event_t* e);
    static void on_filter_month_clicked(lv_event_t* e);
    static void on_filter_year_clicked(lv_event_t* e);
    static void on_filter_all_clicked(lv_event_t* e);
    static void on_view_history_clicked(lv_event_t* e);

  private:
    //
    // === Widget References ===
    //

    // Filter buttons
    lv_obj_t* filter_day_ = nullptr;
    lv_obj_t* filter_week_ = nullptr;
    lv_obj_t* filter_month_ = nullptr;
    lv_obj_t* filter_year_ = nullptr;
    lv_obj_t* filter_all_ = nullptr;

    // Stat labels (2x2 grid)
    lv_obj_t* stat_total_prints_ = nullptr;
    lv_obj_t* stat_print_time_ = nullptr;
    lv_obj_t* stat_filament_ = nullptr;
    lv_obj_t* stat_success_rate_ = nullptr;

    // Containers
    lv_obj_t* stats_grid_ = nullptr;
    lv_obj_t* charts_section_ = nullptr;
    lv_obj_t* empty_state_ = nullptr;
    lv_obj_t* btn_view_history_ = nullptr;

    // Charts
    lv_obj_t* trend_chart_container_ = nullptr;
    lv_obj_t* trend_chart_ = nullptr;
    lv_chart_series_t* trend_series_ = nullptr;
    lv_obj_t* trend_period_label_ = nullptr;

    lv_obj_t* filament_chart_container_ = nullptr;
    // Filament bar rows stored for cleanup/refresh
    std::vector<lv_obj_t*> filament_bar_rows_;

    //
    // === Dependencies ===
    //

    PrintHistoryManager* history_manager_ = nullptr; ///< Shared history cache (DRY)

    //
    // === State ===
    //

    HistoryTimeFilter current_filter_ = HistoryTimeFilter::ALL_TIME;
    std::vector<PrintHistoryJob> cached_jobs_; ///< Time-filtered subset for get_cached_jobs()
    bool is_active_ = false;                   ///< Track if panel is currently visible

    // Parent screen reference
    lv_obj_t* parent_screen_ = nullptr;

    // Callback registration tracking
    bool callbacks_registered_ = false;

    // Connection state observer to auto-refresh when connected (ObserverGuard handles cleanup)
    ObserverGuard connection_observer_;

    /// Observer callback for history manager changes
    helix::HistoryChangedCallback history_observer_;

    // SubjectManager for automatic subject cleanup (RAII)
    SubjectManager subjects_;

    // Subject for empty state binding (must persist for LVGL binding lifetime)
    lv_subject_t history_has_jobs_subject_;

    // Boolean subjects for filter button state binding (L040: two bind_styles pattern)
    lv_subject_t history_filter_day_active_;
    lv_subject_t history_filter_week_active_;
    lv_subject_t history_filter_month_active_;
    lv_subject_t history_filter_year_active_;
    lv_subject_t history_filter_all_active_;

    // String subjects for stat labels
    lv_subject_t stat_total_prints_subject_;
    lv_subject_t stat_print_time_subject_;
    lv_subject_t stat_filament_subject_;
    lv_subject_t stat_success_rate_subject_;
    lv_subject_t trend_period_subject_;

    // Static buffers for string subjects (required for lv_subject_init_string)
    char stat_total_prints_buf_[32];
    char stat_print_time_buf_[32];
    char stat_filament_buf_[32];
    char stat_success_rate_buf_[16];
    char trend_period_buf_[32];

    //
    // === Data Fetching ===
    //

    /**
     * @brief Fetch history data from Moonraker with current filter
     */
    void refresh_data();

    /**
     * @brief Fetch server.history.totals for accurate ALL_TIME stats
     *
     * Called when filter is ALL_TIME. Uses Moonraker's server-computed
     * totals instead of summing the cached 500-job subset.
     */
    void fetch_totals_for_all_time();

    /**
     * @brief Update stats display using server-computed totals
     *
     * Uses PrintHistoryTotals for total_prints, time, filament.
     * Success rate is approximated from cached jobs since Moonraker
     * doesn't provide status breakdown in totals.
     *
     * @param totals Server-computed lifetime totals
     */
    void update_all_time_statistics(const PrintHistoryTotals& totals);

    /**
     * @brief Calculate and display statistics from job list
     *
     * @param jobs Vector of print history jobs
     */
    void update_statistics(const std::vector<PrintHistoryJob>& jobs);

    //
    // === Formatting Helpers ===
    //

    /**
     * @brief Format seconds as human-readable duration
     * @param seconds Duration in seconds
     * @return "2h 15m", "45m", "30s"
     */
    static std::string format_duration(double seconds);

    /**
     * @brief Format filament length for display
     * @param mm Filament in millimeters
     * @return "12.5m" or "1.2km"
     */
    static std::string format_filament(double mm);

    //
    // === Chart Helpers ===
    //

    /**
     * @brief Create the trend sparkline chart
     */
    void create_trend_chart();

    /**
     * @brief Create the filament bar chart
     */
    void create_filament_chart();

    /**
     * @brief Update trend chart with prints-per-day data
     * @param jobs The job list to analyze
     */
    void update_trend_chart(const std::vector<PrintHistoryJob>& jobs);

    /**
     * @brief Update filament chart with usage by type
     * @param jobs The job list to analyze
     */
    void update_filament_chart(const std::vector<PrintHistoryJob>& jobs);

    /**
     * @brief Get the number of periods for trend based on time filter
     * @return Number of data points (7 for day/week, more for longer ranges)
     */
    int get_trend_period_count() const;

    /**
     * @brief Get the seconds per period for trend calculation
     * @return Seconds per time bucket based on current filter
     */
    double get_trend_period_seconds() const;
};

/**
 * @brief Global instance accessor
 *
 * Creates instance on first call. Used by static callbacks.
 */
HistoryDashboardPanel& get_global_history_dashboard_panel();
