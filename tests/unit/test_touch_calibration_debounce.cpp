// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_touch_calibration_debounce.cpp
 * @brief Capture-on-press / commit-on-release debounce suite (issue #943)
 *
 * Capacitive panels (Goodix/Q2) emit multiple press/release cycles per single
 * physical contact. Sampling on every LV_EVENT_PRESSED inflated one tap into
 * several samples, auto-completing a point and cascading the state machine —
 * "one tap jumps to the next screen" with a garbage matrix.
 *
 * The panel captures the press point on on_press(), commits it on on_release() (or
 * a stall-timeout fallback), gated by a release-IMMUNE time refractory. A second
 * commit within kRefractoryMs of the last committed sample is bounce and is
 * dropped. The behavior is ON by default; HELIX_TOUCH_CAL_DEBOUNCE=0 opts out to
 * the legacy sample-on-press path for A/B testing on real hardware.
 *
 * These tests drive the panel directly via on_press / on_release /
 * commit_pending_if_stale with an injected monotonic clock so the refractory and
 * stall windows advance synchronously without spinning a real LVGL timer.
 *
 * The unit tests can only APPROXIMATE real-hardware bounce: they model a contact
 * as discrete press/release pairs at chosen timestamps. On glass, the controller
 * may interleave edges differently, drop a release entirely (covered by the
 * stall-timeout case), or report jittered coordinates. Final validation is on
 * physical Goodix/Q2 panels.
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
 * @brief Debounce fixture with a deterministic injected clock.
 *
 * The debounce gate is forced ON per-instance via the test-access seam (the
 * process-global RuntimeConfig cache is order-dependent across a test binary).
 * `set_now_fn` injects `fake_now_ms_` so refractory/stall windows are advanced
 * by assignment rather than wall-clock.
 */
class DebounceFixture {
  public:
    DebounceFixture() {
        panel_ = std::make_unique<TouchCalibrationPanel>();
        panel_->set_screen_size(800, 480);
        TouchCalibrationPanelTestAccess::set_now_fn(*panel_, [this]() { return fake_now_ms_; });
        TouchCalibrationPanelTestAccess::set_debounce_enabled(*panel_, true);
    }

  protected:
    std::unique_ptr<TouchCalibrationPanel> panel_;
    uint32_t fake_now_ms_ = 0;

    int sample_count() const {
        return panel_->get_progress().current_sample;
    }

    /// Press at the current clock, then release. Commits subject to refractory.
    void tap(Point p) {
        panel_->on_press(p);
        panel_->on_release();
    }

    /// Advance the injected clock.
    void advance_to(uint32_t ms) {
        fake_now_ms_ = ms;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Case 1: Bounce collapse — the repro. ONE physical contact arrives as several
// press/release cycles inside the refractory window; only one sample commits.
// (This is the behavior the TEMP repro asserted; it FAILED against the legacy
// sample-on-press default.)
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(DebounceFixture,
                 "Debounce: one contact's press/release bursts collapse to 1 sample",
                 "[touch][calibration][debounce]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // press@0 / release@5 / press@8 / release@12 / press@15 / release@20 — all
    // within kRefractoryMs of the first commit.
    advance_to(0);
    panel_->on_press(Point{100, 120});
    advance_to(5);
    panel_->on_release(); // commits sample 1 (dt from last_sample anchor is large)
    advance_to(8);
    panel_->on_press(Point{100, 120});
    advance_to(12);
    panel_->on_release(); // bounce — dropped
    advance_to(15);
    panel_->on_press(Point{100, 120});
    advance_to(20);
    panel_->on_release(); // bounce — dropped

    REQUIRE(sample_count() == 1);
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
}

// ---------------------------------------------------------------------------
// Case 2: Deliberate taps — clean, well-spaced taps each commit, filling a
// point and advancing the state machine.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(DebounceFixture, "Debounce: deliberate well-spaced taps fill a point",
                 "[touch][calibration][debounce]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    advance_to(0);
    tap(Point{100, 120});
    advance_to(200);
    tap(Point{100, 120});
    advance_to(400);
    tap(Point{100, 120});

    // 3 samples committed -> POINT_1 captured -> POINT_2.
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);
}

// ---------------------------------------------------------------------------
// Case: noise rejection must reset the sample count BEFORE the failure callback
// runs (#943/#986). The failure callback refreshes the "touch N of 3"
// instruction label; if it observed current_sample still at kSamplesRequired,
// the "(current_sample + 1) of total" math renders "touch 4 of 3".
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(DebounceFixture,
                 "Debounce: noise rejection resets sample count before failure callback",
                 "[touch][calibration][debounce]") {
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    int count_seen_in_failure = -1;
    bool failed = false;
    panel_->set_failure_callback([&](const char*) {
        failed = true;
        count_seen_in_failure = panel_->get_progress().current_sample;
    });

    // Three well-spaced taps (so all commit) at WIDELY different points — the
    // sample spread exceeds kMaxSampleSpread so compute_median_point fails.
    advance_to(0);
    tap(Point{100, 120});
    advance_to(200);
    tap(Point{300, 400});
    advance_to(400);
    tap(Point{250, 150});

    REQUIRE(failed);
    REQUIRE(count_seen_in_failure == 0); // reset BEFORE the callback, not 3
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);
}

