// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace helix {

/**
 * @brief What the caller should log about a run of rejected samples.
 *
 * A "run" is an unbroken stretch of rejected samples for one key. The policy
 * reports the two ends of that run plus a slow heartbeat in the middle; every
 * other sample in the run reports Nothing.
 */
enum class InvalidSampleLog : uint8_t {
    Nothing,      ///< Inside a run, before the next heartbeat is due — say nothing.
    Entered,      ///< First rejected sample of a run.
    StillInvalid, ///< Heartbeat: the run is still going, here is how big it has grown.
    Recovered,    ///< First accepted sample after a run — the run is over.
};

/**
 * @brief One decision from InvalidSampleTracker::record().
 *
 * The counters are filled for every loggable kind, so a single format string at
 * the call site can say which key, what the offending value was, how many
 * samples have been dropped, and over how long.
 */
struct InvalidSampleReport {
    InvalidSampleLog what = InvalidSampleLog::Nothing;
    int64_t dropped = 0;     ///< Samples rejected so far in this run (>= 1 when loggable).
    int64_t duration_ms = 0; ///< How long the run has lasted at this point.
    int first_temp_deci = 0; ///< The value that opened the run.
    int last_temp_deci = 0;  ///< The most recent rejected value in the run.

    [[nodiscard]] bool loggable() const {
        return what != InvalidSampleLog::Nothing;
    }
};

/**
 * @brief Collapses a permanent stream of rejected samples into a few log lines.
 *
 * The problem this solves (prestonbrown/helixscreen#1348): a rejection predicate
 * that is *permanent* rather than transient — an open or disconnected thermistor
 * reads out of range forever — turns a per-sample log line into an unbounded
 * flood. One field bundle was 95.5% a single dropped-sample message, which
 * evicted every other line from the in-memory crash ring and cut that bundle's
 * coverage to 16 minutes of a 7.7 hour uptime.
 *
 * ## Why transitions plus a backing-off heartbeat, and not either alone
 *
 * Pure transition logging (one line on entering the bad state, one on leaving)
 * is the right *shape*, but on its own it loses the case that actually caused
 * the bug: a thermistor that is open for the whole life of the process never
 * produces the closing line, and its opening line is evicted from the ring by
 * ordinary traffic long before anyone collects a bundle. The maintainer is then
 * back to seeing nothing at all.
 *
 * A flat rate limit (one line per key per minute) keeps a recent line but is
 * still O(uptime): six heaters over 7.7 hours is ~2,800 lines, ~14% of the ring.
 *
 * So: log both transitions, and between them emit a heartbeat on a doubling
 * interval capped at MAX_REPEAT_MS. That is O(log uptime) — ~20 lines per key
 * for a 7.7 hour run instead of ~3,200 — while guaranteeing the ring always
 * holds a recent line naming the key, the value, the dropped count, and the
 * elapsed time. Bad state is never silent, and never expensive.
 *
 * ## Usage
 *
 * ```cpp
 * const bool valid = in_range(temp_deci);
 * const auto report = tracker.record(heater_name, valid, temp_deci, now_ms);
 * if (report.loggable()) { spdlog::debug(...); }
 * if (!valid) return false;   // rejection behaviour is unchanged
 * ```
 *
 * Pure and header-only: no clock of its own (the caller supplies @p now_ms), no
 * LVGL, no logging. Not thread-safe — callers hold their own lock, as
 * TemperatureHistoryManager does.
 */
class InvalidSampleTracker {
  public:
    /// First heartbeat comes one minute into a run.
    static constexpr int64_t FIRST_REPEAT_MS = 60'000;
    /// Heartbeat interval doubles per emission, but never past half an hour.
    static constexpr int64_t MAX_REPEAT_MS = 1'800'000;

