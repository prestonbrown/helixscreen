// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_state.cpp
 * @brief Reactive printer state model with LVGL subjects for all printer data
 *
 * @pattern Singleton with set_*() -> set_*_internal() for thread-safe updates
 * @threading Public setters called from WebSocket; internal setters run on main thread
 * @gotchas Static string buffers; init subjects before XML; temps in centidegrees
 *
 * @see moonraker_client.cpp, ui_update_queue.h
 */

#include "printer_state.h"

#include "ui_update_queue.h"

#include "accel_sensor_manager.h"
#include "async_helpers.h"
#include "capability_overrides.h"
#include "color_sensor_manager.h"
#include "device_display_name.h"
#include "filament_sensor_manager.h"
#include "hardware_validator.h"
#include "humidity_sensor_manager.h"
#include "led/led_controller.h"
#include "lvgl.h"
#include "lvgl/src/display/lv_display_private.h" // For rendering_in_progress check
#include "lvgl_debug_invalidate.h"
#include "moonraker_client.h" // For ConnectionState enum
#include "probe_sensor_manager.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "static_subject_registry.h"
#include "temperature_sensor_manager.h"
#include "timelapse_state.h"
#include "unit_conversions.h"
#include "width_sensor_manager.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// ============================================================================
// PrintJobState Free Functions
// ============================================================================

