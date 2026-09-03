// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_update_queue.h"

#include "lvgl/src/misc/lv_timer_private.h"

#include <vector>

namespace helix::ui {

class UpdateQueueTestAccess {
  public:
    static void drain(UpdateQueue& q) {
        q.process_pending();
    }

    /// Drop queued callbacks WITHOUT running them.
    ///
    /// The cross-test counterpart to drain(). Once a test has returned, its
    /// fixture is destroyed and anything it left queued closes over dead state,
    /// so *executing* those callbacks is precisely the use-after-free — draining
    /// is only safe while the owning objects are still alive (i.e. inside the
    /// fixture's own destructor body). Between tests, discard.
    /// Returns the tag of each dropped callback, in queue order. Naming the
    /// producer is what makes a leak actionable: a tag pointing at a process
    /// singleton (AmsState::…) is benign unflushed work, while one closing over
    /// a per-test object is a real use-after-free waiting for the next drain.
    /// Untagged callbacks report as "<untagged>".
    /// One dropped callback: its producer tag, plus the call site recorded for
    /// untagged enqueues (file == nullptr when the producer passed a tag).
    struct DroppedCallback {
        const char* tag = nullptr; ///< "<untagged>" when the producer passed none
        const char* file = nullptr;
        int line = 0;
    };

    static std::vector<DroppedCallback> discard_pending(UpdateQueue& q) {
        std::queue<TaggedCallback> dropped;
        {
            std::lock_guard<std::mutex> lock(q.mutex_);
            std::swap(dropped, q.pending_);
        }
        // Collect tags and destroy the callbacks outside the lock. They are never
        // invoked, so the state they capture is only released, never dereferenced.
        std::vector<DroppedCallback> out;
        out.reserve(dropped.size());
        while (!dropped.empty()) {
            const TaggedCallback& e = dropped.front();
            out.push_back({e.tag != nullptr ? e.tag : "<untagged>",
                           e.tag != nullptr ? nullptr : e.file, e.line});
            dropped.pop();
        }
        return out;
    }

    /// Number of queued callbacks that have thrown since process start.
    ///
    /// process_pending() swallows callback exceptions on purpose, so a test that
    /// drains the queue sees success whether the callback ran or blew up. Snapshot
    /// this before draining and compare after to assert the callback ran clean.
    static uint32_t callback_exception_count() {
        return UpdateQueue::callback_exception_count_.load(std::memory_order_relaxed);
    }

    /// True when nothing is queued.
    ///
    /// A settle loop that waits on a worker pool needs this: the pool reports
    /// idle the moment its task finishes, but the task's result is still sitting
    /// in this queue, so pool-idle alone is not "everything has landed".
    static bool queue_empty(UpdateQueue& q) {
        std::lock_guard<std::mutex> lock(q.mutex_);
        return q.pending_.empty();
    }

    /// Period of the drain timer, in milliseconds.
    ///
    /// lv_timer_handler() answers with the shortest time until any live timer is
    /// next due, and the main loop sleeps for that long, so this timer's period
    /// is a floor on how often the whole loop wakes up.
    static uint32_t timer_period(UpdateQueue& q) {
        return q.timer_ != nullptr ? q.timer_->period : 0;
    }

    /// Drain repeatedly until the queue is fully empty (handles nested queue_update calls)
    static void drain_all(UpdateQueue& q, int max_iterations = 10) {
        for (int i = 0; i < max_iterations; ++i) {
            {
                std::lock_guard<std::mutex> lock(q.mutex_);
                if (q.pending_.empty())
                    return;
            }
            q.process_pending();
        }
    }
};

} // namespace helix::ui

// Convenience alias for use with 'using namespace helix'
