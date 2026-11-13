// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// WiFiManager's status reads: one backend read at a time on the control lane,
// answering every caller that asked before it began, and the watch for an
// address while a joined link waits on DHCP.

#include "core/http_executor.h"
#include "spdlog/spdlog.h"
#include "wifi_manager.h"

#include <chrono>
#include <mutex>
#include <vector>

namespace helix {

void WiFiManager::get_status_async(
    helix::LifetimeToken token,
    helix::SharedCallback<void(const WifiBackend::ConnectionStatus&)> on_done) {
    if (!on_done) {
        return;
    }
    if (!helix::http::HttpExecutor::fast().running()) {
        if (!lane_pooled_) {
            const WifiBackend::ConnectionStatus status =
                backend_ ? backend_->get_status() : WifiBackend::ConnectionStatus{};
            watch_address_if_awaiting(status);
            on_done(status);
            return;
        }
        // The pool has stopped (app exit): no read may run on this thread.
        WifiBackend::ConnectionStatus cached;
        {
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            cached = cached_status_;
        }
        cached.unresponsive = true;
        on_done(cached);
        return;
    }
    const bool stalled = control_stalled();
    {
        std::lock_guard<std::mutex> lock(status_waiters_mutex_);
        if (status_read_inflight_ && !stalled) {
            status_waiters_.push_back({std::move(token), std::move(on_done)});
            return;
        }
        if (stalled || !breaker_allows_call()) {
            WifiBackend::ConnectionStatus cached = cached_status_;
            cached.unresponsive = true;
            token.defer("WiFiManager::get_status_async(unresponsive)",
                        [cb = std::move(on_done), cached]() { cb(cached); });
            return;
        }
        status_waiters_.push_back({std::move(token), std::move(on_done)});
        status_read_inflight_ = true;
    }
    // A read dropped unrun clears the in-flight mark, so the next caller
    // starts a read that also answers everyone still waiting.
    submit_control(
        "get_status", ControlKind::Health, [this]() { run_status_reads(); },
        [this]() {
            breaker_abandon_probe();
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            status_read_inflight_ = false;
        });
}

void WiFiManager::run_status_reads() {
    // `this` is valid for the whole body: see radio_op_mutex_.
    // get_status_async() admitted the first read; each later one asks again.
    bool first = true;
    for (;;) {
        std::vector<StatusWaiter> batch;
        {
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            if (status_waiters_.empty()) {
                status_read_inflight_ = false;
                return;
            }
            batch.swap(status_waiters_);
        }
        const bool admitted = first || breaker_allows_call();
        first = false;
        WifiBackend::ConnectionStatus status;
        if (admitted) {
            // Every waiter in the batch asked before this read began.
            const auto started = std::chrono::steady_clock::now();
            status = backend_ ? backend_->get_status() : WifiBackend::ConnectionStatus{};
            const bool answered_in_time = record_health_call(started);
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            if (answered_in_time) {
                cached_status_ = status;
                watch_address_if_awaiting(status);
            } else {
                // A read that ran out the reply deadline parsed an empty reply,
                // which says "disconnected" about a link nobody could ask about.
                status = cached_status_;
                status.unresponsive = breaker_open();
            }
        } else {
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            status = cached_status_;
            status.unresponsive = true;
        }
        for (auto& w : batch) {
            w.token.defer("WiFiManager::get_status_async",
                          [cb = std::move(w.on_done), status]() { cb(status); });
        }
    }
}

void WiFiManager::watch_address_if_awaiting(const WifiBackend::ConnectionStatus& status) {
    if (!status.awaiting_ipv4) {
        return;
    }
    async_lifetime_.token().defer("WiFiManager::watch_address",
                                  [this]() { start_address_watch(); });
}

void WiFiManager::start_address_watch() {
    address_watch_elapsed_ms_ = 0;
    if (address_events_ || !backend_) {
        return;
    }
    address_events_ = backend_->open_address_events();
    if (!address_events_) {
        return;
    }
    spdlog::debug("[WiFiManager] Joined without an address yet; watching for one");
    after_ms(kAddressWatchPeriodMs, [this]() { address_watch_tick(); });
}

void WiFiManager::address_watch_tick() {
    if (!address_events_) {
        return;
    }
    if (address_events_->poll()) {
        spdlog::debug("[WiFiManager] Address event; refreshing status observers");
        notify_state_observers();
    }
    // A CONNECTED event leaves a cached status with no address and no flag,
    // so "still waiting" is a joined link without an address, not the flag.
    bool awaiting;
    {
        std::lock_guard<std::mutex> lock(status_waiters_mutex_);
        awaiting = cached_status_.connected && cached_status_.ip_address.empty();
    }
    address_watch_elapsed_ms_ += kAddressWatchPeriodMs;
    if (!awaiting || address_watch_elapsed_ms_ >= kAddressWatchMaxMs) {
        spdlog::debug("[WiFiManager] Address watch stopped ({})",
                      awaiting ? "timed out" : "address known");
        address_events_.reset();
        return;
    }
    after_ms(kAddressWatchPeriodMs, [this]() { address_watch_tick(); });
}

bool WiFiManager::cached_connected() {
    std::lock_guard<std::mutex> lock(status_waiters_mutex_);
    return cached_status_.connected;
}

} // namespace helix
