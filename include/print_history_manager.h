// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "print_history_data.h"

#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class IMoonrakerAPI;
namespace helix {
class IMoonrakerClient;
class PrintHistoryManagerTestAccess;
} // namespace helix

/**
 * @brief Per-filename aggregated print history stats
 *
 * Used by PrintSelectPanel to show status indicators:
 * - success_count: Number of completed prints (shows as "N ✓")
 * - failure_count: Number of failed/cancelled prints
 * - last_status: Status of most recent print (determines icon)
 */
struct PrintHistoryStats {
    int success_count = 0; ///< Count of COMPLETED jobs for this filename
    int failure_count = 0; ///< Count of CANCELLED + ERROR jobs
    PrintJobStatus last_status = PrintJobStatus::UNKNOWN; ///< Status of most recent job
    double last_print_time = 0.0;                         ///< Unix timestamp of most recent job
    std::string uuid;      ///< UUID from most recent job for this filename
    size_t size_bytes = 0; ///< Size from most recent job for this filename
};

namespace helix {
/// Observer callback when history data changes
using HistoryChangedCallback = std::function<void()>;
} // namespace helix

/**
 * @brief Centralized print history cache with observer notification
 *
 * PrintHistoryManager provides a single source of truth for print history,
 * serving both the History panels and PrintSelectPanel status indicators.
 *
 * ## Data Views
 *
 * Two views of the same cached data:
 * 1. **Raw jobs list** (`get_jobs()`) - For HistoryDashboardPanel, HistoryListPanel
 * 2. **Filename stats map** (`get_filename_stats()`) - For PrintSelectPanel status indicators
 *
 * ## Usage Example
 *
 * ```cpp
 * // In panel constructor
 * manager_->add_observer([this]() { on_history_changed(); });
 *
 * // In on_activate - ensure_loaded(), never fetch(). fetch() means "the cache
 * // is wrong", and asking for it while a request is in flight queues a second
 * // identical one.
 * manager_->ensure_loaded();
 * if (manager_->is_loaded()) {
 *     update_from_history();
 * }
 *
 * // In on_history_changed
 * update_from_history();
 * ```
 *
 * ## Cache Invalidation
 *
 * The manager subscribes to Moonraker's `notify_history_changed` notification
 * and automatically invalidates + re-fetches when a print completes.
 *
 * @see PrintHistoryStats for per-file aggregation structure
 * @see PrintHistoryJob for raw job data structure
 */
