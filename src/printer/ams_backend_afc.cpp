// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_afc.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_update_queue.h"

#include "action_prompt_manager.h"
#include "afc_defaults.h"
#include "ams_bypass_policy.h"
#include "ams_fault_event.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "operation_patterns.h" // helix::contains_ci
#include "printer_discovery.h"
#include "settings_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <optional>
#include <sstream>
#include <vector>

using namespace helix;

namespace {

// Extract trailing numeric suffix from a string (e.g., "lane12" → 12, "Turtle_1" → 1).
// Returns INT_MAX if no digits found, so non-numeric names sort last.
int trailing_number(const std::string& s) {
    auto pos = s.find_last_not_of("0123456789");
    if (pos == std::string::npos || pos == s.size() - 1)
        return INT_MAX;
    return std::stoi(s.substr(pos + 1));
}

// Natural sort by trailing number, then lexicographic tiebreak.
// "lane2" < "lane10", "Turtle_1" < "Turtle_2" < "Turtle_10"
bool natural_less(const std::string& a, const std::string& b) {
    int na = trailing_number(a);
    int nb = trailing_number(b);
    if (na != nb)
        return na < nb;
    return a < b;
}

// Read AFC's spool-vendor field into `out`, accepting every spelling the
// ecosystem uses. Returns true when a value was found.
//
// This reader serves BOTH AFC surfaces, and #808 (shipped by #833) spells the
// value differently on each — deliberately:
//   - lane_data uses `vendor_name`, the key Happy Hare already established in
//     that shared namespace, so a consumer of lane_data needs one spelling
//     regardless of which backend wrote the record.
//   - get_status uses `spool_vendor`, AFCLane's own attribute name, since the
//     status dict is AFC's private surface and mirrors its attributes.
// `vendor` is the name we originally proposed and the key our own
// to_lane_data_record() still emits — that record lands in a PRIVATE namespace
// for AFC today (#1158), so this reader does not meet it yet, but it will the
// moment #1158 migrates AFC's overrides into lane_data proper. `brand` is a
// defensive fourth spelling.
//
// Empty values are IGNORED rather than treated as a clear. That is deliberate and
// differs from the color/material handling above: #808 is unimplemented, so we do
// not know whether an unlinked lane will omit the key or publish "". Guessing
// wrong in the clearing direction wipes a brand nothing re-supplies. A user's
// override is safe either way — both callers run apply_overrides() after this
// reader (#1195 closed that gap on the lane_data path) — so the exposure is lanes
// with no override at all. Revisit once #808 ships and the payload is observable.
bool read_vendor(const nlohmann::json& src, std::string& out) {
    for (const char* key : {"vendor_name", "spool_vendor", "vendor", "brand"}) {
        auto it = src.find(key);
        if (it != src.end() && it->is_string()) {
            std::string v = it->get<std::string>();
            if (!v.empty()) {
                out = std::move(v);
                return true;
            }
        }
    }
    return false;
}

// Split a firmware state token into sentence-case words:
//   "ToolSwap"        → "Tool swap"
//   "SOME_LOUD_STATE" → "Some loud state"
//   "purging_bucket"  → "Purging bucket"
//
// Used as the display fallback for states we have no translation for. AFC emits
// camelCase state values (v1.2.0), and those must never reach the screen raw —
// operation_detail is passed through to the UI verbatim.
std::string humanize_state(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 4);
    bool word_start = true;
    for (size_t i = 0; i < raw.size(); ++i) {
        auto c = static_cast<unsigned char>(raw[i]);
        if (std::isalnum(c) == 0) {
            // Any separator run collapses to a single space.
            if (!out.empty() && out.back() != ' ')
                out.push_back(' ');
            word_start = true;
            continue;
        }
        // camelCase boundary: an uppercase letter that either follows a
        // lowercase/digit ("ToolSwap") or begins a word after an acronym run
        // ("HUBLoading" → "HUB loading").
        if (std::isupper(c) != 0 && i > 0) {
            auto prev = static_cast<unsigned char>(raw[i - 1]);
            bool after_lower = std::isalnum(prev) != 0 && std::isupper(prev) == 0;
            bool acronym_end = std::isupper(prev) != 0 && i + 1 < raw.size() &&
                               std::islower(static_cast<unsigned char>(raw[i + 1])) != 0;
            if (after_lower || acronym_end) {
                if (!out.empty() && out.back() != ' ')
                    out.push_back(' ');
                word_start = true;
            }
        }
        // Sentence case: capitalize only the very first character.
        if (word_start && out.empty()) {
            out.push_back(static_cast<char>(std::toupper(c)));
        } else {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
        word_start = false;
    }
    // Trim a trailing separator-induced space.
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// Translated display text for AFC's known state vocabulary, keyed by normalized
// token so a rewording (AFC's "Tool swap" → "ToolSwap") keeps its translation.
// Unknown states fall back to humanize_state() — untranslated, but readable.
std::string afc_state_detail(std::string_view raw) {
    struct StateLabel {
        const char* token;
        const char* label;
    };
    static constexpr StateLabel LABELS[] = {
        {"idle", "Idle"},
        {"initialized", "Initialized"},
        {"loading", "Loading"},
        {"unloading", "Unloading"},
        {"error", "Error"},
        {"ejecting", "Ejecting"},
        {"moving", "Moving lane"},
        {"restoring", "Restoring position"},
        {"toolswap", "Tool swap"},
        {"tooldock", "Docking tool"},
        {"toolpickup", "Picking up tool"},
    };
    const std::string token = ams_normalize_state_token(raw);
    for (const auto& e : LABELS) {
        if (token == e.token)
            return lv_tr(e.label);
    }
    return humanize_state(raw);
}

// [L067] LABELS feeds lv_tr() through a variable, which the translation
// extractor cannot see. Name each label literally here so it lands in the
// catalogs. Keep in sync with LABELS above.
// clang-format off
void afc_state_translation_hints_() {
    (void)lv_tr("Idle"); (void)lv_tr("Initialized"); (void)lv_tr("Loading");
    (void)lv_tr("Unloading"); (void)lv_tr("Error"); (void)lv_tr("Ejecting");
    (void)lv_tr("Moving lane"); (void)lv_tr("Restoring position");
    (void)lv_tr("Tool swap"); (void)lv_tr("Docking tool"); (void)lv_tr("Picking up tool");
}
// clang-format on

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsBackendAfc::AmsBackendAfc(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info with AFC defaults
    system_info_.type = AmsType::AFC;
    system_info_.type_name = "AFC";
    // AFC capabilities from shared defaults
    auto caps = helix::printer::afc_default_capabilities();
    // AFC has no on/off switch for endless spool: present means on. Sourced
    // from the shared defaults so the mock's AFC scenario cannot drift.
    system_info_.endless_spool_enabled = caps.supports_endless_spool;
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
    // lifetime_ destructor calls invalidate() automatically
}

// ============================================================================
// Sensor Ownership (#1054)
// ============================================================================

bool AmsBackendAfc::owns_filament_sensor(const std::string& bare_name,
                                         const helix::PrinterDiscovery& discovery) {
    auto ends_with = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    // Fixed names at the extruder.
    if (bare_name == "tool_start" || bare_name == "tool_end") {
        return true;
    }
    // Per-lane sensors are <lane>_prep, <lane>_load, <lane>_selector.
    for (const auto& lane : discovery.afc_lane_names()) {
        if (bare_name == lane + "_prep" || bare_name == lane + "_load" ||
            bare_name == lane + "_selector") {
            return true;
        }
    }
    // Per-buffer sensors are <buffer>_expanded, <buffer>_compressed.
    for (const auto& buffer : discovery.afc_buffer_names()) {
        if (bare_name == buffer + "_expanded" || bare_name == buffer + "_compressed") {
            return true;
        }
    }
    // AFC HTLF units register <unit>_home_pin. The unit name may be anything;
    // once AFC is the detected backend, the _home_pin suffix is unambiguous.
    return ends_with(bare_name, "_home_pin");
}

// ============================================================================
// Capability queries
// ============================================================================

bool AmsBackendAfc::auto_unloads_after_print() const {
    // AFC's post-print unload depends on the user's end-of-print macros, so
    // this is a per-printer user setting rather than a firmware constant.
    return SettingsManager::instance().get_afc_unload_after_print();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

void AmsBackendAfc::on_started() {
    // Version is informational only (see apply_afc_version_response) — nothing
    // below depends on the result, so this does not need to complete first.
    // Load persisted per-slot overrides BEFORE any status callback can parse a
    // lane, so the first frame already carries the user's identity. Private
    // namespace: lane_data belongs to AFC's own plugin (it deletes that whole
    // namespace on every boot), so sharing it would both lose our records and
    // ingest AFC's as if the user had authored them.
    if (api_) {
        override_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            api_, "afc", helix::ams::lane_key_style_for(get_type()), OVERRIDE_NAMESPACE);
        auto loaded = override_store_->load_blocking();
        const auto loaded_count = loaded.size();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            overrides_ = std::move(loaded);
        }
        spdlog::info("[AMS AFC] Loaded {} slot overrides", loaded_count);
    }

    detect_afc_version();

    // If we have discovered lanes (from PrinterCapabilities), initialize them now.
    // This provides immediate lane data for every AFC version. query_lane_data()
    // may later supplement it with colors/materials/spool IDs from the database.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!discovered_lane_names_.empty() && !slots_.is_initialized()) {
            spdlog::info("[AMS AFC] Initializing {} lanes from discovery",
                         discovered_lane_names_.size());
            initialize_slots(discovered_lane_names_);
        }
    }

    // Always attempt the lane_data query. There is no reliable capability flag to
    // gate on: AFC's `lane_data_enabled` reports whether Moonraker has the (now
    // unused) [lane_data] section, NOT whether the database namespace holds data —
    // send_lane_data() writes to the database unconditionally. A live BoxTurtle on
    // 2026-07-26 had lane_data_enabled=false with a fully populated namespace.
    // Attempting and adapting is the only correct detection: a missing namespace
    // just errors, and the probe is silent.
    query_lane_data();

    // AFC indexes toolheads by th_extruder_name, which no get_status() carries.
    // configfile.settings is the only published source, so fetch it once here;
    // parse_afc_extruder() falls back to the section name until it lands.
    query_afc_configfile_topology();

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

    // Check for stuck operations on every UI poll, not just on status updates.
    // The sidebar's stall watchdog polls this whenever the action is non-IDLE,
    // which is the only clock AFC has left when the printer goes silent
    // mid-operation (network drop, Klipper shutdown). Must run BEFORE the
    // uninitialized-registry early return below or that path skips it.
    const_cast<AmsBackendAfc*>(this)->check_action_timeout();

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
    info.next_slot = system_info_.next_slot;
    info.position_saved = system_info_.position_saved;
    info.spoolman_url = system_info_.spoolman_url;
    info.filament_loaded = system_info_.filament_loaded;
    info.endless_spool_enabled = system_info_.endless_spool_enabled;
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
        info.units[u].lane_is_hub_routed = system_info_.units[u].lane_is_hub_routed;
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

AmsError AmsBackendAfc::clear_message_queue() {
    // AFC keeps a persistent message_queue + error_state that won't clear until
    // AFC_CLEAR_MESSAGE is sent. Without this the error dialog reappears
    // immediately because AFC keeps reporting ERROR (#497). Fire-and-forget: a
    // failure here is non-fatal (the command may not be supported on older AFC).
    spdlog::debug("[AMS AFC] Clearing AFC message queue");
    return execute_gcode("AFC_CLEAR_MESSAGE");
}

AmsError AmsBackendAfc::clear_fault(int slot_index) {
    // AFC has no per-lane fault clear; both commands are system-scoped.
    (void)slot_index;

    // Drop any queued LANE_UNLOAD requests, exactly as cancel() does. Clearing a
    // fault means the user wants to stop, not to chain through a pile of pending
    // ejects afterwards. eject_in_flight_ is deliberately NOT cleared — the
    // in-flight LANE_UNLOAD's completion callback still fires and clears it once
    // it sees the empty queue.
    {
        std::lock_guard<std::mutex> lock(eject_queue_mutex_);
        if (!pending_eject_lanes_.empty()) {
            spdlog::info("[AMS AFC] Clear fault: discarding {} queued LANE_UNLOAD request(s)",
                         pending_eject_lanes_.size());
            pending_eject_lanes_.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Arm the drain. printer.AFC.message is a FIFO head — one clear pops one
        // entry, so a second queued error would otherwise stay on screen and look
        // exactly like the Reset having done nothing. The drain runs until the
        // queue reports empty; MESSAGE_DRAIN_MAX_CLEARS is only the runaway guard.
        message_drain_budget_ = MESSAGE_DRAIN_MAX_CLEARS;
        message_drain_pending_ = false;
        // Bound the arm in wall-clock time. If the queue was already empty, no
        // later delta will carry `message` at all, so the empty-message disarm
        // below never fires and the budget would otherwise persist indefinitely.
        message_drain_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        // Drop the stuck-action latch: the user dismissed the error, so AFC's
        // own state string drives the action again. If AFC is genuinely still
        // stuck the clock restarts from here and re-fires after a full budget,
        // which is bounded and correct.
        if (timed_out_state_.has_value()) {
            spdlog::debug("[AMS AFC] Clear fault: releasing stuck-action latch on '{}'",
                          *timed_out_state_);
            timed_out_state_.reset();
            timed_out_detail_.clear();
        }
        action_start_time_ = std::chrono::steady_clock::now();
    }

    // Deliberately does NOT route through cancel(): cancel() returns early when
    // the action is IDLE, which is the common case for a queued message — AFC
    // keeps printer.AFC.message populated long after current_state returns to
    // Idle, and AFC_RESET does not touch it.
    spdlog::info("[AMS AFC] Clearing fault (draining message queue, max {} clears)",
                 MESSAGE_DRAIN_MAX_CLEARS);
    // execute_gcode_notify, matching cancel(): the user pressed a button, so a
    // failed RESET_FAILURE must surface rather than being logged silently.
    AmsError failure_reset = execute_gcode_notify(
        "RESET_FAILURE", lv_tr("AFC failure reset complete"), lv_tr("AFC failure reset failed"));
    AmsError message_clear = clear_message_queue();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (message_drain_budget_ > 0) {
            --message_drain_budget_;
        }
    }
    return failure_reset.success() ? message_clear : failure_reset;
}

void AmsBackendAfc::maybe_drain_message_queue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!message_drain_pending_ || message_drain_budget_ <= 0) {
            return;
        }
        // This is the ONLY place a stale arm is retired — parse_afc_state()'s
        // re-arm has no expiry check of its own. It cannot: an arm goes stale
        // precisely when the queue was already empty at clear_fault() time, and
        // AFC then omits the unchanged `message` key forever, so the
        // empty-message disarm there never runs. The firing decision therefore
        // has to be made here, against the wall clock, or a months-old arm would
        // pop the user's next unrelated error.
        if (std::chrono::steady_clock::now() > message_drain_deadline_) {
            message_drain_budget_ = 0;
            message_drain_pending_ = false;
            return;
        }
        message_drain_pending_ = false;
        --message_drain_budget_;
        if (message_drain_budget_ == 0) {
            // Last permitted clear. If the queue is still non-empty after it,
            // whatever the user sees next is residue we gave up on and this is
            // the only trace of why — parse_afc_state() stops re-arming once
            // the budget is spent, so nothing downstream records it.
            spdlog::warn("[AMS AFC] Message drain reached its {}-clear cap; sending the "
                         "last clear and stopping",
                         MESSAGE_DRAIN_MAX_CLEARS);
        }
    }

    spdlog::debug("[AMS AFC] Draining next queued message");
    clear_message_queue();
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

    // Check sensors from furthest to nearest. PathSegment::HUB is deliberately
    // unreachable here: AFC's per-lane loaded_to_hub is latched at prep and
    // never updated, so it cannot distinguish "at hub" from "prepped once" —
    // it read true on every prepped lane while the shared hub sensor read
    // clear. There is no per-slot hub sensor to fall back on for a non-active
    // slot, so below load/prep is the furthest this can honestly report.
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

bool AmsBackendAfc::slot_has_filament_at_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string lane_name = slots_.name_of(slot_index);
    if (lane_name.empty()) {
        return false;
    }

    // The extruder sensors say something is at a toolhead; lane_loaded says
    // whose. Without that pairing the trip is unattributable, and the base
    // contract is explicit that an unknown signal reads false rather than being
    // fabricated onto a lane. Scanning the map rather than indexing by tool
    // number keeps this right when a lane feeds a non-default extruder.
    for (const auto& [ext_name, sensors] : extruder_sensors_) {
        if (sensors.lane_loaded == lane_name) {
            return sensors.tool_start || sensors.tool_end;
        }
    }
    return false;
}

std::vector<helix::RecoveryAction> AmsBackendAfc::build_recovery_actions() const {
    // Caller holds mutex_.
    std::vector<helix::RecoveryAction> actions;

    // Resume after the user clears the jam (always offered). Resuming a paused
    // print extrudes on the next move, so it needs the hotend up.
    actions.push_back({lv_tr("Resume"), "RESUME", "afc::resume", "primary",
                       /*needs_hot_nozzle=*/true});

    const bool toolhead_loaded = tool_start_sensor_ || system_info_.filament_loaded;
    if (toolhead_loaded) {
        // Closes R1: unload from the toolhead before any eject is possible.
        // Retracts filament back out through the melt zone — cold, it snaps or
        // leaves the plug behind.
        actions.push_back({lv_tr("Unload"), "TOOL_UNLOAD", "afc::tool_unload", "",
                           /*needs_hot_nozzle=*/true});
    } else if (!current_lane_name_.empty()) {
        // Empty toolhead but a lane is selected — eject that lane. Lane to spool
        // only; the filament never reaches the nozzle, so no heat is required.
        actions.push_back(
            {lv_tr("Eject"), "LANE_UNLOAD LANE=" + current_lane_name_, "afc::lane_unload", ""});
    }

    // Reset/re-prep all lanes (last resort). Re-prep runs the lane motors up to
    // the hub, not through the toolhead.
    actions.push_back({lv_tr("Recover"), "AFC_RESET", "afc::reset", "danger"});
    return actions;
}

std::optional<helix::ErrorEvent> AmsBackendAfc::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // error_state_ is the exact condition that produced the AmsAction::ERROR
    // edge the bridge fired on. Upstream's AFC_error.py set_error_state() is the
    // ONLY writer of afc.error_state, and it assigns current_state = State.ERROR
    // on the very next line, so the two cannot diverge — "Error" reaching
    // ams_action_from_string() and error_state_ being true are the same event.
    //
    // The stuck-action latch (apply_action_timeout_latch_locked) also drives the
    // action to ERROR without AFC ever setting error_state. Returning nullopt
    // there is deliberate: that fault is ours, not AFC's, and it keeps its
    // existing last-resort toast rather than claiming a recovery set for a
    // condition the firmware does not agree is an error.
    if (!error_state_) {
        return std::nullopt;
    }

    helix::ErrorEvent e =
        helix::make_ams_fault_event(helix::ErrorSource::AFC, lv_tr("Filament System Error"),
                                    /*detail=*/"", build_recovery_actions());

    // AFC.message is a FIFO *peek* (_get_message(clear=False)) that
    // RESET_FAILURE does not pop, so it can still hold the text of an
    // already-resolved fault. Only trust it while AFC itself still types the
    // queued message as an error; otherwise fall back to the action mapping's
    // detail, which at minimum names what was being attempted.
    if (last_message_type_ == "error" && !last_seen_message_.empty()) {
        e.detail = last_seen_message_;
    } else if (!system_info_.operation_detail.empty() &&
               system_info_.operation_detail != last_seen_message_) {
        // operation_detail is only a useful fallback when it describes the
        // OPERATION. parse_afc_state overwrites it with the queued message text
        // for any message type, so without this inequality the branch above
        // would reject a warning-typed message and this one would hand the same
        // string straight back.
        e.detail = system_info_.operation_detail;
    } else {
        e.detail = lv_tr("The filament system reported an error");
    }

    return e;
}

std::optional<helix::ErrorEvent>
AmsBackendAfc::classify_error(const std::string& raw_line,
                              const helix::ClassifyContext& ctx) const {
    // Only `!!` emergency lines are candidates.
    if (!helix::is_bang_line(raw_line)) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Strip the "!! " prefix for the detail text.
    std::string detail = helix::strip_bang_prefix(raw_line);

    // 1) Toolhead jam / break (handle_toolhead_runout signature).
    const bool is_jam = helix::contains_ci(detail, "tool_end") &&
                        (helix::contains_ci(detail, "jam") || helix::contains_ci(detail, "break") ||
                         helix::contains_ci(detail, "runout detected"));
    if (is_jam) {
        return helix::make_ams_fault_event(helix::ErrorSource::AFC, lv_tr("Toolhead jam"), detail,
                                           build_recovery_actions());
    }

    // 2) Catch-all: any pausing !! while AFC is in an error state.
    if (ctx.is_paused && error_state_) {
        return helix::make_ams_fault_event(helix::ErrorSource::AFC, lv_tr("Filament System Error"),
                                           detail, build_recovery_actions());
    }

    // 3) Not an AFC-owned fault — let the generic classifier handle it.
    return std::nullopt;
}

std::vector<AmsBackend::ToolchangePhase>
AmsBackendAfc::toolchange_phase_template(StepOperationType op) const {
    // Mirrors AFC's real CHANGE_TOOL, which is TOOL_UNLOAD(old) then
    // TOOL_LOAD(new). The purge-to-bucket, kick and wipe all live in
    // do_poop_kick_wipe(), which TOOL_LOAD calls only AFTER load_sequence()
    // succeeds — the poop purges the old colour out THROUGH the newly fed
    // filament, so it cannot precede the feed. Verified against upstream
    // v1.1.0 and v1.2.0.
    //
    // AFC has exactly one purge (the poop macro, `poop_cmd`) and one wipe
    // (`wipe_cmd`/AFC_BRUSH), so there is no "Purge" distinct from the poop and
    // no "Clean nozzle" distinct from the brush.
    //
    // The wipe runs TWICE, straddling the kick. Upstream order is
    // poop -> wipe -> kick -> wipe, unchanged across both releases:
    // AFC.py do_poop_kick_wipe() v1.2.0:1390-1413, and the same sequence inline
    // in TOOL_LOAD at v1.1.0:1417-1440. All four macros narrate at the shipped
    // default `variable_verbose: 1` (config/AFC_Macro_Vars.cfg:17), so the
    // router sees poop, brush, kick, brush.
    //
    // The phases are therefore ordered by FIRST occurrence — poop, brush, kick —
    // not by the order a naive reading of the macro names suggests. Listing kick
    // before brush made the published index run 4 -> 6 -> 5 -> 6 on every stock
    // toolchange: the bar jumped to "Brush nozzle", snapped back to "Kick away",
    // then forward again. The bar has no notion of a repeated step, so the
    // trailing wipe re-reports the brush phase; AmsState::set_narration_phase()
    // latches the high-water index and swallows it.
    switch (op) {
    case StepOperationType::LOAD_SWAP:
        return {
            {"heat", "Heat nozzle", false},       {"cut", "Cut tip", true},
            {"unload", "Unload filament", false}, {"feed", "Feed filament", false},
            {"poop", "Purge to bucket", true},    {"brush", "Brush nozzle", true},
            {"kick", "Kick away", true},          {"load", "Load complete", false},
        };
    case StepOperationType::LOAD_FRESH:
        return {
            {"heat", "Heat nozzle", false},    {"feed", "Feed filament", false},
            {"poop", "Purge to bucket", true}, {"brush", "Brush nozzle", true},
            {"kick", "Kick away", true},       {"load", "Load complete", false},
        };
    case StepOperationType::UNLOAD:
        return {
            {"heat", "Heat nozzle", false},
            {"cut", "Cut tip", true},
            {"unload", "Retract filament", false},
        };
    }
    return {};
}

