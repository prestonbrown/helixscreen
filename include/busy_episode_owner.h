// SPDX-License-Identifier: GPL-3.0-or-later
// include/busy_episode_owner.h
#pragma once

#include "idle_timeout_busy.h"

#include <array>
#include <atomic>
#include <chrono>

namespace helix {

/**
 * Decides whether a busy episode (one continuous run of idle_timeout
 * "Printing") is one the app's own sends began.
 *
 * Any gcode flips idle_timeout to "Printing" while it runs, so a burst of the
 * app's own commands can hold the flag up continuously: a jog, or light taps
 * 0.2 s apart that each read "Printing" for ~0.8 s. A grace window after the
 * last ack cannot tell that apart from an external op for long, because a
 * sustained burst outlasts any window. The episode's start time can: the
 * episode is the app's when the app sent something in the kGraceWindow before
 * it. Only a send made before the episode was observed can have caused it:
 * episode_start is when the main thread saw idle_timeout go high, and a send
 * is stamped before it goes out.
 *
 * Acks are the second witness, for sends tracked with note_sent()/note_done().
 * Klipper runs one gcode at a time, so a send queued behind someone else's
 * long command does not ack until that command ends, while a jog into an idle
 * gcode queue acks in milliseconds:
 *   - a tracked send in flight with no ack for kGraceWindow is stuck behind
 *     another operation, and disowns the episode for good, as does an ack
 *     that arrived that late;
 *   - a tracked send made before the episode settled (within
 *     IdleTimeoutBusy::kSettle of its start) that acked promptly adopts it:
 *     a housekeeping burst can start an episode that the app's own fast
 *     jogs then keep up. While such a send is still in flight and not stuck,
 *     the episode reads as not owned and the decision waits for a later ask.
 * Adoption trusts that a prompt ack means nobody else holds Klipper's gcode
 * lock. An external operation that releases the lock between its commands
 * (a host streaming gcode line by line, a probe sequence sent command by
 * command) lets a jog ack promptly in its first SETTLE, and is adopted.
 * Untracked sends (fire-and-forget, or acked only when a long command ends)
 * have no ack to witness either way and only count from before the start.
 *
 * Asks come late (the first one ~SETTLE into the episode, after more sends
 * have landed), so the recent send times are kept rather than only the last.
 * Ownership is decided once per episode, on the first ask that can settle it,
 * and lapses
 * kMaxSelfBusy after the latest activity: an ack, an untracked send or
 * note_still_active(). A tracked send alone does not extend it; one that
 * never acks is stuck, not activity. More than kHistory sends between an
 * episode's start and its first ask reads as not owned.
 *
 * Thread-safe: sends are stamped on the main thread and acks arrive on the
 * websocket thread. Every field is its own atomic, and the in-flight count
 * only steps down by compare-and-swap, so an ack racing a send never loses
 * the send. Acks are counted where they land rather than deferred to the
 * main thread: the next jog's ask must see an ack that already arrived.
 */
class BusyEpisodeOwner {
  public:
    using clock = BusyClock;
    static constexpr std::chrono::seconds kGraceWindow{2};
    /// Longest a busy episode can pass as the app's own after its last activity.
    static constexpr std::chrono::seconds kMaxSelfBusy{30};

    /// Send times kept to answer whether one landed just before an episode.
    static constexpr unsigned kHistory = 16;

    /// A send whose ack (or error) will be reported through note_done().
    void note_sent(clock::time_point now = clock::now()) {
        const long long ns = now.time_since_epoch().count();
        if (inflight_.fetch_add(1, std::memory_order_relaxed) <= 0) {
            progress_ns_.store(ns, std::memory_order_relaxed);
        }
        record(ns);
    }

