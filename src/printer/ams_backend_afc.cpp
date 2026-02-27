// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_afc.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_update_queue.h"

#include "action_prompt_manager.h"
#include "afc_defaults.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsBackendAfc::AmsBackendAfc(MoonrakerAPI* api, MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info with AFC defaults
    system_info_.type = AmsType::AFC;
    system_info_.type_name = "AFC";
    // AFC capabilities from shared defaults
    auto caps = helix::printer::afc_default_capabilities();
    system_info_.supports_endless_spool = caps.supports_endless_spool;
    system_info_.supports_tool_mapping = caps.supports_tool_mapping;
    system_info_.supports_bypass = caps.supports_bypass;
    system_info_.supports_purge = caps.supports_purge;
    system_info_.tip_method = caps.tip_method;
    // Default to hardware sensor. Actual detection happens in set_discovered_sensors()
    // which checks for "filament_switch_sensor virtual_bypass" in the Klipper objects list.
    system_info_.has_hardware_bypass_sensor = true;

    spdlog::debug("[AMS AFC] Backend created");
}

AmsBackendAfc::~AmsBackendAfc() {
    // Invalidate alive guard FIRST so in-flight async callbacks bail out
    alive_->store(false);
}

// ============================================================================
// Lifecycle Management
// ============================================================================

void AmsBackendAfc::on_started() {
    // Detect AFC version (async - results come via callback)
    // This will set has_lane_data_db_ for v1.0.32+
    detect_afc_version();

    // If we have discovered lanes (from PrinterCapabilities), initialize them now.
    // This provides immediate lane data for ALL AFC versions (including < 1.0.32).
    // For v1.0.32+, query_lane_data() may later supplement this with richer data.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!discovered_lane_names_.empty() && !slots_.is_initialized()) {
            spdlog::info("[AMS AFC] Initializing {} lanes from discovery",
                         discovered_lane_names_.size());
            initialize_slots(discovered_lane_names_);
        }
    }

    // Note: With the early hardware discovery callback architecture, this backend is
    // created and started BEFORE printer.objects.subscribe is called. The notification
    // handler registered above will naturally receive the initial state when the
    // subscription response arrives. No explicit query_initial_state() needed.

    // Load AFC config files for device settings
    load_afc_configs();
}

void AmsBackendAfc::set_discovered_lanes(const std::vector<std::string>& lane_names,
                                         const std::vector<std::string>& hub_names) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Store discovered lane and hub names (from printer.objects.list)
    // These will be used as a fallback for AFC versions < 1.0.32
    if (!lane_names.empty()) {
        discovered_lane_names_ = lane_names;
        spdlog::debug("[AMS AFC] Set {} discovered lanes", discovered_lane_names_.size());
    }

    if (!hub_names.empty()) {
        hub_names_ = hub_names;
        spdlog::debug("[AMS AFC] Set {} discovered hubs", hub_names_.size());
    }
}

void AmsBackendAfc::set_discovered_sensors(const std::vector<std::string>& sensor_names) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Detect hardware vs virtual bypass from Klipper object names.
    // AFC creates "filament_switch_sensor virtual_bypass" when no hardware sensor is configured,
    // or uses the existing "filament_switch_sensor bypass" when hardware is present.
    bool has_virtual = false;
    bool has_hardware = false;
    for (const auto& name : sensor_names) {
        if (name == "filament_switch_sensor virtual_bypass") {
            has_virtual = true;
        } else if (name == "filament_switch_sensor bypass") {
            has_hardware = true;
        }
    }

    if (has_virtual) {
        system_info_.has_hardware_bypass_sensor = false;
        spdlog::info("[AMS AFC] Virtual bypass sensor detected");
    } else if (has_hardware) {
        system_info_.has_hardware_bypass_sensor = true;
        spdlog::info("[AMS AFC] Hardware bypass sensor detected");
    }
    // If neither found, keep the default (true — assumes hardware)
}

// stop(), release_subscriptions(), is_running() provided by AmsSubscriptionBackend

// ============================================================================
// Event System
// ============================================================================

// set_event_callback() and emit_event() provided by AmsSubscriptionBackend

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendAfc::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!slots_.is_initialized()) {
        return system_info_;
    }

    // Build slot data from registry, then overlay non-slot metadata from system_info_
    auto info = slots_.build_system_info();

    // Copy system-level fields not managed by registry
    info.type = system_info_.type;
    info.type_name = system_info_.type_name;
    info.version = system_info_.version;
    info.action = system_info_.action;
    info.operation_detail = system_info_.operation_detail;
    info.current_slot = system_info_.current_slot;
    info.current_tool = system_info_.current_tool;
    info.pending_target_slot = system_info_.pending_target_slot;
    info.current_toolchange = system_info_.current_toolchange;
    info.number_of_toolchanges = system_info_.number_of_toolchanges;
    info.filament_loaded = system_info_.filament_loaded;
    info.supports_endless_spool = system_info_.supports_endless_spool;
    info.supports_tool_mapping = system_info_.supports_tool_mapping;
    info.supports_bypass = system_info_.supports_bypass;
    info.has_hardware_bypass_sensor = system_info_.has_hardware_bypass_sensor;
    info.tip_method = system_info_.tip_method;
    info.supports_purge = system_info_.supports_purge;

    // Copy unit-level metadata not managed by registry
    for (size_t u = 0; u < info.units.size() && u < system_info_.units.size(); ++u) {
        info.units[u].name = system_info_.units[u].name;
        info.units[u].display_name = system_info_.units[u].display_name;
        info.units[u].connected = system_info_.units[u].connected;
        info.units[u].has_hub_sensor = system_info_.units[u].has_hub_sensor;
        info.units[u].hub_sensor_triggered = system_info_.units[u].hub_sensor_triggered;
        info.units[u].buffer_health = system_info_.units[u].buffer_health;
        info.units[u].topology = system_info_.units[u].topology;
        info.units[u].hub_tool_label = system_info_.units[u].hub_tool_label;
        info.units[u].has_encoder = system_info_.units[u].has_encoder;
        info.units[u].has_toolhead_sensor = system_info_.units[u].has_toolhead_sensor;
        info.units[u].has_slot_sensors = system_info_.units[u].has_slot_sensors;
    }

    return info;
}

AmsType AmsBackendAfc::get_type() const {
    return AmsType::AFC;
}

SlotInfo AmsBackendAfc::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto* entry = slots_.get(slot_index);
    if (entry) {
        return entry->info;
    }

    // Return empty slot info for invalid index
    SlotInfo empty;
    empty.slot_index = -1;
    empty.global_index = -1;
    return empty;
}

// get_current_action(), get_current_tool(), get_current_slot(), is_filament_loaded()
// provided by AmsSubscriptionBackend

PathTopology AmsBackendAfc::get_topology() const {
    // AFC uses a hub topology (Box Turtle / Armored Turtle style)
    return PathTopology::HUB;
}

PathTopology AmsBackendAfc::get_unit_topology(int unit_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // unit_infos_ is in AFC JSON order, system_info_.units is alphabetically sorted.
    // Must match by name, not by index.
    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size())) {
        const auto& unit_name = system_info_.units[unit_index].name;
        for (const auto& ui : unit_infos_) {
            std::string display_name = ui.type + " " + ui.name;
            if (display_name == unit_name) {
                return ui.topology;
            }
        }
        return system_info_.units[unit_index].topology;
    }
    return get_topology(); // Fallback to system-wide topology
}

PathSegment AmsBackendAfc::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return compute_filament_segment_unlocked();
}

PathSegment AmsBackendAfc::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if this is the active slot - return the current filament segment
    if (slot_index == system_info_.current_slot && system_info_.filament_loaded) {
        return compute_filament_segment_unlocked();
    }

    // For non-active slots, check lane sensors to determine filament position
    const auto* entry = slots_.get(slot_index);
    if (!entry) {
        return PathSegment::NONE;
    }

    const auto& sensors = entry->sensors;

    // Check sensors from furthest to nearest
    if (sensors.loaded_to_hub) {
        return PathSegment::HUB; // Filament reached hub sensor
    }
    if (sensors.load) {
        return PathSegment::LANE; // Filament in lane (load sensor triggered)
    }
    if (sensors.prep) {
        return PathSegment::PREP; // Filament at prep sensor
    }

    // Check slot status - if available, assume filament at spool
    if (entry->info.status == SlotStatus::AVAILABLE ||
        entry->info.status == SlotStatus::FROM_BUFFER) {
        return PathSegment::SPOOL;
    }

    return PathSegment::NONE;
}

PathSegment AmsBackendAfc::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_segment_;
}

bool AmsBackendAfc::slot_has_prep_sensor(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // AFC always has prep sensors on all lanes
    return slot_index >= 0 && slot_index < system_info_.total_slots;
}

PathSegment AmsBackendAfc::compute_filament_segment_unlocked() const {
    // Must be called with mutex_ held!
    // Returns the furthest point filament has reached based on sensor states.
    //
    // Sensor progression (AFC hub topology):
    //   SPOOL → PREP → LANE → HUB → OUTPUT → TOOLHEAD → NOZZLE
    //
    // Mapping from sensors:
    //   tool_end_sensor   → NOZZLE (filament at nozzle tip)
    //   tool_start_sensor → TOOLHEAD (filament entered toolhead)
    //   hub_sensor        → OUTPUT (filament past hub, heading to toolhead)
    //   loaded_to_hub     → HUB (filament reached hub merger)
    //   load              → LANE (filament in lane between prep and hub)
    //   prep              → PREP (filament at prep sensor, past spool)
    //   (no sensors)      → NONE or SPOOL depending on context

    // Check toolhead sensors first (furthest along path)
    if (tool_end_sensor_) {
        return PathSegment::NOZZLE;
    }

    if (tool_start_sensor_) {
        return PathSegment::TOOLHEAD;
    }

    // Check hub sensors (any hub triggered means filament past hub)
    for (const auto& [name, triggered] : hub_sensors_) {
        if (triggered) {
            return PathSegment::OUTPUT;
        }
    }

    // Check per-lane sensors for the current lane
    // If no current lane is set, check all lanes for any activity
    int lane_to_check = -1;
    if (!current_lane_name_.empty()) {
        lane_to_check = slots_.index_of(current_lane_name_);
    }

    // If we have a current lane, check its sensors
    if (lane_to_check >= 0) {
        const auto* entry = slots_.get(lane_to_check);
        if (entry) {
            const auto& sensors = entry->sensors;

            if (sensors.loaded_to_hub) {
                return PathSegment::HUB;
            }

            if (sensors.load) {
                return PathSegment::LANE;
            }

            if (sensors.prep) {
                return PathSegment::PREP;
            }
        }
    }

    // Fallback: check all lanes for any sensor activity
    for (int i = 0; i < slots_.slot_count(); ++i) {
        const auto* entry = slots_.get(i);
        if (!entry)
            continue;
        const auto& sensors = entry->sensors;

        if (sensors.loaded_to_hub) {
            return PathSegment::HUB;
        }

        if (sensors.load) {
            return PathSegment::LANE;
        }

        if (sensors.prep) {
            return PathSegment::PREP;
        }
    }

    // No sensors triggered - filament either at spool or absent
    // If we know filament is loaded somewhere, assume SPOOL
    if (system_info_.filament_loaded || system_info_.current_slot >= 0) {
        return PathSegment::SPOOL;
    }

    return PathSegment::NONE;
}

// ============================================================================
// Moonraker Status Update Handling
// ============================================================================

