// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_state.cpp
 * @brief Reactive printer state model with LVGL subjects for all printer data
 *
 * @pattern Singleton with set_*() -> set_*_internal() for thread-safe updates
 * @threading Public setters called from WebSocket; internal setters run on main thread
 * @gotchas Static string buffers; init subjects before XML; temps in decidegrees
 *
 * @see moonraker_client.cpp, ui_update_queue.h
 */

#include "printer_state.h"

#include "ui_update_queue.h"

#include "accel_sensor_manager.h"
#include "async_helpers.h"
#include "capability_overrides.h"
#include "color_sensor_manager.h"
#include "connection_state.h" // For ConnectionState enum
#include "device_display_name.h"
#include "filament_sensor_manager.h"
#include "hardware_validator.h"
#include "humidity_sensor_manager.h"
#include "i_moonraker_client.h" // for helix::CACHED_SNAPSHOT_MARKER
#include "json_utils.h"
#include "led/led_controller.h"
#include "lvgl.h"
#include "lvgl/src/display/lv_display_private.h" // For rendering_in_progress check
#include "lvgl_debug_invalidate.h"
#include "printer_cache_registry.h"
#include "probe_sensor_manager.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "static_subject_registry.h"
#include "system/crash_handler.h"
#include "temperature_sensor_manager.h"
#include "timelapse_state.h"
#include "unit_conversions.h"
#include "width_sensor_manager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

// ============================================================================
// PrintJobState Free Functions
// ============================================================================

namespace helix {

PrintJobState parse_print_job_state(const char* state_str) {
    if (!state_str) {
        return PrintJobState::STANDBY;
    }

    // RAW_PRINT_STATE_OK: whole function. This IS the wire parse. Everything
    // downstream that wants the derived axis gets it from derive_print_state().
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
    // RAW_PRINT_STATE_OK: whole function - the wire enum's own name table.
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

    // Load user-configured capability overrides from settings.json
    capability_overrides_.load_from_config();
}

PrinterState::~PrinterState() {
    // Backstop for the path that skips deinit_subjects() entirely: the subjects
    // are members, so they die with this object even though nothing called
    // lv_subject_deinit() on them. Any ObserverGuard still holding one would
    // otherwise reset() against freed storage. Flipping costs nothing when
    // deinit_subjects() already ran — that installed a fresh token, and flipping
    // it just tells holders registered since then that the state is gone too.
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
    }
}

void PrinterState::deinit_subjects() {
    if (!subjects_initialized_) {
        spdlog::trace(
            "[PrinterState] deinit_subjects: subjects not initialized, nothing to deinit");
        return;
    }

    spdlog::trace("[PrinterState] deinit_subjects: Deinitializing all subjects");

    // Expire any setter callbacks still queued on the UpdateQueue. They capture
    // `this` and touch the subjects torn down below (directly or through
    // apply_dynamic_options()); without this the next drain notifies a freed
    // observer list (#1165, #1146).
    async_lifetime_.invalidate();

    // Drop the per-printer cache invalidator registered by init_subjects(). It captures
    // `this`, and the registry outlives non-singleton instances (test fixtures own their
    // own PrinterState), so leaving it registered would hold a callback over freed memory.
    helix::PrinterCacheRegistry::instance().unregister("PrinterState");

    // Signal death of EVERY subject below BEFORE anything is torn down. Observers
    // held by objects that outlive this call — process-lifetime panel singletons,
    // most of all — check this token in ObserverGuard::reset() and skip
    // lv_observer_remove() on observer nodes that lv_subject_deinit() is about to
    // free. Flipping the bool (rather than only dropping our shared_ptr) is what
    // makes it work when a holder still has a copy: expired() would report false,
    // but the value tells them the subject is gone. Same contract as
    // PrinterPrintState's static_subjects_lifetime_.
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
    }
    // Install a fresh live token rather than clearing the member: an empty token
    // reads as "dead" in ObserverGuard::reset() and would make every observer
    // registered after this point skip its removal (see the member's comment).
    subjects_lifetime_ = std::make_shared<bool>(true);

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

    // NOTE: subjects_lifetime_ is deliberately NOT refreshed here. It is created
    // with the object and replaced by deinit_subjects(), so the token covering
    // the subjects below is already live and already handed to any observer that
    // subscribed before this call. Minting a new one here would strand those
    // holders on a token that never flips false.

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

    // has_any_preprint_options aggregate (per-op can_show_* subjects retired)
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

    // Z-offset save visibility (1 = manual save needed, 0 = firmware auto-saves)
    INIT_SUBJECT_INT(z_offset_can_save, 1, subjects_, register_xml);

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

    // Self-register per-printer cache invalidation. capability_overrides_ is loaded from
    // Config::df() in the constructor only, and this object outlives every printer switch,
    // so without this the map keeps the startup printer's enable/disable choices.
    helix::PrinterCacheRegistry::instance().register_invalidator(
        "PrinterState", [this]() { reload_capability_overrides(); });

    spdlog::trace("[PrinterState] Subjects initialized and registered successfully");
}

