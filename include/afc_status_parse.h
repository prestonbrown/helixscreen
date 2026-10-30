// include/afc_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_translation.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

/**
 * @file afc_status_parse.h
 * @brief AFC's Moonraker status objects and lane_data records, read into plain
 *        structs.
 *
 * Moonraker sends deltas, so a field a frame may omit is a std::optional and an
 * omitted one reads as nullopt. Several AFC keys also go JSON null on purpose
 * (an eject, a buffer with nothing loaded); those are Clearable, which keeps
 * "said nothing" apart from "said it is gone". A parse decides nothing about
 * slots, tools or actions; AmsBackendAfc applies the result under its lock.
 * Pure: no I/O, no locks, no logging, no AmsState.
 */
namespace helix {

/**
 * @brief Per-extruder info for toolchanger configurations
 *
 * When AFC detects a toolchanger (num_extruders > 1), the webhook status
 * includes per-extruder data: which lane is loaded and which lanes can
 * feed each extruder.
 */
struct AfcExtruderInfo {
    std::string name;                         ///< Extruder name ("extruder", "extruder1")
    std::string lane_loaded;                  ///< Currently loaded lane (or empty)
    std::vector<std::string> available_lanes; ///< Lanes that can feed this extruder
    float tool_stn = 72.0f;                   ///< Sensor-to-nozzle distance (mm)
    float tool_stn_unload = 100.0f;           ///< Unload retraction distance (mm)
    float tool_sensor_after_extruder = 0.0f;  ///< Post-sensor clear distance (mm)
};

namespace afc {

/// Highest tool number a lane may claim. Anything above this is treated as
/// garbage rather than grown into, so a malformed field cannot size a vector.
constexpr int kMaxToolNumber = 64;

/// A key AFC publishes as a value or as JSON null. The outer nullopt is silence
/// (absent, or a value of another type); an engaged empty inner is AFC stating
/// the value is gone.
template <typename T> using Clearable = std::optional<std::optional<T>>;

/// Which AFC surface a lane record came from. The two spell some keys
/// differently on purpose; see read_lane_identity().
enum class LaneSurface {
    Status,   ///< AFC_stepper / AFC_lane get_status()
    LaneData, ///< the Moonraker `lane_data` database namespace
};

/// The spool identity fields both lane surfaces carry, read one way for both.
struct LaneIdentity {
    /// `color`, when it is a string: a colour, a clear ('' or '#'), or junk.
    std::optional<ams::ColorReading> color;
    std::optional<std::string> material;   ///< Empty is a deliberate clear
    std::optional<std::string> spool_name; ///< Empty is a deliberate clear
    /// `spool_id`: an integer link, or null (AFC unlinks on eject).
    Clearable<int> spool_id;
    /// The spool vendor under any of its spellings. Never empty: an empty
    /// value is not read as a clear.
    std::optional<std::string> brand;
};

[[nodiscard]] LaneIdentity read_lane_identity(const nlohmann::json& lane, LaneSurface surface);

/// One AFC_stepper / AFC_lane status object.
struct LaneStatusDelta {
    LaneIdentity identity;

    std::optional<bool> prep;
    std::optional<bool> load;
    std::optional<bool> loaded_to_hub;
    /// Published only by units with a selector sensor (HTLF, QuattroBox).
    std::optional<bool> selector;
    std::optional<std::string> buffer_status;
    std::optional<std::string> filament_status;
    std::optional<std::string> filament_status_led;
    std::optional<std::string> endstops;
    std::optional<float> dist_hub;

    std::optional<ams::ColorReading> td1_color;
    /// `td1_td`: the reading, or -1 when the key carries no number.
    std::optional<float> td1_td;
    std::optional<std::string> td1_scan_time;

    /// `multi_color_hexes` as '#'-prefixed, comma-joined hex; empty clears.
    std::optional<std::string> multi_color_hexes;
    Clearable<int> bed_temp;      ///< Rounded to whole degrees
    Clearable<int> extruder_temp; ///< One recommended temperature, rounded
    std::optional<float> weight;
    std::optional<float> initial_weight;
    std::optional<bool> remember_spool;

