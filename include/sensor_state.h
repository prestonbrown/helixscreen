// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"
#include "static_subject_registry.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

namespace helix {

/**
 * @brief Metadata about a discovered Moonraker sensor
 */
struct SensorInfo {
    std::string id;                      ///< Sensor identifier (e.g. "ina219_power")
    std::string friendly_name;           ///< Human-readable name from Moonraker
    std::string type;                    ///< Sensor type (e.g. "INA219")
    std::vector<std::string> value_keys; ///< Value keys reported (e.g. "power", "voltage")
};

/**
 * @brief Singleton tracking Moonraker server.sensors state
 *
 * Subscribes to notify_sensor_update WebSocket events and maintains
 * per-sensor, per-value LVGL subjects encoded as centi-units:
 *   - power:   value * 100          (centi-watts)
 *   - voltage: value * 100          (centi-volts)
 *   - current: value * 100000       (centi-milliamps for mA precision)
 *   - energy:  value * 100          (centi-kilowatt-hours)
 *
 * Uses SubjectLifetime pattern for dynamic subject safety since sensors
 * can be rediscovered on reconnection.
 */
class SensorState {
  public:
    static SensorState& instance();

    /// Register for notify_sensor_update WebSocket events
    void subscribe(IMoonrakerAPI& api);

    /// Unregister callback and clean up subjects
    void unsubscribe(IMoonrakerAPI& api);

    /// Set discovered sensors (called from discovery sequence)
    void set_sensors(const std::vector<SensorInfo>& sensors);

    /// Set discovered sensors with initial values applied atomically
    void set_sensors(const std::vector<SensorInfo>& sensors, const nlohmann::json& initial_values);

    /// Get value subject for a sensor+key pair. Returns nullptr if not found.
    lv_subject_t* get_value_subject(const std::string& sensor_id, const std::string& key,
                                    SubjectLifetime& lt);

    /// Get list of all tracked sensor IDs
    std::vector<std::string> sensor_ids() const;

    /// Get sensor info by ID. Returns nullptr if not found.
    const SensorInfo* get_sensor_info(const std::string& sensor_id) const;

    /// Get list of sensor IDs that are energy sensors
    std::vector<std::string> energy_sensor_ids() const;

    /// Check if a sensor is an energy sensor (has power/voltage/current/energy keys)
    static bool is_energy_sensor(const SensorInfo& info);

    /// Convert a raw float value to centi-unit integer encoding
    static int to_centi_units(const std::string& key, double value);

    /// Format a centi-unit value to a human-readable string with units
    static std::string format_value(const std::string& key, int centi_value);

    /// Clean up all LVGL subjects (called by StaticSubjectRegistry)
    void deinit_subjects();

  private:
    SensorState() = default;
    ~SensorState() = default;

    SensorState(const SensorState&) = delete;
    SensorState& operator=(const SensorState&) = delete;

    void on_sensor_update(const nlohmann::json& msg);

    /**
     * @brief Wraps an lv_subject_t with lifecycle management.
     *
     * Values stored as centi-units (value * 100, or * 100000 for current).
     */
    struct DynamicIntSubject {
        lv_subject_t subject{};
        bool initialized = false;
        SubjectLifetime lifetime; ///< Alive token for ObserverGuard safety

        ~DynamicIntSubject() {
            lifetime.reset();
            if (initialized && lv_is_initialized()) {
                lv_subject_deinit(&subject);
            }
            initialized = false;
        }
    };

    struct SensorEntry {
        SensorInfo info;
        /// One DynamicIntSubject per value key (e.g. "power", "voltage")
        std::unordered_map<std::string, std::unique_ptr<DynamicIntSubject>> value_subjects;
    };

    mutable std::recursive_mutex mutex_;
    std::unordered_map<std::string, SensorEntry> sensors_;
    bool subjects_initialized_ = false;
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix
