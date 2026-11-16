// SPDX-License-Identifier: GPL-3.0-or-later
// include/app_motion_activity.h
#pragma once

#include "busy_episode_owner.h"

#include <atomic>
#include <chrono>

namespace helix {

/**
 * Tracks app-initiated motion (jog) RPC activity so the discretionary-gcode
 * busy guard can tell self-inflicted busy (idle_timeout == "Printing" because
 * OUR jog is executing) from an external blocking op (calibration, console
 * gcode from another UI). Thread-safe: sends stamp from the main thread,
 * acks/errors from the websocket thread.
 *
 * Invariant: MoonrakerMotionAPI wraps BOTH the success and error callback of
 * every stamped send, and the request tracker guarantees one of them fires
 * (including on timeout) — so the in-flight count cannot leak upward
 * permanently. A leak would read as a stuck send and disown, never adopt.
 */
class AppMotionActivity {
  public:
    using clock = BusyClock;
    static constexpr std::chrono::seconds kGraceWindow = BusyEpisodeOwner::kGraceWindow;
    /// Longest a busy episode can pass as the app's own after its last send.
    static constexpr std::chrono::seconds kMaxSelfBusy = BusyEpisodeOwner::kMaxSelfBusy;

    void note_sent(clock::time_point now = clock::now()) {
        episode_.note_sent(now);
    }

    /**
     * Record a send that can start a busy episode without tracking it in
     * flight: a G28 whose continuation moves the toolhead once it acks, while
     * a homing macro may still hold idle_timeout "Printing".
     */
    void note_episode_send(clock::time_point now = clock::now()) {
        episode_.note_untracked_send(now);
    }

    /// Keeps an episode the app already owns from lapsing; starts nothing.
    void note_still_active(clock::time_point now = clock::now()) {
        episode_.note_still_active(now);
    }

    void note_done(clock::time_point now = clock::now()) {
        episode_.note_done(now);
    }

    bool recently_active(clock::time_point now = clock::now()) const {
        if (episode_.inflight() > 0) {
            return true;
        }
        const clock::time_point last = episode_.last_done();
        if (last.time_since_epoch().count() == 0) {
            return false;
        }
        return (now - last) < kGraceWindow;
    }

    /**
     * Whether a busy episode that began at @p episode_start is the app's own
     * motion. The ack arrives when Klipper queues a move, so a long absolute
     * move keeps idle_timeout "Printing" well past recently_active(). Decided
     * by BusyEpisodeOwner from the motion sends alone.
     */
    bool owns_busy_episode(clock::time_point episode_start, clock::time_point now = clock::now()) {
        return episode_.owns(episode_start, now);
    }

  private:
    BusyEpisodeOwner episode_;
};

} // namespace helix