namespace helix {

PrintJobState parse_print_job_state(const char* state_str) {
    if (!state_str) {
        return PrintJobState::STANDBY;
    }

    if (std::strcmp(state_str, "standby") == 0) {
        return PrintJobState::STANDBY;
    } else if (std::strcmp(state_str, "printing") == 0) {
        return PrintJobState::PRINTING;
    } else if (std::strcmp(state_str, "paused") == 0) {
        return PrintJobState::PAUSED;
    } else if (std::strcmp(state_str, "complete") == 0) {
        return PrintJobState::COMPLETE;
    } else if (std::strcmp(state_str, "cancelled") == 0) {
        return PrintJobState::CANCELLED;
    } else if (std::strcmp(state_str, "error") == 0) {
        return PrintJobState::ERROR;
    }

    // Unknown state defaults to STANDBY
    spdlog::warn("[PrinterState] Unknown print state string: '{}', defaulting to STANDBY",
                 state_str);
    return PrintJobState::STANDBY;
}

const char* print_job_state_to_string(PrintJobState state) {
    switch (state) {
    case PrintJobState::STANDBY:
        return "Standby";
    case PrintJobState::PRINTING:
        return "Printing";
    case PrintJobState::PAUSED:
        return "Paused";
    case PrintJobState::COMPLETE:
        return "Complete";
    case PrintJobState::CANCELLED:
        return "Cancelled";
    case PrintJobState::ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}

} // namespace helix

using namespace helix;

// ============================================================================
// PrinterState Implementation
// ============================================================================

PrinterState::PrinterState() {
    // Note: String buffer initialization is now handled by component classes:
    // - homed_axes_buf_ is now in motion_state_ component
    // - print-related buffers are now in print_domain_ component
    // - printer_connection_message_buf_ is now in network_state_ component
    // - klipper_version_buf_, moonraker_version_buf_ are now in versions_state_ component

    // Load user-configured capability overrides from helixconfig.json
    capability_overrides_.load_from_config();
}

PrinterState::~PrinterState() {}

void PrinterState::deinit_subjects() {
    if (!subjects_initialized_) {
        spdlog::trace(
            "[PrinterState] deinit_subjects: subjects not initialized, nothing to deinit");
        return;
    }

    spdlog::trace("[PrinterState] deinit_subjects: Deinitializing all subjects");

    // Deinit all sub-component subjects
    temperature_state_.deinit_subjects();
    motion_state_.deinit_subjects();
    led_state_component_.deinit_subjects();
    fan_state_.deinit_subjects();
    print_domain_.deinit_subjects();
    capabilities_state_.deinit_subjects();
    plugin_status_state_.deinit_subjects();
    calibration_state_.deinit_subjects();
    hardware_validation_state_.deinit_subjects();
    composite_visibility_state_.deinit_subjects();
    network_state_.deinit_subjects();
    versions_state_.deinit_subjects();
    excluded_objects_state_.deinit_subjects();

    // Deinit PrinterState's own subjects (multi-printer)
    lv_subject_deinit(&active_printer_name_);
    lv_subject_deinit(&multi_printer_enabled_);
    subjects_.deinit_all();

    subjects_initialized_ = false;
}

void PrinterState::register_temperature_xml_subjects() {
    temperature_state_.register_xml_subjects();
}

void PrinterState::init_subjects(bool register_xml) {
    // Detect LVGL reinitialization - display pointer changes when lv_init() called again
    // This happens in test suites where each test reinitializes LVGL but the PrinterState
    // singleton persists. Without this check, subjects would point to freed memory.
    lv_display_t* current_display = lv_display_get_default();

    if (subjects_initialized_) {
        if (current_display != cached_display_) {
            // LVGL was reinitialized - our subjects are now invalid
            spdlog::warn("[PrinterState] LVGL reinitialized (display changed), resetting subjects");
            deinit_subjects();
        } else {
            spdlog::debug("[PrinterState] Subjects already initialized, skipping");
            return;
        }
    }

    cached_display_ = current_display;

    spdlog::trace("[PrinterState] Initializing subjects (register_xml={})", register_xml);

    // Initialize temperature state component (extruder and bed temperatures)
    temperature_state_.init_subjects(register_xml);

    // Initialize motion state component (position, speed/flow, z-offset)
    motion_state_.init_subjects(register_xml);

    // Initialize LED state component (RGBW channels, brightness, on/off state)
    led_state_component_.init_subjects(register_xml);

    // Initialize fan state component (fan speed, multi-fan tracking)
    fan_state_.init_subjects(register_xml);

    // Initialize print state component (progress, state, timing, layers, print start)
    print_domain_.init_subjects(register_xml);

    // Initialize capabilities state component (hardware capabilities, feature availability)
    capabilities_state_.init_subjects(register_xml);

    // Note: Print subjects are now initialized by print_domain_.init_subjects() above

    // Note: Motion subjects (position_x_, position_y_, position_z_, homed_axes_,
    // speed_factor_, flow_factor_, gcode_z_offset_, pending_z_offset_delta_)
    // are now initialized by motion_state_.init_subjects() above

    // Note: Fan subjects (fan_speed_, fans_version_) are now initialized by
    // fan_state_.init_subjects() above

    // Note: Capability subjects (printer_has_qgl_, printer_has_z_tilt_, etc.)
    // are now initialized by capabilities_state_.init_subjects() above

    // Initialize network state component (connection, klippy, nav buttons)
    network_state_.init_subjects(register_xml);

    // Note: LED subjects are initialized by led_state_component_.init_subjects() above

    // Excluded objects state component (excluded_objects_version, excluded_objects set)
    excluded_objects_state_.init_subjects(register_xml);

    // Plugin status subjects - delegated to plugin_status_state_ component
    plugin_status_state_.init_subjects(register_xml);

    // Calibration state subjects (firmware retraction, manual probe, motor state)
    calibration_state_.init_subjects(register_xml);

    // Hardware validation subjects (for Hardware Health section in Settings)
    hardware_validation_state_.init_subjects(register_xml);

    // Composite visibility subjects (derived can_show_* subjects)
    composite_visibility_state_.init_subjects(register_xml);

    // Note: Hardware validation subjects are now initialized by
    // hardware_validation_state_.init_subjects() above

    // Note: Firmware retraction, manual probe, and motor state subjects
    // are now initialized by calibration_state_.init_subjects() above

    // Version subjects (for About section) - delegated to versions_state_ component
    versions_state_.init_subjects(register_xml);

    // Register all subjects with SubjectManager for automatic cleanup
    // Note: Temperature subjects are managed by temperature_state_ component
    // Note: Print subjects are managed by print_domain_ component
    // Note: Motion subjects are registered by motion_state_ component
    // Note: Fan subjects are registered by fan_state_ component
    // Note: Capability subjects are managed by capabilities_state_ component
    // Note: Network subjects are registered by network_state_.init_subjects()
    // Note: LED subjects are registered by led_state_component_.init_subjects()
    // Note: Excluded objects subjects are registered by excluded_objects_state_.init_subjects()
    // Note: Plugin status subjects are registered by plugin_status_state_.init_subjects()
    // Note: Composite visibility subjects are registered by
    // composite_visibility_state_.init_subjects() Note: Hardware validation subjects are registered
    // by hardware_validation_state_.init_subjects() Note: Firmware retraction, manual probe, and
    // motor state subjects are registered by calibration_state_.init_subjects()
    // Note: Version subjects are registered by versions_state_.init_subjects()

    // Multi-printer subjects (owned directly by PrinterState)
    INIT_SUBJECT_STRING(active_printer_name, "", subjects_, register_xml);
    INIT_SUBJECT_INT(multi_printer_enabled, 0, subjects_, register_xml);

    spdlog::trace("[PrinterState] Registered {} subjects with SubjectManager", subjects_.count());

    // Register all subjects with LVGL XML system (CRITICAL for XML bindings)
    // Note: Temperature subjects are registered by temperature_state_ component
    // Note: Print subjects are registered by print_domain_ component
    // Note: Motion subjects are registered by motion_state_ component
    // Note: Fan subjects are registered by fan_state_ component
    // Note: Capability subjects are registered by capabilities_state_ component
    // Note: Network subjects are registered by network_state_.init_subjects()
    // Note: LED subjects are registered by led_state_component_.init_subjects()
    // Note: Plugin status subjects are registered by plugin_status_state_.init_subjects()
    // Note: Composite visibility subjects are registered by
    // composite_visibility_state_.init_subjects() Note: Hardware validation subjects are registered
    // by hardware_validation_state_.init_subjects() Note: Firmware retraction, manual probe, and
    // motor state subjects are registered by calibration_state_.init_subjects()
    // Note: Version subjects are registered by versions_state_.init_subjects()
    // Note: Excluded objects subjects are registered by excluded_objects_state_.init_subjects()
    // All component subjects handle their own XML registration in init_subjects(register_xml)

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit("PrinterState",
                                                      [this]() { deinit_subjects(); });

    spdlog::trace("[PrinterState] Subjects initialized and registered successfully");
}

void PrinterState::update_from_notification(const json& notification) {
    // Moonraker notifications have structure:
    // {"method": "notify_status_update", "params": [{...printer state...}, eventtime]}

    if (!notification.contains("method") || !notification.contains("params")) {
        return;
    }

    std::string method = notification["method"].get<std::string>();
    if (method != "notify_status_update") {
        return;
    }

    // Extract printer state from params[0] and delegate to update_from_status
    // CRITICAL: Defer to main thread via ui_queue_update to avoid LVGL assertion
    // when subject updates trigger lv_obj_invalidate() during rendering
    auto params = notification["params"];
    if (params.is_array() && !params.empty()) {
        helix::ui::queue_update([this, state_json = params[0]]() {
            // Debug check: log if we're somehow in render phase (should never happen)
            if (lvgl_is_rendering()) {
                spdlog::error("[PrinterState] async status update running during render phase!");
                spdlog::error("[PrinterState] This should not happen - lv_async_call should run "
                              "between frames");
            }
            update_from_status(state_json);
        });
    }
}

void PrinterState::update_from_status(const json& state) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Debug: Check if we're in render phase (this should never be true)
    LV_DEBUG_RENDER_STATE();

