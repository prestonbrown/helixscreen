// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_history_list.h"

#include "ui_callback_helpers.h"
#include "ui_filename_utils.h"
#include "ui_fonts.h"
#include "ui_format_utils.h"
#include "ui_nav_manager.h"
#include "ui_notification.h"
#include "ui_panel_common.h"
#include "ui_panel_print_select.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_client.h"
#include "observer_factory.h"
#include "print_history_manager.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "thumbnail_cache.h"
#include "ui/ui_cleanup_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <lvgl.h>
#include <map>

using namespace helix;

// MDI chevron-down symbol for dropdown arrows (replaces FontAwesome LV_SYMBOL_DOWN)
static const char* MDI_CHEVRON_DOWN = "\xF3\xB0\x85\x80"; // F0140

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<HistoryListPanel> g_history_list_panel;

HistoryListPanel& get_global_history_list_panel() {
    if (!g_history_list_panel) {
        g_history_list_panel = std::make_unique<HistoryListPanel>();
        StaticPanelRegistry::instance().register_destroy("HistoryListPanel",
                                                         []() { g_history_list_panel.reset(); });
    }
    return *g_history_list_panel;
}

// ============================================================================
// Constructor
// ============================================================================

HistoryListPanel::HistoryListPanel() : history_manager_(get_print_history_manager()) {
    spdlog::trace("[{}] Constructor", get_name());
}

// Destructor - remove observer from history manager
HistoryListPanel::~HistoryListPanel() {
    deinit_subjects();
    auto* mgr = get_print_history_manager();
    if (mgr && history_observer_) {
        mgr->remove_observer(&history_observer_);
        history_observer_ = nullptr;
    }
    if (search_timer_) {
        lv_timer_delete(search_timer_);
        search_timer_ = nullptr;
    }
    // Guard against static destruction order fiasco (spdlog may be gone)
    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[HistoryListPanel] Destroyed");
    }
}

// ============================================================================
// Subject Initialization
// ============================================================================

void HistoryListPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize subject for panel state binding (0=LOADING, 1=EMPTY, 2=HAS_JOBS)
    UI_MANAGED_SUBJECT_INT(subject_panel_state_, 0, "history_list_panel_state", subjects_);

    // Collapsible filter dropdowns: 0 = collapsed (default), 1 = expanded
    UI_MANAGED_SUBJECT_INT(subject_filters_expanded_, 0, "history_filters_expanded", subjects_);

    // Funnel accent driver: 1 when a status/search/sort filter is active.
    // The XML binds two <style>s to this via bind_style; C++ only sets the value.
    UI_MANAGED_SUBJECT_INT(subject_filter_active_, 0, "history_filter_active", subjects_);

    // Initialize empty state message subjects
    UI_MANAGED_SUBJECT_STRING(subject_empty_message_, empty_message_buf_, "No print history found",
                              "history_empty_message", subjects_);
    UI_MANAGED_SUBJECT_STRING(subject_empty_hint_, empty_hint_buf_,
                              "Completed prints will appear here", "history_empty_hint", subjects_);

    // Initialize detail overlay subjects
    init_detail_subjects();

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void HistoryListPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // SubjectManager handles all subject cleanup via RAII
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[HistoryListPanel] Subjects deinitialized");
}

// ============================================================================
// Callback Registration
// ============================================================================

