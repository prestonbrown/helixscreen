// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_calibration_pid.h"

namespace helix {
namespace ui {

// Test-only reach into PIDCalibrationPanel's private send_save_config(), so
// the expected-restart flow tests can drive the real SAVE_CONFIG initiation
// without the panel's state machine and XML UI. Follows the existing
// TestAccess pattern (tests/test_helpers/, [L088]).
struct PIDCalibrationPanelTestAccess {
    static void send_save_config(PIDCalibrationPanel& p) {
        p.send_save_config();
    }
    static void start_clicked(PIDCalibrationPanel& p) {
        p.handle_start_clicked();
    }
    static void activate(PIDCalibrationPanel& p) {
        p.on_activate();
    }
    static void head_clicked(PIDCalibrationPanel& p, int head) {
        p.handle_head_clicked(head);
    }
    static void bed_clicked(PIDCalibrationPanel& p) {
        p.handle_heater_bed_clicked();
    }
    static void set_fan_speed(PIDCalibrationPanel& p, int percent) {
        p.fan_speed_ = percent;
    }
    static void use_mpc(PIDCalibrationPanel& p) {
        p.selected_method_ = PIDCalibrationPanel::CalibMethod::MPC;
    }
    static void turn_off_fan(PIDCalibrationPanel& p) {
        p.turn_off_fan();
    }
};

} // namespace ui
} // namespace helix
