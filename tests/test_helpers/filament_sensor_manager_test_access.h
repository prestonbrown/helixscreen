// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_sensor_manager.h"
#include "json_fwd.h"
#include "printer_discovery.h"
#include "sensor_manager_test_access.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix {

// Friend of FilamentSensorManager: returns the singleton to a clean, synchronous
// state with the startup grace period already expired.
class FilamentSensorManagerTestAccess {
  public:
    static void reset(FilamentSensorManager& mgr) {
        // Enable sync mode for testing (avoids lv_async_call)
        mgr.set_sync_mode(true);

        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());

        // Clear all sensors and states
        forget_sensors(mgr);

        // Reset master enabled
        mgr.master_enabled_ = true;

        // Clear callback
        mgr.state_change_callback_ = nullptr;

        // Reset initial status tracking (ensures first update_from_status triggers subjects)
        mgr.initial_status_received_ = false;

        // Reset startup time to 10 seconds in the past so the stabilization grace
        // period is already expired in tests (avoids flaky timing-dependent failures)
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);

        // Reset subjects if initialized
        if (mgr.subjects_initialized()) {
            lv_subject_set_int(&mgr.runout_detected_, -1);
            lv_subject_set_int(&mgr.toolhead_detected_, -1);
            lv_subject_set_int(&mgr.entry_detected_, -1);
            lv_subject_set_int(&mgr.any_runout_, 0);
            lv_subject_set_int(&mgr.motion_signature_, 0);
            lv_subject_set_int(&mgr.master_enabled_subject_, 1);
            lv_subject_set_int(mgr.count_subject(), 0);
        }
    }

    /// Forget every discovered sensor and turn the master switch back on,
    /// touching no subject's observer list, so it is safe from any fixture.
    static void clear_sensors(FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        forget_sensors(mgr);
        mgr.master_enabled_ = true;
        if (mgr.subjects_initialized()) {
            lv_subject_set_int(mgr.count_subject(), 0);
        }
    }

    [[nodiscard]] static bool initial_status_received(FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        return mgr.initial_status_received_;
    }

    static void clear_startup_grace_period(FilamentSensorManager& mgr) {
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }

    /// Entries, readings, lanes and the known-name set. Caller holds the mutex.
    static void forget_sensors(FilamentSensorManager& mgr) {
        mgr.entries_locked().clear();
        mgr.readings_.clear();
        mgr.lanes_.clear();
        mgr.known_.clear();
    }

    /// Whether any sensor is currently held armed by bypass.
    [[nodiscard]] static bool has_bypass_armed_sensors(FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        return !mgr.bypass_armed_.empty();
    }

    /// Reading of the first holder of @p role, whatever its enabled flags.
    [[nodiscard]] static std::optional<FilamentSensorState> role_state(FilamentSensorManager& mgr,
                                                                       FilamentSensorRole role) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        if (role == FilamentSensorRole::NONE) {
            return std::nullopt;
        }
        for (const auto& entry : mgr.entries_locked()) {
            if (entry.role == static_cast<uint8_t>(role)) {
                const auto* reading = mgr.state(entry.klipper_name);
                return reading ? std::optional(*reading) : std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static lv_subject_t* master_enabled_subject(FilamentSensorManager& mgr) {
        return &mgr.master_enabled_subject_;
    }

    [[nodiscard]] static lv_subject_t* scoped_runout_subject(FilamentSensorManager& mgr) {
        return &mgr.scoped_runout_;
    }
};

/// Discover @p objects as printer.objects.list reports them.
inline void discover_filament_sensors(FilamentSensorManager& mgr,
                                      const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json(objects));
    mgr.discover(hw);
}

} // namespace helix
