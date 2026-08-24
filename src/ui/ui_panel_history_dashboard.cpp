// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_history_dashboard.h"

#include "ui_callback_helpers.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_panel_history_list.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_client.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <sstream>

using namespace helix;

// ============================================================================
// Constants
// ============================================================================

namespace {

/// Filament-by-type bar row: share of the row width taken by each element.
///
/// These must sum to less than 100 so the row cannot overflow its card at any
/// width — the three inter-element gaps and the right-aligning spacer live in
/// the remainder. They replaced fixed pixel widths (50px type, 60px amount) that
/// needed 207px on the 170px card this chart gets at 480x272, which clipped the
/// amount label (prestonbrown/helixscreen#1204).
constexpr int FILAMENT_ROW_TYPE_PCT = 22;
constexpr int FILAMENT_ROW_BAR_MAX_PCT = 30;
constexpr int FILAMENT_ROW_AMOUNT_PCT = 36;
static_assert(FILAMENT_ROW_TYPE_PCT + FILAMENT_ROW_BAR_MAX_PCT + FILAMENT_ROW_AMOUNT_PCT < 100,
              "filament bar row shares must leave room for gaps and the spacer");

} // namespace

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<HistoryDashboardPanel> g_history_dashboard_panel;

HistoryDashboardPanel& get_global_history_dashboard_panel() {
    if (!g_history_dashboard_panel) {
        g_history_dashboard_panel = std::make_unique<HistoryDashboardPanel>();
        StaticPanelRegistry::instance().register_destroy(
            "HistoryDashboardPanel", []() { g_history_dashboard_panel.reset(); });
    }
    return *g_history_dashboard_panel;
}

// ============================================================================
// CONSTRUCTOR
// ============================================================================

HistoryDashboardPanel::HistoryDashboardPanel() : history_manager_(get_print_history_manager()) {
    spdlog::trace("[{}] Constructor", get_name());
}

// Destructor - cleanup subjects and observers
HistoryDashboardPanel::~HistoryDashboardPanel() {
    deinit_subjects();
    auto* mgr = get_print_history_manager();
    if (mgr && history_observer_) {
        mgr->remove_observer(&history_observer_);
        history_observer_ = nullptr;
    }
    // Guard against static destruction order fiasco (spdlog may be gone)
    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[HistoryDashboard] Destroyed");
    }
}

// ============================================================================
// Subject Initialization
// ============================================================================

void HistoryDashboardPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize subject for empty state visibility binding
    // 0 = no history (show empty state), 1 = has history (show stats grid)
    UI_MANAGED_SUBJECT_INT(history_has_jobs_subject_, 0, "history_has_jobs", subjects_);

    // Boolean subjects for filter button state binding (L040: two bind_styles pattern)
    // Default to ALL_TIME (only "all" button is active)
    UI_MANAGED_SUBJECT_INT(history_filter_day_active_, 0, "history_filter_day_active", subjects_);
    UI_MANAGED_SUBJECT_INT(history_filter_week_active_, 0, "history_filter_week_active", subjects_);
    UI_MANAGED_SUBJECT_INT(history_filter_month_active_, 0, "history_filter_month_active",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(history_filter_year_active_, 0, "history_filter_year_active", subjects_);
    UI_MANAGED_SUBJECT_INT(history_filter_all_active_, 1, "history_filter_all_active", subjects_);

    // Initialize string subjects for stat labels
    UI_MANAGED_SUBJECT_STRING(stat_total_prints_subject_, stat_total_prints_buf_, "0",
                              "stat_total_prints", subjects_);

    UI_MANAGED_SUBJECT_STRING(stat_print_time_subject_, stat_print_time_buf_, "0h",
                              "stat_print_time", subjects_);

    UI_MANAGED_SUBJECT_STRING(stat_filament_subject_, stat_filament_buf_, "0m", "stat_filament",
                              subjects_);

    UI_MANAGED_SUBJECT_STRING(stat_success_rate_subject_, stat_success_rate_buf_, "0%",
                              "stat_success_rate", subjects_);

    UI_MANAGED_SUBJECT_STRING(trend_period_subject_, trend_period_buf_, "Last 7 days",
                              "trend_period", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void HistoryDashboardPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // SubjectManager handles all subject cleanup via RAII
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

void HistoryDashboardPanel::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    // Register XML event callbacks
    register_xml_callbacks({
        {"history_filter_day_clicked", HistoryDashboardPanel::on_filter_day_clicked},
        {"history_filter_week_clicked", HistoryDashboardPanel::on_filter_week_clicked},
        {"history_filter_month_clicked", HistoryDashboardPanel::on_filter_month_clicked},
        {"history_filter_year_clicked", HistoryDashboardPanel::on_filter_year_clicked},
        {"history_filter_all_clicked", HistoryDashboardPanel::on_filter_all_clicked},
        {"history_view_full_clicked", HistoryDashboardPanel::on_view_history_clicked},
    });

    // Register row click callback for opening from Advanced panel
    lv_xml_register_event_cb(nullptr, "on_history_row_clicked", [](lv_event_t* /*e*/) {
        spdlog::debug("[History Dashboard] History row clicked");

        auto& overlay = get_global_history_dashboard_panel();

        // Ensure subjects and callbacks are initialized
        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();

        // Create the overlay if not already created
        lv_obj_t* screen = lv_screen_active();
        lv_obj_t* overlay_root = overlay.get_root();
        if (!overlay_root) {
            overlay_root = overlay.create(screen);
            if (!overlay_root) {
                spdlog::error("[History Dashboard] Failed to create dashboard panel");
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to open history"),
                                              2000);
                return;
            }
            // Register with NavigationManager for lifecycle callbacks
            NavigationManager::instance().register_overlay_instance(overlay_root, &overlay);
        }

        // Push as overlay (slides in from right)
        NavigationManager::instance().push_overlay(overlay_root);

        spdlog::debug("[History Dashboard] Dashboard panel opened");
    });

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* HistoryDashboardPanel::create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Creating overlay from XML", get_name());

    parent_screen_ = parent;

    // Reset cleanup flag when (re)creating
    cleanup_called_ = false;

    // Create overlay from XML
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "history_dashboard_panel", nullptr));

    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    // Find widget references - Filter buttons
    filter_day_ = lv_obj_find_by_name(overlay_root_, "filter_day");
    filter_week_ = lv_obj_find_by_name(overlay_root_, "filter_week");
    filter_month_ = lv_obj_find_by_name(overlay_root_, "filter_month");
    filter_year_ = lv_obj_find_by_name(overlay_root_, "filter_year");
    filter_all_ = lv_obj_find_by_name(overlay_root_, "filter_all");

    // Stat labels (2x2 grid)
    stat_total_prints_ = lv_obj_find_by_name(overlay_root_, "stat_total_prints");
    stat_print_time_ = lv_obj_find_by_name(overlay_root_, "stat_print_time");
    stat_filament_ = lv_obj_find_by_name(overlay_root_, "stat_filament");
    stat_success_rate_ = lv_obj_find_by_name(overlay_root_, "stat_success_rate");

    // Containers
    stats_grid_ = lv_obj_find_by_name(overlay_root_, "stats_grid");
    charts_section_ = lv_obj_find_by_name(overlay_root_, "charts_section");
    empty_state_ = lv_obj_find_by_name(overlay_root_, "empty_state");
    btn_view_history_ = lv_obj_find_by_name(overlay_root_, "btn_view_history");

    // Chart containers
    trend_chart_container_ = lv_obj_find_by_name(overlay_root_, "trend_chart_container");
    trend_period_label_ = lv_obj_find_by_name(overlay_root_, "trend_period");
    filament_chart_container_ = lv_obj_find_by_name(overlay_root_, "filament_chart_container");

    // Log found widgets
    spdlog::debug("[{}] Widget refs - filters: {}/{}/{}/{}/{}, stats: {}/{}/{}/{}", get_name(),
                  filter_day_ != nullptr, filter_week_ != nullptr, filter_month_ != nullptr,
                  filter_year_ != nullptr, filter_all_ != nullptr, stat_total_prints_ != nullptr,
                  stat_print_time_ != nullptr, stat_filament_ != nullptr,
                  stat_success_rate_ != nullptr);
    spdlog::debug("[{}] Chart containers: trend={}, filament={}", get_name(),
                  trend_chart_container_ != nullptr, filament_chart_container_ != nullptr);

    // Create charts inside their containers
    create_trend_chart();
    create_filament_chart();

    // Register connection state observer to auto-refresh when connected
    // This handles the case where the panel is opened before connection is established
    lv_subject_t* conn_subject = get_printer_state().get_printer_connection_state_subject();
    connection_observer_ = helix::ui::observe_int_sync<HistoryDashboardPanel>(
        conn_subject, this, [](HistoryDashboardPanel* self, int state) {
            if (state == static_cast<int>(ConnectionState::CONNECTED) && self->is_active_) {
                spdlog::debug("[{}] Connection established - refreshing data", self->get_name());
                self->refresh_data();
            }
        });

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void HistoryDashboardPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    is_active_ = true;

    // Register as observer of history manager to refresh when data changes
    // Guard: only register if not already registered (prevents double-registration)
    if (history_manager_ && !history_observer_) {
        history_observer_ = [this]() {
            if (is_active_) {
                spdlog::debug("[{}] History changed - refreshing data", get_name());
                refresh_data();
            }
        };
        history_manager_->add_observer(&history_observer_);
    }

    spdlog::debug("[{}] Activated - refreshing data with filter {}", get_name(),
                  static_cast<int>(current_filter_));
    refresh_data();
}

void HistoryDashboardPanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    is_active_ = false;

    // Remove observer to prevent callbacks when panel is not visible
    if (history_manager_ && history_observer_) {
        history_manager_->remove_observer(&history_observer_);
        history_observer_ = nullptr;
    }

    // Call base class last
    OverlayBase::on_deactivate();
}

// ============================================================================
// PUBLIC API
// ============================================================================

void HistoryDashboardPanel::set_time_filter(HistoryTimeFilter filter) {
    if (current_filter_ == filter) {
        return;
    }

    current_filter_ = filter;

    // Update boolean subjects for each button (L040: two bind_styles pattern)
    lv_subject_set_int(&history_filter_day_active_, filter == HistoryTimeFilter::DAY ? 1 : 0);
    lv_subject_set_int(&history_filter_week_active_, filter == HistoryTimeFilter::WEEK ? 1 : 0);
    lv_subject_set_int(&history_filter_month_active_, filter == HistoryTimeFilter::MONTH ? 1 : 0);
    lv_subject_set_int(&history_filter_year_active_, filter == HistoryTimeFilter::YEAR ? 1 : 0);
    lv_subject_set_int(&history_filter_all_active_, filter == HistoryTimeFilter::ALL_TIME ? 1 : 0);

    refresh_data();
}

// ============================================================================
// DATA FETCHING
// ============================================================================

void HistoryDashboardPanel::refresh_data() {
    if (!history_manager_) {
        spdlog::warn("[{}] No history manager available", get_name());
        return;
    }

    // If manager hasn't loaded data yet, trigger a fetch
    // The observer callback will call refresh_data() again when data arrives
    if (!history_manager_->is_loaded()) {
        spdlog::debug("[{}] History not loaded, triggering fetch", get_name());
        history_manager_->fetch();
        return;
    }

    // Calculate time range based on filter
    double since = 0.0;
    double now = static_cast<double>(std::time(nullptr));

    switch (current_filter_) {
    case HistoryTimeFilter::DAY:
        since = now - (24 * 60 * 60);
        break;
    case HistoryTimeFilter::WEEK:
        since = now - (7 * 24 * 60 * 60);
        break;
    case HistoryTimeFilter::MONTH:
        since = now - (30 * 24 * 60 * 60);
        break;
    case HistoryTimeFilter::YEAR:
        since = now - (365 * 24 * 60 * 60);
        break;
    case HistoryTimeFilter::ALL_TIME:
    default:
        since = 0.0;
        break;
    }

    spdlog::debug("[{}] Filtering history since {} (filter={})", get_name(), since,
                  static_cast<int>(current_filter_));

    // Get time-filtered jobs from manager (DRY: uses shared cache)
    cached_jobs_ = history_manager_->get_jobs_since(since);
    spdlog::info("[{}] Got {} jobs from manager (filter={})", get_name(), cached_jobs_.size(),
                 static_cast<int>(current_filter_));

    if (current_filter_ == HistoryTimeFilter::ALL_TIME) {
        // For ALL_TIME, use server.history.totals for accurate lifetime stats.
        // The cached 500 jobs can't represent the full history, but totals can.
        // Success rate is computed from cached jobs (best available approximation).
        fetch_totals_for_all_time();
    } else {
        update_statistics(cached_jobs_);
    }
}

