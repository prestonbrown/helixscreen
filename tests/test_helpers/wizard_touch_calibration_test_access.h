// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_wizard_touch_calibration.h"

#include "touch_calibration_panel.h"

/**
 * @brief Test-only access to WizardTouchCalibrationStep internals.
 *
 * The wizard owns a private TouchCalibrationPanel. Driving that panel directly
 * (via helix::TouchCalibrationPanelTestAccess) is the only way to exercise the
 * wizard's verify-entry auto-accept wiring (#1029) without standing up the full
 * LVGL/XML screen — the panel reaching COMPLETE proves the wizard wired
 * set_verify_entry_callback to accept() on the real commit path.
 *
 * Lives in the global namespace to match WizardTouchCalibrationStep (which is
 * not namespaced), so the `friend class WizardTouchCalibrationTestAccess;`
 * declaration in the wizard header resolves to this class.
 */
class WizardTouchCalibrationTestAccess {
  public:
    static helix::TouchCalibrationPanel* panel(WizardTouchCalibrationStep& step) {
        return step.controller_.panel();
    }

    // The wizard's calibration session — begin_capture/revert_for_retry/restore.
    static helix::TouchCalibrationSession& session(WizardTouchCalibrationStep& step) {
        return step.controller_.session();
    }

    // Inject the calibration sink every phase drives, so the session paths can be
    // exercised without a live DisplayManager (#943).
    static void set_calibration_sink(WizardTouchCalibrationStep& step,
                                     helix::ICalibrationSink* sink) {
        step.controller_.set_sink_override(sink);
    }

    // Drive the completion handler the panel's callback reaches once create() has
    // built the screen; `root` stands in for that screen.
    static void invoke_calibration_complete(WizardTouchCalibrationStep& step,
                                            const helix::TouchCalibration* cal, lv_obj_t* root) {
        step.screen_root_ = root;
        step.on_calibration_complete(cal);
        step.screen_root_ = nullptr;
    }

    // Drive the real Retry handler (private in production).
    static void invoke_retry(WizardTouchCalibrationStep& step) {
        step.handle_retry_clicked();
    }
};
