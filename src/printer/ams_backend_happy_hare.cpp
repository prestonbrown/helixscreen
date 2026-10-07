// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_happy_hare.h"

#include "ui_insert_notice.h"
#include "ui_update_queue.h"

#include "ams_bypass_policy.h"
#include "ams_fault_event.h"
#include "ams_state.h"
#include "config.h"
#include "happy_hare_status_parse.h"
#include "hh_defaults.h"
#include "humidity_sensor_types.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "operation_patterns.h"    // helix::contains_ci
#include "print_lifecycle_state.h" // job_holds_machine
#include "printer_state.h"         // PrinterState: mid-print clear guard
#include "settings_manager.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <map>

namespace helix {

namespace tio = ::helix::text_io;

namespace {

/// The two printer.mmu filament_pos values Happy Hare's own check_if_loaded()
/// treats as "not loaded" (mmu.py FILAMENT_POS_UNKNOWN / FILAMENT_POS_UNLOADED).
/// Every other position, including the intermediate ones, is refused.
constexpr int HAPPY_HARE_POS_UNKNOWN = -1;
constexpr int HAPPY_HARE_POS_UNLOADED = 0;

/// HH's reasons carry raw newlines (it swaps them for ". " only in its own
/// dialog); one-line UI text needs the same.
std::string display_reason(std::string reason) {
    for (auto at = reason.find('\n'); at != std::string::npos; at = reason.find('\n', at)) {
        reason.replace(at, 1, ". ");
    }
    return reason;
}

/// Shared empty object so the resolver can be handed a missing source without
/// every caller minting its own temporary.
const nlohmann::json& hh_empty_object() {
    static const nlohmann::json empty = nlohmann::json::object();
    return empty;
}

/// Which unit a global gate index falls in, or -1 when it falls in none.
int unit_index_for_gate(const AmsSystemInfo& info, int gate) {
    if (gate < 0) {
        return -1;
    }
    for (const AmsUnit& u : info.units) {
        if (gate >= u.first_slot_global_index && gate < u.first_slot_global_index + u.slot_count) {
            return u.unit_index;
        }
    }
    return -1;
}

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
    // endless_spool_enabled arrives (see handle_status).
    system_info_.endless_spool_enabled = false;
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
    // Overridden by apply_tip_method_config() once configfile response arrives.
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
    // keyword-bearing names (v3 mmu_gate / mmu_pre_gate_N / mmu_gear_N, v4
    // mmu_entry_N / mmu_exit_N) are caught by PrinterHardware's substring path.
    return bare_name == "extruder" || bare_name == "toolhead" || bare_name == "filament_tension" ||
           bare_name == "filament_compression";
}

AmsBackendHappyHare::~AmsBackendHappyHare() {
    // Expire queued callbacks before this class's members are destroyed; the
    // base guard itself outlives them.
    lifetime_.invalidate();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

void AmsBackendHappyHare::on_started() {
    // Load the user's attached slot identity before any of the queries below,
    // so the first gate-map frame they provoke already has something to layer.
    // Outside mutex_, because the DB round-trip blocks and the status
    // subscription is already live. Publish under it so the parse path reads a
    // whole map.
    auto loaded = helix::ams::make_loaded_override_store(api_, "happyhare", get_type(),
                                                         backend_log_tag(), OVERRIDE_NAMESPACE);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        override_store_ = std::move(loaded.store);
        overrides_ = std::move(loaded.overrides);
    }
    if (override_store_) {
        helix::ams::ingest_legacy_records(*override_store_, helix::ams::LegacyLockKeys::LaneData,
                                          backend_index());
    }

    // Tip method (cutter vs tip-forming), selector type (topology), filament heater
    // and the speed/distance defaults all come from one configfile query.
    query_config_from_printer();
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

    auto info = slots_.build_system_info(system_info_);

    // Surface per-unit environment data (box heater temp + humidity) so the AMS
    // panel indicator (heat-waves icon, live temp) and the dryer overlay show a
    // reading. Each unit resolves its OWN heater + env sensor: a scalar shared
    // sensor/heater applies to every unit (QIDI Box, common case); a per-gate
    // (EMU) list maps each unit to the object at its first gate, so multi-MMU rigs
    // with distinct box sensors read correctly. Per-gate *drying control*
    // (start/stop/countdown) still uses the global dryer model — true per-gate
    // drying is a separate gap (drying_state array; see apply_mmu_drying_locked).
    // Environment is independent of dryer support: a passive enclosure has a sensor and
    // no heater, and a heater reports a temperature with no sensor configured. Each unit
    // below is skipped on its own when it has neither, so no outer gate is needed.
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

        // Surface the unit's environment whenever ANY reading is present: an
        // enclosure can be monitored without a heater at all, and a heater's
        // temperature can arrive after its humidity does.
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

    return info;
}

AmsType AmsBackendHappyHare::get_type() const {
    return AmsType::HAPPY_HARE;
}

bool AmsBackendHappyHare::manages_active_spool() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.spoolman_mode != SpoolmanMode::OFF;
}

SlotInfo* AmsBackendHappyHare::cached_slot_locked(int slot_index) {
    // A repaint needs no refresh_gate_statuses_locked() after it. The lane's
    // presence is the sensed record this backend files from gate_status_raw_,
    // the same array that refresh reads, so the status a repaint narrows is
    // already the one the refresh derived.
    auto* entry = slots_.get_mut(slot_index);
    return entry ? &entry->info : nullptr;
}