void HistoryDashboardPanel::fetch_totals_for_all_time() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        // Fallback to cached jobs if no API
        update_statistics(cached_jobs_);
        return;
    }

    auto token = lifetime_.token();

    api->history().get_history_totals(
        [this, token](const PrintHistoryTotals& totals) {
            if (token.expired())
                return;

            token.defer("HistoryDashboard::totals_received",
                        [this, totals]() { update_all_time_statistics(totals); });
        },
        [this, token](const MoonrakerError& error) {
            if (token.expired())
                return;
            spdlog::warn("[{}] Failed to fetch totals, falling back to cached: {}", get_name(),
                         error.message);

            token.defer("HistoryDashboard::totals_fallback",
                        [this]() { update_statistics(cached_jobs_); });
        });
}

void HistoryDashboardPanel::update_all_time_statistics(const PrintHistoryTotals& totals) {
    lv_subject_set_int(&history_has_jobs_subject_, totals.total_jobs > 0 ? 1 : 0);

    if (totals.total_jobs == 0) {
        lv_subject_copy_string(&stat_total_prints_subject_, "0");
        lv_subject_copy_string(&stat_print_time_subject_, "0h");
        lv_subject_copy_string(&stat_filament_subject_, "0m");
        lv_subject_copy_string(&stat_success_rate_subject_, "0%");
        return;
    }

    // Total prints, time, filament from server-computed totals (accurate)
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(totals.total_jobs));
    lv_subject_copy_string(&stat_total_prints_subject_, buf);

    std::string time_str = format_duration(static_cast<double>(totals.total_time));
    lv_subject_copy_string(&stat_print_time_subject_, time_str.c_str());

    std::string filament_str = format_filament(totals.total_filament_used);
    lv_subject_copy_string(&stat_filament_subject_, filament_str.c_str());

    // Success rate: computed from cached jobs (best approximation available —
    // Moonraker's totals endpoint doesn't provide status breakdown)
    double success_rate = 0.0;
    if (!cached_jobs_.empty()) {
        uint64_t completed = 0;
        for (const auto& job : cached_jobs_) {
            if (job.status == PrintJobStatus::COMPLETED) {
                completed++;
            }
        }
        success_rate =
            (static_cast<double>(completed) / static_cast<double>(cached_jobs_.size())) * 100.0;
    }
    snprintf(buf, sizeof(buf), "%.0f%%", success_rate);
    lv_subject_copy_string(&stat_success_rate_subject_, buf);

    // Charts still use cached jobs (visual approximation)
    update_trend_chart(cached_jobs_);
    update_filament_chart(cached_jobs_);

    spdlog::debug("[{}] ALL_TIME stats from totals: {} prints, {} time, {} filament, ~{:.0f}% "
                  "success (from {} cached)",
                  get_name(), totals.total_jobs,
                  format_duration(static_cast<double>(totals.total_time)),
                  format_filament(totals.total_filament_used), success_rate, cached_jobs_.size());
}

void HistoryDashboardPanel::update_statistics(const std::vector<PrintHistoryJob>& jobs) {
    // Update subject to drive XML bindings (0=no jobs, 1=has jobs)
    // XML bindings will automatically show/hide stats, charts, and empty state
    lv_subject_set_int(&history_has_jobs_subject_, jobs.empty() ? 0 : 1);

    if (jobs.empty()) {
        // Clear stats via subjects (bindings will update UI automatically)
        lv_subject_copy_string(&stat_total_prints_subject_, "0");
        lv_subject_copy_string(&stat_print_time_subject_, "0h");
        lv_subject_copy_string(&stat_filament_subject_, "0m");
        lv_subject_copy_string(&stat_success_rate_subject_, "0%");
        return;
    }

    // Calculate statistics
    uint64_t total_prints = jobs.size();
    double total_time = 0.0;
    double total_filament = 0.0;
    uint64_t completed_count = 0;

    for (const auto& job : jobs) {
        total_time += job.print_duration;
        total_filament += job.filament_used;

        if (job.status == PrintJobStatus::COMPLETED) {
            completed_count++;
        }
    }

    // Calculate success rate
    double success_rate = 0.0;
    if (total_prints > 0) {
        success_rate =
            (static_cast<double>(completed_count) / static_cast<double>(total_prints)) * 100.0;
    }

    // Update stat subjects (bindings will update UI automatically)
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(total_prints));
    lv_subject_copy_string(&stat_total_prints_subject_, buf);

    std::string time_str = format_duration(total_time);
    lv_subject_copy_string(&stat_print_time_subject_, time_str.c_str());

    std::string filament_str = format_filament(total_filament);
    lv_subject_copy_string(&stat_filament_subject_, filament_str.c_str());

    snprintf(buf, sizeof(buf), "%.0f%%", success_rate);
    lv_subject_copy_string(&stat_success_rate_subject_, buf);

    // Update charts
    update_trend_chart(jobs);
    update_filament_chart(jobs);

    spdlog::debug("[{}] Stats updated: {} prints, {} time, {} filament, {:.0f}% success",
                  get_name(), total_prints, format_duration(total_time),
                  format_filament(total_filament), success_rate);
}

