// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_happy_hare.h"

#include "ams_bypass_policy.h"
#include "ams_fault_event.h"
#include "ams_state.h"
#include "config.h"
#include "hh_defaults.h"
#include "humidity_sensor_types.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "operation_patterns.h" // helix::contains_ci
#include "settings_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <sstream>

using namespace helix;

namespace {

/// The two printer.mmu filament_pos values Happy Hare's own check_if_loaded()
/// treats as "not loaded" (mmu.py FILAMENT_POS_UNKNOWN / FILAMENT_POS_UNLOADED).
/// Every other position, including the intermediate ones, is refused.
constexpr int HAPPY_HARE_POS_UNKNOWN = -1;
constexpr int HAPPY_HARE_POS_UNLOADED = 0;

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsBackendHappyHare::AmsBackendHappyHare(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info with Happy Hare defaults
    system_info_.type = AmsType::HAPPY_HARE;
    system_info_.type_name = "Happy Hare";
    // Endless spool AVAILABILITY is unconditional for Happy Hare and lives in
    // get_endless_spool_capabilities(). This is the ENABLE bit, and it starts
    // false so nothing claims the feature is running before mmu.
    // endless_spool_enabled arrives (see handle_status_update).
    system_info_.endless_spool_enabled = false;
    system_info_.supports_tool_mapping = true;
    // Bypass support is determined at runtime from mmu.has_bypass status field.
    // Starts false so the bypass UI stays absent until the firmware confirms it:
    // an optimistic default shows the toggle, the Device Operations section and
    // the path node on every connect, then withdraws all three a moment later on
    // any machine that has no bypass. A control arriving late reads as loading;
    // one that appears and vanishes reads as a bug.
    system_info_.supports_bypass = false;
    // Happy Hare bypass is always positional (selector moves to bypass position), never a sensor
    system_info_.has_hardware_bypass_sensor = false;
    // Default to TIP_FORM -- Happy Hare's default macro is _MMU_FORM_TIP.
    // Overridden by query_tip_method_from_config() once configfile response arrives.
    system_info_.tip_method = TipMethod::TIP_FORM;

    spdlog::debug("[AMS HappyHare] Backend created");
}

// ============================================================================
// Sensor Ownership (#1054)
// ============================================================================

bool AmsBackendHappyHare::owns_filament_sensor(const std::string& bare_name,
                                               const helix::PrinterDiscovery& discovery) {
    (void)discovery; // Happy Hare's named sensors are fixed; no discovery needed.
    // Documented HH sensor names that don't carry the "mmu" substring. The
    // keyword-bearing names (mmu_gate / mmu_pre_gate_N / mmu_gear_N) are caught
    // by PrinterHardware's substring path.
    return bare_name == "extruder" || bare_name == "toolhead" || bare_name == "filament_tension" ||
           bare_name == "filament_compression";
}

AmsBackendHappyHare::~AmsBackendHappyHare() {
    // lifetime_ destructor calls invalidate() automatically
}

// ============================================================================
// Lifecycle Management
// ============================================================================

void AmsBackendHappyHare::on_started() {
    // Query configfile to determine tip method (cutter vs tip-forming).
    // Happy Hare determines this from form_tip_macro: if it contains "cut",
    // it's a cutter system; otherwise it's tip-forming or none.
    query_tip_method_from_config();

    // Query selector type to determine topology (Type A=LINEAR vs Type B=HUB)
    query_selector_type_from_config();

    // Query filament_heater name and heater_max_temp for dryer support
    query_heater_config_from_config();

    // Query configfile.settings.mmu for speed/distance defaults
    query_config_defaults();
}

// stop(), release_subscriptions(), is_running() provided by AmsSubscriptionBackend

// ============================================================================
// Event System
// ============================================================================

// set_event_callback() and emit_event() provided by AmsSubscriptionBackend

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendHappyHare::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!slots_.is_initialized()) {
        return system_info_;
    }

    // Build slot data from registry, then overlay non-slot metadata
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
    info.endless_spool_enabled = system_info_.endless_spool_enabled;
    info.supports_tool_mapping = system_info_.supports_tool_mapping;
    info.supports_bypass = system_info_.supports_bypass;
    info.has_hardware_bypass_sensor = system_info_.has_hardware_bypass_sensor;
    info.tip_method = system_info_.tip_method;
    info.supports_purge = system_info_.supports_purge;

    // Happy Hare v4 extended fields
    info.spoolman_mode = system_info_.spoolman_mode;
    info.pending_spool_id = system_info_.pending_spool_id;
    info.espooler_state = system_info_.espooler_state;
    info.sync_feedback_state = system_info_.sync_feedback_state;
    info.sync_drive = system_info_.sync_drive;
    info.clog_detection = system_info_.clog_detection;
    info.encoder_flow_rate = system_info_.encoder_flow_rate;
    info.encoder_info = system_info_.encoder_info;
    info.flowguard_info = system_info_.flowguard_info;
    info.sync_feedback_flow_rate = system_info_.sync_feedback_flow_rate;
    info.sync_feedback_bias = system_info_.sync_feedback_bias;
    info.sync_feedback_bias_raw = system_info_.sync_feedback_bias_raw;
    info.toolchange_purge_volume = system_info_.toolchange_purge_volume;

    // Copy unit-level metadata not managed by registry
    for (size_t u = 0; u < info.units.size() && u < system_info_.units.size(); ++u) {
        info.units[u].connected = system_info_.units[u].connected;
        info.units[u].has_encoder = system_info_.units[u].has_encoder;
        info.units[u].has_toolhead_sensor = system_info_.units[u].has_toolhead_sensor;
        info.units[u].has_slot_sensors = system_info_.units[u].has_slot_sensors;
        info.units[u].topology = system_info_.units[u].topology;
        info.units[u].has_hub_sensor = system_info_.units[u].has_hub_sensor;
        info.units[u].hub_sensor_triggered = system_info_.units[u].hub_sensor_triggered;
    }

    // Surface per-unit environment data (box heater temp + humidity) so the AMS
    // panel indicator (heat-waves icon, live temp) and the dryer overlay show a
    // reading. Each unit resolves its OWN heater + env sensor: a scalar shared
    // sensor/heater applies to every unit (QIDI Box, common case); a per-gate
    // (EMU) list maps each unit to the object at its first gate, so multi-MMU rigs
    // with distinct box sensors read correctly. Per-gate *drying control*
    // (start/stop/countdown) still uses the global dryer model — true per-gate
    // drying is a separate gap (drying_state array; see parse_mmu_state).
    if (dryer_info_.supported) {
        for (auto& unit : info.units) {
            const int gi = unit.first_slot_global_index;

            std::string heater = filament_heater_name_;
            if (heater.empty() && gi >= 0 && gi < static_cast<int>(filament_heaters_.size())) {
                heater = filament_heaters_[gi];
            }
            std::string sensor = environment_sensor_name_;
            if (sensor.empty() && gi >= 0 && gi < static_cast<int>(environment_sensors_.size())) {
                sensor = environment_sensors_[gi];
            }

            // Temperature: prefer a live heater reading, then the env sensor's own
            // ambient temperature (heater-less enclosures), then the global dryer
            // temp (scalar/shared, object-form drying_state).
            float temp = 0.0f;
            bool have_temp = false;
            if (auto t = heater_temp_.find(heater); t != heater_temp_.end()) {
                temp = t->second;
                have_temp = true;
            } else if (auto st = sensor_temp_.find(sensor); st != sensor_temp_.end()) {
                temp = st->second;
                have_temp = true;
            } else if (dryer_info_.current_temp_c > 0.0f) {
                temp = dryer_info_.current_temp_c;
                have_temp = true;
            }

            float humidity = 0.0f;
            bool have_humidity = false;
            if (auto h = sensor_humidity_.find(sensor); h != sensor_humidity_.end()) {
                humidity = h->second;
                have_humidity = true;
            }

            // Surface the unit's environment whenever ANY reading is present. Gating
            // on a positive heater temp previously discarded humidity for enclosures
            // monitored without (or before) a heater reading.
            if (!have_temp && !have_humidity) {
                continue;
            }

            EnvironmentData env;
            env.temperature_c = temp; // 0 only if humidity-only and no temp source
            if (have_humidity) {
                env.humidity_pct = humidity;
                env.has_humidity = true;
            }
            unit.environment = env;
        }
    }

    return info;
}

AmsType AmsBackendHappyHare::get_type() const {
    return AmsType::HAPPY_HARE;
}

bool AmsBackendHappyHare::manages_active_spool() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.spoolman_mode != SpoolmanMode::OFF;
}

SlotInfo AmsBackendHappyHare::get_slot_info(int slot_index) const {
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

PathTopology AmsBackendHappyHare::get_topology() const {
    // Type B (VirtualSelector) uses HUB topology (3MS, Box Turtle, Night Owl)
    // Type A (LinearSelector, RotarySelector, ServoSelector) uses LINEAR (ERCF, Tradrack)
    std::lock_guard<std::mutex> lock(mutex_);
    return is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
}

PathTopology AmsBackendHappyHare::get_unit_topology(int unit_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size())) {
        return system_info_.units[unit_index].topology;
    }
    return is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
}

bool AmsBackendHappyHare::is_type_b() const {
    return selector_type_ == "VirtualSelector";
}

void AmsBackendHappyHare::update_unit_topologies() {
    auto topo = is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
    bool encoder = !is_type_b();
    for (auto& unit : system_info_.units) {
        unit.topology = topo;
        unit.has_encoder = encoder;
    }
}

PathSegment AmsBackendHappyHare::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Convert Happy Hare filament_pos to unified PathSegment
    return path_segment_from_happy_hare_pos(filament_pos_);
}

PathSegment AmsBackendHappyHare::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if this is the active slot - return the current filament segment
    if (slot_index == system_info_.current_slot && system_info_.filament_loaded) {
        return path_segment_from_happy_hare_pos(filament_pos_);
    }

    // For non-active slots, check pre-gate sensor first for better visualization
    const auto* entry = slots_.get(slot_index);
    if (entry) {
        if (entry->sensors.has_pre_gate_sensor && entry->sensors.pre_gate_triggered) {
            return PathSegment::PREP; // Filament detected at pre-gate sensor
        }

        // Fall back to gate_status for slots without pre-gate sensors
        if (entry->info.status == SlotStatus::AVAILABLE ||
            entry->info.status == SlotStatus::FROM_BUFFER) {
            return PathSegment::SPOOL; // Filament at spool ready position
        }
    }

    return PathSegment::NONE;
}

PathSegment AmsBackendHappyHare::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_segment_;
}

bool AmsBackendHappyHare::slot_has_prep_sensor(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* entry = slots_.get(slot_index);
    if (!entry) {
        return false;
    }
    return entry->sensors.has_pre_gate_sensor;
}

// ============================================================================
// Moonraker Status Update Handling
// ============================================================================

