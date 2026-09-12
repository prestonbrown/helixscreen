// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>

class IMoonrakerAPI;

namespace helix::ui {

/// Run the active AMS backend's prepare_for_resume, then dispatch the
/// Resume StandardMacro. On either error path (prep failure or macro send
/// failure) emit a contextual `NOTIFY_ERROR` toast and invoke `on_failure`,
/// which call sites typically use to clear optimistic-UI state.
///
/// All work is async; this returns immediately. `on_failure` and the
/// internal toast emission fire on the **main thread** even when the
/// underlying macro error callback originates from a background WebSocket
/// thread — the helper bounces those through the UpdateQueue. Callers
/// don't need to do their own marshalling for the on_failure body, but
/// they must still ensure any state it touches is alive at the time of
/// the bounce (singletons or main-thread-owned state are the usual
/// choices).
///
/// @param api          Moonraker API to use for macro execution. Must
///                     outlive the dispatch — typically a panel member or
///                     a singleton-owned pointer. Must not be nullptr;
///                     callers handle the api == nullptr case (e.g. mock
///                     mode fallback) before calling.
/// @param log_prefix   Spdlog tag for log lines, e.g. `"[Print Status]"`.
///                     Passed by value so the helper owns its own copy
///                     and the async lambdas don't depend on the caller's
///                     storage outliving the dispatch.
/// @param on_failure   Optional. Invoked on the main thread on either
///                     error path AFTER the toast has been emitted.
///                     Default: no-op.
void dispatch_prepared_resume(IMoonrakerAPI* api, std::string log_prefix,
                              std::function<void()> on_failure = {});

/// Confirm, then cancel the running print through the configured Cancel
/// StandardMacro.
///
/// **Raises a confirmation dialog and returns immediately** — the macro is sent
/// only if the user accepts, from the confirm callback. Nothing is dispatched
/// synchronously, so a caller must not treat the return as "the print is
/// cancelling". Copy and severity match print_cancel_confirm_modal.xml, the
/// confirmation the print-status panel's Stop button raises, because both
/// cancel the same print.
///
/// Cancelling a print is destructive and unrecoverable; both callers reach it
/// from a dialog whose other buttons are all harmless, which is exactly the
/// shape a misplaced tap ruins a print with.
///
/// Refuses up front, before confirming, with a "Cancel macro not configured"
/// warning toast when the slot is empty — skipping that check makes the button
/// silently do nothing. Send errors surface as a NOTIFY_ERROR toast.
///
/// Both the runout guidance dialog and the home Filament tile's paused modal
/// call this, so the confirmation cannot diverge between two dialogs that render
/// the same buttons. PrintControlButtons' Stop button is a separate path
/// (PrintCancelModal → AbortManager) that has always confirmed.
///
/// Main thread only — it shows a modal.
///
/// @param api          Moonraker API for macro execution. A null api is a no-op
///                     (logged), matching the guard both call sites already had.
/// @param log_prefix   Spdlog tag, e.g. `"[FilamentRunoutHandler]"`. By value so
///                     the confirmation context owns its copy.
/// @param on_confirmed Optional. Run on the main thread when the user accepts,
///                     before the macro is sent, and never if they decline.
///                     Both callers use it to close the dialog the Cancel Print
///                     button lives in — that dialog must stay up behind the
///                     confirmation so declining returns to it, so closing it is
///                     this callback's job rather than the button hook's. Guard
///                     it with a lifetime token like any retained callback.
void dispatch_cancel_print(IMoonrakerAPI* api, std::string log_prefix,
                           std::function<void()> on_confirmed = {});

/// Show the "Print Was Terminated — Restart from the beginning?" modal.
/// Exposed so post-resume backstops (e.g. AmsBackendSnapmaker) can surface it
/// after a silent RESUME no-op, not just the up-front prepare_for_resume gate.
///
/// @param authored_reason Backend-authored explanation of why restart is
///                        needed (e.g. an AmsError's user_msg). Preferred over
///                        Klipper's raw print_stats.message; empty falls back
///                        to that firmware text, so a caller with no authored
///                        copy yet still shows why restart is needed.
/// @param on_failure      May be null.
void show_restart_required_modal(IMoonrakerAPI* api, const std::string& filename,
                                 const std::string& authored_reason, std::string log_prefix,
                                 std::function<void()> on_failure);

} // namespace helix::ui
