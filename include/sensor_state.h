// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dynamic_subject_map.h"
#include "i_moonraker_api.h"
#include "json_fwd.h"
#include "sensor_manager.h"

#include <lvgl/lvgl.h>

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
 * @brief Moonraker server.sensors state: power and energy meters.
 *
 * Fed by set_sensors() (discovery) and notify_sensor_update WebSocket events,
 * not by PrinterDiscovery. Keeps one int subject per (sensor, value key),
 * encoded as centi-units:
 *   - power:   value * 100          (centi-watts)
 *   - voltage: value * 100          (centi-volts)
 *   - current: value * 100000       (centi-milliamps for mA precision)
 *   - energy:  value * 100          (centi-kilowatt-hours)
 *
 * Subjects live in a DynamicSubjectMap and are rebuilt on every rediscovery, so
 * observers use the lifetime from get_value_subject(). No roles, no persisted
 * settings, no Settings rows.
 */
class SensorState : public sensors::SensorManager {
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

    /// A copy of the sensor's info, taken under the lock; empty if not found.
    std::optional<SensorInfo> get_sensor_info(const std::string& sensor_id) const;

    /// Get list of sensor IDs that are energy sensors
    std::vector<std::string> energy_sensor_ids() const;

    /// Check if a sensor is an energy sensor (has power/voltage/current/energy keys)
    static bool is_energy_sensor(const SensorInfo& info);

    /// Convert a raw float value to centi-unit integer encoding
    static int to_centi_units(const std::string& key, double value);

    /// Format a centi-unit value to a human-readable string with units
    static std::string format_value(const std::string& key, int centi_value);

  protected:
    void register_subjects(SubjectManager& subjects) override;
    void refresh_subjects() override;
    bool parse_status(const nlohmann::json& status) override;
    void on_deinit_subjects() override;

  private:
    SensorState();
    static Options make_options();

    void on_sensor_update(const nlohmann::json& msg);

    /// Mutex held, main thread: one subject per (sensor, key), none for vanished ones.
    void sync_subjects_locked();

    static std::string subject_key(const std::string& sensor_id, const std::string& key);

    std::unordered_map<std::string, SensorInfo> infos_;
    /// Latest centi-unit value per sensor id and key, applied to subjects by refresh_subjects().
    std::unordered_map<std::string, std::map<std::string, int>> readings_;
    sensors::DynamicSubjectMap subjects_by_key_;
};

} // namespace helix