void PrinterState::reload_capability_overrides() {
    // Only the override map is refreshed. The effective capability subjects are re-derived
    // from set_hardware(discovery_, capability_overrides_) when the new printer's discovery
    // lands; deriving them here would pair the new printer's overrides with the OLD
    // printer's still-cached discovery_.
    capability_overrides_.load_from_config();
}

void PrinterState::update_from_notification(const json& notification) {
    // Moonraker notifications have structure:
    // {"method": "notify_status_update", "params": [{...printer state...}, eventtime]}

    auto method_it = notification.find("method");
    if (method_it == notification.end() || !method_it->is_string() ||
        !notification.contains("params")) {
        return;
    }

    std::string method = method_it->get<std::string>();
    if (method != "notify_status_update") {
        return;
    }

    // Extract printer state from params[0] and delegate to update_from_status
    // CRITICAL: Defer to main thread via ui_queue_update to avoid LVGL assertion
    // when subject updates trigger lv_obj_invalidate() during rendering
    auto params = notification["params"];
    if (params.is_array() && !params.empty()) {
        // params[1] is Klipper's eventtime. It is monotonic-clock derived, so it
        // survives a Klipper restart and only rewinds on a host reboot — a usable
        // freshness key within one connection. Absent or non-numeric means the
        // frame was synthesized rather than received.
        const double eventtime =
            (params.size() > 1 && params[1].is_number()) ? params[1].get<double>() : 0.0;
        const bool from_cached_snapshot = notification.value(helix::CACHED_SNAPSHOT_MARKER, false);
        async_lifetime_.defer("PrinterState::on_status_update", [this, state_json = params[0],
                                                                 eventtime,
                                                                 from_cached_snapshot]() {
            // Debug check: log if we're somehow in render phase (should never happen)
            if (lvgl_is_rendering()) {
                spdlog::error("[PrinterState] async status update running during render phase!");
                spdlog::error("[PrinterState] This should not happen - lv_async_call should run "
                              "between frames");
            }
            update_from_status(state_json, eventtime, from_cached_snapshot);
        });
    }
}