    // Delegate temperature updates to temperature state component
    temperature_state_.update_from_status(state);

    // Delegate motion updates to motion state component
    motion_state_.update_from_status(state);

    // Delegate print updates to print state component
    print_domain_.update_from_status(state);

    // Note: Toolhead position, homed_axes, speed_factor, flow_factor, and gcode_z_offset
    // are now updated by motion_state_.update_from_status() above

    // Extract kinematics type (determines if bed moves on Z or gantry moves)
    // This is not part of motion_state_ as it affects printer_bed_moves_ subject
    if (state.contains("toolhead")) {
        const auto& toolhead = state["toolhead"];
        if (toolhead.contains("kinematics") && toolhead["kinematics"].is_string()) {
            std::string kin = toolhead["kinematics"].get<std::string>();
            set_kinematics(kin);
        }

        // Track active extruder from toolhead (for tool changers and multi-extruder setups)
        if (toolhead.contains("extruder") && toolhead["extruder"].is_string()) {
            std::string active_ext = toolhead["extruder"].get<std::string>();
            temperature_state_.set_active_extruder(active_ext);
        }
    }

    // Delegate fan state updates to fan component
    fan_state_.update_from_status(state);

    // Delegate LED state updates to LED component
    led_state_component_.update_from_status(state);

    // Update LED controller per-strip color cache
    auto& led_ctrl = helix::led::LedController::instance();
    if (led_ctrl.is_initialized()) {
        led_ctrl.native().update_from_status(state);
        led_ctrl.effects().update_from_status(state);
        led_ctrl.output_pin().update_from_status(state);
    }

