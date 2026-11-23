// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "logging_init.h"

#include <spdlog/details/circular_q.h>
#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/base_sink.h>

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helix {
namespace logging {

/**
 * @brief In-memory ring sink that stamps CLOCK_MONOTONIC at log time
 *
 * Replaces spdlog's ringbuffer_sink for the debug-bundle log_tail. The upstream
 * sink stores raw messages and formats them in last_formatted(), which means
 * the `%*` flag reads the clock at DUMP time and every line in a 2000-line dump
 * receives the same monotonic value. Worse, the step detector then compares a
 * real wall-clock delta against a monotonic delta of ~0 and fires on every idle
 * gap over a second: bundle TDQCCQB3 carried 97 CLOCK_STEP annotations summing
 * to 8978 s, which is the whole session.
 *
 * Here sink_it_() records the offset alongside the message and last_formatted()
 * replays it, so the column means what it says. Formatting stays at dump time
 * (the log path keeps its single preallocated block and does no formatting),
 * which is what the constrained MIPS/ARM targets need — the ring runs at debug
 * level while the persistent sinks sit at warning.
 *
 * A run of identical lines (same logger, level and text) occupies one entry: a
 * poller logging the same line every few seconds would otherwise push the rest
 * of the session out of a small device's ring within hours. The dump prints the
 * first line of the run, then one summary line stamped with the last repeat.
 */
class MonotonicRingSink final : public spdlog::sinks::base_sink<std::mutex> {
  public:
    /// Clock source, injectable so tests need no sleeps. Seconds since start.
    using ClockFn = std::function<double()>;

    explicit MonotonicRingSink(size_t n_items, ClockFn clock = &monotonic_seconds)
        : q_{n_items}, clock_{std::move(clock)} {}

    /**
     * @brief Oldest-first formatted lines, each carrying its own log-time offset
     *
     * @param lim Maximum entries to return, newest-biased; 0 means all. A
     *            collapsed run is one entry and prints two lines, so a ring
     *            dumped at its capacity ships everything it holds.
     *
     * Deterministic: repeated calls on unchanged content return identical
     * output, because the replay guard resets the formatter's step memory on
     * entry rather than carrying it across dumps.
     */
    std::vector<std::string> last_formatted(size_t lim = 0) {
        std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
        const size_t available = q_.size();
        const size_t count = lim > 0 ? (lim < available ? lim : available) : available;

        ReplayGuard guard;
        std::vector<std::string> out;
        out.reserve(count);
        for (size_t i = available - count; i < available; ++i) {
            const Entry& e = q_.at(i);
            guard.set_value(e.mono);
            append_formatted(e.msg, out);
            if (e.repeats > 0) {
                const std::string summary = fmt::format(
                    "[repeated {} more time{}] {}", e.repeats, e.repeats == 1 ? "" : "s",
                    std::string_view(e.msg.payload.data(), e.msg.payload.size()));
                spdlog::details::log_msg last{e.last_time, e.msg.source, e.msg.logger_name,
                                              e.msg.level, summary};
                last.thread_id = e.msg.thread_id;
                guard.set_value(e.last_mono);
                append_formatted(last, out);
            }
        }
        return out;
    }

    /// Drop every held entry.
    void clear() {
        std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
        while (!q_.empty()) {
            q_.pop_front();
        }
    }

    /// Entry count currently held.
    size_t size() {
        std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
        return q_.size();
    }

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // Stamp here, not in last_formatted(). This is the entire point of the
        // class; see the header comment.
        const double mono = clock_ ? clock_() : 0.0;
        if (!q_.empty()) {
            const Entry& newest = q_.at(q_.size() - 1);
            if (repeats(newest.msg, msg)) {
                ++newest.repeats;
                newest.last_time = msg.time;
                newest.last_mono = mono;
                return;
            }
        }
        q_.push_back(Entry{spdlog::details::log_msg_buffer{msg}, mono});
    }

    void flush_() override {}

  private:
    // The repeat fields are mutable because circular_q only hands out const
    // references, and a repeat updates the newest entry in place.
    struct Entry {
        spdlog::details::log_msg_buffer msg;
        double mono = 0.0;
        mutable size_t repeats = 0;
        mutable spdlog::log_clock::time_point last_time{};
        mutable double last_mono = 0.0;
    };

    static bool repeats(const spdlog::details::log_msg& held,
                        const spdlog::details::log_msg& incoming) {
        return held.level == incoming.level && held.logger_name == incoming.logger_name &&
               held.payload == incoming.payload;
    }

    void append_formatted(const spdlog::details::log_msg& msg, std::vector<std::string>& out) {
        spdlog::memory_buf_t formatted;
        base_sink<std::mutex>::formatter_->format(msg, formatted);
        out.emplace_back(formatted.data(), formatted.size());
    }

    /// Scoped activation of the thread-local replay state, exception-safe so a
    /// throwing formatter cannot leave live sinks replaying a stale offset.
    class ReplayGuard {
      public:
        ReplayGuard() {
            auto& r = detail::monotonic_replay();
            r.active = true;
            r.reset_sequence = true;
        }
        void set_value(double v) const {
            detail::monotonic_replay().value = v;
        }
        ~ReplayGuard() {
            auto& r = detail::monotonic_replay();
            r.active = false;
            r.reset_sequence = false;
        }
        ReplayGuard(const ReplayGuard&) = delete;
        ReplayGuard& operator=(const ReplayGuard&) = delete;
    };

    spdlog::details::circular_q<Entry> q_;
    ClockFn clock_;
};

using MonotonicRingSinkMt = MonotonicRingSink;

} // namespace logging
} // namespace helix