std::optional<std::string>
AmsBackendAfc::match_narration_phase(const std::string& narration) const {
    if (narration.empty())
        return std::nullopt;

    // Normalize to lowercase words joined by single spaces. Punctuation and
    // separator style then stop mattering, so "AFC_Brush: Clean Nozzle",
    // "AFC Brush - Clean nozzle" and "[AFC_Brush] Clean Nozzle!" all match the
    // same needle. This matcher IS the step-bar model and upstream owns the
    // wording, so it has to tolerate rewording that keeps the same words.
    std::string s;
    s.reserve(narration.size());
    for (unsigned char c : narration) {
        if (std::isalnum(c) != 0) {
            s.push_back(static_cast<char>(std::tolower(c)));
        } else if (!s.empty() && s.back() != ' ') {
            s.push_back(' ');
        }
    }
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    if (s.empty())
        return std::nullopt;

    auto has = [&](const char* needle) { return s.find(needle) != std::string::npos; };
    // Order matters: more specific phrases first.
    if (has("is now loaded in toolhead") || has("load complete") || has("loaded in toolhead"))
        return "load";
    // Must precede the "feed" needles below: AFC announces the old lane coming
    // out as "Unloading lane1", and normalized "unloading lane1" CONTAINS the
    // substring "loading lane" — checked later it would resolve to feed and the
    // bar would skip forward over the unload.
    if (has("unload"))
        return "unload";
    // AFC_BRUSH is the only wipe. It emits "AFC_Brush: Clean Nozzle" at the
    // default variable_verbose=1 and "AFC_Brush: Move to Brush." only at
    // verbose>1, so both spellings must land on the same phase or the step never
    // lights on a stock install.
    if (has("clean nozzle") || has("cleaning nozzle") || has("brush"))
        return "brush";
    // The poop macro IS the purge; it says "Starting poop" at verbose 1 and
    // "Move To Purge Location" at verbose>1. One phase, both spellings.
    if (has("purg") || has("poop"))
        return "poop";
    if (has("kick"))
        return "kick";
    // Before the retract needle: AFC_Cut says "Retract Filament for Cut", which
    // is part of the cut, not the old filament coming out of the toolhead.
    if (has("cut"))
        return "cut";
    if (has("retract")) // UNLOAD ends on this step; keep it reachable (#1046)
        return "unload";
    if (has("to hub") || has("feed") || has("loading lane"))
        return "feed";
    if (has("heat"))
        return "heat";
    return std::nullopt;
}

namespace {

/// Lowercase the line and split it on whitespace. Punctuation stays attached, so
/// AFC's `t:0` and `lane3` each stay a single token and the token COUNT is a
/// usable anchor.
std::vector<std::string> split_lower_words(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : line) {
        if (std::isspace(c) != 0) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::string to_lower_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

/// Highest tool number a lane may claim. Anything above this is treated as
/// garbage rather than grown into, so a malformed field cannot size a vector.
constexpr int AFC_MAX_TOOL_NUMBER = 64;

/// Parse AFC's per-lane `map` field into tool numbers, in the order AFC sent them.
/// Also used for the scalar `current_map`, which shares the `T<n>` token grammar and
/// simply yields zero or one entry.
///
/// Two wire shapes are live at once and both must keep working. Before virtual
/// tools (AFC #605) a lane's map is a single `"T0"` string; from #605 on it is
/// ALWAYS a list — even a lane with exactly one tool, and even when virtual tools
/// are left disabled — so the list shape is what every AFC install sends once that
/// version is out, not just the ones that opted in.
///
/// The tool number must be ALL digits: `std::stoi("14,13")` returns 14 without
/// throwing, so a laxer parse would turn a hypothetical comma-joined string into a
/// confident, wrong, silent single mapping. Unparseable entries are skipped
/// individually — one junk element does not discard a lane's good ones.
std::vector<int> parse_afc_lane_map(const nlohmann::json& map_value) {
    auto parse_one = [](const nlohmann::json& value, std::vector<int>& out) {
        if (!value.is_string())
            return;
        const std::string token = value.get<std::string>();
        if (token.size() < 2 || token[0] != 'T')
            return;
        const std::string digits = token.substr(1);
        if (!std::all_of(digits.begin(), digits.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; }))
            return;
        try {
            const int tool = std::stoi(digits);
            if (tool >= 0 && tool <= AFC_MAX_TOOL_NUMBER)
                out.push_back(tool);
        } catch (...) {
            // Out of int range — skip
        }
    };

    std::vector<int> tools;
    if (map_value.is_array()) {
        for (const auto& entry : map_value)
            parse_one(entry, tools);
    } else {
        parse_one(map_value, tools);
    }
    return tools;
}

} // namespace

std::optional<std::string>
AmsBackendAfc::match_bare_narration_phase(const std::string& line) const {
    // Unlike match_narration_phase(), this runs on the printer's OPEN console —
    // M105 reports, `echo:` chatter and the gcode filename all land here, and the
    // filename is user-controlled. So each shape below is anchored on fixed words
    // AND on position/count; nothing is a free substring test. `File opened:
    // haircut.gcode` must not read as a Cut-tip step.
    //
    // Shapes are verbatim from AFC's own logger calls, captured in
    // tests/unit/test_afc_console_corpus.cpp, captured from a live BoxTurtle:
    //
    //   Loading lane3                          -> feed
    //   Unloading lane1                        -> unload
    //   lane3 is now loaded in toolhead t:0    -> load
    //   Lane lane1 unload done t:0             -> unload
    //
    // Deliberately NOT mapped, because the step template has no phase for them:
    // `Tool Change - lane1 -> lane3`, `Total change time: t:0`, and the #1183
    // no-op `lane1 already loaded` — the last of which must especially not read
    // as a completed load.
    const std::vector<std::string> t = split_lower_words(line);
    if (t.size() < 2)
        return std::nullopt;

    // Exactly the verb plus the lane name. AFC emits `'Loading ' + lane.name`,
    // so any trailing words mean this is somebody else's line.
    if (t.size() == 2) {
        if (t[0] == "loading")
            return "feed";
        if (t[0] == "unloading")
            return "unload";
    }

    // Five fixed words in fixed positions after the lane name. The trailing
    // `t:<n>` is optional so pre-toolchanger AFC builds still match.
    if (t.size() >= 6 && t[1] == "is" && t[2] == "now" && t[3] == "loaded" && t[4] == "in" &&
        t[5] == "toolhead")
        return "load";

    if (t.size() >= 4 && t[0] == "lane" && t[2] == "unload" && t[3] == "done")
        return "unload";

    return std::nullopt;
}

bool AmsBackendAfc::is_narration_drift_candidate(const std::string& line) const {
    const std::string s = to_lower_copy(line);

    // Every AFC narration line either names the system (`AFC_Cut:`, `AFC_Brush:`)
    // or names a lane. Looser than the matchers on purpose: the hint exists to
    // catch upstream REWORDING, which by definition no matcher recognizes. A
    // false positive costs one deduped debug line.
    if (s.find("afc") == std::string::npos && s.find("lane") == std::string::npos)
        return false;

    // ...minus the lines AFC emits every toolchange that have no phase by design.
    // Without this the log would report them as drift forever.
    static constexpr const char* KNOWN_PHASELESS[] = {
        "tool change",
        "toolchange",
        "already loaded",
        "total change time",
        "rotation distance reset",
    };
    for (const char* known : KNOWN_PHASELESS) {
        if (s.find(known) != std::string::npos)
            return false;
    }
    return true;
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
    //   load              → LANE (filament in lane between prep and hub)
    //   prep              → PREP (filament at prep sensor, past spool)
    //   (no sensors)      → NONE or SPOOL depending on context
    //
    // PathSegment::HUB is deliberately unreachable here. AFC's per-lane
    // loaded_to_hub is latched at prep and never updated, so it cannot
    // distinguish "at hub" from "prepped once"; the hub sensor already covers
    // the real transition as OUTPUT. HUB stays in the enum for Happy Hare.

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

        // Track whether parse_afc_state set current_slot from current_load/current_lane.
        // When set, the reconciliation block must not overwrite it (tool changers have
        // ALL lanes loaded, so scanning for first loaded lane picks the wrong one).
        bool current_slot_set_by_afc_state = false;

        // Distinct from the flag above, which is true whichever WAY the AFC object
        // spoke — a named current_load sets it too. This one means specifically
        // "AFC said nothing is at the toolhead", and only apply_mount_state()
        // consults it. Both are per-frame locals on purpose: #1229 was a value
        // latching and never moving again, and a variable reborn every frame
        // cannot latch.
        bool afc_stated_unloaded = false;

        // Parse global AFC state if present
        if (params.contains("AFC") && params["AFC"].is_object()) {
            parse_afc_state(params["AFC"], deferred_error_event, current_slot_set_by_afc_state,
                            afc_stated_unloaded);
            state_changed = true;
        }

        // Legacy: also check for lowercase "afc" (older AFC versions)
        if (params.contains("afc") && params["afc"].is_object()) {
            parse_afc_state(params["afc"], deferred_error_event, current_slot_set_by_afc_state,
                            afc_stated_unloaded);
            state_changed = true;
        }

        // Learn which prefix this firmware publishes lanes under, then fire the
        // one-shot feature probe. The probe CANNOT read a status frame: the
        // subscription is field-scoped (afc_stepper_fields in
        // moonraker_discovery_sequence.cpp) and does not request
        // filament_name/multi_color_hexes/initial_weight, so those keys never
        // arrive here on any AFC version. Reading a frame reported "legacy" on a
        // confirmed v1.2.0 BoxTurtle. Only an explicit unscoped
        // printer.objects.query returns the whole lane object.
        if (!feature_level_checked_ && lane_object_prefix_.empty()) {
            for (auto it = params.begin(); it != params.end(); ++it) {
                const std::string& k = it.key();
                if (!it.value().is_object()) {
                    continue;
                }
                if (k.rfind("AFC_stepper ", 0) == 0) {
                    lane_object_prefix_ = "AFC_stepper ";
                } else if (k.rfind("AFC_lane ", 0) == 0) {
                    lane_object_prefix_ = "AFC_lane ";
                } else {
                    continue;
                }
                probe_feature_level(k);
                break;
            }
        }

        // Parse AFC_stepper lane objects for sensor states
        // Keys like "AFC_stepper lane1", "AFC_stepper lane2", etc.
        bool lanes_updated = false;
        for (int i = 0; i < slots_.slot_count(); ++i) {
            std::string lane_name = slots_.name_of(i);
            std::string key = "AFC_stepper " + lane_name;
            if (params.contains(key) && params[key].is_object()) {
                parse_afc_stepper(i, lane_name, params[key]);
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
                parse_afc_stepper(i, lane_name, params[key]);
                state_changed = true;
                lanes_updated = true;
            }
        }

        // After processing lane updates, reconcile filament_loaded from the slot
        // statuses. Unconditional: no AFC version publishes a filament_loaded
        // key on the AFC object (AFC.py get_status, v1.1.0 and v1.2.0:2531-2564),
        // so the per-lane scan is the only authority there has ever been.
        if (lanes_updated && slots_.is_initialized()) {
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
            // Only set current_slot as fallback when AFC state didn't provide
            // an authoritative value (tool changers have ALL lanes loaded, so
            // scanning for first loaded lane would pick the wrong one)
            if (!current_slot_set_by_afc_state) {
                system_info_.current_slot = any_loaded ? loaded_slot : -1;
                spdlog::debug("[AMS AFC] Reconciliation: current_slot={} (from lane scan)",
                              system_info_.current_slot);
            } else {
                spdlog::debug("[AMS AFC] Reconciliation: preserving current_slot={} (set by AFC "
                              "state), scan found loaded_slot={}",
                              system_info_.current_slot, loaded_slot);
            }
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
        // Track the slot BEFORE extruder parsing so we can detect if an active
        // tool's lane_loaded updated current_slot (more authoritative than the
        // default tool_to_slot_map used by reconciliation below).
        int slot_before_extruder = system_info_.current_slot;
        bool extruder_set_active_slot = false;
        if (!extruder_names_.empty()) {
            for (const auto& ext_name : extruder_names_) {
                std::string key = "AFC_extruder " + ext_name;
                if (params.contains(key) && params[key].is_object()) {
                    parse_afc_extruder(ext_name, params[key]);
                    state_changed = true;
                }
            }
        } else {
            // Backward compat: single extruder fallback
            if (params.contains("AFC_extruder extruder") &&
                params["AFC_extruder extruder"].is_object()) {
                parse_afc_extruder("extruder", params["AFC_extruder extruder"]);
                state_changed = true;
            }
        }
        // If an active tool's extruder updated current_slot, don't let
        // reconciliation override it with the default tool_to_slot_map
        if (system_info_.current_slot != slot_before_extruder && system_info_.current_tool >= 0) {
            extruder_set_active_slot = true;
        }

        // Parse unit-level Klipper objects (AFC_BoxTurtle, AFC_OpenAMS, AFC_vivid).
        // These build the multi-unit layout (parse_afc_unit_object →
        // rebuild_unit_map_from_klipper → reorganize_slots), so they MUST run before
        // anything that resolves a lane to a unit. Parsing buffers first meant every
        // buffer in the first frame resolved against the synthetic single unit
        // initialize_slots() creates, and a five-unit rig put all five buffers on
        // unit 0 (bundle XGVDYEB5).
        for (auto& unit_info : unit_infos_) {
            if (params.contains(unit_info.klipper_key) &&
                params[unit_info.klipper_key].is_object()) {
                parse_afc_unit_object(unit_info, params[unit_info.klipper_key]);
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

        // Parse toolchanger.tool_number from Klipper status updates.
        // In mixed toolchanger + AFC systems, when a print starts and the
        // slicer sends a tool command (e.g. T0), Klipper updates
        // toolchanger.tool_number before AFC firmware sends its own update.
        // Without this, current_tool stays stale and the reconciliation
        // block below picks the wrong slot.
        if (params.contains("toolchanger") && params["toolchanger"].is_object()) {
            const auto& tc_data = params["toolchanger"];
            if (tc_data.contains("tool_number") && tc_data["tool_number"].is_number_integer()) {
                int tool_num = tc_data["tool_number"].get<int>();
                if (tool_num != system_info_.current_tool) {
                    spdlog::debug("[AMS AFC] Toolchanger tool_number update: T{} (was T{})",
                                  tool_num, system_info_.current_tool);
                    system_info_.current_tool = tool_num;
                    state_changed = true;
                }
            }

            // Record what is on the carriage as a fact. The derivation that acts
            // on it runs once, after every parser has had its say (#1229).
            // `status` gates the read: mid-change Klipper sets tool_number to the
            // incoming tool before it is physically picked up, so a raw read
            // would report MOUNTED during the swap.
            const MountState previous_mount = system_info_.mount_state;
            std::string tc_status;
            if (tc_data.contains("status") && tc_data["status"].is_string()) {
                tc_status = tc_data["status"].get<std::string>();
            }

            if (!tc_status.empty() && tc_status != "ready") {
                system_info_.mount_state = MountState::CHANGING;
                system_info_.mounted_tool = -1;
            } else if (system_info_.current_tool >= 0) {
                system_info_.mount_state = MountState::MOUNTED;
                system_info_.mounted_tool = system_info_.current_tool;
            } else {
                system_info_.mount_state = MountState::NONE;
                system_info_.mounted_tool = -1;
            }

            if (system_info_.mount_state != previous_mount) {
                spdlog::debug("[AMS AFC] Mount state: {} -> {} (tool T{})",
                              mount_state_to_string(previous_mount),
                              mount_state_to_string(system_info_.mount_state),
                              system_info_.mounted_tool);
                state_changed = true;
            }
        }

        // Tool changer reconciliation: when we have a live tool-to-slot
        // mapping and an active tool, the tool authoritatively determines
        // current_slot. During tool swaps current_load may briefly go null
        // while current_tool already reflects the new tool.
        // Skip if AFC_extruder already set the slot for the active tool —
        // lane_loaded is more authoritative than the tool-to-slot map.
        // Uses the registry's live mapping (updated from lane "map" fields).
        if (!extruder_set_active_slot && !current_slot_set_by_afc_state &&
            slots_.is_initialized() && system_info_.current_tool >= 0) {
            int tool = system_info_.current_tool;
            int slot = slots_.slot_for_tool(tool);
            if (slot >= 0 && slot < slots_.slot_count()) {
                if (system_info_.current_slot != slot) {
                    spdlog::debug("[AMS AFC] Tool changer reconciliation: T{} -> slot {} "
                                  "(was {})",
                                  tool, slot, system_info_.current_slot);
                }
                system_info_.current_slot = slot;
                system_info_.filament_loaded = true;
            }
        }

        // Single authority for machines that have a carriage. Runs last, so it
        // overrides whatever the individual parsers negotiated among themselves
        // (#1229). No-op on every backend without a toolchanger.
        apply_mount_state(extruder_set_active_slot, afc_stated_unloaded);

        // Backstop for a frame that never terminates the operation. Runs with
        // mutex_ held; the emit happens below, outside it. A flip to ERROR is
        // itself a state change worth publishing even on a frame that touched
        // nothing else.
        const AmsAction action_before_timeout = system_info_.action;
        check_action_timeout();
        if (system_info_.action != action_before_timeout) {
            state_changed = true;
        }
    }

    // Emit events OUTSIDE the lock to avoid deadlock with callbacks
    if (!deferred_error_event.empty()) {
        emit_event(EVENT_ERROR, deferred_error_event);
    }

    // Pop the next queued AFC message if a clear_fault() drain is in flight.
    // Must be outside the lock — clear_message_queue() sends gcode.
    maybe_drain_message_queue();

    if (state_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendAfc::apply_state_string(const std::string& raw, const char* source) {
    bool recognized = true;
    system_info_.action = ams_action_from_string(raw, &recognized);
    system_info_.operation_detail = afc_state_detail(raw);
    // Keys the stuck-action latch. Deliberately the raw wire token, not the
    // humanized detail: two different states can share a display label.
    last_raw_state_ = raw;

    // AFC reporting anything busy means it has taken the operation over, so its
    // own state machine owns completion from here and the pending macro ack must
    // keep its hands off — forcing IDLE underneath a live 67s toolchange would
    // truncate it. A re-echoed "Idle" is deliberately NOT taking over: that is
    // exactly the no-op this mechanism exists to catch (#1183), so it leaves the
    // pending dispatch alone. (The ack's own value guard makes that case a
    // no-op anyway, since the frame already produced the IDLE transition the UI
    // needed.)
    if (pending_dispatch_action_.has_value() && system_info_.action != AmsAction::IDLE) {
        spdlog::debug("[AMS AFC] AFC took over the dispatched operation ({} from {} '{}')",
                      ams_action_to_string(system_info_.action), source, raw);
        pending_dispatch_action_.reset();
    }

    if (!recognized && !raw.empty() && unknown_state_warned_.insert(raw).second) {
        // Schema drift: AFC reported a state outside our known vocabulary. The
        // fuzzy fallback picked an action, but the mapping is a guess and the
        // detail text is machine-humanized rather than translated. Logged once
        // per distinct string so a rename shows up in logs instead of silently
        // reading as IDLE.
        spdlog::warn("[AMS AFC] Unrecognized {} '{}' — mapped to {} by fallback. AFC may have "
                     "reworded its state vocabulary; update ams_action_from_string().",
                     source, raw, ams_action_to_string(system_info_.action));
    }

    spdlog::trace("[AMS AFC] {}: {} ({} -> '{}')", source,
                  ams_action_to_string(system_info_.action), raw, system_info_.operation_detail);
}

void AmsBackendAfc::apply_action_timeout_latch_locked() {
    if (!timed_out_state_.has_value()) {
        return;
    }

    if (*timed_out_state_ != last_raw_state_) {
        // AFC moved on to a genuinely different state: the stuck operation
        // resolved, or something replaced it. Stop overriding and let the
        // firmware drive the action again.
        spdlog::info("[AMS AFC] Stuck-action latch released: '{}' -> '{}'", *timed_out_state_,
                     last_raw_state_);
        timed_out_state_.reset();
        timed_out_detail_.clear();
        return;
    }

    // Same stuck string as when the budget blew. AFC re-derives the action from
    // that string on every frame, so without this override the normal mapping
    // would put the backend straight back into the busy action, the frame-level
    // change would restart the clock, and the whole thing would flap between
    // busy and ERROR for as long as AFC stays stuck.
    system_info_.action = AmsAction::ERROR;
    system_info_.operation_detail = timed_out_detail_;
}

void AmsBackendAfc::check_action_timeout() {
    // Already latched: the budget has fired for this state string and the clock
    // means nothing until AFC reports something else. apply_action_timeout_
    // latch_locked() owns the state from here.
    if (timed_out_state_.has_value()) {
        return;
    }

    const AmsAction a = system_info_.action;
    // IDLE has no operation to time out and ERROR is already the terminal state
    // this would produce. PAUSED is legitimately indefinite — AFC is waiting on
    // the user to clear a jam or swap a spool, and failing that into ERROR would
    // discard a prompt they are in the middle of answering.
    if (a == AmsAction::IDLE || a == AmsAction::ERROR || a == AmsAction::PAUSED) {
        return;
    }

    int limit = ACTION_TIMEOUT_SECONDS;
    if (a == AmsAction::SELECTING) {
        limit = SELECTING_TIMEOUT_SECONDS;
    } else if (a == AmsAction::HEATING) {
        limit = HEATING_TIMEOUT_SECONDS;
    } else if (a == AmsAction::PURGING) {
        limit = PURGING_TIMEOUT_SECONDS;
    } else if (a == AmsAction::LOADING || a == AmsAction::UNLOADING) {
        limit = LOAD_UNLOAD_TIMEOUT_SECONDS;
    }

    const auto elapsed = std::chrono::steady_clock::now() - action_start_time_;
    if (elapsed < std::chrono::seconds(limit)) {
        return;
    }

    spdlog::warn("[AMS AFC] {} (state '{}') timed out after {}s of a {}s budget, surfacing ERROR",
                 ams_action_to_string(a), last_raw_state_,
                 std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), limit);

    // Preserve what was happening as the error description so the error-center
    // bridge shows more than a bare failure.
    const std::string timeout_detail = system_info_.operation_detail.empty()
                                           ? lv_tr("Filament operation timed out")
                                           : system_info_.operation_detail + lv_tr(" (timed out)");
    system_info_.action = AmsAction::ERROR;
    system_info_.operation_detail = timeout_detail;

    // Only latch when AFC itself is driving the action. The latch keys on
    // last_raw_state_, and for an action this backend set optimistically at
    // dispatch that token is still AFC's previous state — usually "Idle".
    // Latching it would have apply_action_timeout_latch_locked() re-force ERROR
    // on every subsequent "Idle" frame for the rest of the session. There is
    // nothing to flap against in that case anyway: AFC is not re-deriving this
    // action from a string, so the next frame simply maps "Idle" to IDLE and
    // the operation ends.
    if (pending_dispatch_action_.has_value() && a == *pending_dispatch_action_) {
        pending_dispatch_action_.reset();
        return;
    }
    timed_out_state_ = last_raw_state_;
    timed_out_detail_ = timeout_detail;
}

uint64_t AmsBackendAfc::begin_dispatch_locked(AmsAction action) {
    // A newer dispatch supersedes any older one whose ack is still in flight:
    // that ack presents a stale generation and finalize_dispatch_after_macro()
    // drops it, so it can never resolve the operation now running.
    const uint64_t generation = ++dispatch_generation_;
    pending_dispatch_action_ = action;

    // The user starting a new operation supersedes the previous one's stuck
    // state, exactly as clear_fault() does. Leaving the latch armed would have
    // the next status frame re-force ERROR over the action just set and the new
    // operation would never be visible at all. If AFC really is still wedged,
    // the clock restarts here and the budget re-fires — bounded and correct.
    if (timed_out_state_.has_value()) {
        spdlog::debug("[AMS AFC] New dispatch: releasing stuck-action latch on '{}'",
                      *timed_out_state_);
        timed_out_state_.reset();
        timed_out_detail_.clear();
    }

    system_info_.action = action;
    // Otherwise the sidebar keeps showing the finished operation's detail until
    // AFC's first frame, which for a no-op never comes.
    switch (action) {
    case AmsAction::UNLOADING:
        system_info_.operation_detail = lv_tr("Unloading");
        break;
    case AmsAction::SELECTING:
        system_info_.operation_detail = lv_tr("Tool swap");
        break;
    default:
        system_info_.operation_detail = lv_tr("Loading");
        break;
    }

    // parse_afc_state() stamps the action clock once per status frame; a
    // dispatch happens outside any frame, so it has to stamp its own. Without
    // this the new action inherits however long the previous one had already
    // been running and check_action_timeout() can fire on its first poll.
    action_start_time_ = std::chrono::steady_clock::now();

    spdlog::debug("[AMS AFC] Dispatch #{}: action set optimistically to {}", generation,
                  ams_action_to_string(action));
    return generation;
}

void AmsBackendAfc::on_home_confirmation_declined() {
    // The confirmation modal is exclusive -- nothing else can begin a new
    // dispatch while it's up -- so the pending dispatch is always the one
    // that just prompted; that exclusivity is what makes this call correct,
    // not the generation compare inside abandon_dispatch(). abandon_dispatch()
    // takes an explicit generation to share its guard with dispatch_operation()'s
    // own `if (!result)` failure path, which captures a real, independent
    // value before this exclusivity window even opens. Here there is no such
    // independent capture: the value handed in is dispatch_generation_ itself,
    // so the compare is trivially true and abandon_dispatch() always proceeds
    // when a dispatch is pending. Read it under mutex_ rather than as a bare
    // member access (every write to dispatch_generation_ is mutex_-guarded,
    // in begin_dispatch_locked()) so this stays race-free even though nothing
    // can invalidate it today. abandon_dispatch() clears pending_dispatch_
    // action_/operation_detail and resets action_start_time_ in addition to
    // the action -> IDLE reset the base class's default performs; skip the
    // base call entirely here since abandon_dispatch() already emits
    // EVENT_STATE_CHANGED.
    //
    // If this hook ever gains a non-modal caller, this guard alone will not
    // protect a genuinely newer dispatch from being abandoned -- that would
    // need the generation captured at prompt time and threaded through here
    // instead of re-read live.
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation = dispatch_generation_;
    }
    abandon_dispatch(generation);
}

void AmsBackendAfc::abandon_dispatch(uint64_t generation) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != dispatch_generation_ || !pending_dispatch_action_.has_value()) {
            return;
        }
        pending_dispatch_action_.reset();
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
        action_start_time_ = std::chrono::steady_clock::now();
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendAfc::finalize_dispatch_after_macro(uint64_t generation) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // "Is the action still mine?" — three ways it can be someone else's:
        //   * a newer dispatch bumped the generation;
        //   * AFC reported a busy state, so apply_state_string() handed the
        //     operation to AFC's own state machine and cleared the pending mark;
        //   * something moved the action off the value this dispatch set —
        //     AFC echoing "Idle" (which already produced the transition the UI
        //     needed), or the stuck-action timeout latching ERROR.
        // In all three the operation is already resolved or is still legitimately
        // running, and forcing IDLE would either lie or truncate it.
        if (generation != dispatch_generation_ || !pending_dispatch_action_.has_value() ||
            system_info_.action != *pending_dispatch_action_) {
            spdlog::debug("[AMS AFC] Macro ack for dispatch #{} resolves nothing (current action "
                          "{}, generation {})",
                          generation, ams_action_to_string(system_info_.action),
                          dispatch_generation_);
            return;
        }

        // The macro ran to completion and AFC never reported doing anything —
        // the "lane3 already loaded" no-op. The gcode ack is the only completion
        // signal that exists for it (#1183).
        spdlog::info("[AMS AFC] Macro complete (gcode ack) with no AFC state change -> IDLE");
        pending_dispatch_action_.reset();
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
        action_start_time_ = std::chrono::steady_clock::now();
        changed = true;
    }
    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

