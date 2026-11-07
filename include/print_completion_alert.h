// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "completion_alert_mode.h"

#include <cstdint>

namespace helix {

/// How a print ended, as the completion alert sees it.
enum class PrintEnding : uint8_t { COMPLETE, CANCELLED, ERROR };

/// What the app does when a print ends. Sounds and the heater shutdown on an error sit outside
/// it: they happen whatever the plan says.
struct CompletionAlertPlan {
    bool wake = false;  ///< wake the display
    bool modal = false; ///< show the rich completion modal
    bool toast = false; ///< show the short completion toast
};

/**
 * @brief Decides how a print's end is announced
 *
 * An error always gets the modal; the print status panel already shows the result, so it needs
 * nothing else; otherwise `mode` decides. A running screensaver is the announcement itself: it
 * shows the end, so nothing wakes the display, and the modal is still created to wait behind it.
 */
CompletionAlertPlan plan_completion_alert(PrintEnding ending, CompletionAlertMode mode,
                                          bool on_print_status, bool saver_running);

} // namespace helix
