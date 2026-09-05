// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_history_manager.h"

#include "ui_update_queue.h"

#include "connection_staleness.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "print_history_parse.h"

#include <spdlog/spdlog.h>

#include <algorithm>

using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

PrintHistoryManager::PrintHistoryManager(IMoonrakerAPI* api, IMoonrakerClient* client)
    : api_(api), client_(client) {
    spdlog::debug("[HistoryManager] Created");
    subscribe_to_notifications();
    watch_connection_state();
}

PrintHistoryManager::~PrintHistoryManager() {
    connection_observer_.reset();

    // Unregister notification callbacks
    if (client_) {
        client_->unregister_method_callback("notify_history_changed", "PrintHistoryManager");
        client_->unregister_method_callback("notify_filelist_changed", "PrintHistoryManager");
    }
}

// ============================================================================
// Fetch / Refresh
// ============================================================================

int PrintHistoryManager::limit_for(HistoryScope scope) {
    return scope == HistoryScope::COMPLETE ? kCompleteJobLimit : kRecentJobLimit;
}

void PrintHistoryManager::queue_refetch(HistoryScope scope) {
    // Widen rather than overwrite: a RECENT invalidation arriving behind a
    // COMPLETE one must not be what the single re-issue ends up asking for.
    int wanted = static_cast<int>(scope);
    int current = pending_scope_.load();
    while (current < wanted && !pending_scope_.compare_exchange_weak(current, wanted)) {
    }
}

void PrintHistoryManager::fetch(HistoryScope scope) {
    if (!api_) {
        spdlog::warn("[HistoryManager] No API available, cannot fetch");
        return;
    }

    if (!api_->is_connected()) {
        spdlog::debug("[HistoryManager] Not connected yet, deferring history fetch");
        return;
    }

    // Atomic check-and-set prevents concurrent fetches. Pairs with the reset
    // in the BG-thread callbacks below.
    bool expected = false;
    if (!is_fetching_.compare_exchange_strong(expected, true)) {
        // The in-flight request was issued before this one, so its response
        // cannot describe whatever just changed. Queue a single re-issue rather
        // than dropping the request outright: deleting several files in a row
        // otherwise leaves the cache one delete behind until the next
        // notification happens to arrive.
        //
        // Only invalidations and scope escalations reach here. A caller that
        // merely wants the cache populated at a scope already in flight goes
        // through ensure_loaded(), which returns without arming this.
        queue_refetch(scope);
        spdlog::debug("[HistoryManager] Fetch already in progress, queuing one refetch");
        return;
    }

    const int limit = limit_for(scope);
    in_flight_scope_.store(static_cast<int>(scope));

    // A request going out supersedes any response still waiting for the main
    // thread, and releases the flag if that response is never applied at all.
    // is_fetching_ is already true here, so ensure_loaded() joins on that
    // instead and clearing this opens no window.
    delivery_pending_.store(false);

    spdlog::debug("[HistoryManager] Fetching history (limit={})", limit);

    auto token = lifetime_.token();

    api_->history().get_history_list(
        limit, 0, 0.0, 0.0, // limit, start, since, before
        [this, token, scope, limit](const std::vector<PrintHistoryJob>& jobs, uint64_t /*total*/) {
            // Hand the join over BEFORE releasing is_fetching_: across these two
            // stores ensure_loaded() must still see a load it can ride on, or
            // the whole list goes out a second time while this one sits in the
            // queue waiting for a busy main thread.
            delivery_pending_.store(true);
            // Clear guard BEFORE posting defer so a freeze-drop doesn't strand us.
            is_fetching_.store(false);
            // No bare expired() check — token.defer's own guard suffices, and
            // dropping the bare check silences the bg_tok_expired_check
            // detector for this site (3XNZQB2R audit). The std::vector copy
            // below is harmless if defer skips.
            std::vector<PrintHistoryJob> jobs_copy = jobs;
            token.defer("PrintHistoryManager::fetch_success",
                        [this, scope, limit, jobs = std::move(jobs_copy)]() mutable {
                            on_history_fetched(std::move(jobs), scope, limit);
                        });
        },
        [this, token](const MoonrakerError& error) {
            in_flight_scope_.store(kNoFetch);
            is_fetching_.store(false);
            // spdlog is thread-safe; logging the warn even on a destroyed
            // manager is harmless (informational). Dropping the bare
            // expired() check silences the detector here.
            (void)token;
            spdlog::warn("[HistoryManager] Failed to fetch history: {}", error.message);
        });
}

