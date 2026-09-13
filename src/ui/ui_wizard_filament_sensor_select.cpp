// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_filament_sensor_select.h"

#include "ui_error_reporting.h"
#include "ui_notification.h"
#include "ui_timer_guard.h"
#include "ui_wizard.h"
#include "ui_wizard_helpers.h"

#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "lvgl/lvgl.h"
#include "moonraker_client.h"
#include "printer_hardware.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardFilamentSensorSelectStep> g_wizard_filament_sensor_select_step;

WizardFilamentSensorSelectStep* get_wizard_filament_sensor_select_step() {
    if (!g_wizard_filament_sensor_select_step) {
        g_wizard_filament_sensor_select_step = std::make_unique<WizardFilamentSensorSelectStep>();
        StaticPanelRegistry::instance().register_destroy("WizardFilamentSensorSelectStep", []() {
            g_wizard_filament_sensor_select_step.reset();
        });
    }
    return g_wizard_filament_sensor_select_step.get();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardFilamentSensorSelectStep::WizardFilamentSensorSelectStep() {
    spdlog::debug("[{}] Instance created", get_name());
}

WizardFilamentSensorSelectStep::~WizardFilamentSensorSelectStep() {
    // NOTE: Do NOT log here - spdlog may be destroyed first

    // Shutdown runs StaticPanelRegistry::destroy_all() BEFORE lv_deinit(), so
    // an armed refresh one-shot is still in LVGL's timer list here, holding a
    // freed `this`. A teardown that destroys the step without calling
    // cleanup() first must not leave it armed. The shared helper goes through
    // lv_timer_cancel_safe(), which no-ops when LVGL is already gone, so it is
    // safe from the destructor either way (prestonbrown/helixscreen#1577).
    cancel_refresh_timer();

    screen_root_ = nullptr;
}

// ============================================================================
// AMS Sensor Detection
// ============================================================================

bool WizardFilamentSensorSelectStep::is_ams_sensor(const std::string& name) {
    // Delegate to shared implementation in PrinterHardware, passing the
    // discovery so HH/AFC-managed sensors (extruder, toolhead,
    // tool_start, <lane>_prep, ...) are excluded from the wizard list.
    return PrinterHardware::is_ams_sensor(name, get_printer_state().get_discovery());
}

void WizardFilamentSensorSelectStep::filter_standalone_sensors() {
    standalone_sensors_.clear();

    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    auto all_sensors = sensor_mgr.get_sensors();

    for (const auto& sensor : all_sensors) {
        if (!is_ams_sensor(sensor.sensor_name)) {
            standalone_sensors_.push_back(sensor);
            spdlog::debug("[{}] Found standalone sensor: {}", get_name(), sensor.sensor_name);
        } else {
            spdlog::debug("[{}] Filtered out AMS sensor: {}", get_name(), sensor.sensor_name);
        }
    }

    spdlog::info("[{}] Found {} standalone sensors (filtered from {} total)", get_name(),
                 standalone_sensors_.size(), all_sensors.size());
}

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardFilamentSensorSelectStep::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized, resetting value", get_name());
        lv_subject_set_int(&runout_sensor_selected_, 0);
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());
    helix::ui::wizard::init_int_subject(&runout_sensor_selected_, 0, "runout_sensor_selected");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Static Callbacks (XML event_cb pattern)
// ============================================================================

static void on_runout_sensor_dropdown_changed(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto* step = get_wizard_filament_sensor_select_step();
    if (step) {
        lv_subject_set_int(step->get_runout_sensor_subject(), index);
        spdlog::debug(
            "[WizardFilamentSensorSelectStep] Runout sensor selection changed to index {}", index);
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardFilamentSensorSelectStep::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_runout_sensor_dropdown_changed",
                             on_runout_sensor_dropdown_changed);
    spdlog::debug("[{}] Registered dropdown callback", get_name());
}

// ============================================================================
// Dropdown Population
// ============================================================================

void WizardFilamentSensorSelectStep::populate_dropdowns() {
    if (!screen_root_)
        return;

    // Build sensor items list: "None" + sensor names
    sensor_items_.clear();
    sensor_items_.push_back("None");
    for (const auto& sensor : standalone_sensors_) {
        sensor_items_.push_back(sensor.klipper_name);
    }

    // Build options string for dropdown (newline-separated)
    std::string options;
    for (size_t i = 0; i < sensor_items_.size(); i++) {
        if (i > 0)
            options += "\n";
        // Use display name (sensor_name) for dropdown, but store klipper_name
        if (i == 0) {
            options += lv_tr("None");
        } else {
            options += standalone_sensors_[i - 1].sensor_name;
        }
    }

    // Find and populate the runout dropdown
    lv_obj_t* runout_dropdown = lv_obj_find_by_name(screen_root_, "runout_sensor_dropdown");

    if (runout_dropdown) {
        lv_dropdown_set_options(runout_dropdown, options.c_str());
        lv_dropdown_set_selected(
            runout_dropdown, static_cast<uint32_t>(lv_subject_get_int(&runout_sensor_selected_)));
    }

    spdlog::debug("[{}] Populated dropdown with {} options", get_name(), sensor_items_.size());
}

