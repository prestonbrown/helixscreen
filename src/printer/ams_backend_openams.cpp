// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_openams.h"

#include "ui_insert_notice.h"
#include "ui_update_queue.h"

#include "i_moonraker_api.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "openams_api.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

#include "hv/json.hpp"

namespace helix {
namespace {

using json = nlohmann::json;

constexpr const char* kLoad = "load";
constexpr const char* kUnload = "unload";
constexpr const char* kCancel = "cancel";
constexpr const char* kReset = "reset";

const json* object_member(const json& object, const char* key) {
    auto it = object.find(key);
    return it != object.end() && it->is_object() ? &(*it) : nullptr;
}

const json& array_member(const json& object, const char* key) {
    static const json kEmpty = json::array();
    auto it = object.find(key);
    return it != object.end() && it->is_array() ? *it : kEmpty;
}

std::string string_member(const json& object, const char* key, std::string fallback = {}) {
    auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}

bool bool_member(const json& object, const char* key, bool fallback) {
    auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

int int_member(const json& object, const char* key, int fallback) {
    auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

/// A command name is sent verbatim, so it must be one G-code word.
bool command_name_is_safe(const std::string& command) {
    return !command.empty() && std::all_of(command.begin(), command.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_';
    });
}

/// Tool number a `T<n>` group stands for, or -1 for any other group name.
int tool_from_group(const std::string& group) {
    if (group.size() < 2 || group.size() > 4 || group.front() != 'T') {
        return -1;
    }
    int value = 0;
    for (std::size_t i = 1; i < group.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(group[i]);
        if (std::isdigit(ch) == 0) {
            return -1;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

std::optional<PathTopology> topology_from_token(const std::string& topology) {
    const std::string token = ams_normalize_state_token(topology);
    if (token == "hub")
        return PathTopology::HUB;
    if (token == "linear")
        return PathTopology::LINEAR;
    if (token == "parallel")
        return PathTopology::PARALLEL;
    if (token == "mixed")
        return PathTopology::MIXED;
    return std::nullopt;
}

/// Put @p info's filament fields on @p slot, so get_slot_info returns them at
/// once rather than after the next frame.
void write_filament_fields(SlotInfo& slot, const SlotInfo& info) {
    slot.color_rgb = info.color_rgb;
    slot.color_name = info.color_name;
    slot.multi_color_hexes = info.multi_color_hexes;
    slot.material = info.material;
    slot.brand = info.brand;
    slot.catalog_id = info.catalog_id;
    slot.product_name = info.product_name;
    slot.spool_name = info.spool_name;
    slot.spoolman_id = info.spoolman_id;
    slot.spoolman_filament_id = info.spoolman_filament_id;
    slot.spoolman_vendor_id = info.spoolman_vendor_id;
    slot.remaining_weight_g = info.remaining_weight_g;
    slot.total_weight_g = info.total_weight_g;
    slot.nozzle_temp_min = info.nozzle_temp_min;
    slot.nozzle_temp_max = info.nozzle_temp_max;
    slot.bed_temp = info.bed_temp;
}

} // namespace

json AmsBackendOpenAms::required_status_objects(const PrinterDiscovery& hw) {
    json objects = json::object();
    if (hw.mmu_type() == AmsType::OPENAMS) {
        objects[openams::kManagerObject] =
            json::array({"api_version", "schema", "ready", "commands", "lanes", "units", "groups"});
    }
    return objects;
}

AmsBackendOpenAms::AmsBackendOpenAms(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    system_info_.type = AmsType::OPENAMS;
    system_info_.type_name = "OpenAMS"; // i18n: do not translate - product name
    system_info_.version = "unknown";
    system_info_.supports_bypass = false;
}

// ============================================================================
// Lifecycle and status
// ============================================================================

void AmsBackendOpenAms::on_started() {
    if (!api_) {
        return;
    }
    // Blocks on the Moonraker DB, so no lock is held; the result is published
    // under the lock in one move.
    auto loaded =
        helix::ams::make_loaded_override_store(api_, "openams", get_type(), backend_log_tag());
    if (loaded.store) {
        helix::ams::ingest_legacy_records(*loaded.store, helix::ams::LegacyLockKeys::LaneData,
                                          backend_index());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        override_store_ = std::move(loaded.store);
        overrides_ = std::move(loaded.overrides);
        parse_snapshot_locked();
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendOpenAms::handle_status_update(const json& notification) {
    const json* objects = &notification;
    auto params = notification.find("params");
    if (params != notification.end() && params->is_array() && !params->empty() &&
        (*params)[0].is_object()) {
        objects = &(*params)[0];
    }
    const json* update = object_member(*objects, openams::kManagerObject);
    if (!update) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = update->begin(); it != update->end(); ++it) {
            snapshot_[it.key()] = it.value();
        }
        parse_snapshot_locked();
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendOpenAms::present_nothing_locked() {
    api_supported_ = false;
    manager_ready_ = false;
    reported_action_ = AmsAction::IDLE;
    lane_states_.clear();
    present_by_slot_id_.clear();
    remote_slot_ids_.clear();
    slot_groups_.clear();
    groups_.clear();
    commands_.clear();
    topology_ = PathTopology::HUB;

    system_info_.units.clear();
    system_info_.total_slots = 0;
    system_info_.tool_to_slot_map.clear();
    system_info_.current_slot = -1;
    system_info_.current_tool = -1;
    system_info_.filament_loaded = false;
    system_info_.action = pending_action_;
    system_info_.operation_detail.clear();
}

void AmsBackendOpenAms::parse_snapshot_locked() {
    if (!openams::api_supported(snapshot_)) {
        if (api_supported_) {
            spdlog::warn("{} oams_manager stopped publishing a supported UI API; presenting "
                         "no slots",
                         backend_log_tag());
        }
        present_nothing_locked();
        return;
    }

    std::unordered_map<std::string, std::string> next_commands;
    if (const json* commands = object_member(snapshot_, "commands")) {
        for (const char* action : {kLoad, kUnload, kCancel, kReset}) {
            const std::string command = string_member(*commands, action);
            if (command.empty()) {
                continue;
            }
            if (!command_name_is_safe(command)) {
                spdlog::warn("{} Ignoring unsafe {} command name '{}'", backend_log_tag(), action,
                             command);
                continue;
            }
            next_commands[action] = command;
        }
    }

    AmsSystemInfo next;
    next.type = AmsType::OPENAMS;
    next.type_name = system_info_.type_name;
    next.version = std::to_string(openams::kApiVersion);
    next.supports_bypass = false;

    std::vector<int> next_remote_ids;
    std::unordered_map<int, int> remote_to_global;
    std::set<PathTopology> topologies;

    for (const auto& unit_json : array_member(snapshot_, "units")) {
        if (!unit_json.is_object()) {
            continue;
        }
        AmsUnit unit;
        unit.unit_index = static_cast<int>(next.units.size());
        unit.name = string_member(unit_json, "id", std::to_string(unit.unit_index));
        unit.display_name = string_member(unit_json, "name", unit.name);
        unit.first_slot_global_index = next.total_slots;
        unit.connected = bool_member(unit_json, "connected", true);
        unit.topology = PathTopology::HUB;
        const std::string topology = string_member(unit_json, "topology");
        if (!topology.empty()) {
            if (auto parsed = topology_from_token(topology)) {
                unit.topology = *parsed;
            } else {
                // Topology only shapes the drawing; slots are addressed by id,
                // so an unfamiliar shape still loads and unloads correctly.
                spdlog::debug("{} Unit '{}' has unknown topology '{}'; drawing it as a hub",
                              backend_log_tag(), unit.name, topology);
            }
        }
        topologies.insert(unit.topology);

        for (const auto& slot_json : array_member(unit_json, "slots")) {
            if (!slot_json.is_object()) {
                continue;
            }
            const int remote_id = int_member(slot_json, "id", -1);
            if (remote_id < 0 || remote_to_global.count(remote_id) != 0) {
                continue;
            }
            SlotInfo slot;
            slot.slot_index = int_member(slot_json, "bay", static_cast<int>(unit.slots.size()));
            slot.global_index = next.total_slots++;
            if (bool_member(slot_json, "loaded", false)) {
                slot.status = SlotStatus::LOADED;
            } else if (bool_member(slot_json, "ready", false)) {
                slot.status = SlotStatus::AVAILABLE;
            } else {
                slot.status = SlotStatus::EMPTY;
            }
            remote_to_global[remote_id] = slot.global_index;
            next_remote_ids.push_back(remote_id);
            unit.slots.push_back(std::move(slot));
        }
        unit.slot_count = static_cast<int>(unit.slots.size());
        next.units.push_back(std::move(unit));
    }

    std::vector<std::string> next_slot_groups(next_remote_ids.size());
    std::vector<Group> next_groups;
    for (const auto& group_json : array_member(snapshot_, "groups")) {
        if (!group_json.is_object()) {
            continue;
        }
        Group group;
        group.name = string_member(group_json, "name");
        group.lane = string_member(group_json, "lane");
        if (group.name.empty()) {
            continue;
        }
        const int tool = tool_from_group(group.name);
        for (const auto& member : array_member(group_json, "slots")) {
            if (!member.is_number_integer()) {
                continue;
            }
            auto global = remote_to_global.find(member.get<int>());
            if (global == remote_to_global.end()) {
                continue;
            }
            group.slots.push_back(global->second);
            next_slot_groups[static_cast<std::size_t>(global->second)] = group.name;
            if (SlotInfo* slot = next.get_slot_global(global->second)) {
                slot->mapped_tool = tool;
            }
        }
        next_groups.push_back(std::move(group));
    }

    reported_action_ = AmsAction::IDLE;
    lane_states_.clear();
    std::set<int> current_slots;
    std::string current_group;
    for (const auto& lane_json : array_member(snapshot_, "lanes")) {
        if (!lane_json.is_object()) {
            continue;
        }
        const std::string state = string_member(lane_json, "state");
        lane_states_.push_back(ams_normalize_state_token(state));
        const AmsAction action = action_from_lane_state(state);
        if (action == AmsAction::ERROR ||
            (reported_action_ != AmsAction::ERROR && action != AmsAction::IDLE)) {
            reported_action_ = action;
        }
        auto global = remote_to_global.find(int_member(lane_json, "current_slot", -1));
        if (global != remote_to_global.end()) {
            current_slots.insert(global->second);
            if (SlotInfo* slot = next.get_slot_global(global->second)) {
                slot->status = SlotStatus::LOADED;
            }
            current_group = string_member(lane_json, "current_group");
        }
    }

    // Independent lanes are never folded into one current slot: with several
    // loaded there is no single answer to give.
    next.filament_loaded = !current_slots.empty();
    next.current_slot = current_slots.size() == 1 ? *current_slots.begin() : -1;
    next.current_tool = current_slots.size() == 1 ? tool_from_group(current_group) : -1;

    remote_slot_ids_ = std::move(next_remote_ids);
    slot_groups_ = std::move(next_slot_groups);
    groups_ = std::move(next_groups);
    commands_ = std::move(next_commands);
    manager_ready_ = bool_member(snapshot_, "ready", false);
    api_supported_ = true;
    topology_ = topologies.size() > 1
                    ? PathTopology::MIXED
                    : (topologies.empty() ? PathTopology::HUB : *topologies.begin());

    // A T<n> group names the slots that can serve tool n. The one the map
    // shows is the slot that would serve it now.
    for (const auto& group : groups_) {
        const int tool = tool_from_group(group.name);
        if (tool < 0 || group.slots.empty()) {
            continue;
        }
        int chosen = group.slots.front();
        for (int slot : group.slots) {
            if (current_slots.count(slot) != 0) {
                chosen = slot;
                break;
            }
        }
        if (current_slots.count(chosen) == 0) {
            for (int slot : group.slots) {
                const SlotInfo* info = next.get_slot_global(slot);
                if (info && info->status == SlotStatus::AVAILABLE) {
                    chosen = slot;
                    break;
                }
            }
        }
        if (tool >= static_cast<int>(next.tool_to_slot_map.size())) {
            next.tool_to_slot_map.resize(static_cast<std::size_t>(tool) + 1, -1);
        }
        next.tool_to_slot_map[static_cast<std::size_t>(tool)] = chosen;
    }

    // OpenAMS reads nothing off a spool, so every insert is one the hardware
    // has no evidence about: the record stays and the user is asked
    // (docs/specs/filament_slots.md §6). Only an observed empty counts, so the
    // first frame after start is a baseline, not a wave of inserts. A unit
    // that is offline, or a manager that is not ready, reports bays it has not
    // read, so those frames neither raise an insert nor move the baseline.
    std::unordered_map<int, bool> present_now;
    for (const auto& unit : next.units) {
        const bool bays_read = manager_ready_ && unit.connected;
        for (const auto& slot : unit.slots) {
            const int slot_id = remote_slot_ids_[static_cast<std::size_t>(slot.global_index)];
            auto before = present_by_slot_id_.find(slot_id);
            if (!bays_read) {
                if (before != present_by_slot_id_.end()) {
                    present_now[slot_id] = before->second;
                }
                continue;
            }
            const bool present = slot.status != SlotStatus::EMPTY;
            if (present && before != present_by_slot_id_.end() && !before->second) {
                const int slot_index = slot.global_index;
                helix::ui::queue_update(
                    [slot_index] { helix::ui::offer_clear_after_unverified_insert(slot_index); });
            }
            present_now[slot_id] = present;
        }
    }
    present_by_slot_id_ = std::move(present_now);

    for (auto& unit : next.units) {
        for (auto& slot : unit.slots) {
            apply_resolved_lane(slot, slot.global_index);
        }
    }

    next.pending_target_slot = pending_slot_;
    next.action = pending_action_ != AmsAction::IDLE ? pending_action_ : reported_action_;
    next.operation_detail = failure_detail_;
    system_info_ = std::move(next);
}

AmsAction AmsBackendOpenAms::action_from_lane_state(const std::string& state) {
    const std::string token = ams_normalize_state_token(state);
    if (token == "loading")
        return AmsAction::LOADING;
    if (token == "unloading")
        return AmsAction::UNLOADING;
    if (token == "calibrating")
        return AmsAction::RESETTING;
    if (token == "error")
        return AmsAction::ERROR;
    if (token == "paused")
        return AmsAction::PAUSED;
    return AmsAction::IDLE;
}

// ============================================================================
// State queries
// ============================================================================

AmsSystemInfo AmsBackendOpenAms::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendOpenAms::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = system_info_.get_slot_global(slot_index);
    return slot ? *slot : SlotInfo{};
}

SlotInfo* AmsBackendOpenAms::cached_slot_locked(int slot_index) {
    return system_info_.get_slot_global(slot_index);
}

void AmsBackendOpenAms::prepare_lane_repaint_locked(int slot_index, SlotInfo& slot) {
    (void)slot_index;
    // The lane is the only supplier. overrides_ is not refreshed by a resync,
    // so restating a field from it would bring back what the lane has dropped.
    helix::ams::clear_lane_only_identity(slot, nullptr);
}

PathTopology AmsBackendOpenAms::get_topology() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return topology_;
}

PathTopology AmsBackendOpenAms::get_unit_topology(int unit_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size())) {
        return system_info_.units[static_cast<std::size_t>(unit_index)].topology;
    }
    return topology_;
}

PathSegment AmsBackendOpenAms::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = system_info_.current_slot >= 0
                               ? system_info_.get_slot_global(system_info_.current_slot)
                               : nullptr;
    if (!slot) {
        return PathSegment::NONE;
    }
    if (slot->status == SlotStatus::LOADED) {
        return PathSegment::NOZZLE;
    }
    return slot->is_present() ? PathSegment::SPOOL : PathSegment::NONE;
}

PathSegment AmsBackendOpenAms::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = system_info_.get_slot_global(slot_index);
    if (!slot) {
        return PathSegment::NONE;
    }
    if (slot->status == SlotStatus::LOADED) {
        return PathSegment::NOZZLE;
    }
    if (slot_index == pending_slot_ && pending_action_ == AmsAction::LOADING) {
        return PathSegment::OUTPUT;
    }
    return slot->is_present() ? PathSegment::SPOOL : PathSegment::NONE;
}

PathSegment AmsBackendOpenAms::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_slot_ >= 0) {
        return pending_action_ == AmsAction::LOADING ? PathSegment::OUTPUT : PathSegment::HUB;
    }
    return system_info_.filament_loaded ? PathSegment::NOZZLE : PathSegment::SPOOL;
}