void AmsBackendAfc::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update has format: { "method": "notify_status_update", "params": [{ ... },
    // timestamp] }
    if (!notification.contains("params") || !notification["params"].is_array() ||
        notification["params"].empty()) {
        return;
    }

    const auto& params = notification["params"][0];
    if (!params.is_object()) {
        return;
    }

    bool state_changed = false;
    std::string deferred_error_event; // Collect error event to emit outside lock

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check if AFC provides an explicit filament_loaded field — if so, the
        // post-scan should not override it (newer AFC versions are authoritative)
        bool has_explicit_filament_loaded = false;
        if (params.contains("AFC") && params["AFC"].is_object()) {
            has_explicit_filament_loaded = params["AFC"].contains("filament_loaded");
        }
        if (!has_explicit_filament_loaded && params.contains("afc") && params["afc"].is_object()) {
            has_explicit_filament_loaded = params["afc"].contains("filament_loaded");
        }

        // Parse global AFC state if present
        if (params.contains("AFC") && params["AFC"].is_object()) {
            parse_afc_state(params["AFC"], deferred_error_event);
            state_changed = true;
        }

        // Legacy: also check for lowercase "afc" (older AFC versions)
        if (params.contains("afc") && params["afc"].is_object()) {
            parse_afc_state(params["afc"], deferred_error_event);
            state_changed = true;
        }

        // Parse AFC_stepper lane objects for sensor states
        // Keys like "AFC_stepper lane1", "AFC_stepper lane2", etc.
        bool lanes_updated = false;
        for (int i = 0; i < slots_.slot_count(); ++i) {
            std::string lane_name = slots_.name_of(i);
            std::string key = "AFC_stepper " + lane_name;
            if (params.contains(key) && params[key].is_object()) {
                parse_afc_stepper(lane_name, params[key]);
                state_changed = true;
                lanes_updated = true;
            }
        }

        // Parse AFC_lane objects (OpenAMS lanes use this prefix instead of AFC_stepper)
        // Same JSON schema as AFC_stepper, so reuse parse_afc_stepper
        for (int i = 0; i < slots_.slot_count(); ++i) {
            std::string lane_name = slots_.name_of(i);
            std::string key = "AFC_lane " + lane_name;
            if (params.contains(key) && params[key].is_object()) {
                parse_afc_stepper(lane_name, params[key]);
                state_changed = true;
                lanes_updated = true;
            }
        }

        // After processing lane updates, reconcile filament_loaded from slot
        // statuses. Only needed on AFC versions that lack an explicit
        // "filament_loaded" field — when present, parse_afc_state() already set
        // the authoritative value and the post-scan must not override it.
        if (lanes_updated && slots_.is_initialized() && !has_explicit_filament_loaded) {
            bool any_loaded = false;
            int loaded_slot = -1;
            for (int i = 0; i < slots_.slot_count(); ++i) {
                const auto* entry = slots_.get(i);
                if (entry && entry->info.status == SlotStatus::LOADED) {
                    any_loaded = true;
                    loaded_slot = i;
                    break;
                }
            }
            system_info_.filament_loaded = any_loaded;
            system_info_.current_slot = any_loaded ? loaded_slot : -1;
        }

        // Parse AFC_hub objects for hub sensor state
        // Keys like "AFC_hub Turtle_1"
        for (const auto& hub_name : hub_names_) {
            std::string key = "AFC_hub " + hub_name;
            if (params.contains(key) && params[key].is_object()) {
                parse_afc_hub(hub_name, params[key]);
                state_changed = true;
            }
        }

        // Parse AFC_extruder for toolhead sensors (multi-extruder support)
        if (!extruder_names_.empty()) {
            for (const auto& ext_name : extruder_names_) {
                std::string key = "AFC_extruder " + ext_name;
                if (params.contains(key) && params[key].is_object()) {
                    parse_afc_extruder(params[key]);
                    state_changed = true;
                }
            }
        } else {
            // Backward compat: single extruder fallback
            if (params.contains("AFC_extruder extruder") &&
                params["AFC_extruder extruder"].is_object()) {
                parse_afc_extruder(params["AFC_extruder extruder"]);
                state_changed = true;
            }
        }

        // Parse AFC_buffer objects for buffer health and fault data
        for (const auto& buf_name : buffer_names_) {
            std::string key = "AFC_buffer " + buf_name;
            if (params.contains(key) && params[key].is_object()) {
                parse_afc_buffer(buf_name, params[key]);
                state_changed = true;
            }
        }

        // Parse unit-level Klipper objects (AFC_BoxTurtle, AFC_OpenAMS)
        for (auto& unit_info : unit_infos_) {
            if (params.contains(unit_info.klipper_key) &&
                params[unit_info.klipper_key].is_object()) {
                parse_afc_unit_object(unit_info, params[unit_info.klipper_key]);
                state_changed = true;
            }
        }
    }

    // Emit events OUTSIDE the lock to avoid deadlock with callbacks
    if (!deferred_error_event.empty()) {
        emit_event(EVENT_ERROR, deferred_error_event);
    }
    if (state_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendAfc::parse_afc_state(const nlohmann::json& afc_data,
                                    std::string& deferred_error_event) {
    // Parse current lane — try "current_lane" first, fall back to "current_load"
    // Some AFC versions use "current_load" instead of "current_lane"
    std::string loaded_lane;
    if (afc_data.contains("current_lane") && afc_data["current_lane"].is_string()) {
        loaded_lane = afc_data["current_lane"].get<std::string>();
    } else if (afc_data.contains("current_load") && afc_data["current_load"].is_string()) {
        loaded_lane = afc_data["current_load"].get<std::string>();
    }

    if (!loaded_lane.empty()) {
        int slot_index = slots_.index_of(loaded_lane);
        if (slot_index >= 0) {
            system_info_.current_slot = slot_index;
            // Derive current_tool from slot's mapped tool
            int mapped = slots_.tool_for_slot(slot_index);
            if (mapped >= 0) {
                system_info_.current_tool = mapped;
                spdlog::trace("[AMS AFC] Derived current_tool T{} from slot {}", mapped,
                              slot_index);
            }
            spdlog::trace("[AMS AFC] Current lane: {} (slot {})", loaded_lane,
                          system_info_.current_slot);
        }
    }

    // Explicit current_tool from firmware overrides derived value
    if (afc_data.contains("current_tool") && afc_data["current_tool"].is_number_integer()) {
        system_info_.current_tool = afc_data["current_tool"].get<int>();
        spdlog::trace("[AMS AFC] Current tool (explicit): {}", system_info_.current_tool);
    }

    // Parse filament loaded state — try explicit field first, derive from current_load
    if (afc_data.contains("filament_loaded") && afc_data["filament_loaded"].is_boolean()) {
        system_info_.filament_loaded = afc_data["filament_loaded"].get<bool>();
        spdlog::trace("[AMS AFC] Filament loaded: {}", system_info_.filament_loaded);
    } else if (!loaded_lane.empty()) {
        // AFC versions without "filament_loaded" field: derive from current_load
        // If a lane is reported as currently loaded, filament is at the toolhead
        system_info_.filament_loaded = true;
        spdlog::trace("[AMS AFC] Filament loaded (derived from current_load={})", loaded_lane);
    } else if (afc_data.contains("current_load") && afc_data["current_load"].is_null()) {
        // current_load went null (unloaded) — clear filament state
        system_info_.filament_loaded = false;
        system_info_.current_slot = -1;
        system_info_.current_tool = -1;
        spdlog::trace("[AMS AFC] Filament unloaded (current_load=null)");
    }

    // Parse action/status
    if (afc_data.contains("status") && afc_data["status"].is_string()) {
        std::string status_str = afc_data["status"].get<std::string>();
        system_info_.action = ams_action_from_string(status_str);
        system_info_.operation_detail = status_str;
        spdlog::trace("[AMS AFC] Status: {} ({})", ams_action_to_string(system_info_.action),
                      status_str);
    }

    // Parse current_state field (preferred over status when present)
    if (afc_data.contains("current_state") && afc_data["current_state"].is_string()) {
        std::string state_str = afc_data["current_state"].get<std::string>();
        system_info_.action = ams_action_from_string(state_str);
        system_info_.operation_detail = state_str;
        spdlog::trace("[AMS AFC] Current state: {} ({})", ams_action_to_string(system_info_.action),
                      state_str);
    }

    // Parse tool change progress (AFC tracks swap count during multi-color prints)
    if (afc_data.contains("current_toolchange") &&
        afc_data["current_toolchange"].is_number_integer()) {
        system_info_.current_toolchange = afc_data["current_toolchange"].get<int>();
        spdlog::trace("[AMS AFC] Current toolchange: {}", system_info_.current_toolchange);
    }
    if (afc_data.contains("number_of_toolchanges") &&
        afc_data["number_of_toolchanges"].is_number_integer()) {
        system_info_.number_of_toolchanges = afc_data["number_of_toolchanges"].get<int>();
        spdlog::trace("[AMS AFC] Total toolchanges: {}", system_info_.number_of_toolchanges);
    }

    // Parse message object for operation detail, error events, and toast notifications
    if (afc_data.contains("message") && afc_data["message"].is_object()) {
        const auto& msg = afc_data["message"];
        if (msg.contains("message") && msg["message"].is_string()) {
            std::string msg_text = msg["message"].get<std::string>();
            if (!msg_text.empty()) {
                system_info_.operation_detail = msg_text;
            }

            // Get message type (error, warning, or empty)
            std::string msg_type;
            if (msg.contains("type") && msg["type"].is_string()) {
                msg_type = msg["type"].get<std::string>();
            }

            // Track message type for per-lane error severity mapping
            last_message_type_ = msg_type;

            // Handle message text changes for toast/notification dispatch
            if (msg_text.empty()) {
                // Error cleared - reset dedup tracking
                last_seen_message_.clear();
                last_error_msg_.clear();
                last_message_type_.clear();
            } else if (msg_text != last_seen_message_) {
                // New or changed message - update dedup tracker
                last_seen_message_ = msg_text;

                // Defer error event for emission outside lock (avoids deadlock)
                if (msg_type == "error" && msg_text != last_error_msg_) {
                    last_error_msg_ = msg_text;
                    deferred_error_event = msg_text;
                }

                // Suppress toasts when:
                // 1. AFC action:prompt modal is already showing (user sees it)
                // 2. Filament operation is active (load/unload generates noise)
                bool afc_prompt_active = helix::ActionPromptManager::is_showing() &&
                                         helix::ActionPromptManager::current_prompt_name().find(
                                             "AFC") != std::string::npos;
                // Only suppress during states that actively move filament.
                // Heating, tip forming, cutting, purging are stationary —
                // a sensor change there indicates a real problem.
                bool operation_active = system_info_.action == AmsAction::LOADING ||
                                        system_info_.action == AmsAction::UNLOADING ||
                                        system_info_.action == AmsAction::SELECTING;
                bool suppress_toast = afc_prompt_active || operation_active;

                if (suppress_toast) {
                    // Notification history only (no toast) - operation in progress
                    spdlog::debug("[AMS AFC] Toast suppressed (prompt={}, op={}): {}",
                                  afc_prompt_active, operation_active, msg_text);
                    ui_notification_info_with_action("AFC", msg_text.c_str(), "afc_message");
                } else {
                    // Show toast based on message type
                    if (msg_type == "error") {
                        NOTIFY_ERROR_T("AFC", "{}", msg_text);
                    } else if (msg_type == "warning") {
                        NOTIFY_WARNING_T("AFC", "{}", msg_text);
                    } else {
                        NOTIFY_INFO_T("AFC", "{}", msg_text);
                    }
                }
            }
        }
    }

    // Parse current_load field (overrides current_lane when present)
    if (afc_data.contains("current_load") && afc_data["current_load"].is_string()) {
        std::string load_lane = afc_data["current_load"].get<std::string>();
        int load_slot = slots_.index_of(load_lane);
        if (load_slot >= 0) {
            system_info_.current_slot = load_slot;
            spdlog::trace("[AMS AFC] Current load: {} (slot {})", load_lane, load_slot);
        }
    }

    // Parse lanes array if present (some AFC versions provide this)
    if (afc_data.contains("lanes") && afc_data["lanes"].is_object()) {
        parse_lane_data(afc_data["lanes"]);
    }

    // Parse unit information if available
    if (afc_data.contains("units") && afc_data["units"].is_array()) {
        const auto& units_json = afc_data["units"];

        // Capture unit-to-lane mapping for multi-unit reorganization
        unit_lane_map_.clear();
        unit_infos_.clear();

        for (const auto& unit_json : units_json) {
            // Handle flat string format: "OpenAMS AMS_1", "Box_Turtle Turtle_1"
            if (unit_json.is_string()) {
                std::string unit_str = unit_json.get<std::string>();
                auto space_pos = unit_str.find(' ');
                if (space_pos != std::string::npos) {
                    AfcUnitInfo info;
                    info.type = unit_str.substr(0, space_pos);
                    info.name = unit_str.substr(space_pos + 1);
                    // AFC convention: Klipper object prefix is "AFC_" + type with underscores
                    // removed. Known mappings: "Box_Turtle" → "AFC_BoxTurtle", "OpenAMS" →
                    // "AFC_OpenAMS" If this convention breaks for future types, use an explicit
                    // mapping table.
                    std::string klipper_type = info.type;
                    klipper_type.erase(std::remove(klipper_type.begin(), klipper_type.end(), '_'),
                                       klipper_type.end());
                    info.klipper_key = "AFC_" + klipper_type + " " + info.name;
                    unit_infos_.push_back(std::move(info));
                    spdlog::debug("[AMS AFC] Parsed string unit: type='{}' name='{}' key='{}'",
                                  unit_infos_.back().type, unit_infos_.back().name,
                                  unit_infos_.back().klipper_key);
                } else {
                    spdlog::debug("[AMS AFC] Skipping unit string with no space: '{}'", unit_str);
                }
                continue;
            }

            // Handle object format (backward compat): {"name": "...", "lanes": [...]}
            if (!unit_json.is_object()) {
                continue;
            }

            std::string unit_name;
            if (unit_json.contains("name") && unit_json["name"].is_string()) {
                unit_name = unit_json["name"].get<std::string>();
            }

            // Capture per-unit lane list
            if (unit_json.contains("lanes") && unit_json["lanes"].is_array()) {
                std::vector<std::string> lanes;
                for (const auto& lane : unit_json["lanes"]) {
                    if (lane.is_string()) {
                        lanes.push_back(lane.get<std::string>());
                    }
                }
                if (!unit_name.empty() && !lanes.empty()) {
                    unit_lane_map_[unit_name] = lanes;
                }
            }
        }

        // Update existing unit names and connection status (backward compat for object format)
        for (size_t i = 0; i < units_json.size() && i < system_info_.units.size(); ++i) {
            if (units_json[i].is_object()) {
                if (units_json[i].contains("name") && units_json[i]["name"].is_string()) {
                    system_info_.units[i].name = units_json[i]["name"].get<std::string>();
                }
                if (units_json[i].contains("connected") &&
                    units_json[i]["connected"].is_boolean()) {
                    system_info_.units[i].connected = units_json[i]["connected"].get<bool>();
                }
            }
        }

        // If we got unit-lane data from object format, re-organize into multi-unit layout.
        // NOTE: This runs under mutex_ lock (held by handle_status_update caller),
        // so system_info_ modifications are safe from concurrent get_system_info() reads.
        if (!unit_lane_map_.empty()) {
            if (!slots_.is_initialized() && !discovered_lane_names_.empty()) {
                initialize_slots(discovered_lane_names_);
            }
            if (slots_.is_initialized()) {
                reorganize_slots();
            }
        }
    }

    // Extract hub names from AFC.hubs array
    if (afc_data.contains("hubs") && afc_data["hubs"].is_array()) {
        hub_names_.clear();
        for (const auto& hub : afc_data["hubs"]) {
            if (hub.is_string()) {
                hub_names_.push_back(hub.get<std::string>());
            }
        }
        spdlog::debug("[AMS AFC] Discovered {} hubs", hub_names_.size());
    }

    // Extract buffer names from AFC.buffers array
    if (afc_data.contains("buffers") && afc_data["buffers"].is_array()) {
        buffer_names_.clear();
        for (const auto& buf : afc_data["buffers"]) {
            if (buf.is_string()) {
                buffer_names_.push_back(buf.get<std::string>());
            }
        }
    }

    // Extract extruder names from top-level AFC.extruders array (for multi-extruder iteration)
    // This is a flat string array: ["extruder", "extruder1", ..., "extruder5"]
    if (afc_data.contains("extruders") && afc_data["extruders"].is_array()) {
        extruder_names_.clear();
        for (const auto& ext : afc_data["extruders"]) {
            if (ext.is_string()) {
                extruder_names_.push_back(ext.get<std::string>());
            }
        }
        spdlog::debug("[AMS AFC] Discovered {} extruder names from AFC state",
                      extruder_names_.size());
    }

    // Parse global quiet_mode and LED state
    if (afc_data.contains("quiet_mode") && afc_data["quiet_mode"].is_boolean()) {
        afc_quiet_mode_ = afc_data["quiet_mode"].get<bool>();
    }
    if (afc_data.contains("led_state") && afc_data["led_state"].is_boolean()) {
        afc_led_state_ = afc_data["led_state"].get<bool>();
    }

    // Parse system.num_extruders and system.extruders (toolchanger support)
    if (afc_data.contains("system") && afc_data["system"].is_object()) {
        const auto& system = afc_data["system"];

        if (system.contains("num_extruders") && system["num_extruders"].is_number_integer()) {
            num_extruders_ = system["num_extruders"].get<int>();
            spdlog::debug("[AMS AFC] num_extruders: {}", num_extruders_);
        }

        if (system.contains("extruders") && system["extruders"].is_object()) {
            extruders_.clear();
            const auto& extruders_json = system["extruders"];

            // Collect extruder names and sort for deterministic ordering
            std::vector<std::string> extruder_names;
            for (auto it = extruders_json.begin(); it != extruders_json.end(); ++it) {
                extruder_names.push_back(it.key());
            }
            std::sort(extruder_names.begin(), extruder_names.end());

            for (const auto& ext_name : extruder_names) {
                const auto& ext_data = extruders_json[ext_name];
                if (!ext_data.is_object()) {
                    continue;
                }

                AfcExtruderInfo info;
                info.name = ext_name;

                // Parse lane_loaded (can be string or null)
                if (ext_data.contains("lane_loaded")) {
                    if (ext_data["lane_loaded"].is_string()) {
                        info.lane_loaded = ext_data["lane_loaded"].get<std::string>();
                    }
                    // null or other types result in empty string (default)
                }

                // Parse lanes array
                if (ext_data.contains("lanes") && ext_data["lanes"].is_array()) {
                    for (const auto& lane : ext_data["lanes"]) {
                        if (lane.is_string()) {
                            info.available_lanes.push_back(lane.get<std::string>());
                        }
                    }
                }

                spdlog::debug("[AMS AFC] Extruder '{}': lane_loaded='{}', {} lanes", ext_name,
                              info.lane_loaded, info.available_lanes.size());
                extruders_.push_back(std::move(info));
            }
        }
    }

    // Parse error state
    if (afc_data.contains("error_state") && afc_data["error_state"].is_boolean()) {
        error_state_ = afc_data["error_state"].get<bool>();
        if (error_state_) {
            // Use unlocked helper since we're already holding mutex_
            error_segment_ = compute_filament_segment_unlocked();
        } else {
            error_segment_ = PathSegment::NONE;
        }
    }

    // Parse bypass state (AFC exposes this via printer.AFC.bypass_state)
    // When bypass is active, current_gate = -2 (convention from Happy Hare)
    if (afc_data.contains("bypass_state") && afc_data["bypass_state"].is_boolean()) {
        bypass_active_ = afc_data["bypass_state"].get<bool>();
        if (bypass_active_) {
            system_info_.current_slot = -2; // -2 = bypass mode
            system_info_.filament_loaded = true;
            spdlog::trace("[AMS AFC] Bypass mode active");
        }
    }
}

// ============================================================================
// AFC Object Parsing (AFC_stepper, AFC_hub, AFC_extruder)
// ============================================================================

void AmsBackendAfc::parse_afc_stepper(const std::string& lane_name, const nlohmann::json& data) {
    // Parse AFC_stepper lane{N} object for sensor states and filament info
    // {
    //   "prep": true,           // Prep sensor
    //   "load": true,           // Load sensor
    //   "loaded_to_hub": true,  // Past hub
    //   "tool_loaded": false,   // At toolhead
    //   "status": "Loaded",
    //   "color": "#00aeff",
    //   "material": "ASA",
    //   "spool_id": 5,
    //   "weight": 931.7
    // }

    int slot_index = slots_.index_of(lane_name);
    if (slot_index < 0) {
        spdlog::trace("[AMS AFC] Unknown lane name: {}", lane_name);
        return;
    }

    auto* entry = slots_.get_mut(slot_index);
    if (!entry)
        return;

    // Update sensor state for this lane
    auto& sensors = entry->sensors;
    if (data.contains("prep") && data["prep"].is_boolean()) {
        sensors.prep = data["prep"].get<bool>();
    }
    if (data.contains("load") && data["load"].is_boolean()) {
        sensors.load = data["load"].get<bool>();
    }
    if (data.contains("loaded_to_hub") && data["loaded_to_hub"].is_boolean()) {
        sensors.loaded_to_hub = data["loaded_to_hub"].get<bool>();
    }
    if (data.contains("buffer_status") && data["buffer_status"].is_string()) {
        sensors.buffer_status = data["buffer_status"].get<std::string>();
    }
    if (data.contains("filament_status") && data["filament_status"].is_string()) {
        sensors.filament_status = data["filament_status"].get<std::string>();
    }
    if (data.contains("dist_hub") && data["dist_hub"].is_number()) {
        sensors.dist_hub = data["dist_hub"].get<float>();
    }

    // Get slot info for filament data update
    auto& slot = entry->info;

    // Parse color
    if (data.contains("color") && data["color"].is_string()) {
        std::string color_str = data["color"].get<std::string>();
        // Remove '#' prefix if present
        if (!color_str.empty() && color_str[0] == '#') {
            color_str = color_str.substr(1);
        }
        try {
            slot.color_rgb = std::stoul(color_str, nullptr, 16);
        } catch (...) {
            // Keep existing color on parse failure
        }
    }

    // Parse material
    if (data.contains("material") && data["material"].is_string()) {
        slot.material = data["material"].get<std::string>();
    }

    // Parse Spoolman ID
    if (data.contains("spool_id") && data["spool_id"].is_number_integer()) {
        slot.spoolman_id = data["spool_id"].get<int>();
    }

    // Parse weight
    if (data.contains("weight") && data["weight"].is_number()) {
        slot.remaining_weight_g = data["weight"].get<float>();
    }

    // Derive slot status from sensors and status string
    bool tool_loaded = false;
    if (data.contains("tool_loaded") && data["tool_loaded"].is_boolean()) {
        tool_loaded = data["tool_loaded"].get<bool>();
    }

    std::string status_str;
    if (data.contains("status") && data["status"].is_string()) {
        status_str = data["status"].get<std::string>();
    }

    // AFC "Loaded" status means hub-loaded, not toolhead-loaded.
    // Only tool_loaded == true means filament is at the extruder.
    if (tool_loaded || status_str == "Tooled") {
        slot.status = SlotStatus::LOADED;
    } else if (status_str == "Loaded" || status_str == "Ready" || sensors.prep || sensors.load) {
        slot.status = SlotStatus::AVAILABLE;
    } else if (status_str == "None" || status_str.empty()) {
        slot.status = SlotStatus::EMPTY;
    } else {
        slot.status = SlotStatus::AVAILABLE; // Default for other states
    }

    // Populate or clear per-slot error based on lane status
    if (status_str == "Error") {
        SlotError err;
        err.message = last_seen_message_.empty() ? "Lane error" : last_seen_message_;
        err.severity = (last_message_type_ == "warning") ? SlotError::WARNING : SlotError::ERROR;
        slot.error = err;
        spdlog::debug("[AMS AFC] Lane {} (slot {}): error state - {}", lane_name, slot_index,
                      err.message);
    } else if (slot.error.has_value()) {
        // Lane exited error state - clear the error
        spdlog::debug("[AMS AFC] Lane {} (slot {}): error cleared", lane_name, slot_index);
        slot.error.reset();
    }

    spdlog::trace("[AMS AFC] Lane {} (slot {}): prep={} load={} hub={} status={}", lane_name,
                  slot_index, sensors.prep, sensors.load, sensors.loaded_to_hub,
                  slot_status_to_string(slot.status));

    // Parse tool mapping from "map" field (e.g., "T0", "T1")
    if (data.contains("map") && data["map"].is_string()) {
        std::string map_str = data["map"].get<std::string>();
        // Parse "T{N}" format
        if (map_str.size() >= 2 && map_str[0] == 'T') {
            try {
                int tool_num = std::stoi(map_str.substr(1));
                if (tool_num >= 0 && tool_num <= 64) {
                    // Update registry tool mapping (also sets slot.mapped_tool)
                    slots_.set_tool_mapping(slot_index, tool_num);
                    spdlog::trace("[AMS AFC] Lane {} mapped to tool T{}", lane_name, tool_num);
                }
            } catch (...) {
                // Invalid tool number format
            }
        }
    }

    // Parse endless spool backup from "runout_lane" field
    if (data.contains("runout_lane")) {
        if (data["runout_lane"].is_string()) {
            std::string backup_lane = data["runout_lane"].get<std::string>();
            int backup_idx = slots_.index_of(backup_lane);
            if (backup_idx >= 0) {
                slots_.set_backup(slot_index, backup_idx);
                spdlog::trace("[AMS AFC] Lane {} runout backup: {} (slot {})", lane_name,
                              backup_lane, backup_idx);
            }
        } else if (data["runout_lane"].is_null()) {
            slots_.set_backup(slot_index, -1);
            spdlog::trace("[AMS AFC] Lane {} runout backup: disabled", lane_name);
        }
    }
}

void AmsBackendAfc::parse_afc_hub(const std::string& hub_name, const nlohmann::json& data) {
    // Parse AFC_hub object for per-hub sensor state
    // { "state": true }

    if (data.contains("state") && data["state"].is_boolean()) {
        bool state = data["state"].get<bool>();
        hub_sensors_[hub_name] = state;
        spdlog::trace("[AMS AFC] Hub sensor {}: {}", hub_name, state);

        // Update the parent AmsUnit's hub_sensor_triggered for real-time state.
        // Two strategies:
        // 1. If unit_infos_ has hub lists (from Klipper unit objects), use those
        //    to find which unit owns this hub (handles OpenAMS per-lane hubs
        //    where hub names differ from unit names).
        // 2. Fallback: match hub name directly against unit.name (works when
        //    hub names match unit names, e.g., standard Box Turtle "Turtle_1").
        // unit_infos_ is in AFC JSON order, system_info_.units is alphabetically sorted.
        // Must find the matching system_info_ unit by name, not by paired index.
        bool found = false;
        for (const auto& uinfo : unit_infos_) {
            bool owns_hub =
                std::find(uinfo.hubs.begin(), uinfo.hubs.end(), hub_name) != uinfo.hubs.end();
            if (owns_hub) {
                // Find the corresponding system_info_ unit by name
                std::string display_name = uinfo.type + " " + uinfo.name;
                for (auto& sys_unit : system_info_.units) {
                    if (sys_unit.name == display_name) {
                        sys_unit.has_hub_sensor = true;
                        bool any_triggered = false;
                        for (const auto& h : uinfo.hubs) {
                            auto it = hub_sensors_.find(h);
                            if (it != hub_sensors_.end() && it->second) {
                                any_triggered = true;
                                break;
                            }
                        }
                        sys_unit.hub_sensor_triggered = any_triggered;
                        break;
                    }
                }
                found = true;
                break;
            }
        }
        // Fallback: direct name match (hub name == unit name)
        if (!found) {
            for (auto& unit : system_info_.units) {
                if (unit.name == hub_name) {
                    unit.has_hub_sensor = true;
                    unit.hub_sensor_triggered = state;
                    break;
                }
            }
        }
    }

    // Store bowden length from hub — in multi-hub setups, all hubs share the same
    // bowden tube to the toolhead so last-writer-wins is acceptable here
    if (data.contains("afc_bowden_length") && data["afc_bowden_length"].is_number()) {
        bowden_length_ = data["afc_bowden_length"].get<float>();
        spdlog::trace("[AMS AFC] Hub bowden length: {}mm", bowden_length_);
    }
}

void AmsBackendAfc::parse_afc_buffer(const std::string& buffer_name, const nlohmann::json& data) {
    // Parse AFC_buffer object for buffer health and fault detection
    // {
    //   "fault_detection_enabled": true,
    //   "distance_to_fault": 25.5,
    //   "state": "Advancing",
    //   "lanes": ["lane1", "lane2", "lane3", "lane4"]
    // }

    BufferHealth health;

    if (data.contains("fault_detection_enabled") && data["fault_detection_enabled"].is_boolean()) {
        health.fault_detection_enabled = data["fault_detection_enabled"].get<bool>();
    }

    if (data.contains("distance_to_fault") && data["distance_to_fault"].is_number()) {
        health.distance_to_fault = data["distance_to_fault"].get<float>();
    }

    if (data.contains("state") && data["state"].is_string()) {
        health.state = data["state"].get<std::string>();
    }

    spdlog::trace("[AMS AFC] Buffer {}: fault_detect={} dist={} state={}", buffer_name,
                  health.fault_detection_enabled, health.distance_to_fault, health.state);

    // Store buffer health at unit level (buffer sits between hub and toolhead, not per-lane)
    // Find which unit this buffer belongs to by checking its lane list
    if (data.contains("lanes") && data["lanes"].is_array()) {
        for (const auto& lane_json : data["lanes"]) {
            if (!lane_json.is_string()) {
                continue;
            }
            std::string lane_name = lane_json.get<std::string>();
            int lane_idx = slots_.index_of(lane_name);
            if (lane_idx < 0) {
                continue;
            }

            // Find the unit containing this lane and set buffer health on the unit
            AmsUnit* unit = system_info_.get_unit_for_slot(lane_idx);
            if (unit) {
                unit->buffer_health = health;
                spdlog::debug("[AMS AFC] Buffer {} health set on unit {} (via lane {})",
                              buffer_name, unit->unit_index, lane_name);
                break; // One buffer per unit — set once and done
            }
        }
    }
}

void AmsBackendAfc::parse_afc_extruder(const nlohmann::json& data) {
    // Parse AFC_extruder object for toolhead sensors
    // {
    //   "tool_start_status": true,   // Toolhead entry sensor
    //   "tool_end_status": false,    // Toolhead exit/nozzle sensor
    //   "lane_loaded": "lane1"       // Currently loaded lane
    // }

    if (data.contains("tool_start_status") && data["tool_start_status"].is_boolean()) {
        tool_start_sensor_ = data["tool_start_status"].get<bool>();
    }

    if (data.contains("tool_end_status") && data["tool_end_status"].is_boolean()) {
        tool_end_sensor_ = data["tool_end_status"].get<bool>();
    }

    if (data.contains("lane_loaded") && !data["lane_loaded"].is_null()) {
        if (data["lane_loaded"].is_string()) {
            current_lane_name_ = data["lane_loaded"].get<std::string>();
            // Update current_slot from lane name
            int loaded_slot = slots_.index_of(current_lane_name_);
            if (loaded_slot >= 0) {
                system_info_.current_slot = loaded_slot;
            }
        }
    }

    spdlog::trace("[AMS AFC] Extruder: tool_start={} tool_end={} lane={}", tool_start_sensor_,
                  tool_end_sensor_, current_lane_name_);
}

void AmsBackendAfc::parse_afc_unit_object(AfcUnitInfo& unit_info, const nlohmann::json& data) {
    // Parse unit-level Klipper object (AFC_BoxTurtle or AFC_OpenAMS)
    // Contains arrays of lanes, extruders, hubs, and buffers belonging to this unit

    if (data.contains("lanes") && data["lanes"].is_array()) {
        unit_info.lanes.clear();
        for (const auto& lane : data["lanes"]) {
            if (lane.is_string()) {
                unit_info.lanes.push_back(lane.get<std::string>());
            }
        }
    }

    if (data.contains("extruders") && data["extruders"].is_array()) {
        unit_info.extruders.clear();
        for (const auto& ext : data["extruders"]) {
            if (ext.is_string()) {
                unit_info.extruders.push_back(ext.get<std::string>());
            }
        }
    }

    if (data.contains("hubs") && data["hubs"].is_array()) {
        unit_info.hubs.clear();
        for (const auto& hub : data["hubs"]) {
            if (hub.is_string()) {
                unit_info.hubs.push_back(hub.get<std::string>());
            }
        }
    }

    if (data.contains("buffers") && data["buffers"].is_array()) {
        unit_info.buffers.clear();
        for (const auto& buf : data["buffers"]) {
            if (buf.is_string()) {
                unit_info.buffers.push_back(buf.get<std::string>());
            }
        }
    }

    // Derive topology from hub/extruder counts:
    // - No hubs + multiple extruders → PARALLEL (Box Turtle: 1:1 lane-to-tool)
    // - Hubs present + single extruder → HUB (OpenAMS: N:1 through hub)
    // - Default: HUB
    if (unit_info.hubs.empty() && unit_info.extruders.size() > 1) {
        unit_info.topology = PathTopology::PARALLEL;
    } else if (!unit_info.hubs.empty() && unit_info.extruders.size() <= 1) {
        unit_info.topology = PathTopology::HUB;
    } else {
        unit_info.topology = PathTopology::HUB;
    }

    spdlog::debug("[AMS AFC] Unit object '{}': {} lanes, {} extruders, {} hubs, {} buffers → {}",
                  unit_info.klipper_key, unit_info.lanes.size(), unit_info.extruders.size(),
                  unit_info.hubs.size(), unit_info.buffers.size(),
                  path_topology_to_string(unit_info.topology));

    // Only reorganize when ALL unit_infos_ have received their lane data.
    // Unit objects may arrive in separate status updates; reorganizing on partial
    // data creates transient incorrect unit structures visible in the UI.
    bool all_have_lanes = !unit_infos_.empty();
    for (const auto& ui : unit_infos_) {
        if (ui.lanes.empty()) {
            all_have_lanes = false;
            break;
        }
    }
    if (all_have_lanes) {
        rebuild_unit_map_from_klipper();
    }
}

void AmsBackendAfc::rebuild_unit_map_from_klipper() {
    // Rebuild unit_lane_map_ from unit_infos_ data and trigger reorganization
    unit_lane_map_.clear();
    for (const auto& ui : unit_infos_) {
        if (!ui.lanes.empty()) {
            // Use the full "Type Name" as map key for reorganize_slots
            // which uses unit names for AmsUnit::name
            std::string display_name = ui.type + " " + ui.name;
            unit_lane_map_[display_name] = ui.lanes;
        }
    }

    if (!unit_lane_map_.empty()) {
        if (!slots_.is_initialized() && !discovered_lane_names_.empty()) {
            initialize_slots(discovered_lane_names_);
        }
        if (slots_.is_initialized()) {
            reorganize_slots();

            // Set per-unit topology on AmsUnit structs from unit_infos_.
            // unit_infos_ is in AFC JSON order, system_info_.units is alphabetically sorted.
            // Must match by name, not by index.
            for (const auto& ui : unit_infos_) {
                std::string display_name = ui.type + " " + ui.name;
                for (auto& sys_unit : system_info_.units) {
                    if (sys_unit.name == display_name) {
                        sys_unit.topology = ui.topology;
                        // For HUB units, derive physical tool label from extruder name
                        if (ui.topology == PathTopology::HUB && ui.extruders.size() == 1) {
                            const auto& ext_name = ui.extruders[0];
                            size_t pos = ext_name.find_last_not_of("0123456789");
                            if (pos != std::string::npos && pos + 1 < ext_name.size()) {
                                sys_unit.hub_tool_label = std::stoi(ext_name.substr(pos + 1));
                            } else if (ext_name == "extruder") {
                                sys_unit.hub_tool_label = 0;
                            }
                        }
                        break;
                    }
                }
            }

            spdlog::debug("[AMS AFC] Reorganized {} units from unit-level objects",
                          unit_infos_.size());
        }
    }
}

// ============================================================================
// Version Detection
// ============================================================================

void AmsBackendAfc::detect_afc_version() {
    if (!client_) {
        spdlog::warn("[AMS AFC] Cannot detect version: client is null");
        return;
    }

    // Query Moonraker database for AFC install version
    // Method: server.database.get_item
    // Namespace: afc-install (contains {"version": "1.0.0"})
    nlohmann::json params = {{"namespace", "afc-install"}};

    auto alive = alive_;
    client_->send_jsonrpc(
        "server.database.get_item", params,
        [this, alive](const nlohmann::json& response) {
            if (!alive->load())
                return;
            bool should_query_lane_data = false;

            if (response.contains("value") && response["value"].is_object()) {
                const auto& value = response["value"];
                if (value.contains("version") && value["version"].is_string()) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        afc_version_ = value["version"].get<std::string>();
                        system_info_.version = afc_version_;

                        // Set capability flags based on version
                        has_lane_data_db_ = version_at_least("1.0.32");
                        should_query_lane_data = has_lane_data_db_;
                    }
                    spdlog::info("[AMS AFC] Detected AFC version: {} (lane_data DB: {})",
                                 afc_version_, has_lane_data_db_ ? "yes" : "no");

                    // Warn if AFC version is older than minimum supported
                    if (!version_at_least("1.0.35")) {
                        auto* msg = new (std::nothrow)
                            std::string(fmt::format("AFC version {} may have compatibility issues. "
                                                    "Please upgrade to v1.0.35 or later.",
                                                    afc_version_));
                        if (msg) {
                            spdlog::warn("[AMS AFC] {}", *msg);
                            helix::ui::async_call(
                                [](void* data) {
                                    auto* m = static_cast<std::string*>(data);
                                    helix::ui::modal_show_alert("AFC Version Warning", m->c_str(),
                                                                ModalSeverity::Warning, "OK");
                                    delete m;
                                },
                                msg);
                        }
                    }
                }
            }

            // For v1.0.32+, query lane_data database for richer data
            // This supplements the basic lane info from printer.objects.list
            if (should_query_lane_data) {
                query_lane_data();
            }
        },
        [this, alive](const MoonrakerError& err) {
            if (!alive->load())
                return;
            spdlog::warn("[AMS AFC] Could not detect AFC version: {}", err.message);
            std::lock_guard<std::mutex> lock(mutex_);
            afc_version_ = "unknown";
            system_info_.version = "unknown";
            // Don't query lane_data - we'll rely on discovered lanes from capabilities
        },
        0,   // default timeout
        true // silent — probe only, don't show toast or log at error level
    );
}