// ============================================================================
// FORMATTING HELPERS
// ============================================================================

std::string HistoryDashboardPanel::format_duration(double seconds) {
    return helix::format::duration(static_cast<int>(seconds));
}

std::string HistoryDashboardPanel::format_filament(double mm) {
    if (mm < 1000) {
        return std::to_string(static_cast<int>(mm)) + "mm";
    }

    double meters = mm / 1000.0;
    if (meters < 1000) {
        // Show 1 decimal for meters
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fm", meters);
        return buf;
    }

    // Kilometers for really large values
    double km = meters / 1000.0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1fkm", km);
    return buf;
}

// ============================================================================
// CHART HELPERS
// ============================================================================

void HistoryDashboardPanel::create_trend_chart() {
    if (!trend_chart_container_) {
        spdlog::warn("[{}] Trend chart container not found", get_name());
        return;
    }

    // Create line chart for prints trend
    trend_chart_ = lv_chart_create(trend_chart_container_);
    if (!trend_chart_) {
        spdlog::error("[{}] Failed to create trend chart", get_name());
        return;
    }

    // Configure chart - explicit height since container is height=content
    // Width fills parent, height is fixed since charts need explicit sizing
    lv_obj_set_size(trend_chart_, LV_PCT(100), 50);

    // Use line chart type
    lv_chart_set_type(trend_chart_, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(trend_chart_, static_cast<uint32_t>(get_trend_period_count()));

    // Styling for a clean sparkline look
    lv_obj_set_style_bg_opa(trend_chart_, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(trend_chart_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(trend_chart_, theme_manager_get_spacing("space_xxs"), LV_PART_MAIN);

    // Hide division lines for sparkline effect
    lv_chart_set_div_line_count(trend_chart_, 0, 0);

    // Series line style - use success color (gold) for visibility
    lv_color_t line_color = theme_manager_get_color("success");
    lv_obj_set_style_line_width(trend_chart_, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(trend_chart_, line_color, LV_PART_ITEMS);

    // Hide point indicators for cleaner sparkline
    lv_obj_set_style_width(trend_chart_, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(trend_chart_, 0, LV_PART_INDICATOR);

    // Add data series with gold color
    trend_series_ = lv_chart_add_series(trend_chart_, line_color, LV_CHART_AXIS_PRIMARY_Y);
    if (!trend_series_) {
        spdlog::error("[{}] Failed to create trend series", get_name());
        return;
    }

    // Initialize with zero data
    for (int i = 0; i < get_trend_period_count(); i++) {
        lv_chart_set_next_value(trend_chart_, trend_series_, 0);
    }

    spdlog::debug("[{}] Trend chart created with {} points", get_name(), get_trend_period_count());
}

void HistoryDashboardPanel::create_filament_chart() {
    if (!filament_chart_container_) {
        spdlog::warn("[{}] Filament chart container not found", get_name());
        return;
    }

    // Container is a flex column - rows will be added dynamically in update_filament_chart()
    spdlog::debug("[{}] Filament chart container ready for labeled bars", get_name());
}

int HistoryDashboardPanel::get_trend_period_count() const {
    // Number of data points for trend based on time filter
    switch (current_filter_) {
    case HistoryTimeFilter::DAY:
        return 24; // Hourly for day view
    case HistoryTimeFilter::WEEK:
        return 7; // Daily for week view
    case HistoryTimeFilter::MONTH:
        return 30; // Daily for month view
    case HistoryTimeFilter::YEAR:
        return 12; // Monthly for year view
    case HistoryTimeFilter::ALL_TIME:
    default:
        return 12; // Monthly-like buckets for all time
    }
}

double HistoryDashboardPanel::get_trend_period_seconds() const {
    // Seconds per time bucket based on current filter
    switch (current_filter_) {
    case HistoryTimeFilter::DAY:
        return 60 * 60; // 1 hour
    case HistoryTimeFilter::WEEK:
        return 24 * 60 * 60; // 1 day
    case HistoryTimeFilter::MONTH:
        return 24 * 60 * 60; // 1 day
    case HistoryTimeFilter::YEAR:
        return 30 * 24 * 60 * 60; // ~1 month
    case HistoryTimeFilter::ALL_TIME:
    default:
        return 24 * 60 * 60; // 1 day
    }
}

void HistoryDashboardPanel::update_trend_chart(const std::vector<PrintHistoryJob>& jobs) {
    if (!trend_chart_ || !trend_series_) {
        return;
    }

    int period_count = get_trend_period_count();
    double period_seconds = get_trend_period_seconds();
    double now = static_cast<double>(std::time(nullptr));

    // For ALL_TIME, calculate period dynamically from oldest job
    if (current_filter_ == HistoryTimeFilter::ALL_TIME && !jobs.empty()) {
        // Find oldest job
        double oldest_time = now;
        for (const auto& job : jobs) {
            if (job.end_time < oldest_time && job.end_time > 0) {
                oldest_time = job.end_time;
            }
        }
        // Calculate span and divide by bucket count
        double span = now - oldest_time;
        if (span > 0) {
            period_seconds = span / static_cast<double>(period_count);
        }
    }

    // Update period label text via subject (binding will update UI automatically)
    const char* period_text = "Last 7 days";
    switch (current_filter_) {
    case HistoryTimeFilter::DAY:
        period_text = "Last 24 hours";
        break;
    case HistoryTimeFilter::WEEK:
        period_text = "Last 7 days";
        break;
    case HistoryTimeFilter::MONTH:
        period_text = "Last 30 days";
        break;
    case HistoryTimeFilter::YEAR:
        period_text = "Last 12 months";
        break;
    case HistoryTimeFilter::ALL_TIME:
        period_text = "All time";
        break;
    }
    lv_subject_copy_string(&trend_period_subject_, period_text);

    // Count prints per period bucket
    std::vector<int> counts(static_cast<size_t>(period_count), 0);

    for (const auto& job : jobs) {
        // Calculate which period bucket this job falls into
        double age = now - job.end_time;
        if (age < 0)
            age = 0;

        int bucket = static_cast<int>(age / period_seconds);
        // Bucket 0 is most recent, bucket (period_count-1) is oldest
        // We want to display oldest on left, newest on right
        int display_bucket = period_count - 1 - bucket;
        if (display_bucket >= 0 && display_bucket < period_count) {
            counts[static_cast<size_t>(display_bucket)]++;
        }
    }

    // Find max for Y-axis scaling
    int max_count = 1;
    for (int count : counts) {
        if (count > max_count)
            max_count = count;
    }

    // Update chart point count if it changed
    if (lv_chart_get_point_count(trend_chart_) != static_cast<uint32_t>(period_count)) {
        lv_chart_set_point_count(trend_chart_, static_cast<uint32_t>(period_count));
    }

    // Set Y-axis range
    lv_chart_set_axis_range(trend_chart_, LV_CHART_AXIS_PRIMARY_Y, 0, max_count);

    // Update series data - use lv_chart_set_value_by_id for precise control
    for (int i = 0; i < period_count; i++) {
        lv_chart_set_value_by_id(trend_chart_, trend_series_, static_cast<uint32_t>(i),
                                 counts[static_cast<size_t>(i)]);
    }

    lv_chart_refresh(trend_chart_);

    spdlog::debug("[{}] Trend chart updated: {} periods, max={}", get_name(), period_count,
                  max_count);
}

void HistoryDashboardPanel::update_filament_chart(const std::vector<PrintHistoryJob>& jobs) {
    if (!filament_chart_container_) {
        return;
    }

    // Clear existing bar rows
    for (auto* row : filament_bar_rows_) {
        helix::ui::safe_delete(row);
    }
    filament_bar_rows_.clear();

    // Aggregate filament by type
    std::map<std::string, double> filament_by_type;

    for (const auto& job : jobs) {
        if (job.filament_type.empty()) {
            filament_by_type["Unknown"] += job.filament_used;
            continue;
        }

        // Split semicolon-separated filament types (OrcaSlicer multi-extruder format)
        std::vector<std::string> types;
        std::stringstream ss(job.filament_type);
        std::string item;
        while (std::getline(ss, item, ';')) {
            // Trim whitespace
            size_t start = item.find_first_not_of(" \t");
            size_t end = item.find_last_not_of(" \t");
            if (start != std::string::npos) {
                types.push_back(item.substr(start, end - start + 1));
            }
        }

        if (types.empty()) {
            filament_by_type["Unknown"] += job.filament_used;
        } else {
            // Distribute filament proportionally among all extruders
            double per_extruder = job.filament_used / static_cast<double>(types.size());
            for (const auto& type : types) {
                filament_by_type[type.empty() ? "Unknown" : type] += per_extruder;
            }
        }
    }

    if (filament_by_type.empty()) {
        return;
    }

    // Sort by usage (highest first) and take top 4 (limited space in side panel)
    std::vector<std::pair<std::string, double>> sorted_types(filament_by_type.begin(),
                                                             filament_by_type.end());
    std::sort(sorted_types.begin(), sorted_types.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    if (sorted_types.size() > 4) {
        sorted_types.resize(4);
    }

    // Find max for proportional bar widths
    double max_filament = 1.0;
    for (const auto& [type, amount] : sorted_types) {
        if (amount > max_filament)
            max_filament = amount;
    }

    // Generate complementary palette from theme's primary color
    // This creates visually harmonious colors that fit the theme
    lv_color_t primary = theme_manager_get_color("primary");

    // Convert primary to HSV to generate palette
    lv_color_hsv_t primary_hsv = lv_color_to_hsv(primary);

    // Generate colors by rotating hue - creates triadic/complementary harmony
    // Each filament type gets a consistent hue offset based on name hash
    auto get_filament_color = [primary_hsv](const std::string& type) -> lv_color_t {
        // Hash the type name for consistent color assignment
        uint32_t hash = 0;
        for (char c : type) {
            // Use uppercase for consistency
            char uc = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            hash = hash * 31 + static_cast<uint32_t>(uc);
        }

        // Rotate hue by hash-based offset (evenly distributed around color wheel)
        // Use golden ratio angle (137.5 deg) for good distribution
        uint16_t hue_offset = static_cast<uint16_t>((hash * 137) % 360);
        uint16_t new_hue = (primary_hsv.h + hue_offset) % 360;

        // Keep saturation and value similar to primary for visual harmony
        // Slightly increase saturation for better visibility on dark backgrounds
        uint8_t sat = std::min(static_cast<uint8_t>(primary_hsv.s + 10), static_cast<uint8_t>(100));
        uint8_t val = std::max(primary_hsv.v, static_cast<uint8_t>(70)); // Ensure brightness

        return lv_color_hsv_to_rgb(new_hue, sat, val);
    };

    // Get theme color for text labels
    lv_color_t text = theme_manager_get_color("text");

    // Create labeled bar rows
    for (const auto& [type, amount] : sorted_types) {
        // Calculate bar width as percentage of max (leaving room for label and amount)
        int bar_pct = static_cast<int>((amount / max_filament) * 100);
        if (bar_pct < 5)
            bar_pct = 5; // Minimum visible width

        // Create row container
        lv_obj_t* row = lv_obj_create(filament_chart_container_);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(row, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        filament_bar_rows_.push_back(row);

        // Use readable font size (small but legible)
        const lv_font_t* font_small = lv_xml_get_font(nullptr, "montserrat_14");
        if (!font_small) {
            font_small = lv_xml_get_font(nullptr, "montserrat_12");
        }

        // Type label.
        //
        // Proportional, not a fixed pixel width. The row used to be 50px (type) +
        // up to 50% (bar) + 60px (amount) + 12px of gaps, which needs 207px on the
        // 170px-wide card this chart gets at 480x272 — so the amount label was
        // pushed past the card edge and rendered clipped ("10" showing as "1(").
        // Percentages that sum under 100 cannot overflow at any card width; the
        // three shares below total 88%, leaving the gaps and the spacer room.
        lv_obj_t* type_label = lv_label_create(row);
        lv_label_set_text(type_label, type.c_str());
        lv_obj_set_width(type_label, LV_PCT(FILAMENT_ROW_TYPE_PCT));
        lv_obj_set_style_text_color(type_label, text, 0);
        if (font_small) {
            lv_obj_set_style_text_font(type_label, font_small, 0);
        }

        // Get line height from font to match text size
        int32_t line_height = font_small ? lv_font_get_line_height(font_small) : 16;

        // Colored bar - width proportional to value (max FILAMENT_ROW_BAR_MAX_PCT
        // of the row). Using percentage width ensures bars are proportional across
        // all rows.
        int bar_width_pct = (bar_pct * FILAMENT_ROW_BAR_MAX_PCT) / 100;
        if (bar_width_pct < 3)
            bar_width_pct = 3; // Minimum visibility

        lv_obj_t* bar = lv_obj_create(row);
        lv_obj_set_size(bar, LV_PCT(bar_width_pct), line_height);
        lv_color_t bar_color = get_filament_color(type);
        lv_obj_set_style_bg_color(bar, bar_color, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        // Spacer to push amount to right edge (fills remaining space)
        lv_obj_t* spacer = lv_obj_create(row);
        lv_obj_set_height(spacer, 1);
        lv_obj_set_flex_grow(spacer, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_0, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_set_style_pad_all(spacer, 0, 0);
        lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

        // Amount label (fixed width, right-aligned text at row's right edge)
        lv_obj_t* amount_label = lv_label_create(row);
        std::string amount_str = format_filament(amount);
        lv_label_set_text(amount_label, amount_str.c_str());
        lv_obj_set_width(amount_label, LV_PCT(FILAMENT_ROW_AMOUNT_PCT));
        lv_obj_set_style_text_color(amount_label, text, 0);
        lv_obj_set_style_text_align(amount_label, LV_TEXT_ALIGN_RIGHT, 0);
        if (font_small) {
            lv_obj_set_style_text_font(amount_label, font_small, 0);
        }

        spdlog::debug("[{}] Filament bar: {} = {} ({}%)", get_name(), type, amount_str, bar_pct);
    }

    spdlog::debug("[{}] Filament chart updated: {} types", get_name(), sorted_types.size());
}

// ============================================================================
// STATIC EVENT CALLBACKS
// ============================================================================

void HistoryDashboardPanel::on_filter_day_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[History Dashboard] Filter: Day clicked");
    get_global_history_dashboard_panel().set_time_filter(HistoryTimeFilter::DAY);
}

void HistoryDashboardPanel::on_filter_week_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[History Dashboard] Filter: Week clicked");
    get_global_history_dashboard_panel().set_time_filter(HistoryTimeFilter::WEEK);
}

void HistoryDashboardPanel::on_filter_month_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[History Dashboard] Filter: Month clicked");
    get_global_history_dashboard_panel().set_time_filter(HistoryTimeFilter::MONTH);
}

void HistoryDashboardPanel::on_filter_year_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[History Dashboard] Filter: Year clicked");
    get_global_history_dashboard_panel().set_time_filter(HistoryTimeFilter::YEAR);
}

void HistoryDashboardPanel::on_filter_all_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[History Dashboard] Filter: All clicked");
    get_global_history_dashboard_panel().set_time_filter(HistoryTimeFilter::ALL_TIME);
}

void HistoryDashboardPanel::on_view_history_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[History Dashboard] View Full History clicked");

    // Get the list panel instance
    auto& list_panel = get_global_history_list_panel();

    // Pass the cached jobs to avoid redundant API calls
    const auto& dashboard = get_global_history_dashboard_panel();
    list_panel.set_jobs(dashboard.get_cached_jobs());

    // Ensure subjects and callbacks are initialized
    if (!list_panel.are_subjects_initialized()) {
        list_panel.init_subjects();
    }
    list_panel.register_callbacks();

    // Create the overlay if not already created
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* overlay_root = list_panel.get_root();
    if (!overlay_root) {
        overlay_root = list_panel.create(screen);
        if (!overlay_root) {
            spdlog::error("[History Dashboard] Failed to create history list panel");
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Failed to open history list"), 2000);
            return;
        }
        // Register with NavigationManager for lifecycle callbacks
        NavigationManager::instance().register_overlay_instance(overlay_root, &list_panel);
    }

    // Push as overlay (slides in from right)
    NavigationManager::instance().push_overlay(overlay_root);

    spdlog::debug("[History Dashboard] History list panel opened");
}
