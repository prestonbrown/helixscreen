// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_print_latch.h"

#include <algorithm>

namespace helix::ui {

namespace {

bool running(ClockPrintPhase p) {
    return p == ClockPrintPhase::PREPARING || p == ClockPrintPhase::PRINTING ||
           p == ClockPrintPhase::PAUSED;
}

bool ended(ClockPrintPhase p) {
    return p == ClockPrintPhase::COMPLETE || p == ClockPrintPhase::FAILED ||
           p == ClockPrintPhase::CANCELLED;
}

} // namespace

void PrintEndLatch::reset() {
    *this = PrintEndLatch{};
}

ClockPrintPhase PrintEndLatch::step(const PrintLatchInput& in) {
    if (has_epoch_ && in.printer_epoch != epoch_) {
        // Another connection or another printer: nothing seen before applies to it.
        latched_ = ClockPrintPhase::NONE;
        saw_running_ = false;
    }
    has_epoch_ = true;
    epoch_ = in.printer_epoch;

    const ClockPrintPhase phase = in.host.phase;
    if (running(phase)) {
        latched_ = ClockPrintPhase::NONE;
        saw_running_ = true;
        progress_pct_ = std::clamp(in.host.progress_pct, 0, 100);
        return phase;
    }
    if (!ended(phase)) {
        latched_ = ClockPrintPhase::NONE;
        saw_running_ = false;
        return ClockPrintPhase::NONE;
    }

    if (saw_running_) {
        // The step from running to ended: this end is what the savers show.
        latched_ = phase;
        latched_at_ms_ = in.now_ms;
        saw_running_ = false;
        return latched_;
    }
    // Input after the end means someone has seen it. Unsigned differences survive the clock
    // wrapping.
    if (latched_ != ClockPrintPhase::NONE && in.inactive_ms < in.now_ms - latched_at_ms_) {
        latched_ = ClockPrintPhase::NONE;
    }
    return latched_;
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