bool AmsBackendAfc::version_at_least(const std::string& required) const {
    // Parse semantic version strings (e.g., "1.0.32")
    // Returns true if afc_version_ >= required

    if (afc_version_ == "unknown" || afc_version_.empty()) {
        return false;
    }

    auto parse_version = [](const std::string& v) -> std::tuple<int, int, int> {
        int major = 0, minor = 0, patch = 0;
        std::istringstream iss(v);
        char dot;
        iss >> major >> dot >> minor >> dot >> patch;
        return {major, minor, patch};
    };

    auto [cur_maj, cur_min, cur_patch] = parse_version(afc_version_);
    auto [req_maj, req_min, req_patch] = parse_version(required);

    if (cur_maj != req_maj)
        return cur_maj > req_maj;
    if (cur_min != req_min)
        return cur_min > req_min;
    return cur_patch >= req_patch;
}

// ============================================================================
// Initial State Query
// ============================================================================

void AmsBackendAfc::query_initial_state() {
    if (!client_) {
        spdlog::warn("[AMS AFC] Cannot query initial state: client is null");
        return;
    }

    // Build list of AFC objects to query
    // We need to get the current state since we were created after the subscription
    // response was processed
    nlohmann::json objects_to_query;

    // Add main AFC object
    objects_to_query["AFC"] = nullptr;

    // Add AFC_stepper objects for each lane
    for (int i = 0; i < slots_.slot_count(); ++i) {
        std::string key = "AFC_stepper " + slots_.name_of(i);
        objects_to_query[key] = nullptr;
    }

    // Add AFC_lane objects (OpenAMS lanes use this prefix instead of AFC_stepper)
    for (int i = 0; i < slots_.slot_count(); ++i) {
        std::string key = "AFC_lane " + slots_.name_of(i);
        objects_to_query[key] = nullptr;
    }

    // Add AFC_hub objects
    for (const auto& hub_name : hub_names_) {
        std::string key = "AFC_hub " + hub_name;
        objects_to_query[key] = nullptr;
    }

    // Add AFC_extruder objects (multi-extruder support)
    if (!extruder_names_.empty()) {
        for (const auto& ext_name : extruder_names_) {
            std::string key = "AFC_extruder " + ext_name;
            objects_to_query[key] = nullptr;
        }
    } else {
        // Backward compat: single extruder
        objects_to_query["AFC_extruder extruder"] = nullptr;
    }

    // Add unit-level Klipper objects (AFC_BoxTurtle, AFC_OpenAMS)
    for (const auto& unit_info : unit_infos_) {
        objects_to_query[unit_info.klipper_key] = nullptr;
    }

    nlohmann::json params = {{"objects", objects_to_query}};

    spdlog::debug("[AMS AFC] Querying initial state for {} objects", objects_to_query.size());

    auto alive = alive_;
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, alive](const nlohmann::json& response) {
            if (!alive->load())
                return;
            // Response structure: {"jsonrpc": "2.0", "result": {"eventtime": ..., "status": {...}},
            // "id": ...}
            if (response.contains("result") && response["result"].contains("status") &&
                response["result"]["status"].is_object()) {
                // The status object format is the same as notify_status_update params
                // Wrap it in a format that handle_status_update expects
                nlohmann::json notification = {
                    {"params", nlohmann::json::array({response["result"]["status"]})}};
                handle_status_update(notification);
                spdlog::info("[AMS AFC] Initial state loaded");
            } else {
                spdlog::warn("[AMS AFC] Initial state query returned unexpected format");
            }
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS AFC] Failed to query initial state: {}", err.message);
        });
}

