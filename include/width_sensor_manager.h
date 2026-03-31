// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"
#include "sensor_registry.h"
#include "subject_managed_panel.h"
#include "width_sensor_types.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix::sensors {

/**
 * @brief Manager for filament width sensors (TSL1401CL and Hall-effect based)
 *
 * Implements ISensorManager interface for integration with SensorRegistry.
 * Provides:
 * - Auto-discovery of width sensors from Klipper objects list
 * - Role assignment for flow compensation
 * - Real-time state tracking from Moonraker updates
 * - LVGL subjects for reactive UI binding
 *
 * Thread-safe for state updates from Moonraker callbacks.
 *
 * Klipper object names:
 * - tsl1401cl_filament_width_sensor
 * - hall_filament_width_sensor
 * - hall_fila_switch (alias for hall_filament_width_sensor)
 *
 * Status JSON format:
 * @code
 * {
 *   "tsl1401cl_filament_width_sensor": {
 *     "Diameter": 1.75,
 *     "Raw": 12345
 *   }
 * }
 * @endcode
 */
class WidthSensorManager : public ISensorManager {
  public:
    /**
     * @brief Get singleton instance
     */
    static WidthSensorManager& instance();

    // Prevent copying
    WidthSensorManager(const WidthSensorManager&) = delete;
    WidthSensorManager& operator=(const WidthSensorManager&) = delete;

    // ========================================================================
    // ISensorManager Interface
    // ========================================================================

    /// @brief Get category name for registry
    [[nodiscard]] std::string category_name() const override;

    /**
     * @brief Discover sensors from Klipper objects list
     * @note MUST be called from main LVGL thread (updates subjects directly)
     */
    void discover(const std::vector<std::string>& klipper_objects) override;

    /// @brief Update state from Moonraker status JSON
    void update_from_status(const nlohmann::json& status) override;

    /// @brief Inject mock sensor objects for testing UI
    void inject_mock_sensors(std::vector<std::string>& objects, nlohmann::json& config_keys,
                             nlohmann::json& moonraker_info) override;

    /// @brief Inject mock status data for testing UI
    void inject_mock_status(nlohmann::json& status) override;

    /**
     * @brief Load sensor configuration from JSON
     * @note MUST be called from main LVGL thread (updates subjects directly)
     */
    void load_config(const nlohmann::json& config) override;

    /// @brief Save configuration to JSON
    [[nodiscard]] nlohmann::json save_config() const override;

    /**
     * @brief Save configuration to file (convenience wrapper for UI callbacks)
     * @note Calls load_config() internally after reading from Config
     */
    void load_config_from_file();

    /// @brief Save configuration to file (convenience wrapper for UI callbacks)
    void save_config_to_file();

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
    [[nodiscard]] std::vector<WidthSensorConfig> get_sensors() const;

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
    void set_sensor_role(const std::string& klipper_name, WidthSensorRole role);

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
     * @brief Get current state for a sensor by role (thread-safe copy)
     *
     * @param role The sensor role to query
     * @return State copy if sensor assigned to role, empty optional otherwise
     */
    [[nodiscard]] std::optional<WidthSensorState> get_sensor_state(WidthSensorRole role) const;

    /**
     * @brief Check if a sensor is available (exists and enabled)
     *
     * @param role The sensor role to check
     * @return true if sensor exists, is enabled, and is available in Klipper
     */
    [[nodiscard]] bool is_sensor_available(WidthSensorRole role) const;

    /**
     * @brief Get current filament diameter for flow compensation role
     *
     * @return Diameter in mm, or 0.0 if no sensor assigned or disabled
     */
    [[nodiscard]] float get_flow_compensation_diameter() const;

    // ========================================================================
    // LVGL Subjects
    // ========================================================================

    /**
     * @brief Get subject for filament diameter
     * @return Subject (int: mm x 1000, -1 if no sensor assigned)
     */
    [[nodiscard]] lv_subject_t* get_diameter_subject();

    /**
     * @brief Get subject for sensor count (for conditional UI visibility)
     * @return Subject (int: number of discovered sensors)
     */
    [[nodiscard]] lv_subject_t* get_sensor_count_subject();

    /**
     * @brief Get subject for filament diameter text (formatted as "1.75mm")
     * @return Subject (string: formatted diameter or "--" if no sensor)
     */
    [[nodiscard]] lv_subject_t* get_diameter_text_subject();

    /**
     * @brief Enable synchronous mode for testing
     *
     * When enabled, update_from_status() calls update_subjects() synchronously
     * instead of using lv_async_call().
     */
    void set_sync_mode(bool enabled);

    /**
     * @brief Update subjects on main LVGL thread (called by async callback)
     */
    void update_subjects_on_main_thread();

    friend class WidthSensorManagerTestAccess;

  private:
    WidthSensorManager();
    ~WidthSensorManager();

    /**
     * @brief Parse Klipper object name to determine if it's a width sensor
     *
     * @param klipper_name Full name like "tsl1401cl_filament_width_sensor"
     * @param[out] sensor_name Extracted short name
     * @param[out] type Detected sensor type
     * @return true if successfully parsed as width sensor
     */
    bool parse_klipper_name(const std::string& klipper_name, std::string& sensor_name,
                            WidthSensorType& type) const;

    /**
     * @brief Find config by Klipper name
     * @return Pointer to config, or nullptr if not found
     */
    WidthSensorConfig* find_config(const std::string& klipper_name);
    const WidthSensorConfig* find_config(const std::string& klipper_name) const;

    /**
     * @brief Find config by assigned role
     * @return Pointer to config, or nullptr if no sensor has this role
     */
    const WidthSensorConfig* find_config_by_role(WidthSensorRole role) const;

    /**
     * @brief Update all LVGL subjects from current state
     * @note Internal method - MUST only be called from main LVGL thread
     */
    void update_subjects();

    // Recursive mutex for thread-safe state access
    mutable std::recursive_mutex mutex_;

    // Configuration
    std::vector<WidthSensorConfig> sensors_;

    // Runtime state (keyed by klipper_name)
    std::map<std::string, WidthSensorState> states_;

    // Test mode: when true, update_from_status() calls update_subjects() synchronously
    bool sync_mode_ = false;

    // LVGL subjects
    bool subjects_initialized_ = false;
    SubjectManager subjects_;
    lv_subject_t diameter_;
    lv_subject_t sensor_count_;
    lv_subject_t diameter_text_;
    char diameter_text_buf_[16]; ///< "1.75mm" or "--"
};

} // namespace helix::sensors
