// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "print_history_data.h"
#include "ui_coalesced_timer.h"

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

namespace helix {

/**
 * @brief How much of Moonraker's history a consumer needs cached
 *
 * The cache is populated at one of two fidelities, so "is it loaded" cannot be
 * answered without saying loaded for what: a RECENT load holds only the newest
 * jobs, and a consumer that aggregates across the whole history would read that
 * boot-time slice as the complete record and render truncated numbers.
 *
 * Every query and every load entry point takes one of these, with no default,
 * so a call site states its fidelity rather than inheriting one.
 */
enum class HistoryScope {
    /// The newest jobs only. Everything the home panel shows is satisfied by
    /// the slice pulled at startup.
    RECENT,
    /// Every job Moonraker will return. Required by anything aggregating over
    /// all of history - per-filename stats, the history panels' ALL_TIME view.
    COMPLETE,
};

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
 * // identical one. The scope says what this consumer needs; get_filename_stats()
 * // and any all-of-history aggregate need COMPLETE.
 * manager_->ensure_loaded(helix::HistoryScope::COMPLETE);
 * if (manager_->is_loaded(helix::HistoryScope::COMPLETE)) {
 *     update_from_history();
 * }
 *
 * // In on_history_changed
 * update_from_history();
 * ```
 *
 * ## Fidelity
 *
 * Startup pulls a RECENT slice, which is all the home panel reads, and the
 * whole list is pulled the first time a consumer that needs it asks. Because
 * the cache can therefore be populated at two fidelities, every query names
 * the scope it is asking about - see HistoryScope.
 *
 * ## Cache Invalidation
 *
 * The manager subscribes to Moonraker's `notify_history_changed` notification.
 * That notification carries the complete job record it is announcing, so the
 * cache is normally amended in place rather than re-pulled; a payload that
 * cannot be applied falls back to a debounced re-fetch at the loaded scope.
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
     * @brief Whether the cache holds enough history to answer @p scope
     *
     * RECENT is satisfied by any completed load. COMPLETE additionally requires
     * that the load asked for the whole list, or that a smaller request came
     * back short and therefore already holds every job the printer has.
     *
     * There is no defaulted overload on purpose: a consumer of
     * get_filename_stats() or the ALL_TIME view that asked the cheap question
     * would render a boot-time slice as the complete record.
     */
    [[nodiscard]] bool is_loaded(helix::HistoryScope scope) const;

    /**
     * @brief Whether every job started at or after @p since is cached
     *
     * A RECENT load is capped at kRecentJobLimit jobs, which on a busy printer
     * can stop short of a window a consumer needs. Cached jobs keep Moonraker's
     * newest-first order, so the oldest one bounds the covered window; a load
     * that came back short of its limit covers everything.
     *
     * @param since Unix timestamp of the oldest job the caller must see
     */
    [[nodiscard]] bool covers_since(double since) const;

    /**
     * @brief Whether any load has completed, at any fidelity
     *
     * The connection-staleness watcher's question: it decides whether there is
     * a latch worth clearing and renders nothing, so it needs no scope. Not a
     * substitute for is_loaded() - this cannot say whether the cache holds
     * enough to answer a particular consumer.
     */
    [[nodiscard]] bool has_cached_data() const {
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

    /// Jobs a RECENT load asks for. Matches Moonraker's own default page size,
    /// and covers a week of prints without escalating on any printer running
    /// fewer than seven jobs a day.
    static constexpr int kRecentJobLimit = 50;

    /// Jobs a COMPLETE load asks for.
    static constexpr int kCompleteJobLimit = 500;

    /// Quiet period that collapses a burst of invalidations into one request.
    /// A klippy restart fires the config-backup move_file and the restart's own
    /// history event together, and deleting several files walks the same path
    /// once per file.
    static constexpr uint32_t kInvalidationDebounceMs = 300;

    /**
     * @brief Fetch history from Moonraker asynchronously
     *
     * Calls `get_history_list()` and populates both `cached_jobs_` and
     * `filename_stats_`. Notifies all observers when complete.
     *
     * This is the INVALIDATION entry point: it means "whatever is cached is
     * wrong, go get it again". If a request is already in flight its response
     * predates the change that prompted this call, so one re-issue is queued to
     * run when that response lands, at the widest scope anyone asked for while
     * it was out. Callers that only want the cache populated must use
     * ensure_loaded() instead - queueing a re-issue for them fetches the same
     * list twice.
     *
     * @param scope How much history to pull
     */
    void fetch(helix::HistoryScope scope);

    /**
     * @brief Populate the cache to @p scope if it is not already there
     *
     * The LAZY-LOAD entry point, for a panel that needs history on activate and
     * does not care whether it or someone else triggered the request. Does
     * nothing when the cache already answers @p scope, or when a request that
     * will answer it is in flight or downloaded and waiting for the main
     * thread - in all those cases the pending response serves this caller too.
     *
     * A request narrower than @p scope does NOT serve it, so a COMPLETE caller
     * arriving behind an in-flight RECENT load still queues the wider one.
     *
     * @param scope How much history the caller needs
     */
    void ensure_loaded(helix::HistoryScope scope);

    /**
     * @brief Load whatever it takes to have every job started since @p since
     *
     * Populates the cache when it is cold and escalates to the whole list when
     * a RECENT slice stops short of @p since. Cheap on a printer whose recent
     * slice already reaches back that far, which is the common case.
     *
     * @param since Unix timestamp of the oldest job the caller must see
     */
    void ensure_covers_since(double since);

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
     *
     * @param jobs      Parsed jobs, newest first
     * @param scope     Scope the request was issued at
     * @param requested Job limit the request carried. A response shorter than
     *                  its limit is the whole history, whatever scope asked.
     */
    void on_history_fetched(std::vector<PrintHistoryJob>&& jobs, helix::HistoryScope scope, int requested);

    /**
     * @brief Fold a single job from a history notification into the cache
     *
     * Replaces the entry with the same job_id, or inserts by start_time when
     * the job is new. Fidelity is unchanged: one more job neither completes an
     * incomplete cache nor truncates a complete one.
     */
    void apply_job_update(PrintHistoryJob&& job);

    /**
     * @brief Queue one refetch behind an in-flight request, widening its scope
     *
     * Several changes landing during one request collapse into a single
     * re-issue, and that re-issue asks for the widest scope any of them needed.
     */
    void queue_refetch(helix::HistoryScope scope);

    /**
     * @brief Stale the cache now and coalesce the resulting request
     *
     * Invalidation is immediate so a consumer reading is_loaded() sees the
     * cache as stale from the moment the change is known; only the round-trip
     * waits out kInvalidationDebounceMs.
     */
    void invalidate_and_refetch();

    /**
     * @brief Whether a notify_history_changed action carries a usable job
     *
     * Moonraker attaches the complete job record, including the `exists` flag
     * it recomputes per request, to the actions it emits from add_job and
     * finish_job. Anything else has to be answered by refetching.
     */
    [[nodiscard]] static bool history_action_carries_job(const std::string& action);

    /// Job limit a request at @p scope carries.
    [[nodiscard]] static int limit_for(helix::HistoryScope scope);

    /// "No request" sentinel for the two scope slots below. HistoryScope's
    /// enumerators are ordered narrowest-first, so a plain integer comparison
    /// answers "is that scope wide enough" and this sits below all of them.
    static constexpr int kNoFetch = -1;

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
     * - `notify_history_changed` - a job was added or finished. The payload
     *   carries that job, so this one normally patches the cache instead of
     *   re-fetching (see history_action_carries_job).
     * - `notify_filelist_changed` - filtered to the actions that can orphan a
     *   job (see filelist_action_affects_history); a delete or move flips a
     *   cached job's `exists` flag and Moonraker never reports that through
     *   the history notification.
     *
     * Both apply on the main thread; the parse runs on the WebSocket thread.
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
    // Fidelity of the cached list: the scope the load that filled it asked for.
    // An invalidation refetches at this scope, so a consumer that escalated to
    // the whole list keeps it across a history event instead of silently
    // dropping back to the startup slice.
    helix::HistoryScope loaded_scope_ = helix::HistoryScope::RECENT;
    // The load came back shorter than the limit it asked for, so the cache
    // holds every job the printer has whatever scope requested it.
    bool holds_every_job_ = false;
    // Atomic because we clear it on the WebSocket BG thread (before posting the
    // main-thread defer) to survive UpdateQueue freeze-drops — otherwise a dropped
    // fetch_success strands the guard and blocks every subsequent fetch.
    std::atomic<bool> is_fetching_{false};
    // Scope of the request currently out, or kNoFetch. ensure_loaded() joins an
    // in-flight load only when it is at least as wide as what the caller needs.
    std::atomic<int> in_flight_scope_{kNoFetch};
    // Scope of the fetch queued behind an in-flight one, or kNoFetch. One slot
    // carrying the widest scope requested, rather than a bare "something is
    // queued" bit: a re-issue that dropped back to the startup slice would
    // truncate a cache a panel had escalated. Atomic for the same reason as
    // is_fetching_: written from the WebSocket thread's parse side.
    std::atomic<int> pending_scope_{kNoFetch};
    // A response is downloaded and its handler is queued for the main thread.
    // Set on the WebSocket BG thread before that handler is posted, cleared
    // when it runs. is_fetching_ is already false across this gap - it is
    // released BG-side on purpose - and the gap is as wide as the main thread
    // is busy, so this is the flag that lets ensure_loaded() join a load whose
    // bytes are already in hand. fetch() is deliberately NOT gated on it: an
    // invalidation must still force a real re-fetch.
    std::atomic<bool> delivery_pending_{false};

    /// Collapses a burst of invalidations into one request. Leading-edge, so
    /// the refetch fires at a bounded rate however fast the notifications
    /// arrive. Cancelled by its own destructor, which is what keeps a pending
    /// callback from outliving this manager.
    helix::ui::CoalescedTimer refetch_debounce_{kInvalidationDebounceMs};

    /// Guard for async callback safety
    /// Prevents use-after-free when callbacks fire after destruction
    helix::AsyncLifetimeGuard lifetime_;

    /// Watches printer connection state so a dropped socket stales the cache.
    ObserverGuard connection_observer_;

    friend class helix::PrintHistoryManagerTestAccess;
};
