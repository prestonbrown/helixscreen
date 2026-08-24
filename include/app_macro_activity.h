// SPDX-License-Identifier: GPL-3.0-or-later
// include/app_macro_activity.h
#pragma once

#include <atomic>
#include <chrono>

namespace helix {

/**
 * Tracks app-initiated macro / homing / calibration / filament-op RPC activity
 * so the busy-queue TOAST can tell self-inflicted busy (idle_timeout ==
 * "Printing" because the user just pressed Unload in HelixScreen) from an
 * external blocking op (a bed mesh kicked off from Mainsail, a macro typed into
 * another frontend's console). Deliberately modeled on AppMotionActivity.
 * Thread-safe: sends stamp from the main thread, acks/errors from the websocket
 * thread.
 *
 * SCOPE: consulted ONLY by the busy-toast decision in
 * IMoonrakerAPI::execute_gcode. Neither PrinterState::is_blocking_operation_active()
 * nor is_external_blocking_operation_active() reads it, and neither may start:
 * those predicates also gate motion, and letting a late jog through during a
 * filament op is a toolhead-collision hazard (#1108). Blast radius is one
 * notification (prestonbrown/helixscreen#1206).
 *
 * Why a grace window is needed at all: a macro's printer.gcode.script RPC stays
 * in flight for the macro's whole duration, so inflight_ alone would seem
 * sufficient. It is not — idle_timeout.state lags the RPC ack. A command landing
 * in the gap between "Moonraker acked the macro" and "idle_timeout finally
 * cleared" would still see a blocking op with nothing outstanding, and toast.
 *
 * Invariant: IMoonrakerAPI::execute_gcode wraps BOTH the success and error
 * callback of every stamped send — including when the caller supplied neither,
 * which is the common case for macro sends — so inflight_ stays balanced.
 */
class AppMacroActivity {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds GRACE_WINDOW{2};

    /**
     * Ceiling on how long a single in-flight send may keep the tracker active.
     *
     * The request tracker fails pending requests out on timeout and on
     * disconnect, but there are still paths where NEITHER callback fires — the
     * MoonrakerClient destructor drops pending requests deliberately (UAF
     * avoidance), and cancel_request() settles nothing. An unbalanced counter
     * would otherwise suppress the busy toast for the entire session: the same
     * silent-wedge shape as #1129, where a dropped callback pair left
     * LedController's in-flight counter stuck and greyed the light buttons out
     * until the printer disconnected.
     *
     * The longest RPC timeout in the codebase is 300000 ms (HOMING_TIMEOUT_MS /
     * AMS_OPERATION_TIMEOUT_MS / MACRO_TIMEOUT_MS, moonraker_api.h), so a
     * 10-minute ceiling sits safely above any legitimate in-flight macro and
     * converts a permanent wedge into a bounded, self-healing one.
     */
    static constexpr std::chrono::minutes MAX_INFLIGHT_AGE{10};

    void note_sent(clock::time_point now = clock::now()) {
        last_sent_ns_.store(now.time_since_epoch().count(), std::memory_order_relaxed);
        inflight_.fetch_add(1, std::memory_order_relaxed);
    }

    void note_done(clock::time_point now = clock::now()) {
        last_done_ns_.store(now.time_since_epoch().count(), std::memory_order_relaxed);
        // Clamp at zero if a done arrives without a matching send.
        int prev = inflight_.fetch_sub(1, std::memory_order_relaxed);
        if (prev <= 0) {
            inflight_.store(0, std::memory_order_relaxed);
        }
    }

    bool recently_active(clock::time_point now = clock::now()) const {
        if (inflight_.load(std::memory_order_relaxed) > 0) {
            // Self-healing ceiling: if even the NEWEST send is older than
            // MAX_INFLIGHT_AGE, the counter is stuck, not busy. Treat it as
            // inactive so the toast comes back rather than staying suppressed
            // for the rest of the session.
            const auto sent_ns = last_sent_ns_.load(std::memory_order_relaxed);
            if (sent_ns != 0) {
                const clock::time_point sent{clock::duration{sent_ns}};
                if ((now - sent) < MAX_INFLIGHT_AGE) {
                    return true;
                }
            }
        }
        const auto last_ns = last_done_ns_.load(std::memory_order_relaxed);
        if (last_ns == 0) {
            return false;
        }
        const clock::time_point last{clock::duration{last_ns}};
        return (now - last) < GRACE_WINDOW;
    }

  private:
    std::atomic<int> inflight_{0};
    std::atomic<long long> last_done_ns_{0};
    std::atomic<long long> last_sent_ns_{0};
};

} // namespace helix