void HistoryListPanel::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    // Register XML event callbacks for search, filter, sort, and detail overlay
    register_xml_callbacks({
        {"history_search_changed",
         [](lv_event_t* /*e*/) { get_global_history_list_panel().on_search_changed(); }},
        {"history_search_clear",
         [](lv_event_t* /*e*/) { get_global_history_list_panel().on_search_clear(); }},
        {"history_filter_status_changed",
         [](lv_event_t* e) {
             lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
             if (dropdown) {
                 int index = lv_dropdown_get_selected(dropdown);
                 get_global_history_list_panel().on_status_filter_changed(index);
             }
         }},
        {"history_sort_changed",
         [](lv_event_t* e) {
             lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
             if (dropdown) {
                 int index = lv_dropdown_get_selected(dropdown);
                 get_global_history_list_panel().on_sort_changed(index);
             }
         }},
        {"history_filters_toggle",
         [](lv_event_t* /*e*/) { get_global_history_list_panel().toggle_filters(); }},
        {"history_detail_reprint",
         [](lv_event_t* /*e*/) { get_global_history_list_panel().handle_reprint(); }},
        {"history_detail_delete",
         [](lv_event_t* /*e*/) { get_global_history_list_panel().handle_delete(); }},
        {"history_detail_view_timelapse",
         [](lv_event_t* /*e*/) { get_global_history_list_panel().handle_view_timelapse(); }},
    });

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* HistoryListPanel::create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Creating overlay from XML", get_name());

    parent_screen_ = parent;

    // Reset cleanup flag when (re)creating
    cleanup_called_ = false;

    // Create overlay from XML
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "history_list_panel", nullptr));

    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    // Get widget references - list containers
    list_content_ = lv_obj_find_by_name(overlay_root_, "list_content");
    list_rows_ = lv_obj_find_by_name(overlay_root_, "list_rows");
    empty_state_ = lv_obj_find_by_name(overlay_root_, "empty_state");

    // Get widget references - filter controls
    search_box_ = lv_obj_find_by_name(overlay_root_, "search_box");
    filter_status_ = lv_obj_find_by_name(overlay_root_, "filter_status");
    sort_dropdown_ = lv_obj_find_by_name(overlay_root_, "sort_dropdown");

    // Seed the funnel accent state (the XML bind_style rules react to the subject).
    refresh_filter_indicator();

    spdlog::debug("[{}] Widget refs - content: {}, rows: {}, empty: {}", get_name(),
                  list_content_ != nullptr, list_rows_ != nullptr, empty_state_ != nullptr);
    spdlog::debug("[{}] Filter refs - search: {}, status: {}, sort: {}", get_name(),
                  search_box_ != nullptr, filter_status_ != nullptr, sort_dropdown_ != nullptr);

    // Set MDI chevron icons for dropdowns (Noto Sans doesn't have LV_SYMBOL_DOWN)
    // Must set BOTH the symbol AND the indicator font to MDI for the symbol to render
    const char* icon_font_name = lv_xml_get_const(nullptr, "icon_font_md");
    const lv_font_t* icon_font =
        icon_font_name ? lv_xml_get_font(nullptr, icon_font_name) : &mdi_icons_24;

    if (filter_status_) {
        lv_dropdown_set_symbol(filter_status_, MDI_CHEVRON_DOWN);
        lv_obj_set_style_text_font(filter_status_, icon_font, LV_PART_INDICATOR);
    }
    if (sort_dropdown_) {
        lv_dropdown_set_symbol(sort_dropdown_, MDI_CHEVRON_DOWN);
        lv_obj_set_style_text_font(sort_dropdown_, icon_font, LV_PART_INDICATOR);
    }

    // Attach scroll event handler for infinite scroll
    if (list_content_) {
        lv_obj_add_event_cb(list_content_, on_scroll_static, LV_EVENT_SCROLL_END, this);
        // Virtual scroll: update visible rows as user scrolls
        lv_obj_add_event_cb(list_content_, on_scroll_update_visible, LV_EVENT_SCROLL, this);
    }

    // Register connection state observer to auto-refresh when connected
    // This handles the case where the panel is opened before connection is established
    lv_subject_t* conn_subject = get_printer_state().get_printer_connection_state_subject();
    connection_observer_ = helix::ui::observe_int_sync<HistoryListPanel>(
        conn_subject, this, [](HistoryListPanel* self, int state) {
            if (state == static_cast<int>(ConnectionState::CONNECTED) && self->is_active_ &&
                !self->jobs_received_) {
                spdlog::debug("[{}] Connection established - refreshing data", self->get_name());
                self->refresh_from_api();
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

void HistoryListPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    is_active_ = true;
    spdlog::debug("[{}] Activated - jobs_received: {}, job_count: {}, detail_was_open: {}, "
                  "history_changed: {}",
                  get_name(), jobs_received_, jobs_.size(), detail_overlay_open_,
                  history_changed_while_detail_open_);

    // Skip refresh when returning from detail overlay if no history changed
    // This preserves scroll position by avoiding unnecessary repopulate
    if (detail_overlay_open_ && !history_changed_while_detail_open_) {
        spdlog::debug("[{}] Returning from detail overlay, no history changes - skipping refresh",
                      get_name());
        detail_overlay_open_ = false;
        history_changed_while_detail_open_ = false;
        return;
    }

    // Clear flags after checking
    detail_overlay_open_ = false;
    history_changed_while_detail_open_ = false;

    // Register as history manager observer if manager available
    if (history_manager_ && !history_observer_) {
        history_observer_ = [this]() {
            if (!is_active_) {
                return;
            }

            // If detail overlay is open, just mark that history changed - will refresh on return
            if (detail_overlay_open_) {
                history_changed_while_detail_open_ = true;
                spdlog::debug("[{}] History changed while detail open, deferring refresh",
                              get_name());
                return;
            }

            spdlog::debug("[{}] History manager notified - refreshing", get_name());
            // Get fresh data from manager and re-apply filters
            if (history_manager_->is_loaded()) {
                jobs_ = history_manager_->get_jobs();
                apply_filters_and_sort();
            }
        };
        history_manager_->add_observer(&history_observer_);
    }

    // Try to use manager data first (shared cache - DRY)
    if (history_manager_ && history_manager_->is_loaded()) {
        jobs_ = history_manager_->get_jobs();
        jobs_received_ = true;
        spdlog::debug("[{}] Using {} jobs from shared manager cache", get_name(), jobs_.size());
        apply_filters_and_sort();
    } else if (!jobs_received_) {
        // Show loading state while fetching from API
        lv_subject_set_int(&subject_panel_state_, 0); // LOADING

        // Trigger manager fetch if available, otherwise direct API call
        if (history_manager_) {
            spdlog::debug("[{}] Manager not loaded, triggering fetch", get_name());
            history_manager_->fetch();
        } else {
            // Fallback: Jobs weren't set by dashboard, fetch from API
            refresh_from_api();
        }
    } else {
        // Jobs were provided via set_jobs(), apply filters and populate the list
        apply_filters_and_sort();
    }
}

void HistoryListPanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    is_active_ = false;

    // Remove history manager observer
    if (history_manager_ && history_observer_) {
        history_manager_->remove_observer(&history_observer_);
        history_observer_ = nullptr;
    }

    // Cancel any pending search timer
    helix::ui::safe_delete_timer(search_timer_);

    // Reset filter state for fresh start on next activation
    search_query_.clear();
    status_filter_ = HistoryStatusFilter::ALL;
    sort_column_ = HistorySortColumn::DATE;
    sort_direction_ = HistorySortDirection::DESC;

    // Reset filter control widgets if available
    // (text_input handles clear button visibility internally via lv_textarea_set_text)
    if (search_box_) {
        lv_textarea_set_text(search_box_, "");
    }
    if (filter_status_) {
        lv_dropdown_set_selected(filter_status_, 0);
    }
    if (sort_dropdown_) {
        lv_dropdown_set_selected(sort_dropdown_, 0);
    }

    // Clear the received flag so next activation will refresh
    jobs_received_ = false;

    // Reset pagination state
    total_job_count_ = 0;
    load_more_guard_.release();
    has_more_data_ = true;

    // Reset virtual scroll view
    if (list_view_) {
        list_view_->reset();
    }

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// Public API
// ============================================================================

void HistoryListPanel::set_jobs(const std::vector<PrintHistoryJob>& jobs) {
    jobs_ = jobs;
    jobs_received_ = true;
    spdlog::debug("[{}] Jobs set: {} items", get_name(), jobs_.size());
}

void HistoryListPanel::refresh_from_api() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] Cannot refresh: API not set", get_name());
        return;
    }

    // Check if WebSocket is actually connected before attempting to send requests
    // This prevents the race condition where the panel is opened before connection is established
    ConnectionState state = api->get_connection_state();
    if (state != ConnectionState::CONNECTED) {
        spdlog::debug("[{}] Cannot fetch history: not connected (state={})", get_name(),
                      static_cast<int>(state));
        return;
    }

    // Reset pagination state for fresh fetch
    jobs_.clear();
    total_job_count_ = 0;
    has_more_data_ = true;
    load_more_guard_.release();

    spdlog::debug("[{}] Fetching first page of history (limit={})", get_name(), PAGE_SIZE);

    api->history().get_history_list(
        PAGE_SIZE, // limit - use page size
        0,         // start - first page
        0.0,       // since (no filter)
        0.0,       // before (no filter)
        [this](const std::vector<PrintHistoryJob>& jobs, uint64_t total) {
            spdlog::info("[{}] Received {} jobs (total: {})", get_name(), jobs.size(), total);
            jobs_ = jobs;
            total_job_count_ = total;
            has_more_data_ = (jobs_.size() < total);

            // Fetch timelapse files and associate them with jobs (calls apply_filters_and_sort)
            fetch_timelapse_files();
        },
        [this](const MoonrakerError& error) {
            spdlog::error("[{}] Failed to fetch history: {}", get_name(), error.message);
            jobs_.clear();
            total_job_count_ = 0;
            has_more_data_ = false;
            apply_filters_and_sort();
        });
}