void AmsBackendHappyHare::prepare_lane_repaint_locked(int slot_index, SlotInfo& slot) {
    const auto it = overrides_.find(slot_index);
    helix::ams::clear_lane_only_identity(slot, it == overrides_.end() ? nullptr : &it->second);
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

bool AmsBackendHappyHare::unit_is_type_b_locked(int unit_index) const {
    if (unit_index >= 0 && unit_index < static_cast<int>(machine_units_.size()) &&
        !machine_units_[unit_index].selector_type.empty()) {
        return machine_units_[unit_index].selector_type == "VirtualSelector";
    }
    return is_type_b();
}

void AmsBackendHappyHare::update_unit_topologies() {
    for (auto& unit : system_info_.units) {
        unit.topology =
            unit_is_type_b_locked(unit.unit_index) ? PathTopology::HUB : PathTopology::LINEAR;
        unit.has_encoder = unit_supports_locked(unit.unit_index, happy_hare::UnitFeature::Encoder);
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
        const auto* gate = gate_sensor(slot_index);
        if (gate && gate->has_pre_gate_sensor && gate->pre_gate_triggered) {
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
    const auto* gate = gate_sensor(slot_index);
    return gate != nullptr && gate->has_pre_gate_sensor;
}

// ============================================================================
// Moonraker Status Update Handling
// ============================================================================

void AmsBackendHappyHare::handle_status(const nlohmann::json& params) {
    spdlog::trace("[AMS HappyHare] Received status update");

    // Per-gate entry sensor objects are sibling keys. Their presence is known
    // before printer.mmu is applied, so the sensors dict there never treats its
    // selected-gate reading as the only one; their readings are applied after
    // it, once a first frame's gate_status has sized the slots.
    const auto entry_sensors = happy_hare::parse_entry_sensor_objects(params);
    if (!entry_sensors.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        entry_sensor_objects_seen_ = true;
    }

    // Parse MMU core state if present.
    const bool mmu_present = params.contains("mmu") && params["mmu"].is_object();
    if (mmu_present) {
        const happy_hare::MmuStatusDelta delta = happy_hare::parse_mmu_status(params["mmu"]);
        MmuFrame frame;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            apply_mmu_status_locked(delta, frame);
        }
        // An insert nothing vouches for asks whether the stored record still
        // describes the spool that went in. The notice re-checks its own guards
        // on the UI thread.
        for (const int gate : frame.unverified_insert_gates) {
            helix::ui::queue_update("AmsBackendHappyHare::handle_status", [gate] {
                helix::ui::offer_clear_after_unverified_insert(gate);
            });
        }
    }

    if (!entry_sensors.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        apply_entry_sensor_objects_locked(entry_sensors);
    }

    // Parse live heater_generic temp/target even when mmu key is absent —
    // Moonraker sends heater updates as sibling keys in the same notification.
    const bool heater_updated = apply_filament_heater_status(params);

    // Parse box humidity from the environment sensor chip (sibling key too).
    const bool humidity_updated = apply_environment_sensor_status(params);

    // Only re-pump downstream sync when this frame actually carried AMS-relevant
    // data; notify_status_update fires for every Klipper object (toolhead, temps,
    // ...), so an unconditional emit here would be a per-frame event storm.
    if (mmu_present || !entry_sensors.empty() || heater_updated || humidity_updated) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendHappyHare::apply_mmu_status_locked(const happy_hare::MmuStatusDelta& delta,
                                                  MmuFrame& frame) {
    // reason_for_pause is empty exactly when no fault stands, so the fault edge
    // at the tail needs the state this frame started from, before the selector
    // step rewrites it.
    frame.was_faulted = !reason_for_pause_.empty();
    // Only v4 publishes tangle_prevention: the frame names the layout before
    // the connect-time query answers.
    if (delta.telemetry.v4_marker) {
        machine_layout_.v4 = true;
    }

    apply_mmu_selector_locked(delta.core);
    apply_mmu_path_locked(delta.core);
    apply_mmu_topology_locked(delta.topology);
    apply_gate_status_locked(delta.identity);
    apply_gate_appearance_locked(delta.identity, frame);
    apply_gate_binding_locked(delta, frame);
    apply_mmu_telemetry_locked(delta.telemetry);
    apply_mmu_sensors_locked(delta);
    converge_mmu_locked(frame);
}

ams::Observation& AmsBackendHappyHare::gate_reading_locked(int gate) {
    // One gate's standing identity record, created on first mention. Every
    // block of the parse amends a record that outlives the frame and the tail
    // files it once, because one ingest replaces that source's record whole.
    auto it = gate_readings_.find(gate);
    if (it == gate_readings_.end()) {
        it = gate_readings_.emplace(gate, ams::Observation(ams::ObservationSource::VendorCache))
                 .first;
    }
    return it->second;
}

void AmsBackendHappyHare::apply_mmu_selector_locked(const happy_hare::MmuCoreDelta& core) {
    // printer.mmu.gate: -1 = no gate selected, -2 = bypass
    if (core.gate) {
        system_info_.current_slot = *core.gate;
        spdlog::trace("[AMS HappyHare] Current slot: {}", system_info_.current_slot);
    }

    if (core.tool) {
        system_info_.current_tool = *core.tool;
        spdlog::trace("[AMS HappyHare] Current tool: {}", system_info_.current_tool);
    }

    // printer.mmu.filament: "Loaded" or "Unloaded"
    if (core.filament_loaded) {
        system_info_.filament_loaded = *core.filament_loaded;
        spdlog::trace("[AMS HappyHare] Filament loaded: {}", system_info_.filament_loaded);
    }

    // reason_for_pause: descriptive error message from Happy Hare. HH publishes
    // it only while print_state is pause_locked or paused and "" once the pause
    // ends (resume or cancel), so its emptiness is the fault state.
    if (core.reason_for_pause) {
        reason_for_pause_ = *core.reason_for_pause;
        spdlog::trace("[AMS HappyHare] Reason for pause: {}", reason_for_pause_);
    }

    // printer.mmu.action: "Idle", "Loading", "Unloading", "Forming Tip",
    // "Heating", "Checking", etc.
    if (core.action) {
        system_info_.action = ams_action_from_string(*core.action);
        system_info_.operation_detail = *core.action;
        spdlog::trace("[AMS HappyHare] Action: {} ({})", ams_action_to_string(system_info_.action),
                      *core.action);

        // Drive the toolchange step bar from the action transition (HH has no
        // // narration). Deferred to main thread inside the helper.
        sync_narration_step();
    }
}

void AmsBackendHappyHare::apply_mmu_path_locked(const happy_hare::MmuCoreDelta& core) {
    // filament_pos: 0=unloaded, 1-2=gate area, 3=in bowden, 4=end bowden,
    // 5=homed extruder, 6=extruder entry, 7-8=loaded
    if (core.filament_pos) {
        filament_pos_ = *core.filament_pos;
        spdlog::trace("[AMS HappyHare] Filament pos: {} -> {}", filament_pos_,
                      path_segment_to_string(path_segment_from_happy_hare_pos(filament_pos_)));

        refresh_hub_sensors_locked();
    }

    // bowden_progress (v4): 0-100 = loading progress percentage, -1 = not
    // applicable
    if (core.bowden_progress) {
        bowden_progress_ = *core.bowden_progress;
        spdlog::trace("[AMS HappyHare] Bowden progress: {}%", bowden_progress_);
    }

    // has_bypass: not all MMU types support bypass (e.g., ERCF/Tradrack do,
    // BoxTurtle does not)
    if (core.has_bypass) {
        status_has_bypass_ = *core.has_bypass;
        apply_bypass_support_locked();
    } else if (!bypass_support_seen_ && !is_v4_locked()) {
        // Field absent entirely. Every Happy Hare we know of publishes it, so this
        // is a fork or a version we have not seen; assume supported rather than
        // silently removing a control the machine may well have. Deliberately not
        // the same as the pre-status default, which is false so that a system
        // without a bypass never flashes the UI up and then withdraws it.
        bypass_support_seen_ = true;
        system_info_.supports_bypass = true;
        spdlog::warn("[AMS HappyHare] No has_bypass field in mmu status; assuming supported");
    }
}

void AmsBackendHappyHare::refresh_hub_sensors_locked() {
    // pos >= 3 means filament is in the bowden or further (past the
    // selector/hub), on the unit whose range holds the current gate.
    const bool past_hub = (filament_pos_ >= 3);
    const int slot = system_info_.current_slot;
    for (auto& unit : system_info_.units) {
        unit.hub_sensor_triggered = past_hub && slot >= unit.first_slot_global_index &&
                                    slot < unit.first_slot_global_index + unit.slot_count;
    }
}

void AmsBackendHappyHare::apply_bypass_support_locked() {
    // Logged at info rather than trace, and on every change rather than never:
    // false here removes the entire bypass UI (sidebar toggle, Device Operations
    // section, path node) and this flag is the sole reason. v3 derives
    // printer.mmu.has_bypass from [mmu_machine] has_bypass, which defaults to 0
    // for mmu_vendor "Other", and on type-A selectors ANDs it with the calibrated
    // bypass offset, so an owner with a physical bypass can legitimately see
    // false and have no way to tell that from a bug in us. v4 publishes
    // printer.mmu.has_bypass as a constant true and puts each unit's answer on
    // mmu_machine.
    // Until the connect-time query names v4's units, a v4 frame asserts nothing.
    const std::optional<bool> has_bypass =
        is_v4_locked() ? machine_layout_.has_bypass : status_has_bypass_;
    if (!has_bypass) {
        return;
    }
    if (!bypass_support_seen_ || *has_bypass != system_info_.supports_bypass) {
        spdlog::info("[AMS HappyHare] Bypass supported: {}", *has_bypass);
        bypass_support_seen_ = true;
    }
    system_info_.supports_bypass = *has_bypass;
}

void AmsBackendHappyHare::apply_mmu_topology_locked(const happy_hare::MmuTopologyDelta& topology) {
    // num_units, for multi-unit Happy Hare setups
    if (topology.num_units) {
        num_units_ = *topology.num_units;
        spdlog::trace("[AMS HappyHare] Number of units: {}", num_units_);
    }

    // Per-unit gate counts: a dissimilar multi-unit setup such as a 6-gate ERCF
    // plus a 4-gate Box Turtle names them as a "6,4" string, a single unit as an
    // integer, the config format as an array.
    if (topology.gate_counts) {
        per_unit_gate_counts_ = *topology.gate_counts;
        spdlog::debug("[AMS HappyHare] Per-unit gate counts: {}", per_unit_gate_counts_.size());
    }

    // Active unit (v4)
    if (topology.active_unit) {
        active_unit_ = *topology.active_unit;
        spdlog::trace("[AMS HappyHare] Active unit: {}", active_unit_);
    }
}

void AmsBackendHappyHare::apply_gate_status_locked(const happy_hare::GateIdentityDelta& identity) {
    // gate_status: -1 = unknown, 0 = empty, 1 = available, 2 = from_buffer
    if (!identity.gate_status) {
        return;
    }
    const auto& gate_status = *identity.gate_status;
    const int gate_count = static_cast<int>(gate_status.size());

    // Initialize gates if this is the first time we see gate_status
    if (!slots_.is_initialized() && gate_count > 0) {
        initialize_slots(gate_count);
    }

    // Cache the raw values. The LOADED stamp is applied by
    // refresh_gate_statuses_locked() at the end of the frame rather than here,
    // because it depends on gate/filament — which arrive in their own deltas,
    // without gate_status (#1199).
    if (gate_status_raw_.size() != gate_status.size()) {
        gate_status_raw_.assign(gate_status.size(), -1);
    }
    for (size_t i = 0; i < gate_status.size(); ++i) {
        if (gate_status[i]) {
            gate_status_raw_[i] = *gate_status[i];
        }
    }
}

void AmsBackendHappyHare::apply_gate_appearance_locked(
    const happy_hare::GateIdentityDelta& identity, MmuFrame& frame) {
    // gate_color_rgb wins over the gate_color hex fallback whenever it painted
    // a gate.
    bool colors_parsed = false;
    if (identity.color_rgb) {
        const auto& colors = *identity.color_rgb;
        for (size_t i = 0; i < colors.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (!entry || !colors[i]) {
                continue;
            }
            const uint32_t rgb = *colors[i];
            entry->info.color_rgb = rgb;
            gate_reading_locked(static_cast<int>(i)).color_rgb = rgb;
            frame.stated_for(static_cast<int>(i)).color_rgb = rgb;
            colors_parsed = true;
        }
    }

    // Fallback: gate_color hex strings ["ffffff", "000000", ...]
    //
    // An empty entry is Happy Hare stating this gate has no colour. Anything
    // else that will not read is a value we cannot make sense of rather than a
    // gate with nothing in it, so the last readable word stands.
    if (!colors_parsed && identity.color) {
        const auto& colors = *identity.color;
        for (size_t i = 0; i < colors.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (!entry || !colors[i]) {
                continue;
            }
            const auto& color = *colors[i];
            const int gate = static_cast<int>(i);
            if (color.kind == ams::ColorReadingKind::Observed) {
                entry->info.color_rgb = color.rgb;
                gate_reading_locked(gate).color_rgb = color.rgb;
                frame.stated_for(gate).color_rgb = color.rgb;
            } else if (color.kind == ams::ColorReadingKind::Cleared) {
                entry->info.color_rgb = AMS_DEFAULT_SLOT_COLOR;
                gate_reading_locked(gate).color_rgb.reset();
                frame.cleared_for(gate).color_rgb = 0u;
            }
        }
    }

    // gate_material: strings like "PLA", "PETG", "ABS"
    if (identity.material) {
        const auto& materials = *identity.material;
        for (size_t i = 0; i < materials.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (!entry || !materials[i]) {
                continue;
            }
            const std::string& material = *materials[i];
            const int gate = static_cast<int>(i);
            entry->info.material = material;
            auto& reading = gate_reading_locked(gate);
            if (material.empty()) {
                reading.material.reset();
                frame.cleared_for(gate).material = std::string{};
            } else {
                reading.material = material;
                frame.stated_for(gate).material = material;
            }
        }
    }
}

void AmsBackendHappyHare::apply_gate_binding_locked(const happy_hare::MmuStatusDelta& delta,
                                                    MmuFrame& frame) {
    const auto& identity = delta.identity;

    // gate_spool_id (v4): per-gate Spoolman spool IDs, which enable weight
    // polling and fill gauges
    if (identity.spool_id) {
        const auto& spool_ids = *identity.spool_id;
        for (size_t i = 0; i < spool_ids.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (!entry || !spool_ids[i]) {
                continue;
            }
            const int id = *spool_ids[i];
            entry->info.spoolman_id = (id > 0) ? id : 0;
            auto& reading = gate_reading_locked(static_cast<int>(i));
            // Happy Hare writes 0 for a gate with no spool, which unlinks the
            // gate rather than naming a spool numbered zero.
            if (id > 0) {
                reading.spoolman_id = id;
            } else {
                reading.spoolman_id.reset();
            }
        }
        // The gate map is the one thing Happy Hare states about a gate's
        // binding, so this is where a binding that has stopped holding is
        // found. The id compared is the gate's own accumulated reading rather
        // than entry->info, which carries the resolved record's id back.
        //
        // Runs BEFORE the lane is painted at the tail: a binding this drops must
        // be gone from the model before anything reads it, or the gate paints
        // the spool that just stopped describing it for one more frame.
        for (size_t i = 0; i < spool_ids.size(); ++i) {
            const int gate = static_cast<int>(i);
            if (!slots_.get(gate)) {
                continue;
            }
            // find(), not gate_reading_locked(): looking a gate up must not
            // create a cache record for one that has none.
            auto it = gate_readings_.find(gate);
            const int firmware_id =
                it != gate_readings_.end() ? it->second.spoolman_id.value_or(0) : 0;
            if (reconcile_lane_binding(gate, firmware_id) != ams::BindingVerdict::Holds) {
                helix::ams::clear_persisted_override(override_store_.get(), overrides_, gate,
                                                     backend_log_tag());
                retire_departed_identity_locked(gate);
                // Another writer moved the gate to a different spool, which is
                // this backend's auto-clear signal: what the gate map states
                // from here on is that spool's own reading, not our echo.
                own_write_echoes_.abandon(gate);
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_spool_id for {} gates", spool_ids.size());
    }

    // gate_temperature (v4): per-gate nozzle temperature recommendations
    if (identity.temperature) {
        const auto& gate_temps = *identity.temperature;
        for (size_t i = 0; i < gate_temps.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (entry && gate_temps[i]) {
                entry->info.nozzle_temp_min = *gate_temps[i];
                entry->info.nozzle_temp_max = *gate_temps[i];
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_temperature for {} gates", gate_temps.size());
    }

    // gate_name (v4): per-gate filament names
    if (identity.name) {
        const auto& gate_names = *identity.name;
        for (size_t i = 0; i < gate_names.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (!entry || !gate_names[i]) {
                continue;
            }
            const std::string& name = *gate_names[i];
            entry->info.color_name = name;
            auto& reading = gate_reading_locked(static_cast<int>(i));
            if (name.empty()) {
                reading.spool_name.reset();
            } else {
                reading.spool_name = name;
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_name for {} gates", gate_names.size());
    }

    // Fallback: gate_filament_name (EMU uses this instead of gate_name)
    //
    // The record's precedence is decided on what this parse has read, never on
    // SlotInfo::color_name: a user's own name is merged into that field, so
    // asking it would let a person's edit choose which of the MMU's two keys
    // the vendor-cache record believes.
    if (identity.filament_name) {
        const auto& names = *identity.filament_name;
        for (size_t i = 0; i < names.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (!entry || !names[i]) {
                continue;
            }
            const std::string& name = *names[i];
            if (entry->info.color_name.empty()) {
                entry->info.color_name = name;
            }
            auto& reading = gate_reading_locked(static_cast<int>(i));
            if (!reading.spool_name.has_value() && !name.empty()) {
                reading.spool_name = name;
            }
        }
        spdlog::trace("[AMS HappyHare] Parsed gate_filament_name for {} gates", names.size());
    }

    // ttg_map (tool-to-gate mapping): update both legacy and registry tool maps.
    // Firmware-sourced: HH publishes the whole ttg_map in get_status() (mmu.py
    // get_status), so this array IS what the MMU currently believes, not our
    // intent. The optimistic counterpart is set_tool_mapping()'s own write below,
    // which precedes the MMU_TTG_MAP send (#1270).
    if (delta.topology.ttg_map) {
        system_info_.tool_to_slot_map = *delta.topology.ttg_map;
        slots_.set_tool_map(*delta.topology.ttg_map,
                            helix::printer::SlotRegistry::MappingSource::Firmware);
    }

    // endless_spool_groups, when the MMU names them
    if (identity.endless_spool_group) {
        const auto& groups = *identity.endless_spool_group;
        for (size_t i = 0; i < groups.size(); ++i) {
            auto* entry = slots_.get_mut(static_cast<int>(i));
            if (entry && groups[i]) {
                entry->info.endless_spool_group = *groups[i];
            }
        }
    }
}

void AmsBackendHappyHare::apply_mmu_telemetry_locked(const happy_hare::MmuTelemetryDelta& t) {
    // === Happy Hare v4 extended status fields ===

    // v4's per-gate list wins over the deprecated single value; the selected
    // gate may move without it. v3's list holds only the gates fitted with an
    // eSpooler, so there it lines up with gate numbers only when every gate is.
    if (t.espooler) {
        espooler_per_gate_ = *t.espooler;
    }
    const bool list_is_per_gate =
        !espooler_per_gate_.empty() &&
        (is_v4_locked() || static_cast<int>(espooler_per_gate_.size()) == slots_.slot_count());
    if (list_is_per_gate) {
        const int gate = system_info_.current_slot;
        system_info_.espooler_state =
            (gate >= 0 && gate < static_cast<int>(espooler_per_gate_.size()))
                ? espooler_per_gate_[gate]
                : std::string{};
        espooler_active_ = system_info_.espooler_state;
    } else if (t.espooler_active) {
        system_info_.espooler_state = *t.espooler_active;
        espooler_active_ = system_info_.espooler_state;
        spdlog::trace("[AMS HappyHare] eSpooler state: {}", system_info_.espooler_state);
    }

    if (t.sync_feedback_state) {
        system_info_.sync_feedback_state = *t.sync_feedback_state;
        spdlog::trace("[AMS HappyHare] Sync feedback: {}", system_info_.sync_feedback_state);
    }

    if (t.sync_feedback_bias) {
        system_info_.sync_feedback_bias = *t.sync_feedback_bias;
        spdlog::trace("[AMS HappyHare] Sync feedback bias (modelled): {:.3f}",
                      system_info_.sync_feedback_bias);
    }

    if (t.sync_feedback_bias_raw) {
        system_info_.sync_feedback_bias_raw = *t.sync_feedback_bias_raw;
        spdlog::trace("[AMS HappyHare] Sync feedback bias (raw): {:.3f}",
                      system_info_.sync_feedback_bias_raw);
    }

    // v4 publishes null for the selected unit's missing buffer: no reading, so
    // the "unavailable" sentinel rather than the last unit's value.
    constexpr float kBiasUnavailable = -2.0f;
    if (t.sync_feedback_bias_null) {
        system_info_.sync_feedback_bias = kBiasUnavailable;
    }
    if (t.sync_feedback_bias_raw_null) {
        system_info_.sync_feedback_bias_raw = kBiasUnavailable;
    }

    if (t.sync_drive) {
        system_info_.sync_drive = *t.sync_drive;
        spdlog::trace("[AMS HappyHare] Sync drive: {}", system_info_.sync_drive);
    }

    // Clog detection mode: 0=off, 1=manual, 2=auto. v3 publishes it as
    // clog_detection_enabled; v4 sends that as a constant false and carries the
    // configured mode as flowguard.encoder_mode on any unit with an encoder.
    // encoder.detection_mode is not it: that is the encoder's runtime state,
    // static until FlowGuard activates.
    std::optional<int> clog_mode = t.clog_detection_enabled;
    if (!clog_mode && t.flowguard) {
        clog_mode = t.flowguard->encoder_mode;
    }
    if (clog_mode) {
        system_info_.clog_detection = *clog_mode;
        system_info_.encoder_info.detection_mode = system_info_.clog_detection;
        system_info_.encoder_info.enabled = (system_info_.clog_detection > 0);
        spdlog::trace("[AMS HappyHare] Clog detection: {}", system_info_.clog_detection);
    }

    if (t.encoder) {
        const auto& encoder = *t.encoder;
        if (encoder.flow_rate) {
            system_info_.encoder_info.flow_rate = *encoder.flow_rate;
            // Keep legacy field in sync
            system_info_.encoder_flow_rate = system_info_.encoder_info.flow_rate;
            spdlog::trace("[AMS HappyHare] Encoder flow rate: {}", system_info_.encoder_flow_rate);
        }
        if (encoder.desired_headroom) {
            system_info_.encoder_info.desired_headroom = *encoder.desired_headroom;
        }
        if (encoder.detection_length) {
            system_info_.encoder_info.detection_length = *encoder.detection_length;
        }
        if (encoder.headroom) {
            system_info_.encoder_info.headroom = *encoder.headroom;
        }
        if (encoder.min_headroom) {
            system_info_.encoder_info.min_headroom = *encoder.min_headroom;
        }
        spdlog::trace("[AMS HappyHare] Encoder: headroom={:.1f}/{:.1f} min={:.1f}",
                      system_info_.encoder_info.headroom,
                      system_info_.encoder_info.detection_length,
                      system_info_.encoder_info.min_headroom);
    }

    if (t.flowguard) {
        const auto& fg = *t.flowguard;
        auto& info = system_info_.flowguard_info;
        if (!fg.buffer_data) {
            info.enabled = false;
        } else if (fg.enabled) {
            info.enabled = *fg.enabled;
        }
        if (fg.active) {
            info.active = *fg.active;
        }
        if (fg.trigger) {
            info.trigger = *fg.trigger;
        }
        if (fg.level) {
            info.level = *fg.level;
        }
        if (fg.max_clog) {
            info.max_clog = *fg.max_clog;
        }
        if (fg.max_tangle) {
            info.max_tangle = *fg.max_tangle;
        }
        if (fg.encoder_mode) {
            flowguard_encoder_mode_ = *fg.encoder_mode;
        }
        spdlog::trace("[AMS HappyHare] Flowguard: enabled={} active={} trigger={} level={:.2f}",
                      info.enabled, info.active, info.trigger, info.level);
    }

    // A null flowguard or encoder is a selected unit without that hardware; the
    // clog meter picks its source on these flags.
    if (t.flowguard_null) {
        system_info_.flowguard_info.enabled = false;
    }
    if (t.encoder_null) {
        system_info_.encoder_info.enabled = false;
    }

    // leds.unit0.exit_effect (v4)
    if (t.led_exit_effect) {
        led_exit_effect_ = *t.led_exit_effect;
        spdlog::trace("[AMS HappyHare] LED exit effect: {}", led_exit_effect_);
    }

    if (t.sync_feedback_flow_rate) {
        system_info_.sync_feedback_flow_rate = *t.sync_feedback_flow_rate;
        spdlog::trace("[AMS HappyHare] Sync feedback flow rate: {:.1f}",
                      system_info_.sync_feedback_flow_rate);
    }

    if (t.toolchange_purge_volume) {
        system_info_.toolchange_purge_volume = *t.toolchange_purge_volume;
        spdlog::trace("[AMS HappyHare] Toolchange purge volume: {:.1f}",
                      system_info_.toolchange_purge_volume);
    }

    if (t.current_toolchange) {
        system_info_.current_toolchange = *t.current_toolchange;
        spdlog::trace("[AMS HappyHare] Toolchange index: {}", system_info_.current_toolchange);
    }

    // The slicer's total, from the metadata the MMU parsed for the print
    if (t.number_of_toolchanges) {
        system_info_.number_of_toolchanges = *t.number_of_toolchanges;
        spdlog::trace("[AMS HappyHare] Total toolchanges from slicer: {}",
                      system_info_.number_of_toolchanges);
    }

    if (t.spoolman_mode) {
        system_info_.spoolman_mode = *t.spoolman_mode;
        spdlog::trace("[AMS HappyHare] Spoolman mode: {}",
                      spoolman_mode_to_string(system_info_.spoolman_mode));
    }

    if (t.pending_spool_id) {
        system_info_.pending_spool_id = *t.pending_spool_id;
        spdlog::trace("[AMS HappyHare] Pending spool ID: {}", system_info_.pending_spool_id);
    }
}

void AmsBackendHappyHare::apply_mmu_sensors_locked(const happy_hare::MmuStatusDelta& delta) {
    // sensors: keys "mmu_pre_gate_X" are the pre-gate sensors per gate; values
    // are true (triggered/filament present), false (not triggered), null
    // (error/unknown)
    if (delta.sensors) {
        const auto& sensors = *delta.sensors;
        bool any_sensor = false;
        toolhead_sensor_fitted_ = sensors.has_toolhead_sensor;
        extruder_sensor_fitted_ = sensors.has_extruder_sensor;

        for (const auto& [gate_idx, triggered] : sensors.pre_gate) {
            auto* gate = gate_sensor_mut(gate_idx);
            if (!gate) {
                continue;
            }
            gate->has_pre_gate_sensor = true;
            gate->pre_gate_triggered = triggered;
            any_sensor = true;

            spdlog::trace("[AMS HappyHare] Pre-gate sensor {}: present=true, triggered={}",
                          gate_idx, gate->pre_gate_triggered);
        }

        // If no per-gate sensors found, check for aggregate format (EMU, and v4
        // while a gate is selected). It reports "mmu_pre_gate" (bool) and
        // "mmu_gear" (bool) for the active gate only.
        if (!any_sensor && !entry_sensor_objects_seen_ && sensors.aggregate_pre_gate) {
            const bool pre_gate_val = *sensors.aggregate_pre_gate;
            // Note: mmu_gear sensor reading is available but not stored — UI only
            // displays pre-gate sensor status. Add to HappyHareGateSensor if needed later.

            // Mark all gates as having sensors, clear stale trigger readings
            // (we only know the current gate's state from aggregate format)
            for (int i = 0; i < slots_.slot_count(); ++i) {
                auto* gate = gate_sensor_mut(i);
                if (gate) {
                    gate->has_pre_gate_sensor = true;
                    gate->pre_gate_triggered = false;
                }
            }

            // Set the current gate's actual reading
            if (system_info_.current_slot >= 0) {
                auto* gate = gate_sensor_mut(system_info_.current_slot);
                if (gate) {
                    gate->pre_gate_triggered = pre_gate_val;
                }
            }

            any_sensor = true;
            spdlog::trace("[AMS HappyHare] Aggregate sensors: pre_gate={}", pre_gate_val);
        }

        // Update has_slot_sensors flag on units based on actual sensor data
        for (auto& unit : system_info_.units) {
            unit.has_slot_sensors = any_sensor || entry_sensor_objects_seen_;
        }
    }

    if (delta.drying) {
        apply_mmu_drying_locked(*delta.drying);
    }

    // The endless-spool ENABLE bit gates apply_endless_spool_backup(), because
    // cmd_MMU_ENDLESS_SPOOL ignores GROUPS while disabled. A future HH that drops
    // both keys leaves the flag at its last value rather than silently flipping
    // to off.
    if (delta.endless_spool_enabled) {
        system_info_.endless_spool_enabled = *delta.endless_spool_enabled;
    }
}

void AmsBackendHappyHare::apply_entry_sensor_objects_locked(
    const std::vector<happy_hare::EntrySensorReading>& readings) {
    bool any = false;
    for (const auto& r : readings) {
        auto* gate = gate_sensor_mut(r.gate);
        if (!gate) {
            continue;
        }
        if (r.detected) {
            gate->object_detected = *r.detected;
        }
        if (r.enabled) {
            gate->object_enabled = *r.enabled;
        }
        gate->has_pre_gate_sensor = true;
        gate->pre_gate_triggered = gate->object_detected && gate->object_enabled;
        any = true;
        spdlog::trace("[AMS HappyHare] Entry sensor {}: detected={} enabled={}", r.gate,
                      gate->object_detected, gate->object_enabled);
    }
    if (any) {
        entry_sensor_objects_seen_ = true;
        for (auto& unit : system_info_.units) {
            unit.has_slot_sensors = true;
        }
    }
}

void AmsBackendHappyHare::apply_mmu_drying_locked(const happy_hare::DryingDelta& drying) {
    if (drying.object) {
        // Traditional object format: {active, current_temp, target_temp, ...}
        const auto& d = *drying.object;
        dryer_info_.supported = true;
        if (d.active) {
            dryer_info_.active = *d.active;
        }
        if (d.current_temp_c) {
            dryer_info_.current_temp_c = *d.current_temp_c;
        }
        if (d.target_temp_c) {
            dryer_info_.target_temp_c = *d.target_temp_c;
        }
        if (d.remaining_min) {
            dryer_info_.remaining_min = *d.remaining_min;
        }
        if (d.duration_min) {
            dryer_info_.duration_min = *d.duration_min;
        }
        if (d.fan_pct) {
            dryer_info_.fan_pct = *d.fan_pct;
        }
        spdlog::trace("[AMS HappyHare] Dryer state (object): active={}, temp={:.1f}°C",
                      dryer_info_.active, dryer_info_.current_temp_c);
    } else if (drying.per_gate) {
        // EMU per-gate array format: ["", "", ...] or ["active", "", ...]
        // Values: "active", "queued" = heater on; "complete", "canceled", "" = off.
        // Presence says the environment manager is loaded, which it always is, so it
        // does NOT imply a heater exists. Only a configured heater sets `supported`.
        bool any_active = false;
        for (const auto& state : *drying.per_gate) {
            if (state == "active" || state == "queued") {
                any_active = true;
            }
        }
        gate_drying_states_ = *drying.per_gate;
        dryer_info_.active = any_active;
        spdlog::trace("[AMS HappyHare] Dryer state (array): active={}", any_active);
    }
}

void AmsBackendHappyHare::converge_mmu_locked(MmuFrame& frame) {
    file_gate_readings_locked(frame);

    // Paint every gate from the lane, after the filing above has put this
    // frame's readings on it. The gate map is the only thing Happy Hare states
    // about a binding, and what it cannot carry - a user's own identity - comes
    // from the lane, so this has to read a model that already holds both. Every
    // gate rather than the ones one key happened to mention: a lane a key is
    // silent about still resolves, and a gate with no records at all resolves
    // to nothing observed and keeps every value the parse set. The clear runs
    // first because the struct persists across frames: without it a field an
    // earlier paint wrote outlives the record that stated it, and the paint
    // below faithfully keeps it there.
    for (int gate = 0; gate < slots_.slot_count(); ++gate) {
        if (auto* entry = slots_.get_mut(gate)) {
            prepare_lane_repaint_locked(gate, entry->info);
            apply_resolved_lane(entry->info, gate);
        }
    }

    // Re-derive every gate's status from the cached gate_status array plus the
    // gate/filament pair this frame may have moved. Unconditional, and last, so
    // no ordering between the three keys can leave a stale stamp behind.
    refresh_gate_statuses_locked();

    apply_fault_edge_locked(frame);
}

void AmsBackendHappyHare::file_gate_readings_locked(MmuFrame& frame) {
    // Gate status is the only thing Happy Hare senses. Everything else in the
    // gate map is mmu_vars.cfg remembering a declaration somebody made once,
    // which is a cache and not a reading, so the two are filed apart.
    //
    // -1 is the MMU saying it does not know, not a gate it found empty, so the
    // record holds no reading rather than an assertion of absence. Reading the
    // cached array rather than SlotInfo::status is what keeps the merged
    // struct out of this: slot_status_from_happy_hare() is the one rule for
    // what a gate_status integer means, and slot_status_reports_filament() the
    // one rule for what a status says about occupancy.
    for (size_t i = 0; i < gate_status_raw_.size(); ++i) {
        // The gate count is fixed by the first gate_status frame, so a longer
        // array later names gates this backend has no slot for. Those are not
        // lanes, and a record on one is a phantom position nothing owns.
        if (!slots_.get(static_cast<int>(i))) {
            continue;
        }
        ams::Observation sensed(ams::ObservationSource::Sensed);
        sensed.present =
            slot_status_reports_filament(slot_status_from_happy_hare(gate_status_raw_[i]));
        ams::ingest(lane_id(static_cast<int>(i)), sensed);

        // An empty gate that now holds filament is an insert. The gate map
        // carries no material or colour the reader read off the spool - those
        // keys are a remembered declaration, not a reading - so the spool_id
        // binding is the only word on what went in: a gate the MMU names keeps
        // its details silently either way (same spool, or the binding verdict
        // swaps them), and an unnamed gate asks. entry->info.status still
        // holds the previous frame's stamp here; refresh_gate_statuses_locked
        // rewrites it after this loop.
        if (sensed.present) {
            const auto* entry = slots_.get(static_cast<int>(i));
            const auto binding = gate_readings_.find(static_cast<int>(i));
            const bool binding_names_spool =
                binding != gate_readings_.end() && binding->second.spoolman_id.value_or(0) > 0;
            if (entry->info.status == SlotStatus::EMPTY && !binding_names_spool) {
                frame.unverified_insert_gates.push_back(static_cast<int>(i));
            }
        }
    }
    for (const auto& [gate, reading] : gate_readings_) {
        // The echo guard: a value repeating the user's own MMU_GATE_MAP write
        // is not a reading, and ingest files the record whole, so a withheld
        // field goes absent rather than back to its pre-edit value. The copy
        // is what gets filed because strip_standing also removes fields the
        // frame was silent about but the accumulator still carries - for an
        // echoed field that standing value is our own write, and filing it
        // would put the abandoned edit back as the machine's word one frame
        // after the echo was withheld. No boundary token: no tag names the
        // spool a gate-map write was made against, so a differing value, a
        // key published empty, or the re-bind verdict ends the suppression.
        ams::Observation stated{ams::ObservationSource::VendorCache};
        ams::Observation cleared{ams::ObservationSource::VendorCache};
        if (const auto it = frame.stated.find(gate); it != frame.stated.end()) {
            stated = it->second;
        }
        if (const auto it = frame.cleared.find(gate); it != frame.cleared.end()) {
            cleared = it->second;
        }
        const ams::Observation judged = stated;
        own_write_echoes_.withhold(gate, std::string{}, stated, cleared);
        ams::Observation filed = reading;
        if (judged.color_rgb && !stated.color_rgb) {
            filed.color_rgb.reset();
        }
        if (judged.material && !stated.material) {
            filed.material.reset();
        }
        own_write_echoes_.strip_standing(gate, filed);
        ams::ingest(lane_id(gate), filed);
    }
}

void AmsBackendHappyHare::apply_fault_edge_locked(const MmuFrame& frame) {
    // After gate and filament_pos, so the marks land where this frame says.
    const bool faulted = !reason_for_pause_.empty();
    if (faulted && !frame.was_faulted) {
        error_segment_ = path_segment_from_happy_hare_pos(filament_pos_);
        if (auto* entry = slots_.get_mut(system_info_.current_slot)) {
            entry->info.error = SlotError{display_reason(reason_for_pause_), SlotError::ERROR};
            spdlog::debug("[AMS HappyHare] Error on slot {}: {}", system_info_.current_slot,
                          reason_for_pause_);
        }
    } else if (!faulted && frame.was_faulted) {
        error_segment_ = PathSegment::NONE;
        for (int i = 0; i < slots_.slot_count(); ++i) {
            if (auto* entry = slots_.get_mut(i)) {
                entry->info.error.reset();
            }
        }
    }
}

void AmsBackendHappyHare::retire_departed_identity_locked(int gate) {
    auto* entry = slots_.get_mut(gate);
    if (!entry) {
        return;
    }
    entry->info.brand.clear();
    entry->info.spool_name.clear();
    entry->info.catalog_id.clear();
    entry->info.product_name.clear();
    entry->info.spoolman_vendor_id = 0;
    entry->info.spoolman_filament_id = 0;
    entry->info.remaining_weight_g = -1.0F;
    entry->info.total_weight_g = -1.0F;
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
    return recovery_actions_locked(/*print_paused=*/true);
}

std::vector<helix::RecoveryAction>
AmsBackendHappyHare::recovery_actions_locked(bool print_paused) const {
    // Caller holds mutex_.
    std::vector<helix::RecoveryAction> actions;

    // Resume after the user clears the fault, primary while a print is paused.
    // A fault outside a print (a failed load or home) has nothing to resume, so
    // Recover leads there. Resuming extrudes on the next move: needs the hotend.
    if (print_paused) {
        actions.push_back({lv_tr("Resume"), "RESUME", "hh::resume", "primary",
                           /*needs_hot_nozzle=*/true});
    }

    // Bare MMU_RECOVER: HH detects the filament position with its own
    // sensors. Our loaded flag reads false for every position HH reports as
    // Unknown, so asserting it would tell HH "unloaded" about filament stuck
    // mid-bowden. State-only, so it stays available on a cold nozzle.
    const bool loaded = system_info_.filament_loaded;
    actions.push_back(
        {lv_tr("Recover"), "MMU_RECOVER", "hh::recover", print_paused ? "" : "primary"});

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

    // Every HH fault goes through its log_error as `!! MMU issue...`: in a print
    // "MMU issue detected. <msg>\nReason: <reason>", outside one (a load or home
    // the user asked for) "MMU issue: <reason>" with no pause at all. A line
    // naming no reason while HH holds a pause is still HH's while it is paused.
    const std::string bare = helix::strip_bang_prefix(raw_line);
    std::string reason;
    if (bare.rfind("MMU issue", 0) == 0) {
        if (const auto at = bare.find("\nReason: "); at != std::string::npos) {
            reason = bare.substr(at + 9);
        } else if (bare.rfind("MMU issue: ", 0) == 0) {
            reason = bare.substr(11);
        } else {
            reason = !reason_for_pause_.empty() ? reason_for_pause_ : bare;
        }
    } else if (ctx.is_paused && !reason_for_pause_.empty()) {
        reason = reason_for_pause_;
    } else {
        return std::nullopt;
    }

    const std::string detail = display_reason(reason);
    return helix::make_ams_fault_event(helix::ErrorSource::HAPPY_HARE,
                                       helix::contains_ci(detail, "runout")
                                           ? lv_tr("Filament runout")
                                           : lv_tr("Filament System Error"),
                                       detail, recovery_actions_locked(ctx.is_paused));
}

bool AmsBackendHappyHare::duplicates_firmware_prompt(const std::string& title) const {
    // HH's _MMU_ERROR_DIALOG, shown for the same pause the recovery popup covers.
    return title == "Happy Hare Error Notice";
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

const HappyHareGateSensor* AmsBackendHappyHare::gate_sensor(int slot_index) const {
    auto it = gate_sensors_.find(slot_index);
    return it != gate_sensors_.end() ? &it->second : nullptr;
}

HappyHareGateSensor* AmsBackendHappyHare::gate_sensor_mut(int slot_index) {
    if (!slots_.is_valid_index(slot_index)) {
        return nullptr;
    }
    return &gate_sensors_[slot_index];
}

void AmsBackendHappyHare::initialize_slots(int gate_count) {
    // A second call (mmu_machine arriving after the first gate_status frame)
    // re-splits the registry and keeps every gate's state, sensors and tool map.
    const bool resplit = slots_.is_initialized();

    // Determine per-unit gate counts:
    // 1. mmu_machine's own units, when they name counts covering every gate
    // 2. per_unit_gate_counts_ from printer.mmu (v3 dissimilar multi-MMU)
    // 3. Fall back to even split (v3 or identical units)
    std::vector<int> unit_counts;
    if (!machine_units_.empty()) {
        int total = 0;
        for (const auto& u : machine_units_) {
            unit_counts.push_back(u.num_gates);
            total += u.num_gates;
        }
        if (total != gate_count ||
            std::any_of(machine_units_.begin(), machine_units_.end(),
                        [](const happy_hare::MachineUnit& u) { return u.num_gates <= 0; })) {
            unit_counts.clear();
        } else {
            num_units_ = static_cast<int>(unit_counts.size());
        }
    }
    if (unit_counts.empty() && !per_unit_gate_counts_.empty() &&
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

    auto unit_name = [this](int u) {
        const std::string display_name = u < static_cast<int>(machine_units_.size())
                                             ? machine_units_[u].display_name
                                             : std::string{};
        return !display_name.empty() ? display_name
               : num_units_ > 1      ? "Unit " + std::to_string(u + 1)
                                     : std::string("MMU");
    };

    // The split mmu_machine names usually matches the one already built; only
    // the topologies can still change then.
    if (resplit && static_cast<int>(system_info_.units.size()) == num_units_) {
        bool same = true;
        for (int u = 0; u < num_units_; ++u) {
            same = same && system_info_.units[u].slot_count == unit_counts[u] &&
                   system_info_.units[u].name == unit_name(u);
        }
        if (same) {
            update_unit_topologies();
            return;
        }
    }

    spdlog::info("[AMS HappyHare] {} {} slots across {} units",
                 resplit ? "Re-splitting" : "Initializing", gate_count, num_units_);

    const bool had_slot_sensors =
        !system_info_.units.empty() && system_info_.units[0].has_slot_sensors;
    system_info_.units.clear();
    if (!resplit) {
        gate_sensors_.clear();
    }

    int global_offset = 0;
    for (int u = 0; u < num_units_; ++u) {
        int unit_gates = unit_counts[u];

        AmsUnit unit;
        unit.unit_index = u;
        unit.name = unit_name(u);
        unit.slot_count = unit_gates;
        unit.first_slot_global_index = global_offset;
        unit.connected = true;
        unit.has_encoder = unit_supports_locked(u, happy_hare::UnitFeature::Encoder);
        unit.has_toolhead_sensor = true;
        unit.topology = unit_is_type_b_locked(u) ? PathTopology::HUB : PathTopology::LINEAR;
        // has_slot_sensors starts false; updated when sensor data arrives in
        // apply_mmu_sensors_locked()
        unit.has_slot_sensors = resplit && had_slot_sensors;
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
    if (!resplit) {
        system_info_.tool_to_slot_map.clear();
        system_info_.tool_to_slot_map.reserve(gate_count);
        for (int i = 0; i < gate_count; ++i) {
            system_info_.tool_to_slot_map.push_back(i);
        }
    }

    // Initialize SlotRegistry alongside legacy state (uses same unit_counts).
    // Slots are named by global gate number, which is what reorganize() keys
    // the preserved entries on.
    std::vector<std::pair<std::string, std::vector<std::string>>> sr_units;
    int sr_offset = 0;
    for (int u = 0; u < num_units_; ++u) {
        int count = unit_counts[u];
        std::vector<std::string> names;
        for (int g = 0; g < count; ++g) {
            names.push_back(std::to_string(sr_offset + g));
        }
        sr_units.push_back({system_info_.units[u].name, names});
        sr_offset += count;
    }
    if (resplit) {
        slots_.reorganize(sr_units);
    } else {
        slots_.initialize_units(sr_units);
    }
    // filament_pos may have arrived before these units existed.
    refresh_hub_sensors_locked();
}

void AmsBackendHappyHare::apply_tip_method_config(const nlohmann::json& settings,
                                                  const happy_hare::MachineLayout& layout) {
    // form_tip_macro decides the tip method the way Happy Hare does internally: a macro
    // name containing "cut" (e.g., _MMU_CUT_TIP) is a cutter system, anything else
    // (e.g., _MMU_FORM_TIP) is tip-forming.
    if (!layout.v4 && (!settings.contains("mmu") || !settings["mmu"].is_object())) {
        spdlog::debug("[AMS HappyHare] No mmu section in configfile settings");
        return;
    }

    TipMethod method = TipMethod::NONE;
    const nlohmann::json* macro_value =
        happy_hare::find_config_param(settings, layout, "form_tip_macro");

    if (macro_value && macro_value->is_string()) {
        std::string macro = macro_value->get<std::string>();

        // Convert to lowercase for comparison (same as Happy Hare)
        std::string lower_macro = helix::text_io::to_lower(macro);

        if (lower_macro.find("cut") != std::string::npos) {
            method = TipMethod::CUT;
        } else {
            method = TipMethod::TIP_FORM;
        }

        spdlog::info("[AMS HappyHare] Tip method from config: {} (form_tip_macro={})",
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
}

void AmsBackendHappyHare::apply_selector_type_config(const nlohmann::json& settings,
                                                     const nlohmann::json& live_mmu_machine) {
    // VirtualSelector = Type B (hub topology: 3MS, Box Turtle, Night Owl, Angry Beaver)
    // LinearSelector/RotarySelector/ServoSelector = Type A (linear: ERCF, Tradrack)
    // Each unit names its own, so a mixed rig gets one topology per unit.
    auto units = happy_hare::read_machine_units(settings, live_mmu_machine);
    if (units.empty()) {
        spdlog::debug("[AMS HappyHare] No mmu_machine fields for selector type");
        return;
    }
    for (size_t u = 0; u < units.size(); ++u) {
        spdlog::info("[AMS HappyHare] Unit {}: selector {}, gates {}+{} '{}'", u,
                     units[u].selector_type, units[u].first_gate, units[u].num_gates,
                     units[u].display_name);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!units[0].selector_type.empty()) {
            selector_type_ = units[0].selector_type;
        }
        machine_units_ = std::move(units);
        if (slots_.is_initialized()) {
            // The first gate_status frame usually lands before this answer, and
            // the machine's own split is the one to show.
            initialize_slots(slots_.slot_count());
        } else {
            update_unit_topologies();
        }
    }

    emit_event(EVENT_STATE_CHANGED);
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

void AmsBackendHappyHare::apply_heater_config(const nlohmann::json& settings,
                                              const nlohmann::json& live_mmu_machine,
                                              const happy_hare::MachineLayout& layout) {
    // Enclosure heaters and environment sensors, each either one shared object
    // (filament_heater / environment_sensor) or one per gate (filament_heaters /
    // environment_sensors). A multi-unit rig whose units differ is read as one
    // entry per gate across every unit; get_system_info() maps each unit to its own.
    const auto units = happy_hare::read_machine_units(settings, live_mmu_machine);
    const auto heaters =
        happy_hare::collect_unit_objects(units, happy_hare::UnitObjectKind::Heater);
    const auto sensors =
        happy_hare::collect_unit_objects(units, happy_hare::UnitObjectKind::EnvironmentSensor);
    const bool any_per_gate_heater = std::any_of(heaters.per_gate.begin(), heaters.per_gate.end(),
                                                 [](const std::string& h) { return !h.empty(); });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!heaters.shared.empty() || any_per_gate_heater) {
            filament_heater_name_ = heaters.shared;
            filament_heaters_ = any_per_gate_heater ? heaters.per_gate : std::vector<std::string>{};
            // A named heater is what a dryer IS, on the shared-enclosure form exactly as
            // on the per-gate one.
            dryer_info_.supported = true;
            // MMU_HEATER TEMP=<t> with DRY unset re-sends the setpoint and updates the
            // running cycle's tracked target. TIMER is read only on the DRY=1 path, and
            // DRY=1 during a cycle is refused, so the duration needs a stop and restart.
            dryer_info_.supports_live_temp = true;
            dryer_info_.supports_live_duration = false;
            spdlog::info("[AMS HappyHare] Filament heater: '{}', per-gate heaters: {}",
                         filament_heater_name_, filament_heaters_.size());
        }
        if (!sensors.shared.empty()) {
            environment_sensor_name_ = sensors.shared;
            spdlog::info("[AMS HappyHare] Environment sensor: {}", environment_sensor_name_);
        }
        if (std::any_of(sensors.per_gate.begin(), sensors.per_gate.end(),
                        [](const std::string& n) { return !n.empty(); })) {
            environment_sensors_ = sensors.per_gate;
            spdlog::info("[AMS HappyHare] Per-gate environment sensors: {}",
                         environment_sensors_.size());
        }
    }

    // heater_max_temp: [mmu] on v3, the unit's parameters on v4. A number or a
    // numeric string.
    if (const nlohmann::json* v =
            happy_hare::find_config_param(settings, layout, "heater_max_temp")) {
        if (const auto max_temp = happy_hare::read_config_number(v)) {
            std::lock_guard<std::mutex> lock(mutex_);
            // A ceiling, not a capability: Klipper reports configured defaults, so
            // this key is present whether or not a heater is fitted.
            dryer_info_.max_temp_c = *max_temp;
            spdlog::info("[AMS HappyHare] Heater max temp: {:.1f}°C", *max_temp);
        } else {
            spdlog::warn("[AMS HappyHare] Could not parse heater_max_temp");
        }
    }
}

// ============================================================================
// Config Defaults Query
// ============================================================================

void AmsBackendHappyHare::apply_config_defaults(const nlohmann::json& settings,
                                                const happy_hare::MachineLayout& layout) {
    // Initial values of speeds and distances. These serve as defaults until the
    // user overrides them via the UI.
    if (!layout.v4 && (!settings.contains("mmu") || !settings["mmu"].is_object())) {
        spdlog::debug("[AMS HappyHare] No mmu section in configfile for defaults");
        return;
    }

    auto parse_float = [&](const char* key, float& out) {
        if (const auto v = happy_hare::read_config_number(
                happy_hare::find_config_param(settings, layout, key))) {
            out = *v;
        }
    };

    auto parse_int = [&](const char* key, int& out) {
        if (const auto v = happy_hare::read_config_number(
                happy_hare::find_config_param(settings, layout, key))) {
            out = static_cast<int>(*v);
        }
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);

        parse_float("gear_from_buffer_speed", config_defaults_.gear_from_buffer_speed);
        parse_float("gear_from_spool_speed", config_defaults_.gear_from_spool_speed);
        parse_float("gear_unload_speed", config_defaults_.gear_unload_speed);
        parse_float("selector_move_speed", config_defaults_.selector_move_speed);
        parse_float("extruder_load_speed", config_defaults_.extruder_load_speed);
        parse_float("extruder_unload_speed", config_defaults_.extruder_unload_speed);
        parse_float("toolhead_sensor_to_nozzle", config_defaults_.toolhead_sensor_to_nozzle);
        parse_float("toolhead_extruder_to_nozzle", config_defaults_.toolhead_extruder_to_nozzle);
        parse_float("toolhead_entry_to_extruder", config_defaults_.toolhead_entry_to_extruder);
        parse_float("toolhead_ooze_reduction", config_defaults_.toolhead_ooze_reduction);
        parse_int("sync_to_extruder", config_defaults_.sync_to_extruder);
        parse_int("clog_detection", config_defaults_.clog_detection);
        config_defaults_.detection_length = happy_hare::read_config_number(
            happy_hare::find_config_param(settings, layout, "detection_length"));

        config_defaults_.loaded = true;

        spdlog::info("[AMS HappyHare] Config defaults loaded: "
                     "gear_buf={}, gear_spool={}, gear_unload={}, "
                     "ext_load={}, ext_unload={}, "
                     "sensor_to_nozzle={}, extruder_to_nozzle={}",
                     config_defaults_.gear_from_buffer_speed,
                     config_defaults_.gear_from_spool_speed, config_defaults_.gear_unload_speed,
                     config_defaults_.extruder_load_speed, config_defaults_.extruder_unload_speed,
                     config_defaults_.toolhead_sensor_to_nozzle,
                     config_defaults_.toolhead_extruder_to_nozzle);
    }

    load_persisted_overrides();
    reapply_overrides();
}

void AmsBackendHappyHare::query_config_from_printer() {
    if (!client_) {
        return;
    }

    // One query feeds every connect-time config reader. The live mmu_machine object comes
    // along because v4 keeps selector_type, filament_heater and environment_sensor there,
    // per unit, and leaves configfile carrying only the version.
    nlohmann::json params = {
        {"objects", nlohmann::json::object(
                        {{"configfile", {"settings"}}, {"mmu_machine", nlohmann::json(nullptr)}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // L081 Mechanism C: defer member access (system_info_, selector_type_,
            // dryer_info_, config_defaults_, emit_event) to main thread.
            token.defer("AmsBackendHappyHare::config_apply", [this,
                                                              response = std::move(response)]() {
                // `response` is const in this non-mutable lambda, so operator[] resolves to
                // the const overload: on a missing key that is a live assert(), an
                // uncatchable SIGABRT. Guard every level before indexing.
                if (!response.contains("result") || !response["result"].contains("status")) {
                    spdlog::warn("[AMS HappyHare] configfile query returned no status");
                    return;
                }
                const auto& status = response["result"]["status"];
                if (!status.contains("configfile") || !status["configfile"].contains("settings") ||
                    !status["configfile"]["settings"].is_object()) {
                    spdlog::warn("[AMS HappyHare] configfile settings unavailable");
                    return;
                }

                const auto& settings = status["configfile"]["settings"];
                const nlohmann::json* live_mm = &hh_empty_object();
                if (status.contains("mmu_machine")) {
                    live_mm = &status["mmu_machine"];
                }

                const happy_hare::MachineLayout layout =
                    happy_hare::read_machine_layout(settings, *live_mm);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    machine_layout_ = layout;
                    system_info_.version = layout.version;
                    apply_bypass_support_locked();
                }
                spdlog::info("[AMS HappyHare] Happy Hare {} ({} layout)",
                             layout.version.empty() ? "version unknown" : layout.version,
                             layout.v4 ? "v4" : "v3");

                apply_tip_method_config(settings, layout);
                apply_selector_type_config(settings, *live_mm);
                apply_heater_config(settings, *live_mm, layout);
                emit_event(EVENT_STATE_CHANGED);
                apply_config_defaults(settings, layout);
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS HappyHare] Failed to query configfile: {}", err.message);
        });
}

void AmsBackendHappyHare::load_persisted_overrides() {
    auto* config = helix::Config::get_instance();
    // An override saved against the built-in default, not the printer's own,
    // still applies: the built-ins stood in whenever the printer's config
    // could not be read. Its record is re-pointed at the real default.
    const ConfigDefaults builtin;
    bool migrated = false;

    // Helper: load an override if its saved config_default still matches.
    auto load = [&](const std::string& key, auto& field, auto current_default,
                    auto builtin_default) {
        using T = decltype(current_default);
        std::string base = "/hh_overrides/" + key;
        // Records saved under the action id selector_speed load too.
        if (!config->exists(base + "/value") && key == "selector_move_speed") {
            base = "/hh_overrides/selector_speed";
        }
        if (!config->exists(base + "/value"))
            return;
        const T saved_default = config->get<T>(base + "/config_default", T(-999));
        auto same = [](T a, T b) { return std::abs(static_cast<float>(a - b)) < 0.01f; };
        if (!same(saved_default, current_default)) {
            if (!same(saved_default, builtin_default)) {
                spdlog::info("[AMS HappyHare] Dropping stale override {} "
                             "(config changed: {} -> {})",
                             key, saved_default, current_default);
                return;
            }
            spdlog::info("[AMS HappyHare] Keeping override {} saved against the built-in "
                         "default {} (printer's default {})",
                         key, saved_default, current_default);
            config->set<T>(base + "/config_default", current_default);
            migrated = true;
        }
        const nlohmann::json* value = config->try_get_json(base + "/value");
        if (!value || !(value->is_number() || value->is_boolean())) {
            spdlog::warn("[AMS HappyHare] Failed to load override {}", key);
            return;
        }
        field = value->get<T>();
        spdlog::debug("[AMS HappyHare] Loaded override {}: {}", key, *field);
    };

    // clang-format off
    load("gear_from_buffer_speed", user_overrides_.gear_from_buffer_speed, config_defaults_.gear_from_buffer_speed, builtin.gear_from_buffer_speed);
    load("gear_from_spool_speed", user_overrides_.gear_from_spool_speed, config_defaults_.gear_from_spool_speed, builtin.gear_from_spool_speed);
    load("gear_unload_speed", user_overrides_.gear_unload_speed, config_defaults_.gear_unload_speed, builtin.gear_unload_speed);
    load("selector_move_speed", user_overrides_.selector_move_speed, config_defaults_.selector_move_speed, builtin.selector_move_speed);
    load("extruder_load_speed", user_overrides_.extruder_load_speed, config_defaults_.extruder_load_speed, builtin.extruder_load_speed);
    load("extruder_unload_speed", user_overrides_.extruder_unload_speed, config_defaults_.extruder_unload_speed, builtin.extruder_unload_speed);
    load("toolhead_sensor_to_nozzle", user_overrides_.toolhead_sensor_to_nozzle, config_defaults_.toolhead_sensor_to_nozzle, builtin.toolhead_sensor_to_nozzle);
    load("toolhead_extruder_to_nozzle", user_overrides_.toolhead_extruder_to_nozzle, config_defaults_.toolhead_extruder_to_nozzle, builtin.toolhead_extruder_to_nozzle);
    load("toolhead_entry_to_extruder", user_overrides_.toolhead_entry_to_extruder, config_defaults_.toolhead_entry_to_extruder, builtin.toolhead_entry_to_extruder);
    load("toolhead_ooze_reduction", user_overrides_.toolhead_ooze_reduction, config_defaults_.toolhead_ooze_reduction, builtin.toolhead_ooze_reduction);
    load("sync_to_extruder", user_overrides_.sync_to_extruder, config_defaults_.sync_to_extruder, builtin.sync_to_extruder);
    load("clog_detection", user_overrides_.clog_detection, config_defaults_.clog_detection, builtin.clog_detection);
    // clang-format on

    if (migrated) {
        config->save();
    }
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
    std::string base = "/hh_overrides/" + key;
    config->set<int>(base + "/value", value);
    config->set<int>(base + "/config_default", get_config_default_int(key));
    config->save();
}

std::string AmsBackendHappyHare::test_config_param_locked(std::string_view key) const {
    return helix::text_io::to_upper(happy_hare::param_name(key, machine_layout_));
}

void AmsBackendHappyHare::reapply_overrides() {
    // Every active override with its value as MMU_TEST_CONFIG spells it.
    std::vector<std::pair<const char*, std::string>> values;
    auto add_float = [&](const char* key, const std::optional<float>& val, bool integer_fmt) {
        if (val) {
            values.emplace_back(key, integer_fmt ? fmt::format("{:.0f}", *val)
                                                 : fmt::format("{:.1f}", *val));
        }
    };
    auto add_int = [&](const char* key, const std::optional<int>& val) {
        if (val) {
            values.emplace_back(key, std::to_string(*val));
        }
    };

    std::vector<std::string> commands;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Speed sliders (integer format)
        add_float("gear_from_buffer_speed", user_overrides_.gear_from_buffer_speed, true);
        add_float("gear_from_spool_speed", user_overrides_.gear_from_spool_speed, true);
        add_float("gear_unload_speed", user_overrides_.gear_unload_speed, true);
        add_float("selector_move_speed", user_overrides_.selector_move_speed, true);
        add_float("extruder_load_speed", user_overrides_.extruder_load_speed, true);
        add_float("extruder_unload_speed", user_overrides_.extruder_unload_speed, true);
        // Toolhead distances (one decimal)
        add_float("toolhead_sensor_to_nozzle", user_overrides_.toolhead_sensor_to_nozzle, false);
        add_float("toolhead_extruder_to_nozzle", user_overrides_.toolhead_extruder_to_nozzle,
                  false);
        add_float("toolhead_entry_to_extruder", user_overrides_.toolhead_entry_to_extruder, false);
        add_float("toolhead_ooze_reduction", user_overrides_.toolhead_ooze_reduction, false);
        // Int toggles
        add_int("sync_to_extruder", user_overrides_.sync_to_extruder);
        add_int("clog_detection", user_overrides_.clog_detection);

        // Each override goes alone, to the unit that takes it: v3 refuses a
        // whole MMU_TEST_CONFIG over one name it does not know, and v4 applies
        // the rest but answers with an error. One no unit takes is left out.
        for (const auto& [key, value] : values) {
            if (auto cmd = test_config_command_locked(key, value)) {
                commands.push_back(std::move(*cmd));
            } else {
                spdlog::info("[AMS HappyHare] Not re-applying {}: no unit takes it", key);
            }
        }
    }

    for (const auto& cmd : commands) {
        spdlog::info("[AMS HappyHare] Re-applying override: {}", cmd);
        execute_gcode(cmd);
    }
}

// ============================================================================
// Filament Operations
// ============================================================================

// check_preconditions() provided by AmsSubscriptionBackend

// execute_gcode() provided by AmsSubscriptionBackend

AmsError AmsBackendHappyHare::do_load_filament(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError gate_valid = validate_slot_index_locked(slot_index);
        if (!gate_valid) {
            return gate_valid;
        }

        // Check if slot has filament available
        const auto* entry = slots_.get(slot_index);
        if (entry && entry->info.status == SlotStatus::EMPTY) {
            return AmsErrorHelper::slot_not_available(lane_noun(), slot_index);
        }
    }

    // Send MMU_LOAD GATE={n} command (Happy Hare uses "gate" in its API)
    const std::string cmd = fmt::format("MMU_LOAD GATE={}", slot_index);

    spdlog::info("[AMS HappyHare] Loading from slot {}", slot_index);
    return dispatch_filament_op(cmd);
}

AmsError AmsBackendHappyHare::do_unload_filament(int /*slot_index*/) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!system_info_.filament_loaded) {
            return AmsErrorHelper::not_loaded();
        }
    }

    spdlog::info("[AMS HappyHare] Unloading filament");
    return dispatch_filament_op("MMU_UNLOAD");
}

AmsError AmsBackendHappyHare::dispatch_filament_op(std::string cmd) {
    // Happy Hare publishes nothing for a command it refuses (paused, disabled,
    // bypass selected, gate mismatch) or for a pre-op G28 that fails, so the
    // gcode error is the only end such an operation gets.
    return ensure_homed_then(
        std::move(cmd), nullptr,
        [this](const MoonrakerError& err) {
            spdlog::error("[AMS HappyHare] Filament operation failed: {}", err.message);
            emit_event(EVENT_ERROR, err.message);
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS, /*skip_homing=*/false, /*silent=*/true,
        // The callback above only logs, so Klipper's `!!` broadcast stays the
        // thing that shows the user why.
        /*caller_surfaces_errors=*/false);
}

AmsError AmsBackendHappyHare::do_select_slot(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError gate_valid = validate_slot_index_locked(slot_index);
        if (!gate_valid) {
            return gate_valid;
        }
    }

    // Send MMU_SELECT GATE={n} command (Happy Hare uses "gate" in its API)
    const std::string cmd = fmt::format("MMU_SELECT GATE={}", slot_index);

    spdlog::info("[AMS HappyHare] Selecting slot {}", slot_index);
    return execute_gcode(cmd);
}

AmsError AmsBackendHappyHare::do_change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tool_number < 0 ||
            tool_number >= static_cast<int>(system_info_.tool_to_slot_map.size())) {
            return AmsErrorHelper::tool_out_of_range(tool_number);
        }
    }

    // Send T{n} command for standard tool change
    const std::string cmd = fmt::format("T{}", tool_number);

    spdlog::info("[AMS HappyHare] Tool change to T{}", tool_number);
    return dispatch_filament_op(cmd);
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
    std::string cmd = "MMU_HOME";
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }
        cmd += unit_suffix_locked(kAllUnits);
    }

    // Happy Hare uses MMU_HOME to reset to a known state
    spdlog::info("[AMS HappyHare] Resetting (homing selector)");
    return execute_gcode(cmd);
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
            AmsError slot_err = validate_slot_index_locked(slot_index);
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

std::string AmsBackendHappyHare::build_recover_command(const RecoverStateRequest& request) {
    std::string cmd = "MMU_RECOVER";
    if (request.bypass) {
        cmd += " BYPASS=1";
    } else if (request.slot >= 0) {
        cmd += " GATE=" + std::to_string(request.slot);
    }
    if (request.loaded.has_value()) {
        cmd += *request.loaded ? " LOADED=1" : " LOADED=0";
    }
    return cmd;
}

// State-only like clear_fault(): MMU_RECOVER moves nothing, so it is allowed
// while busy or printing, which is when a confused MMU needs correcting.
AmsError AmsBackendHappyHare::recover_with_state(const RecoverStateRequest& request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }
        if (request.bypass) {
            if (!system_info_.supports_bypass) {
                return AmsErrorHelper::not_supported("Bypass");
            }
        } else if (request.slot >= 0) {
            AmsError slot_err = validate_slot_index_locked(request.slot);
            if (!slot_err) {
                return slot_err;
            }
        }
    }

    const std::string cmd = build_recover_command(request);
    spdlog::info("[AMS HappyHare] Recovering with asserted state: {}", cmd);
    return execute_gcode(cmd);
}

AmsError AmsBackendHappyHare::preload_lane(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Happy Hare's MMU_PRELOAD refuses while printing, so ours does too.
        AmsError precondition = check_preconditions(/*requires_toolhead_motion=*/true);
        if (!precondition) {
            return precondition;
        }

        AmsError slot_err = validate_slot_index_locked(slot_index);
        if (!slot_err) {
            return slot_err;
        }
    }

    spdlog::info("[AMS HappyHare] Preloading gate {}", slot_index);
    return execute_gcode("MMU_PRELOAD GATE=" + std::to_string(slot_index));
}

AmsError AmsBackendHappyHare::eject_lane(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        AmsError slot_err = validate_slot_index_locked(slot_index);
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

        AmsError slot_err = validate_slot_index_locked(slot_index);
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

        AmsError slot_err = validate_slot_index_locked(slot_index);
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

void AmsBackendHappyHare::persist_override(int slot_index, const SlotInfo& info,
                                           const helix::ams::Observation& declared) {
    // Callers hold mutex_.
    const helix::ams::FilamentSlotOverride o =
        helix::ams::stage_user_override(overrides_, slot_index, info, declared);

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
        helix::printer::SlotEntry* entry = slots_.get_mut(slot_index);
        clear_override_locked(slot_index, entry ? &entry->info : nullptr);
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

void AmsBackendHappyHare::write_gate_locked(int slot_index, SlotInfo& slot, const SlotInfo& info) {
    const int old_mapped_tool = slot.mapped_tool;
    const bool changed = slot.assign_filament_fields(info) || info.mapped_tool != old_mapped_tool;
    // Tool mapping change goes through registry so reverse maps stay consistent.
    if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0) {
        slots_.set_tool_mapping(slot_index, info.mapped_tool);
    }

    if (changed) {
        spdlog::info("[AMS HappyHare] Updated slot {} info: {} {}", slot_index, info.material,
                     info.color_name);
    }
}

namespace {
/// The Clear Spool funnel hands the backend a slot with nothing on it: no
/// material, no declarable colour, no identity text, no spool link. Only that
/// shape takes the full-wipe path below - an editor commit always carries the
/// whole slot, so a kept value arrives non-empty and stays incremental.
bool is_full_clear(const SlotInfo& info) {
    return info.spoolman_id == 0 && !info.has_filament_info() && info.brand.empty() &&
           info.spool_name.empty();
}

/// Spoolman pull mode refuses local gate-map writes by logging the refusal
/// only, so every refusal path reports it here instead: a partial failure
/// (HelixScreen's own layer is already written) naming Spoolman as the owner.
AmsError pull_mode_refusal(const char* title, const char* message) {
    AmsError refused(AmsResult::COMMAND_FAILED, "Happy Hare Spoolman pull mode owns the gate map",
                     lv_tr(title), lv_tr(message));
    refused.partially_applied = true;
    return refused;
}
} // namespace

AmsError AmsBackendHappyHare::apply_user_edit(int slot_index, const SlotInfo& info,
                                              const helix::ams::Observation& declared) {
    int old_spoolman_id = 0;
    int old_mapped_tool = -1;
    bool old_had_identity = false;
    std::string old_material;
    uint32_t old_color_rgb = AMS_DEFAULT_SLOT_COLOR;
    SpoolmanMode spoolman_mode = SpoolmanMode::OFF;
    int current_slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, slots_.slot_count() - 1);
        }

        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, slots_.slot_count() - 1);
        }

        // Capture old values BEFORE updating (needed to detect clears / remaps)
        old_spoolman_id = entry->info.spoolman_id;
        old_mapped_tool = entry->info.mapped_tool;
        old_had_identity = old_spoolman_id > 0 || entry->info.has_filament_info() ||
                           !entry->info.brand.empty() || !entry->info.spool_name.empty();
        old_material = entry->info.material;
        old_color_rgb = entry->info.color_rgb;
        spoolman_mode = system_info_.spoolman_mode;
        current_slot = system_info_.current_slot;
        write_gate_locked(slot_index, entry->info, info);

        // Record the user's identity in the override store: the gate map cannot
        // hold brand / spool_name / total weight / colour name at all.
        persist_override(slot_index, info, declared);
    }

    // Set when the material could not be expressed as a G-code parameter. Reported
    // after every other write has gone out, so a name the gate map cannot store costs
    // the user only the material rather than the whole save — but is never silent.
    std::string rejected_material;

    // Record our own id write so Rule 1 does not read the in-flight
    // frames (still reporting old_spoolman_id until the echo lands) as
    // an external re-bind. An unlink (SPOOLID=-1) erases the pending
    // expectation instead. The command build runs OUTSIDE mutex_ -
    // take the lock just for the record, matching every other writer.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        record_own_spool_write(slot_index, info.spoolman_id, old_spoolman_id);
    }

    // Clear Spool: wipe everything the gate map holds for this gate. Happy
    // Hare keeps omitted params at their current value, so each writable field
    // is named with an explicit empty value - the only form that empties
    // MATERIAL, COLOR, NAME and VENDOR (v2/v3 ignore params they do not fetch;
    // VENDOR is v4-only). Never RESET: on v2/v3 it ignores GATE and wipes
    // every gate. Never TEMP=0 (falsy means "keep") or AVAILABLE=0 (that
    // marks the gate EMPTY, not unknown).
    if (is_full_clear(info) && old_had_identity) {
        // The gate is being emptied deliberately, so a frame restating the
        // edit's values afterwards is the machine's own reading, not an echo
        // to hide - the guard from an earlier edit must not outlive it.
        // Under the lock: the parse mutates the same map under mutex_.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            own_write_echoes_.abandon(slot_index);
        }

        // Spoolman pull mode: Happy Hare refuses local writes to material,
        // colour, name, vendor and spool id, and logs the refusal rather than
        // returning it - the gate map belongs to Spoolman on this printer.
        if (spoolman_mode == SpoolmanMode::PULL) {
            spdlog::warn("[AMS HappyHare] Spoolman pull mode owns the gate map; gate {} "
                         "cleared locally only",
                         slot_index);
            emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
            return pull_mode_refusal("Couldn't clear the printer's gate map",
                                     "This printer fills its gates from Spoolman. HelixScreen "
                                     "cleared its own copy; remove the spool in Spoolman.");
        }

        // A job on this gate: the print UI refuses clears while a job holds
        // the machine, so one reaching here is a backstop - rewriting the gate
        // map under a running print is the one thing that must not happen.
        if (api_ && slot_index == current_slot &&
            job_holds_machine(api_->printer_state().print_state().get_print_lifecycle())) {
            spdlog::warn("[AMS HappyHare] Clear of gate {} reached us mid-print; firmware "
                         "write skipped",
                         slot_index);
            emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
            AmsError skipped(AmsResult::COMMAND_FAILED,
                             "A running print is using this gate; firmware write skipped",
                             lv_tr("Couldn't clear the printer's gate map"),
                             lv_tr("A print is running on this gate. HelixScreen cleared its "
                                   "own copy; clear it again once the print finishes."));
            skipped.partially_applied = true;
            return skipped;
        }

        const std::string wipe = fmt::format(
            "MMU_GATE_MAP GATE={} MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1", slot_index);
        execute_gcode(wipe);
        spdlog::debug("[AMS HappyHare] Sent: {}", wipe);

        // Tool-to-gate mapping is a separate Happy Hare concern from
        // MMU_GATE_MAP (which is filament metadata).
        if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0) {
            execute_gcode(fmt::format("MMU_TTG_MAP TOOL={} GATE={}", info.mapped_tool, slot_index));
        }

        emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
        return AmsErrorHelper::success();
    }

    // Persist via MMU_GATE_MAP command (Happy Hare stores in mmu_vars.cfg automatically).
    bool has_changes = false;
    std::string cmd = fmt::format("MMU_GATE_MAP GATE={}", slot_index);

    // Color (hex format, no # prefix). A deliberate pure black (#000000)
    // reaches the gate map; the "no color reading" sentinel does not. A
    // sentinel where the gate map held a colour is a clear, and an omitted
    // parameter keeps the current value - the explicit empty is the only form
    // that empties it (the same rule the full-wipe command relies on).
    if (ams::is_declarable_color(info.color_rgb)) {
        cmd += fmt::format(" COLOR={:06X}", info.color_rgb & 0xFFFFFF);
        has_changes = true;
    } else if (ams::is_declarable_color(old_color_rgb)) {
        cmd += " COLOR=";
        has_changes = true;
    }

    // Material (validate to prevent command injection). The material charset is
    // deliberately wider than an identifier's: `PLA+`, `PA6-CF` and `Silk PLA` are
    // all in our own filament database, and gating this on is_safe_gcode_param()
    // dropped every one of them. An empty where the gate map held a material is
    // a clear and needs the explicit empty for the same reason as the colour.
    if (!info.material.empty() && IMoonrakerAPI::is_safe_material_param(info.material)) {
        cmd += fmt::format(" MATERIAL={}", IMoonrakerAPI::gcode_param_value(info.material));
        has_changes = true;
    } else if (!info.material.empty()) {
        spdlog::warn("[AMS HappyHare] Skipping MATERIAL - unsafe characters in: {}", info.material);
        rejected_material = info.material;
    } else if (!old_material.empty()) {
        cmd += " MATERIAL=";
        has_changes = true;
    }

    // Spoolman ID (-1 to clear)
    if (info.spoolman_id > 0) {
        cmd += fmt::format(" SPOOLID={}", info.spoolman_id);
        has_changes = true;
    } else if (info.spoolman_id == 0 && old_spoolman_id > 0) {
        cmd += " SPOOLID=-1"; // Clear existing link
        has_changes = true;
    }

    // Only send command if there are actual changes to persist. Spoolman pull
    // mode refuses local writes to every field this command carries and logs
    // the refusal rather than returning it - send nothing and report the
    // partial failure below instead of claiming success. Tool-to-gate remaps
    // are not gate-map fields and still go out.
    const bool pull_owns_gate_map = spoolman_mode == SpoolmanMode::PULL;
    if (has_changes && pull_owns_gate_map) {
        spdlog::warn("[AMS HappyHare] Spoolman pull mode owns the gate map; gate {} "
                     "edit kept locally only",
                     slot_index);
        // No write goes out, so no echo is coming: the gate map's next frame
        // is Spoolman's word, not ours.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            own_write_echoes_.abandon(slot_index);
        }
    } else if (has_changes) {
        // Remember what the user declared, pruned to the fields this command
        // actually carries, so the parse can tell the gate map repeating their
        // choice back from Happy Hare's own readings. Staged before the
        // dispatch: the guard has to be standing before any echo can arrive.
        // A dispatch that failed outright leaves it armed to self-clean the
        // same way - firmware still holds a value the declaration disagrees
        // with. Under the lock: the parse mutates the same map under mutex_.
        // The dispatch itself stays outside it, as every other writer here.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            own_write_echoes_.stage(slot_index, declared);
            if (auto* staged = own_write_echoes_.staged(slot_index)) {
                // COLOR= is skipped for the no-colour sentinel and MATERIAL= for
                // an unsafe or cleared name: a field the write omitted (or asked
                // firmware to drop) is the gate map's to keep, so its echo is a
                // reading.
                if (!ams::is_declarable_color(info.color_rgb)) {
                    staged->color_rgb.reset();
                }
                if (info.material.empty() ||
                    !IMoonrakerAPI::is_safe_material_param(info.material)) {
                    staged->material.reset();
                }
                // The command carries no name, brand, colour name or product
                // line, so any value firmware reports for them is its own. An
                // inert field left declared would keep the entry alive after the
                // real fields are all released.
                staged->brand.reset();
                staged->spool_name.reset();
                staged->color_name.reset();
                staged->product_name.reset();
            }
            // No boundary token: no tag names the spool a gate-map write was made
            // against, so suppression ends on a differing value or a key
            // published empty rather than on a boundary event.
            own_write_echoes_.arm(slot_index, std::string{});
        }

        execute_gcode(cmd);
        spdlog::debug("[AMS HappyHare] Sent: {}", cmd);
    }

    // Tool-to-gate mapping is a separate Happy Hare concern from MMU_GATE_MAP
    // (which is filament metadata). Emit MMU_TTG_MAP whenever the slot edit
    // path changes mapped_tool — mirrors set_tool_mapping() for the modal flow.
    if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0) {
        execute_gcode(fmt::format("MMU_TTG_MAP TOOL={} GATE={}", info.mapped_tool, slot_index));
    }

    // Emit OUTSIDE the lock to avoid deadlock with callbacks
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));

    if (has_changes && pull_owns_gate_map) {
        return pull_mode_refusal("Couldn't save the printer's gate map",
                                 "This printer fills its gates from Spoolman. Make colour, "
                                 "material and spool changes in Spoolman.");
    }

    if (!rejected_material.empty()) {
        AmsError partial(AmsResult::COMMAND_FAILED,
                         "Material '" + rejected_material +
                             "' contains characters that cannot be "
                             "sent as a G-code parameter",
                         lv_tr("Couldn't save the material name"),
                         lv_tr("Everything else was saved. Rename the material using letters, "
                               "digits, spaces, and + - _ . ( ) /"));
        // Every other write above has already gone out, the spool id included.
        partial.partially_applied = true;
        return partial;
    }

    return AmsErrorHelper::success();
}

