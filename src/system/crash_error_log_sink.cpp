// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/crash_error_log_sink.h"

namespace helix {

CrashErrorLogSink& CrashErrorLogSink::instance() {
    static CrashErrorLogSink s;
    return s;
}

CrashErrorLogSink::CrashErrorLogSink() {
    // Capture only ERROR and above — the high-signal lines worth keeping.
    set_level(spdlog::level::err);
    // Register the process-lifetime ring exactly once. The crash handler just
    // stores these pointers; install() does not clear them, so order vs.
    // install() doesn't matter.
    crash_handler::register_error_log_ring(&ring_[0][0], CAP, &next_);
}

void CrashErrorLogSink::sink_it_(const spdlog::details::log_msg& msg) {
    // base_sink already holds the mutex here. Capture the raw payload (the
    // formatted user message, no timestamp/level prefix) into the next slot,
    // stripping newlines so each entry stays a single `key:value` crash line.
    char* slot = ring_[next_ % CAP];
    const char* src = msg.payload.data();
    const size_t srclen = msg.payload.size();
    size_t n = 0;
    for (; n + 1 < LEN && n < srclen; ++n) {
        char c = src[n];
        slot[n] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    slot[n] = '\0';
    // Advance only after the slot is fully written + terminated, so the crash
    // handler (which reads slots strictly behind next_) never sees a partial.
    ++next_;
}

} // namespace helix