void AmsBackendHappyHare::handle_status_update(const nlohmann::json& notification) {
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

    spdlog::trace("[AMS HappyHare] Received status update");

    // Parse MMU core state if present.
    const bool mmu_present = params.contains("mmu") && params["mmu"].is_object();
    if (mmu_present) {
        std::lock_guard<std::mutex> lock(mutex_);
        parse_mmu_state(params["mmu"]);
    }

    // Parse live heater_generic temp/target even when mmu key is absent —
    // Moonraker sends heater updates as sibling keys in the same notification.
    const bool heater_updated = apply_filament_heater_status(params);

    // Parse box humidity from the environment sensor chip (sibling key too).
    const bool humidity_updated = apply_environment_sensor_status(params);

    // Only re-pump downstream sync when this frame actually carried AMS-relevant
    // data; notify_status_update fires for every Klipper object (toolhead, temps,
    // ...), so an unconditional emit here would be a per-frame event storm.
    if (mmu_present || heater_updated || humidity_updated) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendHappyHare::parse_mmu_state(const nlohmann::json& mmu_data) {
    // Parse current gate: printer.mmu.gate
    // -1 = no gate selected, -2 = bypass
    if (mmu_data.contains("gate") && mmu_data["gate"].is_number_integer()) {
        system_info_.current_slot = mmu_data["gate"].get<int>();
        spdlog::trace("[AMS HappyHare] Current slot: {}", system_info_.current_slot);
    }

    // Parse current tool: printer.mmu.tool
    if (mmu_data.contains("tool") && mmu_data["tool"].is_number_integer()) {
        system_info_.current_tool = mmu_data["tool"].get<int>();
        spdlog::trace("[AMS HappyHare] Current tool: {}", system_info_.current_tool);
    }

    // Parse filament loaded state: printer.mmu.filament
    // Values: "Loaded", "Unloaded"
    if (mmu_data.contains("filament") && mmu_data["filament"].is_string()) {
        std::string filament_state = mmu_data["filament"].get<std::string>();
        system_info_.filament_loaded = (filament_state == "Loaded");
        spdlog::trace("[AMS HappyHare] Filament loaded: {}", system_info_.filament_loaded);
    }

    // Parse reason_for_pause: descriptive error message from Happy Hare
    if (mmu_data.contains("reason_for_pause") && mmu_data["reason_for_pause"].is_string()) {
        reason_for_pause_ = mmu_data["reason_for_pause"].get<std::string>();
        spdlog::trace("[AMS HappyHare] Reason for pause: {}", reason_for_pause_);
    }

    // Parse action: printer.mmu.action
    // Values: "Idle", "Loading", "Unloading", "Forming Tip", "Heating", "Checking", etc.
    if (mmu_data.contains("action") && mmu_data["action"].is_string()) {
        std::string action_str = mmu_data["action"].get<std::string>();
        AmsAction prev_action = system_info_.action;
        system_info_.action = ams_action_from_string(action_str);
        system_info_.operation_detail = action_str;
        spdlog::trace("[AMS HappyHare] Action: {} ({})", ams_action_to_string(system_info_.action),
                      action_str);

        // Clear error segment when recovering to idle
        if (prev_action == AmsAction::ERROR && system_info_.action == AmsAction::IDLE) {
            error_segment_ = PathSegment::NONE;
            reason_for_pause_.clear();

            // Clear slot errors on all slots
            for (int i = 0; i < slots_.slot_count(); ++i) {
                auto* entry = slots_.get_mut(i);
                if (entry && entry->info.error.has_value()) {
                    entry->info.error.reset();
                    spdlog::debug("[AMS HappyHare] Cleared error on slot {}", i);
                }
            }
        }

        // Set slot error when entering error state
        if (system_info_.action == AmsAction::ERROR && prev_action != AmsAction::ERROR) {
            error_segment_ = path_segment_from_happy_hare_pos(filament_pos_);

            // Set error on current slot (if valid)
            if (system_info_.current_slot >= 0) {
                auto* entry = slots_.get_mut(system_info_.current_slot);
                if (entry) {
                    SlotError err;
                    // Use reason_for_pause if available; fall back to operation_detail
                    if (!reason_for_pause_.empty()) {
                        err.message = reason_for_pause_;
                    } else {
                        err.message = action_str;
                    }
                    err.severity = SlotError::ERROR;
                    entry->info.error = err;
                    spdlog::debug("[AMS HappyHare] Error on slot {}: {}", system_info_.current_slot,
                                  err.message);
                }
            }
        }

        // Drive the toolchange step bar from the action transition (HH has no
        // // narration). Deferred to main thread inside the helper.
        sync_narration_step();
    }

    // Parse filament_pos: printer.mmu.filament_pos
    // Values: 0=unloaded, 1-2=gate area, 3=in bowden, 4=end bowden, 5=homed extruder,
    //         6=extruder entry, 7-8=loaded
    if (mmu_data.contains("filament_pos") && mmu_data["filament_pos"].is_number_integer()) {
        filament_pos_ = mmu_data["filament_pos"].get<int>();
        spdlog::trace("[AMS HappyHare] Filament pos: {} -> {}", filament_pos_,
                      path_segment_to_string(path_segment_from_happy_hare_pos(filament_pos_)));

        // Update hub_sensor_triggered on units based on filament position
        // pos >= 3 means filament is in bowden or further (past the selector/hub)
        bool past_hub = (filament_pos_ >= 3);
        for (auto& unit : system_info_.units) {
            // Active unit: determined by current_slot falling within this unit's range
            int slot = system_info_.current_slot;
            if (slot >= unit.first_slot_global_index &&
                slot < unit.first_slot_global_index + unit.slot_count) {
                unit.hub_sensor_triggered = past_hub;
            } else {
                unit.hub_sensor_triggered = false;
            }
        }
    }

    // Parse bowden_progress: printer.mmu.bowden_progress (v4)
    // 0-100 = loading progress percentage, -1 = not applicable
    if (mmu_data.contains("bowden_progress") && mmu_data["bowden_progress"].is_number_integer()) {
        bowden_progress_ = std::clamp(mmu_data["bowden_progress"].get<int>(), -1, 100);
        spdlog::trace("[AMS HappyHare] Bowden progress: {}%", bowden_progress_);
    }

    // Parse has_bypass: printer.mmu.has_bypass
    // Not all MMU types support bypass (e.g., ERCF/Tradrack do, BoxTurtle does not)
    //
    // Logged at info rather than trace, and on every change rather than never:
    // false here removes the entire bypass UI (sidebar toggle, Device Operations
    // section, path node) and this flag is the sole reason. Happy Hare derives it
    // from [mmu_machine] has_bypass, which defaults to 0 for mmu_vendor "Other",
    // and on type-A selectors ANDs it with the calibrated bypass offset — so an
    // owner with a physical bypass can legitimately see false and have no way to
    // tell that from a bug in us.
    if (mmu_data.contains("has_bypass") && mmu_data["has_bypass"].is_boolean()) {
        const bool has_bypass = mmu_data["has_bypass"].get<bool>();
        if (!bypass_support_seen_ || has_bypass != system_info_.supports_bypass) {
            spdlog::info("[AMS HappyHare] Bypass supported: {}", has_bypass);
            bypass_support_seen_ = true;
        }
        system_info_.supports_bypass = has_bypass;
    } else if (!bypass_support_seen_) {
        // Field absent entirely. Every Happy Hare we know of publishes it, so this
        // is a fork or a version we have not seen; assume supported rather than
        // silently removing a control the machine may well have. Deliberately not
        // the same as the pre-status default, which is false so that a system
        // without a bypass never flashes the UI up and then withdraws it.
        bypass_support_seen_ = true;
        system_info_.supports_bypass = true;
        spdlog::warn("[AMS HappyHare] No has_bypass field in mmu status; assuming supported");
    }

    // Parse num_units if available (multi-unit Happy Hare setups)
    if (mmu_data.contains("num_units") && mmu_data["num_units"].is_number_integer()) {
        num_units_ = mmu_data["num_units"].get<int>();
        if (num_units_ < 1)
            num_units_ = 1;
        spdlog::trace("[AMS HappyHare] Number of units: {}", num_units_);
    }

    // Parse num_gates for dissimilar multi-unit (v4)
    // Can be a string like "6,4" for 6-gate ERCF + 4-gate Box Turtle, or plain int
    if (mmu_data.contains("num_gates")) {
        const auto& ng = mmu_data["num_gates"];
        if (ng.is_string()) {
            // Parse comma-separated per-unit gate counts (v4 dissimilar multi-MMU)
            std::string ng_str = ng.get<std::string>();
            std::vector<int> counts;
            std::istringstream iss(ng_str);
            std::string token;
            while (std::getline(iss, token, ',')) {
                try {
                    int count = std::stoi(token);
                    if (count > 0) {
                        counts.push_back(count);
                    } else {
                        spdlog::warn("[AMS HappyHare] Ignoring non-positive gate count {} in "
                                     "num_gates string",
                                     count);
                    }
                } catch (...) {
                    spdlog::warn("[AMS HappyHare] Ignoring invalid token in num_gates string");
                }
            }
            if (!counts.empty()) {
                per_unit_gate_counts_ = counts;
                spdlog::debug("[AMS HappyHare] Per-unit gate counts from num_gates string: {}",
                              ng_str);
            }
        } else if (ng.is_number_integer()) {
            // EMU sends plain integer (single unit)
            int count = ng.get<int>();
            if (count > 0) {
                per_unit_gate_counts_ = {count};
                spdlog::debug("[AMS HappyHare] Single-unit gate count from num_gates int: {}",
                              count);
            }
        } else if (ng.is_array()) {
            // Config format: [8] or [6, 4]
            std::vector<int> counts;
            for (const auto& c : ng) {
                if (c.is_number_integer()) {
                    int count = c.get<int>();
                    if (count > 0)
                        counts.push_back(count);
                }
            }
            if (!counts.empty()) {
                per_unit_gate_counts_ = counts;
                spdlog::debug("[AMS HappyHare] Per-unit gate counts from num_gates array");
            }
        }
    }

    // Parse unit_gate_counts array if present (future-proof for v4)
    if (mmu_data.contains("unit_gate_counts") && mmu_data["unit_gate_counts"].is_array()) {
        std::vector<int> counts;
        for (const auto& c : mmu_data["unit_gate_counts"]) {
            if (c.is_number_integer()) {
                counts.push_back(c.get<int>());
            }
        }
        if (!counts.empty()) {
            per_unit_gate_counts_ = counts;
            spdlog::debug("[AMS HappyHare] Per-unit gate counts from unit_gate_counts array");
        }
    }

    // Parse active unit: printer.mmu.unit (v4)
    if (mmu_data.contains("unit") && mmu_data["unit"].is_number_integer()) {
        active_unit_ = mmu_data["unit"].get<int>();
        spdlog::trace("[AMS HappyHare] Active unit: {}", active_unit_);
    }

    // Parse gate_status array: printer.mmu.gate_status
    // Values: -1 = unknown, 0 = empty, 1 = available, 2 = from_buffer
    if (mmu_data.contains("gate_status") && mmu_data["gate_status"].is_array()) {
        const auto& gate_status = mmu_data["gate_status"];
        int gate_count = static_cast<int>(gate_status.size());

        // Initialize gates if this is the first time we see gate_status
        if (!slots_.is_initialized() && gate_count > 0) {
            initialize_slots(gate_count);
        }

        // Cache the raw values. The LOADED stamp is applied by
        // refresh_gate_statuses_locked() at the end of this function rather than
        // here, because it depends on gate/filament — which arrive in their own
        // deltas, without gate_status (#1199).
        if (gate_status_raw_.size() != gate_status.size()) {
            gate_status_raw_.assign(gate_status.size(), -1);
        }
        for (size_t i = 0; i < gate_status.size(); ++i) {
            if (gate_status[i].is_number_integer()) {
                gate_status_raw_[i] = gate_status[i].get<int>();
            }
        }
    }

    // Parse gate_color_rgb: integer array [0xRRGGBB, ...] or float array [[R,G,B], ...]
    bool colors_parsed = false;
    if (mmu_data.contains("gate_color_rgb") && mmu_data["gate_color_rgb"].is_array()) {
        const auto& colors = mmu_data["gate_color_rgb"];
        for (size_t i = 0; i < colors.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (!entry)
                continue;

            if (colors[i].is_number_integer()) {
                // Traditional format: 0xRRGGBB integer
                entry->info.color_rgb = static_cast<uint32_t>(colors[i].get<int>());
                colors_parsed = true;
            } else if (colors[i].is_array() && colors[i].size() >= 3 && colors[i][0].is_number() &&
                       colors[i][1].is_number() && colors[i][2].is_number()) {
                // EMU format: [R, G, B] floats 0.0-1.0
                auto r = static_cast<uint8_t>(
                    std::clamp(colors[i][0].get<double>(), 0.0, 1.0) * 255.0 + 0.5);
                auto g = static_cast<uint8_t>(
                    std::clamp(colors[i][1].get<double>(), 0.0, 1.0) * 255.0 + 0.5);
                auto b = static_cast<uint8_t>(
                    std::clamp(colors[i][2].get<double>(), 0.0, 1.0) * 255.0 + 0.5);
                entry->info.color_rgb = (static_cast<uint32_t>(r) << 16) |
                                        (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                colors_parsed = true;
            }
        }
    }

    // Fallback: parse gate_color hex strings ["ffffff", "000000", ...]
    if (!colors_parsed && mmu_data.contains("gate_color") && mmu_data["gate_color"].is_array()) {
        const auto& colors = mmu_data["gate_color"];
        for (size_t i = 0; i < colors.size(); ++i) {
            if (colors[i].is_string()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    try {
                        entry->info.color_rgb = static_cast<uint32_t>(
                            std::stoul(colors[i].get<std::string>(), nullptr, 16));
                    } catch (...) {
                        // Invalid hex string, leave default color
                    }
                }
            }
        }
    }

    // Parse gate_material array: printer.mmu.gate_material
    // Values are strings like "PLA", "PETG", "ABS"
    if (mmu_data.contains("gate_material") && mmu_data["gate_material"].is_array()) {
        const auto& materials = mmu_data["gate_material"];
        for (size_t i = 0; i < materials.size(); ++i) {
            if (materials[i].is_string()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    entry->info.material = materials[i].get<std::string>();
                }
            }
        }
    }

    // === Happy Hare v4 extended status fields ===

    // Parse espooler_active: printer.mmu.espooler_active (v4)
    if (mmu_data.contains("espooler_active") && mmu_data["espooler_active"].is_string()) {
        system_info_.espooler_state = mmu_data["espooler_active"].get<std::string>();
        espooler_active_ = system_info_.espooler_state;
        spdlog::trace("[AMS HappyHare] eSpooler state: {}", system_info_.espooler_state);
    }

    // Parse sync_feedback_state: printer.mmu.sync_feedback_state (v4)
    if (mmu_data.contains("sync_feedback_state") && mmu_data["sync_feedback_state"].is_string()) {
        system_info_.sync_feedback_state = mmu_data["sync_feedback_state"].get<std::string>();
        spdlog::trace("[AMS HappyHare] Sync feedback: {}", system_info_.sync_feedback_state);
    }

    // Parse sync_feedback_bias_modelled: printer.mmu.sync_feedback_bias_modelled (v4)
    if (mmu_data.contains("sync_feedback_bias_modelled") &&
        mmu_data["sync_feedback_bias_modelled"].is_number()) {
        system_info_.sync_feedback_bias = mmu_data["sync_feedback_bias_modelled"].get<float>();
        spdlog::trace("[AMS HappyHare] Sync feedback bias (modelled): {:.3f}",
                      system_info_.sync_feedback_bias);
    }

    // Parse sync_feedback_bias_raw: printer.mmu.sync_feedback_bias_raw (v4)
    if (mmu_data.contains("sync_feedback_bias_raw") &&
        mmu_data["sync_feedback_bias_raw"].is_number()) {
        system_info_.sync_feedback_bias_raw = mmu_data["sync_feedback_bias_raw"].get<float>();
        spdlog::trace("[AMS HappyHare] Sync feedback bias (raw): {:.3f}",
                      system_info_.sync_feedback_bias_raw);
    }

    // Parse sync_drive: printer.mmu.sync_drive (v4)
    if (mmu_data.contains("sync_drive") && mmu_data["sync_drive"].is_boolean()) {
        system_info_.sync_drive = mmu_data["sync_drive"].get<bool>();
        spdlog::trace("[AMS HappyHare] Sync drive: {}", system_info_.sync_drive);
    }

    // Parse clog_detection_enabled: printer.mmu.clog_detection_enabled (v4)
    // 0=off, 1=manual, 2=auto
    if (mmu_data.contains("clog_detection_enabled") &&
        mmu_data["clog_detection_enabled"].is_number_integer()) {
        system_info_.clog_detection = mmu_data["clog_detection_enabled"].get<int>();
        system_info_.encoder_info.detection_mode = system_info_.clog_detection;
        system_info_.encoder_info.enabled = (system_info_.clog_detection > 0);
        spdlog::trace("[AMS HappyHare] Clog detection: {}", system_info_.clog_detection);
    }

    // Parse encoder: printer.mmu.encoder (v4, nested)
    if (mmu_data.contains("encoder") && mmu_data["encoder"].is_object()) {
        const auto& encoder = mmu_data["encoder"];
        if (encoder.contains("flow_rate") && encoder["flow_rate"].is_number()) {
            system_info_.encoder_info.flow_rate = encoder["flow_rate"].get<int>();
            // Keep legacy field in sync
            system_info_.encoder_flow_rate = system_info_.encoder_info.flow_rate;
            spdlog::trace("[AMS HappyHare] Encoder flow rate: {}", system_info_.encoder_flow_rate);
        }
        if (encoder.contains("desired_headroom") && encoder["desired_headroom"].is_number()) {
            system_info_.encoder_info.desired_headroom = encoder["desired_headroom"].get<float>();
        }
        if (encoder.contains("detection_length") && encoder["detection_length"].is_number()) {
            system_info_.encoder_info.detection_length = encoder["detection_length"].get<float>();
        }
        if (encoder.contains("headroom") && encoder["headroom"].is_number()) {
            system_info_.encoder_info.headroom = encoder["headroom"].get<float>();
        }
        if (encoder.contains("min_headroom") && encoder["min_headroom"].is_number()) {
            system_info_.encoder_info.min_headroom = encoder["min_headroom"].get<float>();
        }
        spdlog::trace("[AMS HappyHare] Encoder: headroom={:.1f}/{:.1f} min={:.1f}",
                      system_info_.encoder_info.headroom,
                      system_info_.encoder_info.detection_length,
                      system_info_.encoder_info.min_headroom);
    }

    // Parse flowguard: printer.mmu.flowguard (v4, nested)
    if (mmu_data.contains("flowguard") && mmu_data["flowguard"].is_object()) {
        const auto& fg = mmu_data["flowguard"];
        if (fg.contains("enabled") && fg["enabled"].is_boolean()) {
            system_info_.flowguard_info.enabled = fg["enabled"].get<bool>();
        }
        if (fg.contains("active") && fg["active"].is_boolean()) {
            system_info_.flowguard_info.active = fg["active"].get<bool>();
        }
        if (fg.contains("trigger") && fg["trigger"].is_string()) {
            system_info_.flowguard_info.trigger = fg["trigger"].get<std::string>();
        }
        if (fg.contains("level") && fg["level"].is_number()) {
            system_info_.flowguard_info.level = fg["level"].get<float>();
        }
        if (fg.contains("max_clog") && fg["max_clog"].is_number()) {
            system_info_.flowguard_info.max_clog = fg["max_clog"].get<float>();
        }
        if (fg.contains("max_tangle") && fg["max_tangle"].is_number()) {
            system_info_.flowguard_info.max_tangle = fg["max_tangle"].get<float>();
        }
        if (fg.contains("encoder_mode") && fg["encoder_mode"].is_number()) {
            flowguard_encoder_mode_ = fg["encoder_mode"].get<int>();
        }
        spdlog::trace("[AMS HappyHare] Flowguard: enabled={} active={} trigger={} level={:.2f}",
                      system_info_.flowguard_info.enabled, system_info_.flowguard_info.active,
                      system_info_.flowguard_info.trigger, system_info_.flowguard_info.level);
    }

    // Parse LED state: printer.mmu.leds.unit0.exit_effect (v4)
    if (mmu_data.contains("leds") && mmu_data["leds"].is_object()) {
        const auto& leds = mmu_data["leds"];
        if (leds.contains("unit0") && leds["unit0"].is_object()) {
            const auto& u0 = leds["unit0"];
            if (u0.contains("exit_effect") && u0["exit_effect"].is_string()) {
                led_exit_effect_ = u0["exit_effect"].get<std::string>();
                spdlog::trace("[AMS HappyHare] LED exit effect: {}", led_exit_effect_);
            }
        }
    }

    // Parse sync_feedback_flow_rate: printer.mmu.sync_feedback_flow_rate (top-level)
    if (mmu_data.contains("sync_feedback_flow_rate") &&
        mmu_data["sync_feedback_flow_rate"].is_number()) {
        system_info_.sync_feedback_flow_rate = mmu_data["sync_feedback_flow_rate"].get<float>();
        spdlog::trace("[AMS HappyHare] Sync feedback flow rate: {:.1f}",
                      system_info_.sync_feedback_flow_rate);
    }

    // Parse toolchange_purge_volume: printer.mmu.toolchange_purge_volume (v4)
    if (mmu_data.contains("toolchange_purge_volume") &&
        mmu_data["toolchange_purge_volume"].is_number()) {
        system_info_.toolchange_purge_volume = mmu_data["toolchange_purge_volume"].get<float>();
        spdlog::trace("[AMS HappyHare] Toolchange purge volume: {:.1f}",
                      system_info_.toolchange_purge_volume);
    }

    // Parse num_toolchanges: printer.mmu.num_toolchanges
    // Count of completed tool changes (1 = first swap done). Convert to 0-based index.
    if (mmu_data.contains("num_toolchanges") && mmu_data["num_toolchanges"].is_number_integer()) {
        int count = mmu_data["num_toolchanges"].get<int>();
        system_info_.current_toolchange = (count > 0) ? (count - 1) : -1;
        spdlog::trace("[AMS HappyHare] Toolchange count: {} -> index: {}", count,
                      system_info_.current_toolchange);
    }

    // Parse slicer_tool_map.total_toolchanges: printer.mmu.slicer_tool_map
    // Contains total_toolchanges (int or null) from slicer metadata
    if (mmu_data.contains("slicer_tool_map") && mmu_data["slicer_tool_map"].is_object()) {
        const auto& stm = mmu_data["slicer_tool_map"];
        if (stm.contains("total_toolchanges") && stm["total_toolchanges"].is_number_integer()) {
            system_info_.number_of_toolchanges = stm["total_toolchanges"].get<int>();
            spdlog::trace("[AMS HappyHare] Total toolchanges from slicer: {}",
                          system_info_.number_of_toolchanges);
        } else {
            system_info_.number_of_toolchanges = 0;
        }
    }

    // Parse spoolman_support: printer.mmu.spoolman_support (v4)
    if (mmu_data.contains("spoolman_support") && mmu_data["spoolman_support"].is_string()) {
        system_info_.spoolman_mode =
            spoolman_mode_from_string(mmu_data["spoolman_support"].get<std::string>());
        spdlog::trace("[AMS HappyHare] Spoolman mode: {}",
                      spoolman_mode_to_string(system_info_.spoolman_mode));
    }

    // Parse pending_spool_id: printer.mmu.pending_spool_id (v4)
    if (mmu_data.contains("pending_spool_id") && mmu_data["pending_spool_id"].is_number_integer()) {
        system_info_.pending_spool_id = mmu_data["pending_spool_id"].get<int>();
        spdlog::trace("[AMS HappyHare] Pending spool ID: {}", system_info_.pending_spool_id);
    }

    // Parse gate_spool_id array: printer.mmu.gate_spool_id (v4)
    // Per-gate Spoolman spool IDs — enables weight polling and fill gauges
    if (mmu_data.contains("gate_spool_id") && mmu_data["gate_spool_id"].is_array()) {
        const auto& spool_ids = mmu_data["gate_spool_id"];
        for (size_t i = 0; i < spool_ids.size(); ++i) {
            if (spool_ids[i].is_number_integer()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    int id = spool_ids[i].get<int>();
                    entry->info.spoolman_id = (id > 0) ? id : 0;
                }
            }
        }
        // Re-supply user-attached identity the gate map cannot carry.
        for (size_t i = 0; i < spool_ids.size(); ++i) {
            if (auto* entry = slots_.get_mut(static_cast<int>(i))) {
                apply_overrides(entry->info, static_cast<int>(i));
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_spool_id for {} gates", spool_ids.size());
    }

    // Parse gate_temperature array: printer.mmu.gate_temperature (v4)
    // Per-gate nozzle temperature recommendations
    if (mmu_data.contains("gate_temperature") && mmu_data["gate_temperature"].is_array()) {
        const auto& gate_temps = mmu_data["gate_temperature"];
        for (size_t i = 0; i < gate_temps.size(); ++i) {
            if (gate_temps[i].is_number()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    int temp = gate_temps[i].get<int>();
                    entry->info.nozzle_temp_min = temp;
                    entry->info.nozzle_temp_max = temp;
                }
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_temperature for {} gates", gate_temps.size());
    }

    // Parse gate_name array: printer.mmu.gate_name (v4)
    // Per-gate filament names
    if (mmu_data.contains("gate_name") && mmu_data["gate_name"].is_array()) {
        const auto& gate_names = mmu_data["gate_name"];
        for (size_t i = 0; i < gate_names.size(); ++i) {
            if (gate_names[i].is_string()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    entry->info.color_name = gate_names[i].get<std::string>();
                }
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_name for {} gates", gate_names.size());
    }

    // Fallback: parse gate_filament_name (EMU uses this instead of gate_name)
    if (mmu_data.contains("gate_filament_name") && mmu_data["gate_filament_name"].is_array()) {
        const auto& names = mmu_data["gate_filament_name"];
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].is_string()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry && entry->info.color_name.empty()) {
                    entry->info.color_name = names[i].get<std::string>();
                }
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_filament_name for {} gates", names.size());
    }

    // Parse ttg_map (tool-to-gate mapping) if available
    if (mmu_data.contains("ttg_map") && mmu_data["ttg_map"].is_array()) {
        const auto& ttg_map = mmu_data["ttg_map"];
        std::vector<int> ttg_vec;
        ttg_vec.reserve(ttg_map.size());

        for (const auto& mapping : ttg_map) {
            if (mapping.is_number_integer()) {
                ttg_vec.push_back(mapping.get<int>());
            }
        }

        // Update both legacy and registry tool maps. Firmware-sourced: HH
        // publishes the whole ttg_map in get_status() (mmu.py get_status), so
        // this array IS what the MMU currently believes, not our intent. The
        // optimistic counterpart is set_tool_mapping()'s own write below, which
        // precedes the MMU_TTG_MAP send (#1270).
        system_info_.tool_to_slot_map = ttg_vec;
        slots_.set_tool_map(ttg_vec, helix::printer::SlotRegistry::MappingSource::Firmware);
    }

    // Parse sensors dict: printer.mmu.sensors
    // Keys matching "mmu_pre_gate_X" indicate pre-gate sensors per gate.
    // Values: true (triggered/filament present), false (not triggered), null (error/unknown)
    if (mmu_data.contains("sensors") && mmu_data["sensors"].is_object()) {
        const auto& sensors = mmu_data["sensors"];
        const std::string prefix = "mmu_pre_gate_";
        bool any_sensor = false;

        for (auto it = sensors.begin(); it != sensors.end(); ++it) {
            const std::string& key = it.key();
            if (key.rfind(prefix, 0) != 0) {
                continue; // Not a pre-gate sensor key
            }

            // Extract gate index from key suffix
            std::string index_str = key.substr(prefix.size());
            int gate_idx = -1;
            try {
                gate_idx = std::stoi(index_str);
            } catch (...) {
                continue; // Not a valid integer suffix
            }

            if (gate_idx < 0) {
                continue;
            }

            auto* entry = slots_.get_mut(gate_idx);
            if (!entry) {
                continue;
            }

            entry->sensors.has_pre_gate_sensor = true;
            entry->sensors.pre_gate_triggered = it.value().is_boolean() && it.value().get<bool>();
            any_sensor = true;

            spdlog::trace("[AMS HappyHare] Pre-gate sensor {}: present=true, triggered={}",
                          gate_idx, entry->sensors.pre_gate_triggered);
        }

        // If no per-gate sensors found, check for aggregate format (EMU)
        // EMU reports "mmu_pre_gate" (bool) and "mmu_gear" (bool) for the active gate
        if (!any_sensor && sensors.contains("mmu_pre_gate")) {
            bool pre_gate_val =
                sensors["mmu_pre_gate"].is_boolean() && sensors["mmu_pre_gate"].get<bool>();
            // Note: mmu_gear sensor reading is available but not stored — UI only
            // displays pre-gate sensor status. Add to SlotSensors if needed later.

            // Mark all gates as having sensors, clear stale trigger readings
            // (we only know the current gate's state from aggregate format)
            for (int i = 0; i < slots_.slot_count(); ++i) {
                auto* entry = slots_.get_mut(i);
                if (entry) {
                    entry->sensors.has_pre_gate_sensor = true;
                    entry->sensors.pre_gate_triggered = false;
                }
            }

            // Set the current gate's actual reading
            if (system_info_.current_slot >= 0) {
                auto* entry = slots_.get_mut(system_info_.current_slot);
                if (entry) {
                    entry->sensors.pre_gate_triggered = pre_gate_val;
                }
            }

            any_sensor = true;
            spdlog::trace("[AMS HappyHare] Aggregate sensors: pre_gate={}", pre_gate_val);
        }

        // Update has_slot_sensors flag on units based on actual sensor data
        for (auto& unit : system_info_.units) {
            unit.has_slot_sensors = any_sensor;
        }
    }

    // Parse drying_state: object (KMS/traditional) or array of strings (EMU per-gate)
    if (mmu_data.contains("drying_state")) {
        const auto& drying = mmu_data["drying_state"];
        if (drying.is_object()) {
            // Traditional object format: {active, current_temp, target_temp, ...}
            dryer_info_.supported = true;
            if (drying.contains("active") && drying["active"].is_boolean()) {
                dryer_info_.active = drying["active"].get<bool>();
            }
            if (drying.contains("current_temp") && drying["current_temp"].is_number()) {
                dryer_info_.current_temp_c = drying["current_temp"].get<float>();
            }
            if (drying.contains("target_temp") && drying["target_temp"].is_number()) {
                dryer_info_.target_temp_c = drying["target_temp"].get<float>();
            }
            if (drying.contains("remaining_min") && drying["remaining_min"].is_number_integer()) {
                dryer_info_.remaining_min = drying["remaining_min"].get<int>();
            }
            if (drying.contains("duration_min") && drying["duration_min"].is_number_integer()) {
                dryer_info_.duration_min = drying["duration_min"].get<int>();
            }
            if (drying.contains("fan_pct") && drying["fan_pct"].is_number_integer()) {
                dryer_info_.fan_pct = drying["fan_pct"].get<int>();
            }
            spdlog::trace("[AMS HappyHare] Dryer state (object): active={}, temp={:.1f}°C",
                          dryer_info_.active, dryer_info_.current_temp_c);
        } else if (drying.is_array()) {
            // EMU per-gate array format: ["", "", ...] or ["active", "", ...]
            // Values: "active", "queued" = heater on; "complete", "canceled", "" = off.
            dryer_info_.supported = true;
            bool any_active = false;
            for (const auto& entry : drying) {
                if (entry.is_string()) {
                    const std::string s = entry.get<std::string>();
                    if (s == "active" || s == "queued") {
                        any_active = true;
                        break;
                    }
                }
            }
            dryer_info_.active = any_active;
            spdlog::trace("[AMS HappyHare] Dryer state (array): supported=true, active={}",
                          any_active);
        }
    }

    // Parse the endless-spool ENABLE bit. Happy Hare publishes it under two
    // keys, both tagged DEPRECATED in mmu.py's get_status() with no replacement
    // shipped, so read the newer spelling and fall back to the older one; a
    // future HH that drops both leaves the flag at its last value rather than
    // silently flipping to off. The bit gates apply_endless_spool_backup(),
    // because cmd_MMU_ENDLESS_SPOOL ignores GROUPS while disabled.
    for (const char* key : {"endless_spool_enabled", "endless_spool"}) {
        if (mmu_data.contains(key) && !mmu_data[key].is_null()) {
            system_info_.endless_spool_enabled = helix::json_util::safe_bool(mmu_data, key, false);
            break;
        }
    }

    // Parse endless_spool_groups if available
    if (mmu_data.contains("endless_spool_groups") && mmu_data["endless_spool_groups"].is_array()) {
        const auto& es_groups = mmu_data["endless_spool_groups"];
        for (size_t i = 0; i < es_groups.size(); ++i) {
            if (es_groups[i].is_number_integer()) {
                auto* entry = slots_.get_mut(static_cast<int>(i));
                if (entry) {
                    entry->info.endless_spool_group = es_groups[i].get<int>();
                }
            }
        }
    }

    // Re-derive every gate's status from the cached gate_status array plus the
    // gate/filament pair this frame may have moved. Unconditional, and last, so
    // no ordering between the three keys can leave a stale stamp behind.
    refresh_gate_statuses_locked();
}

void AmsBackendHappyHare::refresh_gate_statuses_locked() {
    for (size_t i = 0; i < gate_status_raw_.size(); ++i) {
        auto* entry = slots_.get_mut(static_cast<int>(i));
        if (!entry) {
            continue;
        }

        SlotStatus status = slot_status_from_happy_hare(gate_status_raw_[i]);

        // The gate Happy Hare reports loaded reads LOADED whatever its fill
        // state is — including gate_status 2 (from_buffer), which the old
        // `status == AVAILABLE` precondition silently excluded, so a buffered
        // gate never showed as seated while it was feeding the toolhead.
        //
        // gate_status 0 is the deliberate exception. An empty gate that Happy
        // Hare still names as loaded is a runout: the filament it already fed is
        // at the toolhead, but the gate has nothing left, and load_filament()'s
        // "slot not available" refusal keys on EMPTY. That disagreement with the
        // aggregate pair is also why this backend does not claim
        // has_per_slot_loaded_authority() — see the comment there.
        if (system_info_.filament_loaded && static_cast<int>(i) == system_info_.current_slot &&
            status != SlotStatus::EMPTY) {
            status = SlotStatus::LOADED;
        }

        entry->info.status = status;
    }
}

// ============================================================================
// Error Classification
// ============================================================================

std::vector<helix::RecoveryAction> AmsBackendHappyHare::build_recovery_actions() const {
    // Caller holds mutex_.
    std::vector<helix::RecoveryAction> actions;

    // Resume after the user clears the fault (always offered, primary). Resuming
    // a paused print extrudes on the next move, so it needs the hotend up.
    actions.push_back({lv_tr("Resume"), "RESUME", "hh::resume", "primary",
                       /*needs_hot_nozzle=*/true});

    // MMU_RECOVER re-syncs HH's filament state; the LOADED/UNLOADED arg must match
    // reality (HH issue #729). Derive from the live loaded flag. State-only — it
    // moves nothing, so it stays available on a cold nozzle.
    const bool loaded = system_info_.filament_loaded;
    actions.push_back({lv_tr("Recover"), loaded ? "MMU_RECOVER LOADED=1" : "MMU_RECOVER UNLOADED=1",
                       "hh::recover", ""});

    // If filament is at the toolhead, offer an explicit unload. Pulls filament
    // back out through the melt zone, so it needs heat.
    if (loaded) {
        actions.push_back({lv_tr("Unload"), "MMU_UNLOAD", "hh::unload", "",
                           /*needs_hot_nozzle=*/true});
    }

    // Force-clear the MMU pause lock (last resort). Lock state only, no motion.
    actions.push_back({lv_tr("Unlock"), "MMU_UNLOCK", "hh::unlock", "danger"});
    return actions;
}

std::optional<helix::ErrorEvent>
AmsBackendHappyHare::classify_error(const std::string& raw_line,
                                    const helix::ClassifyContext& ctx) const {
    // Only `!!` emergency lines are candidates (matches AFC).
    if (!helix::is_bang_line(raw_line)) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Happy Hare reports the descriptive cause in reason_for_pause_; prefer it
    // over the terse !! line for the modal detail.
    std::string bare = helix::strip_bang_prefix(raw_line);
    std::string detail = !reason_for_pause_.empty() ? reason_for_pause_ : bare;

    // A recognized MMU fault: a descriptive reason is present, OR the print is
    // paused while HH is in its ERROR action. Mirrors AFC's error_state_ gate.
    const bool hh_error_state = (system_info_.action == AmsAction::ERROR);
    const bool recognized =
        helix::contains_ci(detail, "runout") || helix::contains_ci(detail, "clog") ||
        helix::contains_ci(detail, "encoder") || helix::contains_ci(detail, "jam") ||
        helix::contains_ci(detail, "manual intervention");

    if (ctx.is_paused && (hh_error_state || (recognized && !reason_for_pause_.empty()))) {
        return helix::make_ams_fault_event(helix::ErrorSource::HAPPY_HARE,
                                           helix::contains_ci(detail, "runout")
                                               ? lv_tr("Filament runout")
                                               : lv_tr("Filament System Error"),
                                           detail, build_recovery_actions());
    }

    // Not an HH-owned fault — let the generic classifier handle it.
    return std::nullopt;
}

std::vector<AmsBackend::ToolchangePhase>
AmsBackendHappyHare::toolchange_phase_template(StepOperationType op) const {
    switch (op) {
    case StepOperationType::LOAD_SWAP:
        return {
            {"heat", "Heat nozzle", false},  {"form_tip", "Form tip", true},
            {"cut", "Cut tip", true},        {"unload", "Unload", false},
            {"select", "Select gate", true}, {"feed", "Load filament", false},
            {"purge", "Purge", true},        {"load", "Load complete", false},
        };
    case StepOperationType::LOAD_FRESH:
        return {
            {"heat", "Heat nozzle", false},   {"select", "Select gate", true},
            {"feed", "Load filament", false}, {"purge", "Purge", true},
            {"load", "Load complete", false},
        };
    case StepOperationType::UNLOAD:
        return {
            {"heat", "Heat nozzle", false},
            {"form_tip", "Form tip", true},
            {"cut", "Cut tip", true},
            {"unload", "Unload", false},
        };
    }
    return {};
}

void AmsBackendHappyHare::sync_narration_step() {
    // Caller holds mutex_. Map the current action to a phase id.
    const char* phase_id = nullptr;
    switch (system_info_.action) {
    case AmsAction::HEATING:
        phase_id = "heat";
        break;
    case AmsAction::FORMING_TIP:
        phase_id = "form_tip";
        break;
    case AmsAction::CUTTING:
        phase_id = "cut";
        break;
    case AmsAction::UNLOADING:
        phase_id = "unload";
        break;
    case AmsAction::SELECTING:
        phase_id = "select";
        break;
    case AmsAction::LOADING:
        phase_id = "feed";
        break;
    case AmsAction::PURGING:
        phase_id = "purge";
        break;
    default:
        break; // IDLE / CHECKING / ERROR / etc. → no step movement
    }
    if (!phase_id)
        return;

    const auto op = AmsState::instance().get_active_step_operation();
    const auto tmpl = toolchange_phase_template(op);
    for (size_t k = 0; k < tmpl.size(); ++k) {
        if (tmpl[k].id == phase_id) {
            auto tok = lifetime_.token();
            const int index = static_cast<int>(k);
            std::string label = tmpl[k].label;
            tok.defer("AmsBackendHappyHare::sync_narration_step",
                      [index, label = std::move(label)]() {
                          AmsState::instance().set_narration_phase(index, label);
                      });
            return;
        }
    }
}

void AmsBackendHappyHare::initialize_slots(int gate_count) {
    spdlog::info("[AMS HappyHare] Initializing {} slots across {} units", gate_count, num_units_);

    system_info_.units.clear();

    // Determine per-unit gate counts:
    // 1. Use per_unit_gate_counts_ if available (v4 dissimilar multi-MMU)
    // 2. Fall back to even split (v3 or identical units)
    std::vector<int> unit_counts;
    if (!per_unit_gate_counts_.empty() &&
        static_cast<int>(per_unit_gate_counts_.size()) == num_units_) {
        // Verify total matches
        int total = 0;
        for (int c : per_unit_gate_counts_)
            total += c;
        if (total == gate_count) {
            unit_counts = per_unit_gate_counts_;
            spdlog::info("[AMS HappyHare] Using dissimilar per-unit gate counts");
        } else {
            spdlog::warn(
                "[AMS HappyHare] Per-unit gate counts sum ({}) != gate_count ({}), falling back",
                total, gate_count);
        }
    }

    // Fallback: even split
    if (unit_counts.empty()) {
        int gates_per_unit = (num_units_ > 1) ? (gate_count / num_units_) : gate_count;
        int remaining = gate_count;
        for (int u = 0; u < num_units_; ++u) {
            int count = (u == num_units_ - 1) ? remaining : gates_per_unit;
            unit_counts.push_back(count);
            remaining -= count;
        }
    }

    int global_offset = 0;
    for (int u = 0; u < num_units_; ++u) {
        int unit_gates = unit_counts[u];

        AmsUnit unit;
        unit.unit_index = u;
        if (num_units_ > 1) {
            unit.name = fmt::format("MMU Unit {}", u + 1);
        } else {
            unit.name = "Happy Hare MMU";
        }
        unit.slot_count = unit_gates;
        unit.first_slot_global_index = global_offset;
        unit.connected = true;
        unit.has_encoder = !is_type_b();
        unit.has_toolhead_sensor = true;
        unit.topology = is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
        // has_slot_sensors starts false; updated when sensor data arrives in parse_mmu_state()
        unit.has_slot_sensors = false;
        unit.has_hub_sensor = true; // HH selector functions as hub equivalent

        for (int i = 0; i < unit_gates; ++i) {
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = global_offset + i;
            slot.status = SlotStatus::UNKNOWN;
            slot.mapped_tool = global_offset + i;
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            unit.slots.push_back(slot);
        }

        system_info_.units.push_back(unit);
        global_offset += unit_gates;
    }

    system_info_.total_slots = gate_count;

    // Initialize tool-to-gate mapping (1:1 default)
    system_info_.tool_to_slot_map.clear();
    system_info_.tool_to_slot_map.reserve(gate_count);
    for (int i = 0; i < gate_count; ++i) {
        system_info_.tool_to_slot_map.push_back(i);
    }

    // Initialize SlotRegistry alongside legacy state (uses same unit_counts)
    {
        std::vector<std::pair<std::string, std::vector<std::string>>> sr_units;
        int sr_offset = 0;
        for (int u = 0; u < num_units_; ++u) {
            int count = unit_counts[u];
            std::vector<std::string> names;
            for (int g = 0; g < count; ++g) {
                names.push_back(std::to_string(sr_offset + g));
            }
            std::string unit_name = "Unit " + std::to_string(u + 1);
            if (num_units_ == 1) {
                unit_name = "MMU";
            }
            sr_units.push_back({unit_name, names});
            sr_offset += count;
        }
        slots_.initialize_units(sr_units);
    }
}

void AmsBackendHappyHare::query_tip_method_from_config() {
    if (!client_) {
        return;
    }

    // Query configfile.settings.mmu to read form_tip_macro.
    // Happy Hare uses the same logic internally: if the macro name contains "cut",
    // it's a cutter system (e.g., _MMU_CUT_TIP). Otherwise it's tip-forming.
    nlohmann::json params = {{"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // L081 Mechanism C: defer member access (system_info_, emit_event)
            // to main thread.
            token.defer("AmsBackendHappyHare::tip_method_apply", [this, response =
                                                                            std::move(response)]() {
                try {
                    // Guard every level before indexing. `response` is const in
                    // this non-mutable lambda, so operator[] resolves to the
                    // const overload — on a missing key that is a live
                    // assert(), an uncatchable SIGABRT, NOT the json exception
                    // the catch below is written for.
                    if (!response.contains("result") || !response["result"].contains("status") ||
                        !response["result"]["status"].contains("configfile") ||
                        !response["result"]["status"]["configfile"].contains("settings") ||
                        !response["result"]["status"]["configfile"]["settings"].is_object()) {
                        spdlog::warn("[AMS HappyHare] configfile settings unavailable for tip "
                                     "method query");
                        return;
                    }

                    const auto& settings = response["result"]["status"]["configfile"]["settings"];

                    if (!settings.contains("mmu") || !settings["mmu"].is_object()) {
                        spdlog::debug("[AMS HappyHare] No mmu section in configfile settings");
                        return;
                    }

                    const auto& mmu_cfg = settings["mmu"];
                    TipMethod method = TipMethod::NONE;

                    if (mmu_cfg.contains("form_tip_macro") &&
                        mmu_cfg["form_tip_macro"].is_string()) {
                        std::string macro = mmu_cfg["form_tip_macro"].get<std::string>();

                        // Convert to lowercase for comparison (same as Happy Hare)
                        std::string lower_macro = macro;
                        for (auto& c : lower_macro) {
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }

                        if (lower_macro.find("cut") != std::string::npos) {
                            method = TipMethod::CUT;
                        } else {
                            method = TipMethod::TIP_FORM;
                        }

                        spdlog::info(
                            "[AMS HappyHare] Tip method from config: {} (form_tip_macro={})",
                            tip_method_to_string(method), macro);
                    } else {
                        // No form_tip_macro configured — default to tip-forming
                        // (Happy Hare default macro is _MMU_FORM_TIP, not a cutter)
                        method = TipMethod::TIP_FORM;
                        spdlog::info("[AMS HappyHare] No form_tip_macro in config, defaulting "
                                     "to TIP_FORM");
                    }

                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        system_info_.tip_method = method;
                    }

                    emit_event(EVENT_STATE_CHANGED);
                } catch (const nlohmann::json::exception& e) {
                    spdlog::warn("[AMS HappyHare] Failed to parse configfile for tip method: {}",
                                 e.what());
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS HappyHare] Failed to query configfile for tip method: {}",
                         err.message);
        });
}

void AmsBackendHappyHare::query_selector_type_from_config() {
    if (!client_) {
        return;
    }

    // Query configfile.settings.mmu_machine to read selector_type.
    // VirtualSelector = Type B (hub topology: 3MS, Box Turtle, Night Owl, Angry Beaver)
    // LinearSelector/RotarySelector/ServoSelector = Type A (linear: ERCF, Tradrack)
    nlohmann::json params = {{"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // L081 Mechanism C: defer member access (selector_type_,
            // update_unit_topologies, emit_event) to main thread.
            token.defer("AmsBackendHappyHare::selector_type_apply", [this, response = std::move(
                                                                               response)]() {
                try {
                    // See query_tip_method_from_config: const operator[] on a
                    // missing key asserts rather than throws, so the chain must
                    // be guarded level by level.
                    if (!response.contains("result") || !response["result"].contains("status") ||
                        !response["result"]["status"].contains("configfile") ||
                        !response["result"]["status"]["configfile"].contains("settings") ||
                        !response["result"]["status"]["configfile"]["settings"].is_object()) {
                        spdlog::warn("[AMS HappyHare] configfile settings unavailable for selector "
                                     "type query");
                        return;
                    }

                    const auto& settings = response["result"]["status"]["configfile"]["settings"];

                    if (!settings.contains("mmu_machine") || !settings["mmu_machine"].is_object()) {
                        spdlog::debug(
                            "[AMS HappyHare] No mmu_machine section in configfile settings");
                        return;
                    }

                    const auto& mmu_machine = settings["mmu_machine"];
                    if (mmu_machine.contains("selector_type") &&
                        mmu_machine["selector_type"].is_string()) {
                        std::string type = mmu_machine["selector_type"].get<std::string>();
                        spdlog::info("[AMS HappyHare] Selector type from config: {}", type);

                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            selector_type_ = type;
                            update_unit_topologies();
                        }

                        emit_event(EVENT_STATE_CHANGED);
                    }
                } catch (const nlohmann::json::exception& e) {
                    spdlog::warn("[AMS HappyHare] Failed to parse configfile for selector type: {}",
                                 e.what());
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS HappyHare] Failed to query configfile for selector type: {}",
                         err.message);
        });
}

// ============================================================================
// Heater Config Query
// ============================================================================

bool AmsBackendHappyHare::apply_filament_heater_status(const nlohmann::json& params) {
    // Gather every configured heater object: the scalar primary (shared enclosure)
    // plus any per-gate heaters (EMU). The primary also drives the global dryer
    // model; all of them populate heater_temp_ for per-unit resolution.
    std::vector<std::string> heaters;
    std::string primary;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!filament_heater_name_.empty()) {
            primary = filament_heater_name_;
            heaters.push_back(filament_heater_name_);
        }
        for (const auto& h : filament_heaters_) {
            if (!h.empty())
                heaters.push_back(h);
        }
    }
    if (heaters.empty()) {
        return false;
    }

    bool any = false;
    for (const auto& hname : heaters) {
        // Happy Hare stores the full Klipper object name (e.g. "heater_generic
        // MMU_heater"), which is exactly the Moonraker status key. Use it verbatim so
        // any heater object type HH permits resolves — not only heater_generic.
        const std::string& status_key = hname;
        auto h_it = params.find(status_key);
        if (h_it == params.end() || !h_it->is_object()) {
            continue;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto t = h_it->find("temperature"); t != h_it->end() && t->is_number()) {
            const float temp = t->get<float>();
            heater_temp_[hname] = temp;
            if (hname == primary)
                dryer_info_.current_temp_c = temp;
            any = true;
        }
        if (auto tg = h_it->find("target"); tg != h_it->end() && tg->is_number()) {
            if (hname == primary)
                dryer_info_.target_temp_c = tg->get<float>();
        }
    }
    return any;
}

