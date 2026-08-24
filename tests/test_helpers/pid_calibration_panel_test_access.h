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
};

} // namespace ui
} // namespace helix