    void note_done(clock::time_point now = clock::now()) {
        const long long ns = now.time_since_epoch().count();
        if (inflight_.load(std::memory_order_relaxed) > 0 &&
            ns - progress_ns_.load(std::memory_order_relaxed) > grace_ns()) {
            slow_ack_ns_.store(ns, std::memory_order_relaxed);
        }
        // A done without a matching send leaves the count at zero.
        int count = inflight_.load(std::memory_order_relaxed);
        while (count > 0 &&
               !inflight_.compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {
        }
        progress_ns_.store(ns, std::memory_order_relaxed);
        last_done_ns_.store(ns, std::memory_order_relaxed);
        activity_ns_.store(ns, std::memory_order_relaxed);
    }

    /// A send nothing will ack back here.
    void note_untracked_send(clock::time_point now = clock::now()) {
        const long long ns = now.time_since_epoch().count();
        record(ns);
        activity_ns_.store(ns, std::memory_order_relaxed);
    }

    /// Extends the lapse of an episode already owned, without adding a send
    /// that could start, or be counted as starting, another one.
    void note_still_active(clock::time_point now = clock::now()) {
        activity_ns_.store(now.time_since_epoch().count(), std::memory_order_relaxed);
    }

    [[nodiscard]] int inflight() const {
        return inflight_.load(std::memory_order_relaxed);
    }

    /// When the last tracked send settled; epoch when none has.
    [[nodiscard]] clock::time_point last_done() const {
        return clock::time_point{clock::duration{last_done_ns_.load(std::memory_order_relaxed)}};
    }

    bool owns(clock::time_point episode_start, clock::time_point now = clock::now()) {
        if (next_.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        const long long start_ns = episode_start.time_since_epoch().count();
        if (decided_episode_ns_.load(std::memory_order_relaxed) != start_ns) {
            const bool started = sent_in(start_ns - grace_ns(), start_ns);
            if (!started && adoption_pending(start_ns, now)) {
                return false;
            }
            episode_owned_.store(started || adopted(start_ns, now), std::memory_order_relaxed);
            decided_episode_ns_.store(start_ns, std::memory_order_relaxed);
        }
        if (stuck(now) || slow_ack_ns_.load(std::memory_order_relaxed) >= start_ns) {
            episode_owned_.store(false, std::memory_order_relaxed);
        }
        if (!episode_owned_.load(std::memory_order_relaxed)) {
            return false;
        }
        if (inflight() > 0) {
            return true; // in flight and not stuck: it acked or started within kGraceWindow
        }
        const clock::time_point last{clock::duration{activity_ns_.load(std::memory_order_relaxed)}};
        return (now - last) < kMaxSelfBusy;
    }

  private:
    static constexpr long long grace_ns() {
        return std::chrono::duration_cast<clock::duration>(kGraceWindow).count();
    }

    void record(long long ns) {
        const unsigned slot = next_.fetch_add(1, std::memory_order_relaxed) % kHistory;
        sent_ns_[slot].store(ns, std::memory_order_relaxed);
    }

    bool stuck(clock::time_point now) const {
        return inflight() > 0 &&
               now.time_since_epoch().count() - progress_ns_.load(std::memory_order_relaxed) >
                   grace_ns();
    }

    /// A tracked send landed before the episode settled, and everything sent has
    /// since acked promptly.
    bool adopted(long long start_ns, clock::time_point now) const {
        if (inflight() > 0 || stuck(now)) {
            return false;
        }
        const long long settle_ns =
            std::chrono::duration_cast<clock::duration>(IdleTimeoutBusy::kSettle).count();
        const long long done_ns = last_done_ns_.load(std::memory_order_relaxed);
        for (const auto& slot : sent_ns_) {
            const long long ns = slot.load(std::memory_order_relaxed);
            if (ns > start_ns && ns <= start_ns + settle_ns && done_ns >= ns) {
                return true;
            }
        }
        return false;
    }

    /// A tracked send could still adopt the episode once it acks.
    bool adoption_pending(long long start_ns, clock::time_point now) const {
        return inflight() > 0 && !stuck(now) &&
               sent_in(start_ns + 1, start_ns + std::chrono::duration_cast<clock::duration>(
                                                    IdleTimeoutBusy::kSettle)
                                                    .count());
    }

    bool sent_in(long long lo, long long hi) const {
        for (const auto& slot : sent_ns_) {
            const long long ns = slot.load(std::memory_order_relaxed);
            if (ns != 0 && ns >= lo && ns <= hi) {
                return true;
            }
        }
        return false;
    }

    std::array<std::atomic<long long>, kHistory> sent_ns_{};
    std::atomic<unsigned> next_{0};
    std::atomic<int> inflight_{0};
    /// Last time the in-flight set made progress: went non-empty, or acked.
    std::atomic<long long> progress_ns_{0};
    std::atomic<long long> last_done_ns_{0};
    std::atomic<long long> slow_ack_ns_{0};
    std::atomic<long long> activity_ns_{0};
    std::atomic<long long> decided_episode_ns_{0};
    std::atomic<bool> episode_owned_{false};
};

} // namespace helix