AmsError AmsBackendHappyHare::sync_external_identity(int slot_index, const SlotInfo& info) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, slots_.slot_count() - 1);
        }

        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, slots_.slot_count() - 1);
        }

        write_gate_locked(slot_index, entry->info, info);
    }

    // Emit OUTSIDE the lock to avoid deadlock with callbacks
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendHappyHare::persist_external_identity_impl(int slot_index,
                                                         const helix::ams::Observation& spoolman) {
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_external_identity(override_store_.get(), overrides_, slot_index,
                                                   spoolman, "[AMS HappyHare]");
}

void AmsBackendHappyHare::persist_slot_weight(int slot_index, float remaining_weight_g,
                                              float total_weight_g) {
    // The gate map holds no weight, so the stored record is its only durable home.
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_weight(override_store_.get(), overrides_, slot_index,
                                        remaining_weight_g, total_weight_g, "[AMS HappyHare]");
}

uint64_t AmsBackendHappyHare::firmware_tool_mapping_generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.firmware_mapping_generation();
}

AmsError AmsBackendHappyHare::can_set_tool_mapping(int tool_number, int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return can_set_tool_mapping_locked(tool_number, slot_index);
}

AmsError AmsBackendHappyHare::can_set_tool_mapping_locked(int tool_number, int slot_index) const {
    if (tool_number < 0 || tool_number >= static_cast<int>(system_info_.tool_to_slot_map.size())) {
        return AmsErrorHelper::tool_out_of_range(tool_number);
    }
    if (!slots_.is_valid_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, slots_.slot_count() - 1);
    }
    return AmsErrorHelper::success();
}