bool AmsBackendOpenAms::can_unload_from_toolhead(int slot_index) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (command_locked(kUnload).empty()) {
            return false;
        }
    }
    return AmsSubscriptionBackend::can_unload_from_toolhead(slot_index);
}

// ============================================================================
// Filament operations
// ============================================================================

AmsError AmsBackendOpenAms::manager_accepts_locked() const {
    if (!api_supported_) {
        return AmsErrorHelper::not_supported("OpenAMS without its UI API");
    }
    if (!manager_ready_) {
        return AmsErrorHelper::wrong_state("OpenAMS not ready", "ready");
    }
    return AmsErrorHelper::success();
}

std::string AmsBackendOpenAms::command_locked(const char* action) const {
    auto it = commands_.find(action);
    return it == commands_.end() ? std::string() : it->second;
}

bool AmsBackendOpenAms::slot_loadable_locked(int slot_index) const {
    const SlotInfo* slot = system_info_.get_slot_global(slot_index);
    return slot && (slot->status == SlotStatus::AVAILABLE || slot->status == SlotStatus::LOADED);
}

int AmsBackendOpenAms::loaded_lane_count_locked() const {
    return static_cast<int>(std::count(lane_states_.begin(), lane_states_.end(), "loaded"));
}

AmsError AmsBackendOpenAms::load_gcode_locked(int slot_index, std::string& gcode) const {
    if (AmsError accepts = manager_accepts_locked(); !accepts.success()) {
        return accepts;
    }
    const std::string command = command_locked(kLoad);
    if (command.empty()) {
        return AmsErrorHelper::not_supported("OpenAMS load (install OPENAMS_LOAD from "
                                             "oams_macros.cfg)");
    }
    if (AmsError valid = validate_slot_index_locked(slot_index); !valid.success()) {
        return valid;
    }
    const std::string& group = slot_groups_[static_cast<std::size_t>(slot_index)];
    if (group.empty() || !IMoonrakerAPI::is_safe_gcode_param(group)) {
        return AmsErrorHelper::invalid_parameter("OpenAMS slot " + std::to_string(slot_index) +
                                                 " belongs to no usable filament group");
    }
    if (!slot_loadable_locked(slot_index)) {
        return AmsErrorHelper::slot_not_available(lane_noun(), slot_index);
    }
    gcode = command + " GROUP=" + IMoonrakerAPI::gcode_param_value(group) +
            " SLOT=" + std::to_string(remote_slot_ids_[static_cast<std::size_t>(slot_index)]);
    return AmsErrorHelper::success();
}

