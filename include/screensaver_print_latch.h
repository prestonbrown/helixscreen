// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"

#include <cstdint>

/**
 * @file screensaver_print_latch.h
 * @brief How long an ended print stays on the screensavers
 *
 * No LVGL, no clock: the manager steps it on every idle-check tick, with or without a saver
 * running, so a print that ends while the display is awake is still latched when a saver
 * starts later.
 */

namespace helix::ui {

/// One tick's worth of what the latch reads.
struct PrintLatchInput {
    ClockPrintPhase host_phase = ClockPrintPhase::NONE; ///< what the printer reports now
    uint64_t printer_epoch = 0; ///< changes on a reconnect or a switch to another printer
    uint32_t inactive_ms = 0;   ///< time since the last user input
};

/**
 * @brief Holds a print's end on screen until someone has seen it
 *
 * A running phase (PREPARING, PRINTING, PAUSED) passes through and clears any latch. The step
 * from a running phase to COMPLETE, FAILED or CANCELLED latches that end, and it reads as the
 * phase until user input arrives after it, a new print starts, the printer reports NONE, or the
 * printer epoch changes. An end the latch never saw a print reach reads as NONE, so a printer
 * that was already finished when the app connected shows no stale result. Display sleep is
 * not input and does not clear it.
 */
class PrintEndLatch {
  public:
    /// Advances by `dt_ms` with the outside state now `in`; returns the phase savers show.
    ClockPrintPhase step(uint32_t dt_ms, const PrintLatchInput& in);
    /// The latched end, or NONE.
    ClockPrintPhase latched() const {
        return latched_;
    }
    void reset();

  private:
    ClockPrintPhase latched_ = ClockPrintPhase::NONE;
    bool saw_running_ = false;
    bool has_epoch_ = false;
    uint64_t epoch_ = 0;
    /// Time since the end was latched.
    uint64_t latched_ms_ = 0;
};

} // namespace helix::ui
