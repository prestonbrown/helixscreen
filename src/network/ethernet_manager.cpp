// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ethernet_manager.h"

#include "core/http_executor.h"

#include <spdlog/spdlog.h>

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