bool AmsBackendHappyHare::apply_environment_sensor_status(const nlohmann::json& params) {
    // Gather every configured env sensor: the scalar shared sensor plus any
    // per-gate sensors (EMU). Each updates sensor_humidity_ keyed by its object
    // name for per-unit resolution in get_system_info().
    std::vector<std::string> sensors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!environment_sensor_name_.empty())
            sensors.push_back(environment_sensor_name_);
        for (const auto& s : environment_sensors_) {
            if (!s.empty())
                sensors.push_back(s);
        }
    }
    if (sensors.empty()) {
        return false;
    }

    bool any = false;
    for (const auto& sname : sensors) {
        // Candidate object keys carrying temperature/humidity, mirroring Happy Hare's
        // _get_environment_status(): the sensor object itself (it may be a humidity
        // chip directly), plus "<chip> <name>" for each humidity-capable chip, where
        // <name> is the bare second token (e.g. "temperature_sensor box" -> "box").
        std::vector<std::string> candidates;
        candidates.push_back(sname);
        if (auto sp = sname.find(' '); sp != std::string::npos) {
            const std::string bare = sname.substr(sp + 1);
            if (!bare.empty()) {
                for (const auto& chip : helix::sensors::humidity_sensor_chips()) {
                    candidates.push_back(std::string(chip.config_id) + " " + bare);
                }
            }
        }
        bool have_temp = false;
        bool have_hum = false;
        float temp_val = 0.0f;
        float hum_val = 0.0f;
        for (const auto& key : candidates) {
            auto it = params.find(key);
            if (it == params.end() || !it->is_object()) {
                continue;
            }
            // Ambient temperature is what HH surfaces for the enclosure when no
            // heater is fitted; capture it so humidity-only enclosures still show a
            // temperature and so the readout never gates humidity on a heater.
            if (!have_temp) {
                if (auto t = it->find("temperature"); t != it->end() && t->is_number()) {
                    temp_val = t->get<float>();
                    have_temp = true;
                }
            }
            if (!have_hum) {
                if (auto h = it->find("humidity"); h != it->end() && h->is_number()) {
                    hum_val = h->get<float>();
                    have_hum = true;
                }
            }
            if (have_temp && have_hum) {
                break;
            }
        }
        if (have_temp || have_hum) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (have_temp) {
                sensor_temp_[sname] = temp_val;
            }
            if (have_hum) {
                sensor_humidity_[sname] = hum_val;
            }
            spdlog::trace("[AMS HappyHare] Env sensor {}: temp={:.1f} (have={}) humidity={:.1f} "
                          "(have={})",
                          sname, temp_val, have_temp, hum_val, have_hum);
            any = true;
        }
    }
    return any;
}

