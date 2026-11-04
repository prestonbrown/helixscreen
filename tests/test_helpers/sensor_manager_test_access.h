// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sensor_manager.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "hv/json.hpp"

namespace helix::sensors {

namespace detail {
std::vector<SensorManager*>& mutable_sensor_managers();
} // namespace detail

/// Test-only reach into SensorManager (friend of the base).
class SensorManagerTestAccess {
  public:
    /// Replace the entries without PrinterDiscovery, as SensorState does.
    static void reconcile(SensorManager& m, std::vector<SensorEntry> entries) {
        m.reconcile(std::move(entries));
    }

    static std::recursive_mutex& mutex(SensorManager& m) {
        return m.mutex();
    }

    static bool sync_mode(SensorManager& m) {
        std::lock_guard<std::recursive_mutex> lock(m.mutex());
        return m.sync_mode_;
    }

    /// Settings JSON as save_settings() would write it, without touching Config.
    static nlohmann::json settings_json(SensorManager& m) {
        std::lock_guard<std::recursive_mutex> lock(m.mutex());
        return m.settings_json_locked();
    }

    /// The manager's RoleSpec table, in declaration order.
    static const std::vector<RoleSpec>& roles(SensorManager& m) {
        return m.options().roles;
    }

    /// Add a manager to sensor_managers() for one test; pair with remove().
    static void add_to_registry(SensorManager* m) {
        detail::mutable_sensor_managers().push_back(m);
    }

    static void remove_from_registry(SensorManager* m) {
        auto& list = detail::mutable_sensor_managers();
        list.erase(std::remove(list.begin(), list.end(), m), list.end());
    }
};

} // namespace helix::sensors