    // Update exclude_object state (for mid-print object exclusion)
    if (state.contains("exclude_object")) {
        const auto& eo = state["exclude_object"];

        if (eo.contains("excluded_objects") && eo["excluded_objects"].is_array()) {
            std::unordered_set<std::string> excluded;
            for (const auto& obj : eo["excluded_objects"]) {
                if (obj.is_string()) {
                    excluded.insert(obj.get<std::string>());
                }
            }
            // set_excluded_objects handles change detection and notification
            // Note: We're inside state_mutex_ lock, but set_excluded_objects only modifies
            // its own data and calls lv_subject_set_int which is safe
            set_excluded_objects(excluded);
        }

        // Parse defined objects list
        if (eo.contains("objects") && eo["objects"].is_array()) {
            std::vector<std::string> defined;
            for (const auto& obj : eo["objects"]) {
                if (obj.is_object() && obj.contains("name") && obj["name"].is_string()) {
                    defined.push_back(obj["name"].get<std::string>());
                }
            }
            excluded_objects_state_.set_defined_objects(defined);
        }

        // Parse current object
        if (eo.contains("current_object")) {
            if (eo["current_object"].is_string()) {
                excluded_objects_state_.set_current_object(eo["current_object"].get<std::string>());
            } else if (eo["current_object"].is_null()) {
                excluded_objects_state_.set_current_object("");
            }
        }
    }

    // Update klippy state from webhooks (for restart simulation)
    if (state.contains("webhooks")) {
        const auto& webhooks = state["webhooks"];
        if (webhooks.contains("state") && webhooks["state"].is_string()) {
            std::string klippy_state_str = webhooks["state"].get<std::string>();
            KlippyState new_state = KlippyState::READY; // default

            if (klippy_state_str == "ready") {
                new_state = KlippyState::READY;
            } else if (klippy_state_str == "startup") {
                new_state = KlippyState::STARTUP;
            } else if (klippy_state_str == "shutdown") {
                new_state = KlippyState::SHUTDOWN;
            } else if (klippy_state_str == "error") {
                new_state = KlippyState::ERROR;
            }

            network_state_.set_klippy_state_internal(new_state);
            spdlog::debug("[PrinterState] Klippy state from webhooks: {}", klippy_state_str);
        }
    }

    // Delegate calibration updates (manual probe, motor state, firmware retraction)
    // to calibration_state_ component
    calibration_state_.update_from_status(state);

    // Forward filament sensor updates to FilamentSensorManager
    // The manager handles all sensor types: filament_switch_sensor and filament_motion_sensor
    helix::FilamentSensorManager::instance().update_from_status(state);

    // Forward updates to all other sensor managers
    helix::sensors::HumiditySensorManager::instance().update_from_status(state);
    helix::sensors::WidthSensorManager::instance().update_from_status(state);
    helix::sensors::ProbeSensorManager::instance().update_from_status(state);
    helix::sensors::AccelSensorManager::instance().update_from_status(state);
    helix::sensors::ColorSensorManager::instance().update_from_status(state);
    helix::sensors::TemperatureSensorManager::instance().update_from_status(state);

}

void PrinterState::reset_for_new_print() {
    print_domain_.reset_for_new_print();
    helix::TimelapseState::instance().reset();
}

// Note: Multi-fan tracking (init_fans, update_fan_speed, get_fan_speed_subject) is now
// delegated to fan_state_ component. See printer_fan_state.cpp.

void PrinterState::set_printer_connection_state(int state, const char* message) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    std::string msg = message ? message : "";
    helix::ui::queue_update(
        [this, state, msg]() { set_printer_connection_state_internal(state, msg.c_str()); });
}