AmsError AmsBackendAfc::dispatch_operation(std::string gcode, AmsAction action) {
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation = begin_dispatch_locked(action);
    }
    // Publish the optimistic action immediately: this is the transition the
    // filament panel's completion observer needs to see start before it can
    // ever see one end.
    emit_event(EVENT_STATE_CHANGED);

    auto token = lifetime_.token();
    AmsError result = ensure_homed_then(std::move(gcode), [this, token, generation]() {
        // L081 Mechanism C: the gcode ack lands on a background thread and the
        // handler writes system_info_ under mutex_. Marshal to main.
        token.defer("AmsBackendAfc::dispatch_macro_complete",
                    [this, generation]() { finalize_dispatch_after_macro(generation); });
    });

    if (!result) {
        // The gcode never left: no IMoonrakerAPI, or the send was refused. No ack
        // will ever arrive, so undo the optimistic action instead of leaving the
        // UI busy until the stuck-action budget expires.
        spdlog::warn("[AMS AFC] Dispatch #{} failed to send ({}), reverting optimistic action",
                     generation, result.technical_msg);
        abandon_dispatch(generation);
    }
    return result;
}

void AmsBackendAfc::parse_afc_state(const nlohmann::json& afc_data,
                                    std::string& deferred_error_event,
                                    bool& current_slot_set_by_afc_state,
                                    bool& afc_stated_unloaded) {
    // Stuck-action clock: captured here and compared at the very bottom of this
    // function. See the tail block for why the comparison is frame-scoped.
    const AmsAction action_at_frame_start = system_info_.action;

    // Version, when upstream supplies it here. AFC is moving the signal into the
    // status object (AFCProject/AFC-Klipper-Add-On PR #807 adds AFC.version to
    // get_status()); the old afc-install DB namespace has been dead since their
    // commit 7d20db7 and is not being repopulated. Prefer status when present and
    // fall back to whatever detect_afc_version() found, so this is a no-op on
    // releases that predate #807.
    //
    // INFORMATIONAL ONLY — do not gate behaviour on the value. AFC_VERSION is a
    // hand-bumped literal that has already drifted from the release tag (it sat at
    // 1.1.37 through the whole v1.2.0 release; #807 moves it to 1.2.1). Presence of
    // the field is the only trustworthy signal. Capabilities stay feature-detected.
    if (afc_data.contains("version") && afc_data["version"].is_string()) {
        std::string v = afc_data["version"].get<std::string>();
        if (!v.empty() && v != afc_version_) {
            afc_version_ = v;
            system_info_.version = v;
            spdlog::info("[AMS AFC] AFC version from status: {} (informational; capabilities "
                         "are feature-detected)",
                         v);
        }
    }

    // Parse current lane — try "current_lane" first, fall back to "current_load".
    // An empty string is treated as absent (fall through to check the other field).
    std::string loaded_lane;
    if (afc_data.contains("current_lane") && afc_data["current_lane"].is_string()) {
        loaded_lane = afc_data["current_lane"].get<std::string>();
    }
    if (loaded_lane.empty() && afc_data.contains("current_load") &&
        afc_data["current_load"].is_string()) {
        loaded_lane = afc_data["current_load"].get<std::string>();
    }

    // Delta semantics: only reconsider the active lane when AFC actually mentions
    // it. A present string sets it, a present null clears it, and absence leaves
    // it alone — AFC omits unchanged keys, and clearing on absence would drop the
    // attribution on the next unrelated delta (message, state, toolchange count).
    const bool mentions_lane = afc_data.contains("current_lane");
    const bool mentions_load = afc_data.contains("current_load");
    if (mentions_lane || mentions_load) {
        active_load_lane_ = loaded_lane;
    }

    // Track current_load on its own, in addition to folding it into
    // active_load_lane_ above. The two answer different questions and AFC
    // publishes them from different variables:
    //
    //   current_load  = AFC.current          — set by set_loaded(), cleared by
    //                                          set_unloaded(). The lane the
    //                                          extruder is actually gripping.
    //   current_lane  = AFC.current_loading  — set at the top of TOOL_LOAD and
    //                                          TOOL_UNLOAD, cleared only on
    //                                          success. The lane a toolchange
    //                                          is WORKING ON, loaded or not.
    //
    // active_load_lane_ prefers current_lane because attribution wants the lane
    // being worked. "Is the toolhead occupied" wants current_load and only
    // current_load — see can_recover_lane_position()'s toolhead guard.
    if (mentions_load) {
        toolhead_lane_ = afc_data["current_load"].is_string()
                             ? afc_data["current_load"].get<std::string>()
                             : std::string();
    }

    if (!loaded_lane.empty()) {
        int slot_index = slots_.index_of(loaded_lane);
        if (slot_index >= 0) {
            system_info_.current_slot = slot_index;
            current_slot_set_by_afc_state = true;
            // Derive current_tool from slot's mapped tool
            int mapped = slots_.tool_for_slot(slot_index);
            if (mapped >= 0) {
                system_info_.current_tool = mapped;
                spdlog::trace("[AMS AFC] Derived current_tool T{} from slot {}", mapped,
                              slot_index);
            }
            spdlog::debug("[AMS AFC] Current lane: '{}' (slot {})", loaded_lane,
                          system_info_.current_slot);
        } else {
            spdlog::warn("[AMS AFC] Current lane '{}' not found in slot registry ({} slots) "
                         "— toolhead loaded state will not display correctly",
                         loaded_lane, slots_.slot_count());
        }
    }

    // Filament-loaded state is DERIVED. The AFC object has never published a
    // "filament_loaded" key, nor a "current_tool" one — AFC.py get_status()
    // (v1.2.0:2531-2564, and the v1.1.0 equivalent) publishes current_load,
    // current_lane, next_lane, current_state, current_toolchange,
    // number_of_toolchanges, spoolman, td1_present, lane_data_enabled,
    // error_state, bypass_state, quiet_mode, position_saved, units, lanes, maps,
    // extruders, hubs, buffers, message and led_state. The tool number therefore
    // only ever comes from the slot→tool mapping resolved above.
    if (!loaded_lane.empty()) {
        // current_load/current_lane being set to a lane name is treated as
        // "loaded" — it may briefly read loaded during an in-progress load,
        // but it is the best signal the object carries.
        system_info_.filament_loaded = true;
        spdlog::debug("[AMS AFC] Filament loaded=true (derived from current_lane='{}')",
                      loaded_lane);
    } else if ((afc_data.contains("current_load") && afc_data["current_load"].is_null()) ||
               (afc_data.contains("current_lane") && afc_data["current_lane"].is_null())) {
        // current_load/current_lane went null (unloaded) — clear filament state.
        system_info_.filament_loaded = false;
        system_info_.current_slot = -1;
        system_info_.current_tool = -1;
        current_slot_set_by_afc_state = true;
        // A mounted tool must not re-elect a slot behind this. The carriage still
        // decides WHICH tool is current; it does not get to invent filament AFC
        // just told us is not there.
        afc_stated_unloaded = true;
        spdlog::debug("[AMS AFC] Filament unloaded (current lane/load=null)");
    }

    // Action comes from current_state, the only state key AFC publishes. (There
    // was a second branch here reading a "status" key as a legacy spelling; no
    // AFC version has ever emitted one on the AFC object — "status" exists only
    // on AFC_lane and AFC_extruder, and those are parsed by their own handlers.)
    if (afc_data.contains("current_state") && afc_data["current_state"].is_string()) {
        apply_state_string(afc_data["current_state"].get<std::string>(), "current_state");
    }

    // Parse tool change progress (AFC tracks swap count during multi-color prints).
    //
    // AFC.current_toolchange on the wire is a 1-BASED count of changes started,
    // not the 0-based index AmsSystemInfo stores. AFC increments its internal
    // counter before logging "Change N out of M", and get_status() clamps the
    // internal -1 sentinel on the way out (`self.current_toolchange if
    // self.current_toolchange >= 0 else 0`), so what we receive is exactly the
    // number AFC prints on the console. Subtracting one restores the documented
    // 0-based index (0 -> -1 "none yet", 1 -> 0 "first change"), which the UI
    // then adds back for display — without this the UI reads one change ahead
    // and renders "162 / 161" at the end of a print.
    if (afc_data.contains("current_toolchange") &&
        afc_data["current_toolchange"].is_number_integer()) {
        int afc_toolchange_count = afc_data["current_toolchange"].get<int>();
        // Floor at -1: the struct documents -1 as the "none yet" sentinel, and a
        // negative count from a firmware that stops clamping must not go past it.
        system_info_.current_toolchange = std::max(-1, afc_toolchange_count - 1);
        spdlog::trace("[AMS AFC] Current toolchange: AFC count {} -> index {}",
                      afc_toolchange_count, system_info_.current_toolchange);
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

                // A message is still queued behind the one we just cleared. Ask
                // the caller to pop another once it has released mutex_.
                if (message_drain_budget_ > 0) {
                    message_drain_pending_ = true;
                }
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
                // Error cleared - reset dedup tracking and the visible detail.
                // operation_detail outranks the action-derived string in
                // AmsState::recompute_action_detail(), so leaving it set pins the
                // sidebar status label to a stale error indefinitely.
                system_info_.operation_detail.clear();
                last_seen_message_.clear();
                last_error_msg_.clear();
                last_message_type_.clear();
                message_drain_budget_ = 0;
                message_drain_pending_ = false;
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
            current_slot_set_by_afc_state = true;
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
                    // "AFC_OpenAMS". Exception: "ViViD" → "AFC_vivid" (Klipper filename is
                    // lowercase, doesn't follow the strip-underscore convention).
                    std::string klipper_type;
                    if (info.type == "ViViD") {
                        klipper_type = "vivid";
                    } else {
                        klipper_type = info.type;
                        klipper_type.erase(
                            std::remove(klipper_type.begin(), klipper_type.end(), '_'),
                            klipper_type.end());
                    }
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
        hub_sensors_.clear();
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

        // Derive the toolchanger shape from the names, because the status
        // subscription is the ONLY surface that reaches us and it does not carry
        // AFC.system. Upstream writes str["system"]['num_extruders'] in exactly
        // two places — _webhooks_status() (the /printer/afc/status HTTP endpoint,
        // which we never call) and save_vars() (the vars file) — while
        // get_status() publishes current_load/current_lane/next_lane/
        // current_state/spoolman/error_state/units/lanes and no system key.
        //
        // Without this, num_extruders_ stayed at its default 1 on every real
        // printer, so `if (num_extruders_ > 1)` in load_filament() and
        // select_tool() was never true and AFC_SELECT_TOOL was never dispatched
        // on an actual AFC toolchanger. extruders_ stayed empty too, which the
        // bounds checks turned into silence rather than a crash.
        //
        // Order is preserved verbatim: extruders_ is indexed POSITIONALLY as a
        // tool number, and AFC emits the array in tool order.
        if (!extruder_names_.empty()) {
            num_extruders_ = static_cast<int>(extruder_names_.size());

            // Seed only what is missing. A system-sourced record carries
            // per-extruder lane_loaded and tool_stn distances that a bare name
            // cannot, so never overwrite one that is already populated.
            for (const auto& ext_name : extruder_names_) {
                auto it =
                    std::find_if(extruders_.begin(), extruders_.end(),
                                 [&](const AfcExtruderInfo& e) { return e.name == ext_name; });
                if (it == extruders_.end()) {
                    AfcExtruderInfo info;
                    info.name = ext_name;
                    extruders_.push_back(std::move(info));
                }
            }
        }
    }

    // Lane AFC has pre-staged for the NEXT toolchange (AFC.next_lane). Same
    // delta rule as current_lane: a string resolves it, an explicit null clears
    // it, and absence leaves the previous value alone. Resolution can fail while
    // the slot registry is still empty; next_slot then stays -1 and the next
    // frame that mentions the lane fixes it.
    if (afc_data.contains("next_lane")) {
        if (afc_data["next_lane"].is_string()) {
            std::string next = afc_data["next_lane"].get<std::string>();
            int next_idx = next.empty() ? -1 : slots_.index_of(next);
            system_info_.next_slot = next_idx;
            spdlog::trace("[AMS AFC] Next lane: '{}' (slot {})", next, next_idx);
        } else if (afc_data["next_lane"].is_null()) {
            system_info_.next_slot = -1;
        }
    }

    // Firmware holds a restorable toolhead position (set when an error
    // interrupted a print mid-move).
    if (afc_data.contains("position_saved") && afc_data["position_saved"].is_boolean()) {
        system_info_.position_saved = afc_data["position_saved"].get<bool>();
    }

    // Spoolman base URL the firmware is configured against. AFC publishes false
    // (not null) when Spoolman is off, so only a string is meaningful.
    if (afc_data.contains("spoolman") && afc_data["spoolman"].is_string()) {
        system_info_.spoolman_url = afc_data["spoolman"].get<std::string>();
    }

    // T-commands AFC registered with Klipper (v1.2.0+). Consumed by the per-lane
    // map cross-check in parse_afc_stepper; re-arm its dedup whenever the set
    // changes so a genuinely new mismatch is not swallowed by an earlier warning.
    if (afc_data.contains("maps") && afc_data["maps"].is_array()) {
        std::vector<std::string> cmds;
        for (const auto& m : afc_data["maps"]) {
            if (m.is_string()) {
                cmds.push_back(m.get<std::string>());
            }
        }
        if (cmds != afc_tool_cmds_) {
            afc_tool_cmds_ = std::move(cmds);
            tool_cmd_missing_warned_.clear();
            spdlog::debug("[AMS AFC] AFC registered {} tool commands", afc_tool_cmds_.size());
        }
    }

    // Virtual-tools firmware marker (#832): publishes multiple_tool_mapping
    // unconditionally, whatever the opt-in's value. Presence alone flips the
    // reset-mapping macro name — reading the bool would pin every stock install
    // to the old name on the very firmware that deregistered it.
    if (afc_data.contains("multiple_tool_mapping")) {
        afc_reset_mapping_renamed_ = true;
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
            toolhead_led_state_.clear();
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

                // Parse toolhead distances
                if (ext_data.contains("tool_stn") && ext_data["tool_stn"].is_number()) {
                    info.tool_stn = ext_data["tool_stn"].get<float>();
                }
                if (ext_data.contains("tool_stn_unload") &&
                    ext_data["tool_stn_unload"].is_number()) {
                    info.tool_stn_unload = ext_data["tool_stn_unload"].get<float>();
                }
                if (ext_data.contains("tool_sensor_after_extruder") &&
                    ext_data["tool_sensor_after_extruder"].is_number()) {
                    info.tool_sensor_after_extruder =
                        ext_data["tool_sensor_after_extruder"].get<float>();
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

    // --- Stuck-action bookkeeping (see check_action_timeout) ----------------
    //
    // Re-assert the latch last so it outranks both the state->action mapping
    // above and the AFC.message block, which also writes operation_detail.
    apply_action_timeout_latch_locked();

    // Stamp the action clock once per status frame, and only when the action
    // actually changed across the WHOLE frame.
    //
    // This deliberately does NOT live in apply_state_string(). That helper runs
    // on every frame AFC sends and up to twice within one (the legacy "status"
    // field, then the authoritative "current_state"). An unconditional stamp
    // there would restart the budget on every delta so it could never elapse;
    // an "action changed" stamp there would misfire whenever those two fields
    // disagree inside a single frame. Either way the timeout would never fire,
    // which is the bug this whole mechanism exists to prevent.
    if (system_info_.action != action_at_frame_start) {
        action_start_time_ = std::chrono::steady_clock::now();
    }
}

// ============================================================================
// AFC Object Parsing (AFC_stepper, AFC_hub, AFC_extruder)
// ============================================================================

void AmsBackendAfc::parse_afc_stepper(int slot_index, const std::string& lane_name,
                                      const nlohmann::json& data) {
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

    auto* entry = slots_.get_mut(slot_index);
    if (!entry) {
        spdlog::trace("[AMS AFC] Invalid slot index {} for lane: {}", slot_index, lane_name);
        return;
    }

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
    // Severity colour for the lane's status LED. Firmware splits one
    // get_filament_status() string into filament_status + this hex, so the two
    // always describe the same condition and are parsed together.
    if (data.contains("filament_status_led") && data["filament_status_led"].is_string()) {
        sensors.filament_status_led = data["filament_status_led"].get<std::string>();
    }
    if (data.contains("dist_hub") && data["dist_hub"].is_number()) {
        sensors.dist_hub = data["dist_hub"].get<float>();
    }
    // Selector sensor, published only by units that have one (HTLF, QuattroBox).
    // Presence is latched: a lane that once reported a selector still has one,
    // and a delta that omits the key must not read as "selector cleared".
    if (data.contains("selector") && data["selector"].is_boolean()) {
        sensors.has_selector = true;
        sensors.selector = data["selector"].get<bool>();
    }
    // Homing endstops configured on this lane, comma-separated. A capability
    // list, not a reading — AFC omits the key entirely on lanes with none.
    if (data.contains("endstops") && data["endstops"].is_string()) {
        sensors.endstops = data["endstops"].get<std::string>();
    }

    // Get slot info for filament data update
    auto& slot = entry->info;

    // Parse color.
    //
    // An EMPTY value is a deliberate clear, not a parse failure. AFC's
    // clear_values() sets color='' on eject, and SET_COLOR with an empty value
    // stores the literal '#'. Both strip to "" here. Previously std::stoul("")
    // threw and was swallowed as "keep existing", so an ejected lane kept
    // painting the previous spool's colour. Genuinely malformed input still
    // keeps the old value — only emptiness clears.
    if (data.contains("color") && data["color"].is_string()) {
        std::string color_str = data["color"].get<std::string>();
        // Remove '#' prefix if present
        if (!color_str.empty() && color_str[0] == '#') {
            color_str = color_str.substr(1);
        }
        if (color_str.empty()) {
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        } else {
            try {
                slot.color_rgb = std::stoul(color_str, nullptr, 16);
            } catch (...) {
                // Keep existing color on parse failure
            }
        }
    }

    // Parse material
    if (data.contains("material") && data["material"].is_string()) {
        slot.material = data["material"].get<std::string>();
    }

    // Filament name, as AFC copied it out of Spoolman's filament record
    // (AFC v1.2.0+). Empty is a deliberate clear — clear_values() sets
    // filament_name="" on eject — matching how `color` is handled above.
    // apply_overrides() runs below, so a user-entered name still wins.
    if (data.contains("filament_name") && data["filament_name"].is_string()) {
        slot.spool_name = data["filament_name"].get<std::string>();
    }

    // Multi-colour hexes (AFC v1.2.0+). Firmware carries these as a list of BARE
    // hex strings, having split Spoolman's comma-joined `multi_color_hexes` and
    // then re-prefixed only the first one into `color`. Normalise back to the
    // '#'-prefixed comma-joined form the swatch renderer and every other backend
    // use. An empty list clears, which is what clear_values() does on eject.
    if (data.contains("multi_color_hexes") && data["multi_color_hexes"].is_array()) {
        std::string joined;
        for (const auto& hex : data["multi_color_hexes"]) {
            if (!hex.is_string()) {
                continue;
            }
            std::string h = hex.get<std::string>();
            if (h.empty()) {
                continue;
            }
            if (!joined.empty()) {
                joined += ',';
            }
            if (h[0] != '#') {
                joined += '#';
            }
            joined += h;
        }
        slot.multi_color_hexes = joined;
    }

    // Recommended bed temperature (AFC v1.2.0+; v1.1.x carries it in lane_data
    // only). Null is the eject clear, same rule as spool_id below.
    if (data.contains("bed_temp")) {
        if (data["bed_temp"].is_number()) {
            slot.bed_temp = static_cast<int>(std::lround(data["bed_temp"].get<double>()));
        } else if (data["bed_temp"].is_null()) {
            slot.bed_temp = 0;
        }
    }

    // Recommended nozzle temperature. AFC_lane.get_status() has carried this on
    // every release since v1.1.0, and AFC_spool.py fills it from the linked
    // spool's Spoolman `settings_extruder_temp` — so it is a single recommended
    // print temperature, not a range, and it is None until a spool is linked.
    //
    // Both ends of the pair get it. SlotInfo models the nozzle as min/max because
    // that is what RFID and the filament DB supply, and active_material_provider
    // layers the pair over the DB as the tier-2 vendor preset. The value the UI
    // then preheats to is MaterialInfo::nozzle_recommended(), the midpoint —
    // so writing only the low end would target (spool + DB max) / 2, a
    // temperature neither source asked for, and could invert the range whenever
    // the spool prints hotter than its material's generic maximum.
    //
    // Null is the eject clear, same rule as bed_temp above: AFC_spool's
    // clear_values() sets extruder_temp = None, and letting a stale value ride
    // would preheat the next spool to the last one's temperature.
    if (data.contains("extruder_temp")) {
        if (data["extruder_temp"].is_number()) {
            int temp = static_cast<int>(std::lround(data["extruder_temp"].get<double>()));
            slot.nozzle_temp_min = temp;
            slot.nozzle_temp_max = temp;
        } else if (data["extruder_temp"].is_null()) {
            slot.nozzle_temp_min = 0;
            slot.nozzle_temp_max = 0;
        }
    }

    // Parse Spoolman ID.
    //
    // JSON null is AFC telling us the link is GONE — clear_values() sets
    // spool_id=None on eject. is_number_integer() is false for null, so this
    // previously retained the old id and an ejected lane stayed "linked",
    // which is what later aimed an edit's Spoolman write at the wrong spool.
    // An ABSENT key still means "unchanged": these are deltas, not snapshots.
    if (data.contains("spool_id")) {
        if (data["spool_id"].is_number_integer()) {
            slot.spoolman_id = data["spool_id"].get<int>();
        } else if (data["spool_id"].is_null()) {
            slot.spoolman_id = 0;
        }
        // Remember firmware's own word separately from the merged slot: the
        // §5 merge below re-supplies the retained override id, so
        // slot.spoolman_id alone can no longer tell whether AFC itself still
        // holds a link. The spool-id re-assert (see
        // maybe_reassert_retained_spool_link) keys off this, not the merge.
        lane_firmware_spool_id_[lane_name] = slot.spoolman_id;
    }

    // Parse weight.
    //
    // This is the ONLY weight source for AFC. AFC_lane.get_status() has carried a
    // live `weight` since v1.1.0, on every release, so no version gate is needed
    // and none of the lane_data staleness (see parse_lane_data) can reach us here.
    if (data.contains("weight") && data["weight"].is_number()) {
        slot.remaining_weight_g = data["weight"].get<float>();
    }

    // Full-spool weight (AFC v1.2.0+), ONLY for lanes with a Spoolman link.
    //
    // The field is espooler_values.full_weight, which is a configured unit-level
    // constant (config `full_weight`, else the unit's) that Spoolman overwrites
    // with the spool's real initial_weight when a spool is linked. Every lane
    // reports it, empty ones included, so adopting it ungated would give an
    // ejected lane a total_weight_g of 1000 and render it as "0 / 1000 g" —
    // total_weight_g's convention is -1 for unknown. spool_id was parsed just
    // above, so slot.spoolman_id is this frame's value.
    //
    // Deliberately does NOT clear on unlink: total_weight_g also comes from the
    // Spoolman weight poll and from user overrides, and neither should be wiped
    // because AFC dropped its own link.
    if (slot.spoolman_id > 0 && data.contains("initial_weight") &&
        data["initial_weight"].is_number()) {
        float full = data["initial_weight"].get<float>();
        if (full > 0.0f) {
            slot.total_weight_g = full;
        }
    }

    // Whether AFC itself retains this lane's spool metadata across an eject.
    // When false, clear_values() wipes colour/material/weight in firmware and
    // our override store is the only thing that puts the user's identity back.
    if (data.contains("remember_spool") && data["remember_spool"].is_boolean()) {
        lane_remember_spool_[lane_name] = data["remember_spool"].get<bool>();
    }

    // Vendor/brand — see read_vendor().
    //
    // Upstream #808 asked for the vendor on BOTH surfaces; #833 ships it as
    // `spool_vendor` here in get_status. The status half is the one that matters to
    // us: it is live and version-independent, where lane_data is a DB snapshot that
    // only refreshes when AFC decides to push. Inert on older firmware; harmless
    // there.
    //
    // apply_overrides() runs directly below, so a user's brand override still wins
    // over whatever firmware reports. The lane_data path does the same since #1195.
    read_vendor(data, slot.brand);

    // Re-supply the user's attached identity on top of firmware truth. This is
    // what keeps a lane's spool across an eject now that the parser honours
    // AFC's clears.
    apply_overrides(slot, slot.global_index >= 0 ? slot.global_index : slot.slot_index);

    // Derive slot status from sensors and status string.
    // Only recompute status when at least one status-related field is present
    // in the update. Partial updates (e.g., weight-only) must not regress the
    // slot from LOADED to AVAILABLE by defaulting tool_loaded to false.
    bool has_tool_loaded = data.contains("tool_loaded") && data["tool_loaded"].is_boolean();
    bool has_status = data.contains("status") && data["status"].is_string();

    // Filament presence BEFORE this frame's recompute — the spool-id
    // re-assert below fires on the empty -> loaded EDGE, so it needs the
    // lane's prior state, and slot.status is that state until rewritten.
    const SlotStatus status_at_frame_start = slot.status;

    if (has_tool_loaded || has_status || data.contains("prep") || data.contains("load")) {
        bool tool_loaded = has_tool_loaded && data["tool_loaded"].get<bool>();
        std::string status_str = has_status ? data["status"].get<std::string>() : "";

        // The lane `status` vocabulary is closed: AFCLaneState in
        // AFC_lane.py:65-76 (v1.2.0; v1.1.0:55-64 is the same list minus
        // "Infinite Runout"). Nothing outside it is ever published.
        //
        // "Loaded" means loaded to the HUB, not to the toolhead
        // (AFC_lane.py:1497). The two that mean toolhead are "Tooled"
        // (AFC_lane.py:1523) and "Tool Loaded", the latter set the moment
        // filament trips the pre-extruder sensor (AFC.py v1.2.0:1633 and :1764,
        // v1.1.0:1360). "Tool Loaded" used to fall through to the catch-all and
        // read as EMPTY unless a cached prep/load sensor happened to rescue it.
        //
        // There is no "Ready" lane status in any AFC version. That belongs to
        // the SEPARATE `filament_status` field, whose vocabulary is
        // In Tool / Ready / Prep / Not Ready (AFC_functions.py:407-414).
        if (tool_loaded || status_str == "Tooled" || status_str == "Tool Loaded") {
            slot.status = SlotStatus::LOADED;
        } else if (status_str == "Loaded" || sensors.prep || sensors.load) {
            slot.status = SlotStatus::AVAILABLE;
        } else if (status_str == "None" || status_str.empty()) {
            slot.status = SlotStatus::EMPTY;
        } else {
            // Unrecognized status (e.g. "Error" after a failed load on an empty
            // lane). We only reach here when prep, load and tool_loaded are all
            // false, so no filament is physically present — the lane is empty,
            // not available. The per-slot error badge below is set independently
            // of slot.status, so the error indicator is preserved.
            slot.status = SlotStatus::EMPTY;
        }
    }

    // Spool-id re-assert (#1289): fire once on the empty -> loaded edge.
    // Both LOADED (toolhead) and AVAILABLE (hub) mean filament is present, so
    // an AVAILABLE -> LOADED promotion within one load is NOT a new edge.
    const bool filament_present_now =
        slot.status == SlotStatus::LOADED || slot.status == SlotStatus::AVAILABLE;
    const bool filament_present_before = status_at_frame_start == SlotStatus::LOADED ||
                                         status_at_frame_start == SlotStatus::AVAILABLE;
    if (filament_present_now && !filament_present_before) {
        maybe_reassert_retained_spool_link(slot_index, lane_name);
    }

    // Populate or clear per-slot error based on lane status
    if (has_status) {
        std::string status_str = data["status"].get<std::string>();
        if (status_str == "Error") {
            SlotError err;
            err.message = last_seen_message_.empty() ? "Lane error" : last_seen_message_;
            err.severity =
                (last_message_type_ == "warning") ? SlotError::WARNING : SlotError::ERROR;
            slot.error = err;
            spdlog::debug("[AMS AFC] Lane {} (slot {}): error state - {}", lane_name, slot_index,
                          err.message);
        } else if (slot.error.has_value()) {
            // Lane exited error state - clear the error
            spdlog::debug("[AMS AFC] Lane {} (slot {}): error cleared", lane_name, slot_index);
            slot.error.reset();
        }
    }

    spdlog::trace("[AMS AFC] Lane {} (slot {}): prep={} load={} hub={} status={}", lane_name,
                  slot_index, sensors.prep, sensors.load, sensors.loaded_to_hub,
                  slot_status_to_string(slot.status));

    // Parse tool mapping from the "map" / "current_map" pair. This function receives
    // Moonraker notify_status_update DELTAS (only changed fields), so an ABSENT field
    // means "unchanged" and must NOT clear the mapping — same partial-delta rule the
    // status block above applies. Only a PRESENT value is authoritative:
    //   map: string "T0"        → AFC before virtual tools; one tool per lane
    //   map: array ["T0", ...]  → AFC with virtual tools (#605), where map is ALWAYS
    //                             a list, single-tool lanes included
    //   map: null/empty/junk    → unmapped, clear
    //   current_map: "T11"      → which of map's tools the lane is ACTUALLY on
    //
    // The two fields move independently — AFC_ADD_MAPPING sends map alone, a tool
    // change within a lane sends current_map alone — so each is handled on its own
    // and the chosen tool is recomputed from whatever we know after both.
    if (data.contains("current_map")) {
        // A lane with one tool may report current_map as null or "": upstream
        // describes it as holding the active tool "when more than one T(n) is mapped
        // to that lane". Empty therefore means "no news", never "unmap" — map is the
        // only field that may unmap a lane.
        const std::vector<int> current = parse_afc_lane_map(data["current_map"]);
        if (!current.empty())
            lane_current_tool_[lane_name] = current.front();
    }

    if (data.contains("map") || data.contains("current_map")) {
        // A current_map-only delta carries no tool list. That is not a problem: the
        // lane's tools did not change, only which of them is active, and the
        // remembered pick below is enough to retarget on its own.
        const bool has_map = data.contains("map");
        const std::vector<int> tools =
            has_map ? parse_afc_lane_map(data["map"]) : std::vector<int>{};

        if (has_map && tools.empty()) {
            // Present-but-null, empty list, or malformed → authoritative unmap.
            // Forget the remembered pick too, so a later remap that happens to list
            // the same tool cannot resurrect it without AFC restating current_map.
            lane_current_tool_.erase(lane_name);
            slots_.clear_tool_mapping(slot_index);
            firmware_mapped_slots_.erase(slot_index);
        } else {
            // One tool per lane is all SlotRegistry can express: set_tool_mapping()
            // drops the slot's previous tool from the forward map, so writing N tools
            // for one lane would leave only the last one reachable. Prefer the tool
            // AFC named in current_map; it is the only authority on which one a
            // multi-tool lane is driving. Fall back to the LOWEST when AFC has not
            // told us — pre-#605 firmware, or before the first current_map arrives.
            // That fallback is arbitrary-but-stable, not a claim about AFC's
            // ordering: its list is not sorted. The extras are not tracked, so a tool
            // change to one of them will not resolve to this lane until the registry
            // models many-to-one.
            const auto remembered = lane_current_tool_.find(lane_name);
            std::optional<int> tool_num;

            if (remembered != lane_current_tool_.end() &&
                (!has_map ||
                 std::find(tools.begin(), tools.end(), remembered->second) != tools.end())) {
                // map stays the authority on which tools a lane owns; current_map
                // only SELECTS among them. A remembered tool that a present map no
                // longer lists is stale (AFC_REMOVE_MAPPING) or drift we do not
                // understand — either way, drop it and fall back.
                tool_num = remembered->second;
            } else {
                if (remembered != lane_current_tool_.end())
                    lane_current_tool_.erase(remembered);
                if (!tools.empty())
                    tool_num = *std::min_element(tools.begin(), tools.end());
            }

            // No value means a current_map-only delta we could not corroborate and
            // no list to fall back on. Treat it as a partial delta and keep the
            // existing mapping — falling through here must NOT skip the rest of the
            // lane parse below, so this is a guard rather than an early return.
            if (tool_num.has_value()) {
                const int chosen = *tool_num;

                // Firmware-sourced: this is AFC's own `map` field coming back over
                // the subscription, which is the only write here that proves the
                // printer applied a mapping (#1270). set_slot_info()'s write is NOT
                // this — that one is our own intent, sent as SET_MAP a few lines
                // later.
                slots_.set_tool_mapping(slot_index, chosen,
                                        helix::printer::SlotRegistry::MappingSource::Firmware);
                firmware_mapped_slots_.insert(slot_index);
                spdlog::trace("[AMS AFC] Lane {} mapped to tool T{}", lane_name, chosen);

                // A T(n)-keyed lane_data payload that arrived before any mapping
                // existed could not resolve its records. This mapping may be the
                // one they were waiting for, and query_lane_data() is one-shot —
                // replay is their only second chance. parse_lane_data() re-parks
                // whatever still resolves to nothing.
                if (pending_tool_lane_data_.has_value()) {
                    nlohmann::json pending = std::move(*pending_tool_lane_data_);
                    pending_tool_lane_data_.reset();
                    parse_lane_data(pending);
                }

                if (tools.size() > 1 && multi_tool_warned_lanes_.insert(lane_name).second) {
                    // Logged in AFC's own order, which is not sorted.
                    std::string tool_list;
                    for (int t : tools)
                        tool_list += (tool_list.empty() ? "T" : ", T") + std::to_string(t);
                    spdlog::warn("[AMS AFC] Lane {} maps to {} tools ({}) — virtual tools (AFC "
                                 "#605); using T{}, the rest are not tracked",
                                 lane_name, tools.size(), tool_list, chosen);
                }

                // Cross-check against the T-commands AFC actually registered with
                // Klipper (AFC.maps, v1.2.0+). A lane claiming a tool that has no
                // registered command means change_tool() would send gcode the
                // firmware does not know. Diagnostic only — the lane's own map field
                // stays authoritative, since maps is absent entirely before v1.2.0.
                // Checks only the tool we actually mapped, for the same reason the
                // extras are dropped above.
                const std::string map_cmd = "T" + std::to_string(chosen);
                if (!afc_tool_cmds_.empty() &&
                    std::find(afc_tool_cmds_.begin(), afc_tool_cmds_.end(), map_cmd) ==
                        afc_tool_cmds_.end() &&
                    tool_cmd_missing_warned_.insert(chosen).second) {
                    spdlog::warn("[AMS AFC] Lane {} maps to {} but AFC registered no such command "
                                 "({} registered) — a tool change to T{} would fail",
                                 lane_name, map_cmd, afc_tool_cmds_.size(), chosen);
                }
            }
        }
    }
    // both fields absent → partial delta, keep existing mapping

    // Parse hub routing for this lane ("direct" or hub name like "HTLF_1")
    if (data.contains("hub") && data["hub"].is_string()) {
        std::string hub = data["hub"].get<std::string>();
        lane_hub_routing_[lane_name] = hub;
        spdlog::trace("[AMS AFC] Lane {} hub routing: {}", lane_name, hub);
    }

    // Extruder name, for shared-extruder dedup and for toolhead identity.
    // AFC names the SECTION here; SlotInfo::extruder_name is documented as the
    // Klipper name and its consumers parse it as one, so resolve at this
    // boundary rather than leaving every reader to guess which it holds.
    // Dedup works either way (it compares strings), so an unanswered configfile
    // costs identity, never the grouping.
    if (data.contains("extruder") && data["extruder"].is_string()) {
        slot.extruder_name = klipper_extruder_name_unlocked(data["extruder"].get<std::string>());
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

bool AmsBackendAfc::printer_retains_spool_info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // ALL semantics: any lane at remember_spool = false still clears on
    // eject, so the HelixScreen toggle keeps governing those lanes and must
    // stay enabled. A lane that never reported the key is conservatively
    // treated as not retaining too, and an empty map (nothing reported —
    // the everyday default) is not retaining either.
    if (lane_remember_spool_.empty() ||
        static_cast<int>(lane_remember_spool_.size()) != slots_.slot_count()) {
        return false;
    }
    return std::all_of(lane_remember_spool_.begin(), lane_remember_spool_.end(),
                       [](const auto& entry) { return entry.second; });
}

void AmsBackendAfc::maybe_reassert_retained_spool_link(int slot_index,
                                                       const std::string& lane_name) {
    // Callers hold mutex_ (parse_afc_stepper via handle_status_update).
    //
    // The #1289 convergence gap: with "Keep Spool Info on Eject" on we keep
    // a lane's spool identity in our override namespace, but AFC itself —
    // default remember_spool = false — ran clear_values() on eject. Re-insert
    // the same spool and HelixScreen paints the retained identity while AFC
    // (and Mainsail, which renders the plugin's state) shows an unknown
    // spool. On the empty -> loaded EDGE we push the retained id back into
    // AFC with the same SET_SPOOL_ID write the editor's re-link uses, so
    // all three views converge.
    //
    // Edge-triggered on purpose: frames while the lane stays loaded never
    // re-send, and a failed write is not retried — the next physical
    // eject/re-insert cycle is the retry. That is the whole debounce: a
    // bouncing spool costs one write per transition, never a stream.
    //
    // The write rides record_own_spool_write() like set_slot_info's own
    // re-link, so the firmware echo of OUR push cannot be misread by the
    // merge's re-bind rule as another writer's statement.
    const int override_key = [=]() {
        // Same key convention as the apply_overrides() call above us.
        auto* entry = slots_.get_mut(slot_index);
        return entry && entry->info.global_index >= 0 ? entry->info.global_index : slot_index;
    }();
    auto it = overrides_.find(override_key);
    if (it == overrides_.end() || it->second.spoolman_id <= 0) {
        return; // nothing retained for this lane — no identity to re-assert
    }
    if (!SettingsManager::instance().get_ams_keep_spool_info_on_eject()) {
        // Retention off: never push, even for a lingering record (the user
        // asked for lanes to start fresh on eject).
        return;
    }
    auto fw_it = lane_firmware_spool_id_.find(lane_name);
    const int firmware_id = fw_it != lane_firmware_spool_id_.end() ? fw_it->second : 0;
    if (firmware_id > 0) {
        // AFC already holds a link — its own remember_spool repopulated the
        // lane, or another writer (Mainsail) set one. Their statement wins;
        // the merge policy renders it.
        return;
    }

    const int retained_id = it->second.spoolman_id;
    spdlog::info("[AMS AFC] Lane {} (slot {}): re-asserting retained spool id {} into AFC",
                 lane_name, slot_index, retained_id);
    // Record before dispatching, exactly like set_slot_info's SET_SPOOL_ID
    // path: firmware_id (0 here) is what firmware last reported.
    record_own_spool_write(override_key, retained_id, firmware_id);
    AmsError err =
        execute_gcode(fmt::format("SET_SPOOL_ID LANE={} SPOOL_ID={}", lane_name, retained_id));
    if (!err) {
        // No retry loop — wait for the next empty -> loaded transition.
        spdlog::warn("[AMS AFC] Lane {} (slot {}): spool id re-assert dispatch failed: {}",
                     lane_name, slot_index, err.technical_msg);
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
    //   "error_sensitivity": 7,
    //   "state": "Advancing",
    //   "lanes": ["lane1", "lane2", "lane3", "lane4"],
    //   "fault_timer": 1.5,
    //   "rotation_distance": 22.67,
    //   "active_lane": "lane2",                 // v1.2.0+
    //   "multiplier": 1.1, "multiplier_high": 1.1, "multiplier_low": 0.9  // v1.2.0+
    // }

    // Remember which lanes this buffer serves. AFC rebuilds the whole status
    // dict every poll but Moonraker forwards only the CHANGED keys, so a frame
    // that moves `state` alone carries no `lanes` and could not otherwise be
    // routed to a unit.
    if (data.contains("lanes") && data["lanes"].is_array()) {
        std::vector<std::string> lanes;
        for (const auto& lane_json : data["lanes"]) {
            if (lane_json.is_string()) {
                lanes.push_back(lane_json.get<std::string>());
            }
        }
        buffer_lane_names_[buffer_name] = std::move(lanes);
    }

    // Accumulate into this buffer's own record, so the update is a
    // read-modify-write of what THIS buffer last reported. Building a fresh
    // BufferHealth and assigning it wholesale would zero every field the delta
    // happens not to mention; reading back the owning unit's copy instead would
    // fold in whatever buffer most recently claimed that unit.
    BufferHealth& health = buffer_health_[buffer_name];

    if (data.contains("fault_detection_enabled") && data["fault_detection_enabled"].is_boolean()) {
        health.fault_detection_enabled = data["fault_detection_enabled"].get<bool>();
    }

    if (data.contains("distance_to_fault") && data["distance_to_fault"].is_number()) {
        health.distance_to_fault = data["distance_to_fault"].get<float>();
    }

    if (data.contains("error_sensitivity") && data["error_sensitivity"].is_number()) {
        health.error_sensitivity = data["error_sensitivity"].get<float>();
    }

    if (data.contains("state") && data["state"].is_string()) {
        health.state = data["state"].get<std::string>();
    }

    // Lane the buffer is regulating (v1.2.0+). AFC publishes null whenever the
    // buffer is disabled or no lane is loaded, which is a real transition to
    // "none" rather than a missing field.
    if (data.contains("active_lane")) {
        if (data["active_lane"].is_string()) {
            health.active_lane = data["active_lane"].get<std::string>();
        } else if (data["active_lane"].is_null()) {
            health.active_lane.clear();
        }
    }

    // rotation_distance and fault_timer both go null when AFC has nothing to
    // report (buffer disabled / no lane loaded / fault detection off). -1 is the
    // struct's "not reported" sentinel, so map null onto it rather than keeping
    // a stale reading.
    if (data.contains("rotation_distance")) {
        if (data["rotation_distance"].is_number()) {
            health.rotation_distance = data["rotation_distance"].get<float>();
        } else if (data["rotation_distance"].is_null()) {
            health.rotation_distance = -1.0f;
        }
    }
    if (data.contains("fault_timer")) {
        if (data["fault_timer"].is_number()) {
            health.fault_timer = data["fault_timer"].get<float>();
        } else if (data["fault_timer"].is_null()) {
            health.fault_timer = -1.0f;
        }
    }

    // Rotation-distance multipliers (v1.2.0+). `multiplier` is the value last
    // applied; the high/low pair is the configured swing it moves between.
    // AFC seeds _last_multiplier with an int 1, so accept any number.
    if (data.contains("multiplier") && data["multiplier"].is_number()) {
        health.multiplier = data["multiplier"].get<float>();
    }
    if (data.contains("multiplier_high") && data["multiplier_high"].is_number()) {
        health.multiplier_high = data["multiplier_high"].get<float>();
    }
    if (data.contains("multiplier_low") && data["multiplier_low"].is_number()) {
        health.multiplier_low = data["multiplier_low"].get<float>();
    }

    spdlog::trace("[AMS AFC] Buffer {}: fault_detect={} dist={} sensitivity={} state={} "
                  "active_lane={} mult={} rot_dist={} fault_timer={}",
                  buffer_name, health.fault_detection_enabled, health.distance_to_fault,
                  health.error_sensitivity, health.state, health.active_lane, health.multiplier,
                  health.rotation_distance, health.fault_timer);

    // Buffer health lives at unit level — the buffer sits between hub and
    // toolhead, not per-lane.
    apply_buffer_health_to_units();
}

void AmsBackendAfc::apply_buffer_health_to_units() {
    for (const auto& [buffer_name, health] : buffer_health_) {
        auto lanes_it = buffer_lane_names_.find(buffer_name);
        if (lanes_it == buffer_lane_names_.end()) {
            continue;
        }
        AmsUnit* unit = nullptr;
        std::string matched_lane;
        for (const auto& lane_name : lanes_it->second) {
            int lane_idx = slots_.index_of(lane_name);
            if (lane_idx < 0) {
                continue;
            }
            // One buffer per unit — first lane that resolves decides.
            unit = system_info_.get_unit_for_slot(lane_idx);
            if (unit) {
                matched_lane = lane_name;
                break;
            }
        }
        // Log the attribution only when it changes. This runs on every frame
        // carrying a buffer OR a unit object, so an unconditional line here is
        // per-buffer-per-unit spam — and it is exactly the line that has to stay
        // readable in a bundle, because a buffer landing on the wrong unit is what
        // it is there to show.
        const int resolved = unit ? unit->unit_index : -1;
        auto attribution = buffer_unit_attribution_.find(buffer_name);
        const bool changed =
            attribution == buffer_unit_attribution_.end() || attribution->second != resolved;
        buffer_unit_attribution_[buffer_name] = resolved;

        if (!unit) {
            if (changed) {
                spdlog::trace("[AMS AFC] Buffer {}: no unit resolved yet", buffer_name);
            }
            continue;
        }
        unit->buffer_health = health;
        if (changed) {
            spdlog::debug("[AMS AFC] Buffer {} health set on unit {} (via lane {})", buffer_name,
                          unit->unit_index, matched_lane);
        }
    }
}

void AmsBackendAfc::parse_afc_extruder(const std::string& ext_name, const nlohmann::json& data) {
    // Parse AFC_extruder object for toolhead sensors
    // {
    //   "tool_start_status": true,   // Toolhead entry sensor
    //   "tool_end_status": false,    // Toolhead exit/nozzle sensor
    //   "lane_loaded": "lane1"       // Currently loaded lane
    // }

    // Per-extruder copy, keyed so slot_has_filament_at_toolhead() can attribute a
    // trip to the lane this extruder holds. Absent fields leave the previous
    // value alone — Moonraker sends deltas, and a frame that carries only
    // lane_loaded must not silently clear the sensors.
    AfcExtruderSensors& sensors = extruder_sensors_[ext_name];

    if (data.contains("tool_start_status") && data["tool_start_status"].is_boolean()) {
        tool_start_sensor_ = data["tool_start_status"].get<bool>();
        sensors.tool_start = tool_start_sensor_;
    }

    if (data.contains("tool_end_status") && data["tool_end_status"].is_boolean()) {
        tool_end_sensor_ = data["tool_end_status"].get<bool>();
        sensors.tool_end = tool_end_sensor_;
    }

    // AFC's own carriage signal. Absent on older AFC, so has_on_shuttle records
    // whether we may draw any conclusion at all — "not reported" and "reported
    // false" mean very different things when deciding whether a slot is current.
    if (data.contains("on_shuttle") && data["on_shuttle"].is_boolean()) {
        sensors.on_shuttle = data["on_shuttle"].get<bool>();
        sensors.has_on_shuttle = true;
    }

    // Explicit null is AFC saying "nothing seated here" (set_unloaded assigns
    // ""), which is information — clear the attribution. Absent is silence.
    if (data.contains("lane_loaded")) {
        if (data["lane_loaded"].is_string()) {
            sensors.lane_loaded = data["lane_loaded"].get<std::string>();
        } else if (data["lane_loaded"].is_null()) {
            sensors.lane_loaded.clear();
        }
    }

    // Toolchanger state (AFC v1.2.0 #768). An entry is created for every
    // AFC_extruder object we see so callers can distinguish "tool known, nothing
    // happening" from "tool never reported". Older AFC omits all three fields
    // and simply leaves the defaults in place.
    {
        AfcToolState& ts = tool_states_[ext_name];
        if (data.contains("status") && data["status"].is_string()) {
            ts.status = data["status"].get<std::string>();
        }
        if (data.contains("next_pickup") && data["next_pickup"].is_boolean()) {
            ts.next_pickup = data["next_pickup"].get<bool>();
        }
        if (data.contains("is_standalone") && data["is_standalone"].is_boolean()) {
            ts.is_standalone = data["is_standalone"].get<bool>();
        }
    }

    if (data.contains("lane_loaded") && !data["lane_loaded"].is_null()) {
        if (data["lane_loaded"].is_string()) {
            std::string lane = data["lane_loaded"].get<std::string>();
            int loaded_slot = slots_.index_of(lane);
            if (loaded_slot >= 0) {
                // Tool number comes from the extruder's th_extruder_name, NOT
                // from the AFC_extruder SECTION name — AFC itself indexes on
                // `config.get("extruder_name", <section name>)`
                // (AFC_extruder.py:222-223, consumed at
                // AFC_Toolchanger.py:231-232, both v1.2.0). The two coincide on
                // `[AFC_extruder extruder1]` and diverge on anything else, e.g.
                // `[AFC_extruder e1]\nextruder_name: extruder1`.
                //
                // The old substr(8) read the section name and threw on any name
                // that was not `extruder<N>`; the catch then mapped EVERY
                // toolhead to T0, so on such a machine each extruder in turn
                // claimed to be the active tool. -1 now means "unknown", and an
                // unknown tool makes no attribution claim at all.
                const int ext_tool = tool_index_for_extruder_unlocked(ext_name);

                // Only update current_slot if this extruder is the active tool
                // or no authoritative source has set it yet
                bool is_active_tool = (ext_tool >= 0) && (system_info_.current_tool >= 0) &&
                                      (ext_tool == system_info_.current_tool);
                if (ext_tool < 0) {
                    spdlog::trace("[AMS AFC] Extruder {}: lane_loaded={} ignored (tool number "
                                  "unresolved)",
                                  ext_name, lane);
                } else if (system_info_.current_slot < 0 || is_active_tool) {
                    current_lane_name_ = lane;
                    system_info_.current_slot = loaded_slot;
                    spdlog::trace("[AMS AFC] Extruder {} (T{}): lane_loaded={} -> slot {}",
                                  ext_name, ext_tool, lane, loaded_slot);
                } else {
                    spdlog::trace("[AMS AFC] Extruder {} (T{}): lane_loaded={} ignored "
                                  "(active tool=T{}, current_slot={})",
                                  ext_name, ext_tool, lane, system_info_.current_tool,
                                  system_info_.current_slot);
                }
            }
        }
    }

    spdlog::trace("[AMS AFC] Extruder {}: tool_start={} tool_end={} lane={}", ext_name,
                  tool_start_sensor_, tool_end_sensor_, current_lane_name_);
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

    // Derive topology from per-lane hub routing data.
    // The per-lane "hub" field ("direct" vs hub name) is the authoritative source.
    // Fallback to extruder/hub count heuristic if per-lane data unavailable.
    bool has_direct = false;
    bool has_hub_routed = false;
    unit_info.lane_is_hub_routed.clear();

    for (const auto& lane : unit_info.lanes) {
        auto it = lane_hub_routing_.find(lane);
        if (it == lane_hub_routing_.end()) {
            // Routing not known yet. Moonraker sends deltas and unit objects sort
            // before AFC_lane ones, so a frame can carry a unit while some of its
            // lanes have never been parsed. Counting that as `direct` is what
            // flipped a pure-hub unit to MIXED and drew one lane straight into a
            // toolhead of its own (#1229 defect 4) — a single unknown lane was
            // enough. Unknown is not direct; it contributes to neither side.
            unit_info.lane_is_hub_routed.push_back(false);
            continue;
        }

        // AFC reports "direct" or "direct_load" for lanes that go straight
        // to an extruder (no hub/merger). Any other value (e.g., "HTLF_1")
        // indicates the lane is routed through a hub.
        const bool is_hub = (it->second.rfind("direct", 0) != 0);
        unit_info.lane_is_hub_routed.push_back(is_hub);
        if (is_hub) {
            has_hub_routed = true;
        } else {
            has_direct = true;
        }
    }

    if (has_direct && has_hub_routed) {
        unit_info.topology = PathTopology::MIXED;
    } else if (has_hub_routed && unit_info.extruders.size() > 1 &&
               unit_info.extruders.size() == unit_info.lanes.size()) {
        // Each lane has its own hub AND its own extruder — parallel topology
        // with hub sensors along independent filament paths (e.g., ACE Pro as
        // lane loader through AFC: 4 lanes, 4 hubs, 4 extruders, 1 unit).
        unit_info.topology = PathTopology::PARALLEL;
    } else if (has_hub_routed) {
        unit_info.topology = PathTopology::HUB;
    } else if (has_direct && unit_info.extruders.size() > 1) {
        unit_info.topology = PathTopology::PARALLEL;
    } else if (!unit_info.hubs.empty() && unit_info.extruders.size() <= 1) {
        unit_info.topology = PathTopology::HUB;
    } else if (unit_info.extruders.size() > 1) {
        unit_info.topology = PathTopology::PARALLEL;
    } else {
        unit_info.topology = PathTopology::HUB; // default
    }

    spdlog::debug("[AMS AFC] Unit object '{}': {} lanes, {} extruders, {} hubs, {} buffers → {}",
                  unit_info.klipper_key, unit_info.lanes.size(), unit_info.extruders.size(),
                  unit_info.hubs.size(), unit_info.buffers.size(),
                  path_topology_to_string(unit_info.topology));

    // Reorganize only when ALL known units have lane data.
    // Triggering early (e.g., >=2) causes the slot registry rebuild to consume
    // stashed lane data before all units are accounted for, resulting in the
    // last-processed unit getting empty/default slot values.
    int units_with_lanes = 0;
    for (const auto& ui : unit_infos_) {
        if (!ui.lanes.empty()) {
            units_with_lanes++;
        }
    }
    if (units_with_lanes == static_cast<int>(unit_infos_.size())) {
        spdlog::debug("[AMS AFC] All {}/{} units have lane data, triggering reorganize",
                      units_with_lanes, unit_infos_.size());
        rebuild_unit_map_from_klipper();
    } else {
        spdlog::debug("[AMS AFC] Waiting for unit data: {}/{} units have lanes", units_with_lanes,
                      unit_infos_.size());
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
                        sys_unit.lane_is_hub_routed = ui.lane_is_hub_routed;
                        // For HUB units, derive physical tool label from extruder
                        // name. An unrecognisable name leaves hub_tool_label at its
                        // -1 "absent" default rather than inventing a number.
                        // ui.extruders holds AFC SECTION names, so this must go
                        // through the resolver — parsing them directly left every
                        // unit at -1 on a renamed config and defeated the
                        // cross-unit nozzle merge entirely.
                        if (ui.topology == PathTopology::HUB && ui.extruders.size() == 1) {
                            const int n = tool_index_for_extruder_unlocked(ui.extruders[0]);
                            if (n >= 0) {
                                sys_unit.hub_tool_label = n;
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

const nlohmann::json& AmsBackendAfc::database_item_value(const nlohmann::json& response) {
    static const nlohmann::json kNull;
    if (!response.is_object())
        return kNull;

    // send_jsonrpc delivers the whole JSON-RPC message, so the payload is at
    // result.value: {"jsonrpc","id","result":{"namespace","key","value":…}}.
    // Reading "value" off the top level found nothing, silently, on every reply
    // (prestonbrown/helixscreen#1148).
    //
    // Deliberately strict about the envelope rather than also accepting a bare
    // payload: a payload is an arbitrary object, so "envelope or payload?" is
    // undecidable — the afc-install payload is {"version":…}, which carries no
    // "value" key to key off. Callers that route through
    // IMoonrakerAPI::database_get_item get the payload pre-unwrapped and must
    // not pass it here.
    const auto result = response.find("result");
    if (result == response.end() || !result->is_object())
        return kNull;
    const auto value = result->find("value");
    return value != result->end() ? *value : kNull;
}

bool AmsBackendAfc::apply_afc_version_response(const nlohmann::json& response) {
    const nlohmann::json& value = database_item_value(response);
    if (!value.is_object())
        return false;

    const auto version = value.find("version");
    if (version == value.end() || !version->is_string())
        return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        afc_version_ = version->get<std::string>();
        system_info_.version = afc_version_;
    }
    // Informational only — never gate behavior on this. AFC removed the code that
    // writes the afc-install namespace (its commit 7d20db7, #451, 2025-06-16), so
    // the value is either absent or frozen at whatever it was before that date. A
    // live BoxTurtle reported "1.0.0" on 2026-07-26 while its payload proved
    // 1.0.32-era. Capabilities are detected from the data itself instead.
    spdlog::info("[AMS AFC] Reported AFC version: {} (informational; capabilities are "
                 "feature-detected)",
                 afc_version_);
    return true;
}

bool AmsBackendAfc::status_has_modern_fields(const nlohmann::json& lane_status) {
    return lane_status.contains("filament_name") || lane_status.contains("multi_color_hexes") ||
           lane_status.contains("initial_weight");
}

void AmsBackendAfc::probe_feature_level(const std::string& lane_object) {
    // An explicit, UNSCOPED query — no "fields" key, so Moonraker returns the
    // whole object. This is the only way to see the v1.2.0 keys: the standing
    // subscription enumerates its fields and does not ask for them.
    if (!client_ || feature_level_checked_) {
        return;
    }
    nlohmann::json params = {{"objects", {{lane_object, nlohmann::json()}}}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token, lane_object](const nlohmann::json& response) {
            token.defer("AmsBackendAfc::probe_feature_level", [this, response, lane_object]() {
                const auto* result = response.contains("result") ? &response["result"] : nullptr;
                if (!result || !result->contains("status") ||
                    !(*result)["status"].contains(lane_object)) {
                    spdlog::debug("[AMS AFC] Feature probe: no status for '{}'", lane_object);
                    return;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                check_afc_feature_level((*result)["status"][lane_object]);
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[AMS AFC] Feature probe query failed: {}", err.message);
        });
}

void AmsBackendAfc::check_afc_feature_level(const nlohmann::json& lane_status) {
    // Runs once, on the COMPLETE object returned by probe_feature_level()'s
    // explicit query — never on a status frame. See status_has_modern_fields().
    if (feature_level_checked_) {
        return;
    }
    feature_level_checked_ = true;

    const bool modern = status_has_modern_fields(lane_status);

    // Side effects only for a backend actually wired to a printer. An unwired
    // one is a harness fixture replaying a synthetic payload — most such
    // payloads carry none of the v1.2.0 keys and so read as legacy, which would
    // mean a config write and an upgrade toast per fixture (126 of them across
    // the [ams] suite) advising nobody to upgrade nothing.
    if (!client_) {
        spdlog::debug("[AMS AFC] Feature level probe: no client, advisory skipped");
        return;
    }

    spdlog::info("[AMS AFC] Feature level: AFC {} the v1.2.0 lane fields",
                 modern ? "publishes" : "does NOT publish");

    auto* config = Config::get_instance();
    if (!config) {
        return;
    }
    constexpr const char* NOTICE_SHOWN_KEY = "/ams/afc_upgrade_notice_shown";

    if (modern) {
        // Re-arm, so a downgrade is reported again rather than silently accepted.
        if (config->get<bool>(NOTICE_SHOWN_KEY, false)) {
            config->set<bool>(NOTICE_SHOWN_KEY, false);
            config->save();
        }
        return;
    }

    if (config->get<bool>(NOTICE_SHOWN_KEY, false)) {
        return; // Already told them once; do not nag on every boot.
    }
    config->set<bool>(NOTICE_SHOWN_KEY, true);
    config->save();

    // Advisory, not an error — nothing is broken, some detail is just missing.
    // Names the version for the user's benefit even though the trigger is
    // capability: "1.2.0" is actionable, "your payload lacks filament_name" is not.
    //
    // Leads with multi-color because that is the part the upgrade uniquely buys.
    // Filament names do NOT require it when Spoolman is configured: the identity
    // cache resolves vendor/name from the lane's spool_id and
    // resolve_filament_label() already uses it, which is why bundle L53W5PKG
    // showed "LDO Industry Blue" on a pre-1.2.0 lane while being told to upgrade
    // "for filament names". Multi-color is different — the automatic lane sync
    // only ever gets multi_color_hexes from lane_data, since apply_spool_to_slot()
    // (the one path that copies Spoolman's) serves manual external-spool
    // assignment, not the AFC lane refresh.
    //
    // Deliberately not branched on is_spoolman_available(): the feature probe and
    // Spoolman discovery both land during startup with no ordering guarantee, and
    // this notice is latched to fire once ever — a mis-timed read would pin the
    // wrong variant permanently. One sentence that is true either way costs
    // nothing and cannot go stale.
    ui_notification_info_with_action(
        lv_tr("AFC Update Available"),
        lv_tr("Upgrade to AFC 1.2.0 or newer for multi-color spools. It also adds filament "
              "names, which otherwise need Spoolman."),
        "afc_message");
}

bool AmsBackendAfc::apply_lane_data_response(const nlohmann::json& response) {
    const nlohmann::json& value = database_item_value(response);
    if (!value.is_object())
        return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        parse_lane_data(value);
    }
    return true;
}

void AmsBackendAfc::detect_afc_version() {
    if (!client_) {
        spdlog::warn("[AMS AFC] Cannot detect version: client is null");
        return;
    }

    // Query Moonraker database for AFC install version
    // Method: server.database.get_item
    // Namespace: afc-install (contains {"version": "1.0.0"})
    nlohmann::json params = {{"namespace", "afc-install"}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "server.database.get_item", params,
        [this, token](const nlohmann::json& response) {
            // L081 Mechanism C: marshal member writes + downstream calls to main.
            token.defer("AmsBackendAfc::detect_afc_version_success",
                        [this, response]() { apply_afc_version_response(response); });
        },
        [this, token](const MoonrakerError& err) {
            // L081 Mechanism C: marshal member writes to main.
            token.defer("AmsBackendAfc::detect_afc_version_error", [this, message = err.message]() {
                spdlog::warn("[AMS AFC] Could not detect AFC version: {}", message);
                std::lock_guard<std::mutex> lock(mutex_);
                afc_version_ = "unknown";
                system_info_.version = "unknown";
                // Don't query lane_data - we'll rely on discovered lanes from capabilities
            });
        },
        0,   // default timeout
        true // silent — probe only, don't show toast or log at error level
    );
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

    // Add unit-level Klipper objects (AFC_BoxTurtle, AFC_OpenAMS, AFC_vivid)
    for (const auto& unit_info : unit_infos_) {
        objects_to_query[unit_info.klipper_key] = nullptr;
    }

    nlohmann::json params = {{"objects", objects_to_query}};

    spdlog::debug("[AMS AFC] Querying initial state for {} objects", objects_to_query.size());

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](const nlohmann::json& response) {
            // L081 Mechanism C: handle_status_update mutates members + emits events.
            token.defer("AmsBackendAfc::query_initial_state_success", [this, response]() {
                // Response structure:
                // {"jsonrpc": "2.0", "result": {"eventtime": ..., "status": {...}}, "id": ...}
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
            });
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

    // Query Moonraker database for AFC lane_data.
    //
    // The namespace is top-level "lane_data" with one key per lane — AFC's
    // send_lane_data() POSTs {"namespace":"lane_data","key":<lane>,"value":{…}}.
    // We previously asked for namespace "AFC" key "lane_data", which Moonraker
    // answers with a 404 ("Key 'lane_data' in namespace 'AFC' not found"), so this
    // query could never have succeeded. It went unnoticed because the version gate
    // above it meant the call was almost never reached.
    //
    // No key: fetch the whole namespace so result.value is {lane → data}, which is
    // the shape parse_lane_data expects.
    nlohmann::json params = {{"namespace", "lane_data"}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "server.database.get_item", params,
        [this, token](const nlohmann::json& response) {
            // L081 Mechanism C: parse_lane_data mutates members under lock; emit on main.
            token.defer("AmsBackendAfc::query_lane_data_success", [this, response]() {
                // apply_lane_data_response takes the lock; emit OUTSIDE it to
                // avoid deadlock with callbacks.
                if (apply_lane_data_response(response)) {
                    emit_event(EVENT_STATE_CHANGED);
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS AFC] Failed to query lane_data: {}", err.message);
        });
}

// ============================================================================
// Toolchanger identity
// ============================================================================

bool AmsBackendAfc::has_toolchanger() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Signal 1: the config section that registers AFC_SELECT_TOOL.
    if (configfile_has_toolchanger_) {
        return true;
    }
    // Signal 2: a published Toolchanger unit. Only reachable when that unit
    // owns at least one lane (AFC.py v1.2.0:2554), which is why signal 1 exists.
    return std::any_of(unit_infos_.begin(), unit_infos_.end(), [](const AfcUnitInfo& u) {
        // `type` is user-overridable (`config.get("type", "Toolchanger")`), so
        // compare case-insensitively rather than pinning the exact spelling.
        return to_lower_copy(u.type) == "toolchanger";
    });
}

void AmsBackendAfc::query_afc_configfile_topology() {
    if (!client_) {
        return;
    }

    // configfile has no sub-field selector in Moonraker — asking for "settings"
    // returns the entire resolved config. Accepted once at startup because it is
    // the only place th_extruder_name and the [AFC_Toolchanger] section are
    // observable.
    nlohmann::json params = {{"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](const nlohmann::json& response) {
            // L081 Mechanism C: the body mutates members under mutex_.
            token.defer("AmsBackendAfc::query_afc_configfile_topology_success", [this, response]() {
                if (!response.contains("result") || !response["result"].contains("status") ||
                    !response["result"]["status"].is_object()) {
                    return;
                }
                const auto& status = response["result"]["status"];
                if (!status.contains("configfile") || !status["configfile"].is_object() ||
                    !status["configfile"].contains("settings") ||
                    !status["configfile"]["settings"].is_object()) {
                    spdlog::debug("[AMS AFC] configfile.settings absent — AFC_extruder tool "
                                  "indices stay derived from section names");
                    return;
                }
                const auto& settings = status["configfile"]["settings"];

                // Klipper lowercases both section headers and option keys in
                // configfile.settings, so `[AFC_extruder T1]` arrives as
                // "afc_extruder t1". The section suffix is matched
                // case-insensitively against the names AFC.extruders publishes.
                static constexpr const char* EXTRUDER_PREFIX = "afc_extruder ";
                static constexpr const char* TOOLCHANGER_PREFIX = "afc_toolchanger ";
                std::unordered_map<std::string, std::string> found;
                bool saw_toolchanger = false;
                for (auto it = settings.begin(); it != settings.end(); ++it) {
                    const std::string key = to_lower_copy(it.key());
                    if (key.rfind(TOOLCHANGER_PREFIX, 0) == 0) {
                        saw_toolchanger = true;
                        continue;
                    }
                    if (key.rfind(EXTRUDER_PREFIX, 0) != 0 || !it.value().is_object()) {
                        continue;
                    }
                    const auto& section = it.value();
                    if (!section.contains("extruder_name") ||
                        !section["extruder_name"].is_string()) {
                        continue;
                    }
                    found[key.substr(std::strlen(EXTRUDER_PREFIX))] =
                        section["extruder_name"].get<std::string>();
                }

                std::lock_guard<std::mutex> lock(mutex_);
                extruder_klipper_names_ = std::move(found);
                // Settings were read. Only now does an absent extruder_name
                // mean the config lacks one rather than that we have not asked.
                configfile_answered_ = true;
                // A newly-arrived mapping can resolve a name that already
                // warned; let it warn again if it still cannot be resolved.
                extruder_tool_index_warned_.clear();
                // Latch only on presence. A config we could not read, or one
                // read before a section was added, must not be taken as proof
                // that no toolchanger exists.
                if (saw_toolchanger) {
                    configfile_has_toolchanger_ = true;
                }
                spdlog::debug("[AMS AFC] configfile: {} AFC_extruder -> Klipper extruder names, "
                              "toolchanger section {}",
                              extruder_klipper_names_.size(),
                              saw_toolchanger ? "present" : "absent");

                // This query races the first status frames — on the reporter's
                // machine it landed 17ms after the units were first mapped, so
                // every toolhead label was derived from names it could not yet
                // resolve. Redo that derivation now rather than carrying wrong
                // labels until AFC happens to push another unit frame.
                if (!extruder_klipper_names_.empty() && !unit_infos_.empty()) {
                    rebuild_unit_map_from_klipper();
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[AMS AFC] Failed to query configfile: {} — tool indices stay derived "
                          "from section names and toolchanger detection falls back to AFC.units",
                          err.message);
        });
}

std::string AmsBackendAfc::klipper_extruder_name_unlocked(const std::string& section_name) const {
    // configfile-sourced th_extruder_name is authoritative — it is what AFC
    // itself indexes on, and the section name is free to be anything. Taken
    // only when it actually names an extruder: AFC accepts any value
    // containing "extruder" (AFC_extruder.py:384), so a config can carry one
    // that no numbering can read, and the section name is the better guess
    // then. Checking here rather than at each caller keeps one fallback chain.
    const auto it = extruder_klipper_names_.find(to_lower_copy(section_name));
    if (it != extruder_klipper_names_.end() && helix::tool_number_for_extruder(it->second)) {
        return it->second;
    }
    // Fall back to the section name. `[AFC_extruder extruder1]` is what AFC's
    // own docs show and what every published config uses, and on v1.1.0 (no
    // extruder_name option at all) it is the ONLY thing that exists.
    return section_name;
}

std::string AmsBackendAfc::afc_extruder_section_for_tool_unlocked(int tool_index) const {
    if (tool_index < 0) {
        return "";
    }
    for (const auto& section : extruder_names_) {
        const auto n = helix::tool_number_for_extruder(klipper_extruder_name_unlocked(section));
        if (n && *n == tool_index) {
            return section;
        }
    }
    return "";
}

int AmsBackendAfc::tool_index_for_extruder_unlocked(const std::string& ext_name) const {
    // Mirrors AFC_Toolchanger.py:231-232 (v1.2.0):
    //     name = lane.extruder_obj.th_extruder_name
    //     tool_index = 0 if name == "extruder" else int(name.replace("extruder", ""))
    // helix::tool_number_for_extruder() is that grammar, hardened and shared —
    // it is the only copy, so a toolhead cannot be numbered one way here and
    // another way in the badge or lane-attribution path.
    if (const auto n = helix::tool_number_for_extruder(klipper_extruder_name_unlocked(ext_name))) {
        return *n;
    }

    // Before configfile answers, "unresolvable" is not established. A section
    // whose name is not `extruder<N>` MUST carry extruder_name or AFC refuses
    // to start (AFC_extruder.py:384 rejects any th_extruder_name without
    // "extruder" in it), so on every machine that boots at all the answer
    // exists and has merely not arrived — the query races the first status
    // frames by milliseconds. Warning here told users to add an option they
    // were already required to have. query_afc_configfile_topology() clears
    // the warned set when it lands, so a config that genuinely lacks one still
    // gets told, once, with the evidence in hand.
    if (!configfile_answered_) {
        spdlog::debug("[AMS AFC] Tool number for AFC_extruder '{}' unresolved for now — "
                      "configfile.settings has not answered yet",
                      ext_name);
        return -1;
    }

    if (extruder_tool_index_warned_.insert(ext_name).second) {
        const auto it = extruder_klipper_names_.find(to_lower_copy(ext_name));
        if (it != extruder_klipper_names_.end()) {
            spdlog::warn("[AMS AFC] Cannot determine a tool number for AFC_extruder '{}': its "
                         "extruder_name is '{}', which is not a Klipper extruder object name "
                         "(expected `extruder` or `extruder<N>`). Lane attribution for this "
                         "toolhead is disabled (it is NOT being assumed to be T0).",
                         ext_name, it->second);
        } else {
            spdlog::warn("[AMS AFC] Cannot determine a tool number for AFC_extruder '{}': the "
                         "section name carries no extruder index and configfile.settings has no "
                         "extruder_name for it. Lane attribution for this toolhead is disabled "
                         "(it is NOT being assumed to be T0). Set `extruder_name: extruder<N>` in "
                         "the [AFC_extruder {}] section.",
                         ext_name, ext_name);
        }
    }
    return -1;
}

void AmsBackendAfc::parse_lane_data(const nlohmann::json& lane_data) {
    // Lane data format — two key styles, one per firmware generation:
    //   pre-virtual-tools: keyed by LANE NAME
    //     { "lane1": {"color": "#FF0000", "material": "PLA", ...}, ... }
    //   virtual-tools firmware (#832): keyed by T(n) MAPPING, one record per
    //   mapped tool (a multi-mapped lane appears once per T(n)), with no lane
    //   identity inside the record:
    //     { "T0": {"color": "#FF0000", ...}, "T5": {...}, "T16": {...} }
    //   The T(n) style is resolved through the live tool mapping — the same one
    //   the status path builds from lane "map"/"current_map".
    //
    // A key that exactly names a known lane always takes the lane-name reading:
    // lane names are user-configurable in AFC, so "T0" is ambiguous in theory —
    // but only a lane literally named T0 can hit that, and the lane-name
    // interpretation is the one every older firmware uses.

    // Classify keys and resolve each slot's record up front.
    std::vector<const nlohmann::json*> slot_records(
        slots_.is_initialized() ? static_cast<size_t>(slots_.slot_count()) : 0, nullptr);
    std::vector<std::string> lane_keys; // lane-name keys, for bootstrap only
    bool has_unresolved_tool_key = false;

    for (auto it = lane_data.begin(); it != lane_data.end(); ++it) {
        const std::string& key = it.key();
        if (!it.value().is_object()) {
            continue; // not a record; the field loop below requires an object
        }

        // Lane-name reading first (see comment above).
        if (slots_.is_initialized()) {
            const int slot = slots_.index_of(key);
            if (slot >= 0) {
                lane_keys.push_back(key);
                slot_records[slot] = &it.value();
                continue;
            }
        }

        // Tool-key reading: exact "T<digits>", digits within the same bound the
        // status path accepts.
        int tool = -1;
        if (key.size() >= 2 && key[0] == 'T' &&
            std::all_of(key.begin() + 1, key.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
            try {
                tool = std::stoi(key.substr(1));
            } catch (...) {
                tool = -1; // out of int range — not a usable tool number
            }
        }

        if (tool < 0 || tool > AFC_MAX_TOOL_NUMBER) {
            // Neither a known lane name nor a tool key. Old firmware with lanes
            // we have not discovered yet — hand it to bootstrap below.
            lane_keys.push_back(key);
            continue;
        }

        const int slot = slots_.is_initialized() ? slots_.slot_for_tool(tool) : -1;
        if (slot >= 0 && firmware_mapped_slots_.count(slot) > 0) {
            if (!slot_records[slot]) {
                slot_records[slot] = &it.value();
            }
        } else {
            // No lane claims this tool (yet), or the mapping is still the
            // identity placeholder initialize_slots() seeded. Park the whole
            // payload: the DB query is one-shot, so the record must survive
            // until a firmware-asserted mapping arrives — parse_afc_stepper()
            // replays it.
            has_unresolved_tool_key = true;
        }
    }

    if (has_unresolved_tool_key) {
        pending_tool_lane_data_ = lane_data;
    } else {
        pending_tool_lane_data_.reset();
    }

    // Extract lane names and sort numerically (lane2 < lane10, not alphabetically)
    std::sort(lane_keys.begin(), lane_keys.end(), natural_less);

    // This payload is a supplement (colours, materials, spool ids), never the
    // authority on WHICH lanes exist. Klipper's object list is. AFC empties the
    // whole lane_data namespace at the start of every PREP (AFC.py
    // delete_lane_data) and writes each lane's key back only as that lane's own
    // prep finishes (AFC_BoxTurtle.py send_lane_data), which for a BoxTurtle
    // means driving each lane motor in turn. The namespace is therefore
    // partially populated for seconds on every boot, and query_lane_data() is
    // one-shot and never retried. Resizing to match it once pinned a 4-lane
    // BoxTurtle to a single slot for the whole session: initialize_slots()
    // clears discovered_lane_names_, and the status handler only iterates slots
    // that exist, so lanes 2-4 became permanently invisible.
    //
    // So: only ever bootstrap an empty registry, and prefer discovery when it
    // has something to offer. Once the registry exists, leave its shape alone.
    // Tool keys NEVER bootstrap: "T0" names a tool, and slots named after tools
    // would be invisible to the status path (which iterates lanes) forever.
    //
    // By value: initialize_slots() clears discovered_lane_names_ on its way
    // out, which would dangle a reference bound to it.
    if (!slots_.is_initialized()) {
        const std::vector<std::string> initial_lanes =
            !discovered_lane_names_.empty() ? discovered_lane_names_ : lane_keys;
        if (initial_lanes.empty()) {
            return; // Nothing names a lane yet; a later payload or discovery will.
        }
        initialize_slots(initial_lanes);
        // Bootstrap may have just created the slots lane_keys names; resolve
        // those records now so the loop below can apply them.
        slot_records.assign(static_cast<size_t>(slots_.slot_count()), nullptr);
        for (const std::string& name : lane_keys) {
            const int slot = slots_.index_of(name);
            if (slot >= 0 && lane_data.contains(name) && lane_data[name].is_object()) {
                slot_records[slot] = &lane_data[name];
            }
        }
    }

    // Track whether any lane has tool_loaded — used to update filament_loaded
    // after scanning all lanes. Without this, filament_loaded could stay stale
    // if no lane reports tool_loaded, leaving the Unload button stuck disabled.
    bool any_tool_loaded = false;
    int tool_loaded_slot = -1;

    // Update lane information
    for (int i = 0; i < slots_.slot_count(); ++i) {
        const nlohmann::json* lane_ptr = slot_records[i];
        if (lane_ptr == nullptr) {
            continue;
        }
        const auto& lane = *lane_ptr;
        auto* entry = slots_.get_mut(i);
        if (!entry) {
            continue;
        }
        auto& slot = entry->info;

        // Parse color. AFC writes "#RRGGBB" here (verified against a live
        // BoxTurtle's lane_data namespace); std::stoul cannot parse the '#', so
        // without stripping it every lane fell back to the default grey. Bare hex
        // is also accepted. Matches the handling in parse_afc_stepper.
        if (lane.contains("color") && lane["color"].is_string()) {
            std::string color_str = lane["color"].get<std::string>();
            if (!color_str.empty() && color_str[0] == '#') {
                color_str = color_str.substr(1);
            }
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

        // Filament name, as AFC copied it out of Spoolman's filament record.
        // Mirrors parse_afc_stepper(): an EMPTY value is a deliberate clear —
        // clear_lane_data()/clear_values() write the key as "" on eject — so it
        // is adopted as-is rather than treated as "keep existing". Without this
        // a lane whose data only ever arrived through the DB path had no name at
        // all, and the loaded card fell back to the algorithmic colour
        // description. apply_overrides() runs below, so a user-entered name
        // still wins.
        //
        // Key ladder mirrors read_vendor(): `name` is the shared lane_data
        // spelling that both AFC (#833) and Happy Hare publish; `filament_name`
        // is AFC's own attribute name, correct on the get_status surface and
        // read here defensively.
        for (const char* key : {"name", "filament_name"}) {
            auto it = lane.find(key);
            if (it != lane.end() && it->is_string()) {
                slot.spool_name = it->get<std::string>();
                break;
            }
        }

        // Parse loaded state.
        // AFC "loaded" means hub-loaded, not toolhead-loaded — only
        // tool_loaded == true means filament is at the extruder.
        //
        // AFC's real lane_data payload carries NONE of these keys (it is metadata
        // only: color/material/temps/spool_id/td). Defaulting to AVAILABLE when no
        // status key is present would clobber the status already derived from the
        // AFC_stepper objects and show empty lanes as loaded. Absent means
        // unchanged, matching how status deltas are treated elsewhere.
        const bool has_status_key = lane.contains("tool_loaded") || lane.contains("loaded") ||
                                    lane.contains("available") || lane.contains("empty");

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
        } else if (has_status_key) {
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

        // Parse spool information if available.
        //
        // Mirrors parse_afc_stepper(): AFC writes spool_id=None on eject, and
        // is_number_integer() is false for null, so guarding on that alone
        // retained the old id and left an ejected lane looking linked — which is
        // what aims a later edit's Spoolman write at the wrong spool. An ABSENT
        // key still means "unchanged"; these are deltas, not snapshots.
        if (lane.contains("spool_id")) {
            if (lane["spool_id"].is_number_integer()) {
                slot.spoolman_id = lane["spool_id"].get<int>();
            } else if (lane["spool_id"].is_null()) {
                slot.spoolman_id = 0;
            }
        }

        // Vendor/brand — see read_vendor(). Arrives as `vendor_name` here; inert
        // on firmware predating #833.
        read_vendor(lane, slot.brand);

        // Re-supply the user's attached identity on top of firmware truth, the
        // same way parse_afc_stepper() does. Without this, which parser ran last
        // decided whether an override was visible: the status path applied it,
        // the DB path silently dropped it. Must follow every firmware read above
        // so the override still wins.
        apply_overrides(slot, slot.global_index >= 0 ? slot.global_index : slot.slot_index);

        // NO WEIGHT IS READ FROM lane_data, on any AFC version. This is deliberate.
        //
        // AFC's lane_data record carries exactly one weight key, `weight`, and it is
        // never the best source:
        //
        //   v1.1.x     key absent entirely (added to send_lane_data in v1.2.0)
        //   v1.2.0     present but STALE — cmd_SET_WEIGHT updated the lane object
        //              without publishing, so the record only refreshed when an
        //              unrelated command (SET_COLOR/SET_MATERIAL/SET_MAP/set_spoolID)
        //              happened to push afterwards. clear_lane_data() also omitted
        //              weight, so a cleared lane kept the old value forever.
        //   post-#812  fixed upstream (AFCProject/AFC-Klipper-Add-On#805): SET_WEIGHT
        //              publishes, and clearing writes weight: 0.
        //
        // The AFC_stepper subscription has carried a live `weight` since v1.1.0, and
        // parse_afc_stepper() already reads it — so lane_data is redundant on every
        // release and actively wrong on v1.2.0. Crucially, the stale and fixed forms
        // are byte-identical on the wire, so no feature detection can tell them apart;
        // it is a freshness problem, not a shape one, and AFC_VERSION is a hand-bumped
        // literal that cannot support a floor (it sat at 1.1.37 through all of v1.2.0).
        //
        // Reading it here would also race: query_lane_data() and the subscription are
        // independent async replies with no ordering guarantee, so a slow DB response
        // can land after the first status snapshot and overwrite fresh with stale.
        //
        // Same reasoning the status block above uses — lane_data must not clobber
        // AFC_stepper truth.
        //
        // (Two readers for `remaining_weight` / `total_weight` used to sit here. No AFC
        // version ever emitted either key, and our own to_lane_data_record() writes
        // them with a `_g` suffix, so they matched nothing and never fired.)
    }

    // Update filament_loaded from lane scan results.
    // The top-level parse_afc_state() may also set this from AFC's own
    // filament_loaded field, but lane data is the authoritative source for
    // which specific slot is loaded.
    system_info_.filament_loaded = any_tool_loaded;
    // Only set current_slot as fallback when no authoritative value exists.
    // In tool changers, multiple lanes are loaded simultaneously, so the
    // first tool_loaded lane is not necessarily the active one.
    if (any_tool_loaded && system_info_.current_slot < 0) {
        system_info_.current_slot = tool_loaded_slot;
    }
}

void AmsBackendAfc::apply_mount_state(bool extruder_set_active_slot, bool afc_stated_unloaded) {
    // --- AFC's own carriage signal, preferred where it exists ---
    //
    // Gated on more than one extruder. A Box Turtle has a single extruder and no
    // carriage at all; if AFC publishes on_shuttle:false there, reading it would
    // clear current_slot forever on every single-extruder machine. "No carriage"
    // and "carriage empty" are not the same claim.
    //
    // num_extruders_ rather than extruder_sensors_.size(): it comes from
    // AFC.extruders, which arrives complete in a single field and is what the
    // AFC maintainers point to for "how many toolheads does this machine have".
    // The sensor map is filled in one AFC_extruder object at a time, so early in
    // a frame it can transiently hold 1 on a six-extruder machine.
    //
    // Not consulted mid-change: toolchanger.status is the only source that knows
    // a swap is under way, and on_shuttle is momentarily false for both the
    // outgoing and incoming tool.
    if (num_extruders_ > 1 && system_info_.mount_state != MountState::CHANGING) {
        int reporters = 0;
        const AfcExtruderSensors* mounted = nullptr;
        std::string mounted_name;
        for (const auto& entry : extruder_sensors_) {
            if (!entry.second.has_on_shuttle) {
                continue;
            }
            ++reporters;
            if (entry.second.on_shuttle && mounted == nullptr) {
                mounted = &entry.second;
                mounted_name = entry.first;
            }
        }

        if (reporters > 0) {
            if (mounted == nullptr) {
                system_info_.mount_state = MountState::NONE;
                system_info_.mounted_tool = -1;
                system_info_.current_slot = -1;
                system_info_.filament_loaded = false;
                return;
            }

            system_info_.mount_state = MountState::MOUNTED;
            // mounted_tool is an int for consumers that predate extruder-name
            // identity. -1 is its "unknown" value, already used by the NONE
            // branch above; the old .value_or(0) here meant a renamed config
            // had every extruder in turn claim to be T0 as it was parsed.
            system_info_.mounted_tool = tool_index_for_extruder_unlocked(mounted_name);

            // The mounted extruder names its own seated lane. Precise even where
            // several lanes feed one extruder, which a lane→tool map cannot be.
            if (!mounted->lane_loaded.empty()) {
                const int slot = slots_.index_of(mounted->lane_loaded);
                if (slot >= 0) {
                    system_info_.current_slot = slot;
                    const helix::printer::SlotEntry* entry = slots_.get(slot);
                    if (entry != nullptr && entry->info.status != SlotStatus::UNKNOWN) {
                        system_info_.filament_loaded = (entry->info.status == SlotStatus::LOADED);
                    }
                    return;
                }
            }

            // Mounted but holding nothing: a real state, not a reason to keep a
            // stale slot from whoever was mounted before.
            system_info_.current_slot = -1;
            system_info_.filament_loaded = false;
            return;
        }
    }

    // UNKNOWN means no carriage exists (every non-toolchanger machine) or no
    // signal has arrived yet. Either way there is nothing to assert, and the
    // negotiated value stands — this is what keeps single-extruder AFC, Box
    // Turtle and friends behaving exactly as before.
    if (system_info_.mount_state == MountState::UNKNOWN) {
        return;
    }

    // Mid-change the sources legitimately disagree: Klipper has already advanced
    // tool_number while the tool is still in flight. Elect nothing rather than
    // flicker through a wrong slot.
    if (system_info_.mount_state == MountState::CHANGING) {
        return;
    }

    if (system_info_.mount_state == MountState::NONE) {
        // Parked toolheads may well hold filament — that is normal on a
        // toolchanger and does NOT make any of them current. Before #1229 the
        // first such lane was elected and then latched, because the other
        // writers are all guarded by `current_slot < 0`.
        if (system_info_.current_slot != -1 || system_info_.filament_loaded) {
            spdlog::debug("[AMS AFC] Carriage empty — clearing elected slot {} (filament_loaded "
                          "was {})",
                          system_info_.current_slot, system_info_.filament_loaded);
        }
        system_info_.current_slot = -1;
        system_info_.filament_loaded = false;
        return;
    }

    // MOUNTED: the tool on the carriage decides, every frame.
    //
    // One thing outranks the lane→tool map: an attribution made from the mounted
    // extruder's own `lane_loaded` this frame. Several lanes can feed one
    // extruder (extruder2 and extruder3 on #1229's machine take two and four),
    // so the map cannot say which of them is seated — but the extruder can, and
    // does. Overriding it with slot_for_tool() reintroduces exactly the
    // wrong-lane selection #379 fixed.
    //
    // This is not the old latch returning: extruder_set_active_slot is recomputed
    // per frame, so it cannot pin a stale value the way `current_slot < 0` did.
    if (extruder_set_active_slot) {
        return;
    }

    // AFC said, this frame, that nothing is at the toolhead. The carriage is still
    // the authority on WHICH tool is current — mounted_tool and current_tool stand
    // — but electing its lane here would manufacture filament the firmware just
    // told us is absent. Same outcome as the "mounted but holding nothing" arm
    // above, reached by a different route: a tool can be on the carriage with an
    // empty melt zone, and current_slot = -1 is how that state is expressed
    // (#1229). Frame-scoped, so the next frame elects normally.
    //
    // Deliberately BELOW the on_shuttle arm, which returns before reaching here.
    // That arm already prefers the mounted extruder's own lane_loaded over the
    // lane→tool map — several lanes can feed one extruder, so the extruder knows
    // which is seated and a map cannot; overriding it is the wrong-lane selection
    // #379 fixed. This check applies the SAME precedence one branch lower, where
    // on_shuttle is absent and the map is otherwise the only voice.
    if (afc_stated_unloaded) {
        // ...unless the MOUNTED extruder names a lane of its own.
        //
        // These are not two signals. AFC.current_load is a property, not a stored
        // field: AFC.py's `current` returns AFC_functions.get_current_lane(), which
        // is `tools[get_current_extruder()].lane_loaded`. So the aggregate IS the
        // mounted toolhead's lane_loaded, resolved through Klipper's active
        // extruder. When the two disagree the specific one wins, because it is the
        // same fact at a finer resolution — and a disagreement means Klipper's
        // active extruder and the toolchanger's mounted tool have not converged yet.
        //
        // Asking instead whether ANY extruder holds filament answers a different
        // question, wrongly: parked toolheads routinely grip filament on a
        // toolchanger (the premise of #1229's own "parked toolheads may well hold
        // filament" note), so that is true on essentially every real multi-tool
        // machine. The suppression would never fire in production and only look
        // fixed in a fixture with no per-extruder data.
        // extruder_sensors_ is keyed by AFC SECTION name, so this needs the
        // resolver too; parsing the key directly matched nothing on a renamed
        // config and silently took the "no evidence" branch below every frame.
        const AfcExtruderSensors* mounted_sensors = nullptr;
        for (const auto& entry : extruder_sensors_) {
            const int tool_num = tool_index_for_extruder_unlocked(entry.first);
            if (tool_num >= 0 && tool_num == system_info_.mounted_tool) {
                mounted_sensors = &entry.second;
                break;
            }
        }

        // No per-extruder evidence at all: nothing contradicts AFC, so believe it.
        if (mounted_sensors == nullptr || mounted_sensors->lane_loaded.empty()) {
            if (system_info_.current_slot != -1 || system_info_.filament_loaded) {
                spdlog::debug("[AMS AFC] AFC reports nothing at the toolhead — mounted T{} elects "
                              "no slot (was {})",
                              system_info_.mounted_tool, system_info_.current_slot);
            }
            system_info_.current_slot = -1;
            system_info_.filament_loaded = false;
            return;
        }
    }

    if (!slots_.is_initialized() || system_info_.mounted_tool < 0) {
        return;
    }
    const int slot = slots_.slot_for_tool(system_info_.mounted_tool);
    if (slot < 0 || slot >= slots_.slot_count()) {
        // Tool is mounted but maps to no lane we know about. Say "unknown"
        // rather than keeping a stale slot from a previous mount.
        system_info_.current_slot = -1;
        system_info_.filament_loaded = false;
        return;
    }
    if (system_info_.current_slot != slot) {
        spdlog::debug("[AMS AFC] Mounted T{} -> slot {} (was {})", system_info_.mounted_tool, slot,
                      system_info_.current_slot);
    }
    system_info_.current_slot = slot;

    // Only the mounted tool's own lane can put filament in the active path — but
    // say so only when the lane's status is actually known. Deriving "not loaded"
    // from UNKNOWN would be the same error as electing a slot from no
    // information, and would clobber a value the parsers legitimately established
    // on frames that carry no per-lane data.
    const helix::printer::SlotEntry* entry = slots_.get(slot);
    if (entry != nullptr && entry->info.status != SlotStatus::UNKNOWN) {
        system_info_.filament_loaded = (entry->info.status == SlotStatus::LOADED);
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

    // Initialize tool-to-lane mapping (1:1 default) in the registry.
    // The registry is the single source of truth for tool mappings — lane
    // "map" fields update it, and get_system_info() reads it via
    // build_system_info(). No cached copy needed in system_info_.
    std::vector<int> default_map(lane_count);
    for (int i = 0; i < lane_count; ++i)
        default_map[i] = i;
    slots_.set_tool_map(default_map);

    // No lane has asserted a mapping yet — the identity map above is a
    // placeholder. parse_lane_data()'s T(n) join must not trust it.
    firmware_mapped_slots_.clear();

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

    // Sort units by their lowest lane number so physical order is preserved
    // (e.g., unit with lane0-3 before unit with lane12-15, regardless of unit name)
    std::vector<std::pair<std::string, std::vector<std::string>>> sorted_units(
        unit_lane_map_.begin(), unit_lane_map_.end());
    for (auto& [name, lanes] : sorted_units) {
        std::sort(lanes.begin(), lanes.end(), natural_less);
    }
    std::sort(sorted_units.begin(), sorted_units.end(), [](const auto& a, const auto& b) {
        int min_a = a.second.empty() ? INT_MAX : trailing_number(a.second.front());
        int min_b = b.second.empty() ? INT_MAX : trailing_number(b.second.front());
        if (min_a != min_b)
            return min_a < min_b;
        return a.first < b.first;
    });
    slots_.reorganize(sorted_units);

    // Rebuild system_info_.units for the unit-level metadata the registry doesn't
    // track. This DESTROYS every AmsUnit: anything not re-derived below is gone.
    // Sensors and topology are re-derived here from hub_sensors_ / unit_infos_;
    // buffer health is re-derived after the loop from buffer_health_. Nothing here
    // is "preserved" — a field whose only writer is a status-delta parser cannot
    // be, because Moonraker forwards only changed keys and that parser may not run
    // again for minutes.
    system_info_.units.clear();
    int global_slot_offset = 0;
    int unit_idx = 0;

    for (const auto& [unit_name, lanes] : sorted_units) {
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

    // The units above are freshly default-constructed, so buffer_health is nullopt
    // on every one of them. Re-derive it from what AFC last reported — the buffer
    // parser will not run again until a buffer field actually changes.
    apply_buffer_health_to_units();

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
        return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
    }

    spdlog::info("[AMS AFC] Executing G-code: {}", gcode);

    // Capture messages by value for async callbacks (thread-safe via ui_queue_update())
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
                    NOTIFY_WARNING(lv_tr("{} — response timed out"), error_prefix);
                }
            } else if (!error_prefix.empty()) {
                NOTIFY_ERROR("{}: {}", error_prefix, err.message);
            } else {
                spdlog::error("[AMS AFC] G-code failed: {} - {}", gcode, err.message);
            }
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);

    return AmsErrorHelper::success();
}

AmsError AmsBackendAfc::do_load_filament(int slot_index) {
    std::string lane_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

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

    // Toolchanger mode: use AFC_SELECT_TOOL with extruder name.
    //
    // Gated on a toolchanger EXISTING (see has_toolchanger() for the two
    // signals), not on the extruder count. AFC_SELECT_TOOL is registered by
    // AfcToolchanger.__init__ and nowhere else (AFC_Toolchanger.py:47-49), a
    // file that exists only from v1.2.0 and that Klipper only loads for an
    // `[AFC_Toolchanger <name>]` section. An IDEX or standalone-toolhead machine
    // has several [AFC_extruder] sections and no toolchanger, so
    // `num_extruders_ > 1` was true there and the command came back
    // `// Unknown command:"AFC_SELECT_TOOL"` with the load never happening.
    if (has_toolchanger()) {
        const auto* entry = slots_.get(slot_index);
        int tool = entry ? entry->info.mapped_tool : slot_index;
        if (tool >= 0 && tool < static_cast<int>(extruders_.size())) {
            std::string cmd = "AFC_SELECT_TOOL TOOL=" + extruders_[tool].name;
            spdlog::info("[AMS AFC] Loading slot {} via toolchanger: {}", slot_index, cmd);
            return dispatch_operation(std::move(cmd), AmsAction::LOADING);
        }
    }

    // Standard mode: CHANGE_TOOL LANE={name}.
    //
    // Also the correct fallback for a multi-extruder machine with no toolchanger
    // unit: CHANGE_TOOL resolves the lane's own extruder object
    // (cur_lane.extruder_obj) and loads through it, so naming the LANE is
    // sufficient and needs no per-tool selection verb.
    std::ostringstream cmd;
    cmd << "CHANGE_TOOL LANE=" << lane_name;

    spdlog::info("[AMS AFC] Loading from lane {} (slot {})", lane_name, slot_index);
    return dispatch_operation(cmd.str(), AmsAction::LOADING);
}

AmsError AmsBackendAfc::do_unload_filament(int slot_index) {
    std::string lane_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!system_info_.filament_loaded) {
            return AmsError(AmsResult::WRONG_STATE, "No filament loaded", "No filament to unload",
                            "Load filament first");
        }

        // Resolve the requested lane so AFC unloads THAT lane, picking up its
        // tool first if it isn't the one on the shuttle (#999). slot_index < 0
        // (the default) means "unload whatever is loaded" and leaves lane_name
        // empty; an out-of-range index resolves to "" too, falling back to the
        // active-lane unload.
        lane_name = slots_.name_of(slot_index);
    }

    // TOOL_UNLOAD [LANE=<lane>]: with LANE, AFC selects that lane's tool and
    // unloads it from the toolhead; without it, AFC unloads the active lane.
    // The lane parameter subsumes the toolchanger case, so we no longer branch
    // to AFC_UNSELECT_TOOL (which parked the active tool while ignoring the
    // requested lane).
    std::string cmd = "TOOL_UNLOAD";
    if (!lane_name.empty()) {
        cmd += " LANE=" + lane_name;
    }

    spdlog::info("[AMS AFC] Unloading: {}", cmd);
    return dispatch_operation(std::move(cmd), AmsAction::UNLOADING);
}

AmsError AmsBackendAfc::do_select_slot(int slot_index) {
    std::string lane_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

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

AmsError AmsBackendAfc::do_change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tool_number < 0 || tool_number >= slots_.slot_count()) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "Select a valid tool");
        }
    }

    std::string cmd;
    if (has_toolchanger() && tool_number < static_cast<int>(extruders_.size())) {
        // Toolchanger mode: use AFC_SELECT_TOOL with extruder name. See
        // load_filament() for why the gate is the unit type and not the
        // extruder count (AFC_Toolchanger.py:47-49, v1.2.0 only).
        cmd = "AFC_SELECT_TOOL TOOL=" + extruders_[tool_number].name;
    } else {
        // Standard mode: use T{n}. Correct for a multi-extruder machine with no
        // toolchanger too — AFC's TcmdAssign registers a T-command per lane on
        // every topology, and cmd_CHANGE_TOOL routes it through the lane's own
        // extruder object.
        cmd = "T" + std::to_string(tool_number);
    }

    spdlog::info("[AMS AFC] Tool change: {}", cmd);
    // SELECTING, not LOADING: every AFC toolchanger state (ToolSwap, ToolDock,
    // ToolPickup, Moving, Restoring) maps to SELECTING, so the optimistic value
    // matches what AFC is about to echo and the operation carries the 300s
    // toolchange budget rather than the 180s load one.
    return dispatch_operation(std::move(cmd), AmsAction::SELECTING);
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

void AmsBackendAfc::apply_overrides(SlotInfo& slot, int slot_index) {
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
    // semantics): if we just re-linked this lane's spool id, in-flight
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
                    spdlog::warn("[AMS AFC] override clear persist failed for slot {}: {}",
                                 slot_index, err);
            });
        }
    }
}

void AmsBackendAfc::persist_override(int slot_index, const SlotInfo& info) {
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
                spdlog::warn("[AMS AFC] override save failed for slot {}: {}", slot_index, err);
            }
        });
    }
}

