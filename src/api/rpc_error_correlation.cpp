// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rpc_error_correlation.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <utility>

namespace helix::rpc_error_correlation {

namespace {

using Clock = std::chrono::steady_clock;

// Causal window: how long after an RPC error_cb fires should we suppress a
// matching `!!` broadcast. 1.5s is loose enough for WebSocket jitter on the
// slowest devices (Pi 3 / MIPS32) yet tight enough not to mask a genuinely
// later error. The two channels (gcode.script RPC error response and the
// gcode-response `!!` line) come from the same Klipper gcode dispatcher
// invocation — they arrive within milliseconds typically.
constexpr auto CAUSAL_WINDOW = std::chrono::milliseconds(1500);

struct Entry {
    std::string message;
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

void record_caller_handled(const std::string& message) {
    if (message.empty())
        return;
    std::lock_guard<std::mutex> lock(mu());
    prune_locked();
    entries().push_back(Entry{message, Clock::now()});
}

bool was_recently_handled(const std::string& message) {
    if (message.empty())
        return false;
    std::lock_guard<std::mutex> lock(mu());
    prune_locked();
    for (const auto& e : entries()) {
        if (e.message == message)
            return true;
    }
    return false;
}

void clear_for_test() {
    std::lock_guard<std::mutex> lock(mu());
    entries().clear();
}

} // namespace helix::rpc_error_correlation
