// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/print_control_buttons_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_control_buttons.h"

namespace helix::ui {

// Test-only access to PrintControlButtons internals. The singleton persists
// across test cases, so reset() must fully tear it down for isolation.
struct PrintControlButtonsTestAccess {
    static void reset() {
        auto& s = PrintControlButtons::instance();
        s.print_state_observer_.reset();
        s.preparing_start_observer_.reset();
        s.preparing_epoch_observer_.reset();
        if (s.pending_action_timeout_) {
            lv_timer_delete(s.pending_action_timeout_);
            s.pending_action_timeout_ = nullptr;
        }
        // Go through the shared teardown rather than reaching for deinit_all()
        // directly: it is what signals subject death to outside ObserverGuards.
        // This runs on EVERY HelixTestFixture construction, so a hand-rolled
        // teardown here silently dangles every cross-object observer in the
        // process (PrintStatusPanel::pending_action_observer_ above all).
        s.teardown_subjects();
        s.pending_action_ = PendingAction::None;
        s.resume_answered_ = false;
        s.resume_error_seen_ = false;
    }

    /// What the cancel modal's confirm button runs.
    static void confirm_stop() {
        PrintControlButtons::instance().confirm_stop();
    }

    static bool cancel_requested() {
        return PrintControlButtons::instance().cancel_state_ !=
               PrintControlButtons::CancelState::Idle;
    }

    static bool cancel_confirm_armed() {
        return PrintControlButtons::instance().cancel_confirm_timer_ != nullptr;
    }

    /// Fire the confirm timer now instead of waiting kCancelConfirmMs.
    static void fire_cancel_confirm() {
        auto& s = PrintControlButtons::instance();
        s.cancel_confirm_timer();
        s.on_cancel_confirm_timeout();
    }

    /// The StaticSubjectRegistry teardown's own step.
    static void teardown_subjects() {
        PrintControlButtons::instance().teardown_subjects();
    }

    /// True if the singleton currently holds a live observer on the global
    /// print_state_enum subject. Used by the nightly-crash regression test to
    /// assert reset_all() detaches the controller before subjects are torn down.
    static bool has_observer() {
        return static_cast<bool>(PrintControlButtons::instance().print_state_observer_);
    }

    static void set_pending(PendingAction a) {
        PrintControlButtons::instance().start_pending_action(a);
    }

    static PendingAction pending() {
        return PrintControlButtons::instance().pending_action_;
    }
};

} // namespace helix::ui