AmsError
AmsBackendOpenAms::send_operation_gcode(const std::string& gcode, std::function<void()> on_complete,
                                        std::function<void(const MoonrakerError&)> on_error) {
    return ensure_homed_then(gcode, std::move(on_complete), std::move(on_error),
                             IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS,
                             /*skip_homing=*/true, /*silent=*/false,
                             /*caller_surfaces_errors=*/false);
}

AmsError AmsBackendOpenAms::begin_operation(AmsAction action, int slot_index,
                                            const std::string& gcode,
                                            const char* completion_event) {
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_action_ = action;
        pending_slot_ = slot_index;
        failure_detail_.clear();
        generation = ++operation_generation_;
        system_info_.action = action;
        system_info_.pending_target_slot = slot_index;
        system_info_.operation_detail.clear();
    }
    emit_event(EVENT_STATE_CHANGED);

    auto token = lifetime_.token();
    AmsError result = send_operation_gcode(
        gcode,
        [this, token, generation, completion_event]() {
            token.defer("AmsBackendOpenAms::operation_complete",
                        [this, generation, completion_event]() {
                            finish_operation(generation, completion_event);
                        });
        },
        [this, token, generation](const MoonrakerError& error) {
            token.defer("AmsBackendOpenAms::operation_error",
                        [this, generation, error]() { fail_operation(generation, error); });
        });
    if (!result.success()) {
        bool unwound = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == operation_generation_) {
                pending_action_ = AmsAction::IDLE;
                pending_slot_ = -1;
                system_info_.action = reported_action_;
                system_info_.pending_target_slot = -1;
                unwound = true;
            }
        }
        if (unwound) {
            emit_event(EVENT_STATE_CHANGED);
        }
    }
    return result;
}

