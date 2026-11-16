// src/ui/print/print_select_button_view.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_select_button_view.h"

namespace helix::ui {

PrintSelectButtonView compute_print_select_button_view(const PrintSelectButtonInputs& in) {
    // The in-flight guard outranks everything, in both modes: the print can
    // end while the add is still on the wire, and an enabled Print button at
    // that moment starts the very file the add is about to queue.
    if (in.queue_add_in_flight) {
        return {PrintSelectButtonMode::Queue, "Adding to queue..."};
    }

    if (in.machine_busy) {
        if (in.job_queue_available && !in.macro_analysis_running) {
            return {PrintSelectButtonMode::Queue, ""};
        }
        return {PrintSelectButtonMode::Print, "Printing: start after this job"};
    }

    if (in.print_start_committed) {
        return {PrintSelectButtonMode::Print, "Printing: start after this job"};
    }

    if (in.macro_analysis_running) {
        return {PrintSelectButtonMode::Print, "Analyzing data..."};
    }

    if (in.file_check_pending) {
        return {PrintSelectButtonMode::Print, "Checking file…"};
    }

    return {PrintSelectButtonMode::Print, ""};
}

} // namespace helix::ui