void PrinterState::set_printer_connection_state_internal(int state, const char* message) {
    // Delegate to network_state_ component
    network_state_.set_printer_connection_state_internal(state, message);
}

void PrinterState::set_network_status(int status) {
    // Delegate to network_state_ component
    network_state_.set_network_status(status);
}

void PrinterState::set_klippy_state(KlippyState state) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    helix::async::call_method(this, &PrinterState::set_klippy_state_internal, state);
}

void PrinterState::set_klippy_state_sync(KlippyState state) {
    // Direct call for main-thread use (testing, or when already on main thread)
    set_klippy_state_internal(state);
}

void PrinterState::set_klippy_state_internal(KlippyState state) {
    // Delegate to network_state_ component
    network_state_.set_klippy_state_internal(state);
}

void PrinterState::update_nav_buttons_enabled() {
    // Delegate to network_state_ component
    network_state_.update_nav_buttons_enabled();
}

void PrinterState::set_print_in_progress(bool in_progress) {
    print_domain_.set_print_in_progress(in_progress);
}

// Note: set_tracked_led() is now delegated to led_state_component_ in the header

void PrinterState::set_hardware(const helix::PrinterDiscovery& hardware) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    helix::async::call_method_ref(this, &PrinterState::set_hardware_internal, hardware);
}

void PrinterState::set_hardware_internal(const helix::PrinterDiscovery& hardware) {
    // Pass auto-detected hardware to the override layer
    capability_overrides_.set_hardware(hardware);

    // Delegate capability subject updates to capabilities_state_ component
    capabilities_state_.set_hardware(hardware, capability_overrides_);

    // Set kinematics from hardware discovery (configfile.config.printer.kinematics)
    // This is more reliable than toolhead status, which returns null on some printers
    if (!hardware.kinematics().empty()) {
        set_kinematics(hardware.kinematics());
    }

    // Tell temperature state which sensor/heater to use for chamber temperature
    temperature_state_.set_chamber_sensor_name(hardware.chamber_sensor_name());
    temperature_state_.set_chamber_heater_name(hardware.chamber_heater_name());

    // Update composite subjects for G-code modification options
    // (visibility depends on both plugin status and capability)
    update_gcode_modification_visibility();
}

void PrinterState::set_klipper_version(const std::string& version) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    helix::async::call_method_ref(this, &PrinterState::set_klipper_version_internal, version);
}

void PrinterState::set_klipper_version_internal(const std::string& version) {
    versions_state_.set_klipper_version_internal(version);
}

void PrinterState::set_moonraker_version(const std::string& version) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    helix::async::call_method_ref(this, &PrinterState::set_moonraker_version_internal, version);
}

void PrinterState::set_moonraker_version_internal(const std::string& version) {
    versions_state_.set_moonraker_version_internal(version);
}

void PrinterState::set_os_version(const std::string& version) {
    helix::async::call_method_ref(this, &PrinterState::set_os_version_internal, version);
}

void PrinterState::set_os_version_internal(const std::string& version) {
    versions_state_.set_os_version_internal(version);
}

void PrinterState::set_power_device_count(int count) {
    // Delegate to capabilities_state_ component (handles thread-safety)
    capabilities_state_.set_power_device_count(count);
}

void PrinterState::set_spoolman_available(bool available) {
    // Delegate to capabilities_state_ component (handles thread-safety)
    capabilities_state_.set_spoolman_available(available);
}

void PrinterState::set_webcam_available(bool available, const std::string& stream_url,
                                        const std::string& snapshot_url,
                                        bool flip_h, bool flip_v) {
    // Delegate to capabilities_state_ component (handles thread-safety)
    capabilities_state_.set_webcam_available(available, stream_url, snapshot_url, flip_h, flip_v);
}

void PrinterState::set_timelapse_available(bool available) {
    // Delegate to capabilities_state_ component (handles thread-safety internally)
    capabilities_state_.set_timelapse_available(available);
    // Recompute aggregate visibility (timelapse affects has_any_preprint_options)
    helix::ui::queue_update([this]() { update_gcode_modification_visibility(); });
}

