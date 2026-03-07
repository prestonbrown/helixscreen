// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_summary.h"

#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_wizard.h"
#include "ui_wizard_input_shaper.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "lvgl/lvgl.h"
#include "static_panel_registry.h"
#include "system_settings_manager.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>
#include <sstream>
#include <string>

using namespace helix;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardSummaryStep> g_wizard_summary_step;

WizardSummaryStep* get_wizard_summary_step() {
    if (!g_wizard_summary_step) {
        g_wizard_summary_step = std::make_unique<WizardSummaryStep>();
        StaticPanelRegistry::instance().register_destroy("WizardSummaryStep",
                                                         []() { g_wizard_summary_step.reset(); });
    }
    return g_wizard_summary_step.get();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardSummaryStep::WizardSummaryStep() {
    // Zero-initialize buffers
    std::memset(printer_name_buffer_, 0, sizeof(printer_name_buffer_));
    std::memset(printer_type_buffer_, 0, sizeof(printer_type_buffer_));
    std::memset(wifi_ssid_buffer_, 0, sizeof(wifi_ssid_buffer_));
    std::memset(moonraker_connection_buffer_, 0, sizeof(moonraker_connection_buffer_));
    std::memset(bed_buffer_, 0, sizeof(bed_buffer_));
    std::memset(hotend_buffer_, 0, sizeof(hotend_buffer_));
    std::memset(part_fan_buffer_, 0, sizeof(part_fan_buffer_));
    std::memset(hotend_fan_buffer_, 0, sizeof(hotend_fan_buffer_));
    std::memset(led_strip_buffer_, 0, sizeof(led_strip_buffer_));
    std::memset(filament_sensor_buffer_, 0, sizeof(filament_sensor_buffer_));
    std::memset(ams_type_buffer_, 0, sizeof(ams_type_buffer_));
    std::memset(input_shaper_buffer_, 0, sizeof(input_shaper_buffer_));
    std::memset(telemetry_info_text_buffer_, 0, sizeof(telemetry_info_text_buffer_));

    spdlog::debug("[{}] Instance created", get_name());
}

WizardSummaryStep::~WizardSummaryStep() {
    // NOTE: Do NOT call LVGL functions here - LVGL may be destroyed first
    // NOTE: Do NOT log here - spdlog may be destroyed first
    screen_root_ = nullptr;
}

// ============================================================================
// Move Semantics
// ============================================================================

// ============================================================================
// Helper Functions
// ============================================================================

std::string WizardSummaryStep::format_bed_summary() {
    Config* config = Config::get_instance();
    std::stringstream ss;

    std::string heater = config->get<std::string>(config->df() + helix::wizard::BED_HEATER, "");
    std::string sensor = config->get<std::string>(config->df() + helix::wizard::BED_SENSOR, "");

    ss << "Heater: " << (heater.empty() ? "None" : heater);
    ss << ", Sensor: " << (sensor.empty() ? "None" : sensor);

    return ss.str();
}

std::string WizardSummaryStep::format_hotend_summary() {
    Config* config = Config::get_instance();
    std::stringstream ss;

    std::string heater = config->get<std::string>(config->df() + helix::wizard::HOTEND_HEATER, "");
    std::string sensor = config->get<std::string>(config->df() + helix::wizard::HOTEND_SENSOR, "");

    ss << "Heater: " << (heater.empty() ? "None" : heater);
    ss << ", Sensor: " << (sensor.empty() ? "None" : sensor);

    return ss.str();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardSummaryStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    // Load all values from config
    Config* config = Config::get_instance();

    // Printer name
    std::string printer_name =
        config ? config->get<std::string>(config->df() + helix::wizard::PRINTER_NAME,
                                          "Unnamed Printer")
               : "Unnamed Printer";
    spdlog::debug("[{}] Printer name from config: '{}'", get_name(), printer_name);

    // Printer type
    std::string printer_type =
        config ? config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "Unknown")
               : "Unknown";
    spdlog::debug("[{}] Printer type from config: '{}'", get_name(), printer_type);

    // WiFi SSID
    std::string wifi_ssid =
        config ? config->get<std::string>(helix::wizard::WIFI_SSID, "Not configured")
               : "Not configured";
    spdlog::debug("[{}] WiFi SSID from config: '{}'", get_name(), wifi_ssid);

    // Moonraker connection (host:port)
    std::string moonraker_host =
        config ? config->get<std::string>(config->df() + helix::wizard::MOONRAKER_HOST,
                                          "Not configured")
               : "Not configured";
    int moonraker_port =
        config ? config->get<int>(config->df() + helix::wizard::MOONRAKER_PORT, 7125) : 7125;
    spdlog::debug("[{}] Moonraker host from config: '{}', port: {}", get_name(), moonraker_host,
                  moonraker_port);
    std::string moonraker_connection;
    if (moonraker_host != "Not configured") {
        moonraker_connection = moonraker_host + ":" + std::to_string(moonraker_port);
    } else {
        moonraker_connection = "Not configured";
    }
    spdlog::debug("[{}] Moonraker connection: '{}'", get_name(), moonraker_connection);

    // Bed configuration
    std::string bed_summary = config ? format_bed_summary() : "Not configured";

    // Hotend configuration
    std::string hotend_summary = config ? format_hotend_summary() : "Not configured";

    // Part cooling fan
    std::string part_fan =
        config ? config->get<std::string>(config->df() + helix::wizard::PART_FAN, "") : "";
    int part_fan_visible = !part_fan.empty() ? 1 : 0;

    // Hotend cooling fan
    std::string hotend_fan =
        config ? config->get<std::string>(config->df() + helix::wizard::HOTEND_FAN, "") : "";
    int hotend_fan_visible = !hotend_fan.empty() ? 1 : 0;

    // LED strip
    std::string led_strip =
        config ? config->get<std::string>(config->df() + helix::wizard::LED_STRIP, "") : "";
    int led_strip_visible = !led_strip.empty() ? 1 : 0;

    // Filament sensor - get from FilamentSensorManager
    std::string filament_sensor = "None";
    int filament_sensor_visible = 0;
    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    auto sensors = sensor_mgr.get_sensors();
    for (const auto& sensor : sensors) {
        if (sensor.role == helix::FilamentSensorRole::RUNOUT) {
            filament_sensor = sensor.sensor_name + " (Runout)";
            filament_sensor_visible = 1;
            break;
        }
    }
    // If no runout, check for any assigned role
    if (filament_sensor_visible == 0) {
        for (const auto& sensor : sensors) {
            if (sensor.role != helix::FilamentSensorRole::NONE) {
                filament_sensor =
                    sensor.sensor_name + " (" + helix::role_to_display_string(sensor.role) + ")";
                filament_sensor_visible = 1;
                break;
            }
        }
    }

    // Initialize and register all subjects
    // NOTE: Pass std::string.c_str() as initial_value, NOT the buffer itself.
    // The macro copies initial_value to buffer - passing the same pointer for both
    // is undefined behavior (overlapping source/dest in snprintf).
    UI_SUBJECT_INIT_AND_REGISTER_STRING(printer_name_, printer_name_buffer_, printer_name.c_str(),
                                        "summary_printer_name");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(printer_type_, printer_type_buffer_, printer_type.c_str(),
                                        "summary_printer_type");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(wifi_ssid_, wifi_ssid_buffer_, wifi_ssid.c_str(),
                                        "summary_wifi_ssid");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(moonraker_connection_, moonraker_connection_buffer_,
                                        moonraker_connection.c_str(),
                                        "summary_moonraker_connection");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(bed_, bed_buffer_, bed_summary.c_str(), "summary_bed");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(hotend_, hotend_buffer_, hotend_summary.c_str(),
                                        "summary_hotend");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(part_fan_, part_fan_buffer_, part_fan.c_str(),
                                        "summary_part_fan");
    UI_SUBJECT_INIT_AND_REGISTER_INT(part_fan_visible_, part_fan_visible,
                                     "summary_part_fan_visible");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(hotend_fan_, hotend_fan_buffer_, hotend_fan.c_str(),
                                        "summary_hotend_fan");
    UI_SUBJECT_INIT_AND_REGISTER_INT(hotend_fan_visible_, hotend_fan_visible,
                                     "summary_hotend_fan_visible");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(led_strip_, led_strip_buffer_, led_strip.c_str(),
                                        "summary_led_strip");
    UI_SUBJECT_INIT_AND_REGISTER_INT(led_strip_visible_, led_strip_visible,
                                     "summary_led_strip_visible");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(filament_sensor_, filament_sensor_buffer_,
                                        filament_sensor.c_str(), "summary_filament_sensor");
    UI_SUBJECT_INIT_AND_REGISTER_INT(filament_sensor_visible_, filament_sensor_visible,
                                     "summary_filament_sensor_visible");

    // AMS/Multi-Material System
    std::string ams_type_str = "None";
    int ams_visible = 0;

    auto& ams = AmsState::instance();
    AmsBackend* backend = ams.get_backend();
    if (backend && backend->get_type() != AmsType::NONE) {
        AmsSystemInfo info = backend->get_system_info();
        // Format: "AFC • 4 lanes" or "Happy Hare • 8 lanes"
        ams_type_str = info.type_name;
        if (info.total_slots > 0) {
            ams_type_str += " • " + std::to_string(info.total_slots) + " lanes";
        }
        ams_visible = 1;
    }

    UI_SUBJECT_INIT_AND_REGISTER_STRING(ams_type_, ams_type_buffer_, ams_type_str.c_str(),
                                        "summary_ams_type");
    UI_SUBJECT_INIT_AND_REGISTER_INT(ams_visible_, ams_visible, "summary_ams_visible");

    // Input Shaping / Accelerometer
    std::string input_shaper_str = "Not available";
    int input_shaper_visible = 0;

    WizardInputShaperStep* is_step = get_wizard_input_shaper_step();
    if (is_step && is_step->has_accelerometer()) {
        input_shaper_visible = 1;
        if (is_step->is_calibration_complete()) {
            input_shaper_str = "Calibrated";
        } else {
            input_shaper_str = "Accelerometer detected (not calibrated)";
        }
    }

    UI_SUBJECT_INIT_AND_REGISTER_STRING(input_shaper_, input_shaper_buffer_,
                                        input_shaper_str.c_str(), "summary_input_shaper");
    UI_SUBJECT_INIT_AND_REGISTER_INT(input_shaper_visible_, input_shaper_visible,
                                     "summary_input_shaper_visible");

    // Telemetry info modal content
    static const char* telemetry_info_md =
        "**HelixScreen is a free, open-source project** built by a tiny team. "
        "Anonymous telemetry helps us understand how the app is actually used "
        "so we can focus on what matters.\n\n"
        "## What we collect\n"
        "- **App version** and platform (Pi model, screen size)\n"
        "- **Printer type** (kinematics, build volume — NOT your printer name)\n"
        "- **Print outcomes** (completed vs failed, duration, temps)\n"
        "- **Crash reports** (stack traces to fix bugs)\n"
        "- **Feature usage** (which panels you use, AMS, input shaper, etc.)\n\n"
        "## What we NEVER collect\n"
        "- Your name, location, or IP address\n"
        "- File names or G-code content\n"
        "- Camera images or thumbnails\n"
        "- WiFi passwords or network details\n"
        "- Anything that could identify you personally\n\n"
        "## Why it matters\n"
        "With just a few hundred users reporting anonymously, we can see which "
        "printers crash most, which features nobody uses, and where to spend our "
        "limited time. **You can view the exact data in Settings > View Telemetry "
        "Data anytime.**";
    UI_SUBJECT_INIT_AND_REGISTER_STRING(telemetry_info_text_, telemetry_info_text_buffer_,
                                        telemetry_info_md, "telemetry_info_text");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized with config values", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardSummaryStep::register_callbacks() {
    spdlog::debug("[{}] Registering callbacks", get_name());
    lv_xml_register_event_cb(nullptr, "on_wizard_telemetry_changed",
                             WizardSummaryStep::on_wizard_telemetry_changed);
    lv_xml_register_event_cb(nullptr, "on_wizard_telemetry_info",
                             WizardSummaryStep::on_wizard_telemetry_info);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardSummaryStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating summary screen", get_name());

    // Safety check: cleanup should have been called by wizard navigation
    if (screen_root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        screen_root_ = nullptr; // Reset pointer, wizard framework handles deletion
    }

    // Refresh subjects with latest config values before creating UI
    init_subjects();

    // Create screen from XML
    screen_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_summary", nullptr));
    if (!screen_root_) {
        spdlog::error("[{}] Failed to create screen from XML", get_name());
        return nullptr;
    }

    // Toggle state synced via bind_state_if_eq on settings_telemetry_enabled subject

    // Hide telemetry opt-in for subsequent printers (already shown on first wizard run)
    auto* cfg = Config::get_instance();
    if (cfg && cfg->get_printer_ids().size() > 1) {
        lv_obj_t* telemetry_row = lv_obj_find_by_name(screen_root_, "telemetry_opt_in_row");
        if (telemetry_row) {
            lv_obj_add_flag(telemetry_row, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_t* telemetry_divider = lv_obj_find_by_name(screen_root_, "telemetry_divider");
        if (telemetry_divider) {
            lv_obj_add_flag(telemetry_divider, LV_OBJ_FLAG_HIDDEN);
        }
    }

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardSummaryStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    // NOTE: Wizard framework handles object deletion - we only null the pointer
    // See HANDOFF.md Pattern #9: Wizard Screen Lifecycle
    screen_root_ = nullptr;
}

// ============================================================================
// Validation
// ============================================================================

bool WizardSummaryStep::is_validated() const {
    // Summary screen is always validated (no user input required)
    return true;
}

// ============================================================================
// Static Callbacks
// ============================================================================

void WizardSummaryStep::on_wizard_telemetry_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[WizardSummary] on_wizard_telemetry_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    spdlog::info("[WizardSummary] Telemetry toggled: {}", enabled ? "ON" : "OFF");
    SystemSettingsManager::instance().set_telemetry_enabled(enabled);
    if (enabled) {
        ToastManager::instance().show(
            ToastSeverity::SUCCESS,
            lv_tr("Thanks! Anonymous usage data helps improve HelixScreen."), 4000);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void WizardSummaryStep::on_wizard_telemetry_info(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[WizardSummary] on_wizard_telemetry_info");
    spdlog::debug("[WizardSummary] Showing telemetry info modal");
    lv_obj_t* dialog = Modal::show("telemetry_info_modal");
    if (dialog) {
        // Wire the OK button to close the modal
        lv_obj_t* ok_btn = lv_obj_find_by_name(dialog, "btn_primary");
        if (ok_btn) {
            lv_obj_add_event_cb(
                ok_btn,
                [](lv_event_t* ev) {
                    auto* dlg = static_cast<lv_obj_t*>(lv_event_get_user_data(ev));
                    Modal::hide(dlg);
                },
                LV_EVENT_CLICKED, dialog);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}