// ============================================================================
// Lane Data Queries
// ============================================================================

void AmsBackendAfc::query_lane_data() {
    if (!client_) {
        spdlog::warn("[AMS AFC] Cannot query lane data: client is null");
        return;
    }

    // Query Moonraker database for AFC lane_data
    // Method: server.database.get_item
    // Params: { "namespace": "AFC", "key": "lane_data" }
    nlohmann::json params = {{"namespace", "AFC"}, {"key", "lane_data"}};

    auto alive = alive_;
    client_->send_jsonrpc(
        "server.database.get_item", params,
        [this, alive](const nlohmann::json& response) {
            if (!alive->load())
                return;
            if (response.contains("value") && response["value"].is_object()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    parse_lane_data(response["value"]);
                }
                // Emit OUTSIDE the lock to avoid deadlock with callbacks
                emit_event(EVENT_STATE_CHANGED);
            }
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS AFC] Failed to query lane_data: {}", err.message);
        });
}

void AmsBackendAfc::parse_lane_data(const nlohmann::json& lane_data) {
    // Lane data format:
    // {
    //   "lane1": {"color": "FF0000", "material": "PLA", "loaded": false, ...},
    //   "lane2": {"color": "00FF00", "material": "PETG", "loaded": true, ...}
    // }

    // Extract lane names and sort them for consistent ordering
    std::vector<std::string> new_lane_names;
    for (auto it = lane_data.begin(); it != lane_data.end(); ++it) {
        new_lane_names.push_back(it.key());
    }
    std::sort(new_lane_names.begin(), new_lane_names.end());

    // Initialize lanes if this is the first time or count changed
    if (!slots_.is_initialized() ||
        static_cast<int>(new_lane_names.size()) != slots_.slot_count()) {
        initialize_slots(new_lane_names);
    }

    // Track whether any lane has tool_loaded — used to update filament_loaded
    // after scanning all lanes. Without this, filament_loaded could stay stale
    // if no lane reports tool_loaded, leaving the Unload button stuck disabled.
    bool any_tool_loaded = false;
    int tool_loaded_slot = -1;

    // Update lane information
    for (int i = 0; i < slots_.slot_count(); ++i) {
        std::string lane_name = slots_.name_of(i);
        if (lane_name.empty() || !lane_data.contains(lane_name) ||
            !lane_data[lane_name].is_object()) {
            continue;
        }

        const auto& lane = lane_data[lane_name];
        auto* entry = slots_.get_mut(i);
        if (!entry) {
            continue;
        }
        auto& slot = entry->info;

        // Parse color (AFC uses hex string without 0x prefix)
        if (lane.contains("color") && lane["color"].is_string()) {
            std::string color_str = lane["color"].get<std::string>();
            try {
                slot.color_rgb = static_cast<uint32_t>(std::stoul(color_str, nullptr, 16));
            } catch (...) {
                slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            }
        }

        // Parse material
        if (lane.contains("material") && lane["material"].is_string()) {
            slot.material = lane["material"].get<std::string>();
        }

        // Parse loaded state
        // AFC "loaded" means hub-loaded, not toolhead-loaded
        // Only tool_loaded == true means filament is at the extruder
        bool tool_loaded = false;
        if (lane.contains("tool_loaded") && lane["tool_loaded"].is_boolean()) {
            tool_loaded = lane["tool_loaded"].get<bool>();
        }

        if (tool_loaded) {
            slot.status = SlotStatus::LOADED;
            any_tool_loaded = true;
            tool_loaded_slot = i;
        } else if (lane.contains("loaded") && lane["loaded"].is_boolean() &&
                   lane["loaded"].get<bool>()) {
            // Hub-loaded: filament is present and ready, not at toolhead
            slot.status = SlotStatus::AVAILABLE;
        } else {
            if (lane.contains("available") && lane["available"].is_boolean() &&
                lane["available"].get<bool>()) {
                slot.status = SlotStatus::AVAILABLE;
            } else if (lane.contains("empty") && lane["empty"].is_boolean() &&
                       lane["empty"].get<bool>()) {
                slot.status = SlotStatus::EMPTY;
            } else {
                slot.status = SlotStatus::AVAILABLE;
            }
        }

        // Parse spool information if available
        if (lane.contains("spool_id") && lane["spool_id"].is_number_integer()) {
            slot.spoolman_id = lane["spool_id"].get<int>();
        }

        if (lane.contains("brand") && lane["brand"].is_string()) {
            slot.brand = lane["brand"].get<std::string>();
        }

        if (lane.contains("remaining_weight") && lane["remaining_weight"].is_number()) {
            slot.remaining_weight_g = lane["remaining_weight"].get<float>();
        }

        if (lane.contains("total_weight") && lane["total_weight"].is_number()) {
            slot.total_weight_g = lane["total_weight"].get<float>();
        }
    }

    // Update filament_loaded from lane scan results.
    // The top-level parse_afc_state() may also set this from AFC's own
    // filament_loaded field, but lane data is the authoritative source for
    // which specific slot is loaded.
    system_info_.filament_loaded = any_tool_loaded;
    if (any_tool_loaded) {
        system_info_.current_slot = tool_loaded_slot;
    }
}