AmsError AmsBackendHappyHare::set_tool_mapping_impl(int tool_number, int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (auto err = can_set_tool_mapping_locked(tool_number, slot_index); !err.success()) {
            return err;
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
    const std::string cmd = fmt::format("MMU_TTG_MAP TOOL={} GATE={}", tool_number, slot_index);

    spdlog::info("[AMS HappyHare] Mapping T{} to slot {}", tool_number, slot_index);
    return execute_gcode(cmd);
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
                            lv_tr("This Happy Hare system does not support bypass mode"), "");
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
                            lv_tr("Filament is still loaded. Unload it before enabling bypass."),
                            "");
        }
    }

    // Happy Hare uses MMU_SELECT_BYPASS to select bypass
    spdlog::info("[AMS HappyHare] Enabling bypass mode");
    return execute_gcode("MMU_SELECT_BYPASS");
}

AmsError AmsBackendHappyHare::disable_bypass() {
    std::string cmd = "MMU_HOME";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cmd += unit_suffix_locked(kAllUnits);

        if (!running_) {
            return AmsErrorHelper::not_connected("Happy Hare backend not started");
        }

        if (system_info_.current_slot != -2) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not active",
                            lv_tr("Bypass mode is not currently active"), "");
        }
    }

    // To disable bypass, select a gate or unload
    // MMU_SELECT GATE=0 or MMU_HOME will deselect bypass
    spdlog::info("[AMS HappyHare] Disabling bypass mode (homing selector)");
    return execute_gcode(cmd);
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
                            lv_tr("Endless spool is turned off on this MMU"),
                            lv_tr("Turn endless spool on, then set the backup gate"));
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