// Parse a Happy Hare config-list value (e.g. environment_sensors) into trimmed
// object names. Moonraker may return it as a JSON array or as a raw
// comma-separated string, so handle both.
static std::vector<std::string> parse_hh_config_list(const nlohmann::json& v) {
    auto trim = [](std::string s) {
        const auto b = s.find_first_not_of(" \t");
        const auto e = s.find_last_not_of(" \t");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    std::vector<std::string> out;
    if (v.is_array()) {
        for (const auto& e : v) {
            if (e.is_string()) {
                std::string s = trim(e.get<std::string>());
                if (!s.empty())
                    out.push_back(std::move(s));
            }
        }
    } else if (v.is_string()) {
        const std::string str = v.get<std::string>();
        size_t start = 0;
        while (start <= str.size()) {
            const size_t comma = str.find(',', start);
            std::string tok = trim(
                str.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!tok.empty())
                out.push_back(std::move(tok));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
    }
    return out;
}

void AmsBackendHappyHare::apply_heater_config(const nlohmann::json& settings) {
    // Parse [mmu_machine] filament_heater — the Klipper heater_generic object name.
    if (settings.contains("mmu_machine") && settings["mmu_machine"].is_object()) {
        const auto& mmu_machine = settings["mmu_machine"];
        if (mmu_machine.contains("filament_heater") && mmu_machine["filament_heater"].is_string()) {
            std::lock_guard<std::mutex> lock(mutex_);
            filament_heater_name_ = mmu_machine["filament_heater"].get<std::string>();
            spdlog::info("[AMS HappyHare] Filament heater: {}", filament_heater_name_);
        }
        // Parse [mmu_machine] environment_sensor — the Klipper object (e.g.
        // "temperature_sensor box") whose backing humidity chip reports box %RH.
        if (mmu_machine.contains("environment_sensor") &&
            mmu_machine["environment_sensor"].is_string()) {
            std::lock_guard<std::mutex> lock(mutex_);
            environment_sensor_name_ = mmu_machine["environment_sensor"].get<std::string>();
            spdlog::info("[AMS HappyHare] Environment sensor: {}", environment_sensor_name_);
        }
        // Per-gate (EMU) form: filament_heaters / environment_sensors are lists with
        // one entry per gate, mutually exclusive with the scalar form. For multi-MMU
        // these span all units' gates; get_system_info() maps each unit to its own.
        if (mmu_machine.contains("filament_heaters")) {
            auto heaters = parse_hh_config_list(mmu_machine["filament_heaters"]);
            if (!heaters.empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                filament_heaters_ = std::move(heaters);
                dryer_info_.supported = true; // per-gate heaters imply a dryer exists
                spdlog::info("[AMS HappyHare] Per-gate filament heaters: {}",
                             filament_heaters_.size());
            }
        }
        if (mmu_machine.contains("environment_sensors")) {
            auto sensors = parse_hh_config_list(mmu_machine["environment_sensors"]);
            if (!sensors.empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                environment_sensors_ = std::move(sensors);
                spdlog::info("[AMS HappyHare] Per-gate environment sensors: {}",
                             environment_sensors_.size());
            }
        }
    }

    // Parse [mmu] heater_max_temp — can be a number or a numeric string.
    if (settings.contains("mmu") && settings["mmu"].is_object()) {
        const auto& mmu = settings["mmu"];
        if (mmu.contains("heater_max_temp")) {
            float max_temp = 0.0f;
            bool parsed = false;
            if (mmu["heater_max_temp"].is_number()) {
                max_temp = mmu["heater_max_temp"].get<float>();
                parsed = true;
            } else if (mmu["heater_max_temp"].is_string()) {
                try {
                    max_temp = std::stof(mmu["heater_max_temp"].get<std::string>());
                    parsed = true;
                } catch (...) {
                    spdlog::warn("[AMS HappyHare] Could not parse heater_max_temp string");
                }
            }
            if (parsed) {
                std::lock_guard<std::mutex> lock(mutex_);
                dryer_info_.max_temp_c = max_temp;
                dryer_info_.supported = true;
                spdlog::info("[AMS HappyHare] Heater max temp: {:.1f}°C", max_temp);
            }
        }
    }
}

void AmsBackendHappyHare::query_heater_config_from_config() {
    if (!client_) {
        return;
    }

    // Query configfile.settings for [mmu_machine] filament_heater and [mmu] heater_max_temp.
    // filament_heater names the heater_generic object driven by MMU_HEATER.
    // heater_max_temp is the hardware safety ceiling for the dryer UI.
    nlohmann::json params = {{"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // L081 Mechanism C: defer member access (filament_heater_name_, dryer_info_)
            // to main thread.
            token.defer("AmsBackendHappyHare::heater_config_apply", [this, response = std::move(
                                                                               response)]() {
                try {
                    // See query_tip_method_from_config: const operator[] on a
                    // missing key asserts rather than throws, so the chain must
                    // be guarded level by level.
                    if (!response.contains("result") || !response["result"].contains("status") ||
                        !response["result"]["status"].contains("configfile") ||
                        !response["result"]["status"]["configfile"].contains("settings") ||
                        !response["result"]["status"]["configfile"]["settings"].is_object()) {
                        spdlog::warn(
                            "[AMS HappyHare] configfile settings unavailable for heater query");
                        return;
                    }

                    const auto& settings = response["result"]["status"]["configfile"]["settings"];
                    apply_heater_config(settings);
                    emit_event(EVENT_STATE_CHANGED);
                } catch (const nlohmann::json::exception& e) {
                    spdlog::warn("[AMS HappyHare] Failed to parse configfile for heater: {}",
                                 e.what());
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS HappyHare] Failed to query configfile for heater: {}", err.message);
        });
}

// ============================================================================
// Config Defaults Query
// ============================================================================

void AmsBackendHappyHare::query_config_defaults() {
    if (!client_) {
        return;
    }

    // Query configfile.settings.mmu for initial values of speeds and distances.
    // These serve as defaults until the user overrides them via the UI.
    nlohmann::json params = {{"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // L081 Mechanism C: defer member access (config_defaults_,
            // load_persisted_overrides, reapply_overrides) to main thread.
            token.defer("AmsBackendHappyHare::config_defaults_apply", [this, response = std::move(
                                                                                 response)]() {
                try {
                    // See query_tip_method_from_config: const operator[] on a
                    // missing key asserts rather than throws, so the chain must
                    // be guarded level by level.
                    if (!response.contains("result") || !response["result"].contains("status") ||
                        !response["result"]["status"].contains("configfile") ||
                        !response["result"]["status"]["configfile"].contains("settings") ||
                        !response["result"]["status"]["configfile"]["settings"].is_object()) {
                        spdlog::warn(
                            "[AMS HappyHare] configfile settings unavailable for config defaults");
                        return;
                    }

                    const auto& settings = response["result"]["status"]["configfile"]["settings"];

                    if (!settings.contains("mmu") || !settings["mmu"].is_object()) {
                        spdlog::debug("[AMS HappyHare] No mmu section in configfile for defaults");
                        return;
                    }

                    const auto& mmu = settings["mmu"];

                    // Helper to parse a float from config (values are strings in configfile)
                    auto parse_float = [&](const char* key, float& out) {
                        if (mmu.contains(key) && mmu[key].is_string()) {
                            try {
                                out = std::stof(mmu[key].get<std::string>());
                            } catch (...) {
                                // Keep default
                            }
                        }
                    };

                    // Helper to parse an int from config
                    auto parse_int = [&](const char* key, int& out) {
                        if (mmu.contains(key) && mmu[key].is_string()) {
                            try {
                                out = std::stoi(mmu[key].get<std::string>());
                            } catch (...) {
                                // Keep default
                            }
                        }
                    };

                    {
                        std::lock_guard<std::mutex> lock(mutex_);

                        parse_float("gear_from_buffer_speed",
                                    config_defaults_.gear_from_buffer_speed);
                        parse_float("gear_from_spool_speed",
                                    config_defaults_.gear_from_spool_speed);
                        parse_float("gear_unload_speed", config_defaults_.gear_unload_speed);
                        parse_float("selector_move_speed", config_defaults_.selector_move_speed);
                        parse_float("extruder_load_speed", config_defaults_.extruder_load_speed);
                        parse_float("extruder_unload_speed",
                                    config_defaults_.extruder_unload_speed);
                        parse_float("toolhead_sensor_to_nozzle",
                                    config_defaults_.toolhead_sensor_to_nozzle);
                        parse_float("toolhead_extruder_to_nozzle",
                                    config_defaults_.toolhead_extruder_to_nozzle);
                        parse_float("toolhead_entry_to_extruder",
                                    config_defaults_.toolhead_entry_to_extruder);
                        parse_float("toolhead_ooze_reduction",
                                    config_defaults_.toolhead_ooze_reduction);
                        parse_int("sync_to_extruder", config_defaults_.sync_to_extruder);
                        parse_int("clog_detection", config_defaults_.clog_detection);

                        config_defaults_.loaded = true;

                        spdlog::info("[AMS HappyHare] Config defaults loaded: "
                                     "gear_buf={}, gear_spool={}, gear_unload={}, "
                                     "ext_load={}, ext_unload={}, "
                                     "sensor_to_nozzle={}, extruder_to_nozzle={}",
                                     config_defaults_.gear_from_buffer_speed,
                                     config_defaults_.gear_from_spool_speed,
                                     config_defaults_.gear_unload_speed,
                                     config_defaults_.extruder_load_speed,
                                     config_defaults_.extruder_unload_speed,
                                     config_defaults_.toolhead_sensor_to_nozzle,
                                     config_defaults_.toolhead_extruder_to_nozzle);
                    }

                    load_persisted_overrides();
                    reapply_overrides();
                } catch (const nlohmann::json::exception& e) {
                    spdlog::warn("[AMS HappyHare] Failed to parse configfile for defaults: {}",
                                 e.what());
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS HappyHare] Failed to query configfile for defaults: {}",
                         err.message);
        });
}

void AmsBackendHappyHare::load_persisted_overrides() {
    auto* config = helix::Config::get_instance();
    if (!config)
        return;

    // Helper: load a float override if config_default matches current default
    auto load_float = [&](const std::string& key, std::optional<float>& field,
                          float current_default) {
        std::string base = "/hh_overrides/" + key;
        if (!config->exists(base + "/value"))
            return;
        try {
            float saved_default = config->get<float>(base + "/config_default", -999.0f);
            if (std::abs(saved_default - current_default) < 0.01f) {
                field = config->get<float>(base + "/value");
                spdlog::debug("[AMS HappyHare] Loaded override {}: {}", key, *field);
            } else {
                spdlog::info("[AMS HappyHare] Dropping stale override {} "
                             "(config changed: {} -> {})",
                             key, saved_default, current_default);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[AMS HappyHare] Failed to load override {}: {}", key, e.what());
        }
    };

    // Helper: load an int override
    auto load_int = [&](const std::string& key, std::optional<int>& field, int current_default) {
        std::string base = "/hh_overrides/" + key;
        if (!config->exists(base + "/value"))
            return;
        try {
            int saved_default = config->get<int>(base + "/config_default", -999);
            if (saved_default == current_default) {
                field = config->get<int>(base + "/value");
                spdlog::debug("[AMS HappyHare] Loaded override {}: {}", key, *field);
            } else {
                spdlog::info("[AMS HappyHare] Dropping stale override {} "
                             "(config changed: {} -> {})",
                             key, saved_default, current_default);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[AMS HappyHare] Failed to load override {}: {}", key, e.what());
        }
    };

    load_float("gear_from_buffer_speed", user_overrides_.gear_from_buffer_speed,
               config_defaults_.gear_from_buffer_speed);
    load_float("gear_from_spool_speed", user_overrides_.gear_from_spool_speed,
               config_defaults_.gear_from_spool_speed);
    load_float("gear_unload_speed", user_overrides_.gear_unload_speed,
               config_defaults_.gear_unload_speed);
    load_float("selector_move_speed", user_overrides_.selector_move_speed,
               config_defaults_.selector_move_speed);
    load_float("extruder_load_speed", user_overrides_.extruder_load_speed,
               config_defaults_.extruder_load_speed);
    load_float("extruder_unload_speed", user_overrides_.extruder_unload_speed,
               config_defaults_.extruder_unload_speed);
    load_float("toolhead_sensor_to_nozzle", user_overrides_.toolhead_sensor_to_nozzle,
               config_defaults_.toolhead_sensor_to_nozzle);
    load_float("toolhead_extruder_to_nozzle", user_overrides_.toolhead_extruder_to_nozzle,
               config_defaults_.toolhead_extruder_to_nozzle);
    load_float("toolhead_entry_to_extruder", user_overrides_.toolhead_entry_to_extruder,
               config_defaults_.toolhead_entry_to_extruder);
    load_float("toolhead_ooze_reduction", user_overrides_.toolhead_ooze_reduction,
               config_defaults_.toolhead_ooze_reduction);
    load_int("sync_to_extruder", user_overrides_.sync_to_extruder,
             config_defaults_.sync_to_extruder);
    load_int("clog_detection", user_overrides_.clog_detection, config_defaults_.clog_detection);
}

/// Helper to get the config default float for a given action key
float AmsBackendHappyHare::get_config_default_float(const std::string& key) const {
    if (key == "gear_from_buffer_speed")
        return config_defaults_.gear_from_buffer_speed;
    if (key == "gear_from_spool_speed")
        return config_defaults_.gear_from_spool_speed;
    if (key == "gear_unload_speed")
        return config_defaults_.gear_unload_speed;
    if (key == "selector_move_speed" || key == "selector_speed")
        return config_defaults_.selector_move_speed;
    if (key == "extruder_load_speed")
        return config_defaults_.extruder_load_speed;
    if (key == "extruder_unload_speed")
        return config_defaults_.extruder_unload_speed;
    if (key == "toolhead_sensor_to_nozzle")
        return config_defaults_.toolhead_sensor_to_nozzle;
    if (key == "toolhead_extruder_to_nozzle")
        return config_defaults_.toolhead_extruder_to_nozzle;
    if (key == "toolhead_entry_to_extruder")
        return config_defaults_.toolhead_entry_to_extruder;
    if (key == "toolhead_ooze_reduction")
        return config_defaults_.toolhead_ooze_reduction;
    return 0.0f;
}

/// Helper to get the config default int for a given action key
int AmsBackendHappyHare::get_config_default_int(const std::string& key) const {
    if (key == "sync_to_extruder")
        return config_defaults_.sync_to_extruder;
    if (key == "clog_detection")
        return config_defaults_.clog_detection;
    return 0;
}

void AmsBackendHappyHare::save_override(const std::string& key, float value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Update in-memory override
        if (key == "gear_from_buffer_speed")
            user_overrides_.gear_from_buffer_speed = value;
        else if (key == "gear_from_spool_speed")
            user_overrides_.gear_from_spool_speed = value;
        else if (key == "gear_unload_speed")
            user_overrides_.gear_unload_speed = value;
        else if (key == "selector_move_speed" || key == "selector_speed")
            user_overrides_.selector_move_speed = value;
        else if (key == "extruder_load_speed")
            user_overrides_.extruder_load_speed = value;
        else if (key == "extruder_unload_speed")
            user_overrides_.extruder_unload_speed = value;
        else if (key == "toolhead_sensor_to_nozzle")
            user_overrides_.toolhead_sensor_to_nozzle = value;
        else if (key == "toolhead_extruder_to_nozzle")
            user_overrides_.toolhead_extruder_to_nozzle = value;
        else if (key == "toolhead_entry_to_extruder")
            user_overrides_.toolhead_entry_to_extruder = value;
        else if (key == "toolhead_ooze_reduction")
            user_overrides_.toolhead_ooze_reduction = value;
    } // end mutex scope

    // Persist to Config JSON (outside lock — disk I/O)
    auto* config = helix::Config::get_instance();
    if (!config)
        return;
    std::string base = "/hh_overrides/" + key;
    config->set<float>(base + "/value", value);
    config->set<float>(base + "/config_default", get_config_default_float(key));
    config->save();
}

void AmsBackendHappyHare::save_override(const std::string& key, int value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (key == "sync_to_extruder")
            user_overrides_.sync_to_extruder = value;
        else if (key == "clog_detection")
            user_overrides_.clog_detection = value;
    } // end mutex scope

    // Persist to Config JSON (outside lock — disk I/O)
    auto* config = helix::Config::get_instance();
    if (!config)
        return;
    std::string base = "/hh_overrides/" + key;
    config->set<int>(base + "/value", value);
    config->set<int>(base + "/config_default", get_config_default_int(key));
    config->save();
}

void AmsBackendHappyHare::reapply_overrides() {
    // Build a single MMU_TEST_CONFIG command with all active overrides
    std::string cmd = "MMU_TEST_CONFIG";
    bool has_params = false;

    // Helper to append a float parameter
    auto append_float = [&](const char* param, const std::optional<float>& val, bool integer_fmt) {
        if (val.has_value()) {
            if (integer_fmt)
                cmd += fmt::format(" {}={:.0f}", param, *val);
            else
                cmd += fmt::format(" {}={:.1f}", param, *val);
            has_params = true;
        }
    };

    // Helper to append an int parameter
    auto append_int = [&](const char* param, const std::optional<int>& val) {
        if (val.has_value()) {
            cmd += fmt::format(" {}={}", param, *val);
            has_params = true;
        }
    };

    // Speed sliders (integer format)
    append_float("GEAR_FROM_BUFFER_SPEED", user_overrides_.gear_from_buffer_speed, true);
    append_float("GEAR_FROM_SPOOL_SPEED", user_overrides_.gear_from_spool_speed, true);
    append_float("GEAR_UNLOAD_SPEED", user_overrides_.gear_unload_speed, true);
    append_float("SELECTOR_MOVE_SPEED", user_overrides_.selector_move_speed, true);
    append_float("EXTRUDER_LOAD_SPEED", user_overrides_.extruder_load_speed, true);
    append_float("EXTRUDER_UNLOAD_SPEED", user_overrides_.extruder_unload_speed, true);

    // Toolhead distances (one decimal)
    append_float("TOOLHEAD_SENSOR_TO_NOZZLE", user_overrides_.toolhead_sensor_to_nozzle, false);
    append_float("TOOLHEAD_EXTRUDER_TO_NOZZLE", user_overrides_.toolhead_extruder_to_nozzle, false);
    append_float("TOOLHEAD_ENTRY_TO_EXTRUDER", user_overrides_.toolhead_entry_to_extruder, false);
    append_float("TOOLHEAD_OOZE_REDUCTION", user_overrides_.toolhead_ooze_reduction, false);

    // Int toggles
    append_int("SYNC_TO_EXTRUDER", user_overrides_.sync_to_extruder);
    append_int("CLOG_DETECTION", user_overrides_.clog_detection);

    if (has_params) {
        spdlog::info("[AMS HappyHare] Re-applying overrides: {}", cmd);
        execute_gcode(cmd);
    }
}

// ============================================================================
// Filament Operations
// ============================================================================

// check_preconditions() provided by AmsSubscriptionBackend

AmsError AmsBackendHappyHare::validate_slot_index(int gate_index) const {
    if (!slots_.is_valid_index(gate_index)) {
        return AmsErrorHelper::invalid_slot(gate_index,
                                            slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
    }
    return AmsErrorHelper::success();
}

// execute_gcode() provided by AmsSubscriptionBackend

AmsError AmsBackendHappyHare::do_load_filament(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError gate_valid = validate_slot_index(slot_index);
        if (!gate_valid) {
            return gate_valid;
        }

        // Check if slot has filament available
        const auto* entry = slots_.get(slot_index);
        if (entry && entry->info.status == SlotStatus::EMPTY) {
            return AmsErrorHelper::slot_not_available(slot_index);
        }
    }

    // Send MMU_LOAD GATE={n} command (Happy Hare uses "gate" in its API)
    std::ostringstream cmd;
    cmd << "MMU_LOAD GATE=" << slot_index;

    spdlog::info("[AMS HappyHare] Loading from slot {}", slot_index);
    return ensure_homed_then(cmd.str());
}

AmsError AmsBackendHappyHare::do_unload_filament(int /*slot_index*/) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!system_info_.filament_loaded) {
            return AmsError(AmsResult::WRONG_STATE, "No filament loaded", "No filament to unload",
                            "Load filament first");
        }
    }

    spdlog::info("[AMS HappyHare] Unloading filament");
    return ensure_homed_then("MMU_UNLOAD");
}

AmsError AmsBackendHappyHare::do_select_slot(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError gate_valid = validate_slot_index(slot_index);
        if (!gate_valid) {
            return gate_valid;
        }
    }

    // Send MMU_SELECT GATE={n} command (Happy Hare uses "gate" in its API)
    std::ostringstream cmd;
    cmd << "MMU_SELECT GATE=" << slot_index;

    spdlog::info("[AMS HappyHare] Selecting slot {}", slot_index);
    return execute_gcode(cmd.str());
}

AmsError AmsBackendHappyHare::do_change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tool_number < 0 ||
            tool_number >= static_cast<int>(system_info_.tool_to_slot_map.size())) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "Select a valid tool");
        }
    }

    // Send T{n} command for standard tool change
    std::ostringstream cmd;
    cmd << "T" << tool_number;

    spdlog::info("[AMS HappyHare] Tool change to T{}", tool_number);
    return ensure_homed_then(cmd.str());
}