void AmsBackendAfc::initialize_slots(const std::vector<std::string>& lane_names) {
    int lane_count = static_cast<int>(lane_names.size());

    // Initialize registry (sets is_initialized = true, creates SlotEntry per lane)
    slots_.initialize("AFC Box Turtle", lane_names);

    // Set up system_info_ for non-slot fields (unit-level metadata)
    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "AFC Box Turtle";
    unit.slot_count = lane_count;
    unit.first_slot_global_index = 0;
    unit.connected = true;
    unit.has_encoder = false;        // AFC typically uses optical sensors, not encoders
    unit.has_toolhead_sensor = true; // Most AFC setups have toolhead sensor
    unit.has_slot_sensors = true;    // AFC has per-lane sensors
    unit.has_hub_sensor = true;      // AFC hubs have filament sensors

    // Initialize slots with defaults
    for (int i = 0; i < lane_count; ++i) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::UNKNOWN;
        slot.mapped_tool = i; // Default 1:1 mapping
        slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        unit.slots.push_back(slot);
    }

    system_info_.units.clear();
    system_info_.units.push_back(unit);
    system_info_.total_slots = lane_count;

    // Initialize tool-to-lane mapping (1:1 default)
    system_info_.tool_to_slot_map.clear();
    system_info_.tool_to_slot_map.reserve(lane_count);
    for (int i = 0; i < lane_count; ++i) {
        system_info_.tool_to_slot_map.push_back(i);
    }

    // Set up tool mapping in registry
    slots_.set_tool_map(system_info_.tool_to_slot_map);

    // Clear pre-init storage now that registry is initialized
    discovered_lane_names_.clear();
}

/**
 * @brief Reorganize flat slot list into multi-unit structure using unit_lane_map_.
 *
 * Called from parse_afc_state() after the "units" JSON array has been parsed.
 * Rebuilds system_info_.units from unit_lane_map_ (unit_name → [lane_names]),
 * preserving existing slot data (colors, materials, status) by matching lane names.
 *
 * @pre mutex_ must be held by caller (via handle_status_update → parse_afc_state)
 * @pre slots_ must be initialized (slots exist in system_info_.units[0])
 */
void AmsBackendAfc::reorganize_slots() {
    if (unit_lane_map_.size() <= 1) {
        // Single unit - just update the name if available
        if (!unit_lane_map_.empty() && !system_info_.units.empty()) {
            const auto& map_name = unit_lane_map_.begin()->first;
            system_info_.units[0].name = map_name;
            // Set pretty display name from unit_infos_ if available
            for (const auto& uinfo : unit_infos_) {
                std::string full_name = uinfo.type + " " + uinfo.name;
                if (full_name == map_name) {
                    system_info_.units[0].display_name = uinfo.name;
                    break;
                }
            }
        }
        return;
    }

    // Reorganize registry (preserves slot data, handles name→index mapping)
    std::map<std::string, std::vector<std::string>> sorted_map(unit_lane_map_.begin(),
                                                               unit_lane_map_.end());
    slots_.reorganize(sorted_map);

    // Rebuild system_info_.units for unit-level metadata (connected, topology,
    // hub_sensor, buffer_health, hub_tool_label) that the registry doesn't track.
    system_info_.units.clear();
    int global_slot_offset = 0;
    int unit_idx = 0;

    for (const auto& [unit_name, lanes] : sorted_map) {
        AmsUnit unit;
        unit.unit_index = unit_idx;
        unit.name = unit_name;

        // Set pretty display name: look up just the instance name from unit_infos_
        // (e.g., "Turtle_1" instead of full "Box_Turtle Turtle_1")
        for (const auto& uinfo : unit_infos_) {
            std::string full_name = uinfo.type + " " + uinfo.name;
            if (full_name == unit_name) {
                unit.display_name = uinfo.name;
                break;
            }
        }

        unit.slot_count = static_cast<int>(lanes.size());
        unit.first_slot_global_index = global_slot_offset;
        unit.connected = true;
        unit.has_toolhead_sensor = true;
        unit.has_slot_sensors = true;

        // Set hub sensor state — two strategies:
        // 1. Check unit_infos_ for hub lists (OpenAMS: hub names differ from unit names)
        // 2. Fallback: direct name match in hub_sensors_ (hub name == unit name)
        unit.has_hub_sensor = false;
        unit.hub_sensor_triggered = false;
        bool found_via_uinfo = false;
        for (const auto& uinfo : unit_infos_) {
            if (uinfo.name == unit_name && !uinfo.hubs.empty()) {
                unit.has_hub_sensor = true;
                for (const auto& h : uinfo.hubs) {
                    auto hub_it = hub_sensors_.find(h);
                    if (hub_it != hub_sensors_.end() && hub_it->second) {
                        unit.hub_sensor_triggered = true;
                        break;
                    }
                }
                found_via_uinfo = true;
                break;
            }
        }
        if (!found_via_uinfo) {
            auto hub_it = hub_sensors_.find(unit_name);
            if (hub_it != hub_sensors_.end()) {
                unit.has_hub_sensor = true;
                unit.hub_sensor_triggered = hub_it->second;
            }
        }

        // Populate slots from registry entries (preserves colors, materials, etc.)
        for (int i = 0; i < unit.slot_count; ++i) {
            int gi = global_slot_offset + i;
            const auto* entry = slots_.get(gi);
            if (entry) {
                SlotInfo slot = entry->info;
                slot.slot_index = i;
                slot.global_index = gi;
                unit.slots.push_back(slot);
            } else {
                SlotInfo slot;
                slot.slot_index = i;
                slot.global_index = gi;
                slot.status = SlotStatus::UNKNOWN;
                slot.mapped_tool = gi;
                slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
                unit.slots.push_back(slot);
            }
        }

        system_info_.units.push_back(unit);
        global_slot_offset += unit.slot_count;
        ++unit_idx;
    }

    system_info_.total_slots = global_slot_offset;

    spdlog::info("[AMS AFC] Reorganized into {} units, {} total slots", system_info_.units.size(),
                 system_info_.total_slots);
}

// ============================================================================
// Filament Operations
// ============================================================================

// check_preconditions() provided by AmsSubscriptionBackend

AmsError AmsBackendAfc::validate_slot_index(int slot_index) const {
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
    }
    return AmsErrorHelper::success();
}

