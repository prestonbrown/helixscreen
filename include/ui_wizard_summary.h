// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "wizard_step.h"

#include <memory>

/**
 * @file ui_wizard_summary.h
 * @brief Wizard summary step - displays configuration overview
 *
 * This is a read-only step that shows all configuration choices made
 * during the wizard. Uses 12 reactive subjects for data binding.
 *
 * ## Class-Based Architecture (Phase 6)
 *
 * This step has been migrated from function-based to class-based design:
 * - Instance members instead of static globals
 * - RAII cleanup for subjects and buffers
 * - Static trampolines for any LVGL callbacks
 * - Global singleton getter for backwards compatibility
 *
 * ## Subject Bindings (14 total):
 *
 * - summary_printer_name (string) - Configured printer name
 * - summary_printer_type (string) - Selected printer type
 * - summary_wifi_ssid (string) - WiFi network name
 * - summary_moonraker_connection (string) - Moonraker host:port
 * - summary_bed (string) - Bed heater/sensor summary
 * - summary_hotend (string) - Hotend heater/sensor summary
 * - summary_part_fan (string) - Part fan selection
 * - summary_part_fan_visible (int) - Part fan row visibility
 * - summary_hotend_fan (string) - Hotend fan selection
 * - summary_hotend_fan_visible (int) - Hotend fan row visibility
 * - summary_led_strip (string) - LED strip selection
 * - summary_led_strip_visible (int) - LED strip row visibility
 * - summary_filament_sensor (string) - Filament sensor selection
 * - summary_filament_sensor_visible (int) - Filament sensor row visibility
 */

/**
 * @class WizardSummaryStep
 * @brief Configuration summary step for the first-run wizard
 *
 * Displays a read-only summary of all configuration choices made during
 * the wizard. No user interaction - Next button is always enabled.
 */
class WizardSummaryStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::Summary;
    }
    const char* component_name() const override {
        return "wizard_summary";
    }
    const char* log_name() const override {
        return "Wizard Summary";
    }
    bool should_skip(const helix::wizard::StepContext& ctx) const override {
        return ctx.preset.first_run;
    }

    WizardSummaryStep();
    ~WizardSummaryStep();

    // Non-copyable, non-movable (singleton with lv_subject_t members that
    // contain internal linked lists — moving corrupts observer pointers)
    WizardSummaryStep(const WizardSummaryStep&) = delete;
    WizardSummaryStep& operator=(const WizardSummaryStep&) = delete;
    WizardSummaryStep(WizardSummaryStep&&) = delete;
    WizardSummaryStep& operator=(WizardSummaryStep&&) = delete;

    /**
     * @brief Initialize reactive subjects with current config values
     *
     * Loads values from Config and initializes all 12 subjects.
     * Safe to call multiple times - refreshes from config each time.
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks (none for summary)
     *
     * Summary step is read-only, so this is a no-op.
     */
    void register_callbacks() override;

    /**
     * @brief Create the summary UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Cleanup resources
     *
     * Resets UI references. Does NOT call lv_obj_del() - wizard
     * framework handles widget deletion.
     */
    void cleanup() override;

    /**
     * @brief Check if step is validated
     *
     * @return true (always validated - no user input required)
     */
    bool is_validated() const override;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "Wizard Summary";
    }

    /** @brief Telemetry toggle callback (wired from XML) */
    static void on_wizard_telemetry_changed(lv_event_t* e);

    /** @brief "Learn more" link callback — shows telemetry info modal */
    static void on_wizard_telemetry_info(lv_event_t* e);

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Subjects (14 total)
    lv_subject_t printer_name_{};
    lv_subject_t printer_type_{};
    lv_subject_t wifi_ssid_{};
    lv_subject_t moonraker_connection_{};
    lv_subject_t bed_{};
    lv_subject_t hotend_{};
    lv_subject_t part_fan_{};
    lv_subject_t part_fan_visible_{};
    lv_subject_t hotend_fan_{};
    lv_subject_t hotend_fan_visible_{};
    lv_subject_t led_strip_{};
    lv_subject_t led_strip_visible_{};
    lv_subject_t filament_sensor_{};
    lv_subject_t filament_sensor_visible_{};
    lv_subject_t ams_type_{};
    lv_subject_t ams_visible_{};
    lv_subject_t input_shaper_{};
    lv_subject_t input_shaper_visible_{};

    // String buffers (must be persistent for subject lifetimes)
    char printer_name_buffer_[128];
    char printer_type_buffer_[128];
    char wifi_ssid_buffer_[128];
    char moonraker_connection_buffer_[128];
    char bed_buffer_[256];
    char hotend_buffer_[256];
    char part_fan_buffer_[128];
    char hotend_fan_buffer_[128];
    char led_strip_buffer_[128];
    char filament_sensor_buffer_[128];
    char ams_type_buffer_[64];
    char input_shaper_buffer_[128];

    // Track initialization
    bool subjects_initialized_ = false;

    // Helper functions
    std::string format_bed_summary();
    std::string format_hotend_summary();
};

// ============================================================================
// Global Instance Access
// ============================================================================

/**
 * @brief Get the global WizardSummaryStep instance
 *
 * Creates the instance on first call. Used by wizard framework.
 *
 * @return Pointer to the singleton instance
 */
WizardSummaryStep* get_wizard_summary_step();
