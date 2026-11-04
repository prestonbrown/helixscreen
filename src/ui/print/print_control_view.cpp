// src/ui/print/print_control_view.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_view.h"

namespace helix::ui {

ControlButtonView compute_control_button_view(const ControlButtonInputs& in) {
    ControlButtonView v;

    // This must EXCLUDE Preparing. Widening it to job_holds_machine() re-enables
    // Pause during a pre-print block and makes Stop send CANCEL_PRINT to a
    // printer holding no job.
    //
    // The printer owns the job only once it reports running or paused. During a
    // host-side pre-start block it still describes the PREVIOUS job.
    const bool printer_has_the_job = helix::printer_has_job(in.job_state);
    const bool preparing = (in.lifecycle == PrintState::Preparing);

    // Stop routes host-side exactly when we hold a job the printer has not
    // been sent. CANCEL_PRINT to a printer with nothing coming is worse than
    // useless: AbortManager reads the terminal state as proof the cancel
    // succeeded. Once the start is sent, withdrawing locally stops nothing, so
    // the cancel goes to the printer even though it has not reported the job.
    const bool unclaimed = in.has_preparing_job && !printer_has_the_job;
    if (unclaimed) {
        v.stop_route = in.start_sent ? StopRoute::CancelSentStart : StopRoute::RetirePreparing;
    }

    // Cancel keeps one promise - the print does not happen - in both
    // architectures. Neither preparing route is gated on the cancel macro:
    // retiring needs none, and refusing a sent start's cancel would leave that
    // start unstoppable from here. A cancel already in flight leaves nothing for
    // a second tap to do.
    v.stop_enabled =
        !in.cancel_requested && (unclaimed || (printer_has_the_job && in.cancel_available));

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
        v.primary_icon = kControlIconHourglass;
        v.primary_label = kControlLabelPausing;
        break;
    case PendingAction::Resuming:
        v.primary_icon = kControlIconHourglass;
        v.primary_label = kControlLabelResuming;
        break;
    case PendingAction::None:
        // RAW_PRINT_STATE_OK: which macro the button would send.
        if (in.job_state == helix::PrintJobState::PAUSED) {
            v.primary_icon = kControlIconPlay;
            v.primary_label = kControlLabelResume;
        } else {
            v.primary_icon = kControlIconPause;
            v.primary_label = kControlLabelPause;
        }
        break;
    }
    return v;
}

} // namespace helix::ui
