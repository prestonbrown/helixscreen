// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_print_latch.h"

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

ClockPrintPhase PrintEndLatch::step(uint32_t dt_ms, const PrintLatchInput& in) {
    if (has_epoch_ && in.printer_epoch != epoch_) {
        // Another connection or another printer: nothing seen before applies to it.
        latched_ = ClockPrintPhase::NONE;
        saw_running_ = false;
    }
    has_epoch_ = true;
    epoch_ = in.printer_epoch;

    if (running(in.host_phase)) {
        latched_ = ClockPrintPhase::NONE;
        saw_running_ = true;
        return in.host_phase;
    }
    if (!ended(in.host_phase)) {
        latched_ = ClockPrintPhase::NONE;
        saw_running_ = false;
        return ClockPrintPhase::NONE;
    }

    if (saw_running_) {
        // The step from running to ended: this end is what the savers show.
        latched_ = in.host_phase;
        latched_ms_ = 0;
        saw_running_ = false;
        return latched_;
    }
    if (latched_ != ClockPrintPhase::NONE) {
        latched_ms_ += dt_ms;
        if (static_cast<uint64_t>(in.inactive_ms) < latched_ms_) {
            // Input arrived after the end: someone has seen it.
            latched_ = ClockPrintPhase::NONE;
        }
    }
    return latched_;
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
