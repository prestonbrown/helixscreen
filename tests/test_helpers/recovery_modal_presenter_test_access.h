// tests/test_helpers/recovery_modal_presenter_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "recovery_modal_presenter.h"

#include <string>
#include <vector>

// White-box accessor (declared friend in recovery_modal_presenter.h) — avoids
// adding _for_testing() methods to the production class ([L065]/[L088]).
//
// Shared by test_recovery_modal_presenter.cpp and test_ams_error_bridge.cpp so
// the two test files do not each define the friend struct (ODR).
struct RecoveryModalPresenterTestAccess {
    /// The body of the modal's gcode callback. present()'s lambda forwards to
    /// this and does nothing else, so calling it is the button tap.
    static void tap(helix::ui::RecoveryModalPresenter& p, const std::string& gcode) {
        p.on_recovery_tapped(gcode);
    }
    static bool preheating(const helix::ui::RecoveryModalPresenter& p) {
        return p.preheat_timer_ != nullptr;
    }
    /// The user closing the modal with a dismiss-only button. That is exactly
    /// what ActionPromptModal::handle_button_click does for an action whose
    /// gcode is empty (#1172): hide, run no callback — which is why the
    /// presenter cannot see it through set_gcode_callback and has to learn it
    /// from the modal's own on_hide(). Backdrop taps and ESC take the same path.
    static void user_dismiss(helix::ui::RecoveryModalPresenter& p) {
        if (p.modal_) {
            p.modal_->hide();
        }
    }
    /// The fault the user has already answered, which present() refuses to put
    /// back on screen. Empty when there is none.
    static const std::string& handled_detail(const helix::ui::RecoveryModalPresenter& p) {
        return p.handled_detail_;
    }
    /// Shrink the nozzle-reaches-target budget so the give-up path is reachable
    /// without running 300s of LVGL ticks.
    static void set_preheat_budget_ms(helix::ui::RecoveryModalPresenter& p, uint32_t ms) {
        p.preheat_budget_ms_ = ms;
    }
    /// The action set currently wired to the on-screen modal. present() assigns
    /// this only when it does NOT dedup, so it is the observable that
    /// distinguishes "replaced" from "suppressed".
    static const std::vector<helix::RecoveryAction>&
    active_actions(const helix::ui::RecoveryModalPresenter& p) {
        return p.active_actions_;
    }
    /// The detail string currently shown. present() assigns this only when it
    /// does NOT dedup. Reading it after a re-consult lets the bridge test prove
    /// a mid-ERROR detail change re-presented rather than going stale.
    static const std::string& shown_detail(const helix::ui::RecoveryModalPresenter& p) {
        return p.shown_detail_;
    }
};