// ============================================================================
// Recovery Operations
// ============================================================================

AmsError AmsBackendHappyHare::recover() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }
    }

    spdlog::info("[AMS HappyHare] Initiating recovery");
    return execute_gcode("MMU_RECOVER");
}

AmsError AmsBackendHappyHare::reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }
    }

    // Happy Hare uses MMU_HOME to reset to a known state
    spdlog::info("[AMS HappyHare] Resetting (homing selector)");
    return execute_gcode("MMU_HOME");
}

// MMU_RECOVER re-syncs Happy Hare's idea of gate state. It moves no filament, so
// it is a fault clear rather than a position recovery.
AmsError AmsBackendHappyHare::clear_fault(int slot_index) {
    // -1 means "no particular gate". MMU_RECOVER without GATE re-syncs the whole
    // selector, the natural system-scoped analogue of AFC's RESET_FAILURE, and
    // the base contract documents -1 as valid. Both UI callers pass current_slot,
    // which is -1 whenever nothing is loaded — the state Reset is pressed in.
    const bool all_gates = (slot_index < 0);

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        if (!all_gates) {
            AmsError slot_err = validate_slot_index(slot_index);
            if (!slot_err) {
                return slot_err;
            }
        }
    }

    if (all_gates) {
        spdlog::info("[AMS HappyHare] Recovering all gates");
        return execute_gcode("MMU_RECOVER");
    }

    // MMU_RECOVER with GATE parameter recovers a specific gate's state
    spdlog::info("[AMS HappyHare] Recovering gate {}", slot_index);
    return execute_gcode("MMU_RECOVER GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::eject_lane(int slot_index) {
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
    }

    // MMU_EJECT fully ejects filament from the gate so the spool can be removed.
    // If filament is loaded it acts like MMU_UNLOAD first, then ejects from gate.
    spdlog::info("[AMS HappyHare] Ejecting gate {}", slot_index);
    return execute_gcode("MMU_EJECT GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::select_gate(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        AmsError slot_err = validate_slot_index(slot_index);
        if (!slot_err) {
            return slot_err;
        }
    }

    spdlog::info("[AMS HappyHare] Selecting gate {} (no load)", slot_index);
    return execute_gcode("MMU_SELECT GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::move_selector(int delta) {
    int target = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        const int count = slots_.slot_count();
        if (count <= 0) {
            return AmsErrorHelper::not_supported("Selector jog");
        }

        // Read the underlying member directly: get_current_slot() locks mutex_.
        int base = system_info_.current_slot;
        if (base < 0) {
            base = 0; // No current / bypass (-1, -2) -> treat as gate 0.
        }
        target = std::clamp(base + delta, 0, count - 1);
    }

    spdlog::info("[AMS HappyHare] Jog selector by {} -> gate {}", delta, target);
    return execute_gcode("MMU_SELECT GATE=" + std::to_string(target));
}

