// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "system/crash_handler.h"

#include <spdlog/sinks/base_sink.h>

#include <mutex>

namespace helix {

/// spdlog sink that retains the most recent ERROR/critical log lines in a
/// fixed static ring, registered with the crash handler so the signal handler
/// can surface them as `recent_error:` lines. This is the last-ditch reason
/// for an abort that left no glibc `__abort_msg` and skipped the std::terminate
/// handler — e.g. a bare `abort()`/`raise(SIGABRT)` from a dependency (#987).
///
/// The producer side (sink_it_) runs under base_sink's mutex at log time and
/// fully populates + NUL-terminates a slot before advancing the write index.
/// The crash handler reads the ring WITHOUT locking (signal context); it only
/// reads slots behind the write index, so a torn read is bounded to a slot
/// being overwritten on ring-wrap during a crash — vanishingly unlikely once
/// logging has stopped, and at worst yields one garbled diagnostic line.
class CrashErrorLogSink : public spdlog::sinks::base_sink<std::mutex> {
  public:
    /// Process-lifetime singleton. Its ring storage must outlive any crash and
    /// survive logger swaps (init_early → init), so the pointers registered
    /// with the crash handler never dangle. Construction registers the ring.
    static CrashErrorLogSink& instance();

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

  private:
    CrashErrorLogSink();

    static constexpr unsigned int CAP = crash_handler::ERROR_LOG_RING_CAPACITY;
    static constexpr unsigned int LEN = crash_handler::ERROR_LOG_ENTRY_LEN;

    char ring_[CAP][LEN] = {};
    // Written only under base_sink's mutex; read racily (by design) by the
    // signal handler. Not atomic — torn reads are tolerated, see class doc.
    unsigned int next_ = 0;
};

} // namespace helix