void AmsBackendAfc::clear_slot_override(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_.erase(slot_index);

        // Also reset the override-exclusive fields on the live slot, so the
        // clear shows up in the very next get_slot_info(). AFC has no concept
        // of brand / spool_name / total weight / colour name, so no firmware
        // update will ever clear them for us — dropping only the store entry
        // would leave the previous spool's identity on screen indefinitely.
        // colour and material are left alone: those DO come from the parse, so
        // the lane's actual firmware values should surface.
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
    if (override_store_) {
        override_store_->clear_async(slot_index, [slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("[AMS AFC] override clear failed for slot {}: {}", slot_index, err);
            }
        });
    }
}

void AmsBackendAfc::publish_external_spool_lane(const SlotInfo* spool) {
    // Capability + index under the lock; the store send happens outside.
    // Lazy store construction: built on first use from api_ so a never-started
    // backend (unit tests) needs no Moonraker connection, and the shared
    // namespace store only ever exists on a live API. Tool key style matches
    // AFC's own lane_data keys since its virtual-tools firmware (T<n>, spec
    // filament_slots.md §4) — one convention per namespace, and the inner
    // 0-based `lane` field is what readers key off either way.
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
            api_, "afc", helix::ams::LaneKeyStyle::Tool);
    }
    helix::ams::publish_external_lane(lane_publish_store_.get(), lane_index, spool,
                                      backend_log_tag());
}

