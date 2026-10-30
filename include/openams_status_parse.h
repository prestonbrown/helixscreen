// include/openams_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hv/json.hpp"

/**
 * @file openams_status_parse.h
 * @brief The oams_manager status object, read into a plain struct.
 *
 * Status updates carry only the fields that changed, so the backend merges
 * them over its last full view and parses the whole merged object each time;
 * the result describes the manager as a whole, never a delta. Slots are
 * flattened into global indices in unit order. A parse makes no slot
 * decisions (tool mapping, endless spool, insert prompts);
 * AmsBackendOpenAms applies the result under its lock. Pure: no I/O, no
 * locks, no logging, no AmsState.
 */
namespace helix::openams {

/// The UI API command names oams_manager advertises under `commands`; the backend
/// dispatches by the same names, so a rename here reaches both.
inline constexpr const char* kCommandLoad = "load";
inline constexpr const char* kCommandUnload = "unload";
inline constexpr const char* kCommandCancel = "cancel";
inline constexpr const char* kCommandReset = "reset";

/// OAMS_DRYER_START takes its duration in whole seconds, 1 s to 7 days.
constexpr int kMaxDryerSeconds = 604800;

/// One entry of a unit's published fault list.
struct UnitFault {
    std::string severity; ///< "stop" or "pause"; other severities are not kept
    std::string code;
    std::string text;
    int bay = -1;           ///< unit-local bay, -1 for the whole unit
    bool clearable = false; ///< the unit advertises clear_fault for it
};

/// One `groups[]` entry, with its members as global slot indices.
struct Group {
    std::string name;
    std::string lane;
    std::vector<int> slots;
};

/// What a unit publishes about its dryer.
struct UnitDryer {
    bool offered = false;           ///< capability plus dryer_start or dryer_stop
    bool can_start = false;         ///< dryer_start is advertised now
    bool can_stop = false;          ///< dryer_stop is advertised now
    bool requires_unloaded = false; ///< no bay may be loaded while it runs
    std::optional<EnvironmentData> environment;
    /// Everything but duration_min, which needs the length of the cycle this
    /// screen started: the unit reports only the time left.
    DryerInfo info;
};

/// One `lanes[]` entry.
struct Lane {
    std::string id;
    std::string state;    ///< normalized token
    int loaded_slot = -1; ///< global slot index on the lane, -1 for none
};

/// The whole oams_manager object.
struct ManagerStatus {
    /// The object publishes a UI API this build reads. When false, nothing
    /// below is filled in.
    bool supported = false;
    bool ready = false;
    /// Advertised command per action ("load", "unload", "cancel", "reset"),
    /// only for names that are one G-code word.
    std::unordered_map<std::string, std::string> commands;
    /// (action, name) for each advertised command refused as unsafe.
    std::vector<std::pair<std::string, std::string>> unsafe_commands;
    /// (unit name, topology) for each unit drawn as a hub because its
    /// topology is unfamiliar.
    std::vector<std::pair<std::string, std::string>> unknown_topologies;

    /// Units with their slots, slot status, extruder, environment and buffer.
    std::vector<AmsUnit> units;
    int total_slots = 0;
    /// Per global slot: its manager slot id, its unit's lane, its group name.
    std::vector<int> remote_slot_ids;
    std::vector<std::string> slot_lanes;
    std::vector<std::string> slot_groups;
    /// Per unit: `units[].id` as a number, -1 when it is not one.
    std::vector<int> unit_oams_idx;
    /// Per unit, parallel to units.
    std::vector<std::vector<UnitFault>> faults;
    std::vector<UnitDryer> dryers;

    std::vector<Group> groups;
    std::vector<Lane> lanes;
    /// What the lanes and the unit faults say the manager is doing: an error
    /// anywhere wins over any other activity.
    AmsAction reported_action = AmsAction::IDLE;
    PathTopology topology = PathTopology::HUB;

    /// Independent lanes are never folded into one current slot: with several
    /// loaded there is no single answer, so current_slot and current_tool
    /// read -1.
    bool filament_loaded = false;
    int current_slot = -1;
    int current_tool = -1;
};

/// Read a full oams_manager object.
[[nodiscard]] ManagerStatus parse_manager_status(const nlohmann::json& manager);

/// Tool number a `T<n>` group stands for, or -1 for any other group name.
[[nodiscard]] int tool_from_group(const std::string& group);

/// The action a lane state reports; IDLE for any state that is not one.
[[nodiscard]] AmsAction action_from_lane_state(const std::string& state);

} // namespace helix::openams