// ---------------------------------------------------------------------------
// Case 3: Threshold guard — taps spaced JUST over kRefractoryMs all commit.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(DebounceFixture, "Debounce: taps just over the refractory window all commit",
                 "[touch][calibration][debounce]") {
    panel_->start();

    const uint32_t spacing = 160; // > kRefractoryMs (150)
    REQUIRE(spacing > TouchCalibrationPanelTestAccess::refractory_ms());

    uint32_t t = 0;
    tap(Point{100, 120}); // sample 1 (no prior anchor within window)
    t += spacing;
    advance_to(t);
    tap(Point{100, 120}); // sample 2
    t += spacing;
    advance_to(t);
    tap(Point{100, 120}); // sample 3 -> advances

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);
}

// ---------------------------------------------------------------------------
// Case 4: Flaky-release stall-timeout — a press that never gets a clean release
// still commits via the stall fallback, so a panel that drops releases is not
// uncalibratable.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(DebounceFixture, "Debounce: stall-timeout commits a press with no release",
                 "[touch][calibration][debounce]") {
    panel_->start();

    advance_to(0);
    panel_->on_press(Point{100, 120}); // captured, NO release ever arrives
    REQUIRE(sample_count() == 0);
    REQUIRE(TouchCalibrationPanelTestAccess::press_pending(*panel_) == true);

    // The stall fallback (driven here as the timer would) commits once the press
    // has been outstanding past kStallCommitMs.
    TouchCalibrationPanelTestAccess::commit_pending_if_stale(*panel_, 650);

    REQUIRE(sample_count() == 1);
    REQUIRE(TouchCalibrationPanelTestAccess::press_pending(*panel_) == false);
}

// ---------------------------------------------------------------------------
// Case 5: End-to-end — 3 points x 3 well-spaced taps reaches VERIFY.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(DebounceFixture, "Debounce: full 3-point flow reaches VERIFY",
                 "[touch][calibration][debounce]") {
    panel_->start();

    const Point taps[3] = {{100, 120}, {380, 390}, {660, 60}};
    uint32_t t = 0;
    for (const auto& p : taps) {
        for (int s = 0; s < 3; ++s) {
            advance_to(t);
            tap(p);
            t += 200; // comfortably > kRefractoryMs
        }
    }

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::VERIFY);
    const TouchCalibration* cal = panel_->get_calibration();
    REQUIRE(cal != nullptr);
    REQUIRE(cal->valid == true);
}

// ---------------------------------------------------------------------------
// Case 6: Opt-out — with debounce OFF, on_press samples immediately and the
// bounce burst reproduces the legacy 3-samples-per-contact failure. This
// documents the escape hatch.
//
// NOTE: RuntimeConfig::touch_cal_debounce() caches its env read process-wide,
// so toggling HELIX_TOUCH_CAL_DEBOUNCE per-test is not reliable in-process.
// Instead we force the panel's gate OFF via the test seam (set_debounce_enabled)
// — exactly what the env value would seed in the constructor.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(DebounceFixture, "Debounce OFF: legacy sample-on-press reproduces the burst",
                 "[touch][calibration][debounce]") {
    TouchCalibrationPanelTestAccess::set_debounce_enabled(*panel_, false);
    panel_->start();
    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_1);

    // One physical contact = three PRESSED edges. With debounce OFF each one
    // samples immediately; release is a no-op. Three samples complete POINT_1
    // and cascade to POINT_2, the failure debounce prevents.
    panel_->on_press(Point{100, 120});
    panel_->on_release(); // no-op when debounce disabled
    panel_->on_press(Point{100, 120});
    panel_->on_release();
    panel_->on_press(Point{100, 120});
    panel_->on_release();

    REQUIRE(panel_->get_state() == TouchCalibrationPanel::State::POINT_2);
}