bool AmsBackendAfc::can_recover_lane_position(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // cmd_AFC_LANE_RESET (AFC_functions.py) retracts filament from the bowden
    // back to the hub, so it needs the hub occupied — upstream rejects with
    // "Hub is already clear while trying to reset '<lane>'" otherwise.
    //
    // SAFETY, NOT POLITENESS: the toolhead check below is broader than the
    // upstream guard, and is not redundant with it on any version.
    //
    // Through v1.2.0 (a06f14d) upstream's toolhead guard was missing its
    // `return`: it logged "Toolhead is loaded with '<lane>'" and then performed
    // the reset moves anyway, retracting the lane while the extruder still
    // gripped the filament. Reported as AFCProject/AFC-Klipper-Add-On#803,
    // fixed by their PR #814 and shipping in 1.3.0. Installs predating that —
    // including the .112 BoxTurtle as of 2026-07-29 — still fall through.
    //
    // Even against 1.3.0 this stays: upstream reads one signal (AFC.current),
    // while toolhead_is_free_unlocked() reads three, and two of them catch the
    // post-restart desync where AFC.current is null but the extruder still
    // grips filament. See that function for the breakdown.
    if (!toolhead_is_free_unlocked()) {
        return false;
    }

    const std::string lane_name = slots_.name_of(slot_index);
    if (lane_name.empty()) {
        return false;
    }

    auto route = lane_hub_routing_.find(lane_name);
    if (route == lane_hub_routing_.end() || route->second.empty() || route->second == "direct") {
        // No hub in this lane's path — nothing to retract to.
        return false;
    }

    // AFC_hub.state is the only signal that tracks hub occupancy. The per-lane
    // loaded_to_hub field is latched at prep and never updated: it reads true on
    // every lane at once, including while the hub is demonstrably clear.
    auto hub = hub_sensors_.find(route->second);
    const bool hub_occupied = hub != hub_sensors_.end() && hub->second;
    if (!hub_occupied) {
        return false;
    }

    // The lane's own load switch must be triggered (#1187).
    //
    // cmd_AFC_LANE_RESET does NOT check this up front. It opens with an
    // unconditional move_to_hub(cur_lane, DISTANCE, MoveDirection.NEG, ...) —
    // DISTANCE defaults to 50 — and only tests raw_load_state after that move
    // plus one further short move inside the retract loop. Dispatching it at a
    // lane whose filament already sits behind its load switch therefore drags
    // that lane a further ~50mm back toward its prep sensor and drive gears
    // before erroring out, which is real damage the in-loop guard runs too late
    // to prevent. That is precisely the state a lane is left in by a failed
    // reset, so without this gate the obvious follow-up — tapping Recover again
    // on the lane that just failed — compounds it.
    //
    // It also matches cmd_AFC_RESET's own picker, which builds its candidate
    // list from lanes with raw_load_state true. AFC publishes that as `load`.
    const helix::printer::SlotEntry* entry = slots_.get(slot_index);
    if (!entry || !entry->sensors.load) {
        return false;
    }

    // The hub sensor is shared by every lane on the unit, so it cannot say whose
    // filament is past it. Only offer the recovery on a lane AFC itself names.
    //
    // This deliberately offers NOTHING when AFC names no lane (#1182). The older
    // all-lanes fallback assumed a wrong guess cost one harmless refusal; the
    // blind opening retract documented above is why that was wrong. Nor does
    // refusing strand the user: the sidebar Reset dispatches AFC_RESET, which is
    // AFC's own lane picker (cmd_AFC_RESET), and its candidate list is a better
    // answer than anything we could guess from a shared sensor.
    //
    // That picker is still not self-consistent, though, and #803 closing did not
    // change it: cmd_AFC_RESET filters on raw_load_state while cmd_AFC_LANE_RESET
    // requires hub_obj.state, so a lane at prep with a clear hub gets offered and
    // then errors "Hub is already clear while trying to reset '<lane>'". That
    // error latches in printer.AFC.message and re-fires for the session.
    return lane_name == active_load_lane_ && recovery_attribution_valid_unlocked();
}