void AmsBackendOpenAms::finish_operation(std::uint64_t generation, const char* event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != operation_generation_) {
            return;
        }
        pending_action_ = AmsAction::IDLE;
        pending_slot_ = -1;
        system_info_.action = reported_action_;
        system_info_.pending_target_slot = -1;
    }
    emit_event(EVENT_STATE_CHANGED);
    emit_event(event);
}

void AmsBackendOpenAms::fail_operation(std::uint64_t generation, const MoonrakerError& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != operation_generation_) {
            return;
        }
        pending_action_ = AmsAction::IDLE;
        pending_slot_ = -1;
        failure_detail_ = error.message;
        system_info_.action = reported_action_;
        system_info_.pending_target_slot = -1;
        system_info_.operation_detail = failure_detail_;
    }
    // The G-code error stream has already shown the macro's own message.
    spdlog::warn("{} Operation failed: {}", backend_log_tag(), error.message);
    emit_event(EVENT_STATE_CHANGED);
}

AmsError AmsBackendOpenAms::do_load_filament(int slot_index) {
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError ready = load_gcode_locked(slot_index, gcode); !ready.success()) {
            return ready;
        }
    }
    return begin_operation(AmsAction::LOADING, slot_index, gcode, EVENT_LOAD_COMPLETE);
}