std::vector<helix::printer::EnvironmentZone>
AmsBackendHappyHare::get_environment_zones(int unit) const {
    // filament_heaters_, environment_sensors_, gate_drying_states_ and heater_temp_ are
    // all written from the status thread under mutex_ (handle_status for the
    // first three, apply_filament_heater_status for the last). Snapshot them once here
    // rather than reading each under no lock at all.
    std::vector<std::string> heaters;
    std::vector<std::string> sensors;
    std::vector<std::string> gate_states;
    std::map<std::string, float> heater_temps;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        heaters = filament_heaters_;
        sensors = environment_sensors_;
        gate_states = gate_drying_states_;
        heater_temps = heater_temp_;
    }

    // The scalar form names one heater and one sensor for the whole machine, which the
    // generic walk already models correctly as one zone per unit.
    if (heaters.empty() && sensors.empty()) {
        return AmsBackend::get_environment_zones(unit);
    }

    const AmsSystemInfo info = get_system_info();
    int gate_count = 0;
    for (const AmsUnit& u : info.units) {
        gate_count += u.slot_count;
    }

    auto zones =
        helix::printer::derive_environment_zones(heaters, sensors, gate_count, gate_states);

    // Fill in what the pure collapse cannot know: which unit a zone's gates fall in,
    // and the dryer behind its heater. target_temp_c, duration_min and remaining_min
    // stay global: Happy Hare tracks one setpoint and one end-of-cycle clock, not one
    // per box, so a per-zone value for those would be invented rather than reported.
    const DryerInfo dryer = get_dryer_info(0);
    for (auto& z : zones) {
        z.unit_index = unit_index_for_gate(info, z.gates.empty() ? -1 : z.gates.front());
        if (!z.heater_name.empty()) {
            z.dryer = dryer;
            z.dryer.supported = true;
            z.dryer.active = z.state == helix::printer::ZoneDryingState::Active;
            if (auto it = heater_temps.find(z.heater_name); it != heater_temps.end()) {
                z.dryer.current_temp_c = it->second;
            }
        }
    }

    if (unit >= 0) {
        zones.erase(std::remove_if(zones.begin(), zones.end(),
                                   [unit](const helix::printer::EnvironmentZone& z) {
                                       return z.unit_index != unit;
                                   }),
                    zones.end());
    }
    return zones;
}

