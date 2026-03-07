// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_heater_select.h"

#include "ui_error_reporting.h"
#include "ui_notification.h"
#include "ui_wizard.h"
#include "ui_wizard_hardware_selector.h"
#include "ui_wizard_helpers.h"

#include "app_globals.h"
#include "config.h"
#include "lvgl/lvgl.h"
#include "moonraker_api.h"
#include "printer_hardware.h"
#include "static_panel_registry.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace helix;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardHeaterSelectStep> g_wizard_heater_select_step;

WizardHeaterSelectStep* get_wizard_heater_select_step() {
    if (!g_wizard_heater_select_step) {
        g_wizard_heater_select_step = std::make_unique<WizardHeaterSelectStep>();
        StaticPanelRegistry::instance().register_destroy(
            "WizardHeaterSelectStep", []() { g_wizard_heater_select_step.reset(); });
    }
    return g_wizard_heater_select_step.get();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardHeaterSelectStep::WizardHeaterSelectStep() {
    spdlog::debug("[{}] Instance created", get_name());
}

WizardHeaterSelectStep::~WizardHeaterSelectStep() {
    // NOTE: Do NOT call LVGL functions here - LVGL may be destroyed first
    // NOTE: Do NOT log here - spdlog may be destroyed first
    screen_root_ = nullptr;
}

// ============================================================================
// ============================================================================
// Subject Initialization
// ============================================================================

void WizardHeaterSelectStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize subjects with default index 0
    // Actual selection will be restored from config during create() after hardware is discovered
    helix::ui::wizard::init_int_subject(&bed_heater_selected_, 0, "bed_heater_selected");
    helix::ui::wizard::init_int_subject(&hotend_heater_selected_, 0, "hotend_heater_selected");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Static Callbacks (XML event_cb pattern)
// ============================================================================

static void on_bed_heater_dropdown_changed(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto* step = get_wizard_heater_select_step();
    if (step) {
        lv_subject_set_int(step->get_bed_heater_subject(), index);
        spdlog::debug("[WizardHeaterSelectStep] Bed heater selection changed to index {}", index);
    }
}

static void on_hotend_heater_dropdown_changed(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto* step = get_wizard_heater_select_step();
    if (step) {
        lv_subject_set_int(step->get_hotend_heater_subject(), index);
        spdlog::debug("[WizardHeaterSelectStep] Hotend heater selection changed to index {}",
                      index);
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardHeaterSelectStep::register_callbacks() {
    // Register XML event callbacks for dropdown value changes
    lv_xml_register_event_cb(nullptr, "on_bed_heater_dropdown_changed",
                             on_bed_heater_dropdown_changed);
    lv_xml_register_event_cb(nullptr, "on_hotend_heater_dropdown_changed",
                             on_hotend_heater_dropdown_changed);
    spdlog::debug("[{}] Registered dropdown callbacks", get_name());
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardHeaterSelectStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating heater select screen", get_name());

    // Safety check: cleanup should have been called by wizard navigation
    if (screen_root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        screen_root_ = nullptr; // Reset pointer, wizard framework handles deletion
    }

    // Create screen from XML
    screen_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_heater_select", nullptr));
    if (!screen_root_) {
        spdlog::error("[{}] Failed to create screen from XML", get_name());
        ui_notification_error(
            "Wizard Error",
            "Failed to load heater configuration screen. Please restart the application.");
        return nullptr;
    }

    // Populate bed heater dropdown (discover + filter + populate + restore)
    // Event handler is wired via XML <event_cb>
    wizard_populate_hardware_dropdown(
        screen_root_, "bed_heater_dropdown", &bed_heater_selected_, bed_heater_items_,
        [](MoonrakerAPI* a) -> const auto& { return a->hardware().heaters(); },
        "bed", // Filter for bed-related heaters
        true,  // Allow "None" option
        helix::wizard::BED_HEATER, [](const PrinterHardware& hw) { return hw.guess_bed_heater(); },
        "[Wizard Heater]", helix::DeviceType::HEATER);

    // Populate hotend heater dropdown (discover + filter + populate + restore)
    // Event handler is wired via XML <event_cb>
    wizard_populate_hardware_dropdown(
        screen_root_, "hotend_heater_dropdown", &hotend_heater_selected_, hotend_heater_items_,
        [](MoonrakerAPI* a) -> const auto& { return a->hardware().heaters(); },
        "extruder", // Filter for extruder-related heaters
        true,       // Allow "None" option
        helix::wizard::HOTEND_HEATER,
        [](const PrinterHardware& hw) { return hw.guess_hotend_heater(); }, "[Wizard Heater]",
        helix::DeviceType::HEATER);

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardHeaterSelectStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::error("[{}] Config instance not available!", get_name());
        return;
    }

    // Save bed heater selection
    // Store the heater name to BOTH heater and sensor paths (Klipper heaters provide temp readings)
    helix::ui::wizard::save_dropdown_selection(&bed_heater_selected_, bed_heater_items_,
                                               helix::wizard::BED_HEATER, "[Wizard Heater]");

    // Get the selected bed heater name and also save it as the sensor
    int32_t bed_idx = lv_subject_get_int(&bed_heater_selected_);
    if (bed_idx >= 0 && static_cast<size_t>(bed_idx) < bed_heater_items_.size()) {
        const std::string& bed_heater_name = bed_heater_items_[static_cast<size_t>(bed_idx)];
        config->set<std::string>(config->df() + helix::wizard::BED_SENSOR, bed_heater_name);
        spdlog::debug("[{}] Bed sensor set to: {}", get_name(), bed_heater_name);
    }

    // Save hotend heater selection
    // Store the heater name to BOTH heater and sensor paths
    helix::ui::wizard::save_dropdown_selection(&hotend_heater_selected_, hotend_heater_items_,
                                               helix::wizard::HOTEND_HEATER, "[Wizard Heater]");

    // Get the selected hotend heater name and also save it as the sensor
    int32_t hotend_idx = lv_subject_get_int(&hotend_heater_selected_);
    if (hotend_idx >= 0 && static_cast<size_t>(hotend_idx) < hotend_heater_items_.size()) {
        const std::string& hotend_heater_name =
            hotend_heater_items_[static_cast<size_t>(hotend_idx)];
        config->set<std::string>(config->df() + helix::wizard::HOTEND_SENSOR, hotend_heater_name);
        spdlog::debug("[{}] Hotend sensor set to: {}", get_name(), hotend_heater_name);
    }

    // Persist to disk
    if (!config->save()) {
        NOTIFY_ERROR("Failed to save heater configuration");
    }

    // Reset UI references
    // Note: Do NOT call lv_obj_del() here - the wizard framework handles
    // object deletion when clearing wizard_content container
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Validation
// ============================================================================

bool WizardHeaterSelectStep::is_validated() const {
    // Always return true for baseline implementation
    return true;
}