AmsError AmsBackendOpenAms::do_unload_filament(int slot_index) {
    (void)slot_index;
    std::string command;
    int current_slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError accepts = manager_accepts_locked(); !accepts.success()) {
            return accepts;
        }
        command = command_locked(kUnload);
        if (command.empty()) {
            return AmsErrorHelper::not_supported("OpenAMS unload (install OPENAMS_UNLOAD from "
                                                 "oams_macros.cfg)");
        }
        // The v1 unload takes no lane, so with two lanes loaded it cannot say
        // which one it empties.
        if (loaded_lane_count_locked() > 1) {
            return AmsErrorHelper::not_supported("OpenAMS unload with several lanes loaded");
        }
        current_slot = system_info_.current_slot;
    }
    return begin_operation(AmsAction::UNLOADING, current_slot, command, EVENT_UNLOAD_COMPLETE);
}

AmsError AmsBackendOpenAms::do_select_slot(int slot_index) {
    (void)slot_index;
    return AmsErrorHelper::not_supported("OpenAMS slot selection without loading");
}

AmsError AmsBackendOpenAms::do_change_tool(int tool_number) {
    std::string gcode;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError accepts = manager_accepts_locked(); !accepts.success()) {
            return accepts;
        }
        const std::string group_name = "T" + std::to_string(tool_number);
        auto group = std::find_if(groups_.begin(), groups_.end(),
                                  [&](const Group& g) { return g.name == group_name; });
        if (tool_number < 0 || group == groups_.end() || group->slots.empty()) {
            return AmsErrorHelper::tool_out_of_range(tool_number);
        }
        // The loaded member first, so a tool change to the active tool stays
        // on its spool; otherwise the first member with a spool ready to feed.
        for (int member : group->slots) {
            const SlotInfo* info = system_info_.get_slot_global(member);
            if (info && info->status == SlotStatus::LOADED) {
                slot = member;
                break;
            }
        }
        if (slot < 0) {
            for (int member : group->slots) {
                if (slot_loadable_locked(member)) {
                    slot = member;
                    break;
                }
            }
        }
        if (slot < 0) {
            return AmsErrorHelper::slot_not_available(lane_noun(), group->slots.front());
        }
        if (AmsError ready = load_gcode_locked(slot, gcode); !ready.success()) {
            return ready;
        }
    }
    return begin_operation(AmsAction::LOADING, slot, gcode, EVENT_LOAD_COMPLETE);
}

