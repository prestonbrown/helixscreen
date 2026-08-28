// tests/test_helpers/zoffset_calibration_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_calibration_zoffset.h"

// Friend-class shim for ZOffsetCalibrationPanel (declared as
// `friend class ZOffsetCalibrationTestAccess;` in the panel header). The
// calibration gcode sends are private and normally reached only through XML
// event callbacks on a fully created overlay; these accessors drive the two
// that need no widget tree or state-machine transition, so a test can assert
// on what actually went out over the wire.
class ZOffsetCalibrationTestAccess {
  public:
    /// Sends the strategy-dependent Z nudge (TESTZ, or G91/G1/G90 under
    /// FIRMWARE_MANAGED). Reads the strategy from the process-wide PrinterState.
    static void adjust_z(ZOffsetCalibrationPanel& panel, float delta) {
        panel.adjust_z(delta);
    }

    /// Arms the "we warmed the bed" latch so turn_off_bed() actually sends.
    static void mark_bed_warmed(ZOffsetCalibrationPanel& panel) {
        panel.bed_was_warmed_ = true;
    }

    /// Sends M140 S0 when the latch above is armed.
    static void turn_off_bed(ZOffsetCalibrationPanel& panel) {
        panel.turn_off_bed_if_needed();
    }

    /// Seeds the accumulated jog distance so send_accept() has a value to
    /// persist, without driving the deferred G1-success bookkeeping.
    static void set_cumulative_delta(ZOffsetCalibrationPanel& panel, float delta) {
        panel.cumulative_z_delta_ = delta;
    }

    /// Fires the accept button's send path. Transitions the panel to SAVING and
    /// back via the mock's synchronous script handler; safe on a bare panel
    /// (widget pointers are null-guarded).
    static void send_accept(ZOffsetCalibrationPanel& panel) {
        panel.send_accept();
    }
};