void HistoryListPanel::load_more() {
    IMoonrakerAPI* api = get_moonraker_api();
    // A healthy in-flight page load short-circuits; a stuck one (response lost
    // >30s ago) falls through and is recovered by try_acquire() below.
    if (!api || (load_more_guard_.active() && !load_more_guard_.is_stuck()) || !has_more_data_) {
        return;
    }

    // Check if WebSocket is connected
    ConnectionState state = api->get_connection_state();
    if (state != ConnectionState::CONNECTED) {
        spdlog::debug("[{}] Cannot load more: not connected", get_name());
        return;
    }

    if (load_more_guard_.try_acquire() == helix::InFlightGuard::AcquireResult::RecoveredStuck) {
        spdlog::warn("[{}] load-more in-flight flag stuck for {}ms — treating prior response as "
                     "lost and retrying",
                     get_name(), load_more_guard_.stuck_threshold().count());
    }
    int start_offset = static_cast<int>(jobs_.size());

    spdlog::debug("[{}] Loading more jobs (start={}, limit={})", get_name(), start_offset,
                  PAGE_SIZE);

    api->history().get_history_list(
        PAGE_SIZE,    // limit
        start_offset, // start - continue from where we left off
        0.0,          // since (no filter)
        0.0,          // before (no filter)
        [this](const std::vector<PrintHistoryJob>& new_jobs, uint64_t total) {
            load_more_guard_.release();
            total_job_count_ = total;

            if (new_jobs.empty()) {
                has_more_data_ = false;
                spdlog::debug("[{}] No more jobs to load", get_name());
                return;
            }

            spdlog::info("[{}] Loaded {} more jobs (now have {}, total: {})", get_name(),
                         new_jobs.size(), jobs_.size() + new_jobs.size(), total);

            // Append new jobs
            jobs_.insert(jobs_.end(), new_jobs.begin(), new_jobs.end());

            // Check if we've loaded everything
            has_more_data_ = (jobs_.size() < total);

            // Re-apply filters to the full job list
            apply_filters_and_sort();

            // Note: apply_filters_and_sort calls populate_list which rebuilds UI
            // For smoother infinite scroll, we could optimize this to only append
        },
        [this](const MoonrakerError& error) {
            load_more_guard_.release();
            spdlog::error("[{}] Failed to load more history: {}", get_name(), error.message);
        });
}

void HistoryListPanel::fetch_timelapse_files() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        apply_filters_and_sort();
        return;
    }

    // List files in the timelapse directory
    api->files().list_files(
        "timelapse", // root
        "",          // path (root)
        false,       // non-recursive
        [this](const std::vector<FileInfo>& timelapse_files) {
            spdlog::debug("[{}] Found {} timelapse files", get_name(), timelapse_files.size());
            associate_timelapse_files(timelapse_files);
            apply_filters_and_sort();
        },
        [this](const MoonrakerError& error) {
            spdlog::debug("[{}] No timelapse files available: {}", get_name(), error.message);
            // Continue without timelapse association - this is not an error
            apply_filters_and_sort();
        });
}

