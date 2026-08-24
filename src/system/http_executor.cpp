// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_executor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix::http {

namespace {

// Thread-local pointer to the executor owning the current worker thread.
// Set by loop() on entry, cleared on exit. Powers on_thread() and correctly
// distinguishes between multiple HttpExecutor instances.
thread_local HttpExecutor* tls_current_executor_ = nullptr;

} // namespace

HttpExecutor::HttpExecutor(std::string name, std::size_t worker_count)
    : name_(std::move(name)), worker_count_(std::max<std::size_t>(worker_count, 1)) {}

HttpExecutor::~HttpExecutor() {
    stop();
}

void HttpExecutor::start() {
    if (running_) {
        return;
    }
    // Fresh shared state for this start cycle (a previous stop() may have
    // transferred ownership to detached workers that are still alive).
    // std::atomic_store so concurrent submit() calls always read a complete
    // pointer (shared_ptr copy is not atomic by itself).
    std::atomic_store(&state_, std::make_shared<SharedState>());
    running_ = true;

    workers_.reserve(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i) {
        auto w = std::make_shared<Worker>();
        auto state = std::atomic_load(&state_); // shared_ptr copy for the thread to hold
        auto name = name_;
        auto* owner = this;
        w->thread = std::thread([state, w, owner, name, i]() {
            loop(state, owner, name, i);
            // Safe even if stop() has already cleared `workers_`: `w` is a
            // captured shared_ptr, so the Worker object lives until the
            // thread's lambda returns.
            w->done.store(true);
        });
        workers_.push_back(std::move(w));
    }
}

void HttpExecutor::stop(std::chrono::milliseconds join_timeout) {
    if (!running_) {
        return;
    }
    running_ = false;

    auto state = std::atomic_load(&state_);
    if (state) {
        {
            std::lock_guard<std::mutex> lk(state->mu);
            state->stopping = true;
        }
        state->cv.notify_all();
    }

    // Wait for each worker to finish its in-flight item, up to join_timeout.
    // Poll a per-worker done flag set at the end of the thread lambda — lets
    // us detach a worker that's stuck in a long HTTP call without blocking
    // the others.
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds(10);
    auto deadline = std::chrono::steady_clock::now() + join_timeout;
    for (auto& w : workers_) {
        while (!w->done.load()) {
            if (std::chrono::steady_clock::now() > deadline) {
                spdlog::warn("[HttpExecutor:{}] worker did not finish in {}ms — "
                             "detaching (will terminate with process)",
                             name_, join_timeout.count());
                w->thread.detach();
                break;
            }
            std::this_thread::sleep_for(POLL_INTERVAL);
        }
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
    workers_.clear();

    // Break promises on anything still queued. By this point submit() rejects
    // new work (state->stopping is true under state->mu), so the queue is stable.
    if (state) {
        std::deque<std::pair<HttpWork, std::promise<void>>> leftover;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            leftover = std::move(state->queue);
        }
        // leftover's promises are dropped as it goes out of scope → broken_promise
        // surfaces to anyone holding the futures.
    }

    // Drop our handle to the shared state. Detached workers (if any) still
    // hold their own shared_ptr and will release it when the thread exits.
    std::atomic_store(&state_, std::shared_ptr<SharedState>{});
}

std::future<void> HttpExecutor::submit(HttpWork work) {
    std::promise<void> promise;
    auto fut = promise.get_future();

    auto state = std::atomic_load(&state_);
    if (!state) {
        // Not started (or already stopped) — broken promise.
        return fut;
    }

    {
        std::lock_guard<std::mutex> lk(state->mu);
        if (state->stopping) {
            // Reject: drop the promise → broken_promise on future::get().
            return fut;
        }
        state->inflight.fetch_add(1, std::memory_order_relaxed);
        state->queue.emplace_back(std::move(work), std::move(promise));
    }
    state->cv.notify_one();
    return fut;
}

void HttpExecutor::run_sync(HttpWork work) {
    auto fut = submit(std::move(work));
    fut.get(); // Throws std::future_error if stopped before running.
}

bool HttpExecutor::on_thread() const noexcept {
    return tls_current_executor_ == this;
}

std::size_t HttpExecutor::inflight() const noexcept {
    auto state = std::atomic_load(&state_);
    return state ? state->inflight.load(std::memory_order_relaxed) : 0;
}

void HttpExecutor::loop(std::shared_ptr<SharedState> state, HttpExecutor* owner, std::string name,
                        std::size_t worker_index) {
    tls_current_executor_ = owner;
    spdlog::debug("[HttpExecutor:{}] worker {} started", name, worker_index);

    while (true) {
        std::pair<HttpWork, std::promise<void>> item;
        {
            std::unique_lock<std::mutex> lk(state->mu);
            state->cv.wait(lk, [&state]() { return state->stopping || !state->queue.empty(); });
            // On stop, exit immediately regardless of remaining queue.
            // Leftover items have their promises broken by stop() afterward.
            if (state->stopping) {
                break;
            }
            item = std::move(state->queue.front());
            state->queue.pop_front();
        }

        // Scope-guard the decrement so a throwing item still releases its
        // inflight slot — wait_idle (and anyone else polling inflight())
        // must never see a stuck counter because a job threw.
        struct InflightGuard {
            std::atomic<std::size_t>& c;
            ~InflightGuard() {
                c.fetch_sub(1, std::memory_order_relaxed);
            }
        } guard{state->inflight};

        try {
            item.first();
            item.second.set_value();
        } catch (...) {
            // Surface the exception via the future. Worker continues.
            try {
                item.second.set_exception(std::current_exception());
            } catch (...) {
                // Promise already satisfied or no shared state — nothing to do.
            }
        }
    }

    // NOTE: `owner` may already be destroyed by the time we reach here if
    // stop() detached us. Clearing the TLS pointer via the stored address
    // is still well-defined (we're writing to our own thread-local, not
    // dereferencing `owner`).
    tls_current_executor_ = nullptr;
    spdlog::debug("[HttpExecutor:{}] worker {} exiting", name, worker_index);
}

// ---------------------------------------------------------------------------
// Process-wide executors
// ---------------------------------------------------------------------------

HttpExecutor& HttpExecutor::fast() {
    // 4 workers: preserves burst parallelism for thumbnail loading and
    // concurrent status polls without reintroducing unbounded spawning.
    static HttpExecutor instance("fast", 4);
    return instance;
}

HttpExecutor& HttpExecutor::slow() {
    // 1 worker: serializes large file transfers, guaranteeing uploads
    // don't saturate the network or flood libhv concurrent-request limits.
    static HttpExecutor instance("slow", 1);
    return instance;
}

void HttpExecutor::start_all() {
    fast().start();
    slow().start();
}

void HttpExecutor::stop_all() {
    // Stop slow first so any in-flight upload gets its join_timeout window
    // before fast lane teardown noise starts.
    slow().stop();
    fast().stop();
}

} // namespace helix::http