// ============================================================================
// Recovery
// ============================================================================

AmsError AmsBackendOpenAms::reset() {
    std::string command;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!api_supported_) {
            return AmsErrorHelper::not_supported("OpenAMS without its UI API");
        }
        command = command_locked(kReset);
    }
    if (command.empty()) {
        return AmsErrorHelper::not_supported("OpenAMS reset");
    }
    return execute_gcode(command);
}

AmsError AmsBackendOpenAms::recover() {
    return reset();
}

AmsError AmsBackendOpenAms::cancel() {
    std::string command;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!api_supported_) {
            return AmsErrorHelper::not_supported("OpenAMS without its UI API");
        }
        command = command_locked(kCancel);
        if (command.empty()) {
            return AmsErrorHelper::not_supported("OpenAMS cancel");
        }
        if (pending_action_ != AmsAction::IDLE) {
            return AmsErrorHelper::busy("an OpenAMS operation started from this screen");
        }
        if (std::find(lane_states_.begin(), lane_states_.end(), "loading") == lane_states_.end()) {
            return AmsErrorHelper::wrong_state("no OpenAMS load running", "loading");
        }
    }
    return execute_gcode(command);
}

bool AmsBackendOpenAms::can_cancel_operation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !command_locked(kCancel).empty() && pending_action_ == AmsAction::IDLE;
}

