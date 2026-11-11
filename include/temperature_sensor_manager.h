// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dynamic_subject_map.h"
#include "sensor_manager.h"
#include "stepper_driver_overtemp.h"
#include "temperature_sensor_types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace helix::sensors {

/**
 * @brief Everything that reports a temperature and is not a real heater.
 *
 * Kinds: temperature_sensor, temperature_fan, the drivers that report a temperature,
 * heater_generic and the humidity chips (BME280, AHT20, ...). The extruders and
 * the bed are excluded by kind, so a sensor merely named like one (an
 * "extruder_board" thermistor) still appears.
 *
 * Discovery, the sensor list, enabled state and publishing come from
 * SensorManager. This class owns the readings, one temperature subject and one
 * target subject per sensor (decidegrees, in DynamicSubjectMap), and the
 * name-driven roles. Roles are not persisted and not offered in Settings:
 * CHAMBER, MCU, HOST, STEPPER_DRIVER and AUXILIARY are assigned by name on every
 * discovery, a `[temperature_sensor]` sensor_type of temperature_mcu or
 * temperature_host turns AUXILIARY into MCU or HOST on the configfile pass, and
 * apply_chamber_sensor_override() moves CHAMBER.
 *
 * It also watches every stepper driver's drv_status, sensor or not, and raises
 * one notice each time a driver's overtemperature level rises.
 *
 * Status JSON format:
 * @code
 * {
 *   "temperature_sensor mcu_temp": { "temperature": 45.2 },
 *   "temperature_fan exhaust_fan": { "temperature": 35.0, "target": 40.0, "speed": 0.5 },
 *   "bme280 chamber": { "temperature": 28.1, "humidity": 41.0 }
 * }
 * @endcode
 */
class TemperatureSensorManager : public SensorManager {
  public:
    static TemperatureSensorManager& instance();

    /// All sensors in discovery order, as the legacy config struct the pickers use.
    [[nodiscard]] std::vector<TemperatureSensorConfig> get_sensors() const;

    /// Sensors sorted by priority (lower first), then by display_name.
    [[nodiscard]] std::vector<TemperatureSensorConfig> get_sensors_sorted() const;

    /// Current reading for a sensor, or nullopt if it is unknown.
    [[nodiscard]] std::optional<TemperatureSensorState>
    get_sensor_state(const std::string& klipper_name) const;

    /**
     * @brief Move the CHAMBER role onto the named sensor (and heater).
     *
     * Clears CHAMBER from every other sensor, re-inferring their role from the
     * name (MCU, HOST or AUXILIARY). An empty name clears the assignment. An
     * unknown sensor name leaves the incumbents alone. A heater_generic heater
     * also takes CHAMBER so it is not listed again as an auxiliary heater; a
     * temperature_fan keeps its own role.
     * @note Main thread.
     */
    void apply_chamber_sensor_override(const std::string& klipper_name,
                                       const std::string& heater_name = "");

    /**
     * @brief A sensor's temperature subject (int: decidegrees), or nullptr if unknown.
     *
     * Observers must use the returned lifetime: it expires before the subject is
     * freed when the sensor vanishes or the manager deinitializes.
     */
    [[nodiscard]] lv_subject_t* get_temp_subject(const std::string& klipper_name,
                                                 SubjectLifetime& lifetime);

    /// Same subject without a lifetime; only for reads that do not observe.
    [[nodiscard]] lv_subject_t* get_temp_subject(const std::string& klipper_name);

    /// A sensor's target subject (int: decidegrees; 0 for objects without a target).
    /// Shares the temperature subject's lifetime.
    [[nodiscard]] lv_subject_t* get_target_subject(const std::string& klipper_name,
                                                   SubjectLifetime& lifetime);

    /// A CHAMBER sensor is set through the chamber assignment dropdowns, so
    /// Settings does not list it as a row.
    [[nodiscard]] bool listed_in_settings(const SensorEntry& entry) const override;
    /// Names a sensor the way the rest of the UI does ("MCU Temperature").
    [[nodiscard]] SensorRowView row(size_t index) const override;
    /// Listed rows in get_sensors_sorted() order (priority, then display name).
    [[nodiscard]] std::vector<size_t> settings_rows() const override;

  protected:
    void register_subjects(SubjectManager& subjects) override;
    void refresh_subjects() override;
    bool parse_status(const nlohmann::json& status) override;
    void on_discovered(const PrinterDiscovery& hw) override;
    void on_entries_changed() override;
    void on_deinit_subjects() override;

  private:
    friend class TemperatureSensorManagerTestAccess;

    TemperatureSensorManager();
    static Options make_options();

    /// Mutex held, main thread: one subject pair per entry, none for vanished sensors.
    void sync_subjects_locked();
    /// Main thread.
    static void publish_driver_overtemp_notice(const DriverOvertempAlert& alert);

    std::map<std::string, TemperatureSensorState> readings_; ///< by klipper_name
    DynamicSubjectMap temp_subjects_;
    DynamicSubjectMap target_subjects_;

    std::vector<std::string> driver_objects_; ///< every stepper driver object; mutex
    /// Drivers above DriverOvertemp::None, by klipper_name; mutex.
    std::map<std::string, DriverOvertemp> driver_levels_;
    /// driver_levels_ as of the last notices raised; main thread only.
    std::map<std::string, DriverOvertemp> driver_levels_notified_;
};

} // namespace helix::sensors
