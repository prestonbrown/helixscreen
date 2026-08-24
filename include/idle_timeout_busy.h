// SPDX-License-Identifier: GPL-3.0-or-later
// include/idle_timeout_busy.h
#pragma once

#include <chrono>

namespace helix {

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
 * flag to hold for SETTLE before it blocks costs a window at the very start of
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
 * `now` so tests drive it without a clock injection seam.
 */
class IdleTimeoutBusy {
  public:
    using clock = std::chrono::steady_clock;

    /// How long idle_timeout must report "Printing" before it counts as blocking.
    static constexpr std::chrono::milliseconds SETTLE{1000};

    /**
     * @brief Report the current idle_timeout "Printing" state.
     *
     * Repeats are idempotent: only a false->true edge (re)starts the settle
     * window. Restarting it on every repeated report would let a printer that
     * re-sends status faster than SETTLE never reach the blocking state, which
     * is a hole in the guard rather than a debounce.
     */
    void set_printing(bool printing, clock::time_point now = clock::now()) {
        if (printing && !printing_) {
            printing_since_ = now;
        }
        printing_ = printing;
    }

    /// True once "Printing" has held continuously for at least SETTLE.
    [[nodiscard]] bool blocking(clock::time_point now = clock::now()) const {
        return printing_ && (now - printing_since_) >= SETTLE;
    }

  private:
    bool printing_ = false;
    clock::time_point printing_since_{};
};

} // namespace helix
