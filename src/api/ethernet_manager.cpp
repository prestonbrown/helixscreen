// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ethernet_manager.h"

#include "http_executor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

EthernetManager::EthernetManager() {
    spdlog::debug("[EthernetManager] Initializing Ethernet manager");

    // Create appropriate backend for this platform
    backend_ = EthernetBackend::create();

    if (!backend_) {
        spdlog::error("[EthernetManager] Failed to create backend");
        return;
    }

    spdlog::debug("[EthernetManager] Ethernet manager initialized");
}

EthernetManager::~EthernetManager() {
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[EthernetManager] Shutting down Ethernet manager\n");
    link_lifetime_.invalidate();
    link_timer_.reset();
    backend_.reset();
}

bool EthernetManager::has_interface() {
    if (!backend_) {
        spdlog::warn("[EthernetManager] Backend not initialized");
        return false;
    }

    return backend_->has_interface();
}

EthernetInfo EthernetManager::get_info() {
    if (!backend_) {
        spdlog::warn("[EthernetManager] Backend not initialized");
        EthernetInfo info;
        info.status = "Backend error";
        return info;
    }

    return backend_->get_info();
}

void EthernetManager::get_info_async(std::function<void(const EthernetInfo&)> callback) {
    if (!callback) {
        spdlog::warn("[EthernetManager] get_info_async called with null callback");
        return;
    }

    // Ensure the fast lane is running. start() is idempotent; this covers unit
    // tests and early-boot call sites that run before Application calls
    // HttpExecutor::start_all().
    helix::http::HttpExecutor::fast().start();

    if (!backend_) {
        // Backend missing — still fire the callback off-thread so the
        // async contract holds (no synchronous invocation on the caller).
        spdlog::warn("[EthernetManager] Backend not initialized (async)");
        helix::http::HttpExecutor::fast().submit([cb = std::move(callback)]() {
            EthernetInfo info;
            info.status = "Backend error";
            cb(info);
        });
        return;
    }

    // The caller is responsible for guarding the callback against owner
    // destruction (see header — typically via helix::LifetimeToken). The
    // callback fires on an HttpExecutor worker thread; UI callers wrap it
    // with tok.defer() to hop back to the main thread.
    //
    // Route through HttpExecutor::fast() (bounded 4-worker pool) instead of
    // spawning a raw std::thread per call. NetworkWidget invokes this on
    // every panel activation, and on thread-constrained platforms like AD5M
    // the per-call spawn pattern can fail with EAGAIN (see #724 and
    // feedback_no_bare_threads_arm.md). Ethernet info fetch is I/O-bound
    // (sysfs/ioctl reads) with the same lane semantics as other fast-lane
    // REST/status work.
    //
    // Capture the backend shared_ptr BY VALUE so it outlives this
    // EthernetManager if destruction races with the probe. Without this,
    // destroying EthernetManager (e.g. during overlay teardown) while the
    // worker is inside backend->get_info() frees the backend and causes a
    // use-after-free segfault on the worker thread.
    std::shared_ptr<EthernetBackend> backend = backend_;
    helix::http::HttpExecutor::fast().submit([backend, cb = std::move(callback)]() {
        spdlog::trace("[EthernetManager] Async probe starting on HTTP fast worker");
        EthernetInfo info = backend->get_info();
        spdlog::trace("[EthernetManager] Async probe done; invoking callback");
        cb(info);
    });
}

std::string EthernetManager::get_ip_address() {
    EthernetInfo info = get_info();

    if (info.connected) {
        return info.ip_address;
    }

    return "";
}

// ============================================================================
// Link Changes
// ============================================================================

namespace {

/// What the link observers care about: a change in any field is news.
std::string link_signature(const EthernetInfo& info) {
    return (info.connected ? "1|" : "0|") + info.interface + "|" + info.ip_address + "|" +
           info.status;
}

} // namespace

void EthernetManager::add_link_observer(helix::LifetimeToken token,
                                        helix::SharedCallback<void()> on_change) {
    if (!on_change || !backend_) {
        return;
    }
    link_observers_.push_back({std::move(token), std::move(on_change)});
    if (link_timer_) {
        return;
    }

    link_events_ = backend_->open_link_events();
    ticks_since_probe_ = 0;
    last_link_signature_.clear();
    spdlog::debug("[EthernetManager] Watching the link ({})",
                  link_events_ ? "backend events" : "periodic probe");
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<EthernetManager*>(lv_timer_get_user_data(t))->link_watch_tick();
        },
        kLinkWatchPeriodMs, this);
    link_timer_.reset(timer);
    // The fallback compares against what the first probe finds, so take it now
    // rather than one fallback period from now.
    if (!link_events_) {
        ticks_since_probe_ = kLinkProbeFallbackMs / kLinkWatchPeriodMs;
    }
}

void EthernetManager::link_watch_tick() {
    link_observers_.erase(
        std::remove_if(link_observers_.begin(), link_observers_.end(),
                       [](const LinkObserver& obs) { return obs.token.expired(); }),
        link_observers_.end());
    if (link_observers_.empty()) {
        stop_link_watch();
        return;
    }

    if (link_events_) {
        if (link_events_->poll()) {
            spdlog::debug("[EthernetManager] Link event");
            notify_link_observers();
        }
        return;
    }

    if (probe_in_flight_ || ++ticks_since_probe_ < kLinkProbeFallbackMs / kLinkWatchPeriodMs) {
        return;
    }
    ticks_since_probe_ = 0;
    probe_in_flight_ = true;
    auto tok = link_lifetime_.token();
    get_info_async([this, tok](const EthernetInfo& info) {
        std::string signature = link_signature(info);
        tok.defer("EthernetManager::link_probe", [this, signature = std::move(signature)]() {
            probe_in_flight_ = false;
            const bool changed = !last_link_signature_.empty() && signature != last_link_signature_;
            last_link_signature_ = signature;
            if (changed) {
                spdlog::debug("[EthernetManager] Link changed: {}", signature);
                notify_link_observers();
            }
        });
    });
}

void EthernetManager::stop_link_watch() {
    spdlog::debug("[EthernetManager] No link observers left; watch stopped");
    link_timer_.reset();
    link_events_.reset();
    link_lifetime_.invalidate();
    probe_in_flight_ = false;
}

void EthernetManager::notify_link_observers() {
    for (const auto& obs : link_observers_) {
        obs.token.defer("EthernetManager::link_observer", obs.callback);
    }
}
