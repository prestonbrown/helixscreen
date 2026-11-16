// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_touch_calibration_reset.cpp
 * @brief Per-session reset of TouchCalibrationPanel (issue #943)
 *
 * The Settings-launched calibration overlay reuses a persistent singleton
 * TouchCalibrationPanel across invocations. The first-run wizard builds a
 * fresh panel each time, so it always starts clean. The Settings path did not
 * reset per-session state on re-show, so the SECOND time a user opened
 * Settings -> Touch Calibration, stale state (an armed press-debounce gate, a
 * half-filled sample buffer, a non-IDLE state-machine position) made the
 * calibration misbehave.
 *
 * TouchCalibrationPanel::reset() returns ALL per-session mutable state to its
 * fresh-constructed baseline. The overlay's show() calls it so every show
 * starts clean — exactly like a freshly built wizard panel.
 *
 * These tests drive a panel into a dirty mid-session state, call reset(), and
 * assert the panel is back to a clean start.
 */

#include "../test_helpers/touch_calibration_panel_test_access.h"
#include "touch_calibration.h"
#include "touch_calibration_panel.h"

#include <cstdint>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/**
 * @brief Fixture providing a panel with a deterministic injected clock and a
 *        controllable debounce gate (so we exercise the stale-gate path).
 */
class TouchCalibrationResetFixture {
  public:
    TouchCalibrationResetFixture() {
        panel_ = std::make_unique<TouchCalibrationPanel>();
        panel_->set_screen_size(800, 480);
        TouchCalibrationPanelTestAccess::set_now_fn(*panel_, [this]() { return fake_now_ms_; });
        TouchCalibrationPanelTestAccess::set_debounce_enabled(*panel_, true);
    }

  protected:
    std::unique_ptr<TouchCalibrationPanel> panel_;
    uint32_t fake_now_ms_ = 0;

    int current_sample_count() const {
        return panel_->get_progress().current_sample;
    }
    bool press_pending() const {
        return TouchCalibrationPanelTestAccess::press_pending(*panel_);
    }

    /// Advance the clock past the refractory window so the next commit lands.
    void clear_refractory() {
        fake_now_ms_ += TouchCalibrationPanelTestAccess::refractory_ms() + 1;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// reset() from a half-captured point with the debounce gate armed
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(TouchCalibrationResetFixture,
                 "TouchCalibrationPanel::reset clears mid-point session state",
                 "[touch][calibration][reset]") {
    // Drive the panel into a genuine mid-session state: POINT_1 with one sample
    // committed and a press captured-but-not-yet-committed.
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // One press/release commits sample 1 for POINT_1.
    panel_->on_press(Point{100, 120});
    panel_->on_release();
    REQUIRE(current_sample_count() == 1);

    // A second press is captured but left uncommitted (no release yet) — exactly
    // the stale pending-press state that would carry into the next Settings show.
    clear_refractory();
    panel_->on_press(Point{100, 120});
    REQUIRE(press_pending());
    REQUIRE(current_sample_count() == 1);

    // Sanity: we are genuinely mid-session.
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    panel_->reset();

    // Back to a clean start: IDLE, no buffered samples, no pending press.
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
    REQUIRE(current_sample_count() == 0);
    REQUIRE_FALSE(press_pending());
}

// ---------------------------------------------------------------------------
// reset() from VERIFY (a completed-but-not-accepted session)
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(TouchCalibrationResetFixture,
                 "TouchCalibrationPanel::reset returns to clean start from VERIFY",
                 "[touch][calibration][reset]") {
    // Drive a full 3-point calibration to VERIFY via clean press/release taps,
    // spacing them past the refractory window so each commits.
    panel_->start();
    const Point taps[3] = {{100, 120}, {380, 390}, {660, 60}};
    for (const auto& tap : taps) {
        for (int s = 0; s < panel_->get_progress().total_samples; ++s) {
            clear_refractory();
            panel_->on_press(tap);
            panel_->on_release();
        }
    }
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);
    REQUIRE(panel_->get_calibration() != nullptr);

    panel_->reset();

    // Fresh start: IDLE, no calibration exposed, no buffered samples, no pending.
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::IDLE);
    REQUIRE(panel_->get_calibration() == nullptr);
    REQUIRE(current_sample_count() == 0);
    REQUIRE_FALSE(press_pending());

    // And the panel is fully usable again after reset (the whole point of #943):
    // a brand-new run reaches POINT_1 and accepts a fresh sample.
    panel_->on_press(Point{100, 120}); // IDLE -> POINT_1 auto-start (no sample yet)
    panel_->on_release();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
    REQUIRE(current_sample_count() == 0);
    clear_refractory();
    panel_->on_press(Point{100, 120});
    panel_->on_release();
    REQUIRE(current_sample_count() == 1);
}

// ---------------------------------------------------------------------------
// reset() does not fire the completion callback (unlike cancel())
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(TouchCalibrationResetFixture,
                 "TouchCalibrationPanel::reset does not invoke completion callback",
                 "[touch][calibration][reset]") {
    bool callback_fired = false;
    panel_->set_completion_callback([&](const TouchCalibration*) { callback_fired = true; });

    panel_->start();
    panel_->on_press(Point{100, 120});
    panel_->on_release();

    panel_->reset();

    // reset() is a silent fresh-start; it must NOT report a cancellation the way
    // cancel() does (the overlay show() path has no completion to report yet).
    REQUIRE_FALSE(callback_fired);
}