// execute_gcode() provided by AmsSubscriptionBackend

AmsError AmsBackendAfc::execute_gcode_notify(const std::string& gcode,
                                             const std::string& success_msg,
                                             const std::string& error_prefix) {
    if (!api_) {
        return AmsErrorHelper::not_connected("MoonrakerAPI not available");
    }

    spdlog::info("[AMS AFC] Executing G-code: {}", gcode);

    // Capture messages by value for async callbacks (thread-safe via ui_async_call)
    api_->execute_gcode(
        gcode,
        [success_msg]() {
            if (!success_msg.empty()) {
                NOTIFY_SUCCESS("{}", success_msg);
            }
        },
        [gcode, error_prefix](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[AMS AFC] G-code response timed out (may still be running): {}",
                             gcode);
                if (!error_prefix.empty()) {
                    NOTIFY_WARNING("{} — response timed out", error_prefix);
                }
            } else if (!error_prefix.empty()) {
                NOTIFY_ERROR("{}: {}", error_prefix, err.message);
            } else {
                spdlog::error("[AMS AFC] G-code failed: {} - {}", gcode, err.message);
            }
        },
        MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);

    return AmsErrorHelper::success();
}

AmsError AmsBackendAfc::load_filament(int slot_index) {
    std::string lane_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        AmsError gate_valid = validate_slot_index(slot_index);
        if (!gate_valid) {
            return gate_valid;
        }

        // Check if lane has filament available
        const auto* entry = slots_.get(slot_index);
        if (entry && entry->info.status == SlotStatus::EMPTY) {
            return AmsErrorHelper::slot_not_available(slot_index);
        }

        lane_name = slots_.name_of(slot_index);
        if (lane_name.empty()) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }
    }

    // Toolchanger mode: use AFC_SELECT_TOOL with extruder name
    if (num_extruders_ > 1) {
        const auto* entry = slots_.get(slot_index);
        int tool = entry ? entry->info.mapped_tool : slot_index;
        if (tool >= 0 && tool < static_cast<int>(extruders_.size())) {
            std::string cmd = "AFC_SELECT_TOOL TOOL=" + extruders_[tool].name;
            spdlog::info("[AMS AFC] Loading slot {} via toolchanger: {}", slot_index, cmd);
            return execute_gcode(cmd);
        }
    }

    // Standard mode: CHANGE_TOOL LANE={name}
    std::ostringstream cmd;
    cmd << "CHANGE_TOOL LANE=" << lane_name;

    spdlog::info("[AMS AFC] Loading from lane {} (slot {})", lane_name, slot_index);
    return execute_gcode(cmd.str());
}

AmsError AmsBackendAfc::unload_filament() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (!system_info_.filament_loaded) {
            return AmsError(AmsResult::WRONG_STATE, "No filament loaded", "No filament to unload",
                            "Load filament first");
        }
    }

    if (num_extruders_ > 1) {
        spdlog::info("[AMS AFC] Unloading via toolchanger: AFC_UNSELECT_TOOL");
        return execute_gcode("AFC_UNSELECT_TOOL");
    }

    spdlog::info("[AMS AFC] Unloading filament");
    return execute_gcode("TOOL_UNLOAD");
}

AmsError AmsBackendAfc::select_slot(int slot_index) {
    std::string lane_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        AmsError gate_valid = validate_slot_index(slot_index);
        if (!gate_valid) {
            return gate_valid;
        }

        lane_name = slots_.name_of(slot_index);
        if (lane_name.empty()) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }
    }

    // AFC does not have a "select without load" command — only CHANGE_TOOL loads filament
    spdlog::debug("[AMS AFC] Select-only requested for lane {} (slot {}), not supported", lane_name,
                  slot_index);
    return AmsErrorHelper::not_supported("AFC does not support select without load");
}

AmsError AmsBackendAfc::change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (tool_number < 0 || tool_number >= slots_.slot_count()) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "Select a valid tool");
        }
    }

    std::string cmd;
    if (num_extruders_ > 1 && tool_number < static_cast<int>(extruders_.size())) {
        // Toolchanger mode: use AFC_SELECT_TOOL with extruder name
        cmd = "AFC_SELECT_TOOL TOOL=" + extruders_[tool_number].name;
    } else {
        // Standard mode: use T{n}
        cmd = "T" + std::to_string(tool_number);
    }

    spdlog::info("[AMS AFC] Tool change: {}", cmd);
    return execute_gcode(cmd);
}

// ============================================================================
// Recovery Operations
// ============================================================================

AmsError AmsBackendAfc::recover() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Only check running_, NOT is_busy() — recovery must work even when
        // the system is stuck in a busy/error state
        if (!running_) {
            return AmsErrorHelper::not_connected("AFC backend not started");
        }
    }

    spdlog::info("[AMS AFC] Initiating recovery");
    return execute_gcode_notify("AFC_RESET", lv_tr("AFC recovery complete"),
                                lv_tr("AFC recovery failed"));
}

AmsError AmsBackendAfc::reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }
    }

    spdlog::info("[AMS AFC] Resetting AFC system");
    return execute_gcode_notify("AFC_RESET", lv_tr("AFC reset complete"),
                                lv_tr("AFC reset failed"));
}

AmsError AmsBackendAfc::reset_lane(int slot_index) {
    std::string lane_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        lane_name = slots_.name_of(slot_index);
        if (lane_name.empty()) {
            return AmsErrorHelper::invalid_slot(
                slot_index, slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
        }
    }

    spdlog::info("[AMS AFC] Resetting lane {}", lane_name);
    return execute_gcode("AFC_LANE_RESET LANE=" + lane_name);
}

AmsError AmsBackendAfc::eject_lane(int slot_index) {
    std::string lane_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        AmsError slot_err = validate_slot_index(slot_index);
        if (!slot_err) {
            return slot_err;
        }

        if (system_info_.filament_loaded && system_info_.current_slot == slot_index) {
            return AmsError(AmsResult::WRONG_STATE, "Lane is loaded in toolhead",
                            "Unload from toolhead first", "Use Unload before Eject");
        }

        lane_name = slots_.name_of(slot_index);
        if (lane_name.empty()) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }
    }

    spdlog::info("[AMS AFC] Ejecting lane {}", lane_name);
    return execute_gcode("LANE_UNLOAD LANE=" + lane_name);
}

AmsError AmsBackendAfc::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("AFC backend not started");
        }

        if (system_info_.action == AmsAction::IDLE) {
            return AmsErrorHelper::success(); // Nothing to cancel
        }
    }

    // AFC uses RESET_FAILURE to cancel/recover from error state
    spdlog::info("[AMS AFC] Cancelling current operation");
    return execute_gcode_notify("RESET_FAILURE", lv_tr("AFC failure reset complete"),
                                lv_tr("AFC failure reset failed"));
}

// ============================================================================
// Configuration Operations
// ============================================================================

AmsError AmsBackendAfc::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }
        auto& slot = entry->info;

        // Capture old spoolman_id before updating for clear detection
        int old_spoolman_id = slot.spoolman_id;

        // Detect whether anything actually changed
        bool changed = slot.color_name != info.color_name || slot.color_rgb != info.color_rgb ||
                       slot.material != info.material || slot.brand != info.brand ||
                       slot.spoolman_id != info.spoolman_id || slot.spool_name != info.spool_name ||
                       slot.remaining_weight_g != info.remaining_weight_g ||
                       slot.total_weight_g != info.total_weight_g ||
                       slot.nozzle_temp_min != info.nozzle_temp_min ||
                       slot.nozzle_temp_max != info.nozzle_temp_max ||
                       slot.bed_temp != info.bed_temp;

        // Update local state
        slot.color_name = info.color_name;
        slot.color_rgb = info.color_rgb;
        slot.material = info.material;
        slot.brand = info.brand;
        slot.spoolman_id = info.spoolman_id;
        slot.spool_name = info.spool_name;
        slot.remaining_weight_g = info.remaining_weight_g;
        slot.total_weight_g = info.total_weight_g;
        slot.nozzle_temp_min = info.nozzle_temp_min;
        slot.nozzle_temp_max = info.nozzle_temp_max;
        slot.bed_temp = info.bed_temp;

        if (changed) {
            spdlog::info("[AMS AFC] Updated slot {} info: {} {}", slot_index, info.material,
                         info.color_name);
        }

        // Persist via G-code commands if AFC version supports it (v1.0.20+).
        // Skip persistence when persist=false — this is used by Spoolman weight
        // polling (refresh_spoolman_weights) to update in-memory state without
        // sending G-code back to AFC firmware. Without this guard, each weight
        // update would fire SET_COLOR/SET_MATERIAL/SET_WEIGHT/SET_SPOOL_ID G-codes,
        // which trigger AFC status_update WebSocket events, which call
        // sync_from_backend → refresh_spoolman_weights → set_slot_info again,
        // creating an infinite feedback loop that saturates the CPU.
        if (persist && version_at_least("1.0.20")) {
            std::string lane_name = slots_.name_of(slot_index);
            if (!lane_name.empty()) {
                // Color (only if changed and valid - not 0 or default grey)
                if (info.color_rgb != 0 && info.color_rgb != AMS_DEFAULT_SLOT_COLOR) {
                    char color_hex[8];
                    snprintf(color_hex, sizeof(color_hex), "%06X", info.color_rgb & 0xFFFFFF);
                    execute_gcode(fmt::format("SET_COLOR LANE={} COLOR={}", lane_name, color_hex));
                }

                // Material (validate to prevent command injection)
                if (!info.material.empty() && MoonrakerAPI::is_safe_gcode_param(info.material)) {
                    execute_gcode(
                        fmt::format("SET_MATERIAL LANE={} MATERIAL={}", lane_name, info.material));
                } else if (!info.material.empty()) {
                    spdlog::warn("[AMS AFC] Skipping SET_MATERIAL - unsafe characters in: {}",
                                 info.material);
                }

                // Weight (if valid)
                if (info.remaining_weight_g > 0) {
                    execute_gcode(fmt::format("SET_WEIGHT LANE={} WEIGHT={:.0f}", lane_name,
                                              info.remaining_weight_g));
                }

                // Spoolman ID
                if (info.spoolman_id > 0) {
                    execute_gcode(fmt::format("SET_SPOOL_ID LANE={} SPOOL_ID={}", lane_name,
                                              info.spoolman_id));
                } else if (info.spoolman_id == 0 && old_spoolman_id > 0) {
                    // Clear Spoolman link with empty string (not -1)
                    execute_gcode(fmt::format("SET_SPOOL_ID LANE={} SPOOL_ID=", lane_name));
                }
            }
        } else if (persist && afc_version_ != "unknown" && !afc_version_.empty()) {
            spdlog::info("[AMS AFC] Version {} - slot changes stored locally only (upgrade to "
                         "1.0.20+ for persistence)",
                         afc_version_);
        }
    }

    // Emit OUTSIDE the lock to avoid deadlock with callbacks
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));

    return AmsErrorHelper::success();
}

AmsError AmsBackendAfc::set_tool_mapping(int tool_number, int slot_index) {
    std::string lane_name; // Declare outside lock for use after release
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tool_number < 0 || tool_number >= slots_.slot_count()) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "");
        }

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }

        // Update registry tool mapping (handles clearing old mappings internally)
        slots_.set_tool_mapping(slot_index, tool_number);

        // Get lane name from registry
        lane_name = slots_.name_of(slot_index);
    }

    // AFC may use a G-code command to set tool mapping
    // This varies by AFC version/configuration
    if (!lane_name.empty()) {
        std::ostringstream cmd;
        cmd << "SET_MAP LANE=" << lane_name << " MAP=T" << tool_number;
        spdlog::info("[AMS AFC] Mapping T{} to lane {} (slot {})", tool_number, lane_name,
                     slot_index);
        return execute_gcode(cmd.str());
    }

    return AmsErrorHelper::success();
}

// ============================================================================
// Bypass Mode Operations
// ============================================================================

AmsError AmsBackendAfc::enable_bypass() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (!system_info_.supports_bypass) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            "This AFC system does not support bypass mode", "");
        }
    }

    // AFC enables bypass via filament sensor control.
    // Sensor name depends on hardware vs virtual bypass:
    //   Hardware: "filament_switch_sensor bypass"
    //   Virtual:  "filament_switch_sensor virtual_bypass"
    const char* sensor = system_info_.has_hardware_bypass_sensor ? "bypass" : "virtual_bypass";
    spdlog::info("[AMS AFC] Enabling bypass mode (sensor={})", sensor);
    return execute_gcode(fmt::format("SET_FILAMENT_SENSOR SENSOR={} ENABLE=1", sensor));
}

AmsError AmsBackendAfc::disable_bypass() {
    const char* sensor = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("AFC backend not started");
        }

        if (!bypass_active_) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not active",
                            "Bypass mode is not currently active", "");
        }

        sensor = system_info_.has_hardware_bypass_sensor ? "bypass" : "virtual_bypass";
    }

    spdlog::info("[AMS AFC] Disabling bypass mode (sensor={})", sensor);
    return execute_gcode(fmt::format("SET_FILAMENT_SENSOR SENSOR={} ENABLE=0", sensor));
}

