// src/ui/print_control_view.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_view.h"

namespace helix::ui {

ControlButtonView compute_control_button_view(const ControlButtonInputs& in) {
    ControlButtonView v;

    // RAW_PRINT_STATE_OK: this must EXCLUDE Preparing. Widening it to
    // job_holds_machine() re-enables Pause during a pre-print block and makes
    // Stop send CANCEL_PRINT to a printer holding no job.
    //
    // The printer owns the job only once it reports running or paused. During a
    // host-side pre-start block it still describes the PREVIOUS job.
    const bool printer_has_the_job = (in.job_state == helix::PrintJobState::PRINTING ||
                                      in.job_state == helix::PrintJobState::PAUSED);
    const bool preparing = (in.lifecycle == PrintState::Preparing);

    // Stop routes host-side exactly when we hold a job the printer has not
    // taken. CANCEL_PRINT to an idle printer is worse than useless: AbortManager
    // reads the terminal state as proof the cancel succeeded while the queued
    // start_print still fires.
    v.stop_retires_preparing = in.has_preparing_job && !printer_has_the_job;

    // Cancel keeps one promise - the print does not happen - in both
    // architectures. Retiring needs no printer macro, so it is not gated on one.
    v.stop_enabled = v.stop_retires_preparing || (printer_has_the_job && in.cancel_available);

    // Pause is disabled for the whole preparing window, including the half where
    // Klipper is already reporting `printing` because pre-print work lives inside
    // PRINT_START. There is nothing meaningful to pause in either case, and PAUSE
    // mid-macro is a footgun: the macro keeps running and the pause lands at the
    // next print move, which breaks many custom start macros.
    // RAW_PRINT_STATE_OK: which macro the button would send is a question about
    // what the printer reports, not about who holds the machine.
    const bool slot_ok =
        (in.job_state == helix::PrintJobState::PAUSED) ? in.resume_available : in.pause_available;
    v.primary_enabled =
        printer_has_the_job && !preparing && in.pending == PendingAction::None && slot_ok;

    switch (in.pending) {
    case PendingAction::Pausing:
        v.primary_icon = CONTROL_ICON_HOURGLASS;
        v.primary_label = "Pausing...";
        break;
    case PendingAction::Resuming:
        v.primary_icon = CONTROL_ICON_HOURGLASS;
        v.primary_label = "Resuming...";
        break;
    case PendingAction::None:
        // RAW_PRINT_STATE_OK: which macro the button would send.
        if (in.job_state == helix::PrintJobState::PAUSED) {
            v.primary_icon = CONTROL_ICON_PLAY;
            v.primary_label = "Resume";
        } else {
            v.primary_icon = CONTROL_ICON_PAUSE;
            v.primary_label = "Pause";
        }
        break;
    }
    return v;
}

} // namespace helix::ui
