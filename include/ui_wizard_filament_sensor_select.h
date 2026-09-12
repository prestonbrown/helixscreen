// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_sensor_types.h"
#include "lvgl/lvgl.h"
#include "wizard_step.h"

#include <memory>
#include <string>
#include <vector>

/**
 * @file ui_wizard_filament_sensor_select.h
 * @brief Wizard filament sensor selection step - assigns runout sensor role
 *
 * Uses FilamentSensorManager to discover sensors and assign the RUNOUT role.
 *
 * ## Skip Logic:
 *
 * - 0 non-AMS sensors: Skip entirely
 * - 1 non-AMS sensor: Auto-assign to RUNOUT, skip step
 * - 2+ non-AMS sensors: Show wizard step for manual assignment
 *
 * AMS sensors (lane/slot sensors from AFC, etc.) are filtered out.
 *
 * ## Subject Bindings (1 total):
 *
 * - runout_sensor_selected (int) - Selected sensor index for runout role
 *
 * Index 0 = "None", 1+ = sensor index in sensor_items_
 */

/**
 * @class WizardFilamentSensorSelectStep
 * @brief Filament sensor configuration step for the first-run wizard
 */
class WizardFilamentSensorSelectStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::FilamentSensor;
    }
    const char* component_name() const override {
        return "wizard_filament_sensor_select";
    }
    const char* log_name() const override {
        return "Wizard Filament Sensor";
    }
    bool should_skip(const helix::wizard::StepContext& ctx) const override {
        return ctx.preset.skip_hardware || should_skip();
    }

    WizardFilamentSensorSelectStep();
    ~WizardFilamentSensorSelectStep();

    // Non-copyable, non-movable (lv_subject_t cannot be safely moved)
    WizardFilamentSensorSelectStep(const WizardFilamentSensorSelectStep&) = delete;
    WizardFilamentSensorSelectStep& operator=(const WizardFilamentSensorSelectStep&) = delete;
    WizardFilamentSensorSelectStep(WizardFilamentSensorSelectStep&&) = delete;
    WizardFilamentSensorSelectStep& operator=(WizardFilamentSensorSelectStep&&) = delete;

    /**
     * @brief Initialize reactive subjects
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks
     */
    void register_callbacks() override;

    /**
     * @brief Create the filament sensor selection UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Cleanup resources and save role assignments to config
     */
    void cleanup() override;

    /**
     * @brief Refresh sensor list and auto-selection
     *
     * Call this when sensors may have been discovered after initial create().
     * Re-filters sensors and runs auto-selection if no sensor is selected.
     */
    void refresh();

    /**
     * @brief Arm the deferred sensor refresh one-shot (1500 ms)
     *
     * Covers the case where sensors are discovered after screen creation
     * (e.g. jumping directly to this step outruns discovery). The step owns
     * the timer: cleanup() and the destructor both cancel it, so it can never
     * fire into a step that is being torn down (prestonbrown/helixscreen#1577).
     */
    void schedule_deferred_refresh();

    /// Test-only: the armed refresh timer, or nullptr when none is armed.
    lv_timer_t* refresh_timer_for_test() const {
        return refresh_timer_;
    }

    /**
     * @brief Check if step should be skipped
     *
     * Returns true if there are fewer than 2 non-AMS sensors.
     * If exactly 1 sensor, it will be auto-assigned to RUNOUT.
     *
     * @return true if step should be skipped
     */
    bool should_skip() const;

    /**
     * @brief Check if step is validated
     *
     * @return true (always validated for baseline)
     */
    bool is_validated() const override;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "Wizard Filament Sensor";
    }

    /**
     * @brief Get the number of standalone (non-AMS) sensors
     *
     * Queries FilamentSensorManager directly as the single source of truth.
     * This works even when the step is skipped and create() was never called.
     */
    size_t get_standalone_sensor_count() const;

    /**
     * @brief Auto-configure a single sensor as RUNOUT
     *
     * Called when exactly 1 sensor is detected to auto-assign it.
     */
    void auto_configure_single_sensor();

    // Public access to subjects for helper functions
    lv_subject_t* get_runout_sensor_subject() {
        return &runout_sensor_selected_;
    }

    std::vector<std::string>& get_sensor_items() {
        return sensor_items_;
    }

  private:
    /**
     * @brief Check if a sensor name indicates it's managed by AMS
     */
    static bool is_ams_sensor(const std::string& name);

    /// Cancel an armed deferred-refresh timer. Safe to call when none is armed.
    void cancel_refresh_timer();

    /// Trampoline for the deferred-refresh one-shot; user_data is the step.
    static void refresh_timer_cb(lv_timer_t* timer);

    /**
     * @brief Filter sensors to get only standalone (non-AMS) sensors
     */
    void filter_standalone_sensors();

    /**
     * @brief Populate dropdown options from discovered sensors
     */
    void populate_dropdowns();

    /**
     * @brief Get the klipper name for a dropdown selection index
     *
     * @param dropdown_index Index in dropdown (0 = None, 1+ = sensor)
     * @return Klipper name, or empty string if None selected
     */
    std::string get_klipper_name_for_index(int dropdown_index) const;

    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Deferred refresh one-shot; owned and cancelled by this step
    lv_timer_t* refresh_timer_ = nullptr;

    // Subject (dropdown selection index)
    lv_subject_t runout_sensor_selected_;

    // Dynamic options storage
    std::vector<std::string> sensor_items_;                       // Klipper names for dropdown
    std::vector<helix::FilamentSensorConfig> standalone_sensors_; // Filtered sensors

    // Track initialization
    bool subjects_initialized_ = false;
};

// ============================================================================
// Global Instance Access
// ============================================================================

WizardFilamentSensorSelectStep* get_wizard_filament_sensor_select_step();