class PrintHistoryManager {
  public:
    /**
     * @brief Construct PrintHistoryManager with API and client references
     *
     * @param api IMoonrakerAPI for fetching history
     * @param client helix::IMoonrakerClient for notification subscription
     */
    PrintHistoryManager(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    ~PrintHistoryManager();

    // Non-copyable
    PrintHistoryManager(const PrintHistoryManager&) = delete;
    PrintHistoryManager& operator=(const PrintHistoryManager&) = delete;

    // ========================================================================
    // Data Access
    // ========================================================================

    /**
     * @brief Get raw jobs list (for History panels)
     * @return Reference to cached jobs vector
     */
    [[nodiscard]] const std::vector<PrintHistoryJob>& get_jobs() const {
        return cached_jobs_;
    }

    /**
     * @brief Newest cached job whose gcode file is still on the printer
     *
     * Moonraker recomputes each job's `exists` flag per history request
     * (`history.py` `_prep_requested_job` -> `file_manager.check_file_exists`),
     * so the flag is accurate as of the last completed fetch. Every consumer
     * that means "the last print the user can still act on" - reprint
     * availability, the idle tile's filename/when/meta, the idle thumbnail key
     * and its freshness stamp - must select through this rather than taking
     * `get_jobs().front()`, which may name a file that was deleted.
     *
     * Cached jobs keep Moonraker's newest-first order, so the first match is
     * the newest one.
     *
     * @return Pointer into the cached jobs vector, or nullptr when history is
     *         not loaded or every job's file is gone. Invalidated by the next
     *         fetch completion.
     */
    [[nodiscard]] const PrintHistoryJob* get_newest_existing_job() const;

    /**
     * @brief Get per-filename stats map (for PrintSelectPanel)
     * @return Reference to aggregated stats map (key = basename, no path)
     */
    [[nodiscard]] const std::unordered_map<std::string, PrintHistoryStats>&
    get_filename_stats() const {
        return filename_stats_;
    }

    /**
     * @brief Check if history data has been loaded
     * @return true if fetch has completed at least once
     */
    [[nodiscard]] bool is_loaded() const {
        return is_loaded_;
    }

    /**
     * @brief Get jobs filtered by start time
     *
     * Returns jobs where `start_time >= since`. Used by HistoryDashboardPanel
     * for time-based filtering (TODAY, WEEK, MONTH, etc.).
     *
     * @param since Unix timestamp threshold (jobs before this are excluded)
     * @return Vector of matching jobs (by value, allows filtering)
     */
    [[nodiscard]] std::vector<PrintHistoryJob> get_jobs_since(double since) const;

    // ========================================================================
    // Fetch / Refresh
    // ========================================================================

    /**
     * @brief Fetch history from Moonraker asynchronously
     *
     * Calls `get_history_list()` and populates both `cached_jobs_` and
     * `filename_stats_`. Notifies all observers when complete.
     *
     * This is the INVALIDATION entry point: it means "whatever is cached is
     * wrong, go get it again". If a request is already in flight its response
     * predates the change that prompted this call, so one re-issue is queued to
     * run when that response lands. Callers that only want the cache populated
     * must use ensure_loaded() instead - queueing a re-issue for them fetches
     * the same list twice.
     *
     * @param limit Maximum number of jobs to fetch (default 500)
     */
    void fetch(int limit = 500);

    /**
     * @brief Populate the cache if it is not already loaded or loading
     *
     * The LAZY-LOAD entry point, for a panel that needs history on activate and
     * does not care whether it or someone else triggered the request. Does
     * nothing when the cache is loaded or a fetch is already in flight, because
     * that fetch's response serves this caller too.
     *
     * Splitting this out of fetch() is the fix for a real double-fetch: the
     * panels that want history call this on activate, four such calls landed
     * while the first request was in flight, and fetch() read every one of them
     * as an invalidation and queued a re-issue. On an
     * AD5X the 500-job list took 10.4s and ~800 KB, and every byte of the second
     * one was redundant (bundles MG34LYR4 / VXYB9JPQ).
     *
     * @param limit Maximum number of jobs to fetch (default 500)
     */
    void ensure_loaded(int limit = 500);

    /**
     * @brief Mark cache as stale
     *
     * Clears `is_loaded_` flag. Does NOT clear cached data (allows
     * stale-while-revalidate pattern).
     */
    void invalidate();

    // ========================================================================
    // Observer Pattern
    // ========================================================================

    /**
     * @brief Register observer callback by pointer
     *
     * Callback is invoked (on main thread) when:
     * - fetch() completes successfully
     * - Cache is invalidated and re-fetched (via notify_history_changed)
     *
     * IMPORTANT: Pass the address of a member variable, not a temporary.
     * The pointer must remain valid until remove_observer() is called.
     *
     * @param cb Pointer to callback function (stored, not copied)
     */
    void add_observer(helix::HistoryChangedCallback* cb);

    /**
     * @brief Remove observer callback by pointer
     *
     * Removes the callback registered with add_observer(). Uses pointer
     * comparison, so this actually works (unlike std::function comparison).
     *
     * @param cb Pointer to callback to remove
     */
    void remove_observer(helix::HistoryChangedCallback* cb);

  private:
    /**
     * @brief Handle completed fetch (runs on main thread)
     */
    void on_history_fetched(std::vector<PrintHistoryJob>&& jobs);

    /**
     * @brief Build filename_stats_ from cached_jobs_
     *
     * Aggregates jobs by basename (strips path), counting successes/failures
     * and tracking the most recent job's status.
     */
    void build_filename_stats();

    /**
     * @brief Call all registered observers
     */
    void notify_observers();

    /**
     * @brief Subscribe to the Moonraker notifications that stale the cache
     *
     * Called in constructor. Two of them:
     * - `notify_history_changed` - a job was added or history was cleared.
     * - `notify_filelist_changed` - filtered to the actions that can orphan a
     *   job (see filelist_action_affects_history); a delete or move flips a
     *   cached job's `exists` flag and Moonraker never reports that through
     *   the history notification.
     *
     * Both invalidate the cache and re-fetch, on the main thread.
     */
    void subscribe_to_notifications();

    /**
     * @brief Whether a notify_filelist_changed action can orphan a history job
     *
     * Uploads, metadata scans and directory listings fire the same
     * notification and cannot change any job's `exists` flag, so they must not
     * trigger a history round-trip.
     */
    [[nodiscard]] static bool filelist_action_affects_history(const std::string& action);

    /**
     * @brief Mark the cache stale whenever the Moonraker socket is not up
     *
     * Called in constructor. The two notifications above only reach a live
     * socket, so a drop is a window in which jobs can be added or deleted with
     * nothing left to announce it. See observe_connection_staleness() in
     * connection_staleness.h for the rule and why it is not hung off the
     * client's connected fan-out.
     */
    void watch_connection_state();

    // Dependencies
    IMoonrakerAPI* api_;
    helix::IMoonrakerClient* client_;

    // Cached data
    std::vector<PrintHistoryJob> cached_jobs_;
    std::unordered_map<std::string, PrintHistoryStats> filename_stats_;

    // Observers (stored as pointers for reliable removal)
    std::vector<helix::HistoryChangedCallback*> observers_;

    // State
    bool is_loaded_ = false;
    // Atomic because we clear it on the WebSocket BG thread (before posting the
    // main-thread defer) to survive UpdateQueue freeze-drops — otherwise a dropped
    // fetch_success strands the guard and blocks every subsequent fetch.
    std::atomic<bool> is_fetching_{false};
    // Set when fetch() is dropped because one is already in flight. The
    // in-flight response predates whatever prompted the dropped call, so the
    // completion handler re-issues exactly one more fetch. Atomic for the same
    // reason as is_fetching_: written from the WebSocket thread's parse side.
    std::atomic<bool> refetch_pending_{false};

    /// Guard for async callback safety
    /// Prevents use-after-free when callbacks fire after destruction
    helix::AsyncLifetimeGuard lifetime_;

    /// Watches printer connection state so a dropped socket stales the cache.
    ObserverGuard connection_observer_;

    friend class helix::PrintHistoryManagerTestAccess;
};