bool PrintHistoryManager::is_loaded(HistoryScope scope) const {
    if (!is_loaded_) {
        return false;
    }
    if (scope == HistoryScope::RECENT) {
        return true;
    }
    // A short response means the printer has no more jobs to give, so a RECENT
    // load on a printer with a short history answers COMPLETE too.
    return holds_every_job_ || loaded_scope_ == HistoryScope::COMPLETE;
}

bool PrintHistoryManager::covers_since(double since) const {
    if (!is_loaded_) {
        return false;
    }
    if (is_loaded(HistoryScope::COMPLETE)) {
        return true;
    }
    // Newest-first order, so the last entry is the oldest job cached and marks
    // how far back the slice reaches.
    return !cached_jobs_.empty() && cached_jobs_.back().start_time <= since;
}

void PrintHistoryManager::ensure_loaded(HistoryScope scope) {
    if (is_loaded(scope)) {
        return;
    }
    // A request is already out, or its response is downloaded and only waiting
    // for the main thread to apply it. When that request is at least as wide as
    // what this caller needs, its response populates the cache and notifies
    // every observer, which is all the caller wanted, so routing through
    // fetch() here would only arm a redundant re-issue of the same list. A
    // narrower request does not serve the caller, and falls through to widen.
    //
    // Both flags are load-bearing. is_fetching_ is released on the WebSocket
    // thread the moment the response arrives, so on its own it reads as
    // "nothing in flight" for as long as the main thread takes to drain the
    // handler - hundreds of milliseconds on a 2-core board painting the home
    // panel at startup, which is exactly when every panel activates and asks.
    if ((is_fetching_.load() || delivery_pending_.load()) &&
        in_flight_scope_.load() >= static_cast<int>(scope)) {
        spdlog::debug("[HistoryManager] Load already in flight, joining it");
        return;
    }

    fetch(scope);
}

void PrintHistoryManager::ensure_covers_since(double since) {
    if (is_loaded_ && !covers_since(since)) {
        // The slice stops short of the window, so only the whole list answers
        // it. Moonraker's `since` parameter cannot be used to widen in place:
        // it would drop the newest-job selection on a printer that has printed
        // nothing inside the window.
        ensure_loaded(HistoryScope::COMPLETE);
        return;
    }
    ensure_loaded(HistoryScope::RECENT);
}

const PrintHistoryJob* PrintHistoryManager::get_newest_existing_job() const {
    if (!is_loaded_) {
        return nullptr;
    }
    for (const auto& job : cached_jobs_) {
        if (job.exists) {
            return &job;
        }
    }
    return nullptr;
}

void PrintHistoryManager::watch_connection_state() {
    if (!api_) {
        return;
    }

    // api_->printer_state() rather than the global accessor: it is the state
    // this manager's API already reads and writes, which keeps the wiring
    // honest under test.
    connection_observer_ =
        helix::observe_connection_staleness(api_->printer_state(), this, "HistoryManager");
}

void PrintHistoryManager::invalidate() {
    spdlog::debug("[HistoryManager] Cache invalidated");
    is_loaded_ = false;
}

// ============================================================================
// Observer Pattern
// ============================================================================

void PrintHistoryManager::add_observer(HistoryChangedCallback* cb) {
    if (cb && *cb) {
        observers_.push_back(cb);
        spdlog::debug("[HistoryManager] Added observer (total: {})", observers_.size());
    }
}

void PrintHistoryManager::remove_observer(HistoryChangedCallback* cb) {
    if (!cb) {
        return;
    }

    auto it = std::find(observers_.begin(), observers_.end(), cb);
    if (it != observers_.end()) {
        observers_.erase(it);
        spdlog::debug("[HistoryManager] Removed observer (remaining: {})", observers_.size());
    }
}

// ============================================================================
// Private Implementation
// ============================================================================

