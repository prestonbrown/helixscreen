// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_overlay_timelapse_install.h"

/// Private wizard-position access for TimelapseInstallOverlay tests.
struct TimelapseInstallOverlayTestAccess {
    /// Park the wizard on its restart step, as a run that got that far leaves it.
    static void park_at_restart(TimelapseInstallOverlay& o) {
        o.current_step_ = TimelapseInstallOverlay::Step::RESTART_MOONRAKER;
    }
    static bool at_restart(const TimelapseInstallOverlay& o) {
        return o.current_step_ == TimelapseInstallOverlay::Step::RESTART_MOONRAKER;
    }
    static bool wizard_active(const TimelapseInstallOverlay& o) {
        return o.wizard_active_;
    }
};
