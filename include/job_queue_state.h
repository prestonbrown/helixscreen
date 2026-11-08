// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "job_queue_tile_state.h"
#include "moonraker_queue_api.h"
#include "subject_managed_panel.h" // SubjectManager

#include <atomic>
#include <lvgl.h>
#include <string>
#include <vector>

struct FileMetadata; // NAMESPACE_OK: declared at global scope in moonraker_types.h

namespace helix {
class IMoonrakerAPI;
class IMoonrakerClient;

/// Parse Moonraker's `job_queue.automatic_transition` out of a `server.config`
/// JSON-RPC response (the full message; "result" is unwrapped here). False
/// when the section, the key, or a well-formed response is absent, and when
/// the value is not a boolean — matching Moonraker's own default.
bool parse_automatic_transition(const json& rpc_response);

/// "Up next: <name> (+N)" — the while-printing queue line. Empty when
/// @p queued_count is 0 or the name is; callers bind visibility to
/// job_queue_count, so the empty form never renders.
std::string format_up_next_text(const std::string& display_name, int queued_count);

/// "Start next: <name>" — the completion modal's secondary button. Empty on
/// an empty queue (the button hides with it).
std::string format_start_next_text(const std::string& display_name, int queued_count);

/// The queue tile's strings for one JobQueueTileState. Fields a state does not
/// show are empty. @p next_estimated_s of 0 means unknown. @p more_count is how many
/// queued jobs the tile neither names nor lists; "+N more" is empty at 0.
struct JobQueueTileText {
    std::string hero;
    std::string count; ///< the queued count alone, for the Strip
    std::string detail;
    std::string more;
    std::string action;
    std::string action_short;
};
JobQueueTileText compose_job_queue_tile_text(JobQueueTileState state,
                                             const std::string& next_display_name, int queued_count,
                                             int next_estimated_s, int more_count);
} // namespace helix

/**
 * @brief Job queue state manager bridging Moonraker Queue API to LVGL subjects
 *
 * Fetches queue status from Moonraker, caches job entries, and exposes
 * LVGL subjects for declarative XML binding. Subscribes to WebSocket
 * notifications for live updates.
 *
 * Created by Application, accessed via get_job_queue_state() global accessor.
 */
