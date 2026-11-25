// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_sensor_manager.h"

#include <chrono>
#include <mutex>

namespace helix {

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
        mgr.set_sync_mode(true);
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        mgr.entries_locked().clear();
        mgr.readings_.clear();
        mgr.lanes_.clear();
        mgr.known_.clear();
        mgr.bypass_armed_.clear();
        mgr.idle_runout_acknowledged_.clear();
        mgr.master_enabled_ = true;
        mgr.initial_status_received_ = false;
        mgr.pending_removal_toast_.clear();
        clear_startup_grace(mgr);
    }

    /// Backdate every outstanding removal dwell, so a test can reach the far side
    /// of helix::kRunoutToastDwell without waiting it out.
    static void age_removal_dwell(helix::FilamentSensorManager& mgr,
                                  std::chrono::steady_clock::duration by) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        for (auto& [klipper_name, since] : mgr.pending_removal_toast_) {
            (void)klipper_name;
            since -= by;
        }
    }

    /// Is a removal toast being held? Distinguishes "the dwell swallowed it" from
    /// "some other gate suppressed it", which otherwise look identical from the
    /// toast side.
    [[nodiscard]] static bool removal_dwell_pending(helix::FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        return !mgr.pending_removal_toast_.empty();
    }

    /// discover() re-anchors the grace to "Moonraker just connected", so
    /// this has to run AFTER the sensors are installed, not before.
    static void clear_startup_grace(helix::FilamentSensorManager& mgr) {
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::minutes(1);
    }

    /// Set a sensor's role directly, bypassing set_role()'s
    /// single-RUNOUT exclusivity. Production reaches a multi-RUNOUT state via
    /// load_config (settings.json restore), which writes sensor->role per
    /// entry with no exclusivity check.
    static void force_role(helix::FilamentSensorManager& mgr, const std::string& klipper,
                           helix::FilamentSensorRole role) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        for (auto& s : mgr.entries_locked()) {
            if (s.klipper_name == klipper) {
                s.role = static_cast<uint8_t>(role);
                s.enabled = true;
                return;
            }
        }
    }
};
} // namespace helix
