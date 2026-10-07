// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The order of a WebSocket client's transport operations, kept apart from the transport so
// the desktop test build can drive it with fakes (no ESP-IDF includes).
//
// Every operation runs as a job on one worker, in order. connect() and disconnect() only
// queue; the worker decides from the request generation what a job still has to do, so a
// later request always wins over an earlier one that has not run yet.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace helix::net {

template <typename Handle> class TransportLifecycle {
  public:
    struct Ops {
        /// Queue a job on the worker; jobs run one at a time, in order.
        std::function<void(const char* what, std::function<void()> job)> post;
        /// Create a transport for the URL, not yet started; nullptr when it cannot.
        std::function<Handle(const std::string& url)> create;
        /// Start a created or stopped transport; false when it cannot.
        std::function<bool(Handle)> begin;
        /// Stop a transport (waits for its task); safe on a stopped one.
        std::function<void(Handle)> stop;
        /// Free a stopped or never-started transport.
        std::function<void(Handle)> destroy;
        /// Whether a new transport task fits now. Asked only after a transport was retired
        /// since the last start, so the first connect after boot never depends on it.
        std::function<bool()> room_for_task;
        /// The task did not fit: the owner's fallback.
        std::function<void()> on_no_room;
        /// A transport could not be created or started: the owner retries later.
        std::function<void()> on_start_failed;
    };

    explicit TransportLifecycle(Ops ops) : ops_(std::move(ops)) {}

    /// Replace whatever runs with a transport for @p url.
    void connect(const std::string& url) {
        {
            std::lock_guard<std::mutex> lock(url_mutex_);
            last_url_ = url;
        }
        const uint64_t request = generation_.fetch_add(1) + 1;
        ops_.post("connect", [this, url, request] {
            retire_current();
            if (generation_.load() != request) {
                return; // a later connect or disconnect owns the transport now
            }
            if (retired_since_start_ && ops_.room_for_task && !ops_.room_for_task()) {
                if (ops_.on_no_room) {
                    ops_.on_no_room();
                }
                return;
            }
            install(ops_.create ? ops_.create(url) : Handle{}, request);
        });
    }

    /// Stop and free the running transport, and cancel any connect not yet run.
    void disconnect() {
        generation_.fetch_add(1);
        ops_.post("disconnect", [this] { retire_current(); });
    }

    /// Restart the running transport in place; with none running because its start failed,
    /// connect to the last requested URL again, unless a request has come since the failure.
    void reconnect() {
        if (!current_.load()) {
            if (failed_generation_.load() != generation_.load()) {
                return; // a disconnect or newer connect owns the transport now
            }
            std::string url;
            {
                std::lock_guard<std::mutex> lock(url_mutex_);
                url = last_url_;
            }
            if (!url.empty()) {
                connect(url);
            }
            return;
        }
        const uint64_t request = generation_.fetch_add(1) + 1;
        ops_.post("reconnect", [this, request] {
            Handle h = current_.load();
            if (!h || generation_.load() != request) {
                return;
            }
            stop_flagged(h);
            connected_.store(false);
            installed_generation_.store(request);
            if (!ops_.begin(h)) {
                fail_start();
            }
        });
    }

    /// Whether an event from @p h belongs to the connection the owner last asked for: a
    /// transport stays running until its job stops it, but what it delivers after a newer
    /// request is stale.
    [[nodiscard]] bool accepts(Handle h) const {
        return h && h == current_.load() && installed_generation_.load() == generation_.load();
    }

    [[nodiscard]] Handle current() const {
        return current_.load();
    }

    /// The running transport's connection came up.
    void mark_connected(Handle h) {
        if (h == current_.load()) {
            connected_.store(true);
        }
    }

    /// Whether the running transport ever connected; a stop of one that never did can be
    /// waiting out a connect attempt.
    [[nodiscard]] bool current_connected() const {
        return connected_.load();
    }

    /// The job running now (a retire or a reconnect) is stopping a transport that never
    /// connected, which can mean waiting out its connect attempt rather than a stuck task.
    [[nodiscard]] bool retiring_unconnected() const {
        return retiring_unconnected_.load();
    }

  private:
    void retire_current() {
        Handle old = current_.exchange(Handle{});
        if (!old) {
            return;
        }
        stop_flagged(old);
        ops_.destroy(old);
        connected_.store(false);
        retired_since_start_ = true;
    }

    void stop_flagged(Handle h) {
        retiring_unconnected_.store(!connected_.load());
        ops_.stop(h);
        retiring_unconnected_.store(false);
    }

    void install(Handle h, uint64_t request) {
        if (!h) {
            fail_start();
            return;
        }
        // Current before it starts, so the new task's first events are not dropped.
        installed_generation_.store(request);
        current_.store(h);
        if (!ops_.begin(h)) {
            current_.store(Handle{});
            ops_.destroy(h);
            fail_start();
            return;
        }
        retired_since_start_ = false;
    }

    void fail_start() {
        failed_generation_.store(generation_.load());
        if (ops_.on_start_failed) {
            ops_.on_start_failed();
        }
    }

    Ops ops_;
    std::atomic<Handle> current_{Handle{}};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> installed_generation_{0};
    std::atomic<bool> connected_{false};
    /// The request generation a start failed in; a retry is only for that request.
    std::atomic<uint64_t> failed_generation_{UINT64_MAX};
    std::atomic<bool> retiring_unconnected_{false};
    std::mutex url_mutex_;
    std::string last_url_;
    bool retired_since_start_ = false; ///< worker-only
};

} // namespace helix::net