void PrinterState::set_helix_plugin_installed(bool installed) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    // We handle the async dispatch here because we need to update composite subjects after
    helix::ui::queue_update([this, installed]() {
        plugin_status_state_.set_installed(installed);

        // Update composite subjects for G-code modification options
        update_gcode_modification_visibility();
    });
}

bool PrinterState::service_has_helix_plugin() const {
    // Delegate to plugin_status_state_ component
    return plugin_status_state_.service_has_helix_plugin();
}

void PrinterState::set_phase_tracking_enabled(bool enabled) {
    // Delegate to plugin_status_state_ component (handles async dispatch internally)
    plugin_status_state_.set_phase_tracking_enabled(enabled);
}

bool PrinterState::is_phase_tracking_enabled() const {
    // Delegate to plugin_status_state_ component
    return plugin_status_state_.is_phase_tracking_enabled();
}

void PrinterState::update_gcode_modification_visibility() {
    // Delegate to composite visibility component
    bool plugin = plugin_status_state_.service_has_helix_plugin();
    composite_visibility_state_.update_visibility(plugin, capabilities_state_);
}

// Note: update_print_show_progress() is now in print_domain_ component

void PrinterState::set_excluded_objects(const std::unordered_set<std::string>& objects) {
    excluded_objects_state_.set_excluded_objects(objects);
}

PrintJobState PrinterState::get_print_job_state() const {
    return print_domain_.get_print_job_state();
}

bool PrinterState::can_start_new_print() const {
    return print_domain_.can_start_new_print();
}

int PrinterState::get_configured_z_offset_microns() {
    if (has_probe()) {
        // Probe printers: z_offset stored in ProbeSensorManager (already in microns)
        return lv_subject_get_int(
            helix::sensors::ProbeSensorManager::instance().get_probe_z_offset_subject());
    }
    // Endstop printers: position_endstop from configfile.settings
    return capabilities_state_.get_stepper_z_endstop_microns();
}

void PrinterState::set_kinematics(const std::string& kinematics) {
    if (kinematics == last_kinematics_) {
        return;
    }
    last_kinematics_ = kinematics;

    // Determine if the bed moves on Z based on kinematics type:
    // - CoreXY: bed typically moves on Z (Voron 0/Trident, Bambu, AD5M, etc.)
    //   Exception: Voron 2.4 and similar with quad_gantry_level have gantry-Z
    // - CoreXZ: gantry moves on Z (Voron Switchwire, etc.) — NOT bed-moves
    // - Cartesian: gantry typically moves on Z (Ender 3, Prusa i3, etc.)
    // - Delta: effector moves on Z, bed is stationary
    bool is_corexy_family = (kinematics.find("corexy") != std::string::npos);

    // CoreXY with QGL = gantry moves on Z (e.g. Voron 2.4), otherwise bed moves
    bool has_qgl = lv_subject_get_int(capabilities_state_.get_printer_has_qgl_subject()) != 0;
    auto_detected_bed_moves_ = is_corexy_family && !has_qgl;

    // Apply with user override considered
    apply_effective_bed_moves();
}

void PrinterState::apply_effective_bed_moves() {
    auto style = SettingsManager::instance().get_z_movement_style();
    bool effective;

    switch (style) {
    case ZMovementStyle::BED_MOVES:
        effective = true;
        break;
    case ZMovementStyle::NOZZLE_MOVES:
        effective = false;
        break;
    case ZMovementStyle::AUTO:
    default:
        effective = auto_detected_bed_moves_;
        break;
    }

    capabilities_state_.set_bed_moves(effective);
    spdlog::debug("[PrinterState] apply_effective_bed_moves: style={}, auto={}, effective={}",
                  static_cast<int>(style), auto_detected_bed_moves_, effective);
}

// Note: Pending Z-offset delta methods are now delegated to motion_state_
// component in the header file.

// ============================================================================
// PRINT START PROGRESS TRACKING - Delegated to print_domain_
// ============================================================================

bool PrinterState::is_in_print_start() const {
    return print_domain_.is_in_print_start();
}

void PrinterState::set_print_start_state(PrintStartPhase phase, const char* message, int progress) {
    print_domain_.set_print_start_state(phase, message, progress);
}