AmsError AmsBackendHappyHare::check_gate(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        AmsError slot_err = validate_slot_index(slot_index);
        if (!slot_err) {
            return slot_err;
        }
    }

    spdlog::info("[AMS HappyHare] Checking gate {}", slot_index);
    return execute_gcode("MMU_CHECK_GATE GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::check_all_gates() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }
    }

    spdlog::info("[AMS HappyHare] Checking all gates");
    return execute_gcode("MMU_CHECK_GATE");
}

AmsError AmsBackendHappyHare::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        if (system_info_.action == AmsAction::IDLE) {
            return AmsErrorHelper::success(); // Nothing to cancel
        }
    }

    // MMU_PAUSE can be used to stop current operation
    spdlog::info("[AMS HappyHare] Cancelling current operation");
    return execute_gcode("MMU_PAUSE");
}

// ============================================================================
// Configuration Operations
// ============================================================================

void AmsBackendHappyHare::apply_overrides(SlotInfo& slot, int slot_index) {
    // Callers hold mutex_. The whole spec §5 policy + the re-bind/eject rules
    // live in helix::ams::merge_override — the single implementation every
    // backend shares.
    auto it = overrides_.find(slot_index);
    if (it == overrides_.end())
        return;
    helix::ams::MergeOptions opts;
    opts.printer_reports_spool_ids = printer_reports_spool_ids();
    opts.keep_spool_info_on_eject = SettingsManager::instance().get_ams_keep_spool_info_on_eject();
    // Own-write echo suppression (SlotFingerprintTracker::expect()
    // semantics): if we just re-linked this gate's spool id, in-flight
    // frames keep reporting the old firmware id for a poll or two — Rule 1
    // must not read that stale frame as an external re-bind.
    const auto [own_old_id, own_new_id] = own_write_expectation(slot_index, slot.spoolman_id);
    opts.suppress_rebind_firmware_old_id = own_old_id;
    opts.suppress_rebind_firmware_new_id = own_new_id;
    const auto result = helix::ams::merge_override(slot, it->second, opts);
    if (result.cleared_rebind || result.cleared_eject) {
        overrides_.erase(it);
        if (override_store_) {
            override_store_->clear_async(slot_index, [slot_index](bool ok, const std::string& err) {
                if (!ok)
                    spdlog::warn("[AMS HH] override clear persist failed for slot {}: {}",
                                 slot_index, err);
            });
        }
    }
}

void AmsBackendHappyHare::persist_override(int slot_index, const SlotInfo& info) {
    // Callers hold mutex_.
    helix::ams::FilamentSlotOverride o;
    o.brand = info.brand;
    o.spool_name = info.spool_name;
    o.spoolman_id = info.spoolman_id;
    o.spoolman_vendor_id = info.spoolman_vendor_id;
    o.remaining_weight_g = info.remaining_weight_g;
    o.total_weight_g = info.total_weight_g;
    o.color_name = info.color_name;
    o.material = info.material;
    // Catalog product identity — see apply_overrides(). Never auto-mirrored;
    // a non-empty value is always a user pick.
    o.catalog_id = info.catalog_id;
    o.product_name = info.product_name;
    if (info.color_rgb != 0 && info.color_rgb != AMS_DEFAULT_SLOT_COLOR) {
        o.color_rgb = info.color_rgb;
        o.color_set = true;
    }
    overrides_[slot_index] = o;

    if (override_store_) {
        override_store_->save_async(slot_index, o, [slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("[AMS HappyHare] override save failed for gate {}: {}", slot_index,
                             err);
            }
        });
    }
}

void AmsBackendHappyHare::clear_slot_override(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_.erase(slot_index);

        // Reset the override-exclusive fields on the live slot too: Happy Hare's
        // gate map has no concept of brand / spool_name / total weight / colour
        // name, so no firmware update will ever clear them.
        if (helix::printer::SlotEntry* entry = slots_.get_mut(slot_index)) {
            entry->info.brand.clear();
            entry->info.spool_name.clear();
            entry->info.spoolman_id = 0;
            entry->info.spoolman_vendor_id = 0;
            entry->info.spoolman_filament_id = 0;
            entry->info.remaining_weight_g = -1.0f;
            entry->info.total_weight_g = -1.0f;
            entry->info.color_name.clear();
            // The catalog pick is override-exclusive on every backend — no AMS
            // firmware carries a branded product id — so a clear always drops it.
            // Leaving it would re-navigate the editor to the removed spool's
            // product on the next open.
            entry->info.catalog_id.clear();
            entry->info.product_name.clear();
        }
    }
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
}

void AmsBackendHappyHare::publish_external_spool_lane(const SlotInfo* spool) {
    // Same shape as AFC's publish: capability + gate count under the lock,
    // lazy shared-namespace store from api_, send outside. Lane key style —
    // HelixScreen's filament-system convention (spec filament_slots.md §4);
    // Happy Hare's own outer keys differ but the inner 0-based `lane` field is
    // what readers key off.
    int lane_index = 0;
    bool supported = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        supported = system_info_.supports_bypass;
        lane_index = system_info_.total_slots;
    }
    if (!supported || lane_index <= 0 || !api_) {
        return;
    }
    if (!lane_publish_store_) {
        lane_publish_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            api_, "happyhare", helix::ams::LaneKeyStyle::Lane);
    }
    helix::ams::publish_external_lane(lane_publish_store_.get(), lane_index, spool,
                                      backend_log_tag());
}