void PrinterState::update_from_status(const json& state, double eventtime,
                                      bool from_cached_snapshot) {
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

    // Update exclude_object state (for mid-print object exclusion). The inner
    // setters (set_excluded_objects / set_defined_objects_with_geometry /
    // set_current_object) already log on actual change.
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

        // Parse defined objects list with geometry (center + polygon bounding box)
        if (eo.contains("objects") && eo["objects"].is_array()) {
            std::vector<PrinterExcludedObjectsState::ObjectInfo> objects;
            for (const auto& obj : eo["objects"]) {
                if (!obj.is_object() || !obj.contains("name"))
                    continue;

                PrinterExcludedObjectsState::ObjectInfo info;
                info.name = obj["name"].get<std::string>();

                if (obj.contains("center") && obj["center"].is_array() &&
                    obj["center"].size() >= 2 && obj["center"][0].is_number() &&
                    obj["center"][1].is_number()) {
                    info.center.x = obj["center"][0].get<float>();
                    info.center.y = obj["center"][1].get<float>();
                    info.has_center = true;
                } else {
                    info.has_center = false;
                }

                if (obj.contains("polygon") && obj["polygon"].is_array() &&
                    !obj["polygon"].empty()) {
                    float min_x = std::numeric_limits<float>::max();
                    float min_y = min_x;
                    float max_x = std::numeric_limits<float>::lowest();
                    float max_y = max_x;
                    for (const auto& pt : obj["polygon"]) {
                        if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() &&
                            pt[1].is_number()) {
                            float x = pt[0].get<float>(), y = pt[1].get<float>();
                            info.polygon.push_back({x, y});
                            min_x = std::min(min_x, x);
                            min_y = std::min(min_y, y);
                            max_x = std::max(max_x, x);
                            max_y = std::max(max_y, y);
                        }
                    }
                    info.bbox_min = {min_x, min_y};
                    info.bbox_max = {max_x, max_y};
                    info.has_bbox = true;
                } else {
                    info.has_bbox = false;
                }

                objects.push_back(std::move(info));
            }
            excluded_objects_state_.set_defined_objects_with_geometry(objects);
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

    // Update klippy state from webhooks (shutdown/error detection).
    //
    // Klippy state is a liveness signal written by two queues that are not ordered
    // against each other: live WebSocket frames, and the discovery subscription
    // snapshot replayed at the end of discovery. Last-write-wins let the replay
    // resurrect READY over a live SHUTDOWN — nav re-enabled, the recovery dialog
    // auto-dismissed, and the gcode guards re-opened against a dead printer.
    //
    // The state_message is gated with the state because they arrive in the same
    // blob: a snapshot too stale to set the state carries an equally stale reason.
    if (state.contains("webhooks")) {
        const auto& webhooks = state["webhooks"];

        // Provenance is STATED by the caller, never inferred from a zero eventtime:
        // the mock client drives its simulated shutdown/recovery through the same
        // untimestamped dispatch, and those are the current truth for their session.
        // The eventtime watermark covers the other case — two genuinely live frames
        // arriving out of order across the queues.
        const bool stale = (from_cached_snapshot && klippy_state_from_live_) ||
                           (eventtime > 0.0 && eventtime < klippy_state_eventtime_);

        if (stale) {
            spdlog::debug("[PrinterState] Ignoring stale klippy webhooks (state='{}', "
                          "eventtime={} vs watermark={}, cached_snapshot={})",
                          helix::json_util::safe_string(webhooks, "state", "<absent>"), eventtime,
                          klippy_state_eventtime_, from_cached_snapshot);
        } else {
            bool applied_state = false;

            if (webhooks.contains("state") && webhooks["state"].is_string()) {
                std::string klippy_state_str = webhooks["state"].get<std::string>();
                KlippyState new_state = KlippyState::READY;
                bool recognized = true;

                if (klippy_state_str == "ready") {
                    new_state = KlippyState::READY;
                } else if (klippy_state_str == "startup") {
                    new_state = KlippyState::STARTUP;
                } else if (klippy_state_str == "shutdown") {
                    new_state = KlippyState::SHUTDOWN;
                } else if (klippy_state_str == "error") {
                    new_state = KlippyState::ERROR;
                } else {
                    // Klipper documents exactly ready/startup/shutdown/error. An
                    // unrecognised value used to resolve to READY, which is
                    // fail-OPEN on a liveness signal: an unknown string re-enabled
                    // nav and re-opened the gcode guards. Leave the current state
                    // alone instead — a stale-but-known state is safer than an
                    // invented READY. Deduped on the string so a value Klipper
                    // repeats every frame warns once, not per frame.
                    recognized = false;
                    if (last_unknown_klippy_state_ != klippy_state_str) {
                        last_unknown_klippy_state_ = klippy_state_str;
                        spdlog::warn("[PrinterState] Unrecognised webhooks.state '{}' — leaving "
                                     "klippy state unchanged",
                                     klippy_state_str);
                    }
                }

                if (recognized) {
                    set_klippy_state_internal(new_state);
                    applied_state = true;
                }
            }

            // Capture state_message (error/shutdown reason text)
            if (webhooks.contains("state_message") && webhooks["state_message"].is_string()) {
                network_state_.set_klippy_state_message(
                    webhooks["state_message"].get<std::string>());
            }

            // Only a frame that actually carried a usable state moves the guard.
            // A delta carrying just state_message must not latch "live seen" and
            // lock out the snapshot that still has to seed the state.
            if (applied_state) {
                if (eventtime > 0.0) {
                    klippy_state_eventtime_ = eventtime;
                }
                if (!from_cached_snapshot) {
                    klippy_state_from_live_ = true;
                }
            }
        }
    }

    // Track Klipper pause_resume.is_paused (PAUSE/RESUME gcode state)
    if (state.contains("pause_resume")) {
        const auto& pr = state["pause_resume"];
        if (pr.contains("is_paused") && pr["is_paused"].is_boolean()) {
            is_paused_ = pr["is_paused"].get<bool>();
        }
    }

    // Delegate calibration updates (manual probe, motor state, firmware retraction)
    // to calibration_state_ component
    calibration_state_.update_from_status(state);

    // Re-arm the once-per-episode busy-queue toast when the COMPOSITE blocking
    // condition has cleared — never on an individual signal's falling edge. A
    // manual-probe session whose idle_timeout bounces to "Ready" between TESTZ moves
    // is still one blocking episode; keying off idle_timeout alone would re-toast
    // mid-episode (#1108 review). is_blocking_operation_active() sees the just-updated
    // manual_probe / idle_timeout / print-job subjects. The store is idempotent, so
    // gating on the predicate needs no separate edge tracking.
    if (!is_blocking_operation_active()) {
        calibration_state_.arm_busy_queue_toast();
    }

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
    async_lifetime_.defer("PrinterState::set_printer_connection_state", [this, state, msg]() {
        set_printer_connection_state_internal(state, msg.c_str());
    });
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
    // These are the notify_klippy_ready / _shutdown / _disconnected paths: live,
    // authoritative, and they must outrank any replayed snapshot from here on.
    mark_klippy_state_live();

    // Thread-safe wrapper: defer LVGL subject updates to main thread
    helix::async::call_method(this, &PrinterState::set_klippy_state_internal, state);
}

