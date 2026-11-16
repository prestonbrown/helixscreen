// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_wizard_printer_identify.h"

/**
 * @brief Reaches the printer-identify step's printer-change tracking.
 *
 * The step detects once per printer: init_subjects() re-runs detection only
 * while its subjects are uninitialized or when the connected printer's URL
 * differs from the one it last detected. Tests have no Moonraker client, so
 * the URL never changes on its own. Pointing the remembered URL elsewhere
 * makes the next init_subjects() take the production printer-change path:
 * caches dropped, saved name and type cleared, detection run again.
 *
 * Lives in the global namespace to match WizardPrinterIdentifyStep, so the
 * `friend class WizardPrinterIdentifyStepTestAccess;` declaration resolves here.
 */
class WizardPrinterIdentifyStepTestAccess {
  public:
    /// Make the next init_subjects() treat the connection as a different
    /// printer. Also forgets the detected kinematics, which only a connected
    /// printer sets, so a later run without one lists every machine again.
    /// With @p same_printer the step detects again for the printer it already
    /// knows, the way its first open in a session does: the saved name and
    /// type stay in the config.
    static void forget_printer(WizardPrinterIdentifyStep& step, bool same_printer = false) {
        if (same_printer) {
            step.subjects_initialized_ = false;
            step.last_detected_url_.clear();
        } else {
            step.last_detected_url_ = "ws://previous-printer.invalid:7125";
        }
        step.detected_kinematics_.clear();
    }
};
