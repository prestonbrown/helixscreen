// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "http_executor.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace helix {

/**
 * @brief One background job at a time on HttpExecutor::fast(), newest wins.
 *
 * For slow disk walks (a USB stick) whose answer the UI shows. UI thread only.
 * A run() while a job is going makes that job's cancelled() return true and
 * drops its result; the newest request then runs once it ends, so any number
 * of requests during a walk cost one more walk. cancel() and destruction drop
 * both the running job's result and any queued request.
 */
class SingleFlightWalk {
  public:
    using Cancelled = std::function<bool()>;
    /// Runs on a worker; returns what to do with its result on the UI thread.
    using Job = std::function<std::function<void()>(const Cancelled& cancelled)>;

    /// @p lane is where jobs run; with no workers running they run inline.
    explicit SingleFlightWalk(helix::http::HttpExecutor& lane = helix::http::HttpExecutor::fast())
        : lane_(lane) {}
    ~SingleFlightWalk() {
        cancel();
    }
    SingleFlightWalk(const SingleFlightWalk&) = delete;
    SingleFlightWalk& operator=(const SingleFlightWalk&) = delete;

    void run(Job job) {
        ++*generation_;
        pending_ = std::move(job);
        if (!in_flight_) {
            start_pending();
        }
    }

    void cancel() {
        ++*generation_;
        pending_ = nullptr;
    }

    /// A job is running or its result has not been handled yet.
    [[nodiscard]] bool in_flight() const {
        return in_flight_;
    }

  private:
    void start_pending() {
        Job job = std::move(pending_);
        pending_ = nullptr;
        if (!lane_.running()) {
            // submit() would drop the job, leaving in_flight_ set forever.
            auto deliver = job([]() { return false; });
            if (deliver) {
                deliver();
            }
            return;
        }
        in_flight_ = true;
        const uint64_t generation = generation_->load();
        lane_.submit([this, tok = lifetime_.token(), job = std::move(job), generation,
                      current = generation_]() {
            auto deliver = job([&]() { return current->load() != generation; });
            tok.defer("SingleFlightWalk::done", [this, generation, deliver = std::move(deliver)]() {
                in_flight_ = false;
                if (generation == generation_->load()) {
                    if (deliver) {
                        deliver();
                    }
                } else if (pending_) {
                    start_pending();
                }
            });
        });
    }

    helix::http::HttpExecutor& lane_;
    helix::AsyncLifetimeGuard lifetime_;
    std::shared_ptr<std::atomic<uint64_t>> generation_ = std::make_shared<std::atomic<uint64_t>>(0);
    bool in_flight_ = false;
    Job pending_;
};

} // namespace helix
