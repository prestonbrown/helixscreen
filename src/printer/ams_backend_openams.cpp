// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_openams.h"

#include "i_moonraker_api.h"
#include "lane_legacy_migration.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

#include "hv/json.hpp"

namespace helix {
namespace {

using json = nlohmann::json;

const json* object_member(const json& object, const char* key) {
    auto it = object.find(key);
    return it != object.end() && it->is_object() ? &(*it) : nullptr;
}

const json* array_member(const json& object, const char* key) {
    auto it = object.find(key);
    return it != object.end() && it->is_array() ? &(*it) : nullptr;
}

std::string string_member(const json& object, const char* key, std::string fallback = {}) {
    auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}

bool bool_member(const json& object, const char* key, bool fallback = false) {
    auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

int int_member(const json& object, const char* key, int fallback = -1) {
    auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

bool command_name_is_safe(const std::string& command) {
    if (command.empty()) {
        return false;
    }
    return std::all_of(command.begin(), command.end(),
                       [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; });
}

int tool_from_group(const std::string& group) {
    if (group.size() < 2 || group.front() != 'T') {
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

AmsBackendOpenAms::AmsBackendOpenAms(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    system_info_.type = AmsType::OPENAMS;
    system_info_.type_name = "OpenAMS";
    system_info_.version = "unknown";
    system_info_.supports_bypass = false;
}

void AmsBackendOpenAms::on_started() {
    auto loaded =
        helix::ams::make_loaded_override_store(api_, "openams", get_type(), backend_log_tag());
    if (loaded.store) {
        helix::ams::ingest_legacy_records(*loaded.store, helix::ams::LegacyLockKeys::LaneData,
                                          backend_index());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    override_store_ = std::move(loaded.store);
    overrides_ = std::move(loaded.overrides);
}

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
    if (system_info_.current_slot < 0) {
        return PathSegment::NONE;
    }
    const SlotInfo* slot = system_info_.get_slot_global(system_info_.current_slot);
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
    if (slot_index == system_info_.current_slot && pending_action_ == AmsAction::UNLOADING) {
        return PathSegment::HUB;
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

PathTopology AmsBackendOpenAms::topology_for_kind(const std::string& kind) {
    return ams_normalize_state_token(kind) == "follower" ? PathTopology::LINEAR : PathTopology::HUB;
}

std::optional<PathTopology> AmsBackendOpenAms::topology_from_token(const std::string& topology) {
    const std::string token = ams_normalize_state_token(topology);
    if (token == "linear")
        return PathTopology::LINEAR;
    if (token == "hub")
        return PathTopology::HUB;
    if (token == "parallel")
        return PathTopology::PARALLEL;
    if (token == "mixed")
        return PathTopology::MIXED;
    return std::nullopt;
}

void AmsBackendOpenAms::handle_status_update(const json& notification) {
    const json* objects = &notification;
    auto params = notification.find("params");
    if (params != notification.end() && params->is_array() && !params->empty() &&
        (*params)[0].is_object()) {
        objects = &(*params)[0];
    }
    const json* update = object_member(*objects, "oams_manager");
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

bool AmsBackendOpenAms::parse_snapshot_locked() {
    const int version = int_member(snapshot_, "api_version", -1);
    const std::string schema = string_member(snapshot_, "schema");
    if (version != SUPPORTED_API_VERSION || schema != "openams.manager") {
        schema_supported_ = false;
        system_info_.units.clear();
        system_info_.total_slots = 0;
        system_info_.current_slot = -1;
        system_info_.current_tool = -1;
        system_info_.filament_loaded = false;
        system_info_.action = AmsAction::ERROR;
        system_info_.operation_detail =
            version < 0 ? "OpenAMS UI API is missing"
                        : "Unsupported OpenAMS UI API version " + std::to_string(version);
        spdlog::error("{} {}", backend_log_tag(), system_info_.operation_detail);
        return false;
    }

    const json* units_json = array_member(snapshot_, "units");
    const json* lanes_json = array_member(snapshot_, "lanes");
    const json* groups_json = array_member(snapshot_, "groups");
    const json* commands_json = object_member(snapshot_, "commands");
    if (!units_json || !lanes_json || !groups_json || !commands_json) {
        return false;
    }

    std::unordered_map<std::string, std::string> next_commands;
    for (const char* name : {"load", "unload", "cancel", "reset"}) {
        const std::string command = string_member(*commands_json, name);
        if (!command_name_is_safe(command)) {
            system_info_.action = AmsAction::ERROR;
            system_info_.operation_detail = std::string("Invalid OpenAMS command: ") + name;
            schema_supported_ = false;
            return false;
        }
        next_commands[name] = command;
    }

    AmsSystemInfo next;
    next.type = AmsType::OPENAMS;
    next.type_name = "OpenAMS";
    next.version = std::to_string(version);
    next.supports_bypass = false;

    std::vector<int> next_remote_ids;
    std::vector<std::string> next_groups;
    std::vector<std::string> next_lanes;
    std::unordered_map<int, int> remote_to_global;
    std::set<PathTopology> topologies;

    for (const auto& unit_json : *units_json) {
        if (!unit_json.is_object()) {
            continue;
        }
        const json* slots_json = array_member(unit_json, "slots");
        if (!slots_json) {
            continue;
        }

        AmsUnit unit;
        unit.unit_index = static_cast<int>(next.units.size());
        unit.name = string_member(unit_json, "id", std::to_string(unit.unit_index));
        unit.display_name = string_member(unit_json, "name", unit.name);
        unit.first_slot_global_index = next.total_slots;
        unit.connected = bool_member(unit_json, "connected");
        const std::string kind = string_member(unit_json, "kind", "oams");
        const std::string topology = string_member(unit_json, "topology");
        const std::string lane = string_member(unit_json, "lane");
        if (topology.empty()) {
            // Transitional v1 producers derived topology from kind. Retain
            // that fallback, but all current producers publish topology so a
            // new family never requires a HelixScreen family-name branch.
            unit.topology = topology_for_kind(kind);
        } else {
            const auto parsed = topology_from_token(topology);
            if (!parsed.has_value()) {
                schema_supported_ = false;
                system_info_.units.clear();
                system_info_.total_slots = 0;
                system_info_.current_slot = -1;
                system_info_.current_tool = -1;
                system_info_.filament_loaded = false;
                system_info_.action = AmsAction::ERROR;
                system_info_.operation_detail = "Unsupported OpenAMS topology " + topology;
                return false;
            }
            unit.topology = *parsed;
        }
        topologies.insert(unit.topology);

        for (const auto& slot_json : *slots_json) {
            if (!slot_json.is_object()) {
                continue;
            }
            const int remote_id = int_member(slot_json, "id", -1);
            if (remote_id < 0 || remote_to_global.find(remote_id) != remote_to_global.end()) {
                continue;
            }
            SlotInfo slot;
            slot.slot_index = int_member(slot_json, "bay", static_cast<int>(unit.slots.size()));
            slot.global_index = next.total_slots++;
            const bool loaded = bool_member(slot_json, "loaded");
            const bool ready = bool_member(slot_json, "ready");
            slot.status =
                loaded ? SlotStatus::LOADED : (ready ? SlotStatus::AVAILABLE : SlotStatus::EMPTY);
            remote_to_global[remote_id] = slot.global_index;
            next_remote_ids.push_back(remote_id);
            next_groups.emplace_back();
            next_lanes.push_back(lane);
            apply_resolved_lane(slot, slot.global_index);
            unit.slots.push_back(std::move(slot));
        }
        unit.slot_count = static_cast<int>(unit.slots.size());
        next.units.push_back(std::move(unit));
    }

    next.tool_to_slot_map.clear();
    for (const auto& group_json : *groups_json) {
        if (!group_json.is_object()) {
            continue;
        }
        const std::string name = string_member(group_json, "name");
        const json* slots = array_member(group_json, "slots");
        if (name.empty() || !slots) {
            continue;
        }
        const int tool = tool_from_group(name);
        int first_global = -1;
        for (const auto& remote_value : *slots) {
            if (!remote_value.is_number_integer()) {
                continue;
            }
            auto global = remote_to_global.find(remote_value.get<int>());
            if (global == remote_to_global.end()) {
                continue;
            }
            const int index = global->second;
            if (index >= 0 && index < static_cast<int>(next_groups.size())) {
                next_groups[static_cast<std::size_t>(index)] = name;
                if (auto* slot = next.get_slot_global(index)) {
                    slot->mapped_tool = tool;
                }
                if (first_global < 0) {
                    first_global = index;
                }
            }
        }
        if (tool >= 0 && first_global >= 0) {
            if (tool >= static_cast<int>(next.tool_to_slot_map.size())) {
                next.tool_to_slot_map.resize(static_cast<std::size_t>(tool + 1), -1);
            }
            next.tool_to_slot_map[static_cast<std::size_t>(tool)] = first_global;
        }
    }

    reported_action_ = AmsAction::IDLE;
    std::set<int> current_slots;
    std::string current_group;
    for (const auto& lane_json : *lanes_json) {
        if (!lane_json.is_object()) {
            continue;
        }
        const AmsAction action = action_from_lane_state(string_member(lane_json, "state"));
        if (action == AmsAction::ERROR ||
            (reported_action_ != AmsAction::ERROR && action != AmsAction::IDLE)) {
            reported_action_ = action;
        }
        const int remote_slot = int_member(lane_json, "current_slot", -1);
        auto global = remote_to_global.find(remote_slot);
        if (global != remote_to_global.end()) {
            current_slots.insert(global->second);
            if (auto* slot = next.get_slot_global(global->second)) {
                slot->status = SlotStatus::LOADED;
            }
        }
        const std::string group = string_member(lane_json, "current_group");
        if (!group.empty()) {
            current_group = group;
        }
    }

    next.filament_loaded = !current_slots.empty();
    next.current_slot = current_slots.size() == 1 ? *current_slots.begin() : -1;
    next.current_tool = current_slots.size() == 1 ? tool_from_group(current_group) : -1;
    next.pending_target_slot = pending_slot_;
    next.action = pending_action_ == AmsAction::IDLE ? reported_action_ : pending_action_;
    next.operation_detail = pending_action_ == AmsAction::IDLE ? std::string() : "OpenAMS macro";

    topology_ = topologies.size() > 1
                    ? PathTopology::MIXED
                    : (topologies.empty() ? PathTopology::HUB : *topologies.begin());
    manager_ready_ = bool_member(snapshot_, "ready");
    schema_supported_ = true;
    commands_ = std::move(next_commands);
    remote_slot_ids_ = std::move(next_remote_ids);
    slot_groups_ = std::move(next_groups);
    slot_lanes_ = std::move(next_lanes);
    system_info_ = std::move(next);
    return true;
}

bool AmsBackendOpenAms::valid_slot_locked(int slot_index) const {
    return schema_supported_ && manager_ready_ && slot_index >= 0 &&
           slot_index < static_cast<int>(remote_slot_ids_.size()) &&
           system_info_.get_slot_global(slot_index) != nullptr;
}

int AmsBackendOpenAms::slot_for_tool_locked(int tool_number) const {
    if (tool_number >= 0 && tool_number < static_cast<int>(system_info_.tool_to_slot_map.size())) {
        return system_info_.tool_to_slot_map[static_cast<std::size_t>(tool_number)];
    }
    return -1;
}

AmsError
AmsBackendOpenAms::send_operation_gcode(const std::string& gcode, std::function<void()> on_complete,
                                        std::function<void(const MoonrakerError&)> on_error) {
    // The advertised OpenAMS macro owns its complete homing/toolchange
    // sequence, so dispatch it unchanged while preserving the base class's
    // explicit RPC-error ownership and lifetime-safe callback path.
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
        generation = ++operation_generation_;
        system_info_.action = action;
        system_info_.pending_target_slot = slot_index;
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
        bool reset = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == operation_generation_) {
                pending_action_ = AmsAction::IDLE;
                pending_slot_ = -1;
                system_info_.action = reported_action_;
                system_info_.pending_target_slot = -1;
                reset = true;
            }
        }
        if (reset) {
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
        system_info_.action = reported_action_;
        system_info_.pending_target_slot = -1;
        system_info_.operation_detail = error.message;
    }
    spdlog::error("{} operation failed: {}", backend_log_tag(), error.message);
    emit_event(EVENT_STATE_CHANGED);
}

AmsError AmsBackendOpenAms::do_load_filament(int slot_index) {
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_slot_locked(slot_index)) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                static_cast<int>(remote_slot_ids_.size()) - 1);
        }
        const std::string& group = slot_groups_[static_cast<std::size_t>(slot_index)];
        if (group.empty() || !IMoonrakerAPI::is_safe_gcode_param(group)) {
            return AmsErrorHelper::invalid_parameter("OpenAMS slot has no safe filament group");
        }
        const std::string& command = commands_.at("load");
        gcode = command + " GROUP=" + IMoonrakerAPI::gcode_param_value(group) +
                " SLOT=" + std::to_string(remote_slot_ids_[static_cast<std::size_t>(slot_index)]);
    }
    return begin_operation(AmsAction::LOADING, slot_index, gcode, EVENT_LOAD_COMPLETE);
}

AmsError AmsBackendOpenAms::do_unload_filament(int slot_index) {
    (void)slot_index;
    std::string command;
    int current_slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!schema_supported_ || !manager_ready_) {
            return AmsErrorHelper::wrong_state("OpenAMS not ready", "ready");
        }
        command = commands_.at("unload");
        current_slot = system_info_.current_slot;
    }
    return begin_operation(AmsAction::UNLOADING, current_slot, command, EVENT_UNLOAD_COMPLETE);
}

AmsError AmsBackendOpenAms::do_select_slot(int slot_index) {
    (void)slot_index;
    return AmsErrorHelper::not_supported("OpenAMS slot selection without loading");
}

AmsError AmsBackendOpenAms::do_change_tool(int tool_number) {
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot = slot_for_tool_locked(tool_number);
    }
    if (slot < 0) {
        return AmsErrorHelper::invalid_parameter("No OpenAMS group is mapped to tool " +
                                                 std::to_string(tool_number));
    }
    return do_load_filament(slot);
}

