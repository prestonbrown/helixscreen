// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "async_lifetime_guard.h"
#include "lvgl.h"
#include "sensor_collection.h"
#include "subject_managed_panel.h"
#include "temperature_sensor_types.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix::sensors {

/**
 * @brief Manager for temperature sensors (temperature_sensor, temperature_fan, heater_generic)
 *
 * Provides:
 * - Auto-discovery of temperature sensors from Klipper objects list
 * - Auto-categorization by role (CHAMBER, MCU, HOST, AUXILIARY)
 * - Real-time state tracking from Moonraker updates
 * - Per-sensor dynamic LVGL subjects for reactive UI binding (decidegrees)
 *
 * Thread-safe for state updates from Moonraker callbacks.
 *
 * Klipper object names:
 * - temperature_sensor <name>  - Read-only temperature sensor
 * - temperature_fan <name>     - Temperature-controlled fan (has target and speed)
 * - heater_generic <name>      - Generic heater (has target); the chamber's own heater
 *                                takes the CHAMBER role via apply_chamber_sensor_override()
 *
 * Excludes: extruder, extruder1, heater_bed (managed by PrinterState)
 *
 * Status JSON format:
 * @code
 * {
 *   "temperature_sensor mcu_temp": {
 *     "temperature": 45.2
 *   },
 *   "temperature_fan exhaust_fan": {
 *     "temperature": 35.0,
 *     "target": 40.0,
 *     "speed": 0.5
 *   }
 * }
 * @endcode
 */
class TemperatureSensorManager {
  public:
    /**
     * @brief Dynamic integer subject for per-sensor temperature binding
     *
     * Wraps an lv_subject_t with lifecycle management. These are NOT registered
     * with the XML system since they are created dynamically per-sensor.
     * Values stored as decidegrees (temperature * 10).
     */
    struct DynamicIntSubject {
        lv_subject_t subject{};
        lv_subject_t target_subject{}; ///< Target in decidegrees (0 for read-only sensors)
        bool initialized = false;
        SubjectLifetime lifetime; ///< Alive token for ObserverGuard safety

        ~DynamicIntSubject() {
            // Expire lifetime token BEFORE deiniting subject — this invalidates
            // all ObserverGuard weak_ptrs so they won't call lv_observer_remove()
            // on the observers that lv_subject_deinit() is about to free.
            lifetime.reset();
            if (initialized && lv_is_initialized()) {
                lv_subject_deinit(&subject);
                lv_subject_deinit(&target_subject);
            }
            initialized = false;
        }
    };

    /**
     * @brief Get singleton instance
     */
    static TemperatureSensorManager& instance();

    // Prevent copying
    TemperatureSensorManager(const TemperatureSensorManager&) = delete;
    TemperatureSensorManager& operator=(const TemperatureSensorManager&) = delete;

    // ========================================================================
    // Discovery, Status and Config
    // ========================================================================

    /**
     * @brief Discover sensors from Klipper objects list
     * @note MUST be called from main LVGL thread (updates subjects directly)
     */
    void discover(const std::vector<std::string>& klipper_objects);

    /// @brief Update state from Moonraker status JSON
    void update_from_status(const nlohmann::json& status);

    /**
     * @brief Load sensor configuration from JSON
     * @note MUST be called from main LVGL thread (updates subjects directly)
     */
    void load_config(const nlohmann::json& config);

    /// @brief Save configuration to JSON
    [[nodiscard]] nlohmann::json save_config() const;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize LVGL subjects for UI binding
     *
     * Must be called before creating any XML components that bind to sensor subjects.
     * Safe to call multiple times (idempotent).
     */
    void init_subjects();

    /**
     * @brief Deinitialize LVGL subjects
     *
     * Must be called before lv_deinit() to properly disconnect observers.
     */
    void deinit_subjects();

    // ========================================================================
    // Sensor Queries
    // ========================================================================

    /**
     * @brief Check if any sensors have been discovered
     */
    [[nodiscard]] bool has_sensors() const;

    /**
     * @brief Get all discovered sensor configurations (thread-safe copy)
     */
    [[nodiscard]] std::vector<TemperatureSensorConfig> get_sensors() const;

    /**
     * @brief Get sensors sorted by priority (lower first), then by display_name
     */
    [[nodiscard]] std::vector<TemperatureSensorConfig> get_sensors_sorted() const;

    /**
     * @brief Get sensor count
     */
    [[nodiscard]] size_t sensor_count() const;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Assign a role to a sensor
     * @note MUST be called from main LVGL thread (updates subjects directly)
     *
     * @param klipper_name Full Klipper object name
     * @param role New role assignment
     */
    void set_sensor_role(const std::string& klipper_name, TemperatureSensorRole role);

