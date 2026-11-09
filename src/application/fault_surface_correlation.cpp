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
constexpr auto kCausalWindow = std::chrono::milliseconds(3000);

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
    while (!entries().empty() && (now - entries().front().recorded_at) > kCausalWindow) {
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

bool contains_as_segment(const std::string& text, const std::string& needle) {
    if (needle.empty() || needle.size() > text.size())
        return false;
    for (size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + 1)) {
        const bool starts_segment =
            pos == 0 || text[pos - 1] == '\n' || (pos >= 2 && text.compare(pos - 2, 2, ": ") == 0);
        const size_t end = pos + needle.size();
        const bool ends_segment = end == text.size() || text[end] == '\n';
        if (starts_segment && ends_segment)
            return true;
    }
    return false;
}

bool was_recently_surfaced_within(const std::string& detail) {
    if (detail.empty())
        return false;
    std::lock_guard<std::mutex> lock(mu());
    prune_locked();
    for (const auto& e : entries()) {
        if (contains_as_segment(detail, e.detail))
            return true;
    }
    return false;
}

void clear_for_test() {
    std::lock_guard<std::mutex> lock(mu());
    entries().clear();
}

} // namespace helix::fault_surface_correlation