AmsError AmsBackendOpenAms::clear_fault(int slot_index) {
    (void)slot_index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_detail_.clear();
        system_info_.operation_detail.clear();
    }
    emit_event(EVENT_STATE_CHANGED);
    return AmsErrorHelper::success();
}

// ============================================================================
// Slot identity
// ============================================================================

AmsError AmsBackendOpenAms::apply_user_edit(int slot_index, const SlotInfo& info,
                                            const helix::ams::Observation& declared) {
    helix::ams::FilamentSlotOverride to_save;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotInfo* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                system_info_.total_slots - 1);
        }
        write_filament_fields(*slot, info);
        helix::ams::stage_user_override(overrides_, slot_index, info, declared);
        auto it = overrides_.find(slot_index);
        if (it != overrides_.end()) {
            to_save = it->second;
        }
    }
    if (override_store_) {
        // The tag is captured by value: the save can complete after this
        // backend is gone.
        const std::string tag = backend_log_tag();
        override_store_->save_async(
            slot_index, to_save, [tag, slot_index](bool ok, const std::string& error) {
                if (!ok) {
                    spdlog::warn("{} Failed to persist slot {}: {}", tag, slot_index, error);
                }
            });
    }
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendOpenAms::sync_external_identity(int slot_index, const SlotInfo& info) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotInfo* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                system_info_.total_slots - 1);
        }
        write_filament_fields(*slot, info);
    }
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendOpenAms::persist_slot_weight(int slot_index, float remaining_weight_g,
                                            float total_weight_g) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_weight(override_store_.get(), overrides_, slot_index,
                                        remaining_weight_g, total_weight_g, tag);
}

void AmsBackendOpenAms::persist_external_identity_impl(int slot_index,
                                                       const helix::ams::Observation& spoolman) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_external_identity(override_store_.get(), overrides_, slot_index,
                                                   spoolman, tag);
}

void AmsBackendOpenAms::clear_slot_override(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_.erase(slot_index);
        helix::ams::reset_lane_to_machine_readings(lane_id(slot_index));

        // OpenAMS states no identity of its own, so nothing would restate
        // these fields; a clear that left them would show the old spool until
        // the next frame.
        if (SlotInfo* slot = system_info_.get_slot_global(slot_index)) {
            slot->material.clear();
            slot->color_rgb = AMS_DEFAULT_SLOT_COLOR;
            slot->color_name.clear();
            slot->multi_color_hexes.clear();
            slot->brand.clear();
            slot->catalog_id.clear();
            slot->product_name.clear();
            slot->spool_name.clear();
            slot->clear_spoolman_link();
            slot->remaining_weight_g = -1.0f;
            slot->total_weight_g = -1.0f;
        }
    }
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    if (override_store_) {
        // The record may have been written by another lane_data author, so
        // the delete goes out whether or not this session loaded one.
        const std::string tag = backend_log_tag();
        override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} Override clear failed for slot {}: {}", tag, slot_index, err);
            }
        });
    }
}

// ============================================================================
// Tool mapping and bypass
// ============================================================================

AmsError AmsBackendOpenAms::set_tool_mapping_impl(int tool_number, int slot_index) {
    (void)tool_number;
    (void)slot_index;
    return AmsErrorHelper::not_supported("OpenAMS tool mapping");
}

std::vector<int> AmsBackendOpenAms::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

AmsError AmsBackendOpenAms::enable_bypass() {
    return AmsErrorHelper::not_supported("OpenAMS bypass");
}

AmsError AmsBackendOpenAms::disable_bypass() {
    return AmsErrorHelper::not_supported("OpenAMS bypass");
}

} // namespace helix