std::string AmsBackendHappyHare::unit_suffix_locked(int unit) const {
    if (!machine_layout_.v4 || machine_layout_.num_units <= 1) {
        return "";
    }
    return unit == kAllUnits ? std::string(" UNIT=ALL") : " UNIT=" + std::to_string(unit);
}

int AmsBackendHappyHare::active_unit_locked() const {
    return active_unit_ >= 0 ? active_unit_ : 0;
}

bool AmsBackendHappyHare::unit_supports_locked(int unit, happy_hare::UnitFeature feature) const {
    if (unit >= 0 && unit < static_cast<int>(machine_units_.size())) {
        return happy_hare::unit_supports(machine_units_[unit], feature, is_v4_locked());
    }
    happy_hare::MachineUnit fallback;
    fallback.selector_type = selector_type_;
    return happy_hare::unit_supports(fallback, feature, is_v4_locked());
}

std::optional<int> AmsBackendHappyHare::unit_with_locked(happy_hare::UnitFeature feature) const {
    const int active = active_unit_locked();
    if (unit_supports_locked(active, feature)) {
        return active;
    }
    for (int u = 0; u < static_cast<int>(machine_units_.size()); ++u) {
        if (unit_supports_locked(u, feature)) {
            return u;
        }
    }
    return std::nullopt;
}

