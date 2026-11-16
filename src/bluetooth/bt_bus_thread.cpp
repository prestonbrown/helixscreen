// SPDX-License-Identifier: GPL-3.0-or-later

#include "bt_bus_thread.h"

#include "helix_thread.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <system_error>
#include <unistd.h>

namespace helix::bluetooth {

BusThread::BusThread(sd_bus* bus) : bus_(bus) {
    if (pipe2(wakeup_fds_, O_CLOEXEC | O_NONBLOCK) != 0) {
        fprintf(stderr, "[bt] BusThread pipe2 failed: %s\n", strerror(errno));
        wakeup_fds_[0] = wakeup_fds_[1] = -1;
    }
}

BusThread::~BusThread() {
    stop();
    if (wakeup_fds_[0] >= 0)
        close(wakeup_fds_[0]);
    if (wakeup_fds_[1] >= 0)
        close(wakeup_fds_[1]);
}

void BusThread::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;
    stopping_.store(false);
    // Wrap — EAGAIN under thread exhaustion throws std::system_error ([L083]).
    try {
        thread_ = helix::make_thread([this] {
            // Published from inside the worker before any work runs: a write
            // from the parent after construction would race the worker's
            // first on_thread() check.
            thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
            loop();
        });
    } catch (const std::system_error& e) {
        fprintf(stderr, "[bt] BusThread failed to spawn worker thread: %s\n", e.what());
        running_.store(false);
    }
}

void BusThread::stop() {
    if (!running_.exchange(false))
        return;
    stopping_.store(true);
    notify();
    if (thread_.joinable())
        thread_.join();

    // Drain remaining queue AFTER join: the worker has exited, so no new items
    // can enter the queue from the worker side, and submit() callers either
    // (a) saw running_ == false and got an immediately-broken-promise future,
    // or (b) pushed before our running_.exchange and we catch them here.
    // (May also be empty already if loop() already drained on an error exit.)
    std::lock_guard<std::mutex> lk(mu_);
    drain_and_break_promises_locked("BusThread stopped");
}

void BusThread::drain_and_break_promises_locked(const char* reason) {
    while (!queue_.empty()) {
        // Move the promise out before popping so set_exception runs on a local
        // that pop_front cannot destroy.
        auto p = std::move(queue_.front().second);
        queue_.pop_front();
        p.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
    }
}

std::future<void> BusThread::submit(BusWork work) {
    std::promise<void> p;
    auto fut = p.get_future();

    // Self-submit guard: if we're already on the bus thread, run inline. This
    // preserves run_sync()'s recursive-safe semantics and prevents any caller
    // that does submit(...).get() / .wait() from a bus-thread callback (e.g. an
    // sd_bus match handler or timeout) from deadlocking on its own future. The
    // queue path would enqueue, return up to sd_bus_process, and only execute
    // on the *next* loop iteration — by which time .get() is already blocked.
    // sd_bus_* calls are legal from inside dispatch, so inline execution is
    // safe.
    if (on_thread()) {
        try {
            work(bus_);
            p.set_value();
        } catch (...) {
            p.set_exception(std::current_exception());
        }
        return fut;
    }

    if (stopping_.load() || !running_.load()) {
        p.set_exception(std::make_exception_ptr(std::runtime_error("BusThread not running")));
        return fut;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        // Re-check under the lock: a concurrent stop() can drain the queue
        // between our unlocked pre-check and this emplace. Without the re-check
        // the work would sit in the queue as an orphan whose promise never
        // resolves until ~BusThread.
        if (stopping_.load() || !running_.load()) {
            p.set_exception(std::make_exception_ptr(std::runtime_error("BusThread not running")));
            return fut;
        }
        queue_.emplace_back(std::move(work), std::move(p));
    }
    notify();
    return fut;
}

void BusThread::run_sync(BusWork work) {
    if (on_thread()) {
        work(bus_);
        return;
    }
    submit(std::move(work)).get();
}

void BusThread::notify() {
    if (wakeup_fds_[1] >= 0) {
        uint8_t b = 1;
        ssize_t n = write(wakeup_fds_[1], &b, 1);
        (void)n; // EAGAIN is fine — pipe already has a pending byte
    }
}

bool BusThread::on_thread() const noexcept {
    return std::this_thread::get_id() == thread_id_.load(std::memory_order_acquire);
}

void BusThread::loop() {
    while (running_.load()) {
        // 1. Drain queued work items.
        for (;;) {
            if (stopping_.load())
                break; // Leave remaining items for stop()'s post-join drain to break their
                       // promises.
            std::pair<BusWork, std::promise<void>> item;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (queue_.empty())
                    break;
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                item.first(bus_);
                item.second.set_value();
            } catch (...) {
                item.second.set_exception(std::current_exception());
            }
        }

        // 2. Process pending bus traffic (skipped if no bus — null bus is legal for tests/idle).
        if (bus_) {
            int r = sd_bus_process(bus_, nullptr);
            if (r < 0) {
                fprintf(stderr, "[bt] BusThread sd_bus_process error: %s\n", strerror(-r));
                running_.store(false);
                stopping_.store(true);
                break;
            }
            if (r > 0)
                continue; // more bus work available; skip the wait
        }

        // 3. Wait for bus activity OR a wakeup-pipe byte, up to 500ms.
        struct pollfd pfds[2];
        int nfds = 0;
        if (bus_) {
            int bus_fd = sd_bus_get_fd(bus_);
            if (bus_fd >= 0) {
                pfds[nfds].fd = bus_fd;
                pfds[nfds].events = sd_bus_get_events(bus_);
                nfds++;
            }
        }
        if (wakeup_fds_[0] >= 0) {
            pfds[nfds].fd = wakeup_fds_[0];
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int timeout_ms = 500;
        if (bus_) {
            uint64_t timeout_us = 0;
            sd_bus_get_timeout(bus_, &timeout_us);
            if (timeout_us != UINT64_MAX) {
                // sd-bus timeout is absolute µs since epoch; clamp to 500ms max.
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now_us = ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
                int64_t delta_ms = (int64_t(timeout_us) - int64_t(now_us)) / 1000;
                if (delta_ms < 0)
                    delta_ms = 0;
                if (delta_ms < timeout_ms)
                    timeout_ms = int(delta_ms);
            }
        }

        poll(pfds, nfds, timeout_ms);

        // Drain the wakeup pipe.
        if (wakeup_fds_[0] >= 0) {
            uint8_t buf[16];
            while (read(wakeup_fds_[0], buf, sizeof(buf)) > 0) {
            }
        }
    }

    // Post-loop drain: break promises for any work items still queued. This
    // matters most for the sd_bus_process error path — without it, those items
    // would sit in the queue until ~BusThread (or stop()) ran, and any caller
    // already blocked on .get() would hang.
    //
    // For the normal stop() path this is usually a no-op: stop() flipped
    // running_ to false (which submit() re-checks under the lock), so no new
    // items can be queued, and stop()'s post-join drain would handle anything
    // left. Doing it here too just frees promises a few microseconds earlier
    // and shrinks the window where a caller hangs on .get() before stop()
    // completes its join.
    {
        std::lock_guard<std::mutex> lk(mu_);
        drain_and_break_promises_locked("BusThread exited");
    }
}

} // namespace helix::bluetooth
