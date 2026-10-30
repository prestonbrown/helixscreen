// src/printer/openams_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "openams_status_parse.h"

#include "openams_api.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace helix::openams {
namespace {

using json = nlohmann::json;

constexpr const char* kCommandActions[] = {kCommandLoad, kCommandUnload, kCommandCancel,
                                           kCommandReset};

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

std::optional<double> number_member(const json& object, const char* key) {
    auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return std::nullopt;
    }
    return it->get<double>();
}

bool lists_string(const json& array, const char* value) {
    return std::any_of(array.begin(), array.end(), [value](const json& entry) {
        return entry.is_string() && entry.get<std::string>() == value;
    });
}

/// The dryer states in which a cycle is under way. Anything the unit does not
/// name as idle or failed counts as running, so a state a newer firmware adds
/// still shows the cycle and offers Stop.
bool dryer_state_is_running(const std::string& state) {
    const std::string token = ams_normalize_state_token(state);
    return !token.empty() && token != "off" && token != "idle" && token != "fault" &&
           token != "none";
}

/// A command name is sent verbatim, so it must be one G-code word.
bool command_name_is_safe(const std::string& command) {
    return !command.empty() && std::all_of(command.begin(), command.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_';
    });
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

SlotInfo* slot_at(std::vector<AmsUnit>& units, int global_index) {
    for (auto& unit : units) {
        for (auto& slot : unit.slots) {
            if (slot.global_index == global_index) {
                return &slot;
            }
        }
    }
    return nullptr;
}

std::vector<UnitFault> parse_faults(const json& device) {
    std::vector<UnitFault> faults;
    for (const auto& fault_json : array_member(device, "faults")) {
        if (!fault_json.is_object()) {
            continue;
        }
        UnitFault fault;
        fault.severity = string_member(fault_json, "severity");
        fault.code = string_member(fault_json, "code");
        fault.text = string_member(fault_json, "text");
        fault.bay = int_member(fault_json, "bay", -1);
        fault.clearable = lists_string(array_member(fault_json, "actions"), "clear_fault");
        if (fault.severity == "stop" || fault.severity == "pause") {
            faults.push_back(std::move(fault));
        }
    }
    return faults;
}

UnitDryer parse_dryer(const json& device) {
    UnitDryer dryer;
    if (const json* env = object_member(device, "environment")) {
        const auto temp = number_member(*env, "temp_c");
        const auto humidity = number_member(*env, "rh_pct");
        if (temp || humidity) {
            EnvironmentData reading;
            reading.temperature_c = temp ? static_cast<float>(*temp) : 0.0f;
            reading.humidity_pct = humidity ? static_cast<float>(*humidity) : 0.0f;
            reading.has_humidity = humidity.has_value();
            dryer.environment = reading;
        }
    }

    const json* capabilities = object_member(device, "capabilities");
    // A running cycle withdraws dryer_start and keeps dryer_stop, so each
    // action is permitted on its own and the dryer stays offered while either
    // is advertised.
    const json& actions = array_member(device, "supported_actions");
    dryer.can_start = lists_string(actions, "dryer_start");
    dryer.can_stop = lists_string(actions, "dryer_stop");
    dryer.offered = capabilities && bool_member(*capabilities, "dryer", false) &&
                    (dryer.can_start || dryer.can_stop);
    if (!dryer.offered) {
        return dryer;
    }
    dryer.requires_unloaded = bool_member(*capabilities, "dryer_requires_unloaded", false);
    DryerInfo& info = dryer.info;
    info.supported = true;
    info.supports_fan_control = false;
    info.min_temp_c = static_cast<float>(
        number_member(*capabilities, "dryer_target_min_c").value_or(info.min_temp_c));
    info.max_temp_c = static_cast<float>(
        number_member(*capabilities, "dryer_target_max_c").value_or(info.max_temp_c));
    info.max_duration_min = kMaxDryerSeconds / 60;
    if (const json* state = object_member(device, "dryer")) {
        info.active = dryer_state_is_running(string_member(*state, "state"));
        info.target_temp_c =
            info.active ? static_cast<float>(number_member(*state, "target_c").value_or(0.0))
                        : 0.0f;
        info.remaining_min =
            info.active ? static_cast<int>(
                              (number_member(*state, "remaining_s").value_or(0.0) + 59.0) / 60.0)
                        : 0;
        info.fan_pct = static_cast<int>(number_member(*state, "fan_pct").value_or(0.0));
    }
    // The chamber probe is the reading that matters for a cycle; fall back to
    // the unit's environment sensor when the dryer telemetry has none.
    if (dryer.environment) {
        info.current_temp_c = dryer.environment->temperature_c;
    }
    if (const json* telemetry = object_member(device, "telemetry")) {
        if (const json* td = object_member(*telemetry, "dryer")) {
            if (const auto chamber = number_member(*td, "chamber_c")) {
                info.current_temp_c = static_cast<float>(*chamber);
            }
        }
    }
    return dryer;
}

} // namespace

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