void PrinterState::set_klippy_state_sync(KlippyState state) {
    mark_klippy_state_live();

    // Direct call for main-thread use (testing, or when already on main thread)
    set_klippy_state_internal(state);
}

void PrinterState::set_klippy_state_if_unseeded(KlippyState state) {
    // Deferred so the "has anything live landed?" check runs on the main thread,
    // in the same serialized order as the webhooks parse. Checking on the caller's
    // thread would race: a live frame could land between the check and the apply,
    // and printer.info's older answer would win anyway.
    helix::async::call_method(this, &PrinterState::set_klippy_state_if_unseeded_internal, state);
}

void PrinterState::set_klippy_state_if_unseeded_internal(KlippyState state) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (klippy_state_from_live_) {
            spdlog::debug("[PrinterState] Ignoring printer.info klippy state {} — a live state "
                          "has already been applied",
                          static_cast<int>(state));
            return;
        }
    }

    // Deliberately does NOT mark the state live: printer.info is a seed, and the
    // subscription snapshot that follows it on the same connection is strictly
    // newer, so it must still be allowed to correct this value.
    set_klippy_state_internal(state);
}

void PrinterState::mark_klippy_state_live() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    klippy_state_from_live_ = true;
}

void PrinterState::reset_klippy_state_freshness() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    klippy_state_eventtime_ = 0.0;
    klippy_state_from_live_ = false;
}

void PrinterState::set_klippy_state_internal(KlippyState state) {
    // Single chokepoint for every Klippy state change: the webhooks JSON parse, the
    // helix::async::call_method wrapper, and set_klippy_state_sync all land here.
    const bool changed = network_state_.set_klippy_state_internal(state);
    if (!changed) {
        return;
    }

    // Any transition invalidates state cached from Klipper's DELTA-only status
    // fields. Both directions matter: READY -> dead means nothing it was doing
    // survives; dead -> READY means a fresh Klipper with nothing blocking yet.
    // Without this, an idle_timeout captured mid-G28 outlived the restart and made
    // the app queue discretionary G-code fire-and-forget against an idle printer,
    // wedging the LED in-flight counter for the whole session (#1129).
    calibration_state_.reset_klippy_volatile();
}

void PrinterState::update_nav_buttons_enabled() {
    // Delegate to network_state_ component
    network_state_.update_nav_buttons_enabled();
}

