// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fault_surface_correlation.h"

#include <chrono>
#include <deque>
#include <mutex>

namespace helix::fault_surface_correlation {

namespace {

using Clock = std::chrono::steady_clock;

/// See the header for why 3 s: it has to span `!!` broadcast -> AFC
/// pause_print() -> Moonraker status delta -> AmsAction::ERROR edge, which is
/// a full round trip on the slowest supported hardware.
constexpr auto CAUSAL_WINDOW = std::chrono::milliseconds(3000);

/// Runaway guard. Nothing prunes on a write-only workload — a printer spraying
/// distinct error strings would otherwise grow this without bound between
/// queries. Well above the ~5 entries one multi-line fault produces.
constexpr size_t MAX_ENTRIES = 64;

struct Entry {
    std::string detail;
    Clock::time_point recorded_at;
};

std::mutex& mu() {
    static std::mutex m;
    return m;
}

std::deque<Entry>& entries() {
    static std::deque<Entry> q;
    return q;
}

// Caller must hold mu().
void prune_locked() {
    const auto now = Clock::now();
    while (!entries().empty() && (now - entries().front().recorded_at) > CAUSAL_WINDOW) {
        entries().pop_front();
    }
}

} // namespace

void record_surfaced(const std::string& detail) {
    if (detail.empty())
        return;
    std::lock_guard<std::mutex> lock(mu());
    prune_locked();
    while (entries().size() >= MAX_ENTRIES) {
        entries().pop_front();
    }
    entries().push_back(Entry{detail, Clock::now()});
}

bool was_recently_surfaced(const std::string& detail) {
    if (detail.empty())
        return false;
    std::lock_guard<std::mutex> lock(mu());
    prune_locked();
    for (const auto& e : entries()) {
        if (e.detail == detail)
            return true;
    }
    return false;
}

void clear_for_test() {
    std::lock_guard<std::mutex> lock(mu());
    entries().clear();
}

} // namespace helix::fault_surface_correlation