    /**
     * @brief Apply manual chamber sensor override
     *
     * Clears CHAMBER role from any existing sensor, then assigns it to
     * the specified sensor (if non-empty). Demoted sensors get their role
     * re-inferred from their name (MCU, HOST, or AUXILIARY).
     *
     * @param klipper_name Full Klipper object name for the new chamber sensor,
     *                     or empty string to clear chamber assignment entirely.
     * @param heater_name  The resolved chamber heater. A heater_generic also takes the
     *                     CHAMBER role so it is not listed a second time as an auxiliary
     *                     heater; a temperature_fan keeps its own role. Empty when none.
     * @note MUST be called from main LVGL thread (updates subjects directly)
     */
    void apply_chamber_sensor_override(const std::string& klipper_name,
                                       const std::string& heater_name = "");

    /**
     * @brief Enable or disable a sensor
     * @note MUST be called from main LVGL thread (updates subjects directly)
     *
     * @param klipper_name Full Klipper object name
     * @param enabled Whether sensor should be monitored
     */
    void set_sensor_enabled(const std::string& klipper_name, bool enabled);

    // ========================================================================
    // State Queries
    // ========================================================================

    /**
     * @brief Get current state for a sensor by klipper_name (thread-safe copy)
     *
     * @param klipper_name The full Klipper name to query
     * @return State copy if sensor exists, empty optional otherwise
     */
    [[nodiscard]] std::optional<TemperatureSensorState>
    get_sensor_state(const std::string& klipper_name) const;

    // ========================================================================
    // LVGL Subjects
    // ========================================================================

    /**
     * @brief Get dynamic subject for a specific sensor's temperature (with lifetime token)
     *
     * IMPORTANT: When creating observers on this subject, always use the returned
     * lifetime token to prevent use-after-free during sensor rediscovery.
     *
     * @param klipper_name Full Klipper object name
     * @param[out] lifetime Receives the subject's lifetime token
     * @return Subject (int: decidegrees), or nullptr if sensor unknown
     */
    [[nodiscard]] lv_subject_t* get_temp_subject(const std::string& klipper_name,
                                                 SubjectLifetime& lifetime);

    /**
     * @brief Get a sensor's target subject (int: decidegrees)
     *
     * Shares the temperature subject's lifetime token. Reads 0 for objects
     * without a target (temperature_sensor, tmc*).
     */
    [[nodiscard]] lv_subject_t* get_target_subject(const std::string& klipper_name,
                                                   SubjectLifetime& lifetime);

    /// @brief Get dynamic subject without lifetime token (only for non-observer uses)
    [[nodiscard]] lv_subject_t* get_temp_subject(const std::string& klipper_name);

    /**
     * @brief Get subject for sensor count (for conditional UI visibility)
     * @return Subject (int: number of discovered sensors)
     */
    [[nodiscard]] lv_subject_t* get_sensor_count_subject();

    /**
     * @brief Death signal for the set-level subjects this singleton owns.
     *
     * deinit_subjects() frees every observer node on them without bumping the
     * ObserverGuard invalidation epoch, so outside observers must pass this to
     * observe_*(), or their guards call lv_observer_remove() on a freed node.
     * Per-sensor subjects have their own tokens via get_temp_subject().
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    /**
     * @brief Enable synchronous mode for testing
     *
     * When enabled, update_from_status() calls update_subjects() synchronously
     * instead of using queue_update().
     */
    void set_sync_mode(bool enabled);

    /**
     * @brief Update subjects on main LVGL thread (called by async callback)
     */
    void update_subjects_on_main_thread();

    friend class TemperatureSensorManagerTestAccess;

  private:
    TemperatureSensorManager();
    ~TemperatureSensorManager();

    /**
     * @brief Parse Klipper object name to determine if it's a temperature sensor
     *
     * @param klipper_name Full name like "temperature_sensor mcu_temp"
     * @param[out] sensor_name Extracted short name (e.g., "mcu_temp")
     * @param[out] type Detected sensor type
     * @return true if successfully parsed as temperature sensor
     */
    bool parse_klipper_name(const std::string& klipper_name, std::string& sensor_name,
                            TemperatureSensorType& type) const;

    /**
     * @brief Update all LVGL subjects from current state
     * @note Internal method - MUST only be called from main LVGL thread
     */
    void update_subjects();

    /**
     * @brief Ensure a dynamic subject exists for a sensor
     * @param klipper_name Full Klipper object name
     */
    void ensure_sensor_subject(const std::string& klipper_name);

    // Recursive mutex for thread-safe state access
    mutable std::recursive_mutex mutex_;

    // Async callback safety guard (L072: never access state after shutdown)
    helix::AsyncLifetimeGuard lifetime_;

    SensorCollection<TemperatureSensorConfig, TemperatureSensorState> sensors_;

    // Per-sensor dynamic subjects (keyed by klipper_name, value in decidegrees)
    std::map<std::string, std::unique_ptr<DynamicIntSubject>> temp_subjects_;

    // Test mode: when true, update_from_status() calls update_subjects() synchronously
    bool sync_mode_ = false;

    // LVGL subjects
    bool subjects_initialized_ = false;
    SubjectManager subjects_;
    lv_subject_t sensor_count_{};
};

} // namespace helix::sensors