    std::optional<bool> tool_loaded;
    std::optional<std::string> status;
    /// Whether the frame says anything about where the filament is: tool_loaded,
    /// status, or a prep/load key of any type.
    bool presence_reading = false;

    /// `map`, when present: the tools it lists, empty for an unmap.
    std::optional<std::vector<int>> map;
    /// `current_map`, when present: zero or one tool.
    std::optional<std::vector<int>> current_map;
    std::optional<std::string> hub;      ///< "direct", "direct_load" or a hub name
    std::optional<std::string> extruder; ///< AFC_extruder SECTION name
    Clearable<std::string> runout_lane;
};

[[nodiscard]] LaneStatusDelta parse_lane_status(const nlohmann::json& data);

/// A lane's status from what one status frame said, with the lane's latched
/// prep and load sensors.
[[nodiscard]] SlotStatus lane_slot_status(bool tool_loaded, const std::string& status, bool prep,
                                          bool load);

/// AFC's per-lane `map` grammar: `"T<digits>"`, or a list of them. Entries
/// that are not all digits or exceed kMaxToolNumber are skipped one by one.
[[nodiscard]] std::vector<int> parse_lane_map(const nlohmann::json& map_value);

/// The tool a lane_data key names (`"T<digits>"` within kMaxToolNumber), or -1.
[[nodiscard]] int lane_data_tool_key(const std::string& key);

/// One object-valued record of a lane_data payload.
struct LaneDataRecord {
    std::string key;
    int tool = -1; ///< lane_data_tool_key(key)
    LaneIdentity identity;
    /// The presence the record states, or nullopt when it carries no status
    /// key (AFC's own records carry none).
    std::optional<SlotStatus> status;
};

/// Every object-valued record, in the payload's key order.
[[nodiscard]] std::vector<LaneDataRecord> parse_lane_data(const nlohmann::json& lane_data);

/// The payload of a server.database.get_item JSON-RPC reply, at result.value;
/// null when the envelope does not hold one.
[[nodiscard]] const nlohmann::json& database_item_value(const nlohmann::json& response);

/// The AFC object's `message`.
struct Message {
    std::string text; ///< Empty when AFC's queue is empty
    std::string type; ///< "error", "warning" or empty
};

/// A flat-string `units` entry, "<type> <name>".
struct UnitRef {
    std::string type;
    std::string name;
    std::string klipper_key; ///< Status key of the unit-level object
};

/// One `units` entry of the object form, kept by position.
struct UnitEntry {
    std::optional<std::string> name;
    std::optional<bool> connected;
};

struct UnitsDelta {
    std::vector<UnitRef> refs;
    /// Unit name -> lanes, from object entries naming both.
    std::unordered_map<std::string, std::vector<std::string>> lane_map;
    /// One per array element; nullopt where the element is not an object.
    std::vector<std::optional<UnitEntry>> entries;
};

/// The Klipper status key AFC publishes a unit type's object under.
[[nodiscard]] std::string unit_klipper_key(const std::string& type, const std::string& name);

/// The `AFC` object.
struct StateDelta {
    std::optional<std::string> version; ///< Non-empty
    /// current_lane, else current_load; empty when neither names a lane.
    std::string loaded_lane;
    bool mentions_lane = false; ///< current_lane present, any type
    bool mentions_load = false; ///< current_load present, any type
    std::optional<std::string> current_load;
    /// current_load or current_lane is null: AFC states nothing is loaded.
    bool lane_stated_null = false;
    std::optional<std::string> current_state;
    /// 0-based index of the change in progress, -1 before the first.
    std::optional<int> toolchange_index;
    std::optional<int> number_of_toolchanges;
    std::optional<Message> message;
    std::optional<std::vector<LaneDataRecord>> lanes;
    std::optional<UnitsDelta> units;
    std::optional<std::vector<std::string>> hubs;
    std::optional<std::vector<std::string>> buffers;
    std::optional<std::vector<std::string>> extruders;
    Clearable<std::string> next_lane;
    std::optional<bool> position_saved;
    std::optional<std::string> spoolman;
    std::optional<std::vector<std::string>> maps;
    bool multiple_tool_mapping = false; ///< Present, whatever its value
    std::optional<bool> quiet_mode;
    std::optional<bool> led_state;
    std::optional<int> num_extruders;
    /// system.extruders, sorted by name.
    std::optional<std::vector<AfcExtruderInfo>> system_extruders;
    std::optional<bool> error_state;
    std::optional<bool> bypass_state;
};

[[nodiscard]] StateDelta parse_state(const nlohmann::json& afc_data);

/// One AFC_hub object.
struct HubDelta {
    std::optional<bool> state;
    std::optional<float> bowden_length;
};

[[nodiscard]] HubDelta parse_hub(const nlohmann::json& data);

/// One AFC_buffer object.
struct BufferDelta {
    std::optional<std::vector<std::string>> lanes;
    std::optional<bool> fault_detection_enabled;
    std::optional<float> distance_to_fault;
    std::optional<float> error_sensitivity;
    std::optional<std::string> state;
    Clearable<std::string> active_lane;
    Clearable<float> rotation_distance;
    Clearable<float> fault_timer;
    std::optional<float> multiplier;
    std::optional<float> multiplier_high;
    std::optional<float> multiplier_low;
    std::optional<float> fps_value;
    std::optional<float> smoothed_fps;
    std::optional<float> set_point;
};

[[nodiscard]] BufferDelta parse_buffer(const nlohmann::json& data);

/// AFC_buffer's `state` word as a direction.
[[nodiscard]] BufferMotion buffer_motion(const std::string& state);

/// Overlays the fields @p delta carries onto @p health; an omitted field keeps
/// its value and a cleared one returns to the not-reported sentinel.
void apply_buffer_delta(BufferHealth& health, const BufferDelta& delta);

/// One AFC_extruder object.
struct ExtruderObjectDelta {
    std::optional<bool> tool_start;
    std::optional<bool> tool_end;
    std::optional<bool> on_shuttle;
    Clearable<std::string> lane_loaded;
    std::optional<std::string> status;
    std::optional<bool> next_pickup;
    std::optional<bool> is_standalone;
};

[[nodiscard]] ExtruderObjectDelta parse_extruder_object(const nlohmann::json& data);

/// One unit-level object (AFC_BoxTurtle, AFC_OpenAMS, AFC_vivid).
struct UnitObjectDelta {
    std::optional<std::vector<std::string>> lanes;
    std::optional<std::vector<std::string>> extruders;
    std::optional<std::vector<std::string>> hubs;
    std::optional<std::vector<std::string>> buffers;
};

[[nodiscard]] UnitObjectDelta parse_unit_object(const nlohmann::json& data);

struct UnitTopology {
    PathTopology topology = PathTopology::HUB;
    std::vector<bool> lane_is_hub_routed; ///< Parallel to the unit's lanes
};

/// A unit's topology from its lanes' hub routing; a lane with no routing yet
/// counts as neither direct nor hub-routed.
[[nodiscard]] UnitTopology
derive_unit_topology(const std::vector<std::string>& lanes, size_t extruder_count, bool has_hubs,
                     const std::unordered_map<std::string, std::string>& lane_hub_routing);

/// One OpenAMS humidity chip reading.
struct OamsEnvironmentReading {
    std::string unit; ///< The chip's object name, lowercased, without its prefix
    std::optional<float> temperature_c;
    std::optional<float> humidity_pct;
};

/// Every OpenAMS humidity chip object in a status frame.
[[nodiscard]] std::vector<OamsEnvironmentReading>
parse_oams_environment(const nlohmann::json& params);

} // namespace afc
} // namespace helix