AmsAction action_from_lane_state(const std::string& state) {
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

ManagerStatus parse_manager_status(const json& manager) {
    ManagerStatus out;
    if (!api_supported(manager)) {
        return out;
    }
    out.supported = true;
    out.ready = bool_member(manager, "ready", false);

    if (const json* commands = object_member(manager, "commands")) {
        for (const char* action : kCommandActions) {
            const std::string command = string_member(*commands, action);
            if (command.empty()) {
                continue;
            }
            if (!command_name_is_safe(command)) {
                out.unsafe_commands.emplace_back(action, command);
                continue;
            }
            out.commands[action] = command;
        }
    }

    std::unordered_map<int, int> remote_to_global;
    std::set<PathTopology> topologies;
    std::vector<std::string> unit_lanes;

    for (const auto& unit_json : array_member(manager, "units")) {
        if (!unit_json.is_object()) {
            continue;
        }
        AmsUnit unit;
        unit.unit_index = static_cast<int>(out.units.size());
        unit.name = string_member(unit_json, "id", std::to_string(unit.unit_index));
        unit.display_name = string_member(unit_json, "name", unit.name);
        unit.first_slot_global_index = out.total_slots;
        unit.connected = bool_member(unit_json, "connected", true);
        unit.topology = PathTopology::HUB;
        // The oams_manager contract publishes no toolhead filament sensor.
        unit.has_toolhead_sensor = false;
        const std::string topology = string_member(unit_json, "topology");
        if (!topology.empty()) {
            if (auto parsed = topology_from_token(topology)) {
                unit.topology = *parsed;
            } else {
                // Topology only shapes the drawing; slots are addressed by id,
                // so an unfamiliar shape still loads and unloads correctly.
                out.unknown_topologies.emplace_back(unit.name, topology);
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
            slot.global_index = out.total_slots++;
            if (bool_member(slot_json, "loaded", false)) {
                slot.status = SlotStatus::LOADED;
            } else if (bool_member(slot_json, "ready", false)) {
                slot.status = SlotStatus::AVAILABLE;
            } else {
                slot.status = SlotStatus::EMPTY;
            }
            remote_to_global[remote_id] = slot.global_index;
            out.remote_slot_ids.push_back(remote_id);
            unit.slots.push_back(std::move(slot));
        }
        unit.slot_count = static_cast<int>(unit.slots.size());
        unit.hub_id = string_member(unit_json, "lane");
        {
            const std::string id = string_member(unit_json, "id");
            char* end = nullptr;
            const long idx = id.empty() ? -1 : std::strtol(id.c_str(), &end, 10);
            out.unit_oams_idx.push_back(end && *end == '\0' ? static_cast<int>(idx) : -1);
        }
        unit_lanes.push_back(unit.hub_id);
        out.units.push_back(std::move(unit));
    }

    for (std::size_t u = 0; u < out.units.size(); ++u) {
        out.slot_lanes.insert(out.slot_lanes.end(), out.units[u].slots.size(), unit_lanes[u]);
    }

    // The extruder a lane feeds, when the openams plugin publishes it. It names the
    // toolhead of every slot on that lane.
    if (const json* by_fps = object_member(manager, "lanes_by_fps")) {
        for (std::size_t u = 0; u < out.units.size(); ++u) {
            const json* lane = object_member(*by_fps, unit_lanes[u].c_str());
            const std::string extruder = lane ? string_member(*lane, "extruder") : std::string();
            for (auto& slot : out.units[u].slots) {
                slot.extruder_name = extruder;
            }
        }
    }

    out.slot_groups.resize(out.remote_slot_ids.size());
    for (const auto& group_json : array_member(manager, "groups")) {
        if (!group_json.is_object()) {
            continue;
        }
        Group group;
        group.name = string_member(group_json, "name");
        group.lane = string_member(group_json, "lane");
        if (group.name.empty()) {
            continue;
        }
        for (const auto& member : array_member(group_json, "slots")) {
            if (!member.is_number_integer()) {
                continue;
            }
            auto global = remote_to_global.find(member.get<int>());
            if (global == remote_to_global.end()) {
                continue;
            }
            group.slots.push_back(global->second);
            out.slot_groups[static_cast<std::size_t>(global->second)] = group.name;
        }
        out.groups.push_back(std::move(group));
    }

    std::set<int> current_slots;
    std::string current_group;
    std::unordered_map<std::string, BufferHealth> lane_fps;
    for (const auto& lane_json : array_member(manager, "lanes")) {
        if (!lane_json.is_object()) {
            continue;
        }
        Lane lane;
        lane.id = string_member(lane_json, "id");
        auto global = remote_to_global.find(int_member(lane_json, "current_slot", -1));
        lane.loaded_slot = global != remote_to_global.end() ? global->second : -1;
        // The lane's filament pressure sensor, 0 (no pressure) to 1 (fully
        // compressed). It has one spring: it measures compression and never
        // tension, so a reading under set_point is less compression, not slack
        // or pull. A manager that publishes no pressure leaves the lane with no
        // buffer rather than a made-up reading.
        auto pressure = lane_json.find("pressure");
        if (pressure != lane_json.end() && pressure->is_number()) {
            BufferHealth fps;
            fps.fps_value = fps.smoothed_fps = pressure->get<float>();
            fps.fps_reported = true;
            fps.compression_only = true;
            fps.filament_loaded = lane.loaded_slot >= 0;
            auto set_point = lane_json.find("set_point");
            if (set_point != lane_json.end() && set_point->is_number()) {
                fps.fps_set_point = set_point->get<float>();
            }
            lane_fps[lane.id] = fps;
        }
        const std::string state = string_member(lane_json, "state");
        lane.state = ams_normalize_state_token(state);
        const AmsAction action = action_from_lane_state(state);
        if (action == AmsAction::ERROR ||
            (out.reported_action != AmsAction::ERROR && action != AmsAction::IDLE)) {
            out.reported_action = action;
        }
        if (lane.loaded_slot >= 0) {
            current_slots.insert(lane.loaded_slot);
            if (SlotInfo* slot = slot_at(out.units, lane.loaded_slot)) {
                slot->status = SlotStatus::LOADED;
            }
            current_group = string_member(lane_json, "current_group");
        }
        out.lanes.push_back(std::move(lane));
    }

    // Faults, environment and dryer, per unit (openams only; klipper_openams
    // publishes no `devices`, so none of them is ever reported there).
    out.faults.resize(out.units.size());
    out.dryers.resize(out.units.size());
    if (const json* devices = object_member(manager, "devices")) {
        for (std::size_t u = 0; u < out.units.size(); ++u) {
            const json* device = object_member(*devices, out.units[u].display_name.c_str());
            if (!device) {
                continue;
            }
            out.faults[u] = parse_faults(*device);
            out.dryers[u] = parse_dryer(*device);
            out.units[u].environment = out.dryers[u].environment;
            if (!out.faults[u].empty()) {
                out.reported_action = AmsAction::ERROR;
            }
        }
    }

    for (std::size_t u = 0; u < out.units.size(); ++u) {
        auto fps = lane_fps.find(unit_lanes[u]);
        if (fps != lane_fps.end()) {
            out.units[u].buffer_health = fps->second;
        }
    }

    out.filament_loaded = !current_slots.empty();
    out.current_slot = current_slots.size() == 1 ? *current_slots.begin() : -1;
    out.current_tool = current_slots.size() == 1 ? tool_from_group(current_group) : -1;
    out.topology = topologies.size() > 1
                       ? PathTopology::MIXED
                       : (topologies.empty() ? PathTopology::HUB : *topologies.begin());
    return out;
}

} // namespace helix::openams