void PrinterState::reset_print_start_state() {
    print_domain_.reset_print_start_state();
}

void PrinterState::set_print_thumbnail_path(const std::string& path) {
    print_domain_.set_print_thumbnail_path(path);
}

void PrinterState::set_print_display_filename(const std::string& name) {
    print_domain_.set_print_display_filename(name);
}

// ============================================================================
// HARDWARE VALIDATION - Delegated to hardware_validation_state_
// ============================================================================

void PrinterState::set_hardware_validation_result(const HardwareValidationResult& result) {
    hardware_validation_state_.set_hardware_validation_result(result);
}

void PrinterState::remove_hardware_issue(const std::string& hardware_name) {
    hardware_validation_state_.remove_hardware_issue(hardware_name);
}

void PrinterState::set_print_outcome(PrintOutcome outcome) {
    print_domain_.set_print_outcome(outcome);
}

// ============================================================================
// PRINTER TYPE AND PRINT START CAPABILITIES
// ============================================================================

void PrinterState::set_printer_type(const std::string& type) {
    // Thread-safe wrapper: defer updates to main thread
    helix::async::call_method_ref(this, &PrinterState::set_printer_type_internal, type);
}

void PrinterState::set_printer_type_sync(const std::string& type) {
    // Direct call for main-thread use (testing, or when already on main thread)
    set_printer_type_internal(type);
}

void PrinterState::set_printer_type_internal(const std::string& type) {
    // Determine what the z-cal strategy would be for this type so we can
    // skip redundant updates (auto-detect often confirms the saved type).
    auto new_caps = PrinterDetector::get_print_start_capabilities(type);
    std::string strategy_str = PrinterDetector::get_z_offset_calibration_strategy(type);
    ZOffsetCalibrationStrategy new_strategy;
    if (strategy_str == "gcode_offset") {
        new_strategy = ZOffsetCalibrationStrategy::GCODE_OFFSET;
    } else if (strategy_str == "endstop") {
        new_strategy = ZOffsetCalibrationStrategy::ENDSTOP;
    } else if (strategy_str == "probe_calibrate") {
        new_strategy = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    } else {
        new_strategy = capabilities_state_.has_probe()
                           ? ZOffsetCalibrationStrategy::PROBE_CALIBRATE
                           : ZOffsetCalibrationStrategy::ENDSTOP;
    }

    if (type == printer_type_ && new_strategy == z_offset_calibration_strategy_) {
        return;
    }

    printer_type_ = type;
    print_start_capabilities_ = new_caps;
    z_offset_calibration_strategy_ = new_strategy;

    // Update printer_has_purge_line_ based on capabilities database
    // "priming" is the capability key for purge/prime line in the database
    bool has_priming = print_start_capabilities_.get_capability("priming") != nullptr;
    capabilities_state_.set_purge_line(has_priming);

    // Recalculate composite visibility subjects
    update_gcode_modification_visibility();

    const char* strategy_names[] = {"probe_calibrate", "gcode_offset", "endstop"};
    spdlog::info(
        "[PrinterState] Printer type set to: '{}' (capabilities: {}, priming={}, z_cal={})", type,
        print_start_capabilities_.empty() ? "none" : print_start_capabilities_.macro_name,
        has_priming, strategy_names[static_cast<int>(z_offset_calibration_strategy_)]);
}

const std::string& PrinterState::get_printer_type() const {
    return printer_type_;
}

const PrintStartCapabilities& PrinterState::get_print_start_capabilities() const {
    return print_start_capabilities_;
}

ZOffsetCalibrationStrategy PrinterState::get_z_offset_calibration_strategy() const {
    return z_offset_calibration_strategy_;
}

// ============================================================================
// MULTI-PRINTER STATE
// ============================================================================

void PrinterState::set_active_printer_name(const std::string& name) {
    lv_subject_copy_string(&active_printer_name_, name.c_str());
    spdlog::debug("[PrinterState] Active printer name set to: '{}'", name);
}

void PrinterState::set_multi_printer_enabled(bool enabled) {
    lv_subject_set_int(&multi_printer_enabled_, enabled ? 1 : 0);
    spdlog::debug("[PrinterState] Multi-printer enabled: {}", enabled);
}
