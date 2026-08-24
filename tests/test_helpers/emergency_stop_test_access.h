// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_emergency_stop.h"

/**
 * Reaches EmergencyStopOverlay's recovery state so tests can assert *when* it
 * changes, not just that a dialog eventually appears. Declared a friend in
 * ui_emergency_stop.h, following the existing TestAccess pattern
 * (tests/test_helpers/, [L088]) rather than adding a production accessor.
 *
 * The distinction matters for show_recovery_for(): the dialog is built via an
 * async hop either way, so "a dialog appeared" cannot tell a caller-thread
 * mutation apart from a marshalled one. recovery_reason_ can.
 */
class EmergencyStopOverlayTestAccess {
  public:
    static RecoveryReason recovery_reason(const EmergencyStopOverlay& o) {
        return o.recovery_reason_;
    }

    /// Reset to a known baseline so a test can observe the transition itself.
    static void reset_recovery_reason(EmergencyStopOverlay& o) {
        o.recovery_reason_ = RecoveryReason::NONE;
    }

    /// Clear the suppression deadline. The overlay is a process-wide singleton
    /// and Catch2 runs the suite in one process, so a test that suppresses must
    /// clear up after itself or every later test inherits the window.
    static void reset_suppression(EmergencyStopOverlay& o) {
        o.suppress_recovery_until_.store(0, std::memory_order_relaxed);
    }

    /// Set the user-initiated-restart flag the way the recovery dialog's
    /// Restart path does. Tests driving a klippy READY need the flag armed to
    /// exercise the expected-restart branch, and a test that arms it without
    /// delivering READY must clear it or every later test inherits it.
    static void set_restart_in_progress(EmergencyStopOverlay& o, bool in_progress) {
        o.restart_in_progress_.store(in_progress, std::memory_order_relaxed);
    }

    /// Drop the dependency pointers init() installed. Same singleton problem as
    /// reset_suppression(), but it outlives memory rather than a deadline: tests
    /// pass init() a stack local or fixture member (test_unified_recovery_dialog
    /// and test_fault_modal_dismiss both do), while the singleton keeps the raw
    /// pointer for the rest of the suite. get_klippy_state_message() returns a
    /// reference INTO that PrinterState, so a later test draining the queue runs
    /// update_recovery_dialog_content() against a dead frame — ASan reports it as
    /// a stack-use-after-return, blamed on whichever test reused those bytes.
    /// Cleared centrally in HelixTestFixture::reset_all() rather than per test,
    /// so the next init() caller cannot reintroduce it by forgetting.
    static void reset_dependencies(EmergencyStopOverlay& o) {
        o.printer_state_ = nullptr;
        o.api_ = nullptr;
    }
};