bool AmsBackendAfc::is_bypass_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bypass_active_;
}

// ============================================================================
// Endless Spool Operations
// ============================================================================

helix::printer::EndlessSpoolCapabilities AmsBackendAfc::get_endless_spool_capabilities() const {
    // AFC supports per-slot backup configuration via SET_RUNOUT G-code
    return {true, true, "AFC per-slot backup"};
}

// ============================================================================
// Tool Mapping Operations
// ============================================================================

helix::printer::ToolMappingCapabilities AmsBackendAfc::get_tool_mapping_capabilities() const {
    // AFC supports per-lane tool assignment via SET_MAP G-code
    return {true, true, "Per-lane tool assignment via SET_MAP"};
}

std::vector<int> AmsBackendAfc::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.build_system_info().tool_to_slot_map;
}

std::vector<helix::printer::EndlessSpoolConfig> AmsBackendAfc::get_endless_spool_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<helix::printer::EndlessSpoolConfig> configs;
    for (int i = 0; i < slots_.slot_count(); ++i) {
        configs.push_back({i, slots_.backup_for_slot(i)});
    }
    return configs;
}

AmsError AmsBackendAfc::set_endless_spool_backup(int slot_index, int backup_slot) {
    std::string lane_name;
    std::string backup_lane_name;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        int lane_count = slots_.slot_count();

        // Validate slot_index
        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(slot_index, lane_count > 0 ? lane_count - 1 : 0);
        }

        // Validate backup_slot (-1 or valid index, not equal to slot_index)
        if (backup_slot != -1) {
            if (!slots_.is_valid_index(backup_slot)) {
                return AmsErrorHelper::invalid_slot(backup_slot,
                                                    lane_count > 0 ? lane_count - 1 : 0);
            }
            if (backup_slot == slot_index) {
                return AmsError(AmsResult::INVALID_SLOT, "Cannot use slot as its own backup",
                                "A slot cannot be set as its own endless spool backup",
                                "Select a different backup slot");
            }
        }

        // Get lane names from registry
        lane_name = slots_.name_of(slot_index);
        if (backup_slot >= 0) {
            backup_lane_name = slots_.name_of(backup_slot);
        }

        // Update registry backup config
        slots_.set_backup(slot_index, backup_slot);
    }

    // Validate lane names to prevent command injection
    if (!MoonrakerAPI::is_safe_gcode_param(lane_name)) {
        spdlog::warn("[AMS AFC] Unsafe lane name characters in endless spool config");
        return AmsError(AmsResult::MAPPING_ERROR, "Invalid lane name",
                        "Lane name contains invalid characters", "Check AFC configuration");
    }
    if (backup_slot >= 0 && !MoonrakerAPI::is_safe_gcode_param(backup_lane_name)) {
        spdlog::warn("[AMS AFC] Unsafe backup lane name characters");
        return AmsError(AmsResult::MAPPING_ERROR, "Invalid backup lane name",
                        "Backup lane name contains invalid characters", "Check AFC configuration");
    }

    // Build and send G-code command
    // AFC uses: SET_RUNOUT LANE=<name> RUNOUT=<name|NONE>
    std::string gcode;
    if (backup_slot >= 0) {
        gcode = fmt::format("SET_RUNOUT LANE={} RUNOUT={}", lane_name, backup_lane_name);
        spdlog::info("[AMS AFC] Setting endless spool backup: {} -> {}", lane_name,
                     backup_lane_name);
    } else {
        gcode = fmt::format("SET_RUNOUT LANE={} RUNOUT=NONE", lane_name);
        spdlog::info("[AMS AFC] Disabling endless spool backup for {}", lane_name);
    }

    return execute_gcode(gcode);
}

AmsError AmsBackendAfc::reset_tool_mappings() {
    spdlog::info("[AMS AFC] Resetting tool mappings");

    // Use RESET_AFC_MAPPING with RUNOUT=no to only reset tool mappings
    AmsError result = execute_gcode("RESET_AFC_MAPPING RUNOUT=no");

    // Tool mapping will be refreshed from next status update
    return result;
}

AmsError AmsBackendAfc::reset_endless_spool() {
    spdlog::info("[AMS AFC] Resetting endless spool mappings");

    int slot_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot_count = slots_.slot_count();
    }

    // AFC has no command to reset only runout lanes, iterate through slots
    // Continue on failure to reset as many as possible, return first error
    AmsError first_error = AmsErrorHelper::success();
    for (int slot = 0; slot < slot_count; slot++) {
        AmsError result = set_endless_spool_backup(slot, -1);
        if (!result.success()) {
            spdlog::error("[AMS AFC] Failed to reset slot {} endless spool: {}", slot,
                          result.technical_msg);
            if (first_error.success()) {
                first_error = result;
            }
        }
    }

    return first_error;
}

// ============================================================================
// AFC Config File Management
// ============================================================================

void AmsBackendAfc::load_afc_configs() {
    if (configs_loading_.load() || configs_loaded_.load()) {
        return;
    }

    if (!api_) {
        spdlog::warn("[AMS AFC] Cannot load configs: MoonrakerAPI is null");
        return;
    }

    configs_loading_ = true;

    // Create managers if not yet created (share alive guard for callback safety)
    if (!afc_config_) {
        afc_config_ = std::make_unique<AfcConfigManager>(api_, alive_);
    }
    if (!macro_vars_config_) {
        macro_vars_config_ = std::make_unique<AfcConfigManager>(api_, alive_);
    }

    // Track completion of both loads
    auto loads_remaining = std::make_shared<std::atomic<int>>(2);

    // Callbacks from download_file run on the libhv background thread.
    // configs_loaded_ is std::atomic<bool> — the store (release) after both loads complete
    // synchronizes-with the load (acquire) in get_device_actions() on the main thread,
    // ensuring all parser writes are visible before the main thread reads them.
    auto alive = alive_;
    auto check_done = [this, alive, loads_remaining]() {
        if (!alive->load())
            return;
        if (loads_remaining->fetch_sub(1) == 1) {
            // Both loads complete — release barrier ensures parser state is visible
            configs_loading_.store(false, std::memory_order_relaxed);
            configs_loaded_.store(true, std::memory_order_release);
            spdlog::info("[AMS AFC] Config files loaded");

            // Detect tip method from AFC config.
            // TODO: Replace with direct Moonraker status query once AFC exposes
            // tool_cut/form_tip in get_status() (upstream AFC enhancement pending).
            update_tip_method_from_config();

            emit_event(EVENT_STATE_CHANGED);
        }
    };

    afc_config_->load("AFC/AFC.cfg", [check_done](bool ok, const std::string& err) {
        if (!ok) {
            spdlog::warn("[AMS AFC] Failed to load AFC.cfg: {}", err);
        }
        check_done();
    });

    macro_vars_config_->load(
        "AFC/AFC_Macro_Vars.cfg", [check_done](bool ok, const std::string& err) {
            if (!ok) {
                spdlog::warn("[AMS AFC] Failed to load AFC_Macro_Vars.cfg: {}", err);
            }
            check_done();
        });
}

float AmsBackendAfc::get_macro_var_float(const std::string& key, float default_val) const {
    if (!macro_vars_config_ || !macro_vars_config_->is_loaded()) {
        return default_val;
    }
    return macro_vars_config_->parser().get_float("gcode_macro AFC_MacroVars", key, default_val);
}

bool AmsBackendAfc::get_macro_var_bool(const std::string& key, bool default_val) const {
    if (!macro_vars_config_ || !macro_vars_config_->is_loaded()) {
        return default_val;
    }
    return macro_vars_config_->parser().get_bool("gcode_macro AFC_MacroVars", key, default_val);
}

void AmsBackendAfc::update_tip_method_from_config() {
    if (!afc_config_ || !afc_config_->is_loaded()) {
        return;
    }

    const auto& parser = afc_config_->parser();

    // Check toolhead cutting (pin-based cutter at nozzle) from [AFC] section
    bool tool_cut = parser.get_bool("AFC", "tool_cut", false);

    // Check hub cutting (servo-based cutter on hub) from any [AFC_hub *] section
    bool hub_cut = false;
    for (const auto& section : parser.get_sections_matching("AFC_hub")) {
        if (parser.get_bool(section, "cut", false)) {
            hub_cut = true;
            break;
        }
    }

    // Check tip forming from [AFC] section
    bool form_tip = parser.get_bool("AFC", "form_tip", false);

    TipMethod method = TipMethod::NONE;
    if (tool_cut || hub_cut) {
        method = TipMethod::CUT;
    } else if (form_tip) {
        method = TipMethod::TIP_FORM;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.tip_method = method;
    }

    spdlog::info("[AMS AFC] Tip method from config: {} (tool_cut={}, hub_cut={}, form_tip={})",
                 tip_method_to_string(method), tool_cut, hub_cut, form_tip);
}

// ============================================================================
// Device Actions (AFC-specific calibration and speed settings)
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendAfc::get_device_sections() const {
    return helix::printer::afc_default_sections();
}

