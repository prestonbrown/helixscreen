// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_completion_alert.h"

namespace helix {

CompletionAlertPlan plan_completion_alert(PrintEnding ending, CompletionAlertMode mode,
                                          bool on_print_status, bool saver_running) {
    CompletionAlertPlan plan;
    if (ending == PrintEnding::ERROR) {
        plan.modal = true;
    } else if (!on_print_status) {
        plan.modal = mode == CompletionAlertMode::ALERT;
        plan.toast = mode == CompletionAlertMode::NOTIFICATION;
    }
    if (saver_running) {
        // The saver shows the end; a toast would expire unseen behind it.
        plan.toast = false;
    } else {
        plan.wake = plan.modal || plan.toast;
    }
    return plan;
}

bool should_turn_heaters_off(PrintEnding ending, bool firmware_owns_error_heaters) {
    return ending == PrintEnding::ERROR && !firmware_owns_error_heaters;
}

} // namespace helix
