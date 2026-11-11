// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::ui {

/// What the file detail view's primary button currently does.
/// Int values are the wire format of the `print_select_button_mode` subject
/// (0 = Print, 1 = Queue); the Disabled mode is carried by
/// `print_select_can_print` going 0, not by a third value.
enum class PrintSelectButtonMode : int { Print = 0, Queue = 1 };

/// Pure view model for the detail view's Print button. No LVGL, no globals.
/// `blocked_reason` is an English string — the panel passes it through
/// lv_tr(), which falls back to the string itself when no translation exists.
/// Empty exactly when the button is usable.
struct PrintSelectButtonView {
    PrintSelectButtonMode mode = PrintSelectButtonMode::Print;
    const char* blocked_reason = "";
};

/// Everything the button mode depends on. A struct rather than positional
/// parameters so a dropped dimension is a compile error at the call site, not
/// a silently defaulted bool.
struct PrintSelectButtonInputs {
    /// job_holds_machine(get_print_lifecycle()): the printer itself reports an
    /// active job. Preparing counts here even though the wire job state cannot
    /// express it — the machine is committed either way.
    bool machine_busy = false;
    /// True while this app owns a start the printer has not accepted yet
    /// (is_print_in_progress()): queueing behind our own unconfirmed job
    /// would offer "Add to Queue" for a machine about to be busy anyway.
    bool print_start_committed = false;
    /// True while macro analysis of the shown file is still running.
    bool macro_analysis_running = false;
    /// True while the shown file's operations scan is still reading and an
    /// option as set needs it (a step turned off, a START_PRINT rewrite).
    bool file_check_pending = false;
    /// True when Moonraker's server.info lists the job_queue component.
    bool job_queue_available = false;
    /// True while an add_job request is on the wire: the button disables for
    /// the flight — in either mode, since the print can end mid-flight — so
    /// a double-tap cannot queue the file twice and a Print tap cannot start
    /// the file the add is about to queue.
    bool queue_add_in_flight = false;
};

/// Decide the button. Queue mode needs all three: the machine held by an
/// active job (not merely an unconfirmed local start), the job_queue
/// component present to receive the job, and macro analysis finished — an
/// analysis in progress leaves the button disabled exactly as it does for
/// Print, because the option rows that get saved are not settled yet. An
/// in-flight add outranks all of it: the button reads as a disabled Queue
/// for the flight, whatever the machine is doing, because the pending
/// operation is a queue add. A pending file check holds a direct Print only:
/// a queue add starts nothing.
PrintSelectButtonView compute_print_select_button_view(const PrintSelectButtonInputs& in);

} // namespace helix::ui