bool AmsBackendAfc::toolhead_is_free_unlocked() const {
    // Callers hold mutex_.
    //
    // Deliberately does NOT read system_info_.filament_loaded. On every AFC
    // build shipped so far the AFC object carries no "filament_loaded" key, so
    // parse_afc_state() derives it from `loaded_lane` — which prefers
    // current_lane (AFC.current_loading). That makes filament_loaded true for
    // the entire duration of a toolchange, including a TOOL_LOAD that has not
    // put anything in the extruder yet. Gating on it therefore reads "toolhead
    // busy" across exactly the window where a lane can be stranded past the hub
    // with the toolhead empty, which is the only window this predicate exists
    // to serve. Three narrower signals say what filament_loaded only approximates.
    //
    // 1. The physical toolhead sensors. Strongest evidence and independent of
    //    any AFC bookkeeping a restart or a desync could lose.
    if (tool_start_sensor_ || tool_end_sensor_) {
        return false;
    }

    // 2. AFC.current, published as current_load. This is precisely what
    //    upstream's own toolhead guard reads — cmd_AFC_LANE_RESET does
    //    `if (tool_load := self.get_current_lane_obj()) is not None`, and
    //    get_current_lane_obj() resolves self.current. Matching it keeps us
    //    aligned with the check the firmware means to make — the one that only
    //    began to actually stop anything in 1.3.0
    //    (AFCProject/AFC-Klipper-Add-On#803, their PR #814). Signals 1 and 3 are
    //    what make this predicate broader than upstream's on every version.
    if (!toolhead_lane_.empty()) {
        return false;
    }

    // 3. Per-lane tool_loaded, surfaced as SlotStatus::LOADED. AFC persists it
    //    through save_vars, so it still reads true after a restart that leaves
    //    AFC.current null — covering the desync case signal 2 would miss.
    for (int i = 0; i < slots_.slot_count(); ++i) {
        const helix::printer::SlotEntry* entry = slots_.get(i);
        if (entry && entry->info.status == SlotStatus::LOADED) {
            return false;
        }
    }

    return true;
}

