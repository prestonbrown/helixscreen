// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"

/**
 * @file screensaver_print_colors.h
 * @brief One colour per print state, shared by every saver, so a glance reads the same anywhere
 *
 * Raw pixel values rather than theme tokens: canvas savers write pixels directly. Each stays
 * distinct from the others after RGB565 quantisation.
 */

namespace helix::ui {

inline constexpr Rgb kPrintPaused{255, 176, 32};
inline constexpr Rgb kPrintDone{72, 200, 104};
inline constexpr Rgb kPrintFailed{232, 72, 64};

/// The colour a saver shows `mode` in: paused amber, done green, failed or cancelled red, and
/// the saver's own `accent` otherwise.
constexpr Rgb print_state_color(ClockMode mode, Rgb accent) {
    switch (mode) {
    case ClockMode::PAUSED:
        return kPrintPaused;
    case ClockMode::DONE:
        return kPrintDone;
    case ClockMode::FAILED:
    case ClockMode::CANCELLED:
        return kPrintFailed;
    case ClockMode::CLOCK:
    case ClockMode::PROGRESS:
    case ClockMode::COUNTDOWN:
        break;
    }
    return accent;
}

} // namespace helix::ui
