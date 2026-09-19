// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_sensor_manager.h"

#include <chrono>
#include <mutex>

// Friend access to FilamentSensorManager (declared as a friend in its header;
// the manager lives in namespace helix but the friend is a global-namespace
// class, matching the RunoutScopeTestAccess / BypassArmingTestAccess shims).
//
// The manager is a process singleton, so any test asserting on has_real_runout()
// or on the "Filament removed" toast has to start from a known sensor set with
// the startup grace already expired — otherwise whichever test ran first in the
// shard decides the answer. Defined in ONE header so the two translation units
// that need it cannot each define their own and violate the ODR.
class PostUnloadGraceTestAccess {
  public:
    static void reset(helix::FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        mgr.sensors_.clear();
        mgr.states_.clear();
        mgr.bypass_armed_.clear();
        mgr.master_enabled_ = true;
        mgr.sync_mode_ = true;
        mgr.initial_status_received_ = false;
        mgr.pending_removal_toast_.clear();
        clear_startup_grace(mgr);
    }

    /// Backdate every outstanding removal dwell, so a test can reach the far side
    /// of AppConstants::Ams::RUNOUT_TOAST_DWELL without waiting it out.
    static void age_removal_dwell(helix::FilamentSensorManager& mgr,
                                  std::chrono::steady_clock::duration by) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        for (auto& [klipper_name, since] : mgr.pending_removal_toast_) {
            (void)klipper_name;
            since -= by;
        }
    }

    /// Is a removal toast being held? Distinguishes "the dwell swallowed it" from
    /// "some other gate suppressed it", which otherwise look identical from the
    /// toast side.
    [[nodiscard]] static bool removal_dwell_pending(helix::FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        return !mgr.pending_removal_toast_.empty();
    }

    /// discover_sensors() re-anchors the grace to "Moonraker just connected", so
    /// this has to run AFTER the sensors are installed, not before.
    static void clear_startup_grace(helix::FilamentSensorManager& mgr) {
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::minutes(1);
    }
};