std::optional<bool> AmsBackendAfc::toolhead_filament_unaccounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Mirror of toolhead_is_free_unlocked()'s three signals, split by
    // question: that predicate asks "is anything claiming or blocking the
    // toolhead"; this asks "is filament PRESENT while nothing claims it".
    // (1) physical sensors say present...
    bool sensor_present = tool_start_sensor_ || tool_end_sensor_;
    for (const auto& [name, s] : extruder_sensors_) {
        if (s.tool_start || s.tool_end) {
            sensor_present = true;
        }
    }
    if (!sensor_present) {
        return false; // hardware without sensors reports both false — no false positive
    }
    // (2) AFC.current names the seated lane -> accounted
    if (!toolhead_lane_.empty()) {
        return false;
    }
    // (3) any per-extruder lane_loaded names a lane -> accounted
    for (const auto& [name, s] : extruder_sensors_) {
        if (!s.lane_loaded.empty()) {
            return false;
        }
    }
    // (4) any lane's persisted tool_loaded (SlotStatus::LOADED) -> accounted
    for (int i = 0; i < slots_.slot_count(); ++i) {
        if (const auto* entry = slots_.get(i); entry && entry->info.status == SlotStatus::LOADED) {
            return false;
        }
    }
    return true;
}

bool AmsBackendAfc::recovery_attribution_valid_unlocked() const {
    // Callers hold mutex_.
    //
    // Attribution means "AFC is telling us, right now, which lane needs help".
    // Two different things can leave a name behind that is not that.
    //
    // 1. A toolchange that FAILED. active_load_lane_ prefers AFC.current_lane
    //    (= AFC.current_loading), which upstream sets at the top of TOOL_LOAD
    //    (AFC.py v1.2.0:1523) and TOOL_UNLOAD (:1948) and clears in exactly two
    //    places — set_tool_loaded() and set_tool_unloaded() — both under
    //    `if normal_toolchange:` (AFC_lane.py:1526, :1545). That is the SUCCESS
    //    path only, so a failed toolchange pins the name until the next
    //    successful one, which a user with a jammed lane cannot perform.
    //
    //    Left standing, that latch outranks Eject in decide_unload_mode(), where
    //    can_recover && attributed wins. The failure that makes someone want to
    //    eject is then the exact thing that removes the Eject button, swapping it
    //    for a lane reset that retracts toward the hub and never returns the
    //    filament to the spool.
    //
    //    The IDLE discrimination that follows from this lives in
    //    lane_recovery_is_attributed(), NOT here — see the note below.
    //
    // 2. A lane rename or unit re-init can leave active_load_lane_ naming a lane
    //    that no longer exists — initialize_slots() never touches it. A stale name
    //    is not attribution, and both the recovery gate and the UI-facing flag must
    //    read it that way or they disagree about the same lane.
    return !active_load_lane_.empty() && slots_.index_of(active_load_lane_) >= 0;
}

bool AmsBackendAfc::lane_recovery_is_attributed() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Narrower than can_recover_lane_position()'s use of the same name, because
    // the two ask different questions of it.
    //
    //   can_recover_lane_position() asks MAY WE OFFER Recover for this lane. It
    //   needs a name so a blind opening retract cannot land on the wrong lane
    //   (#1182) — a safety gate, and a latched name still identifies the right
    //   lane, so it must keep answering true here or a genuinely stranded lane
    //   loses its recovery entirely.
    //
    //   This function asks SHOULD Recover OUTRANK Eject in decide_unload_mode().
    //   That needs the diagnosis to be live, not residue. AFC.current_loading is
    //   cleared only on the success path of a toolchange (AFC_lane.py:1526,
    //   :1545), so a FAILED one pins the name until the next successful
    //   toolchange — which a user with a jammed lane cannot perform. Treating
    //   that as a confident diagnosis means the failure that makes someone want
    //   Eject is the very thing that removes the Eject button, swapping it for a
    //   lane reset that retracts toward the hub and never returns filament to
    //   the spool.
    //
    // AFC's action discriminates: a toolchange genuinely under way (or faulted
    // into ERROR) is non-IDLE and its name describes live work. Back at IDLE the
    // name is residue, so the lane drops to the unattributed arm of the ranking —
    // Recover stays available as a last resort, it just no longer displaces Eject.
    if (system_info_.action == AmsAction::IDLE) {
        return false;
    }
    return recovery_attribution_valid_unlocked();
}

AmsError AmsBackendAfc::recover_lane_position(int slot_index) {
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
    // No gate on cmd_LANE_UNLOAD's own if/elif chain. Those conditions differ
    // across AFC versions and there is no reliable version to read (the
    // afc-install database key reports 1.0.0 on installs that are not), so any
    // copy of them here is a guess that goes stale — see
    // prestonbrown/helixscreen#1258. Send the macro and let AFC decide.
    //
    // What that costs: AFC's own refusals reach the user only as far as AFC
    // reports them. "Lane is loaded in toolhead" is a bare logger.info, console
    // only, absent from AFC.message, so it is silent here. Matching that console
    // string would re-create exactly the coupling this shed; the fix belongs
    // upstream (promote it to logger.warning, which reaches message_queue and so
    // AFC.message, which parse_afc_state() already toasts).
    //
    // refuse_if_printing() below is deliberately NOT part of that removal. It is
    // one stable predicate, the first line of cmd_LANE_UNLOAD at every AFC
    // version checked, and it is the same rule the context menu already greys the
    // button on (cold_lane_ops_refused_during_print). Dropping it here while
    // keeping it there would leave the two layers disagreeing.
    std::string lane_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (AmsError printing = refuse_if_printing(); !printing) {
            return printing;
        }

        AmsError slot_err = validate_slot_index(slot_index);
        if (!slot_err) {
            return slot_err;
        }

        lane_name = slots_.name_of(slot_index);
        if (lane_name.empty()) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }
    }

    return enqueue_lane_unload(lane_name);
}

AmsError AmsBackendAfc::enqueue_lane_unload(const std::string& lane_name) {
    bool fire_now = false;
    {
        std::lock_guard<std::mutex> lock(eject_queue_mutex_);
        if (eject_in_flight_) {
            // Avoid enqueuing duplicate consecutive entries — UI repeat-tap.
            if (pending_eject_lanes_.empty() || pending_eject_lanes_.back() != lane_name) {
                pending_eject_lanes_.push_back(lane_name);
            }
            spdlog::info("[AMS AFC] Queued LANE_UNLOAD lane={} (queue depth: {})", lane_name,
                         pending_eject_lanes_.size());
        } else {
            eject_in_flight_ = true;
            fire_now = true;
        }
    }
    if (fire_now) {
        dispatch_lane_unload(lane_name);
    }
    return AmsErrorHelper::success();
}