void PrintHistoryManager::on_history_fetched(std::vector<PrintHistoryJob>&& jobs, HistoryScope scope,
                                             int requested) {
    // The response is no longer in transit; is_loaded_ takes over as what tells
    // ensure_loaded() the cache is populated.
    delivery_pending_.store(false);
    in_flight_scope_.store(kNoFetch);

    spdlog::debug("[HistoryManager] Fetched {} jobs (limit={})", jobs.size(), requested);

    cached_jobs_ = std::move(jobs);
    // Moonraker fills a page up to the limit and stops, so a short response is
    // the whole history and the cache answers COMPLETE however narrow the
    // request was.
    holds_every_job_ = static_cast<int>(cached_jobs_.size()) < requested;
    loaded_scope_ = scope;
    build_filename_stats();

    is_loaded_ = true;
    // is_fetching_ was cleared on the BG thread before this defer was posted.

    notify_observers();

    // A change landed while this request was in flight; its response predates
    // that change, so go around once more, at the widest scope anything asked
    // for meanwhile. Bounded: the slot is cleared before the re-issue, so it
    // only repeats while changes keep arriving.
    const int queued = pending_scope_.exchange(kNoFetch);
    if (queued != kNoFetch) {
        spdlog::debug("[HistoryManager] Re-fetching (history changed mid-flight)");
        fetch(static_cast<HistoryScope>(queued));
    }
}

void PrintHistoryManager::apply_job_update(PrintHistoryJob&& job) {
    auto same_id = [&job](const PrintHistoryJob& j) { return j.job_id == job.job_id; };
    auto it = std::find_if(cached_jobs_.begin(), cached_jobs_.end(), same_id);

    if (it != cached_jobs_.end()) {
        // A job announced as added and then as finished keeps its id, so the
        // second notification replaces the first rather than duplicating it.
        *it = std::move(job);
        spdlog::debug("[HistoryManager] Patched cached job in place");
    } else {
        // Cached jobs are newest-first, and get_newest_existing_job() reads the
        // first match as the newest, so a job has to land in start_time order
        // rather than simply at the front.
        auto pos = std::lower_bound(
            cached_jobs_.begin(), cached_jobs_.end(), job.start_time,
            [](const PrintHistoryJob& j, double t) { return j.start_time > t; });
        cached_jobs_.insert(pos, std::move(job));
        spdlog::debug("[HistoryManager] Inserted job from notification ({} cached)",
                      cached_jobs_.size());
    }

    // Fidelity is unchanged. One more job does not complete a truncated cache,
    // and a cache holding every job still holds every job. The list is allowed
    // to run past its fetch limit: trimming the tail would shrink the window
    // covers_since() reports, and the growth is bounded by prints this session.
    build_filename_stats();
    notify_observers();
}

void PrintHistoryManager::invalidate_and_refetch() {
    // Stale the cache immediately: every consumer that checks is_loaded()
    // before reading must see it as stale from the moment the change is known,
    // even though the round-trip that repairs it waits out the debounce.
    invalidate();

    const HistoryScope scope = loaded_scope_;
    refetch_debounce_.schedule_once([this, scope]() { fetch(scope); });
}

void PrintHistoryManager::build_filename_stats() {
    filename_stats_.clear();

    for (const auto& job : cached_jobs_) {
        // Strip path from filename to get basename
        std::string basename = job.filename;
        auto slash_pos = basename.rfind('/');
        if (slash_pos != std::string::npos) {
            basename = basename.substr(slash_pos + 1);
        }

        if (basename.empty()) {
            continue;
        }

        auto& stats = filename_stats_[basename];

        // Count successes and failures
        if (job.status == PrintJobStatus::COMPLETED) {
            stats.success_count++;
        } else if (job.status == PrintJobStatus::CANCELLED || job.status == PrintJobStatus::ERROR) {
            stats.failure_count++;
        }

        // Track most recent job for this filename
        if (job.start_time > stats.last_print_time) {
            stats.last_print_time = job.start_time;
            stats.last_status = job.status;
            stats.uuid = job.uuid;
            stats.size_bytes = job.size_bytes;
        }
    }

    spdlog::debug("[HistoryManager] Built stats for {} unique filenames", filename_stats_.size());
}

std::vector<PrintHistoryJob> PrintHistoryManager::get_jobs_since(double since) const {
    std::vector<PrintHistoryJob> filtered;
    filtered.reserve(cached_jobs_.size()); // Avoid reallocation

    for (const auto& job : cached_jobs_) {
        if (job.start_time >= since) {
            filtered.push_back(job);
        }
    }

    return filtered;
}

