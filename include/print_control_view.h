// include/print_control_view.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_lifecycle_state.h" // for PrintState
#include "printer_state.h"         // for helix::PrintJobState enum

namespace helix::ui {

/// Optimistic in-flight action while a Pause/Resume RPC is unacknowledged.
/// Int values are the wire format of the `print_pending_action` subject.
enum class PendingAction : int { None = 0, Pausing = 1, Resuming = 2 };

/// MDI glyphs (UTF-8). pause=F03E4, play=F040A, hourglass=F051F.
inline constexpr const char* CONTROL_ICON_PAUSE = "\xF3\xB0\x8F\xA4";
inline constexpr const char* CONTROL_ICON_PLAY = "\xF3\xB0\x90\x8A";
inline constexpr const char* CONTROL_ICON_HOURGLASS = "\xF3\xB0\x94\x9F";

/// Pure view model for the two print-control buttons. No LVGL, no globals.
/// `primary_label` is an English string — callers pass it through lv_tr(),
/// which falls back to the string itself when no translation exists.
struct ControlButtonView {
    const char* primary_icon = CONTROL_ICON_PAUSE;
    const char* primary_label = "Pause";
    bool primary_enabled = false;
    bool stop_enabled = false;
    /// Stop must retire the preparing job rather than send CANCEL_PRINT.
    /// Computed here, not at the call site, so the button's enablement and the
    /// handler's routing can never disagree about which mechanism applies.
    bool stop_retires_preparing = false;
};

/// Everything the two buttons depend on.
///
/// A struct rather than positional parameters: the row is gated on both the
/// printer's job state and the UI lifecycle, and a defaulted trailing bool is
/// exactly the shape of mistake that silently drops a dimension at one call
/// site (cf. the SubjectLifetime trap in CLAUDE.md).
struct ControlButtonInputs {
    /// RAW_PRINT_STATE_OK: the wire half of the pair. `lifecycle` below is the
    /// derived half; the view needs both, which is why neither replaced the other.
    helix::PrintJobState job_state = helix::PrintJobState::STANDBY;
    PrintState lifecycle = PrintState::Idle;
    /// True while this screen owns a job the printer has not accepted yet.
    bool has_preparing_job = false;
    PendingAction pending = PendingAction::None;
    bool pause_available = false;
    bool resume_available = false;
    bool cancel_available = false;
};

/// Decide both buttons.
///
/// The governing rule (#798) is that affordance is a function of `PrintState`
/// alone: it must not depend on whether pre-print work runs host-side, in front
/// of the job, or firmware-side inside Klipper's PRINT_START. Keying on
/// `job_state` alone cannot express that, because the two architectures report
/// different job states for the same user-visible moment.
ControlButtonView compute_control_button_view(const ControlButtonInputs& in);

} // namespace helix::ui