AmsError AmsBackendHappyHare::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    int old_spoolman_id = 0;
    int old_mapped_tool = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(
                slot_index, slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
        }

        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(
                slot_index, slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
        }

        auto& slot = entry->info;

        // Capture old values BEFORE updating (needed to detect clears / remaps)
        old_spoolman_id = slot.spoolman_id;
        old_mapped_tool = slot.mapped_tool;

        // Detect whether anything actually changed
        bool changed = slot.color_name != info.color_name || slot.color_rgb != info.color_rgb ||
                       slot.material != info.material || slot.brand != info.brand ||
                       slot.catalog_id != info.catalog_id ||
                       slot.product_name != info.product_name ||
                       slot.spoolman_id != info.spoolman_id || slot.spool_name != info.spool_name ||
                       slot.remaining_weight_g != info.remaining_weight_g ||
                       slot.total_weight_g != info.total_weight_g ||
                       slot.nozzle_temp_min != info.nozzle_temp_min ||
                       slot.nozzle_temp_max != info.nozzle_temp_max ||
                       slot.bed_temp != info.bed_temp || slot.mapped_tool != info.mapped_tool;

        // Update local state
        slot.color_name = info.color_name;
        slot.color_rgb = info.color_rgb;
        slot.material = info.material;
        slot.brand = info.brand;
        // Carry the catalog product identity through preview writes too — a
        // persist=false preview that dropped it would make the editor snap
        // back to a different variant on the next get_slot_info().
        slot.catalog_id = info.catalog_id;
        slot.product_name = info.product_name;
        slot.spoolman_id = info.spoolman_id;
        slot.spool_name = info.spool_name;
        slot.remaining_weight_g = info.remaining_weight_g;
        slot.total_weight_g = info.total_weight_g;
        slot.nozzle_temp_min = info.nozzle_temp_min;
        slot.nozzle_temp_max = info.nozzle_temp_max;
        slot.bed_temp = info.bed_temp;
        // Tool mapping change goes through registry so reverse maps stay consistent.
        if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0) {
            slots_.set_tool_mapping(slot_index, info.mapped_tool);
        }

        if (changed) {
            spdlog::info("[AMS HappyHare] Updated slot {} info: {} {}", slot_index, info.material,
                         info.color_name);
        }

        // Record the user's identity in the override store: the gate map cannot
        // hold brand / spool_name / total weight / colour name at all.
        if (persist) {
            persist_override(slot_index, info);
        }
    }

    // Persist via MMU_GATE_MAP command (Happy Hare stores in mmu_vars.cfg automatically).
    // Skip persistence when persist=false — used by Spoolman weight polling to update
    // in-memory state without sending G-code back to firmware. Without this guard,
    // weight updates would trigger MMU_GATE_MAP → firmware status_update WebSocket
    // event → sync_from_backend → refresh_spoolman_weights → set_slot_info again,
    // creating an infinite feedback loop.
    // Set when the material could not be expressed as a G-code parameter. Reported
    // after every other write has gone out, so a name the gate map cannot store costs
    // the user only the material rather than the whole save — but is never silent.
    std::string rejected_material;

    if (persist) {
        bool has_changes = false;
        std::string cmd = fmt::format("MMU_GATE_MAP GATE={}", slot_index);

        // Color (hex format, no # prefix)
        if (info.color_rgb != 0 && info.color_rgb != AMS_DEFAULT_SLOT_COLOR) {
            cmd += fmt::format(" COLOR={:06X}", info.color_rgb & 0xFFFFFF);
            has_changes = true;
        }

        // Material (validate to prevent command injection). The material charset is
        // deliberately wider than an identifier's: `PLA+`, `PA6-CF` and `Silk PLA` are
        // all in our own filament database, and gating this on is_safe_gcode_param()
        // dropped every one of them.
        if (!info.material.empty() && IMoonrakerAPI::is_safe_material_param(info.material)) {
            cmd += fmt::format(" MATERIAL={}", IMoonrakerAPI::gcode_param_value(info.material));
            has_changes = true;
        } else if (!info.material.empty()) {
            spdlog::warn("[AMS HappyHare] Skipping MATERIAL - unsafe characters in: {}",
                         info.material);
            rejected_material = info.material;
        }

        // Spoolman ID (-1 to clear)
        if (info.spoolman_id > 0) {
            cmd += fmt::format(" SPOOLID={}", info.spoolman_id);
            has_changes = true;
        } else if (info.spoolman_id == 0 && old_spoolman_id > 0) {
            cmd += " SPOOLID=-1"; // Clear existing link
            has_changes = true;
        }

        // Record our own id write so Rule 1 does not read the in-flight
        // frames (still reporting old_spoolman_id until the echo lands) as
        // an external re-bind. An unlink (SPOOLID=-1) erases the pending
        // expectation instead. The gcode block above runs OUTSIDE mutex_ —
        // take the lock just for the record, matching every other writer.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            record_own_spool_write(slot_index, info.spoolman_id, old_spoolman_id);
        }

        // Only send command if there are actual changes to persist
        if (has_changes) {
            execute_gcode(cmd);
            spdlog::debug("[AMS HappyHare] Sent: {}", cmd);
        }

        // Tool-to-gate mapping is a separate Happy Hare concern from MMU_GATE_MAP
        // (which is filament metadata). Emit MMU_TTG_MAP whenever the slot edit
        // path changes mapped_tool — mirrors set_tool_mapping() for the modal flow.
        if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0) {
            execute_gcode(fmt::format("MMU_TTG_MAP TOOL={} GATE={}", info.mapped_tool, slot_index));
        }
    }

    // Emit OUTSIDE the lock to avoid deadlock with callbacks
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));

    if (!rejected_material.empty()) {
        return AmsError(AmsResult::COMMAND_FAILED,
                        "Material '" + rejected_material +
                            "' contains characters that cannot be "
                            "sent as a G-code parameter",
                        "Couldn't save the material name",
                        "Everything else was saved. Rename the material using letters, digits, "
                        "spaces, and + - _ . ( ) /");
    }

    return AmsErrorHelper::success();
}

uint64_t AmsBackendHappyHare::firmware_tool_mapping_generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.firmware_mapping_generation();
}

AmsError AmsBackendHappyHare::set_tool_mapping(int tool_number, int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tool_number < 0 ||
            tool_number >= static_cast<int>(system_info_.tool_to_slot_map.size())) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "");
        }

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(
                slot_index, slots_.slot_count() > 0 ? slots_.slot_count() - 1 : 0);
        }

        // Check if another tool already maps to this slot
        for (size_t i = 0; i < system_info_.tool_to_slot_map.size(); ++i) {
            if (i != static_cast<size_t>(tool_number) &&
                system_info_.tool_to_slot_map[i] == slot_index) {
                spdlog::warn("[AMS HappyHare] Tool {} will share slot {} with tool {}", tool_number,
                             slot_index, i);
                break;
            }
        }
    }

    // Send MMU_TTG_MAP command to update tool-to-gate mapping (Happy Hare uses "gate" in its API)
    std::ostringstream cmd;
    cmd << "MMU_TTG_MAP TOOL=" << tool_number << " GATE=" << slot_index;

    spdlog::info("[AMS HappyHare] Mapping T{} to slot {}", tool_number, slot_index);
    return execute_gcode(cmd.str());
}

// ============================================================================
// Bypass Mode Operations
// ============================================================================

AmsError AmsBackendHappyHare::enable_bypass() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (!helix::bypass_available_for(system_info_.supports_bypass)) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            "This Happy Hare system does not support bypass mode", "");
        }

        // Twin of the AFC guard in AmsBackendAfc::enable_bypass(), and required
        // for the same reason: allows_implicit_chaining() == false means the
        // sidebar sends one command and lets the backend refuse, but
        // execute_gcode() is fire-and-forget (returns success before Klipper
        // answers), so a refused MMU_SELECT_BYPASS would report success and
        // change nothing. Happy Hare's cmd_MMU_SELECT_BYPASS runs
        // check_if_loaded() and answers only with a `!!` line, which reaches the
        // user as a bare "Operation not possible. Filament is loaded" toast
        // contradicting the success we already reported.
        //
        // Keyed on filament_pos rather than system_info_.filament_loaded because
        // that flag is set solely from filament == "Loaded" — Happy Hare refuses
        // at every position except UNLOADED and UNKNOWN, so an intermediate
        // position (mid-bowden, mid-unload) has to refuse here too.
        if (filament_pos_ != HAPPY_HARE_POS_UNLOADED && filament_pos_ != HAPPY_HARE_POS_UNKNOWN) {
            return AmsError(AmsResult::WRONG_STATE, "Unload filament first",
                            "Filament is still loaded. Unload it before enabling bypass.", "");
        }
    }

    // Happy Hare uses MMU_SELECT_BYPASS to select bypass
    spdlog::info("[AMS HappyHare] Enabling bypass mode");
    return execute_gcode("MMU_SELECT_BYPASS");
}

AmsError AmsBackendHappyHare::disable_bypass() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        if (system_info_.current_slot != -2) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not active",
                            "Bypass mode is not currently active", "");
        }
    }

    // To disable bypass, select a gate or unload
    // MMU_SELECT GATE=0 or MMU_HOME will deselect bypass
    spdlog::info("[AMS HappyHare] Disabling bypass mode (homing selector)");
    return execute_gcode("MMU_HOME");
}

bool AmsBackendHappyHare::is_bypass_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_slot == -2;
}

std::optional<bool> AmsBackendHappyHare::toolhead_filament_unaccounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!system_info_.filament_loaded) {
        return false;
    }
    // current_slot: >=0 gate feeding, -1 none, -2 bypass.
    return system_info_.current_slot == -1;
}

// ============================================================================
// Endless Spool Operations (group-based, runtime-editable on a single unit)
// ============================================================================

helix::printer::EndlessSpoolCapabilities
AmsBackendHappyHare::get_endless_spool_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    using namespace helix::printer;

    EndlessSpoolCapabilities caps;
    caps.availability = EndlessSpoolAvailability::Available;
    // Derived from the transport-parsed carrier, never answered independently.
    // mmu.endless_spool_enabled is the only source; when the subscription has
    // not delivered a frame yet the flag is still false, which the NotReady
    // restriction below is what actually communicates.
    caps.enabled =
        system_info_.endless_spool_enabled ? EndlessSpoolEnabled::On : EndlessSpoolEnabled::Off;

    if (!slots_.is_initialized()) {
        caps.editability = EndlessSpoolEditability::ReadOnly;
        caps.restriction = EndlessSpoolRestriction::NotReady;
        caps.enabled = EndlessSpoolEnabled::Unknown;
        return caps;
    }
    // MMU_ENDLESS_SPOOL has no UNIT= and acts on the currently-selected unit, so
    // a client cannot reliably target one unit's groups on an EMU rig.
    if (system_info_.units.size() > 1) {
        caps.editability = EndlessSpoolEditability::ReadOnly;
        caps.restriction = EndlessSpoolRestriction::MultiUnit;
        return caps;
    }
    caps.editability = EndlessSpoolEditability::Group;
    return caps;
}

helix::printer::EndlessSpoolConfig AmsBackendHappyHare::get_endless_spool_config() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!slots_.is_initialized()) {
        return {};
    }
    // One group id per gate, straight from mmu.endless_spool_groups. The lossy
    // "pick the first other member" step that used to happen right here is now a
    // rendering concern (endless_spool_backup_edges), so a 4-gate group survives
    // as one group instead of four arbitrary arrows.
    std::vector<int> group_ids;
    group_ids.reserve(static_cast<size_t>(slots_.slot_count()));
    for (int i = 0; i < slots_.slot_count(); ++i) {
        const auto* entry = slots_.get(i);
        group_ids.push_back(entry ? entry->info.endless_spool_group : -1);
    }
    return helix::printer::endless_spool_config_from_groups(group_ids);
}

AmsError AmsBackendHappyHare::apply_endless_spool_backup(int slot_index, int backup_slot) {
    std::string csv;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Availability, editability, ranges and self-backup are settled by
        // AmsBackend::set_endless_spool_backup(). What is left is Happy Hare's
        // own precondition: GROUPS is ignored while the feature is off.
        if (!system_info_.endless_spool_enabled) {
            return AmsError(AmsResult::WRONG_STATE,
                            "MMU_ENDLESS_SPOOL ignores GROUPS while endless spool is disabled",
                            "Endless spool is turned off on this MMU",
                            "Turn endless spool on, then set the backup gate");
        }

        const int n = slots_.slot_count();

        // Build the GROUPS array (length == num_gates), indexed by gate. HH requires
        // a non-negative group id for every gate; gates with no group get a fresh
        // standalone id. For a single-unit MMU the gate number equals the global
        // slot index, so slot_index/backup_slot index directly into the array.
        std::vector<int> groups(n, -1);
        int next_unique = 0;
        for (int i = 0; i < n; ++i) {
            const auto* e = slots_.get(i);
            int gate = (e ? e->info.global_index : i);
            if (gate < 0 || gate >= n) {
                gate = i;
            }
            int g = (e && e->info.endless_spool_group >= 0) ? e->info.endless_spool_group : -1;
            groups[gate] = g;
            if (g >= next_unique) {
                next_unique = g + 1;
            }
        }
        for (int i = 0; i < n; ++i) {
            if (groups[i] < 0) {
                groups[i] = next_unique++;
            }
        }

        if (backup_slot < 0) {
            // Remove backup: move this gate into a fresh standalone group.
            int mx = 0;
            for (int g : groups) {
                mx = std::max(mx, g);
            }
            groups[slot_index] = mx + 1;
        } else {
            // Join the backup gate's group so the two back each other up.
            groups[slot_index] = groups[backup_slot];
        }

        for (int i = 0; i < n; ++i) {
            if (i) {
                csv += ',';
            }
            csv += std::to_string(groups[i]);
        }
    }

    spdlog::info("[AMS HappyHare] Setting endless spool: slot {} backup {} -> GROUPS={}",
                 slot_index, backup_slot, csv);
    // No ENABLE=: the guard above already established that endless spool is on,
    // so GROUPS will be honoured, and passing ENABLE=1 would persist a state
    // change the user did not ask for.
    return execute_gcode("MMU_ENDLESS_SPOOL QUIET=1 GROUPS=" + csv);
}

AmsError AmsBackendHappyHare::reset_tool_mappings() {
    spdlog::info("[AMS HappyHare] Resetting tool mappings to 1:1");

    int tool_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tool_count = static_cast<int>(system_info_.tool_to_slot_map.size());
    }

    // Reset to 1:1 mapping (T0→Gate0, T1→Gate1, etc.)
    // Continue on failure to reset as many as possible, return first error
    AmsError first_error = AmsErrorHelper::success();
    for (int tool = 0; tool < tool_count; tool++) {
        AmsError result = set_tool_mapping(tool, tool);
        if (!result.success()) {
            spdlog::error("[AMS HappyHare] Failed to reset tool {} mapping: {}", tool,
                          result.technical_msg);
            if (first_error.success()) {
                first_error = result;
            }
        }
    }

    return first_error;
}

AmsError AmsBackendHappyHare::reset_endless_spool() {
    // ENABLE=1 is required: HH's MMU_ENDLESS_SPOOL handler early-returns (ignoring
    // RESET) when endless spool is currently disabled. With ENABLE=1, RESET=1 then
    // restores both the groups and the enabled flag to the config defaults
    // (_reset_endless_spool overwrites the momentary enable with the config default).
    spdlog::info("[AMS HappyHare] Resetting endless spool groups to config defaults");
    return execute_gcode("MMU_ENDLESS_SPOOL ENABLE=1 RESET=1 QUIET=1");
}

// ============================================================================
// Tool Mapping Operations
// ============================================================================

helix::printer::ToolMappingCapabilities AmsBackendHappyHare::get_tool_mapping_capabilities() const {
    // Happy Hare supports tool-to-gate mapping via MMU_TTG_MAP G-code
    return {true, true, "Tool-to-gate mapping via MMU_TTG_MAP"};
}