class JobQueueState {
  public:
    JobQueueState(helix::IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~JobQueueState();

    JobQueueState(const JobQueueState&) = delete;
    JobQueueState& operator=(const JobQueueState&) = delete;

    /// Fetch queue status from API, update subjects
    void fetch();

    /// Whether any load has completed. The connection-staleness watcher's
    /// question - see observe_connection_staleness().
    bool has_cached_data() const {
        return is_loaded_;
    }

    /// Check if data has been loaded
    bool is_loaded() const {
        return is_loaded_;
    }

    /// Mark the cached queue stale without dropping it
    ///
    /// Leaves cached_jobs_ in place so a reader mid-outage still renders the
    /// last known queue rather than blanking; only the loaded-latch drops, so
    /// the next consumer to ask re-fetches. See observe_connection_staleness().
    void invalidate();

    const std::vector<helix::JobQueueEntry>& get_jobs() const {
        return cached_jobs_;
    }

    /// Whether a queue response has ever arrived. Unlike is_loaded() it survives
    /// invalidate(), so a dropped socket keeps showing the last known queue.
    [[nodiscard]] bool has_ever_loaded() const {
        return ever_loaded_;
    }

    [[nodiscard]] helix::QueueRunState run_state() const {
        return helix::parse_queue_run_state(queue_state_);
    }

    /// Estimated print time of the first queued job, in seconds. 0 while its
    /// metadata is unknown, absent or still loading.
    [[nodiscard]] int next_estimated_seconds() const {
        return next_estimated_s_;
    }

    /// Whether Moonraker starts the next queued job by itself on completion
    ///
    /// Read from `server.config` (`config.job_queue.automatic_transition`)
    /// once per connect — false when the key, the section or the whole config
    /// is unreadable, which is also Moonraker's own default. While true,
    /// Moonraker calls start_print itself and HelixScreen's per-job option
    /// store cannot participate, so UI hides it.
    bool automatic_transition() const {
        return automatic_transition_;
    }

    /// Whether the home tile's one-tap start (QueuedJobStarter) is checking
    /// and starting the first job. Published as job_queue_start_in_flight.
    void set_start_in_flight(bool in_flight);
    [[nodiscard]] bool start_in_flight() const {
        return start_in_flight_;
    }

    /// Initialize LVGL subjects (call before XML creation)
    void init_subjects();

    /// Death signal for the subjects this object owns.
    ///
    /// deinit_subjects() and the destructor both free every observer node on
    /// them without bumping the ObserverGuard invalidation epoch, so outside
    /// observers must pass this to observe_*(), or their guards call
    /// lv_observer_remove() on a freed node.
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

  private:
    friend class JobQueueStateTestAccess;

    void on_queue_fetched(const helix::JobQueueStatus& status);
    void subscribe_to_notifications();

    /**
     * @brief Read Moonraker's job_queue config once per connect
     *
     * Called from fetch(). Latched: while @p automatic_transition_loaded_
     * holds, further fetches (widget activations) skip the request. A socket
     * drop invalidates the latch via invalidate(), so the next connect
     * re-reads — a Moonraker restart is free to have changed the config.
     */
    void fetch_automatic_transition();

    /// Drops stored per-job options whose job left the queue (async, best-effort)
    void prune_option_store();

    /**
     * @brief Mark the queue stale whenever the Moonraker socket is not up
     *
     * Called in constructor. notify_job_queue_changed only reaches a live
     * socket, so a queue edited from another client while we are down is never
     * announced. See observe_connection_staleness() in connection_staleness.h.
     */
    void watch_connection_state();
    void update_subjects();
    void deinit_subjects();

    /// Fetch the first queued job's metadata once per change of first job.
    void refresh_next_job_media();
    /// Main thread. Drops an answer for a job that is no longer first.
    void on_next_job_metadata(const std::string& filename, const FileMetadata& metadata);
    /// Unknown estimate and the placeholder thumbnail; supersedes any load in flight.
    void clear_next_job_media();

    helix::IMoonrakerAPI* api_;
    helix::IMoonrakerClient* client_;

    // Cached data
    std::vector<helix::JobQueueEntry> cached_jobs_;
    std::string queue_state_ = "ready";

    /// Watches printer connection state so a dropped socket stales the queue.
    ObserverGuard connection_observer_;

    // State
    bool is_loaded_ = false;
    bool ever_loaded_ = false; // set in on_queue_fetched(), never cleared
    // Atomic so the BG callback can clear it before the defer is posted —
    // prevents UpdateQueue freeze-drops from stranding the fetch guard.
    std::atomic<bool> is_fetching_{false};
    bool subjects_initialized_ = false;
    bool automatic_transition_ = false;
    bool automatic_transition_loaded_ = false;
    // Option-store prune latched per connect — see on_queue_fetched().
    bool pruned_this_connect_ = false;
    bool start_in_flight_ = false;
    int jobs_revision_ = 0;

    // LVGL subjects
    // "Up next: <name> (+N)" for the print-status panel and home widget; the
    // display name of the first queued job, "" when the queue is empty.
    lv_subject_t job_queue_up_next_text_subject_{};
    char up_next_text_buffer_[320];
    // "Start next: <name>" for the completion modal's secondary button.
    lv_subject_t job_queue_start_next_text_subject_{};
    char start_next_text_buffer_[320];
    // automatic_transition as 0/1, for XML bindings that hide queue-mode UI
    // Moonraker's own start would bypass.
    lv_subject_t job_queue_automatic_transition_subject_{};
    // Queued-job count. The refresh channel for every queue surface: the home
    // panel's job_queue widget, the print-status widget's queue row, and the
    // job-queue modal each observe it and rebuild off a change. Nothing else
    // rebuilds them, so a queue mutation that does not move this subject is
    // invisible until the next resize.
    lv_subject_t job_queue_count_subject_{};
    // helix::QueueRunState as an int. Published before the count so a count
    // observer reads it settled.
    lv_subject_t job_queue_run_state_subject_{};
    // Bumped whenever the queued jobs change, before the count. A reorder, or
    // a remove plus an add, leaves the count where it was, so surfaces that
    // name the jobs observe this.
    lv_subject_t job_queue_revision_subject_{};
    // start_in_flight_ as 0/1.
    lv_subject_t job_queue_start_in_flight_subject_{};
    // The first queued job's estimated time (seconds, 0 = unknown) and
    // thumbnail path. The thumbnail is never "": it holds the placeholder
    // until a real image is cached.
    std::string next_media_filename_;
    /// Set by invalidate() and by a failed read: the next fetch asks again for
    /// the same first job.
    bool next_media_stale_ = false;
    int next_estimated_s_ = 0;
    std::atomic<uint32_t> next_thumbnail_generation_{0};
    lv_subject_t job_queue_next_estimate_subject_{};
    lv_subject_t job_queue_next_thumbnail_subject_{};
    char next_thumbnail_buf_[256];
    /// Owns every subject above and the death signal
    /// get_subjects_lifetime() hands out.
    SubjectManager subjects_;

    // Async callback safety guard
    helix::AsyncLifetimeGuard lifetime_;
};
