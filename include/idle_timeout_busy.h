// SPDX-License-Identifier: GPL-3.0-or-later
// include/idle_timeout_busy.h
#pragma once

#include <atomic>
#include <chrono>

namespace helix {

/**
 * The busy trackers' time source: steady_clock plus an offset that only moves
 * forward.
 *
 * Every decision IdleTimeoutBusy and BusyEpisodeOwner make compares two
 * readings of this clock, so advancing it is indistinguishable to them from
 * that much real time passing. That lets a test cross kSettle or kGraceWindow
 * without sleeping through it; nothing else reads this clock, so moving it
 * reaches no other timer.
 *
 * Its time_point is its own type, not steady_clock's: a steady_clock reading
 * handed to a busy tracker would ignore the offset, so the mix is a compile
 * error rather than a reading that lands in the past.
 */
class BusyClock {
  public:
    using duration = std::chrono::steady_clock::duration;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<BusyClock, duration>;
    static constexpr bool is_steady = true;

    static time_point now() {
        return time_point{std::chrono::steady_clock::now().time_since_epoch() +
                          duration{offset().load(std::memory_order_relaxed)}};
    }

    /// Moves now() forward by @p by. Never backward: a reading already taken
    /// must not land in the future. Tests only; tests/shell/test_code_lint.bats
    /// refuses a call under src/ or include/.
    static void advance_for_test(duration by) {
        if (by.count() > 0) {
            offset().fetch_add(by.count(), std::memory_order_relaxed);
        }
    }

  private:
    static std::atomic<rep>& offset() {
        static std::atomic<rep> value{0};
        return value;
    }
};

/**
 * Debounces Klipper's idle_timeout "Printing" flag before it counts as a
 * blocking operation.
 *
 * idle_timeout.state reads "Printing" while ANY gcode executes, not only during
 * a blocking operation. A printer with housekeeping delayed_gcode loops
 * therefore reports it in short bursts forever while sitting idle: debug bundle
 * L53W5PKG (Voron Trident with bedfanloop, _AIR_FILTER_TIMER and AFC PREP)
 * logged 632 transitions, the flag high for ~0.7 s out of every 10 s. Each of
 * those windows made the discretionary-gcode guard refuse jogs with "Printer is
 * busy — try again in a moment" on an idle printer.
 *
 * Duration is the discriminator: the operations the guard exists for (G28,
 * QGL, BED_MESH_CALIBRATE, PROBE_ACCURACY, manual probe, long macros) hold the
 * flag for many seconds, housekeeping bursts for well under one. Requiring the
 * flag to hold for kSettle before it blocks costs a window at the very start of
 * a real operation, where a jog can still slip through and get queued behind
 * it — bounded, and narrower than the false-refusal rate it removes.
 *
 * Asymmetric by design: clearing takes effect immediately, since there is no
 * false negative to protect against on that edge and making the user wait after
 * a homing completes would be its own bug.
 *
 * Not thread-safe; fed from PrinterCalibrationState::update_from_status and read
 * at gcode send time, both on the main thread. Mirrors the shape of
 * AppMotionActivity (app_motion_activity.h): header-only, with a defaulted
 * `now` so unit tests pass explicit times; end-to-end tests advance BusyClock.
 */
class IdleTimeoutBusy {
  public:
    using clock = BusyClock;

    /// How long idle_timeout must report "Printing" before it counts as blocking.
    static constexpr std::chrono::milliseconds kSettle{1000};

    /**
     * @brief Report the current idle_timeout "Printing" state.
     *
     * Repeats are idempotent: only a false->true edge (re)starts the settle
     * window. Restarting it on every repeated report would let a printer that
     * re-sends status faster than kSettle never reach the blocking state, which
     * is a hole in the guard rather than a debounce.
     */
    void set_printing(bool printing, clock::time_point now = clock::now()) {
        if (printing && !printing_) {
            printing_since_ = now;
        }
        printing_ = printing;
    }

    /// True once "Printing" has held continuously for at least kSettle.
    [[nodiscard]] bool blocking(clock::time_point now = clock::now()) const {
        return printing_ && (now - printing_since_) >= kSettle;
    }

    /// When the current "Printing" episode began; meaningful while printing.
    [[nodiscard]] clock::time_point printing_since() const {
        return printing_since_;
    }

  private:
    bool printing_ = false;
    clock::time_point printing_since_{};
};

} // namespace helix