void HistoryListPanel::associate_timelapse_files(const std::vector<FileInfo>& timelapse_files) {
    if (timelapse_files.empty() || jobs_.empty()) {
        return;
    }

    // Build a map of base filename (without extension) -> timelapse file path
    // Timelapse files are typically named like "print_name_timestamp.mp4"
    std::map<std::string, std::string> timelapse_map;
    for (const auto& tf : timelapse_files) {
        if (tf.is_dir)
            continue;

        // Skip non-video files
        std::string name_lower = tf.filename;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name_lower.find(".mp4") == std::string::npos &&
            name_lower.find(".webm") == std::string::npos &&
            name_lower.find(".avi") == std::string::npos) {
            continue;
        }

        timelapse_map[tf.filename] = "timelapse/" + tf.filename;
        spdlog::trace("[{}] Timelapse file: {}", get_name(), tf.filename);
    }

    // Match timelapse files to jobs
    // Strategy: Check if job filename (without .gcode) is contained in timelapse filename
    for (auto& job : jobs_) {
        if (job.filename.empty())
            continue;

        // Get job filename without extension and path. Must cover .gcode/.gco/
        // .g/.3mf case-insensitively — a leftover extension is never present in
        // the video's name, so the match below would silently never fire.
        std::string job_base = helix::gcode::get_display_filename(job.filename);

        // Convert to lowercase for comparison
        std::string job_base_lower = job_base;
        std::transform(job_base_lower.begin(), job_base_lower.end(), job_base_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Search for a timelapse file that contains this job's base name
        for (const auto& [tf_name, tf_path] : timelapse_map) {
            std::string tf_lower = tf_name;
            std::transform(tf_lower.begin(), tf_lower.end(), tf_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            if (tf_lower.find(job_base_lower) != std::string::npos) {
                job.timelapse_filename = tf_path;
                job.has_timelapse = true;
                spdlog::debug("[{}] Associated timelapse '{}' with job '{}'", get_name(), tf_name,
                              job.filename);
                break; // One timelapse per job
            }
        }
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void HistoryListPanel::populate_list() {
    if (!list_rows_) {
        spdlog::error("[{}] Cannot populate: list_rows container is null", get_name());
        return;
    }

    // Update empty state
    update_empty_state();

    if (filtered_jobs_.empty()) {
        if (list_view_) {
            list_view_->reset();
        }
        spdlog::debug("[{}] No jobs to display after filtering", get_name());
        return;
    }

    if (!list_view_) {
        list_view_ = std::make_unique<helix::ui::HistoryListView>();
        list_view_->setup(list_rows_, list_content_,
                          [this](size_t index) { handle_row_click(index); });
    }

    list_view_->populate(filtered_jobs_);

    spdlog::debug("[{}] List populated with {} jobs via virtual scroll", get_name(),
                  filtered_jobs_.size());
}

void HistoryListPanel::clear_list() {
    if (list_view_) {
        list_view_->reset();
    }
}

void HistoryListPanel::update_empty_state() {
    // Determine panel state and update subject declaratively
    // State values: 0=LOADING, 1=EMPTY, 2=HAS_JOBS
    int state;
    bool has_filtered_jobs = !filtered_jobs_.empty();

    if (has_filtered_jobs) {
        state = 2; // HAS_JOBS
    } else {
        state = 1; // EMPTY
    }

    lv_subject_set_int(&subject_panel_state_, state);

    // Update empty state message based on whether filters are active
    if (!has_filtered_jobs) {
        bool filters_active = !search_query_.empty() || status_filter_ != HistoryStatusFilter::ALL;

        if (filters_active) {
            // Filters are active but yielded no results
            lv_subject_copy_string(&subject_empty_message_, lv_tr("No matching prints"));
            lv_subject_copy_string(&subject_empty_hint_,
                                   lv_tr("Try adjusting your search or filters"));
        } else if (jobs_.empty()) {
            // No jobs at all
            lv_subject_copy_string(&subject_empty_message_, lv_tr("No print history found"));
            lv_subject_copy_string(&subject_empty_hint_,
                                   lv_tr("Completed prints will appear here"));
        }
    }

    spdlog::debug("[{}] Panel state updated: state={}, has_filtered_jobs={}, total_jobs={}",
                  get_name(), state, has_filtered_jobs, jobs_.size());
}

const char* HistoryListPanel::get_status_color(PrintJobStatus status) {
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

const char* HistoryListPanel::get_status_text(PrintJobStatus status) {
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

// ============================================================================
// Click Handlers
// ============================================================================

void HistoryListPanel::attach_row_click_handler(lv_obj_t* row, size_t index) {
    // Store index in user data (cast to void* for LVGL)
    // This matches the pattern used by PrintSelectPanel
    lv_obj_set_user_data(row, reinterpret_cast<void*>(index));
    lv_obj_add_event_cb(row, on_row_clicked_static, LV_EVENT_CLICKED, this);
}

void HistoryListPanel::on_row_clicked_static(lv_event_t* e) {
    // Get panel instance from event user data
    HistoryListPanel* panel = static_cast<HistoryListPanel*>(lv_event_get_user_data(e));
    // Get the row that was clicked (target of the event)
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_event_get_target(e));

    if (!panel || !row)
        return;

    // Get the index stored in the row's user data
    size_t index = reinterpret_cast<size_t>(lv_obj_get_user_data(row));
    panel->handle_row_click(index);
}

void HistoryListPanel::handle_row_click(size_t index) {
    if (index >= filtered_jobs_.size()) {
        spdlog::warn("[{}] Invalid row index: {}", get_name(), index);
        return;
    }

    selected_job_index_ = index;
    const auto& job = filtered_jobs_[index];
    spdlog::info("[{}] Row clicked: {} ({})", get_name(), job.filename,
                 get_status_text(job.status));

    show_detail_overlay(job);
}

// ============================================================================
// Filter/Sort Implementation
// ============================================================================

void HistoryListPanel::apply_filters_and_sort() {
    spdlog::debug("[{}] Applying filters - search: '{}', status: {}, sort: {} {}", get_name(),
                  search_query_, static_cast<int>(status_filter_), static_cast<int>(sort_column_),
                  sort_direction_ == HistorySortDirection::DESC ? "DESC" : "ASC");

    // Keep the funnel accent in sync with the active-filter state.
    refresh_filter_indicator();

    // Chain: search -> status -> sort
    auto result = apply_search_filter(jobs_);
    result = apply_status_filter(result);
    apply_sort(result);

    filtered_jobs_ = std::move(result);

    spdlog::debug("[{}] Filter result: {} jobs -> {} filtered", get_name(), jobs_.size(),
                  filtered_jobs_.size());

    populate_list();
}

std::vector<PrintHistoryJob>
HistoryListPanel::apply_search_filter(const std::vector<PrintHistoryJob>& source) {
    if (search_query_.empty()) {
        return source;
    }

    // Case-insensitive search
    std::string query_lower = search_query_;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::vector<PrintHistoryJob> result;
    result.reserve(source.size());

    for (const auto& job : source) {
        std::string filename_lower = job.filename;
        std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (filename_lower.find(query_lower) != std::string::npos) {
            result.push_back(job);
        }
    }

    return result;
}

std::vector<PrintHistoryJob>
HistoryListPanel::apply_status_filter(const std::vector<PrintHistoryJob>& source) {
    if (status_filter_ == HistoryStatusFilter::ALL) {
        return source;
    }

    std::vector<PrintHistoryJob> result;
    result.reserve(source.size());

    for (const auto& job : source) {
        bool include = false;

        switch (status_filter_) {
        case HistoryStatusFilter::COMPLETED:
            include = (job.status == PrintJobStatus::COMPLETED);
            break;
        case HistoryStatusFilter::FAILED:
            include = (job.status == PrintJobStatus::ERROR);
            break;
        case HistoryStatusFilter::CANCELLED:
            include = (job.status == PrintJobStatus::CANCELLED);
            break;
        default:
            include = true;
            break;
        }

        if (include) {
            result.push_back(job);
        }
    }

    return result;
}

void HistoryListPanel::apply_sort(std::vector<PrintHistoryJob>& jobs) {
    auto sort_col = sort_column_;
    auto sort_dir = sort_direction_;

    std::sort(jobs.begin(), jobs.end(),
              [sort_col, sort_dir](const PrintHistoryJob& a, const PrintHistoryJob& b) {
                  bool result = false;

                  switch (sort_col) {
                  case HistorySortColumn::DATE:
                      result = a.start_time < b.start_time;
                      break;
                  case HistorySortColumn::DURATION:
                      result = a.total_duration < b.total_duration;
                      break;
                  case HistorySortColumn::FILENAME:
                      result = a.filename < b.filename;
                      break;
                  }

                  // For DESC, invert the result
                  if (sort_dir == HistorySortDirection::DESC) {
                      result = !result;
                  }

                  return result;
              });
}

// ============================================================================
// Filter/Sort Event Handlers
// ============================================================================

void HistoryListPanel::on_search_changed() {
    // Cancel existing timer if any
    helix::ui::safe_delete_timer(search_timer_);

    // Create debounce timer (300ms)
    search_timer_ = lv_timer_create(on_search_timer_static, 300, this);
    lv_timer_set_repeat_count(search_timer_, 1); // Fire once
}

void HistoryListPanel::on_search_clear() {
    // Text is already cleared by text_input's internal clear button handler.
    // We just need to update the search state and apply immediately.
    search_query_.clear();
    helix::ui::safe_delete_timer(search_timer_);
    apply_filters_and_sort();
}

void HistoryListPanel::on_search_timer_static(lv_timer_t* timer) {
    auto* panel = static_cast<HistoryListPanel*>(lv_timer_get_user_data(timer));
    if (panel) {
        panel->do_debounced_search();
    }
}

void HistoryListPanel::do_debounced_search() {
    search_timer_ = nullptr; // Timer is auto-deleted after single fire

    if (!search_box_) {
        return;
    }

    const char* text = lv_textarea_get_text(search_box_);
    search_query_ = text ? text : "";

    spdlog::debug("[{}] Search query changed: '{}'", get_name(), search_query_);
    apply_filters_and_sort();
}

void HistoryListPanel::on_status_filter_changed(int index) {
    status_filter_ = static_cast<HistoryStatusFilter>(index);
    spdlog::debug("[{}] Status filter changed to: {}", get_name(), index);
    apply_filters_and_sort();
}

void HistoryListPanel::on_sort_changed(int index) {
    // Map dropdown indices to sort settings:
    // 0: Date (newest) -> DATE, DESC
    // 1: Date (oldest) -> DATE, ASC
    // 2: Duration      -> DURATION, DESC
    // 3: Filename      -> FILENAME, ASC

    switch (index) {
    case 0: // Date (newest)
        sort_column_ = HistorySortColumn::DATE;
        sort_direction_ = HistorySortDirection::DESC;
        break;
    case 1: // Date (oldest)
        sort_column_ = HistorySortColumn::DATE;
        sort_direction_ = HistorySortDirection::ASC;
        break;
    case 2: // Duration
        sort_column_ = HistorySortColumn::DURATION;
        sort_direction_ = HistorySortDirection::DESC;
        break;
    case 3: // Filename
        sort_column_ = HistorySortColumn::FILENAME;
        sort_direction_ = HistorySortDirection::ASC;
        break;
    default:
        spdlog::warn("[{}] Unknown sort index: {}", get_name(), index);
        return;
    }

    spdlog::debug("[{}] Sort changed to: column={}, dir={}", get_name(),
                  static_cast<int>(sort_column_),
                  sort_direction_ == HistorySortDirection::DESC ? "DESC" : "ASC");
    apply_filters_and_sort();
}

void HistoryListPanel::toggle_filters() {
    int expanded = lv_subject_get_int(&subject_filters_expanded_) ? 0 : 1;
    lv_subject_set_int(&subject_filters_expanded_, expanded);
    spdlog::debug("[{}] Filter dropdowns {}", get_name(), expanded ? "expanded" : "collapsed");
}

void HistoryListPanel::refresh_filter_indicator() {
    // Active when either dropdown is off its default (status != All, or sort != the
    // default Date-newest) or the search box is non-empty. Publish it as a subject;
    // the XML bind_style rules recolour the funnel reactively (appearance stays in XML).
    bool sort_default =
        sort_column_ == HistorySortColumn::DATE && sort_direction_ == HistorySortDirection::DESC;
    bool active =
        !search_query_.empty() || status_filter_ != HistoryStatusFilter::ALL || !sort_default;
    lv_subject_set_int(&subject_filter_active_, active ? 1 : 0);
}

// ============================================================================
// Detail Overlay Implementation
// ============================================================================

void HistoryListPanel::init_detail_subjects() {
    // Initialize all string subjects with buffers using managed macros
    UI_MANAGED_SUBJECT_STRING(detail_filename_, detail_filename_buf_, "", "history_detail_filename",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_status_, detail_status_buf_, "", "history_detail_status",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_status_icon_, detail_status_icon_buf_, "help_circle",
                              "history_detail_status_icon", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_status_variant_, detail_status_variant_buf_, "secondary",
                              "history_detail_status_variant", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_start_time_, detail_start_time_buf_, "",
                              "history_detail_start_time", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_end_time_, detail_end_time_buf_, "", "history_detail_end_time",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_duration_, detail_duration_buf_, "", "history_detail_duration",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_layers_, detail_layers_buf_, "", "history_detail_layers",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_layer_height_, detail_layer_height_buf_, "",
                              "history_detail_layer_height", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_nozzle_temp_, detail_nozzle_temp_buf_, "",
                              "history_detail_nozzle_temp", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_bed_temp_, detail_bed_temp_buf_, "", "history_detail_bed_temp",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_filament_, detail_filament_buf_, "", "history_detail_filament",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_filament_type_, detail_filament_type_buf_, "",
                              "history_detail_filament_type", subjects_);

    // Initialize int subjects
    UI_MANAGED_SUBJECT_INT(detail_can_reprint_, 1, "history_detail_can_reprint", subjects_);
    UI_MANAGED_SUBJECT_INT(detail_status_code_, 0, "history_detail_status_code", subjects_);
    UI_MANAGED_SUBJECT_INT(detail_has_timelapse_, 0, "history_detail_has_timelapse", subjects_);

    spdlog::debug("[{}] Detail overlay subjects initialized", get_name());
}

void HistoryListPanel::show_detail_overlay(const PrintHistoryJob& job) {
    // Track that detail overlay is open (for smart refresh skip on return)
    detail_overlay_open_ = true;
    history_changed_while_detail_open_ = false;

    // Update subjects with job data first
    update_detail_subjects(job);

    // Create overlay if not exists (lazy init)
    if (!detail_overlay_) {
        detail_overlay_ = static_cast<lv_obj_t*>(
            lv_xml_create(parent_screen_, "history_detail_overlay", nullptr));

        if (detail_overlay_) {
            spdlog::debug("[{}] Detail overlay created", get_name());
        } else {
            spdlog::error("[{}] Failed to create detail overlay", get_name());
            return;
        }
    }

    // Update thumbnail display
    lv_obj_t* thumbnail_image = lv_obj_find_by_name(detail_overlay_, "thumbnail_image");
    lv_obj_t* thumbnail_fallback = lv_obj_find_by_name(detail_overlay_, "thumbnail_fallback");

    // One staleness context per overlay open. Creating it bumps
    // detail_overlay_generation_, exactly as the bare `++` did, so a thumbnail
    // still in flight from a previously-opened job now reports stale. It also
    // carries the panel's lifetime token, which the raw generation compare
    // never had — the queued apply below used to dereference a possibly-freed
    // panel just to read the counter.
    ThumbnailLoadContext ctx = ThumbnailLoadContext::create(lifetime_, &detail_overlay_generation_);

    if (thumbnail_image && thumbnail_fallback) {
        if (!job.thumbnail_path.empty()) {
            // Show fallback initially while loading
            lv_obj_add_flag(thumbnail_image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(thumbnail_fallback, LV_OBJ_FLAG_HIDDEN);

            IMoonrakerAPI* api = get_moonraker_api();

            // The detail overlay has always rendered the full-resolution PNG,
            // so it asks for FullPng and req.target goes unused. The cache key
            // is the job's Moonraker relative path, unchanged.
            ThumbnailRequest req;
            req.key = job.thumbnail_path;
            req.api = api;
            req.format = ThumbnailRequest::ThumbnailFormat::FullPng;

            auto* self = this;
            get_thumbnail_cache().fetch(
                req, ctx,
                // Success callback - may be called from background thread.
                // Capture the load context, NOT widget pointers (avoids use-after-free)
                [self, ctx](const std::string& lvgl_path, bool /*degraded*/) {
                    // Dispatch UI update to main thread
                    struct ThumbUpdate {
                        HistoryListPanel* panel;
                        ThumbnailLoadContext ctx;
                        std::string path;
                    };
                    helix::ui::queue_update<ThumbUpdate>(
                        std::make_unique<ThumbUpdate>(ThumbUpdate{self, ctx, lvgl_path}),
                        [](ThumbUpdate* t) {
                            // Panel alive AND generation still current? The
                            // overlay may have been closed, reopened for a
                            // different job, or torn down entirely between the
                            // fetch completing and this queued apply running.
                            if (!t->ctx.is_valid()) {
                                spdlog::debug("[HistoryListPanel] Thumbnail callback stale, "
                                              "ignoring");
                                return;
                            }
                            if (!t->panel->detail_overlay_) {
                                return;
                            }

                            // Look up widgets by name (safe - fresh lookup each time)
                            lv_obj_t* image =
                                lv_obj_find_by_name(t->panel->detail_overlay_, "thumbnail_image");
                            lv_obj_t* fallback = lv_obj_find_by_name(t->panel->detail_overlay_,
                                                                     "thumbnail_fallback");

                            if (image && fallback) {
                                lv_image_set_src(image, t->path.c_str());
                                lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
                                lv_obj_add_flag(fallback, LV_OBJ_FLAG_HIDDEN);
                                spdlog::debug("[HistoryListPanel] Thumbnail loaded: {}", t->path);
                            }
                        });
                },
                // Error callback
                [](const std::string& error) {
                    spdlog::warn("[HistoryListPanel] Failed to load thumbnail: {}", error);
                    // Fallback is already showing, nothing to do
                });
        } else {
            // No thumbnail path - show fallback
            lv_obj_add_flag(thumbnail_image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(thumbnail_fallback, LV_OBJ_FLAG_HIDDEN);
            spdlog::debug("[{}] No thumbnail path, showing fallback", get_name());
        }
    }

    // Register detail sub-overlay with nullptr lifecycle — managed by HistoryListPanel
    NavigationManager::instance().register_overlay_instance(detail_overlay_, nullptr);

    // Push the overlay
    NavigationManager::instance().push_overlay(detail_overlay_);
    spdlog::info("[{}] Showing detail overlay for: {}", get_name(), job.filename);
}

void HistoryListPanel::update_detail_subjects(const PrintHistoryJob& job) {
    // Update string subjects using lv_subject_copy_string (LVGL 9.4 API)
    lv_subject_copy_string(&detail_filename_, job.filename.c_str());
    lv_subject_copy_string(&detail_status_, get_status_text(job.status));
    lv_subject_copy_string(&detail_status_icon_, status_to_icon(job.status));
    lv_subject_copy_string(&detail_status_variant_, status_to_variant(job.status));

    // Format timestamps
    lv_subject_copy_string(&detail_start_time_, job.date_str.c_str());

    // Format end time from end_time timestamp
    if (job.end_time > 0) {
        time_t end_ts = static_cast<time_t>(job.end_time);
        std::string end_str = helix::ui::format_modified_date(end_ts);
        lv_subject_copy_string(&detail_end_time_, end_str.c_str());
    } else {
        lv_subject_copy_string(&detail_end_time_, "-");
    }

    lv_subject_copy_string(&detail_duration_, job.duration_str.c_str());

    // Format layers
    char layers_buf[32];
    if (job.layer_count > 0) {
        snprintf(layers_buf, sizeof(layers_buf), "%u", job.layer_count);
    } else {
        snprintf(layers_buf, sizeof(layers_buf), "-");
    }
    lv_subject_copy_string(&detail_layers_, layers_buf);

    // Format layer height
    char layer_height_buf[32];
    if (job.layer_height > 0) {
        helix::format::format_distance_mm(job.layer_height, 2, layer_height_buf,
                                          sizeof(layer_height_buf));
    } else {
        snprintf(layer_height_buf, sizeof(layer_height_buf), "-");
    }
    lv_subject_copy_string(&detail_layer_height_, layer_height_buf);

    // Format temperatures
    char temp_buf[32];
    if (job.nozzle_temp > 0) {
        snprintf(temp_buf, sizeof(temp_buf), "%.0f°C", job.nozzle_temp);
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "-");
    }
    lv_subject_copy_string(&detail_nozzle_temp_, temp_buf);

    if (job.bed_temp > 0) {
        snprintf(temp_buf, sizeof(temp_buf), "%.0f°C", job.bed_temp);
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "-");
    }
    lv_subject_copy_string(&detail_bed_temp_, temp_buf);

    lv_subject_copy_string(&detail_filament_, job.filament_str.c_str());
    lv_subject_copy_string(&detail_filament_type_,
                           job.filament_type.empty() ? "Unknown" : job.filament_type.c_str());

    // Set reprint availability based on file existence
    lv_subject_set_int(&detail_can_reprint_, job.exists ? 1 : 0);

    // Set timelapse availability
    lv_subject_set_int(&detail_has_timelapse_, job.has_timelapse ? 1 : 0);

    // Set status code for icon visibility binding: 0=completed, 1=cancelled, 2=error, 3=in_progress
    int status_code = 0; // Default to completed
    switch (job.status) {
    case PrintJobStatus::COMPLETED:
        status_code = 0;
        break;
    case PrintJobStatus::CANCELLED:
        status_code = 1;
        break;
    case PrintJobStatus::ERROR:
        status_code = 2;
        break;
    case PrintJobStatus::IN_PROGRESS:
        status_code = 3;
        break;
    default:
        status_code = 0;
        break;
    }
    lv_subject_set_int(&detail_status_code_, status_code);

    spdlog::debug("[{}] Detail subjects updated for: {} (status_code={})", get_name(), job.filename,
                  status_code);
}

void HistoryListPanel::handle_reprint() {
    if (selected_job_index_ >= filtered_jobs_.size()) {
        spdlog::warn("[{}] Invalid selected job index for reprint", get_name());
        return;
    }

    const auto& job = filtered_jobs_[selected_job_index_];

    if (!job.exists) {
        spdlog::warn("[{}] Cannot reprint - file no longer exists: {}", get_name(), job.filename);
        ui_notification_warning("File no longer exists on printer");
        return;
    }

    spdlog::info("[{}] Reprint requested for: {}", get_name(), job.filename);

    // Navigate to the Print Select file detail view (DRY - reuse existing UI)
    // Step 1: Close all history overlays (detail → list → dashboard)
    NavigationManager::instance().go_back(); // Close history detail overlay
    NavigationManager::instance().go_back(); // Close history list panel
    NavigationManager::instance().go_back(); // Close history dashboard

    // Step 2: Switch to Print Select panel
    NavigationManager::instance().set_active(PanelId::PrintSelect);

    // Step 3: Get PrintSelectPanel and navigate to file details
    PrintSelectPanel* print_panel =
        get_print_select_panel(get_printer_state(), get_moonraker_api());
    if (print_panel) {
        // select_file_by_name searches the file list and shows detail view if found
        if (print_panel->select_file_by_name(job.filename)) {
            spdlog::info("[{}] Navigated to file details for: {}", get_name(), job.filename);
        } else {
            spdlog::warn("[{}] File not found in print panel: {}", get_name(), job.filename);
            ui_notification_warning("File not found in print list");
        }
    } else {
        spdlog::error("[{}] Could not get PrintSelectPanel", get_name());
        ui_notification_error("Error", "Could not open print panel", false);
    }
}

void HistoryListPanel::handle_delete() {
    if (selected_job_index_ >= filtered_jobs_.size()) {
        spdlog::warn("[{}] Invalid selected job index for delete", get_name());
        return;
    }

    const auto& job = filtered_jobs_[selected_job_index_];
    spdlog::info("[{}] Delete requested for: {} (job_id: {})", get_name(), job.filename,
                 job.job_id);

    // For now, directly delete without confirmation dialog
    // TODO: Add confirmation dialog
    confirm_delete();
}

void HistoryListPanel::confirm_delete() {
    if (selected_job_index_ >= filtered_jobs_.size()) {
        spdlog::warn("[{}] Invalid selected job index for confirm delete", get_name());
        return;
    }

    const auto& job = filtered_jobs_[selected_job_index_];
    std::string job_id = job.job_id;
    std::string filename = job.filename;

    spdlog::info("[{}] Confirming delete for job_id: {}", get_name(), job_id);

    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        api->history().delete_history_job(
            job_id,
            [this, job_id, filename]() {
                spdlog::info("[{}] Job deleted: {} ({})", get_name(), filename, job_id);

                // Remove from jobs_ and filtered_jobs_
                jobs_.erase(std::remove_if(
                                jobs_.begin(), jobs_.end(),
                                [&job_id](const PrintHistoryJob& j) { return j.job_id == job_id; }),
                            jobs_.end());

                // Close detail overlay and refresh list
                NavigationManager::instance().go_back();
                apply_filters_and_sort();

                ui_notification_success("Print job deleted");
            },
            [this, filename](const MoonrakerError& error) {
                spdlog::error("[{}] Failed to delete job {}: {}", get_name(), filename,
                              error.message);
                ui_notification_error("Delete Failed", error.message.c_str(), false);
            });
    }
}

void HistoryListPanel::handle_view_timelapse() {
    if (selected_job_index_ >= filtered_jobs_.size()) {
        spdlog::warn("[{}] Invalid selected job index for view timelapse", get_name());
        return;
    }

    const auto& job = filtered_jobs_[selected_job_index_];

    if (!job.has_timelapse || job.timelapse_filename.empty()) {
        spdlog::warn("[{}] No timelapse available for: {}", get_name(), job.filename);
        ui_notification_warning("No timelapse available");
        return;
    }

    spdlog::info("[{}] View timelapse requested for: {} (file: {})", get_name(), job.filename,
                 job.timelapse_filename);

    // TODO: Phase 6 - Open timelapse viewer/player
    // For now, show a toast with the filename
    std::string message = "Timelapse: " + job.timelapse_filename;
    ui_notification_info(message.c_str());
}

// ============================================================================
// Infinite Scroll Implementation
// ============================================================================

void HistoryListPanel::on_scroll_static(lv_event_t* e) {
    auto* panel = static_cast<HistoryListPanel*>(lv_event_get_user_data(e));
    if (panel) {
        panel->check_scroll_position();
    }
}

void HistoryListPanel::on_scroll_update_visible(lv_event_t* e) {
    auto* panel = static_cast<HistoryListPanel*>(lv_event_get_user_data(e));
    if (panel && panel->list_view_ && !panel->filtered_jobs_.empty()) {
        panel->list_view_->update_visible(panel->filtered_jobs_);
    }
}

void HistoryListPanel::check_scroll_position() {
    if (!list_content_ || !has_more_data_ || load_more_guard_.active()) {
        return;
    }

    // Get scroll position and content height
    int32_t scroll_y = lv_obj_get_scroll_y(list_content_);
    int32_t content_height = lv_obj_get_scroll_bottom(list_content_);

    // Load more when within 100px of the bottom
    constexpr int32_t LOAD_MORE_THRESHOLD = 100;

    if (content_height <= LOAD_MORE_THRESHOLD) {
        spdlog::debug("[{}] Near bottom (scroll_y={}, remaining={}), loading more...", get_name(),
                      scroll_y, content_height);
        load_more();
    }
}

void HistoryListPanel::append_rows(size_t /*start_index*/) {
    // Virtual scroll handles display - just trigger a re-populate with new data
    if (list_view_) {
        list_view_->populate(filtered_jobs_, true); // preserve_scroll=true
    }
}
