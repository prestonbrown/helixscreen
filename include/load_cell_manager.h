// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "async_lifetime_guard.h"
#include "load_cell_types.h"
#include "lvgl.h"
#include "sensor_registry.h"
#include "subject_managed_panel.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix::sensors {

/**
 * @brief Manager for load cells (load_cell)
 *
 * Implements ISensorManager interface for integration with SensorRegistry.
 * Provides:
 * - Auto-discovery of load cells from Klipper objects list
 * - Auto-categorization by role (SPOOL_WEIGHT)
 * - Real-time state tracking from Moonraker updates
 *
 * Thread-safe for state updates from Moonraker callbacks.
 *
 * Klipper object names:
 * - load_cell <name>  - Load cell
 *
 * Status JSON format:
 * @code
 * {
 *   "load_cell spool_weight": {
 *     "force_g": 45.2
 *   },
 * }
 * @endcode
 */
class LoadCellManager : public ISensorManager {
  public:
    /**
     * @brief Get singleton instance
     */
    static LoadCellManager& instance();

    // Prevent copying
    LoadCellManager(const LoadCellManager&) = delete;
    LoadCellManager& operator=(const LoadCellManager&) = delete;

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

    /**
     * @brief Load sensor configuration from JSON
     * @note MUST be called from main LVGL thread (updates subjects directly)
     */
    void load_config(const nlohmann::json& config) override;

    /// @brief Save configuration to JSON
    [[nodiscard]] nlohmann::json save_config() const override;

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

    /**
     * @brief Death signal for every subject this manager owns.
     *
     * deinit_subjects() frees the observer nodes on all of them at once, so an
     * observer whose owner outlives this manager's teardown must pass this
     * token to observe_*(). Without it the guard's reset() calls
     * lv_observer_remove() on a freed node.
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    // ========================================================================
    // Sensor Queries
    // ========================================================================

    /**
     * @brief Return whether a spool weight load cell is available.
     */
    [[nodiscard]] bool has_spool_weight_load_cell() const;

    /**
     * @brief Get sensors sorted by priority (lower first), then by display_name
     */
    [[nodiscard]] std::vector<LoadCellConfig> get_sensors_sorted() const;

    /**
     * @brief Get sensor count
     */
    [[nodiscard]] size_t sensor_count() const;

    // ============================================================================
    // Testing Support
    // ============================================================================

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

    friend class LoadCellManagerTestAccess;

  private:
    LoadCellManager();
    ~LoadCellManager();

    /**
     * @brief Parse Klipper object name to determine if it's a load cell
     *
     * @param klipper_name Full name like "load_cell spool_weight"
     * @param[out] sensor_name Extracted short name (e.g., "spool_weight")
     * @return true if successfully parsed as load cell
     */
    const std::optional<const std::string>
    parse_klipper_name(const std::string& klipper_name) const;

    /**
     * @brief Find config by assigned role
     * @return Pointer to config, or nullptr if no load cell has this role
     */
    const LoadCellConfig* find_config_by_role(LoadCellRole role) const;

    /**
     * @brief Update all LVGL subjects from current state
     * @note Internal method - MUST only be called from main LVGL thread
     */
    void update_subjects();

    // Recursive mutex for thread-safe state access
    mutable std::recursive_mutex mutex_;

    // Async callback safety guard (L072: never access state after shutdown)
    helix::AsyncLifetimeGuard lifetime_;

    // Configuration
    std::vector<LoadCellConfig> sensors_;

    // Runtime state (keyed by klipper_name)
    std::map<std::string, LoadCellState> states_;

    // Test mode: when true, update_from_status() calls update_subjects() synchronously
    bool sync_mode_ = false;

    // LVGL subjects
    bool subjects_initialized_ = false;
    SubjectManager subjects_;
    lv_subject_t sensor_count_;
};

} // namespace helix::sensors