void AmsBackendAfc::dispatch_lane_unload(const std::string& lane_name) {
    if (!api_) {
        spdlog::error("[AMS AFC] Cannot fire LANE_UNLOAD: api_ is null");
        on_lane_unload_done();
        return;
    }
    spdlog::info("[AMS AFC] Ejecting lane {}", lane_name);
    auto tok = lifetime_.token();
    api_->execute_gcode(
        "LANE_UNLOAD LANE=" + lane_name,
        [this, tok, lane_name]() {
            // L081 Mechanism C: on_lane_unload_done touches members under lock + redispatches.
            tok.defer("AmsBackendAfc::dispatch_lane_unload_done", [this, lane_name]() {
                spdlog::debug("[AMS AFC] LANE_UNLOAD lane={} completed", lane_name);
                on_lane_unload_done();
            });
        },
        [this, tok, lane_name](const MoonrakerError& err) {
            // L081 Mechanism C: on_lane_unload_done touches members under lock + redispatches.
            tok.defer("AmsBackendAfc::dispatch_lane_unload_error", [this, lane_name,
                                                                    err_type = err.type,
                                                                    err_msg = err.message]() {
                if (err_type == MoonrakerErrorType::TIMEOUT) {
                    spdlog::warn("[AMS AFC] LANE_UNLOAD lane={} response timed out (may still be "
                                 "running): {}",
                                 lane_name, err_msg);
                } else {
                    spdlog::error("[AMS AFC] LANE_UNLOAD lane={} failed: {}", lane_name, err_msg);
                }
                on_lane_unload_done();
            });
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS, /*silent=*/false, /*on_queued=*/nullptr,
        // The error callback logs and pumps the eject queue; it shows the user
        // nothing, so GcodeErrorRouter keeps the report for a rejected
        // LANE_UNLOAD. See include/rpc_error_policy.h.
        /*caller_surfaces_errors=*/false);
}

void AmsBackendAfc::on_lane_unload_done() {
    std::string next_lane;
    {
        std::lock_guard<std::mutex> lock(eject_queue_mutex_);
        if (pending_eject_lanes_.empty()) {
            eject_in_flight_ = false;
            return;
        }
        next_lane = std::move(pending_eject_lanes_.front());
        pending_eject_lanes_.pop_front();
    }
    dispatch_lane_unload(next_lane);
}

AmsError AmsBackendAfc::cancel() {
    // Drop any queued LANE_UNLOAD requests — the user wants to abort, not chain
    // through a pile of pending ejects after RESET_FAILURE. We deliberately do
    // NOT clear `eject_in_flight_`: the in-flight LANE_UNLOAD's completion
    // callback will still fire and call on_lane_unload_done(), which sees the
    // empty queue and clears the flag itself. Clearing the flag here would
    // leave us out of sync with the (still pending) callback.
    {
        std::lock_guard<std::mutex> lock(eject_queue_mutex_);
        if (!pending_eject_lanes_.empty()) {
            spdlog::info("[AMS AFC] Cancel: discarding {} queued LANE_UNLOAD request(s)",
                         pending_eject_lanes_.size());
            pending_eject_lanes_.clear();
        }
    }

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
    // Set when the material could not be expressed as a G-code parameter. Reported
    // after every other write has gone out, so a name AFC cannot store costs the user
    // only the material rather than the whole save — but is never silent.
    std::string rejected_material;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }
        auto& slot = entry->info;

        // Capture old spoolman_id before updating for clear detection
        int old_spoolman_id = slot.spoolman_id;
        int old_mapped_tool = slot.mapped_tool;

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
            spdlog::info("[AMS AFC] Updated slot {} info: {} {}", slot_index, info.material,
                         info.color_name);
        }

        // Persist via G-code commands when persist=true.
        // Skip persistence when persist=false — this is used by Spoolman weight
        // polling (refresh_spoolman_weights) to update in-memory state without
        // sending G-code back to AFC firmware. Without this guard, each weight
        // update would fire SET_COLOR/SET_MATERIAL/SET_WEIGHT/SET_SPOOL_ID G-codes,
        // which trigger AFC status_update WebSocket events, which call
        // sync_from_backend → refresh_spoolman_weights → set_slot_info again,
        // creating an infinite feedback loop that saturates the CPU.
        //
        // Record the user's identity in the override store as well. AFC cannot
        // hold brand / spool_name / total_weight / colour name / filament+vendor
        // ids at all, and the fields it DOES hold get cleared by its own
        // clear_values() on eject.
        if (persist) {
            persist_override(slot_index, info);
        }

        // Persistence is never version-gated. These SET_* commands have existed
        // since well before any version we would recognize, and the version string
        // is not a usable signal (AFC stopped writing it — see
        // apply_afc_version_response). Skipping gcode on an unrecognized version
        // caused issue #644, where spool assignment silently bypassed AFC.
        if (persist) {
            std::string lane_name = slots_.name_of(slot_index);
            if (!lane_name.empty()) {
                // Spoolman ID FIRST — both branches of AFC's set_spoolID() rewrite the
                // lane's material/color/weight/temps, so this must precede our own
                // writes or it clobbers them:
                //   valid id  -> AFC fetches the spool from Spoolman and overwrites
                //               material, color, weight, temps, density, diameter
                //   empty id  -> AFC runs clear_values() and wipes all of the above
                // Emitting it last made a single save set the data and then destroy it,
                // which is why an edit needed two passes to stick.
                //
                // Record the write before dispatching: in-flight status frames
                // keep reporting old_spoolman_id until the echo lands, and
                // Rule 1 must not read those as an external re-bind. An
                // unlink (id 0) erases the pending expectation instead.
                record_own_spool_write(slot_index, info.spoolman_id, old_spoolman_id);
                if (info.spoolman_id > 0) {
                    execute_gcode(fmt::format("SET_SPOOL_ID LANE={} SPOOL_ID={}", lane_name,
                                              info.spoolman_id));
                } else if (info.spoolman_id == 0 && old_spoolman_id > 0) {
                    // Clear Spoolman link with empty string (not -1)
                    execute_gcode(fmt::format("SET_SPOOL_ID LANE={} SPOOL_ID=", lane_name));
                }

                // Color (only if changed and valid - not 0 or default grey)
                if (info.color_rgb != 0 && info.color_rgb != AMS_DEFAULT_SLOT_COLOR) {
                    char color_hex[8];
                    snprintf(color_hex, sizeof(color_hex), "%06X", info.color_rgb & 0xFFFFFF);
                    execute_gcode(fmt::format("SET_COLOR LANE={} COLOR={}", lane_name, color_hex));
                }

                // Material (validate to prevent command injection). The material
                // charset is deliberately wider than an identifier's: `PLA+`,
                // `PA6-CF` and `Silk PLA` are all in our own filament database, and
                // gating this on is_safe_gcode_param() dropped every one of them.
                if (!info.material.empty() &&
                    IMoonrakerAPI::is_safe_material_param(info.material)) {
                    execute_gcode(fmt::format("SET_MATERIAL LANE={} MATERIAL={}", lane_name,
                                              IMoonrakerAPI::gcode_param_value(info.material)));
                } else if (!info.material.empty()) {
                    spdlog::warn("[AMS AFC] Skipping SET_MATERIAL - unsafe characters in: {}",
                                 info.material);
                    rejected_material = info.material;
                }

                // Weight (if valid)
                if (info.remaining_weight_g > 0) {
                    execute_gcode(fmt::format("SET_WEIGHT LANE={} WEIGHT={:.0f}", lane_name,
                                              info.remaining_weight_g));
                }

                // Tool mapping (lane → tool number) via SET_MAP.
                // Mirrors set_tool_mapping() but is reachable from the slot edit modal,
                // which routes all changes through set_slot_info().
                if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0) {
                    execute_gcode(
                        fmt::format("SET_MAP LANE={} MAP=T{}", lane_name, info.mapped_tool));
                }
            }
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

        if (!helix::bypass_available_for(system_info_.supports_bypass)) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            "This AFC system does not support bypass mode", "");
        }

        // Explicit precondition, not a redundant guard. AmsSubscriptionBackend::
        // execute_gcode() is fire-and-forget — it returns success before Klipper
        // answers and passes silent=true — so a bypass request with filament at
        // the toolhead would report success and change nothing. Refusing here is
        // what makes allows_implicit_chaining() == false safe: the sidebar no
        // longer unloads on AFC's behalf, so AFC has to give the answer (#1229).
        if (system_info_.filament_loaded) {
            return AmsError(AmsResult::WRONG_STATE, "Unload filament first",
                            "Filament is loaded at the toolhead. Unload it before enabling bypass.",
                            "");
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
    // AFC has no global on/off switch: a lane either names a runout lane or it
    // does not, so the feature is On whenever it is present. Editing is per-slot
    // via SET_RUNOUT, one lane at a time, with no side effects on other lanes.
    // provider stays empty: AFC implements this itself, there is no optional
    // package to name.
    return {.availability = helix::printer::EndlessSpoolAvailability::Available,
            .enabled = helix::printer::EndlessSpoolEnabled::On,
            .editability = helix::printer::EndlessSpoolEditability::PerSlot};
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

uint64_t AmsBackendAfc::firmware_tool_mapping_generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.firmware_mapping_generation();
}

helix::printer::EndlessSpoolConfig AmsBackendAfc::get_endless_spool_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return helix::printer::endless_spool_config_from_edges(slots_.backup_edges());
}

AmsError AmsBackendAfc::apply_endless_spool_backup(int slot_index, int backup_slot) {
    std::string lane_name;
    std::string backup_lane_name;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Ranges are the base's job; this only resolves names. A registry that
        // has not caught up with the slot count the base validated against still
        // yields an empty name, which the injection screen below rejects.
        lane_name = slots_.name_of(slot_index);
        if (backup_slot >= 0) {
            backup_lane_name = slots_.name_of(backup_slot);
        }
    }

    // Validate lane names to prevent command injection. Empty counts as unsafe:
    // `SET_RUNOUT LANE=` is a malformed command, not a no-op.
    if (lane_name.empty() || !IMoonrakerAPI::is_safe_gcode_param(lane_name)) {
        spdlog::warn("[AMS AFC] Unsafe lane name characters in endless spool config");
        return AmsError(AmsResult::MAPPING_ERROR, "Invalid lane name",
                        "Lane name contains invalid characters", "Check AFC configuration");
    }
    if (backup_slot >= 0 &&
        (backup_lane_name.empty() || !IMoonrakerAPI::is_safe_gcode_param(backup_lane_name))) {
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

    AmsError result = execute_gcode(gcode);
    if (!result.success()) {
        // Leave the registry alone: the printer did not take the change, and a
        // desynced mirror would render an arrow the hardware will not honour.
        return result;
    }
    // Optimistic mirror so the arrows and the dropdown update before the next
    // AFC status frame lands; that frame is authoritative and will overwrite it.
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.set_backup(slot_index, backup_slot);
    return result;
}

AmsError AmsBackendAfc::reset_tool_mappings() {
    spdlog::info("[AMS AFC] Resetting tool mappings");

    // RUNOUT=no keeps the endless-spool lanes out of the reset on both macro
    // generations. The name flipped in #832 and the old one is DEREGISTERED
    // there, so guessing wrong is an "unknown command" error, not a no-op —
    // hence the presence-detected latch rather than a version floor.
    const char* macro = afc_reset_mapping_renamed_ ? "AFC_RESET_MAPPING" : "RESET_AFC_MAPPING";
    AmsError result = execute_gcode(fmt::format("{} RUNOUT=no", macro));

    // Tool mapping will be refreshed from next status update
    return result;
}

// reset_endless_spool() is not overridden: AFC has no command that resets only
// the runout lanes, and the "loop the setter with -1" fallback that used to live
// here is now AmsBackend::reset_endless_spool() for every editable backend.

// ============================================================================
// AFC Config File Management
// ============================================================================

void AmsBackendAfc::load_afc_configs() {
    if (configs_loading_.load() || configs_loaded_.load()) {
        return;
    }

    if (!api_) {
        spdlog::warn("[AMS AFC] Cannot load configs: IMoonrakerAPI is null");
        return;
    }

    configs_loading_ = true;

    // Create managers if not yet created (share lifetime token for callback safety)
    if (!afc_config_) {
        afc_config_ = std::make_unique<AfcConfigManager>(api_, lifetime_.token());
    }
    if (!macro_vars_config_) {
        macro_vars_config_ = std::make_unique<AfcConfigManager>(api_, lifetime_.token());
    }

    // Track completion of both loads
    auto loads_remaining = std::make_shared<std::atomic<int>>(2);

    // Callbacks from download_file run on the libhv background thread.
    // configs_loaded_ is std::atomic<bool> — the store (release) after both loads complete
    // synchronizes-with the load (acquire) in get_device_actions() on the main thread,
    // ensuring all parser writes are visible before the main thread reads them.
    auto token = lifetime_.token();
    auto check_done = [this, token, loads_remaining]() {
        // L081 Mechanism C: download_file callbacks run on libhv bg thread.
        // Decrement the atomic on bg (it's a shared_ptr, safe), but marshal
        // the member writes + update_tip_method_from_config() + event emit to main.
        if (loads_remaining->fetch_sub(1) != 1) {
            return;
        }
        token.defer("AmsBackendAfc::load_afc_configs_done", [this]() {
            // Both loads complete — release barrier ensures parser state is visible
            configs_loading_.store(false, std::memory_order_relaxed);
            configs_loaded_.store(true, std::memory_order_release);
            spdlog::info("[AMS AFC] Config files loaded");

            // Detect tip method from AFC config.
            // TODO: Replace with direct Moonraker status query once AFC exposes
            // tool_cut/form_tip in get_status() (upstream AFC enhancement pending).
            update_tip_method_from_config();

            emit_event(EVENT_STATE_CHANGED);
        });
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
    std::lock_guard<std::mutex> lock(mutex_);
    auto sections = helix::printer::afc_default_sections();

    // Hide tip forming section when tip forming isn't the active method
    if (system_info_.tip_method != TipMethod::TIP_FORM) {
        sections.erase(std::remove_if(sections.begin(), sections.end(),
                                      [](const helix::printer::DeviceSection& s) {
                                          return s.id == "tip_forming";
                                      }),
                       sections.end());
    }

    return sections;
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

    // Overlay toolhead distances from first extruder (single-extruder default)
    if (!extruders_.empty()) {
        const auto& ext = extruders_[0];
        for (auto& a : actions) {
            if (a.id == "tool_stn") {
                a.current_value = std::any(ext.tool_stn);
            } else if (a.id == "tool_stn_unload") {
                a.current_value = std::any(ext.tool_stn_unload);
            } else if (a.id == "tool_sensor_after_extruder") {
                a.current_value = std::any(ext.tool_sensor_after_extruder);
            }
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

    // Multi-extruder: replace single toolhead actions with per-extruder
    if (num_extruders_ > 1 && !extruders_.empty()) {
        actions.erase(std::remove_if(actions.begin(), actions.end(),
                                     [](const DeviceAction& a) {
                                         return a.id == "tool_stn" || a.id == "tool_stn_unload" ||
                                                a.id == "tool_sensor_after_extruder";
                                     }),
                      actions.end());

        for (int i = 0; i < static_cast<int>(extruders_.size()); ++i) {
            const auto& ext = extruders_[i];
            std::string suffix = "_T" + std::to_string(i);
            std::string tool_label = " (T" + std::to_string(i) + ")";

            actions.push_back(
                DeviceAction{"tool_stn" + suffix,
                             "Sensor to Nozzle" + tool_label,
                             "ruler",
                             "toolhead",
                             "Distance from toolhead sensor to nozzle for T" + std::to_string(i),
                             ActionType::SLIDER,
                             std::any(ext.tool_stn),
                             {},
                             0.0f,
                             200.0f,
                             "mm",
                             -1,
                             true,
                             ""});
            actions.push_back(DeviceAction{"tool_stn_unload" + suffix,
                                           "Unload Distance" + tool_label,
                                           "ruler",
                                           "toolhead",
                                           "Retraction distance for T" + std::to_string(i),
                                           ActionType::SLIDER,
                                           std::any(ext.tool_stn_unload),
                                           {},
                                           0.0f,
                                           200.0f,
                                           "mm",
                                           -1,
                                           true,
                                           ""});
            actions.push_back(DeviceAction{"tool_sensor_after_extruder" + suffix,
                                           "Post-Sensor Clear" + tool_label,
                                           "ruler",
                                           "toolhead",
                                           "Extra clear distance for T" + std::to_string(i),
                                           ActionType::SLIDER,
                                           std::any(ext.tool_sensor_after_extruder),
                                           {},
                                           0.0f,
                                           100.0f,
                                           "mm",
                                           -1,
                                           true,
                                           ""});
        }
    }

    // Multi-extruder: replace single led_extruder with per-extruder LED toggles
    if (num_extruders_ > 1 && !extruders_.empty()) {
        actions.erase(std::remove_if(actions.begin(), actions.end(),
                                     [](const DeviceAction& a) { return a.id == "led_extruder"; }),
                      actions.end());
        for (int i = 0; i < static_cast<int>(extruders_.size()); ++i) {
            auto it = toolhead_led_state_.find(i);
            bool led_on = (it != toolhead_led_state_.end()) && it->second;
            actions.push_back(DeviceAction{
                fmt::format("led_extruder_T{}", i),
                fmt::format("{} Toolhead LED (T{})", led_on ? "Turn Off" : "Turn On", i),
                led_on ? "lightbulb-off" : "lightbulb-on",
                "setup",
                fmt::format("Toggle toolhead LED for T{}", i),
                ActionType::BUTTON,
                {},
                {},
                0,
                0,
                "",
                -1,
                true,
                ""});
        }
    }

    // Per-lane dist_hub actions in the hub section
    for (int i = 0; i < slots_.slot_count(); ++i) {
        const auto* entry = slots_.get(i);
        if (!entry)
            continue;
        std::string lane_name = slots_.name_of(i);
        std::string id = "dist_hub_" + lane_name;
        std::string label = "Hub Distance (" + lane_name + ")";
        float current = entry->sensors.dist_hub;

        actions.push_back(DeviceAction{id,
                                       label,
                                       "ruler",
                                       "hub",
                                       "Distance from lane extruder to hub",
                                       ActionType::SLIDER,
                                       std::any(current),
                                       {},
                                       0.0f,
                                       std::max(500.0f, current * 1.5f),
                                       "mm",
                                       i,
                                       true,
                                       ""});
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

        // Purge & Wipe actions — from afc_config_ [AFC] and [AFC_poop] sections
        else if (a.id == "purge_enabled") {
            if (cfg_ready) {
                a.current_value = std::any(afc_config_->parser().get_bool("AFC", "poop", false));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "purge_length") {
            if (cfg_ready) {
                a.current_value =
                    std::any(afc_config_->parser().get_float("AFC_poop", "purge_length", 70.0f));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        } else if (a.id == "brush_enabled") {
            if (cfg_ready) {
                a.current_value = std::any(afc_config_->parser().get_bool("AFC", "wipe", false));
                a.enabled = true;
            } else {
                a.enabled = false;
                a.disable_reason = not_loaded_reason;
            }
        }

        // (config section removed — all changes are immediate)
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
        std::lock_guard<std::mutex> lock(mutex_);
        // ENABLE must be explicit. AFC's cmd_AFC_QUIET_MODE defaults the
        // parameter to the CURRENT value — `gcmd.get_int("ENABLE",
        // self._get_quiet_mode(), ...)` (AFC.py v1.2.0:934-953, identical at
        // v1.1.0:736-756) — so a bare `AFC_QUIET_MODE` sets quiet mode to what
        // it already was and the button did nothing.
        //
        // The `show_macros` wrapper does not supply one either: _create_options
        // emits `{%set dummy=params.ENABLE|default('0')|int%}` followed by
        // `_AFC_QUIET_MODE {rawparams}` (AFC_functions.py:637-644), and the
        // `dummy` assignment is discarded — only rawparams reaches the real
        // command, and rawparams is empty for a bare call.
        //
        // Same shape as led_toggle above: we hold the state, so we send the
        // value we want rather than asking for a toggle AFC has no verb for.
        return execute_gcode(afc_quiet_mode_ ? "AFC_QUIET_MODE ENABLE=0"
                                             : "AFC_QUIET_MODE ENABLE=1");
    } else if (action_id.rfind("led_extruder_T", 0) == 0) {
        // Per-extruder toolhead LED toggle: led_extruder_T0, led_extruder_T1, etc.
        try {
            int tool_idx = std::stoi(action_id.substr(14));
            std::lock_guard<std::mutex> lock(mutex_);
            if (tool_idx < 0 || tool_idx >= static_cast<int>(extruders_.size())) {
                return AmsErrorHelper::not_supported("Invalid extruder index: " +
                                                     std::to_string(tool_idx));
            }
            bool currently_on = toolhead_led_state_[tool_idx];
            int turn_on = currently_on ? 0 : 1;
            auto err = execute_gcode(fmt::format("AFC_SET_EXTRUDER_LED EXTRUDER={} TURN_ON={}",
                                                 extruders_[tool_idx].name, turn_on));
            if (err) {
                toolhead_led_state_[tool_idx] = !currently_on;
            }
            return err;
        } catch (const std::exception&) {
            return AmsErrorHelper::not_supported("Invalid LED action: " + action_id);
        }
    }

    // Per-lane dist_hub actions
    if (action_id.rfind("dist_hub_", 0) == 0) {
        std::string lane_name = action_id.substr(9);
        if (!value.has_value()) {
            return AmsError(AmsResult::WRONG_STATE, "Value required", "Missing value", "");
        }
        try {
            float val = std::any_cast<float>(value);
            AmsError err =
                execute_gcode(fmt::format("SET_HUB_DIST LANE={} LENGTH={:g}", lane_name, val));
            if (!err)
                return err;
            AmsError save_err = execute_gcode("SAVE_HUB_DIST LANE=" + lane_name);
            if (!save_err) {
                spdlog::warn("[AMS AFC] SAVE_HUB_DIST failed (runtime value was set)");
            }
            return AmsErrorHelper::success();
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid value type", "Expected float", "");
        }
    }

    // ---- Toolhead distance actions (single + multi-extruder) ----
    // Longest-prefix-first: tool_stn is a prefix of tool_stn_unload
    auto parse_toolhead_action = [](const std::string& id) -> std::pair<std::string, int> {
        static const std::vector<std::string> fields = {"tool_sensor_after_extruder",
                                                        "tool_stn_unload", "tool_stn"};
        for (const auto& field : fields) {
            if (id == field) {
                return {field, 0};
            }
            if (id.rfind(field + "_T", 0) == 0) {
                try {
                    int idx = std::stoi(id.substr(field.size() + 2));
                    return {field, idx};
                } catch (...) {
                }
            }
        }
        return {"", -1};
    };

    auto [th_field, th_tool] = parse_toolhead_action(action_id);
    if (!th_field.empty()) {
        if (!value.has_value()) {
            return AmsError(AmsResult::WRONG_STATE, "Value required", "Missing value", "");
        }
        try {
            float val = std::any_cast<float>(value);

            // UPDATE_TOOLHEAD_SENSORS / SAVE_EXTRUDER_VALUES are mux commands
            // keyed on the AFC_extruder SECTION name (AFC_extruder.py:364-369,
            // muxed on self.name = the section suffix), not on the Klipper
            // extruder name. Rebuilding "extruder<N>" here addressed a mux key
            // that does not exist on a renamed config, so both commands failed.
            // Same key the sibling AFC_SET_EXTRUDER_LED action already uses.
            std::string ext_name;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ext_name = afc_extruder_section_for_tool_unlocked(th_tool);
            }
            if (ext_name.empty()) {
                ext_name = (th_tool > 0) ? "extruder" + std::to_string(th_tool) : "extruder";
            }

            std::string param;
            if (th_field == "tool_stn")
                param = "TOOL_STN";
            else if (th_field == "tool_stn_unload")
                param = "TOOL_STN_UNLOAD";
            else if (th_field == "tool_sensor_after_extruder")
                param = "TOOL_AFTER_EXTRUDER";

            std::string cmd =
                fmt::format("UPDATE_TOOLHEAD_SENSORS EXTRUDER={} {}={:g}", ext_name, param, val);
            AmsError err = execute_gcode(cmd);
            if (!err)
                return err;

            AmsError save_err = execute_gcode("SAVE_EXTRUDER_VALUES EXTRUDER=" + ext_name);
            if (!save_err) {
                spdlog::warn("[AMS AFC] SAVE_EXTRUDER_VALUES failed (runtime value was set)");
            }
            return AmsErrorHelper::success();
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid value type", "Expected float", "");
        }
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

    // ---- Config-backed macro var actions (tip forming — macro_vars_config_) ----
    static const std::unordered_map<std::string, std::string> macro_var_slider_keys = {
        {"ramming_volume", "variable_ramming_volume"},
        {"unloading_speed_start", "variable_unloading_speed_start"},
        {"cooling_tube_length", "variable_cooling_tube_length"},
        {"cooling_tube_retraction", "variable_cooling_tube_retraction"},
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

    // ---- Purge/wipe toggles — immediate via AFC_TOGGLE_MACRO G-code ----
    if (action_id == "purge_enabled") {
        try {
            bool val = std::any_cast<bool>(value);
            execute_gcode(fmt::format("AFC_TOGGLE_MACRO POOP={}", val ? 1 : 0));
            return AmsErrorHelper::success();
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid value type", "Expected boolean", "");
        }
    }
    if (action_id == "brush_enabled") {
        try {
            bool val = std::any_cast<bool>(value);
            execute_gcode(fmt::format("AFC_TOGGLE_MACRO WIPE={}", val ? 1 : 0));
            return AmsErrorHelper::success();
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid value type", "Expected boolean", "");
        }
    }

    // ---- Purge length — config-backed, no runtime G-code available ----
    if (action_id == "purge_length") {
        if (!afc_config_ || !afc_config_->is_loaded()) {
            return AmsError(AmsResult::WRONG_STATE, "AFC config not loaded",
                            "Configuration not available", "Wait for config to load");
        }
        try {
            float val = std::any_cast<float>(value);
            afc_config_->parser().set("AFC_poop", "purge_length", fmt::format("{:g}", val));
            afc_config_->mark_dirty();
            afc_config_->save("AFC/AFC.cfg", [](bool ok, const std::string& err) {
                if (!ok)
                    spdlog::error("[AMS AFC] Failed to save purge_length: {}", err);
            });
            return AmsErrorHelper::success();
        } catch (const std::bad_any_cast&) {
            return AmsError(AmsResult::WRONG_STATE, "Invalid value type", "Expected float", "");
        }
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}