void PrinterState::set_print_in_progress(bool in_progress) {
    print_domain_.set_print_in_progress(in_progress);
}

// Note: set_tracked_led() is now delegated to led_state_component_ in the header

void PrinterState::set_hardware(helix::PrinterDiscovery hardware) {
    // Called directly from the main LVGL thread (hardware discovery callback).
    // No deferral needed — the caller is already inside a queue_update callback.
    // Taken by value so we own a stable copy — the caller's source may alias
    // api->hardware_ which can be written by other queued callbacks (#799).
    static int s_set_hardware_n = 0;
    long sh_n = static_cast<long>(++s_set_hardware_n);
    crash_handler::breadcrumb::note("disc", "sh_entry",
                                    static_cast<long>(hardware.macros().size()));

    spdlog::debug("[PrinterState] set_hardware: has_probe={}", hardware.has_probe());

    // Store for later access by UI (e.g., chamber assignment dropdowns)
    discovery_ = std::move(hardware);
    crash_handler::breadcrumb::note("disc", "sh_moved",
                                    static_cast<long>(discovery_.macros().size()));

    // Pass auto-detected hardware to the override layer
    crash_handler::breadcrumb::note("disc", "pre_co_set", sh_n);
    capability_overrides_.set_hardware(discovery_);
    crash_handler::breadcrumb::note("disc", "post_co_set", sh_n);

    // Delegate capability subject updates to capabilities_state_ component
    capabilities_state_.set_hardware(discovery_, capability_overrides_);

    // Re-synthesize dynamic pre-print options now that hardware capabilities are
    // known. The bed_mesh option's adaptive_active flag (which relabels it to
    // "Adaptive Bed Mesh" and enables the adaptive param) depends on
    // discovery_.has_exclude_object(), which only becomes true here —
    // set_printer_type_internal() ran earlier (before hardware), so the option
    // would otherwise stay "Auto Bed Mesh".
    apply_dynamic_options();

    // Set kinematics from hardware discovery (configfile.config.printer.kinematics)
    // This is more reliable than toolhead status, which returns null on some printers
    if (!discovery_.kinematics().empty()) {
        set_kinematics(discovery_.kinematics());
    }

    // Resolve chamber assignments: manual override > auto-detection
    auto& settings = helix::SettingsManager::instance();

    std::string chamber_sensor = settings.get_chamber_sensor_assignment();
    if (chamber_sensor == "auto") {
        chamber_sensor = discovery_.chamber_sensor_name();
    } else if (chamber_sensor == "none") {
        chamber_sensor = "";
    }

    std::string chamber_heater = settings.get_chamber_heater_assignment();
    if (chamber_heater == "auto") {
        chamber_heater = discovery_.chamber_heater_name();
    } else if (chamber_heater == "none") {
        chamber_heater = "";
    }

    spdlog::debug("[PrinterState] Chamber resolved: sensor='{}' heater='{}'", chamber_sensor,
                  chamber_heater);
    temperature_state_.set_chamber_sensor_name(chamber_sensor);
    temperature_state_.set_chamber_heater_name(chamber_heater);
    // Cooling-fan name has no manual override — it's read straight from discovery.
    // In COOLING mode the K2 M141 macro parks the setpoint on this fan's target.
    temperature_state_.set_chamber_cooling_fan_name(discovery_.chamber_cooling_fan_name());
    // Cooling fan's configured resting/off target (from configfile.settings). M141
    // S0 returns the fan here, so the chamber mode treats this value as Off rather
    // than a deliberate "Maintaining" set.
    temperature_state_.set_chamber_fan_resting(discovery_.chamber_fan_resting_deci());

    // Update capability flags based on resolved chamber assignments
    // (set_hardware above used discovery flags which miss manual overrides)
    capabilities_state_.set_has_chamber_sensor(!chamber_sensor.empty());
    capabilities_state_.set_has_chamber_heater(!chamber_heater.empty());

    // Promote the resolved chamber sensor to CHAMBER role in the sensor
    // manager. Required for vendors whose chamber sensor name doesn't match
    // the "chamber" substring used by the manager's auto-categorizer
    // (Snapmaker uses "cavity", Elegoo "enclosure"). Without this promotion,
    // the temp graph would add the sensor twice — once as "Chamber" (from
    // PrinterTemperatureState::chamber_sensor_name) and once under its raw
    // display name (because the AUXILIARY role isn't filtered out).
    auto& temp_mgr = helix::sensors::TemperatureSensorManager::instance();
    temp_mgr.apply_chamber_sensor_override(chamber_sensor);

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

void PrinterState::set_sensor_count(int count) {
    // Delegate to capabilities_state_ component (handles thread-safety)
    capabilities_state_.set_sensor_count(count);
}

void PrinterState::set_spoolman_available(bool available) {
    // Delegate to capabilities_state_ component (handles thread-safety)
    capabilities_state_.set_spoolman_available(available);
}

void PrinterState::set_webcam_available(bool available, const std::string& stream_url,
                                        const std::string& snapshot_url, bool flip_h, bool flip_v,
                                        int target_fps) {
    // Delegate to capabilities_state_ component (handles thread-safety)
    capabilities_state_.set_webcam_available(available, stream_url, snapshot_url, flip_h, flip_v,
                                             target_fps);
}

void PrinterState::set_timelapse_available(bool available) {
    // Delegate to capabilities_state_ component (handles thread-safety internally)
    capabilities_state_.set_timelapse_available(available);
    // Resynthesize the option set (timelapse is appended dynamically when
    // available) and recompute aggregate visibility — both must run on the
    // main thread.
    async_lifetime_.defer("PrinterState::set_timelapse_available", [this]() {
        apply_dynamic_options();
        update_gcode_modification_visibility();
    });
}

void PrinterState::set_timelapse_default_enabled(bool enabled) {
    // Seed the timelapse pre-print option's default from the global
    // moonraker-timelapse `enabled` setting. Both the member write and the
    // resynthesis must run on the main thread (#1094).
    async_lifetime_.defer("PrinterState::set_timelapse_default_enabled", [this, enabled]() {
        timelapse_default_enabled_ = enabled;
        apply_dynamic_options();
        update_gcode_modification_visibility();
    });
}

void PrinterState::set_helix_plugin_installed(bool installed) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    // We handle the async dispatch here because we need to update composite subjects after
    async_lifetime_.defer("PrinterState::set_helix_plugin_installed", [this, installed]() {
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
    composite_visibility_state_.update_visibility(plugin, capabilities_state_,
                                                  pre_print_option_set_.options.size());
}

// Note: update_print_show_progress() is now in print_domain_ component

void PrinterState::set_excluded_objects(const std::unordered_set<std::string>& objects) {
    excluded_objects_state_.set_excluded_objects(objects);
}

PrintJobState PrinterState::get_print_job_state() const {
    return print_domain_.get_print_job_state();
}

bool PrinterState::is_blocking_operation_active() {
    // Interactive manual probe (PROBE_CALIBRATE / Z_ENDSTOP_CALIBRATE): always
    // blocking. idle_timeout may bounce to Ready between TESTZ commands, so this
    // is tracked independently. This deliberately takes precedence over the
    // file-print exclusion below — a manual probe and a running file print are
    // mutually exclusive in Klipper, so there is no real case where this would
    // wrongly block mid-print.
    if (lv_subject_get_int(calibration_state_.get_manual_probe_active_subject()) != 0) {
        return true;
    }

    // idle_timeout.state == "Printing" is Klipper's canonical busy flag. It is
    // also true during a real file print, so exclude PRINTING/PAUSED — mid-print
    // fan/temp changes are legitimate and Klipper queues them between moves.
    //
    // Debounced, not the raw subject: the flag is equally true for a one-shot
    // housekeeping macro, and a printer with delayed_gcode loops would otherwise
    // refuse a jog for ~7% of its idle life (bundle L53W5PKG).
    if (!calibration_state_.idle_timeout_busy().blocking()) {
        return false;
    }

    // RAW_PRINT_STATE_OK: this predicate is INVERTED — the print state is used
    // to SUPPRESS the blocking answer, not to assert it — so job_holds_machine()
    // would flip it the wrong way. During a host-side pre-print block
    // idle_timeout reads "Printing" (the host is running G-code) while
    // print_stats still reads standby, and answering "blocked" there is correct:
    // the toolhead really is busy. Widening to Preparing would make this return
    // false and ADMIT jogs during the bed mesh.
    const PrintJobState pstate = get_print_job_state();
    return pstate != PrintJobState::PRINTING && pstate != PrintJobState::PAUSED;
}

bool PrinterState::is_external_blocking_operation_active() {
    // Manual probe is an absolute block: TESTZ sessions must never accept
    // jog gcode regardless of how recently the app itself sent motion.
    if (lv_subject_get_int(calibration_state_.get_manual_probe_active_subject()) != 0) {
        return true;
    }
    if (!is_blocking_operation_active()) {
        return false;
    }
    // idle_timeout == "Printing" during any move, including our own jog. If the
    // app has motion in flight (or acked within the grace window), the busy-ness
    // is self-inflicted — let discretionary gcode through so jogs don't self-block.
    return !app_motion_activity_.recently_active();
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

void PrinterState::set_print_thumbnail(const std::string& for_file, const std::string& path) {
    print_domain_.set_print_thumbnail(for_file, path);
}

#if defined(HELIX_PLATFORM_ESP32)
void PrinterState::set_print_psram_thumbnail(std::shared_ptr<helix::ui::EspPsramThumbnail> thumb) {
    print_domain_.set_print_psram_thumbnail(std::move(thumb));
}
#endif

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
    auto new_options = PrinterDetector::get_pre_print_option_set(type);
    std::string strategy_str = PrinterDetector::get_z_offset_calibration_strategy(type);
    ZOffsetCalibrationStrategy new_strategy;
    if (strategy_str == "firmware_managed") {
        new_strategy = ZOffsetCalibrationStrategy::FIRMWARE_MANAGED;
    } else if (strategy_str == "endstop") {
        new_strategy = ZOffsetCalibrationStrategy::ENDSTOP;
    } else if (strategy_str == "probe_calibrate") {
        new_strategy = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    } else {
        new_strategy = capabilities_state_.has_probe() ? ZOffsetCalibrationStrategy::PROBE_CALIBRATE
                                                       : ZOffsetCalibrationStrategy::ENDSTOP;
    }

    if (type == printer_type_ && new_strategy == z_offset_calibration_strategy_) {
        return;
    }

    printer_type_ = type;
    pre_print_option_set_ = new_options;
    z_offset_calibration_strategy_ = new_strategy;

    // Synthesize runtime-dependent options (timelapse) on top of the DB load.
    apply_dynamic_options();

    // Update z_offset_can_save subject: 0 when firmware/macros auto-persist (FIRMWARE_MANAGED)
    int can_save = (new_strategy != ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) ? 1 : 0;
    if (subjects_initialized_ && lv_subject_get_int(&z_offset_can_save_) != can_save) {
        lv_subject_set_int(&z_offset_can_save_, can_save);
    }

    // Apply probe type override from database (e.g., prtouch_v2 for K1 series)
    std::string probe_type_str = PrinterDetector::get_probe_type(type);
    if (!probe_type_str.empty()) {
        auto probe_type = helix::sensors::probe_type_from_string(probe_type_str);
        if (probe_type != helix::sensors::ProbeSensorType::STANDARD) {
            helix::sensors::ProbeSensorManager::instance().set_probe_type_override(probe_type);
        }
    }

    // Update printer_has_purge_line_ based on the option set.
    // "priming" is the option id for purge/prime line in the database (also accept legacy
    // "nozzle_priming" as an alias).
    bool has_priming = (pre_print_option_set_.find("priming") != nullptr) ||
                       (pre_print_option_set_.find("nozzle_priming") != nullptr);
    capabilities_state_.set_purge_line(has_priming);

    // Recalculate composite visibility subjects
    update_gcode_modification_visibility();

    const char* strategy_names[] = {"probe_calibrate", "firmware_managed", "endstop"};
    spdlog::info(
        "[PrinterState] Printer type set to: '{}' (pre_print_options: {}, priming={}, z_cal={})",
        type, pre_print_option_set_.empty() ? "none" : pre_print_option_set_.macro_name,
        has_priming, strategy_names[static_cast<int>(z_offset_calibration_strategy_)]);
}

void PrinterState::apply_dynamic_options() {
    // Strip any previously synthesized dynamic options before re-adding so
    // this method is idempotent and handles capability changes (e.g.
    // moonraker-timelapse plugin going from absent to present).
    pre_print_option_set_.options.erase(
        std::remove_if(pre_print_option_set_.options.begin(), pre_print_option_set_.options.end(),
                       [](const PrePrintOption& opt) { return opt.id == "timelapse"; }),
        pre_print_option_set_.options.end());

    // Adaptive bed mesh: a property of the SINGLE bed_mesh toggle, not a separate
    // row. When ALL hold, the bed_mesh option is relabeled "Adaptive Bed Mesh"
    // and emits its adaptive token (e.g. ADAPTIVE=1) alongside the enable param
    // when ON; otherwise it stays the plain "Auto Bed Mesh" with unchanged
    // behavior. Conditions (so it's never a silent no-op):
    //   1. the bed_mesh option is a MacroParam declaring an adaptive_param (the
    //      START_PRINT forwarding signal — the macro passes the token into
    //      BED_MESH_CALIBRATE),
    //   2. the firmware exposes [exclude_object] (adaptive maps printed objects),
    //   3. no custom calibration.bed_mesh_gcode template is in use (that path
    //      runs verbatim and ignores ADAPTIVE).
    // Recomputed each run (idempotent, non-destructive) so it tracks capability
    // changes — e.g. exclude_object only becomes known once hardware arrives.
    for (auto& opt : pre_print_option_set_.options) {
        if (opt.id != "bed_mesh") {
            continue;
        }
        const auto* mp = std::get_if<PrePrintStrategyMacroParam>(&opt.strategy);
        const bool has_adaptive_param = mp && !mp->adaptive_param.empty();
        const bool firmware_forwards = discovery_.has_exclude_object();
        const bool custom_template =
            !PrinterDetector::get_bed_mesh_calibrate_gcode(printer_type_).empty();
        opt.adaptive_active = has_adaptive_param && firmware_forwards && !custom_template;
        break;
    }

    // Timelapse: append when the moonraker-timelapse plugin reports available.
    // Strategy is RuntimeCommand with sentinel values that
    // PrintPreparationManager::start_print() recognizes (see the dispatch
    // for command_enabled / command_disabled prefixed with "timelapse:").
    // These are NOT gcode lines — start_print() routes them to
    // `api_->timelapse().set_timelapse_enabled(...)`.
    if (lv_subject_get_int(capabilities_state_.get_printer_has_timelapse_subject()) == 1) {
        PrePrintOption tl;
        tl.id = "timelapse";
        tl.label_key = "Timelapse";
        tl.category = PrePrintCategory::Monitoring;
        tl.order = 100;
        // Default reflects the global moonraker-timelapse `enabled` setting
        // (seeded at discovery via set_timelapse_default_enabled). The plugin
        // has no per-print concept — the toggle writes the global `enabled` at
        // print start — so defaulting to a hardcoded false silently disabled a
        // user's global timelapse on every print start (#1094).
        tl.default_enabled = timelapse_default_enabled_;
        tl.strategy_kind = PrePrintStrategyKind::RuntimeCommand;
        PrePrintStrategyRuntimeCommand cmd;
        cmd.command_enabled = "timelapse:on";
        cmd.command_disabled = "timelapse:off";
        tl.strategy = cmd;
        pre_print_option_set_.options.push_back(std::move(tl));
    }

    // Maintain the (category, order) sort guarantee from
    // parse_pre_print_option_set so renderers still see options in their
    // documented order (covers both synthesized options above).
    std::sort(pre_print_option_set_.options.begin(), pre_print_option_set_.options.end(),
              [](const PrePrintOption& a, const PrePrintOption& b) {
                  if (a.category != b.category) {
                      return static_cast<int>(a.category) < static_cast<int>(b.category);
                  }
                  return a.order < b.order;
              });
}

const std::string& PrinterState::get_printer_type() const {
    return printer_type_;
}

const PrePrintOptionSet& PrinterState::get_pre_print_option_set() const {
    return pre_print_option_set_;
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
