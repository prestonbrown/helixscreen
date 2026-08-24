// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_history_manager.h"

#include "ui_update_queue.h"

#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"

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
}

PrintHistoryManager::~PrintHistoryManager() {
    // Unregister notification callbacks
    if (client_) {
        client_->unregister_method_callback("notify_history_changed", "PrintHistoryManager");
        client_->unregister_method_callback("notify_filelist_changed", "PrintHistoryManager");
    }
}

// ============================================================================
// Fetch / Refresh
// ============================================================================

void PrintHistoryManager::fetch(int limit) {
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
        refetch_pending_.store(true);
        spdlog::debug("[HistoryManager] Fetch already in progress, queuing one refetch");
        return;
    }

    spdlog::debug("[HistoryManager] Fetching history (limit={})", limit);

    auto token = lifetime_.token();

    api_->history().get_history_list(
        limit, 0, 0.0, 0.0, // limit, start, since, before
        [this, token](const std::vector<PrintHistoryJob>& jobs, uint64_t /*total*/) {
            // Clear guard BEFORE posting defer so a freeze-drop doesn't strand us.
            is_fetching_.store(false);
            // No bare expired() check — token.defer's own guard suffices, and
            // dropping the bare check silences the bg_tok_expired_check
            // detector for this site (3XNZQB2R audit). The std::vector copy
            // below is harmless if defer skips.
            std::vector<PrintHistoryJob> jobs_copy = jobs;
            token.defer("PrintHistoryManager::fetch_success",
                        [this, jobs = std::move(jobs_copy)]() mutable {
                            on_history_fetched(std::move(jobs));
                        });
        },
        [this, token](const MoonrakerError& error) {
            is_fetching_.store(false);
            // spdlog is thread-safe; logging the warn even on a destroyed
            // manager is harmless (informational). Dropping the bare
            // expired() check silences the detector here.
            (void)token;
            spdlog::warn("[HistoryManager] Failed to fetch history: {}", error.message);
        });
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

void PrintHistoryManager::on_history_fetched(std::vector<PrintHistoryJob>&& jobs) {
    spdlog::debug("[HistoryManager] Fetched {} jobs", jobs.size());

    cached_jobs_ = std::move(jobs);
    build_filename_stats();

    is_loaded_ = true;
    // is_fetching_ was cleared on the BG thread before this defer was posted.

    notify_observers();

    // A change landed while this request was in flight; its response predates
    // that change, so go around once more. Bounded: the flag is cleared before
    // the re-issue, so it only repeats while changes keep arriving.
    if (refetch_pending_.exchange(false)) {
        spdlog::debug("[HistoryManager] Re-fetching (history changed mid-flight)");
        fetch();
    }
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
    client_->register_method_callback(
        "notify_history_changed", "PrintHistoryManager",
        [this, token](const nlohmann::json& /*data*/) {
            spdlog::debug("[HistoryManager] Received notify_history_changed");
            token.defer("PrintHistoryManager::notify_history_changed", [this]() {
                invalidate();
                fetch();
            });
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
            // Moonraker can send null/missing fields, so probe before reading.
            std::string action;
            const auto params_it = data.find("params");
            if (params_it != data.end() && params_it->is_array() && !params_it->empty()) {
                const nlohmann::json& p = (*params_it)[0];
                if (p.is_object()) {
                    action = helix::json_util::safe_string(p, "action");
                }
            }
            if (!filelist_action_affects_history(action)) {
                return;
            }
            spdlog::debug("[HistoryManager] filelist action '{}' invalidates history", action);
            token.defer("PrintHistoryManager::notify_filelist_changed", [this]() {
                // invalidate() first: fetch() is not gated on is_loaded_, but
                // every consumer that checks is_loaded_ before reading must see
                // the cache as stale until the refetch lands.
                invalidate();
                fetch();
            });
        });
}
