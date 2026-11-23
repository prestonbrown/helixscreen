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
    static void abort_clicked(PIDCalibrationPanel& p) {
        p.handle_abort_clicked();
    }
    static void start_clicked(PIDCalibrationPanel& p) {
        p.handle_start_clicked();
    }
    static void measure_fan_row_clicked(PIDCalibrationPanel& p) {
        p.handle_measure_fan_row_clicked();
    }
    static void switch_mpc_clicked(PIDCalibrationPanel& p) {
        p.handle_switch_mpc_clicked();
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
    static void power_pick(PIDCalibrationPanel& p, int pick) {
        p.handle_power_pick(pick);
    }
    static void turn_off_fan(PIDCalibrationPanel& p) {
        p.turn_off_fan();
    }
    /// The confirmed migration, as confirm_migration() runs it.
    static void start_mpc_migration(PIDCalibrationPanel& p) {
        p.set_state(PIDCalibrationPanel::State::MIGRATING);
        p.start_migration(PIDCalibrationPanel::CalibMethod::MPC, /*adding_fan=*/false);
    }
    static helix::system::KlipperConfigEditor& config_editor(PIDCalibrationPanel& p) {
        return p.config_editor_;
    }
    static void set_migration_restart_timeout_ms(PIDCalibrationPanel& p, int ms) {
        p.migration_restart_timeout_ms_ = ms;
    }
    static std::string error_message(PIDCalibrationPanel& p) {
        return lv_subject_get_string(p.subj_error_message_.get());
    }
    /// The control-type query on_activate() runs, with the method preselected.
    static void detect_control_type(PIDCalibrationPanel& p) {
        p.detect_heater_control_type(true);
    }
    static std::vector<helix::system::ConfigEdit> mpc_migration_edits(PIDCalibrationPanel& p) {
        return p.control_migration_edits(PIDCalibrationPanel::CalibMethod::MPC);
    }
};

} // namespace ui
} // namespace helix
