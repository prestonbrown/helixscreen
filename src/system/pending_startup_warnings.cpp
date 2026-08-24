// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/pending_startup_warnings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix {

// Upper bound on the queue. A runaway pre-init loop (e.g. repeated NOTIFY_*
// on AD5M's 107 MB budget) would otherwise OOM before ToastManager::init()
// drains this. Drops silently past the cap; one log line is emitted.
static constexpr size_t MAX_PENDING = 64;

PendingStartupWarnings& PendingStartupWarnings::instance() {
    static PendingStartupWarnings s_instance;
    return s_instance;
}

void PendingStartupWarnings::enqueue(Severity severity, std::string message) {
    std::lock_guard<std::mutex> lock(mu_);
    if (pending_.size() >= MAX_PENDING) {
        // Log once per overflow boundary — not once per drop.
        if (pending_.size() == MAX_PENDING) {
            spdlog::warn("[PendingStartupWarnings] queue full ({} entries) — "
                         "dropping subsequent early notifications",
                         MAX_PENDING);
        }
        return;
    }
    // Deduplicate identical notifications. The same condition can be reported by
    // more than one layer during startup — e.g. a resolution-fallback warning is
    // enqueued by both the DRM backend and the fbdev backend when DisplayManager
    // tries DRM first and then falls back to fbdev for software rotation. The
    // user should see one toast, not one per backend that hit the same condition.
    if (std::any_of(pending_.begin(), pending_.end(), [&](const auto& entry) {
            return entry.first == severity && entry.second == message;
        })) {
        return;
    }
    pending_.emplace_back(severity, std::move(message));
}

void PendingStartupWarnings::drain(
    const std::function<void(Severity, const std::string&)>& on_warning) {
    // Swap under the lock, then invoke callbacks without holding it. This
    // avoids any risk of the callback re-entering enqueue() (e.g. from a
    // toast implementation that logs something), and keeps the lock hold
    // time minimal.
    std::vector<std::pair<Severity, std::string>> local;
    {
        std::lock_guard<std::mutex> lock(mu_);
        local.swap(pending_);
    }
    for (const auto& entry : local) {
        on_warning(entry.first, entry.second);
    }
}

void PendingStartupWarnings::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.clear();
}

} // namespace helix