std::vector<int> AmsBackendHappyHare::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

// ============================================================================
// Dryer Control (v4 - KMS/EMU hardware)
// ============================================================================

DryerInfo AmsBackendHappyHare::get_dryer_info(int unit) const {
    (void)unit; // single-unit
    std::lock_guard<std::mutex> lock(mutex_);
    DryerInfo out = dryer_info_;
    if (dry_end_epoch_ > 0) {
        const int remaining = static_cast<int>((dry_end_epoch_ - now_fn_()) / 60);
        out.remaining_min = remaining > 0 ? remaining : 0;
    }
    return out;
}

std::string AmsBackendHappyHare::gates_suffix_for_unit(int unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Single-unit MMU (or unspecified unit): omit GATES so HH targets all
    // non-empty gates, matching the long-standing whole-MMU behavior.
    if (unit < 0 || system_info_.units.size() <= 1) {
        return "";
    }
    for (const auto& u : system_info_.units) {
        if (u.unit_index != unit) {
            continue;
        }
        // Target every gate on this unit. NOTE: includes empty gates; per-gate
        // occupancy filtering is a future refinement (needs real EMU hardware).
        std::string csv;
        for (int i = 0; i < u.slot_count; ++i) {
            if (i) {
                csv += ',';
            }
            csv += std::to_string(u.first_slot_global_index + i);
        }
        return csv.empty() ? "" : (" GATES=" + csv);
    }
    return "";
}

AmsError AmsBackendHappyHare::start_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)fan_pct; // Happy Hare MMU_HEATER does not accept a FAN parameter
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dryer_info_.supported) {
            return AmsErrorHelper::not_supported("Dryer not available on this hardware");
        }
    }

    // Happy Hare uses TIMER= (minutes) not DURATION=, and has no FAN parameter.
    // GATES targets a specific unit's gates on multi-unit (EMU) rigs; gates_suffix
    // locks internally, so it is called with no lock held.
    std::string cmd = fmt::format("MMU_HEATER DRY=1 TEMP={:.0f} TIMER={}", temp_c, duration_min) +
                      gates_suffix_for_unit(unit);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        dryer_info_.duration_min = duration_min;
        dry_end_epoch_ = now_fn_() + static_cast<std::time_t>(duration_min) * 60;
    }

    spdlog::info("[AMS HappyHare] Starting dryer: {:.0f}°C for {} min (unit {})", temp_c,
                 duration_min, unit);
    return execute_gcode(cmd);
}

AmsError AmsBackendHappyHare::stop_drying(int unit) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dryer_info_.supported) {
            return AmsErrorHelper::not_supported("Dryer not available on this hardware");
        }
        dry_end_epoch_ = 0;
        dryer_info_.active = false;
        dryer_info_.remaining_min = 0;
        dryer_info_.duration_min = 0;
    }

    spdlog::info("[AMS HappyHare] Stopping dryer (unit {})", unit);
    return execute_gcode("MMU_HEATER STOP=1" + gates_suffix_for_unit(unit));
}

// ============================================================================
// Device Management
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendHappyHare::get_device_sections() const {
    return helix::printer::hh_default_sections();
}

std::vector<helix::printer::DeviceAction> AmsBackendHappyHare::get_device_actions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    using namespace helix::printer;
    auto actions = hh_default_actions();

    // If config hasn't loaded yet, disable all non-button actions
    if (!config_defaults_.loaded) {
        for (auto& a : actions) {
            if (a.type != ActionType::BUTTON) {
                a.enabled = false;
                a.disable_reason = "Loading configuration...";
            }
        }
        return actions;
    }

    // Helper: effective float value (user override > config default)
    auto eff_f = [&](const std::optional<float>& ovr, float def) -> double {
        return static_cast<double>(ovr.value_or(def));
    };
    auto eff_i = [&](const std::optional<int>& ovr, int def) -> int { return ovr.value_or(def); };

    // Overlay effective values onto actions
    for (auto& a : actions) {
        // --- Speed sliders ---
        if (a.id == "gear_from_buffer_speed") {
            a.current_value = eff_f(user_overrides_.gear_from_buffer_speed,
                                    config_defaults_.gear_from_buffer_speed);
        } else if (a.id == "gear_from_spool_speed") {
            a.current_value = eff_f(user_overrides_.gear_from_spool_speed,
                                    config_defaults_.gear_from_spool_speed);
        } else if (a.id == "gear_unload_speed") {
            a.current_value =
                eff_f(user_overrides_.gear_unload_speed, config_defaults_.gear_unload_speed);
        } else if (a.id == "selector_speed") {
            a.current_value =
                eff_f(user_overrides_.selector_move_speed, config_defaults_.selector_move_speed);
        } else if (a.id == "extruder_load_speed") {
            a.current_value =
                eff_f(user_overrides_.extruder_load_speed, config_defaults_.extruder_load_speed);
        } else if (a.id == "extruder_unload_speed") {
            a.current_value = eff_f(user_overrides_.extruder_unload_speed,
                                    config_defaults_.extruder_unload_speed);
        }
        // --- Toolhead sliders ---
        else if (a.id == "toolhead_sensor_to_nozzle") {
            a.current_value = eff_f(user_overrides_.toolhead_sensor_to_nozzle,
                                    config_defaults_.toolhead_sensor_to_nozzle);
        } else if (a.id == "toolhead_extruder_to_nozzle") {
            a.current_value = eff_f(user_overrides_.toolhead_extruder_to_nozzle,
                                    config_defaults_.toolhead_extruder_to_nozzle);
        } else if (a.id == "toolhead_entry_to_extruder") {
            a.current_value = eff_f(user_overrides_.toolhead_entry_to_extruder,
                                    config_defaults_.toolhead_entry_to_extruder);
        } else if (a.id == "toolhead_ooze_reduction") {
            a.current_value = eff_f(user_overrides_.toolhead_ooze_reduction,
                                    config_defaults_.toolhead_ooze_reduction);
        }
        // --- Accessories ---
        else if (a.id == "espooler_mode") {
            // Status-backed: use live value if available
            if (!espooler_active_.empty()) {
                a.current_value = espooler_active_;
            }
        } else if (a.id == "clog_detection") {
            // Status-backed flowguard_encoder_mode_ takes priority, else override/config
            int mode = (flowguard_encoder_mode_ >= 0)
                           ? flowguard_encoder_mode_
                           : eff_i(user_overrides_.clog_detection, config_defaults_.clog_detection);
            // Map int to display string
            switch (mode) {
            case 1:
                a.current_value = std::string("Manual");
                break;
            case 2:
                a.current_value = std::string("Auto");
                break;
            default:
                a.current_value = std::string("Off");
                break;
            }
        } else if (a.id == "sync_to_extruder") {
            int val = eff_i(user_overrides_.sync_to_extruder, config_defaults_.sync_to_extruder);
            a.current_value = (val != 0);
        }
        // --- Setup ---
        else if (a.id == "led_mode") {
            // Status-backed: use live LED exit_effect if available
            if (!led_exit_effect_.empty()) {
                a.current_value = led_exit_effect_;
            }
        }
    }

    // --- Topology filtering (Type B = hub-based, no servo/selector/encoder) ---
    if (is_type_b()) {
        for (auto& a : actions) {
            if (a.id == "calibrate_encoder" || a.id == "servo_buzz" || a.id == "servo_up" ||
                a.id == "servo_move" || a.id == "servo_down") {
                a.enabled = false;
                a.disable_reason = "Not available on hub-based (Type B) systems";
            } else if (a.id == "selector_speed") {
                a.enabled = false;
                a.disable_reason = "No selector on hub-based systems";
            } else if (a.id == "clog_detection") {
                a.enabled = false;
                a.disable_reason = "Encoder-based clog detection not available on Type B";
            }
        }
    }

    return actions;
}

AmsError AmsBackendHappyHare::execute_device_action(const std::string& action_id,
                                                    const std::any& value) {
    spdlog::info("[AMS HappyHare] Executing device action: {}", action_id);

    // Helper to extract a typed value from std::any with uniform error handling
    auto require_string = [&](const char* label) -> std::pair<std::string, AmsError> {
        if (!value.has_value()) {
            return {"", AmsError(AmsResult::WRONG_STATE, fmt::format("{} value required", label),
                                 "Missing value", fmt::format("Select a {}", label))};
        }
        try {
            return {std::any_cast<std::string>(value), AmsErrorHelper::success()};
        } catch (const std::bad_any_cast&) {
            return {"", AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                                 "Invalid value type", fmt::format("Select a valid {}", label))};
        }
    };

    // Helper to look up an action's min/max range from defaults
    auto get_action_range = [](const std::string& id) -> std::pair<float, float> {
        static auto defaults = helix::printer::hh_default_actions();
        for (const auto& a : defaults) {
            if (a.id == id)
                return {a.min_value, a.max_value};
        }
        return {-1e6f, 1e6f};
    };

    // Helper to extract double from std::any (UI sends doubles)
    auto require_double = [&](const char* label) -> std::pair<double, AmsError> {
        if (!value.has_value()) {
            return {0.0, AmsError(AmsResult::WRONG_STATE, fmt::format("{} value required", label),
                                  "Missing value", fmt::format("Provide a {}", label))};
        }
        try {
            return {std::any_cast<double>(value), AmsErrorHelper::success()};
        } catch (const std::bad_any_cast&) {
            return {0.0,
                    AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                             "Invalid value type", fmt::format("Provide a numeric {}", label))};
        }
    };

    // Helper to extract bool from std::any
    auto require_bool = [&](const char* label) -> std::pair<bool, AmsError> {
        if (!value.has_value()) {
            return {false, AmsError(AmsResult::WRONG_STATE, fmt::format("{} value required", label),
                                    "Missing value", fmt::format("Provide {}", label))};
        }
        try {
            return {std::any_cast<bool>(value), AmsErrorHelper::success()};
        } catch (const std::bad_any_cast&) {
            return {false,
                    AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                             "Invalid value type", fmt::format("Provide a boolean {}", label))};
        }
    };

    // --- Simple button actions (no value required) ---
    // clang-format off
    static const std::pair<const char*, const char*> button_actions[] = {
        {"calibrate_bowden",    "MMU_CALIBRATE_BOWDEN"},
        {"calibrate_encoder",   "MMU_CALIBRATE_ENCODER"},
        {"calibrate_gear",      "MMU_CALIBRATE_GEAR"},
        {"calibrate_gates",     "MMU_CALIBRATE_GATES"},
        {"test_grip",           "MMU_TEST_GRIP"},
        {"test_load",           "MMU_TEST_LOAD"},
        {"test_move",           "MMU_TEST_MOVE"},
        {"servo_buzz",          "MMU_SERVO"},
        {"servo_up",            "MMU_SERVO POS=up"},
        {"servo_move",          "MMU_SERVO POS=move"},
        {"servo_down",          "MMU_SERVO POS=down"},
        {"reset_servo_counter", "MMU_STATS COUNTER=servo RESET=1"},
        {"reset_blade_counter", "MMU_STATS COUNTER=cutter RESET=1"},
    };
    // clang-format on
    for (const auto& [id, gcode] : button_actions) {
        if (action_id == id) {
            return execute_gcode(gcode);
        }
    }

    // --- LED mode dropdown ---
    if (action_id == "led_mode") {
        auto [mode, err] = require_string("LED mode");
        if (!err)
            return err;
        return execute_gcode("MMU_LED EXIT_EFFECT=" + mode);
    }

    // --- Speed sliders (integer formatting) ---
    // clang-format off
    static const std::pair<const char*, const char*> speed_params[] = {
        {"gear_from_buffer_speed", "GEAR_FROM_BUFFER_SPEED"},
        {"gear_from_spool_speed",  "GEAR_FROM_SPOOL_SPEED"},
        {"gear_unload_speed",      "GEAR_UNLOAD_SPEED"},
        {"selector_speed",         "SELECTOR_MOVE_SPEED"},
        {"extruder_load_speed",    "EXTRUDER_LOAD_SPEED"},
        {"extruder_unload_speed",  "EXTRUDER_UNLOAD_SPEED"},
    };
    // clang-format on
    for (const auto& [id, param] : speed_params) {
        if (action_id == id) {
            auto [speed, err] = require_double("speed");
            if (!err)
                return err;
            auto [lo, hi] = get_action_range(id);
            speed = std::clamp(speed, static_cast<double>(lo), static_cast<double>(hi));
            auto result = execute_gcode(fmt::format("MMU_TEST_CONFIG {}={:.0f}", param, speed));
            if (result.success()) {
                save_override(action_id, static_cast<float>(speed));
            }
            return result;
        }
    }

    // --- Toolhead distance sliders (one decimal place) ---
    // clang-format off
    static const std::pair<const char*, const char*> toolhead_params[] = {
        {"toolhead_sensor_to_nozzle",   "TOOLHEAD_SENSOR_TO_NOZZLE"},
        {"toolhead_extruder_to_nozzle", "TOOLHEAD_EXTRUDER_TO_NOZZLE"},
        {"toolhead_entry_to_extruder",  "TOOLHEAD_ENTRY_TO_EXTRUDER"},
        {"toolhead_ooze_reduction",     "TOOLHEAD_OOZE_REDUCTION"},
    };
    // clang-format on
    for (const auto& [id, param] : toolhead_params) {
        if (action_id == id) {
            auto [dist, err] = require_double("distance");
            if (!err)
                return err;
            auto [lo, hi] = get_action_range(id);
            dist = std::clamp(dist, static_cast<double>(lo), static_cast<double>(hi));
            auto result = execute_gcode(fmt::format("MMU_TEST_CONFIG {}={:.1f}", param, dist));
            if (result.success()) {
                save_override(action_id, static_cast<float>(dist));
            }
            return result;
        }
    }

    // --- sync_to_extruder toggle ---
    if (action_id == "sync_to_extruder") {
        auto [enable, err] = require_bool("sync state");
        if (!err)
            return err;
        int val = enable ? 1 : 0;
        auto result = execute_gcode(fmt::format("MMU_TEST_CONFIG SYNC_TO_EXTRUDER={}", val));
        if (result.success()) {
            save_override(action_id, val);
        }
        return result;
    }

    // --- eSpooler mode dropdown ---
    if (action_id == "espooler_mode") {
        auto [mode, err] = require_string("eSpooler mode");
        if (!err)
            return err;
        return execute_gcode("MMU_ESPOOLER OPERATION=" + mode);
    }

    // --- Clog detection dropdown ---
    if (action_id == "clog_detection") {
        auto [mode_str, err] = require_string("clog detection mode");
        if (!err)
            return err;
        int mode_int = 0;
        if (mode_str == "Manual")
            mode_int = 1;
        else if (mode_str == "Auto")
            mode_int = 2;
        auto result = execute_gcode(fmt::format("MMU_TEST_CONFIG CLOG_DETECTION={}", mode_int));
        if (result.success()) {
            save_override(action_id, mode_int);
        }
        return result;
    }

    // --- Runtime gear-motor sync (live action, distinct from config sync_to_extruder) ---
    if (action_id == "gear_sync") {
        auto [enable, err] = require_bool("gear sync state");
        if (!err)
            return err;
        return execute_gcode(enable ? "MMU_SYNC_GEAR_MOTOR SYNC=1" : "MMU_SYNC_GEAR_MOTOR SYNC=0");
    }

    // --- Motors toggle ---
    if (action_id == "motors_toggle") {
        auto [enable, err] = require_bool("motor state");
        if (!err)
            return err;
        return execute_gcode(enable ? "MMU_HOME" : "MMU_MOTORS_OFF");
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}