std::string WizardFilamentSensorSelectStep::get_klipper_name_for_index(int dropdown_index) const {
    if (dropdown_index <= 0 || static_cast<size_t>(dropdown_index) >= sensor_items_.size()) {
        return ""; // "None" selected or invalid
    }
    return sensor_items_[static_cast<size_t>(dropdown_index)];
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardFilamentSensorSelectStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating filament sensor select screen", get_name());

    // Safety check
    if (screen_root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        screen_root_ = nullptr;
    }

    // Filter sensors to get standalone (non-AMS) sensors
    filter_standalone_sensors();

    // Create screen from XML
    screen_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_filament_sensor_select", nullptr));
    if (!screen_root_) {
        spdlog::error("[{}] Failed to create screen from XML", get_name());
        ui_notification_error(
            lv_tr("Wizard Error"),
            lv_tr("Failed to load filament sensor configuration screen. Please restart the "
                  "application."));
        return nullptr;
    }

    // Restore selection from existing FilamentSensorManager config
    bool has_configured_runout = false;
    for (size_t i = 0; i < standalone_sensors_.size(); i++) {
        const auto& sensor = standalone_sensors_[i];
        int dropdown_index = static_cast<int>(i + 1); // +1 because index 0 is "None"

        if (sensor.role == helix::FilamentSensorRole::RUNOUT) {
            lv_subject_set_int(&runout_sensor_selected_, dropdown_index);
            has_configured_runout = true;
            spdlog::debug("[{}] Restored RUNOUT sensor from config: {}", get_name(),
                          sensor.sensor_name);
            break;
        }
    }

    // If no sensor is configured with RUNOUT role, try to guess the best one
    if (!has_configured_runout && !standalone_sensors_.empty()) {
        std::vector<std::string> sensor_names;
        sensor_names.reserve(standalone_sensors_.size());
        for (const auto& s : standalone_sensors_) {
            sensor_names.push_back(s.sensor_name);
            spdlog::debug("[{}] Sensor candidate for guessing: {}", get_name(), s.sensor_name);
        }

        std::string guess = PrinterHardware::guess_runout_sensor(sensor_names);
        spdlog::debug("[{}] guess_runout_sensor returned: '{}'", get_name(),
                      guess.empty() ? "(empty)" : guess);

        if (!guess.empty()) {
            // Find the index of the guessed sensor
            for (size_t i = 0; i < standalone_sensors_.size(); i++) {
                if (standalone_sensors_[i].sensor_name == guess) {
                    int dropdown_index = static_cast<int>(i + 1); // +1 because index 0 is "None"
                    lv_subject_set_int(&runout_sensor_selected_, dropdown_index);
                    spdlog::info("[{}] Auto-selected runout sensor: {} (index {})", get_name(),
                                 guess, dropdown_index);
                    break;
                }
            }
        }
    } else if (standalone_sensors_.empty()) {
        spdlog::debug("[{}] No standalone sensors available for guessing", get_name());
    }

    // Populate dropdowns
    populate_dropdowns();

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Refresh
// ============================================================================

void WizardFilamentSensorSelectStep::schedule_deferred_refresh() {
    // Re-arming must not stack a second one-shot behind a still-pending one.
    cancel_refresh_timer();
    refresh_timer_ = lv_timer_create(refresh_timer_cb, 1500, this);
    lv_timer_set_repeat_count(refresh_timer_, 1); // One-shot timer
}

void WizardFilamentSensorSelectStep::cancel_refresh_timer() {
    // Neuter rather than delete: cleanup() can run from inside lv_timer_handler
    // (a Next click), where deleting a timer corrupts LVGL's timer list.
    // lv_timer_cancel_safe() also no-ops when LVGL is already gone, so this is
    // safe from the destructor as well.
    if (refresh_timer_ && lv_is_initialized()) {
        helix::ui::lv_timer_cancel_safe(refresh_timer_);
    }
    refresh_timer_ = nullptr;
}

void WizardFilamentSensorSelectStep::refresh_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<WizardFilamentSensorSelectStep*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }
    // The timer is one-shot — LVGL deletes it immediately after this callback
    // returns. Null the member FIRST so no later cancel can touch freed memory.
    self->refresh_timer_ = nullptr;
    self->refresh();
}

