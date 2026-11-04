// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_callback.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace helix::http {

/// Work item executed on a worker thread.
using HttpWork = helix::Callback<void()>;

/// Bounded-worker executor for HTTP work. Replaces per-request std::thread
/// spawning, which failed with EAGAIN under thread exhaustion (see commits
/// 04884df30, 453ec3703 for the band-aid this class supersedes).
///
/// Two process-wide instances are exposed — `fast()` for status/REST calls
/// (4 workers, preserves burst parallelism for thumbnail loading etc.)
/// and `slow()` for large file transfers (1 worker, avoids head-of-line
/// blocking quick requests behind multi-minute uploads).
///
/// Public API mirrors helix::bluetooth::BusThread for consistency:
/// submit/run_sync/stop/on_thread.
class HttpExecutor {
  public:
    /// Construct with a logging name and worker count. Worker count is
    /// clamped to >=1. Workers share a FIFO queue.
    HttpExecutor(std::string name, std::size_t worker_count);
    ~HttpExecutor();

    HttpExecutor(const HttpExecutor&) = delete;
    HttpExecutor& operator=(const HttpExecutor&) = delete;

    /// Launch the workers. Idempotent. A no-op on the ESP32 firmware, which
    /// runs its HTTP through EspHttpLane: each worker's stack comes from
    /// internal RAM, and a pool of them starves the network task at boot.
    /// submit() on a pool that never started drops the work.
    void start();

    /// Whether the workers are running. Main thread only.
    [[nodiscard]] bool running() const {
        return running_;
    }

    /// Drain currently-executing items up to `join_timeout`, break promises
    /// on queued items, join or detach workers. Idempotent. Safe to call
    /// from the destructor.
    ///
    /// If a worker doesn't finish its current item within `join_timeout`,
    /// it is detached — the thread runs until the item naturally completes
    /// or the process exits. This prevents shutdown hangs during long HTTP
    /// transfers (libhv blocking calls can take up to an hour) at the cost
    /// of leaking the work-item state for the remainder of the process.
    /// Safe for process-exit singletons; don't destruct a non-singleton
    /// HttpExecutor with a pending long-running item unless you've
    /// confirmed the timeout is generous enough.
    void stop(std::chrono::milliseconds join_timeout = std::chrono::seconds(2));

    /// Queue `work` for execution. Returns a future that resolves when the
    /// work completes. If the executor is stopped before the item runs,
    /// the promise is broken and `future::get()` throws std::future_error.
    ///
    /// Safe to call from any thread, including re-entrantly from inside
    /// another work item. Callers MUST NOT wait on the returned future
    /// from inside a work item (self-wait deadlock risk on single-worker
    /// lanes; wasted serialization on multi-worker lanes).
    std::future<void> submit(HttpWork work);

    /// Convenience: submit() + wait(). Throws std::future_error if stopped
    /// before the work ran; propagates any exception the work raised.
    /// MUST NOT be called from a worker thread on a single-worker lane
    /// (self-wait deadlock). Check `on_thread()` if unsure.
    void run_sync(HttpWork work);

    /// True if the calling thread is one of this executor's workers.
    /// Uses a thread_local back-pointer, so it correctly distinguishes
    /// between multiple HttpExecutor instances.
    bool on_thread() const noexcept;

    /// Items submitted but not yet finished, across queue and workers.
    /// 0 if the executor isn't running — same convention as submit() finding
    /// nothing to reject into.
    std::size_t inflight() const noexcept;

    /// Process-wide executors.
    static HttpExecutor& fast(); ///< REST/API/timelapse/thumbnails (4 workers)
    static HttpExecutor& slow(); ///< large file transfers (1 worker)
    static void start_all();     ///< called at app init
    static void stop_all();      ///< called at app shutdown

  private:
    // Shared state held behind a shared_ptr so it survives past HttpExecutor
    // destruction while any detached worker is still running. Without this,
    // a worker stuck in a long HTTP call (that stop() gave up on and detached)
    // would wake up and attempt to re-acquire `mu` on a destroyed mutex
    // (EINVAL → std::system_error → terminate). See fix notes on the
    // `HttpExecutor: stop with timeout detaches stuck worker` test.
    struct SharedState {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::pair<HttpWork, std::promise<void>>> queue;
        bool stopping = false; // guarded by mu
        // Items submitted but not yet finished — incremented in submit() at
        // enqueue time, decremented by the worker once the item returns
        // (including via exception, see InflightGuard in loop()).
        std::atomic<std::size_t> inflight{0};
    };

    struct Worker {
        std::thread thread;
        std::atomic<bool> done{false};
    };

    static void loop(std::shared_ptr<SharedState> state, HttpExecutor* owner, std::string name,
                     std::size_t worker_index);

    std::string name_;
    std::size_t worker_count_;
    std::shared_ptr<SharedState> state_;
    // Workers are shared_ptr so detached threads keep their Worker alive
    // (the thread's final `done.store(true)` would otherwise UAF the raw
    // pointer after stop() clears `workers_`).
    std::vector<std::shared_ptr<Worker>> workers_;
    bool running_ = false; // main-thread only; guarded by start/stop sequencing
};

} // namespace helix::http
