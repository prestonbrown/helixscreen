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
    ClockPrint host;            ///< what the printer reports now
    uint64_t printer_epoch = 0; ///< changes on a reconnect or a switch to another printer
    uint32_t now_ms = 0;        ///< a millisecond clock; wraps
    uint32_t inactive_ms = 0;   ///< time since the last user input, on the same clock
};

/**
 * @brief Holds a print's end on screen until someone has seen it
 *
 * A running phase (PREPARING, PRINTING, PAUSED) passes through and clears any latch. The step
 * from a running phase to COMPLETE, FAILED or CANCELLED latches that end, and it reads as the
 * phase, with the progress the print last reported while running, until user input arrives
 * after it, a new print starts, the printer reports NONE, or the printer epoch changes. An end the
 * latch never saw a print reach reads as NONE, so a printer that was already finished when the app
 * connected shows no stale result. Display sleep is not input and does not clear it.
 */
class PrintEndLatch {
  public:
    /// Takes the outside state now `in`; returns the phase savers show.
    ClockPrintPhase step(const PrintLatchInput& in);
    /// The latched end, or NONE.
    ClockPrintPhase latched() const {
        return latched_;
    }
    /// The progress the print last reported while running, 0 to 100.
    int32_t latched_progress_pct() const {
        return progress_pct_;
    }
    void reset();

  private:
    ClockPrintPhase latched_ = ClockPrintPhase::NONE;
    bool saw_running_ = false;
    bool has_epoch_ = false;
    uint64_t epoch_ = 0;
    int32_t progress_pct_ = 0;
    /// now_ms when the end was latched.
    uint32_t latched_at_ms_ = 0;
};

} // namespace helix::ui