namespace {
/// The unit capability v4 checks before accepting MMU_TEST_CONFIG @p key.
std::optional<happy_hare::UnitFeature> feature_for_param(std::string_view key) {
    using happy_hare::UnitFeature;
    if (key == "gear_from_buffer_speed")
        return UnitFeature::FilamentBuffer;
    if (key == "selector_move_speed")
        return UnitFeature::SelectorSpeed;
    if (key == "sync_to_extruder")
        return UnitFeature::SyncToExtruder;
    if (key == "clog_detection" || key == "detection_length")
        return UnitFeature::Encoder;
    return std::nullopt;
}

/// The unit capability a device action needs.
std::optional<happy_hare::UnitFeature> feature_for_action(std::string_view id) {
    using happy_hare::UnitFeature;
    if (id == "servo_buzz" || id == "servo_up" || id == "servo_move" || id == "servo_down")
        return UnitFeature::Servo;
    if (id == "selector_speed")
        return UnitFeature::SelectorSpeed;
    if (id == "calibrate_encoder" || id == "calibrate_gates" || id == "clog_detection")
        return UnitFeature::Encoder;
    if (id == "sync_to_extruder")
        return UnitFeature::SyncToExtruder;
    if (id == "gear_from_buffer_speed")
        return UnitFeature::FilamentBuffer;
    return std::nullopt;
}

} // namespace

std::optional<int> AmsBackendHappyHare::test_config_unit_locked(std::string_view key) const {
    const int active = active_unit_locked();
    if (happy_hare::param_name(key, machine_layout_).empty()) {
        return std::nullopt;
    }
    // v3 checks only that a parameter names one of its own attributes, which
    // a selector without a moving carriage has no selector_move_speed for.
    if (!is_v4_locked()) {
        return key == "selector_move_speed"
                   ? unit_with_locked(happy_hare::UnitFeature::SelectorSpeed)
                   : std::optional<int>(active);
    }
    // v4 guards these on a fitted sensor, machine-wide.
    if ((key == "toolhead_sensor_to_nozzle" && toolhead_sensor_fitted_ == false) ||
        (key == "toolhead_entry_to_extruder" && extruder_sensor_fitted_ == false)) {
        return std::nullopt;
    }
    if (const auto feature = feature_for_param(key)) {
        return unit_with_locked(*feature);
    }
    return active;
}

std::optional<std::string>
AmsBackendHappyHare::test_config_command_locked(std::string_view key,
                                                const std::string& value) const {
    const auto unit = test_config_unit_locked(key);
    const std::string param = test_config_param_locked(key);
    if (!unit || param.empty()) {
        return std::nullopt;
    }
    return fmt::format("MMU_TEST_CONFIG {}={}{}", param, value,
                       happy_hare::param_is_per_unit(key) ? unit_suffix_locked(*unit)
                                                          : std::string{});
}