void WizardFilamentSensorSelectStep::refresh() {
    if (!screen_root_) {
        return; // No screen to refresh
    }

    // Re-filter sensors (may have been discovered since create())
    size_t old_count = standalone_sensors_.size();
    filter_standalone_sensors();

    // Nothing changed — don't repopulate and disrupt user interaction
    if (old_count == standalone_sensors_.size()) {
        spdlog::debug("[{}] Refresh: sensor count unchanged ({}), skipping repopulate", get_name(),
                      old_count);
        return;
    }

    spdlog::info("[{}] Sensor count changed ({} -> {}), refreshing dropdown", get_name(), old_count,
                 standalone_sensors_.size());

    // If sensors were just discovered and none selected, run auto-selection
    if (old_count == 0 && !standalone_sensors_.empty()) {
        if (lv_subject_get_int(&runout_sensor_selected_) == 0) {
            std::vector<std::string> sensor_names;
            sensor_names.reserve(standalone_sensors_.size());
            for (const auto& s : standalone_sensors_) {
                sensor_names.push_back(s.sensor_name);
            }

            std::string guess = PrinterHardware::guess_runout_sensor(sensor_names);
            if (!guess.empty()) {
                for (size_t i = 0; i < standalone_sensors_.size(); i++) {
                    if (standalone_sensors_[i].sensor_name == guess) {
                        int dropdown_index = static_cast<int>(i + 1);
                        lv_subject_set_int(&runout_sensor_selected_, dropdown_index);
                        spdlog::info("[{}] Auto-selected runout sensor on refresh: {} (index {})",
                                     get_name(), guess, dropdown_index);
                        break;
                    }
                }
            }
        }
    }

    // Re-populate dropdown with new sensor list
    populate_dropdowns();
}

// ============================================================================
// Skip Logic
// ============================================================================

size_t WizardFilamentSensorSelectStep::get_standalone_sensor_count() const {
    // Query FilamentSensorManager directly as the single source of truth
    // This works even when the step is skipped and create() was never called
    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    auto all_sensors = sensor_mgr.get_sensors();

    size_t count = 0;
    for (const auto& sensor : all_sensors) {
        if (!is_ams_sensor(sensor.sensor_name)) {
            count++;
        }
    }
    return count;
}

bool WizardFilamentSensorSelectStep::should_skip() const {
    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    auto all_sensors = sensor_mgr.get_sensors();

    // Count non-AMS sensors from the single source of truth
    size_t standalone_count = 0;
    for (const auto& sensor : all_sensors) {
        if (!is_ams_sensor(sensor.sensor_name)) {
            standalone_count++;
        }
    }

    spdlog::debug("[{}] should_skip: {} standalone sensors", get_name(), standalone_count);
    return standalone_count < 2;
}

void WizardFilamentSensorSelectStep::auto_configure_single_sensor() {
    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    auto all_sensors = sensor_mgr.get_sensors();

    // Find the single non-AMS sensor
    for (const auto& sensor : all_sensors) {
        if (!is_ams_sensor(sensor.sensor_name)) {
            spdlog::info("[{}] Auto-configuring single sensor '{}' as RUNOUT", get_name(),
                         sensor.sensor_name);
            sensor_mgr.set_sensor_role(sensor.klipper_name, helix::FilamentSensorRole::RUNOUT);
            sensor_mgr.save_config_to_file();
            return;
        }
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardFilamentSensorSelectStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    // A pending refresh must never fire into a screen that navigation is
    // tearing down; the shared helper neuters instead of unlinking because
    // cleanup can run from inside lv_timer_handler (via a click event).
    cancel_refresh_timer();

    auto& sensor_mgr = helix::FilamentSensorManager::instance();

    if (subjects_initialized_) {
        // Clear existing RUNOUT role assignments first
        for (const auto& sensor : standalone_sensors_) {
            if (sensor.role == helix::FilamentSensorRole::RUNOUT) {
                sensor_mgr.set_sensor_role(sensor.klipper_name, helix::FilamentSensorRole::NONE);
            }
        }

        // Apply new role assignment based on dropdown selection
        std::string runout_name =
            get_klipper_name_for_index(lv_subject_get_int(&runout_sensor_selected_));

        if (!runout_name.empty()) {
            sensor_mgr.set_sensor_role(runout_name, helix::FilamentSensorRole::RUNOUT);
            spdlog::info("[{}] Assigned RUNOUT role to: {}", get_name(), runout_name);
        }

        // Persist to disk
        sensor_mgr.save_config_to_file();
    } else {
        spdlog::warn("[{}] Subjects not initialized, skipping role assignment", get_name());
    }

    // Reset UI references
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Validation
// ============================================================================

bool WizardFilamentSensorSelectStep::is_validated() const {
    // Always return true for baseline implementation
    return true;
}