std::vector<helix::printer::DeviceAction> AmsBackendAfc::get_device_actions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    using helix::printer::ActionType;
    using helix::printer::DeviceAction;

    // Start from shared defaults for static actions
    auto actions = helix::printer::afc_default_actions();

    // Overlay dynamic values onto default actions
    for (auto& a : actions) {
        if (a.id == "bowden_length") {
            a.current_value = bowden_length_;
            a.max_value = std::max(2000.0f, bowden_length_ * 1.5f);
        }
        if (a.id == "led_toggle") {
            a.label = afc_led_state_ ? "Turn Off LEDs" : "Turn On LEDs";
            a.icon = afc_led_state_ ? "lightbulb-off" : "lightbulb-on";
        }
    }

    // Multi-extruder: replace single bowden with per-extruder sliders
    if (num_extruders_ > 1 && !extruders_.empty()) {
        actions.erase(std::remove_if(actions.begin(), actions.end(),
                                     [](const DeviceAction& a) { return a.id == "bowden_length"; }),
                      actions.end());
        for (int i = 0; i < static_cast<int>(extruders_.size()); ++i) {
            std::string id = "bowden_T" + std::to_string(i);
            std::string label = "Bowden Length (T" + std::to_string(i) + ")";
            std::string desc = "Bowden tube length for tool " + std::to_string(i);
            actions.push_back(
                DeviceAction{id,
                             label,
                             "ruler",
                             "setup",
                             desc,
                             ActionType::SLIDER,
                             bowden_length_, // shared default until per-extruder tracking
                             {},
                             100.0f,
                             std::max(2000.0f, bowden_length_ * 1.5f),
                             "mm",
                             -1,
                             true,
                             ""});
        }
    }

    // ---- Overlay dynamic values from config onto default actions ----

    // Acquire barrier pairs with release in load_afc_configs() to ensure
    // parser state written on bg thread is visible here on the main thread.
    bool loaded = configs_loaded_.load(std::memory_order_acquire);
    bool cfg_ready = loaded && afc_config_ && afc_config_->is_loaded();
    bool macro_ready = loaded && macro_vars_config_ && macro_vars_config_->is_loaded();
    std::string not_loaded_reason = "Loading configuration...";

    // Find first AFC_hub section from config (for hub actions)
    std::string hub_section;
    if (cfg_ready) {
        auto hubs = afc_config_->parser().get_sections_matching("AFC_hub");
        if (!hubs.empty()) {
            hub_section = hubs[0];
        }
    }
    bool hub_ready = cfg_ready && !hub_section.empty();

    // Config save state
    bool has_changes = (afc_config_ && afc_config_->has_unsaved_changes()) ||
                       (macro_vars_config_ && macro_vars_config_->has_unsaved_changes());

    for (auto& a : actions) {
        // Hub & Cutter actions — from afc_config_ hub section
        if (a.id == "hub_cut_enabled") {
            if (hub_ready) {
                a.current_value =
                    std::any(afc_config_->parser().get_bool(hub_section, "cut", false));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "hub_cut_dist") {
            if (hub_ready) {
                a.current_value =
                    std::any(afc_config_->parser().get_float(hub_section, "cut_dist", 0.0f));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "hub_bowden_length") {
            if (hub_ready) {
                a.current_value = std::any(
                    afc_config_->parser().get_float(hub_section, "afc_bowden_length", 450.0f));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "assisted_retract") {
            if (hub_ready) {
                a.current_value = std::any(
                    afc_config_->parser().get_bool(hub_section, "assisted_retract", false));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        }

        // Tip Forming actions — from macro_vars_config_
        else if (a.id == "ramming_volume") {
            if (macro_ready) {
                a.current_value = std::any(get_macro_var_float("variable_ramming_volume", 0.0f));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "unloading_speed_start") {
            if (macro_ready) {
                a.current_value =
                    std::any(get_macro_var_float("variable_unloading_speed_start", 0.0f));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "cooling_tube_length") {
            if (macro_ready) {
                a.current_value =
                    std::any(get_macro_var_float("variable_cooling_tube_length", 0.0f));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "cooling_tube_retraction") {
            if (macro_ready) {
                a.current_value =
                    std::any(get_macro_var_float("variable_cooling_tube_retraction", 0.0f));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        }

        // Purge & Wipe actions — from macro_vars_config_
        else if (a.id == "purge_enabled") {
            if (macro_ready) {
                a.current_value = std::any(get_macro_var_bool("variable_purge_enabled", false));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "purge_length") {
            if (macro_ready) {
                a.current_value = std::any(get_macro_var_float("variable_purge_length", 0.0f));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "brush_enabled") {
            if (macro_ready) {
                a.current_value = std::any(get_macro_var_bool("variable_brush_enabled", false));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        }

        // Config section — save_restart enabled only when there are unsaved changes
        else if (a.id == "save_restart") {
            a.enabled = has_changes;
            a.disable_reason = has_changes ? "" : "No unsaved changes";
        }
    }

    return actions;
}

AmsError AmsBackendAfc::execute_device_action(const std::string& action_id, const std::any& value) {
    spdlog::info("[AMS AFC] Executing device action: {}", action_id);

    if (action_id == "calibration_wizard") {
        return execute_gcode("AFC_CALIBRATION");
    } else if (action_id == "bowden_length") {
        if (!value.has_value()) {
            return AmsError(AmsResult::WRONG_STATE, "Bowden length value required", "Missing value",
                            "Provide a bowden length value");
        }
        try {
            float length = std::any_cast<float>(value);
            std::lock_guard<std::mutex> lock(mutex_);
            float max_len = std::max(2000.0f, bowden_length_ * 1.5f);
            if (length < 100.0f || length > max_len) {
                return AmsError(AmsResult::WRONG_STATE,
                                fmt::format("Bowden length must be 100-{:.0f}mm", max_len),
                                "Invalid value",
                                fmt::format("Enter a length between 100 and {:.0f}mm", max_len));
            }
            // AFC uses SET_BOWDEN_LENGTH HUB={hub_name} LENGTH={mm}
            if (!hub_names_.empty()) {
                std::string hub_name = hub_names_[0];
                return execute_gcode("SET_BOWDEN_LENGTH HUB=" + hub_name +
                                     " LENGTH=" + std::to_string(static_cast<int>(length)));
            }
            return AmsErrorHelper::not_supported("No AFC hubs configured");
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid bowden length type",
                            "Invalid value type", "Provide a numeric value");
        }
    } else if (action_id.rfind("bowden_T", 0) == 0) {
        // Per-extruder bowden length (toolchanger): bowden_T0, bowden_T1, etc.
        if (!value.has_value()) {
            return AmsError(AmsResult::WRONG_STATE, "Bowden length value required", "Missing value",
                            "Provide a bowden length value");
        }
        try {
            float length = std::any_cast<float>(value);
            std::lock_guard<std::mutex> lock(mutex_);
            float max_len = std::max(2000.0f, bowden_length_ * 1.5f);
            if (length < 100.0f || length > max_len) {
                return AmsError(AmsResult::WRONG_STATE,
                                fmt::format("Bowden length must be 100-{:.0f}mm", max_len),
                                "Invalid value",
                                fmt::format("Enter a length between 100 and {:.0f}mm", max_len));
            }
            // Extract tool index from action_id (e.g., "bowden_T0" -> 0)
            int tool_idx = std::stoi(action_id.substr(8));
            if (tool_idx >= 0 && tool_idx < static_cast<int>(extruders_.size())) {
                // Find the hub for this extruder by matching unit membership
                std::string hub_name;
                const std::string& ext_name = extruders_[tool_idx].name;
                for (const auto& unit : unit_infos_) {
                    auto it = std::find(unit.extruders.begin(), unit.extruders.end(), ext_name);
                    if (it != unit.extruders.end() && !unit.hubs.empty()) {
                        hub_name = unit.hubs[0];
                        break;
                    }
                }
                // Fall back to first known hub
                if (hub_name.empty() && !hub_names_.empty()) {
                    hub_name = hub_names_[0];
                }
                if (hub_name.empty()) {
                    return AmsErrorHelper::not_supported("No AFC hub found for extruder");
                }
                return execute_gcode("SET_BOWDEN_LENGTH HUB=" + hub_name +
                                     " LENGTH=" + std::to_string(static_cast<int>(length)));
            }
            return AmsErrorHelper::not_supported("Invalid extruder index: " +
                                                 std::to_string(tool_idx));
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid bowden length type",
                            "Invalid value type", "Provide a numeric value");
        }
    } else if (action_id == "speed_fwd" || action_id == "speed_rev") {
        if (!value.has_value()) {
            return AmsError(AmsResult::WRONG_STATE, "Speed multiplier value required",
                            "Missing value", "Provide a speed multiplier value");
        }
        try {
            float multiplier = std::any_cast<float>(value);
            if (multiplier < 0.5f || multiplier > 2.0f) {
                return AmsError(AmsResult::WRONG_STATE, "Speed multiplier must be 0.5-2.0x",
                                "Invalid value", "Enter a multiplier between 0.5 and 2.0");
            }
            // AFC SET_LONG_MOVE_SPEED is per-lane; apply to all lanes
            std::string param = (action_id == "speed_fwd") ? "FWD_SPEED" : "RWD_FACTOR";
            if (slots_.slot_count() == 0) {
                return AmsErrorHelper::not_supported("No AFC lanes configured");
            }
            for (int i = 0; i < slots_.slot_count(); ++i) {
                AmsError err = execute_gcode("SET_LONG_MOVE_SPEED LANE=" + slots_.name_of(i) + " " +
                                             param + "=" + std::to_string(multiplier));
                if (!err)
                    return err; // Return first error
            }
            return AmsErrorHelper::success();
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid speed multiplier type",
                            "Invalid value type", "Provide a numeric value");
        }
    } else if (action_id == "test_lanes") {
        return execute_gcode("AFC_TEST_LANES");
    } else if (action_id == "change_blade") {
        return execute_gcode("AFC_CHANGE_BLADE");
    } else if (action_id == "park") {
        return execute_gcode("AFC_PARK");
    } else if (action_id == "brush") {
        return execute_gcode("AFC_BRUSH");
    } else if (action_id == "reset_motor") {
        // AFC_RESET_MOTOR_TIME is per-lane; reset all lanes
        if (slots_.slot_count() == 0) {
            return AmsErrorHelper::not_supported("No AFC lanes configured");
        }
        for (int i = 0; i < slots_.slot_count(); ++i) {
            AmsError err = execute_gcode("AFC_RESET_MOTOR_TIME LANE=" + slots_.name_of(i));
            if (!err)
                return err;
        }
        return AmsErrorHelper::success();
    } else if (action_id == "led_toggle") {
        std::lock_guard<std::mutex> lock(mutex_);
        return execute_gcode(afc_led_state_ ? "TURN_OFF_AFC_LED" : "TURN_ON_AFC_LED");
    } else if (action_id == "quiet_mode") {
        return execute_gcode("AFC_QUIET_MODE");
    }

    // ---- Config-backed hub actions (afc_config_) ----
    if (action_id == "hub_cut_enabled" || action_id == "hub_cut_dist" ||
        action_id == "hub_bowden_length" || action_id == "assisted_retract") {
        if (!afc_config_ || !afc_config_->is_loaded()) {
            return AmsError(AmsResult::WRONG_STATE, "AFC config not loaded",
                            "Configuration not available", "Wait for config to load");
        }

        auto hubs = afc_config_->parser().get_sections_matching("AFC_hub");
        if (hubs.empty()) {
            return AmsError(AmsResult::WRONG_STATE, "No hub section found in AFC config",
                            "No hub configured", "Check AFC configuration");
        }
        const std::string& hub_section = hubs[0];

        if (action_id == "hub_cut_enabled") {
            try {
                bool val = std::any_cast<bool>(value);
                afc_config_->parser().set(hub_section, "cut", val ? "True" : "False");
                afc_config_->mark_dirty();
                return AmsErrorHelper::success();
            } catch (const std::bad_any_cast&) {
                return AmsError(AmsResult::WRONG_STATE, "Invalid value type for toggle",
                                "Expected boolean", "");
            }
        } else if (action_id == "hub_cut_dist") {
            try {
                float val = std::any_cast<float>(value);
                afc_config_->parser().set(hub_section, "cut_dist", fmt::format("{:g}", val));
                afc_config_->mark_dirty();
                return AmsErrorHelper::success();
            } catch (const std::bad_any_cast&) {
                return AmsError(AmsResult::WRONG_STATE, "Invalid value type for slider",
                                "Expected float", "");
            }
        } else if (action_id == "hub_bowden_length") {
            try {
                float val = std::any_cast<float>(value);
                afc_config_->parser().set(hub_section, "afc_bowden_length",
                                          fmt::format("{:g}", val));
                afc_config_->mark_dirty();
                return AmsErrorHelper::success();
            } catch (const std::bad_any_cast&) {
                return AmsError(AmsResult::WRONG_STATE, "Invalid value type for slider",
                                "Expected float", "");
            }
        } else if (action_id == "assisted_retract") {
            try {
                bool val = std::any_cast<bool>(value);
                afc_config_->parser().set(hub_section, "assisted_retract", val ? "True" : "False");
                afc_config_->mark_dirty();
                return AmsErrorHelper::success();
            } catch (const std::bad_any_cast&) {
                return AmsError(AmsResult::WRONG_STATE, "Invalid value type for toggle",
                                "Expected boolean", "");
            }
        }
    }

    // ---- Config-backed macro var actions (macro_vars_config_) ----
    static const std::unordered_map<std::string, std::string> macro_var_slider_keys = {
        {"ramming_volume", "variable_ramming_volume"},
        {"unloading_speed_start", "variable_unloading_speed_start"},
        {"cooling_tube_length", "variable_cooling_tube_length"},
        {"cooling_tube_retraction", "variable_cooling_tube_retraction"},
        {"purge_length", "variable_purge_length"},
    };

    static const std::unordered_map<std::string, std::string> macro_var_toggle_keys = {
        {"purge_enabled", "variable_purge_enabled"},
        {"brush_enabled", "variable_brush_enabled"},
    };

    if (auto it = macro_var_slider_keys.find(action_id); it != macro_var_slider_keys.end()) {
        if (!macro_vars_config_ || !macro_vars_config_->is_loaded()) {
            return AmsError(AmsResult::WRONG_STATE, "Macro vars config not loaded",
                            "Configuration not available", "Wait for config to load");
        }
        try {
            float val = std::any_cast<float>(value);
            macro_vars_config_->parser().set("gcode_macro AFC_MacroVars", it->second,
                                             fmt::format("{:g}", val));
            macro_vars_config_->mark_dirty();
            return AmsErrorHelper::success();
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid value type for slider",
                            "Expected float", "");
        }
    }

    if (auto it = macro_var_toggle_keys.find(action_id); it != macro_var_toggle_keys.end()) {
        if (!macro_vars_config_ || !macro_vars_config_->is_loaded()) {
            return AmsError(AmsResult::WRONG_STATE, "Macro vars config not loaded",
                            "Configuration not available", "Wait for config to load");
        }
        try {
            bool val = std::any_cast<bool>(value);
            macro_vars_config_->parser().set("gcode_macro AFC_MacroVars", it->second,
                                             val ? "True" : "False");
            macro_vars_config_->mark_dirty();
            return AmsErrorHelper::success();
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid value type for toggle",
                            "Expected boolean", "");
        }
    }

    // ---- Save & Restart action ----
    if (action_id == "save_restart") {
        bool has_changes = (afc_config_ && afc_config_->has_unsaved_changes()) ||
                           (macro_vars_config_ && macro_vars_config_->has_unsaved_changes());
        if (!has_changes) {
            return AmsError(AmsResult::WRONG_STATE, "No unsaved changes", "Nothing to save", "");
        }

        auto saves_remaining = std::make_shared<std::atomic<int>>(0);

        if (afc_config_ && afc_config_->has_unsaved_changes()) {
            saves_remaining->fetch_add(1);
        }
        if (macro_vars_config_ && macro_vars_config_->has_unsaved_changes()) {
            saves_remaining->fetch_add(1);
        }

        auto save_errors = std::make_shared<std::vector<std::string>>();

        auto on_save_done = [this, saves_remaining, save_errors](bool ok, const std::string& err) {
            if (!ok) {
                spdlog::error("[AMS AFC] Config save failed: {}", err);
                save_errors->push_back(err);
            }
            if (saves_remaining->fetch_sub(1) == 1) {
                if (save_errors->empty()) {
                    // All saves succeeded — restart Klipper to apply
                    spdlog::info("[AMS AFC] All configs saved, sending RESTART");
                    execute_gcode("RESTART");
                } else {
                    spdlog::error("[AMS AFC] {} config save(s) failed, NOT restarting",
                                  save_errors->size());
                }
            }
        };

        if (afc_config_ && afc_config_->has_unsaved_changes()) {
            afc_config_->save("AFC/AFC.cfg", on_save_done);
        }
        if (macro_vars_config_ && macro_vars_config_->has_unsaved_changes()) {
            macro_vars_config_->save("AFC/AFC_Macro_Vars.cfg", on_save_done);
        }

        return AmsErrorHelper::success();
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}