void PrintHistoryManager::notify_observers() {
    // Iterate a snapshot so an observer callback can add/remove observers without
    // invalidating our loop. But the snapshot holds raw HistoryChangedCallback*
    // whose pointees can be freed mid-dispatch: a registrant destroyed during
    // this pass (e.g. a PrintStatusWidget torn down on panel teardown) removes
    // itself via remove_observer() from its destructor. Re-check each pointer
    // against the live set before calling — a pointer no longer in observers_
    // has been removed and may now dangle, so calling through it is a
    // use-after-free (debug bundle S52DJB5W: SIGSEGV at ~0x800020c during
    // nav-away + reconnect).
    auto observers_copy = observers_;
    for (auto* cb : observers_copy) {
        if (std::find(observers_.begin(), observers_.end(), cb) == observers_.end()) {
            continue;
        }
        if (cb && *cb) {
            (*cb)();
        }
    }
}

bool PrintHistoryManager::history_action_carries_job(const std::string& action) {
    // The only two actions Moonraker emits on this notification, both from its
    // job tracker, and both carrying the complete job record the list endpoint
    // would return for it.
    return action == "added" || action == "finished";
}

bool PrintHistoryManager::filelist_action_affects_history(const std::string& action) {
    // notify_filelist_changed fires for every file operation, including uploads
    // and Moonraker's own metadata scans (an AFC printer rewrites
    // AFC/AFC.var.unit on every SET_* command). Only the actions that can make
    // a job's file stop being where history says it is invalidate the cached
    // `exists` flags; everything else must not cost a history round-trip.
    return action == "delete_file" || action == "delete_dir" || action == "move_file" ||
           action == "move_dir";
}

void PrintHistoryManager::subscribe_to_notifications() {
    if (!client_) {
        return;
    }

    auto token = lifetime_.token();

    // Bare expired() check on the bg thread is the L081 Mechanism C anti-pattern
    // (detector hit on v0.99.60/ad5x telemetry). token.defer() runs its own
    // expiration check atomically on the main thread, so the bg-side short-circuit
    // gains nothing and trips the watchdog. Let defer handle it.
    // The notification carries the whole job it is announcing, so the common
    // case folds one record into the cache instead of re-pulling the list. The
    // refetch below is the fallback for a payload we cannot use: a foreign
    // action, a missing job object, or a cache with nothing to patch.
    client_->register_method_callback(
        "notify_history_changed", "PrintHistoryManager",
        [this, token](const nlohmann::json& data) {
            spdlog::debug("[HistoryManager] Received notify_history_changed");

            // bg thread: parse only, no member access. Moonraker can send
            // null/missing fields, so probe before reading.
            std::string action;
            nlohmann::json job_json;
            if (const auto* payload = helix::json_util::notification_payload(data)) {
                action = helix::json_util::safe_string(*payload, "action");
                const auto job_it = payload->find("job");
                if (job_it != payload->end() && job_it->is_object()) {
                    job_json = *job_it;
                }
            }

            if (history_action_carries_job(action) && job_json.is_object()) {
                // Same parser the list response goes through, including the
                // server-recomputed `exists` flag.
                PrintHistoryJob job = helix::parse_history_job(job_json);
                token.defer("PrintHistoryManager::history_job_patch",
                            [this, action, job = std::move(job)]() mutable {
                                if (!is_loaded_) {
                                    // Nothing to amend. A patch cannot
                                    // establish which jobs precede this one.
                                    fetch(loaded_scope_);
                                    return;
                                }
                                spdlog::debug("[HistoryManager] Applying '{}' job from "
                                              "notification, no refetch",
                                              action);
                                apply_job_update(std::move(job));
                            });
                return;
            }

            token.defer("PrintHistoryManager::notify_history_changed",
                        [this]() { invalidate_and_refetch(); });
        });

    // Deleting or moving a gcode file fires notify_filelist_changed, never
    // notify_history_changed - Moonraker emits the latter only when a job is
    // added or history is cleared. Without this the cache keeps serving a job
    // whose file is gone, which the Print Status idle tile then offers as
    // "Reprint Last". Covers our own deletes and external ones alike (Mainsail,
    // Fluidd, OrcaSlicer).
    client_->register_method_callback(
        "notify_filelist_changed", "PrintHistoryManager",
        [this, token](const nlohmann::json& data) {
            // bg thread: parse into a plain string only, no member access.
            const std::string action = helix::json_util::notification_action(data);
            if (!filelist_action_affects_history(action)) {
                return;
            }
            spdlog::debug("[HistoryManager] filelist action '{}' invalidates history", action);
            token.defer("PrintHistoryManager::notify_filelist_changed",
                        [this]() { invalidate_and_refetch(); });
        });
}