AmsError AmsBackendOpenAms::dispatch_simple_command(const std::string& command,
                                                    const char* feature) {
    if (command.empty()) {
        return AmsErrorHelper::not_supported(feature);
    }
    return execute_gcode(command);
}

AmsError AmsBackendOpenAms::reset() {
    std::string command;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = commands_.find("reset");
        command = it == commands_.end() ? std::string() : it->second;
    }
    return dispatch_simple_command(command, "OpenAMS reset");
}

AmsError AmsBackendOpenAms::recover() {
    return reset();
}

AmsError AmsBackendOpenAms::cancel() {
    std::string command;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++operation_generation_;
        pending_action_ = AmsAction::IDLE;
        pending_slot_ = -1;
        system_info_.action = reported_action_;
        system_info_.pending_target_slot = -1;
        auto it = commands_.find("cancel");
        command = it == commands_.end() ? std::string() : it->second;
    }
    emit_event(EVENT_STATE_CHANGED);
    return dispatch_simple_command(command, "OpenAMS cancel");
}

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

AmsError AmsBackendOpenAms::set_tool_mapping_impl(int tool_number, int slot_index) {
    (void)tool_number;
    (void)slot_index;
    return AmsErrorHelper::not_supported("OpenAMS tool mapping");
}

AmsError AmsBackendOpenAms::enable_bypass() {
    return AmsErrorHelper::not_supported("OpenAMS bypass");
}

AmsError AmsBackendOpenAms::disable_bypass() {
    return AmsErrorHelper::not_supported("OpenAMS bypass");
}

} // namespace helix
