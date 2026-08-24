// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "moonraker_queue_api.h"

#include <atomic>
#include <lvgl.h>
#include <string>
#include <vector>

class IMoonrakerAPI;
namespace helix {
class IMoonrakerClient;
}

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
    JobQueueState(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~JobQueueState();

    // Non-copyable
    JobQueueState(const JobQueueState&) = delete;
    JobQueueState& operator=(const JobQueueState&) = delete;

    /// Fetch queue status from API, update subjects
    void fetch();

    /// Check if data has been loaded
    bool is_loaded() const {
        return is_loaded_;
    }

    /// Get cached jobs
    const std::vector<JobQueueEntry>& get_jobs() const {
        return cached_jobs_;
    }

    /// Get queue state string
    const std::string& get_queue_state() const {
        return queue_state_;
    }

    /// Initialize LVGL subjects (call before XML creation)
    void init_subjects();

  private:
    friend class JobQueueStateTestAccess;

    void on_queue_fetched(const JobQueueStatus& status);
    void subscribe_to_notifications();
    void update_subjects();
    void deinit_subjects();

    IMoonrakerAPI* api_;
    helix::IMoonrakerClient* client_;

    // Cached data
    std::vector<JobQueueEntry> cached_jobs_;
    std::string queue_state_ = "ready";

    // State
    bool is_loaded_ = false;
    // Atomic so the BG callback can clear it before the defer is posted —
    // prevents UpdateQueue freeze-drops from stranding the fetch guard.
    std::atomic<bool> is_fetching_{false};
    bool subjects_initialized_ = false;

    // LVGL subjects
    lv_subject_t job_queue_state_subject_;
    char state_buffer_[64];
    lv_subject_t job_queue_summary_subject_;
    char summary_buffer_[128];

    // Async callback safety guard
    helix::AsyncLifetimeGuard lifetime_;
};
