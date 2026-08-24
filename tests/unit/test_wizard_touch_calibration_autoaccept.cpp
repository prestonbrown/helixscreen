// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_touch_calibration_autoaccept.cpp
 * @brief Regression test for the wizard auto-accept hang (issue #1029)
 *
 * The first-run wizard has no interactive verify step — it must auto-accept the
 * computed calibration the instant the panel enters VERIFY. The auto-accept used
 * to live ONLY in the press handler (handle_screen_touched). After the #943
 * debounce moved the POINT_3->VERIFY commit off the press edge to on_release()
 * (or the 600ms stall timer), that press-handler check no longer ran on clean
 * capacitive panels (Goodix/Q2) — the wizard reached VERIFY, showed
 * "Computing calibration...", and hung there forever because accept() was never
 * called.
 *
 * The fix wires the wizard's auto-accept to the panel's verify-entry hook
 * (set_verify_entry_callback), which fires on EVERY commit path. These tests
 * drive the wizard's own TouchCalibrationPanel through a full 3-point capture via
 * the release path and the stall path, and assert the panel reaches COMPLETE —
 * proving auto-accept fired. accept() sets state_=COMPLETE before invoking the
 * (screen_root_-guarded) completion callback, so this needs no LVGL screen.
 *
 * The three calibration touch points (100,120)/(380,390)/(660,60) are the same
 * non-degenerate triangle the panel suite uses; they pass compute + validation
 * on an 800x480 target.
 */

#include "ui_wizard_touch_calibration.h"

#include "../test_helpers/touch_calibration_panel_test_access.h"
#include "../test_helpers/wizard_touch_calibration_test_access.h"
#include "touch_calibration_panel.h"

#include <cstdint>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Non-degenerate triangle that passes compute_calibration + validation at 800x480.
constexpr Point POINT1{100, 120};
constexpr Point POINT2{380, 390};
constexpr Point POINT3{660, 60};

/**
 * @brief Drives the wizard's private panel with a deterministic injected clock.
 *
 * Debounce is forced ON per-instance (the RuntimeConfig cache is process-global
 * and order-dependent across the binary). The clock is advanced past the 150ms
 * release-immune refractory before every tap so each sample commits instead of
 * being dropped as bounce.
 */
class WizardAutoAcceptFixture {
  public:
    WizardAutoAcceptFixture() {
        panel_ = WizardTouchCalibrationTestAccess::panel(step_);
        REQUIRE(panel_ != nullptr);
        panel_->set_screen_size(800, 480);
        TouchCalibrationPanelTestAccess::set_now_fn(*panel_, [this]() { return now_ms_; });
        TouchCalibrationPanelTestAccess::set_debounce_enabled(*panel_, true);
        panel_->start(); // IDLE -> POINT_1
    }

  protected:
    WizardTouchCalibrationStep step_;
    TouchCalibrationPanel* panel_ = nullptr;
    uint32_t now_ms_ = 0;

    // One clean contact past the refractory window: press then release.
    void tap(Point p) {
        now_ms_ += 200; // clear the 150ms refractory so the sample commits
        panel_->on_press(p);
        panel_->on_release();
    }

    // SAMPLES_REQUIRED clean taps to fully capture one calibration point.
    void capture_point_via_release(Point p) {
        for (int i = 0; i < TouchCalibrationPanelTestAccess::samples_required(); ++i) {
            tap(p);
        }
    }

    // Press with no release; commit via the stall-timeout fallback (models a
    // capacitive panel that drops the RELEASED edge entirely).
    void tap_via_stall(Point p) {
        now_ms_ += 200; // clear the refractory
        panel_->on_press(p);
        now_ms_ += TouchCalibrationPanelTestAccess::stall_commit_ms();
        TouchCalibrationPanelTestAccess::commit_pending_if_stale(*panel_, now_ms_);
    }

    TouchCalibrationPanel::State state() const {
        return panel_->get_state();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The repro: all three points commit on RELEASE (debounce default). Before the
// fix the panel stuck in VERIFY ("Computing calibration..." forever). With the
// verify-entry auto-accept wired, it advances to COMPLETE.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(WizardAutoAcceptFixture,
                 "Wizard auto-accept: 3 points via release reach COMPLETE (#1029)",
                 "[wizard][touch-calibration][autoaccept]") {
    capture_point_via_release(POINT1); // POINT_1 -> POINT_2
    capture_point_via_release(POINT2); // POINT_2 -> POINT_3
    capture_point_via_release(POINT3); // POINT_3 -> VERIFY -> (auto-accept) COMPLETE

    REQUIRE(state() == TouchCalibrationPanel::State::COMPLETE);
}

// ---------------------------------------------------------------------------
// The decisive POINT_3->VERIFY commit arrives via the 600ms stall timer (the
// panel never delivered a clean RELEASED). The press-handler-only check could
// never have caught this path; the verify-entry hook does.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(WizardAutoAcceptFixture,
                 "Wizard auto-accept: final commit via stall timer reaches COMPLETE (#1029)",
                 "[wizard][touch-calibration][autoaccept]") {
    capture_point_via_release(POINT1);
    capture_point_via_release(POINT2);

    // First two samples of POINT_3 on release; the last on the stall fallback.
    tap(POINT3);
    tap(POINT3);
    tap_via_stall(POINT3); // commits the 3rd sample -> VERIFY -> auto-accept

    REQUIRE(state() == TouchCalibrationPanel::State::COMPLETE);
}

// ---------------------------------------------------------------------------
// Sanity guard: the panel must NOT auto-complete before all three points are in.
// (Pins that the auto-accept fires on the real POINT_3->VERIFY transition, not
// some earlier edge.)
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(WizardAutoAcceptFixture,
                 "Wizard auto-accept: stays mid-capture until POINT_3 commits (#1029)",
                 "[wizard][touch-calibration][autoaccept]") {
    capture_point_via_release(POINT1);
    capture_point_via_release(POINT2);

    REQUIRE(state() == TouchCalibrationPanel::State::POINT_3);
}