    /**
     * @brief Feed one sample's verdict and learn whether to log.
     *
     * @param key         Identifies the run — the heater/sensor name.
     * @param valid       Whether this sample passed the caller's range check.
     * @param temp_deci   The sample value, recorded so the log line can name it.
     * @param now_ms      Caller's wall-clock timestamp in milliseconds.
     * @return What to log, if anything.
     */
    InvalidSampleReport record(const std::string& key, bool valid, int temp_deci, int64_t now_ms) {
        InvalidSampleReport report;

        if (valid) {
            // Fast path: a healthy key never allocates a map entry. Only a key
            // with a run in flight has one, and closing the run erases it.
            auto it = runs_.find(key);
            if (it == runs_.end()) {
                return report;
            }
            const Run& run = it->second;
            report.what = InvalidSampleLog::Recovered;
            report.dropped = run.dropped;
            report.duration_ms = elapsed(run.start_ms, now_ms);
            report.first_temp_deci = run.first_temp_deci;
            report.last_temp_deci = run.last_temp_deci;
            runs_.erase(it);
            ++log_events_;
            return report;
        }

        auto [it, inserted] = runs_.try_emplace(key);
        Run& run = it->second;
        run.last_temp_deci = temp_deci;

        if (inserted) {
            run.start_ms = now_ms;
            run.dropped = 1;
            run.first_temp_deci = temp_deci;
            run.interval_ms = FIRST_REPEAT_MS;
            run.next_log_ms = now_ms + FIRST_REPEAT_MS;

            report.what = InvalidSampleLog::Entered;
            report.dropped = 1;
            report.duration_ms = 0;
            report.first_temp_deci = temp_deci;
            report.last_temp_deci = temp_deci;
            ++log_events_;
            return report;
        }

        ++run.dropped;
        if (now_ms < run.next_log_ms) {
            return report; // Nothing — still inside the current backoff window.
        }

        // Heartbeat due. Widen the window first so the next one is further out.
        run.interval_ms = std::min(run.interval_ms * 2, MAX_REPEAT_MS);
        run.next_log_ms = now_ms + run.interval_ms;

        report.what = InvalidSampleLog::StillInvalid;
        report.dropped = run.dropped;
        report.duration_ms = elapsed(run.start_ms, now_ms);
        report.first_temp_deci = run.first_temp_deci;
        report.last_temp_deci = temp_deci;
        ++log_events_;
        return report;
    }

    /**
     * @brief Samples dropped so far in @p key's current run, 0 if it has none.
     */
    [[nodiscard]] int64_t dropped_in_run(const std::string& key) const {
        auto it = runs_.find(key);
        return (it == runs_.end()) ? 0 : it->second.dropped;
    }

    /// Whether @p key currently has a run in flight.
    [[nodiscard]] bool in_run(const std::string& key) const {
        return runs_.find(key) != runs_.end();
    }

    /**
     * @brief Loggable reports returned since construction, across all keys.
     *
     * This is the bound the caller's log volume is held to — one line per
     * report, however many samples were fed in.
     */
    [[nodiscard]] int64_t log_events() const {
        return log_events_;
    }

    /// Forget every run. Does not reset log_events().
    void clear() {
        runs_.clear();
    }

  private:
    struct Run {
        int64_t start_ms = 0;    ///< When the run opened.
        int64_t next_log_ms = 0; ///< Earliest timestamp the next heartbeat may fire.
        int64_t interval_ms = 0; ///< Current backoff window.
        int64_t dropped = 0;     ///< Samples rejected in this run.
        int first_temp_deci = 0; ///< Value that opened the run.
        int last_temp_deci = 0;  ///< Most recent rejected value.
    };

    /// Clamped so a wall-clock step backwards reports 0 rather than a negative age.
    static int64_t elapsed(int64_t start_ms, int64_t now_ms) {
        return (now_ms > start_ms) ? (now_ms - start_ms) : 0;
    }

    std::unordered_map<std::string, Run> runs_;
    int64_t log_events_ = 0;
};

} // namespace helix