std::string AmsBackendHappyHare::heater_suffix_locked(int unit) const {
    const std::string unit_suffix = unit_suffix_locked(unit);
    // Single-unit MMU: omit GATES so HH targets all non-empty gates, matching
    // the long-standing whole-MMU behavior.
    if (unit < 0 || system_info_.units.size() <= 1) {
        return unit_suffix;
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
        return csv.empty() ? unit_suffix : (unit_suffix + " GATES=" + csv);
    }
    return unit_suffix;
}

std::vector<std::string> AmsBackendHappyHare::heater_targets_for_unit(int unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit >= 0 || unit_suffix_locked(kAllUnits).empty()) {
        return {heater_suffix_locked(unit)};
    }
    // The whole machine on a multi-unit v4: UNIT=ALL stops at the first unit
    // with no heater, so each heated unit gets its own command.
    std::vector<std::string> targets;
    for (int u = 0; u < static_cast<int>(machine_units_.size()); ++u) {
        const auto& mu = machine_units_[u];
        const bool heated = !mu.filament_heater.empty() ||
                            std::any_of(mu.filament_heaters.begin(), mu.filament_heaters.end(),
                                        [](const std::string& h) { return !h.empty(); });
        if (heated) {
            targets.push_back(heater_suffix_locked(u));
        }
    }
    return targets;
}

AmsError AmsBackendHappyHare::send_heater_command(const std::string& command, int unit) {
    AmsError result = AmsErrorHelper::not_supported("No unit with a heater");
    const auto targets = heater_targets_for_unit(unit);
    for (size_t i = 0; i < targets.size(); ++i) {
        result = execute_gcode(command + targets[i]);
        if (!result.success()) {
            // A drying start that failed part-way leaves no unit heating on its
            // own: the ones already started are stopped again.
            if (command.rfind("MMU_HEATER DRY=1", 0) == 0) {
                for (size_t j = 0; j < i; ++j) {
                    execute_gcode("MMU_HEATER STOP=1" + targets[j]);
                }
            }
            return result;
        }
    }
    return result;
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
    // send_heater_command() names the unit and its gates; it locks internally,
    // so it is called with no lock held.
    const std::string cmd =
        fmt::format("MMU_HEATER DRY=1 TEMP={:.0f} TIMER={}", temp_c, duration_min);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        dryer_info_.duration_min = duration_min;
        dry_end_epoch_ = now_fn_() + static_cast<std::time_t>(duration_min) * 60;
    }

    spdlog::info("[AMS HappyHare] Starting dryer: {:.0f}°C for {} min (unit {})", temp_c,
                 duration_min, unit);
    return send_heater_command(cmd, unit);
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
    return send_heater_command("MMU_HEATER STOP=1", unit);
}

AmsError AmsBackendHappyHare::update_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)fan_pct; // Happy Hare MMU_HEATER does not accept a FAN parameter

    const bool want_temp = temp_c >= 0.0f;
    const bool want_duration = duration_min > 0;
    if (!want_temp && !want_duration) {
        return AmsErrorHelper::success();
    }

    // TIMER is read only on the DRY=1 path and DRY=1 mid-cycle is refused, so moving the
    // clock means a fresh cycle. Carry the running target when only the duration changed.
    if (want_duration) {
        float target = temp_c;
        if (!want_temp) {
            std::lock_guard<std::mutex> lock(mutex_);
            target = dryer_info_.target_temp_c;
        }
        auto stopped = stop_drying(unit);
        if (!stopped.success()) {
            return stopped;
        }
        return start_drying(target, duration_min, fan_pct, unit);
    }

    // Temperature alone re-sends the setpoint and leaves the cycle and its timer alone.
    return send_heater_command(fmt::format("MMU_HEATER TEMP={:.0f}", temp_c), unit);
}

// ============================================================================
// Device Management
// ============================================================================

std::optional<std::string> AmsBackendHappyHare::clog_detection_mode_gcode(int mode,
                                                                          float det_length) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // v4 refuses the encoder mode on a unit with no encoder.
    const auto unit = test_config_unit_locked("clog_detection");
    if (!unit) {
        return std::nullopt;
    }
    std::string cmd = fmt::format("MMU_TEST_CONFIG {}={}",
                                  happy_hare::param_name("clog_detection", machine_layout_), mode);
    if (mode == 1 && det_length > 0) {
        cmd += fmt::format(" {}={:.1f}",
                           happy_hare::param_name("detection_length", machine_layout_), det_length);
    }
    return cmd + unit_suffix_locked(*unit);
}

std::optional<float> AmsBackendHappyHare::clog_detection_length_setting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_defaults_.detection_length;
}

std::vector<helix::printer::DeviceSection> AmsBackendHappyHare::get_device_sections() const {
    return helix::printer::hh_default_sections();
}

std::vector<helix::printer::DeviceAction> AmsBackendHappyHare::get_device_actions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    using namespace helix::printer;
    auto actions = hh_default_actions();

    // Status-backed, so it applies before the configfile has loaded.
    if (system_info_.spoolman_mode == SpoolmanMode::OFF) {
        for (auto& a : actions) {
            if (a.id == "spoolman_refresh") {
                a.enabled = false;
                a.disable_reason = "Spoolman support is off in Happy Hare";
            }
        }
    }

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

    // --- Hardware filtering: an action no unit's hardware takes is disabled ---
    for (auto& a : actions) {
        if (const auto feature = feature_for_action(a.id); feature && !unit_with_locked(*feature)) {
            a.enabled = false;
            switch (*feature) {
            case happy_hare::UnitFeature::Servo:
                a.disable_reason = "No unit has a servo";
                break;
            case happy_hare::UnitFeature::SelectorSpeed:
                a.disable_reason = "No unit has a moving selector";
                break;
            case happy_hare::UnitFeature::Encoder:
                a.disable_reason = "No unit has an encoder";
                break;
            case happy_hare::UnitFeature::SyncToExtruder:
                a.disable_reason = "Every unit keeps its filament gripped";
                break;
            case happy_hare::UnitFeature::FilamentBuffer:
                a.disable_reason = "No unit has a filament buffer";
                break;
            }
        } else if ((a.id == "toolhead_sensor_to_nozzle" || a.id == "toolhead_entry_to_extruder") &&
                   !test_config_unit_locked(a.id)) {
            a.enabled = false;
            a.disable_reason = "No sensor fitted for this distance";
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
                                 lv_tr("Missing value"), fmt::format(lv_tr("Select a {}"), label))};
        }
        if (const auto* s = std::any_cast<std::string>(&value)) {
            return {*s, AmsErrorHelper::success()};
        }
        return {"", AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                             lv_tr("Invalid value type"),
                             fmt::format(lv_tr("Select a valid {}"), label))};
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
            return {0.0,
                    AmsError(AmsResult::WRONG_STATE, fmt::format("{} value required", label),
                             lv_tr("Missing value"), fmt::format(lv_tr("Provide a {}"), label))};
        }
        if (const auto* d = std::any_cast<double>(&value)) {
            return {*d, AmsErrorHelper::success()};
        }
        return {0.0, AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                              lv_tr("Invalid value type"),
                              fmt::format(lv_tr("Provide a numeric {}"), label))};
    };

    // Helper to extract bool from std::any
    auto require_bool = [&](const char* label) -> std::pair<bool, AmsError> {
        if (!value.has_value()) {
            return {false,
                    AmsError(AmsResult::WRONG_STATE, fmt::format("{} value required", label),
                             lv_tr("Missing value"), fmt::format(lv_tr("Provide {}"), label))};
        }
        if (const auto* b = std::any_cast<bool>(&value)) {
            return {*b, AmsErrorHelper::success()};
        }
        return {false, AmsError(AmsResult::WRONG_STATE, fmt::format("Invalid {} type", label),
                                lv_tr("Invalid value type"),
                                fmt::format(lv_tr("Provide a boolean {}"), label))};
    };

    // --- Simple button actions (no value required) ---
    // `per_unit`: v4 resolves the unit from UNIT= (MMU_SERVO, MMU_CALIBRATE_GATE
    // ALL=1) or infers it from a selected gate (MMU_TEST_GRIP), so these name
    // a unit: the selected one, or for an action only some units' hardware
    // takes, the first unit that has it. The calibrations that act on the
    // selected gate's unit read no UNIT.
    struct ButtonAction {
        const char* id;
        const char* gcode;
        bool per_unit;
    };
    // clang-format off
    static const ButtonAction button_actions[] = {
        {"calibrate_bowden",    "MMU_CALIBRATE_BOWDEN",            false},
        {"calibrate_encoder",   "MMU_CALIBRATE_ENCODER",           false},
        {"calibrate_gear",      "MMU_CALIBRATE_GEAR",              false},
        {"calibrate_gates",     "MMU_CALIBRATE_GATES",             true},
        {"test_grip",           "MMU_TEST_GRIP",                   true},
        {"test_load",           "MMU_TEST_LOAD",                   false},
        {"test_move",           "MMU_TEST_MOVE",                   false},
        {"servo_buzz",          "MMU_SERVO",                       true},
        {"servo_up",            "MMU_SERVO POS=up",                true},
        {"servo_move",          "MMU_SERVO POS=move",              true},
        {"servo_down",          "MMU_SERVO POS=down",              true},
        {"reset_servo_counter", "MMU_STATS COUNTER=servo RESET=1", false},
        {"reset_blade_counter", "MMU_STATS COUNTER=cutter RESET=1", false},
    };
    // clang-format on
    for (const auto& action : button_actions) {
        if (action_id != action.id) {
            continue;
        }
        std::string cmd = action.gcode;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // v4 registers only MMU_CALIBRATE_GATE; MMU_CALIBRATE_GATES is a
            // config macro alias that sends no UNIT.
            if (action_id == "calibrate_gates" && is_v4_locked()) {
                cmd = "MMU_CALIBRATE_GATE ALL=1";
            }
            if (action.per_unit) {
                std::optional<int> unit = active_unit_locked();
                if (const auto feature = feature_for_action(action_id)) {
                    unit = unit_with_locked(*feature);
                }
                if (!unit) {
                    return AmsErrorHelper::not_supported(action_id);
                }
                cmd += unit_suffix_locked(*unit);
            }
        }
        return execute_gcode(cmd);
    }

    // Extruder-only moves drive the toolhead extruder, and HH puts no print
    // check on MMU_LOAD/MMU_UNLOAD; an EXTRUDER_ONLY unload forms a tip and
    // retracts over the part. HH heats the nozzle itself.
    if (action_id == "load_extruder" || action_id == "unload_extruder") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            AmsError precondition = check_preconditions(/*requires_toolhead_motion=*/true);
            if (!precondition) {
                return precondition;
            }
        }
        return execute_gcode(action_id == "load_extruder" ? "MMU_LOAD EXTRUDER_ONLY=1"
                                                          : "MMU_UNLOAD EXTRUDER_ONLY=1");
    }

    // HH refuses MMU_SPOOLMAN outright while its spoolman_support is off.
    if (action_id == "spoolman_refresh") {
        if (!manages_active_spool()) {
            return AmsErrorHelper::not_supported("Spoolman support is off in Happy Hare");
        }
        return execute_gcode("MMU_SPOOLMAN REFRESH=1");
    }

    // --- LED mode dropdown ---
    if (action_id == "led_mode") {
        auto [mode, err] = require_string("LED mode");
        if (!err)
            return err;
        return execute_gcode("MMU_LED EXIT_EFFECT=" + mode);
    }

    // MMU_TEST_CONFIG <param>=<value>, the parameter named as this install
    // spells it.
    auto test_config = [this](const char* key, const std::string& value) {
        std::optional<std::string> cmd;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmd = test_config_command_locked(key, value);
        }
        if (!cmd) {
            return AmsErrorHelper::not_supported(std::string("Happy Hare parameter ") + key);
        }
        return execute_gcode(*cmd);
    };

    // --- Speed sliders (integer formatting) ---
    // clang-format off
    static const std::pair<const char*, const char*> speed_params[] = {
        {"gear_from_buffer_speed", "gear_from_buffer_speed"},
        {"gear_from_spool_speed",  "gear_from_spool_speed"},
        {"gear_unload_speed",      "gear_unload_speed"},
        {"selector_speed",         "selector_move_speed"},
        {"extruder_load_speed",    "extruder_load_speed"},
        {"extruder_unload_speed",  "extruder_unload_speed"},
    };
    // clang-format on
    for (const auto& [id, key] : speed_params) {
        if (action_id == id) {
            auto [speed, err] = require_double("speed");
            if (!err)
                return err;
            auto [lo, hi] = get_action_range(id);
            speed = std::clamp(speed, static_cast<double>(lo), static_cast<double>(hi));
            auto result = test_config(key, fmt::format("{:.0f}", speed));
            if (result.success()) {
                save_override(key, static_cast<float>(speed));
            }
            return result;
        }
    }

    // --- Toolhead distance sliders (one decimal place) ---
    static const char* const toolhead_params[] = {
        "toolhead_sensor_to_nozzle",
        "toolhead_extruder_to_nozzle",
        "toolhead_entry_to_extruder",
        "toolhead_ooze_reduction",
    };
    for (const char* key : toolhead_params) {
        if (action_id == key) {
            auto [dist, err] = require_double("distance");
            if (!err)
                return err;
            auto [lo, hi] = get_action_range(key);
            dist = std::clamp(dist, static_cast<double>(lo), static_cast<double>(hi));
            auto result = test_config(key, fmt::format("{:.1f}", dist));
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
        auto result = test_config("sync_to_extruder", std::to_string(val));
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
        auto result = test_config("clog_detection", std::to_string(mode_int));
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
        std::string cmd = enable ? "MMU_MOTORS_ON" : "MMU_MOTORS_OFF";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmd += unit_suffix_locked(kAllUnits);
        }
        return execute_gcode(cmd);
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}

} // namespace helix
