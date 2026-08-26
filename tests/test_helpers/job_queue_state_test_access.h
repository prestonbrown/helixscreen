// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "job_queue_state.h"

#include <utility>
#include <vector>

/// Reaches JobQueueState's private cached-job list so widget tests can seed
/// queued jobs directly. JobQueueState only ever populates cached_jobs_ /
/// is_loaded_ from a real Moonraker response (on_queue_fetched(), private),
/// so there is no public path to a loaded-with-jobs state without a live
/// (or mocked end-to-end) connection.
class JobQueueStateTestAccess {
  public:
    static void set_jobs(JobQueueState& state, std::vector<JobQueueEntry> jobs) {
        state.cached_jobs_ = std::move(jobs);
        state.is_loaded_ = true;
    }

    /// Drives a queue response through the real fetch-completion path
    /// (on_queue_fetched -> update_subjects), which is what publishes the
    /// job_queue_count / job_queue_state_text / job_queue_summary_text
    /// subjects. set_jobs() above deliberately does NOT touch subjects, so a
    /// test that needs the observer wiring to fire must come through here.
    ///
    /// In production this is only ever reached from fetch()'s success callback
    /// via tok.defer(), i.e. on the main thread — call it on the main thread
    /// here too.
    static void deliver_status(JobQueueState& state, const JobQueueStatus& status) {
        state.on_queue_fetched(status);
    }
};
