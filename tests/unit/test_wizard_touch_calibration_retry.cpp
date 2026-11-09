// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

/**
 * @file test_wizard_touch_calibration_retry.cpp
 * @brief Regression test for the wizard-retry affine asymmetry (issue #943)
 *
 * The first-run wizard auto-accepts calibration, so Retry runs cancel()+start().
 * Retry must revert the just-applied affine; otherwise the re-capture runs with
 * the new (possibly bad) transform still ACTIVE, a feedback loop that makes
 * recalibration produce garbage. The wizard does what the Settings overlay retry
 * does (session_.revert_for_retry()).
 *
 * This drives the wizard's REAL handle_retry_clicked() against an injected
 * calibration sink (no live DisplayManager needed) and asserts the affine is
 * disabled and the pre-session backup restored before the re-capture starts.
 */

#include "ui_wizard_touch_calibration.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/wizard_touch_calibration_test_access.h"
#include "touch_calibration_session.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix;

namespace {

// Models the device's stored calibration + affine-enabled state the way the real
// backend does: disable_affine() leaves the stored calibration intact but off;
// apply_calibration()/enable_affine() reactivate it. (Mirrors the FakeSink in
// test_touch_calibration_session.cpp.)
struct FakeSink : ICalibrationSink {
    TouchCalibration stored{};
    bool affine_enabled = true;
    std::vector<std::string> ops;

    static TouchCalibration make(float a, bool valid = true) {
        TouchCalibration c{};
        c.a = a;
        c.e = a;
        c.valid = valid;
        return c;
    }

    TouchCalibration current_calibration() const override {
        return stored;
    }
    bool apply_calibration(const TouchCalibration& cal) override {
        if (!cal.valid) {
            ops.push_back("apply:rejected");
            return false;
        }
        stored = cal;
        affine_enabled = true;
        ops.push_back("apply");
        return true;
    }
    void clear_calibration() override {
        stored = TouchCalibration{};
        ops.push_back("clear");
    }
    void disable_affine() override {
        affine_enabled = false;
        ops.push_back("disable");
    }
    void enable_affine() override {
        affine_enabled = true;
        ops.push_back("enable");
    }
};

} // namespace

// The LVGLUITestFixture stands up LVGL + the wizard container subjects
// (wizard_subtitle / wizard_show_skip) that handle_retry_clicked() writes to.
TEST_CASE_METHOD(LVGLUITestFixture, "Wizard retry reverts the affine before re-capture (#943)",
                 "[wizard][touch][calibration][retry][943]") {
    WizardTouchCalibrationStep step;
    step.init_subjects(); // registers the step's own current_step_/calibration_valid_

    FakeSink sink;
    TouchCalibration original = FakeSink::make(0.5f);
    sink.stored = original;

    // Route the wizard's session at this sink and open a capture session, exactly
    // as create() does on the real device (snapshot backup + disable affine).
    WizardTouchCalibrationTestAccess::set_calibration_sink(step, &sink);
    WizardTouchCalibrationTestAccess::session(step).begin_capture(sink);
    REQUIRE(sink.affine_enabled == false);

    // A freshly computed (bad) calibration is accepted and becomes active — this
    // is the state the wizard is in when the user taps Retry after auto-accept.
    sink.apply_calibration(FakeSink::make(0.9f));
    REQUIRE(sink.affine_enabled == true);

    // Drive the real Retry handler.
    WizardTouchCalibrationTestAccess::invoke_retry(step);

    // Retry must revert to the pre-session calibration and disable the
    // affine so the re-capture reads raw coords (no feedback loop). Without the
    // revert this fails — the bad affine would still be active for the re-run.
    REQUIRE(sink.affine_enabled == false);
    REQUIRE(sink.stored.a == Approx(original.a));

    // Backup is retained across retry (the user may retry again).
    REQUIRE(WizardTouchCalibrationTestAccess::session(step).has_backup() == true);
}

// Guards that the revert is wired to the session, not a no-op: with no backup
// open, retry must not crash and must leave the sink's affine disabled (the
// re-capture always begins from the disabled state).
TEST_CASE_METHOD(LVGLUITestFixture,
                 "Wizard retry without an open session still disables affine for capture (#943)",
                 "[wizard][touch][calibration][retry][943]") {
    WizardTouchCalibrationStep step;
    step.init_subjects();

    FakeSink sink;
    sink.stored = FakeSink::make(0.5f);
    WizardTouchCalibrationTestAccess::set_calibration_sink(step, &sink);

    // No begin_capture(): revert_for_retry has no backup to restore, but must
    // still leave the affine disabled for the fresh capture.
    WizardTouchCalibrationTestAccess::invoke_retry(step);

    REQUIRE(sink.affine_enabled == false);
}
