// src/printer/afc_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_status_parse.h"

#include "ams_status_json.h"
#include "humidity_sensor_types.h"
#include "text_io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>

namespace helix::afc {
namespace {

bool all_digits(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

/// The strings in @p obj[key] when it is an array; non-string elements skipped.
std::optional<std::vector<std::string>> read_string_list(const nlohmann::json& obj,
                                                         const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (const auto& v : *it) {
        if (v.is_string()) {
            out.push_back(v.get<std::string>());
        }
    }
    return out;
}

/// @p obj[key] when it is a JSON integer.
std::optional<int> read_integer_field(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return it == obj.end() ? std::nullopt : ams::read_integer(*it);
}

template <typename T> Clearable<T> read_clearable(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return std::nullopt;
    }
    if (it->is_null()) {
        return std::optional<T>{};
    }
    if (auto v = ams::read_scalar<T>(*it)) {
        return std::optional<T>{std::move(*v)};
    }
    return std::nullopt;
}

/// A temperature key: rounded to whole degrees, or null for AFC's eject clear.
Clearable<int> read_temperature(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return std::nullopt;
    }
    if (it->is_number()) {
        return std::optional<int>{static_cast<int>(std::lround(it->get<double>()))};
    }
    if (it->is_null()) {
        return std::optional<int>{};
    }
    return std::nullopt;
}

std::optional<ams::ColorReading> read_color(const nlohmann::json& obj, const char* key) {
    if (auto v = ams::read_field<std::string>(obj, key)) {
        return ams::read_lane_color(*v);
    }
    return std::nullopt;
}

// The spool vendor under every spelling the ecosystem uses. lane_data spells it
// `vendor_name` (the key Happy Hare established in that shared namespace) and
// get_status `spool_vendor` (AFCLane's own attribute name), deliberately
// (AFCProject/AFC-Klipper-Add-On#808, shipped by #833). `vendor` is the key
// our own to_lane_data_record() emits into AFC's private namespace (#1158);
// `brand` is a defensive fourth spelling.
//
// Empty values are ignored rather than read as a clear: whether an unlinked
// lane omits the key or publishes "" is unobserved, and guessing wrong in the
// clearing direction wipes a brand nothing re-supplies. A user's declaration is
// safe either way, since both lane paths re-apply it after this read.
std::optional<std::string> read_vendor(const nlohmann::json& src) {
    for (const char* key : {"vendor_name", "spool_vendor", "vendor", "brand"}) {
        auto v = ams::read_field<std::string>(src, key);
        if (v && !v->empty()) {
            return v;
        }
    }
    return std::nullopt;
}

/// lane_data's presence keys. AFC's own records carry none of them, and absent
/// means unchanged: defaulting would clobber the status AFC_stepper derived.
/// `loaded` is hub-loaded; only tool_loaded puts filament at the extruder.
std::optional<SlotStatus> lane_data_status(const nlohmann::json& lane) {
    if (ams::read_field<bool>(lane, "tool_loaded").value_or(false)) {
        return SlotStatus::LOADED;
    }
    if (ams::read_field<bool>(lane, "loaded").value_or(false)) {
        return SlotStatus::AVAILABLE;
    }
    const bool has_status_key = lane.contains("tool_loaded") || lane.contains("loaded") ||
                                lane.contains("available") || lane.contains("empty");
    if (!has_status_key) {
        return std::nullopt;
    }
    if (ams::read_field<bool>(lane, "available").value_or(false)) {
        return SlotStatus::AVAILABLE;
    }
    if (ams::read_field<bool>(lane, "empty").value_or(false)) {
        return SlotStatus::EMPTY;
    }
    return SlotStatus::AVAILABLE;
}

UnitsDelta parse_units(const nlohmann::json& units_json) {
    UnitsDelta d;
    for (const auto& unit_json : units_json) {
        // Flat string form: "OpenAMS AMS_1", "Box_Turtle Turtle_1"
        if (unit_json.is_string()) {
            const std::string unit_str = unit_json.get<std::string>();
            const auto space_pos = unit_str.find(' ');
            if (space_pos != std::string::npos) {
                UnitRef ref;
                ref.type = unit_str.substr(0, space_pos);
                ref.name = unit_str.substr(space_pos + 1);
                ref.klipper_key = unit_klipper_key(ref.type, ref.name);
                d.refs.push_back(std::move(ref));
            }
            d.entries.emplace_back();
            continue;
        }
        if (!unit_json.is_object()) {
            d.entries.emplace_back();
            continue;
        }
        // Object form: {"name": "...", "lanes": [...], "connected": ...}
        UnitEntry entry;
        entry.name = ams::read_field<std::string>(unit_json, "name");
        entry.connected = ams::read_field<bool>(unit_json, "connected");
        if (auto lanes = read_string_list(unit_json, "lanes")) {
            const std::string name = entry.name.value_or("");
            if (!name.empty() && !lanes->empty()) {
                d.lane_map[name] = std::move(*lanes);
            }
        }
        d.entries.emplace_back(std::move(entry));
    }
    return d;
}

std::vector<AfcExtruderInfo> parse_system_extruders(const nlohmann::json& extruders_json) {
    std::vector<std::string> names;
    for (auto it = extruders_json.begin(); it != extruders_json.end(); ++it) {
        names.push_back(it.key());
    }
    std::sort(names.begin(), names.end());

    std::vector<AfcExtruderInfo> out;
    for (const auto& name : names) {
        const auto& ext = extruders_json[name];
        if (!ext.is_object()) {
            continue;
        }
        AfcExtruderInfo info;
        info.name = name;
        // null or any other type leaves no lane loaded
        if (auto v = ams::read_field<std::string>(ext, "lane_loaded")) {
            info.lane_loaded = std::move(*v);
        }
        if (auto lanes = read_string_list(ext, "lanes")) {
            info.available_lanes = std::move(*lanes);
        }
        if (auto v = ams::read_field<float>(ext, "tool_stn")) {
            info.tool_stn = *v;
        }
        if (auto v = ams::read_field<float>(ext, "tool_stn_unload")) {
            info.tool_stn_unload = *v;
        }
        if (auto v = ams::read_field<float>(ext, "tool_sensor_after_extruder")) {
            info.tool_sensor_after_extruder = *v;
        }
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace

LaneIdentity read_lane_identity(const nlohmann::json& lane, LaneSurface surface) {
    LaneIdentity id;
    // AFC writes "#RRGGBB"; clear_values() writes '' on eject and SET_COLOR with
    // no value stores the bare '#', both clears. Anything else that will not
    // parse is a value we cannot read, not a lane with no colour.
    id.color = read_color(lane, "color");
    id.material = ams::read_field<std::string>(lane, "material");

    // The filament name AFC copied out of Spoolman. get_status calls it
    // `filament_name`, AFCLane's attribute; lane_data uses `name`, the shared
    // spelling Happy Hare publishes (#833), and the attribute name defensively.
    // get_status is not offered `name`: there it would be the lane's own name.
    const auto name_keys = surface == LaneSurface::LaneData
                               ? std::initializer_list<const char*>{"name", "filament_name"}
                               : std::initializer_list<const char*>{"filament_name"};
    for (const char* key : name_keys) {
        if (auto v = ams::read_field<std::string>(lane, key)) {
            id.spool_name = std::move(*v);
            break;
        }
    }

    // JSON null is AFC dropping the link (clear_values() sets spool_id=None on
    // eject); a float or a string is neither a link nor a clear.
    if (auto v = read_integer_field(lane, "spool_id")) {
        id.spool_id = std::optional<int>{*v};
    } else if (lane.contains("spool_id") && lane["spool_id"].is_null()) {
        id.spool_id = std::optional<int>{};
    }

    id.brand = read_vendor(lane);
    return id;
}

LaneStatusDelta parse_lane_status(const nlohmann::json& data) {
    LaneStatusDelta d;
    d.identity = read_lane_identity(data, LaneSurface::Status);

    d.prep = ams::read_field<bool>(data, "prep");
    d.load = ams::read_field<bool>(data, "load");
    d.loaded_to_hub = ams::read_field<bool>(data, "loaded_to_hub");
    d.selector = ams::read_field<bool>(data, "selector");
    d.buffer_status = ams::read_field<std::string>(data, "buffer_status");
    d.filament_status = ams::read_field<std::string>(data, "filament_status");
    d.filament_status_led = ams::read_field<std::string>(data, "filament_status_led");
    d.endstops = ams::read_field<std::string>(data, "endstops");
    d.dist_hub = ams::read_field<float>(data, "dist_hub");

    // TD-1 scanner reading: empty strings mean no scan; the colour is bare hex.
    d.td1_color = read_color(data, "td1_color");
    if (data.contains("td1_td")) {
        d.td1_td = ams::read_scalar<float>(data["td1_td"]).value_or(-1.0f);
    }
    d.td1_scan_time = ams::read_field<std::string>(data, "td1_scan_time");

    // Firmware carries these as BARE hex strings, having split Spoolman's
    // comma-joined `multi_color_hexes`. Normalise to the '#'-prefixed
    // comma-joined form every other backend uses.
    const auto hexes = data.find("multi_color_hexes");
    if (hexes != data.end() && hexes->is_array()) {
        std::string joined;
        for (const auto& hex : *hexes) {
            if (!hex.is_string()) {
                continue;
            }
            const std::string h = hex.get<std::string>();
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
        d.multi_color_hexes = std::move(joined);
    }

    d.bed_temp = read_temperature(data, "bed_temp");
    // AFC_spool fills this from the linked spool's settings_extruder_temp: one
    // recommended print temperature, not a range, and None until a spool links.
    d.extruder_temp = read_temperature(data, "extruder_temp");
    d.weight = ams::read_field<float>(data, "weight");
    d.initial_weight = ams::read_field<float>(data, "initial_weight");
    d.remember_spool = ams::read_field<bool>(data, "remember_spool");

    d.tool_loaded = ams::read_field<bool>(data, "tool_loaded");
    d.status = ams::read_field<std::string>(data, "status");
    d.presence_reading =
        d.tool_loaded || d.status || data.contains("prep") || data.contains("load");

    if (data.contains("map")) {
        d.map = parse_lane_map(data["map"]);
    }
    if (data.contains("current_map")) {
        d.current_map = parse_lane_map(data["current_map"]);
    }
    d.hub = ams::read_field<std::string>(data, "hub");
    d.extruder = ams::read_field<std::string>(data, "extruder");
    d.runout_lane = read_clearable<std::string>(data, "runout_lane");
    return d;
}

SlotStatus lane_slot_status(bool tool_loaded, const std::string& status, bool prep, bool load) {
    // The lane `status` vocabulary is closed: AFCLaneState in AFC_lane.py
    // (v1.2.0; v1.1.0 is the same list minus "Infinite Runout").
    //
    // "Loaded" means loaded to the HUB. The two that mean toolhead are "Tooled"
    // and "Tool Loaded", the latter set the moment filament trips the
    // pre-extruder sensor. There is no "Ready" lane status in any AFC version:
    // that word belongs to the separate `filament_status` field.
    if (tool_loaded || status == "Tooled" || status == "Tool Loaded") {
        return SlotStatus::LOADED;
    }
    if (status == "Loaded" || prep || load) {
        return SlotStatus::AVAILABLE;
    }
    // "None", empty, or an unrecognized word such as "Error" after a failed load:
    // prep, load and tool_loaded are all false here, so nothing is physically
    // present. A lane error is reported separately from the status.
    return SlotStatus::EMPTY;
}

// Two wire shapes are live at once. Before virtual tools (AFC #605) a lane's
// map is a single "T0" string; from #605 on it is ALWAYS a list, even for a
// lane with one tool and with virtual tools disabled.
//
// The tool number must be ALL digits: a laxer parse would read a comma-joined
// "T14,13" as a confident, wrong, single mapping.
std::vector<int> parse_lane_map(const nlohmann::json& map_value) {
    auto parse_one = [](const nlohmann::json& value, std::vector<int>& out) {
        if (!value.is_string()) {
            return;
        }
        const std::string token = value.get<std::string>();
        if (token.size() < 2 || token[0] != 'T') {
            return;
        }
        const std::string digits = token.substr(1);
        if (!all_digits(digits)) {
            return;
        }
        const auto tool = text_io::parse_leading<int>(digits);
        if (tool && *tool >= 0 && *tool <= kMaxToolNumber) {
            out.push_back(*tool);
        }
    };

    std::vector<int> tools;
    if (map_value.is_array()) {
        for (const auto& entry : map_value) {
            parse_one(entry, tools);
        }
    } else {
        parse_one(map_value, tools);
    }
    return tools;
}

int lane_data_tool_key(const std::string& key) {
    if (key.size() < 2 || key[0] != 'T' || !all_digits(key.substr(1))) {
        return -1;
    }
    const int tool = text_io::parse_leading<int>(key.substr(1)).value_or(-1);
    return tool <= kMaxToolNumber ? tool : -1;
}

std::vector<LaneDataRecord> parse_lane_data(const nlohmann::json& lane_data) {
    std::vector<LaneDataRecord> out;
    if (!lane_data.is_object()) {
        return out;
    }
    for (auto it = lane_data.begin(); it != lane_data.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        LaneDataRecord r;
        r.key = it.key();
        r.tool = lane_data_tool_key(r.key);
        r.identity = read_lane_identity(it.value(), LaneSurface::LaneData);
        r.status = lane_data_status(it.value());
        out.push_back(std::move(r));
    }
    return out;
}

// send_jsonrpc delivers the whole JSON-RPC message, so the payload sits at
// result.value (prestonbrown/helixscreen#1148). Deliberately strict about the
// envelope: a payload is an arbitrary object, so "envelope or payload?" cannot
// be decided from its shape.
const nlohmann::json& database_item_value(const nlohmann::json& response) {
    static const nlohmann::json kNull;
    if (!response.is_object()) {
        return kNull;
    }
    const auto result = response.find("result");
    if (result == response.end() || !result->is_object()) {
        return kNull;
    }
    const auto value = result->find("value");
    return value != result->end() ? *value : kNull;
}

// The Klipper object prefix is "AFC_" + the type with underscores removed
// ("Box_Turtle" -> "AFC_BoxTurtle"), except ViViD, whose module file is
// lowercase.
std::string unit_klipper_key(const std::string& type, const std::string& name) {
    std::string klipper_type;
    if (type == "ViViD") {
        klipper_type = "vivid";
    } else {
        klipper_type = type;
        klipper_type.erase(std::remove(klipper_type.begin(), klipper_type.end(), '_'),
                           klipper_type.end());
    }
    return "AFC_" + klipper_type + " " + name;
}

StateDelta parse_state(const nlohmann::json& afc) {
    StateDelta d;

    if (auto v = ams::read_field<std::string>(afc, "version"); v && !v->empty()) {
        d.version = std::move(*v);
    }

    // current_lane first, then current_load; an empty string is absent.
    d.loaded_lane = ams::read_field<std::string>(afc, "current_lane").value_or("");
    d.current_load = ams::read_field<std::string>(afc, "current_load");
    if (d.loaded_lane.empty() && d.current_load) {
        d.loaded_lane = *d.current_load;
    }
    d.mentions_lane = afc.contains("current_lane");
    d.mentions_load = afc.contains("current_load");
    d.lane_stated_null = (d.mentions_load && afc["current_load"].is_null()) ||
                         (d.mentions_lane && afc["current_lane"].is_null());

    d.current_state = ams::read_field<std::string>(afc, "current_state");

    // current_toolchange on the wire is a 1-BASED count of changes started (AFC
    // clamps its internal -1 to 0 on the way out), so it is exactly the number
    // AFC prints in "Change N out of M". Subtract one for the 0-based index,
    // floored at the -1 "none yet" sentinel.
    if (auto v = read_integer_field(afc, "current_toolchange")) {
        d.toolchange_index = std::max(-1, *v - 1);
    }
    d.number_of_toolchanges = read_integer_field(afc, "number_of_toolchanges");

    const auto msg = afc.find("message");
    if (msg != afc.end() && msg->is_object()) {
        if (auto text = ams::read_field<std::string>(*msg, "message")) {
            d.message =
                Message{std::move(*text), ams::read_field<std::string>(*msg, "type").value_or("")};
        }
    }

    const auto lanes = afc.find("lanes");
    if (lanes != afc.end() && lanes->is_object()) {
        d.lanes = parse_lane_data(*lanes);
    }
    const auto units = afc.find("units");
    if (units != afc.end() && units->is_array()) {
        d.units = parse_units(*units);
    }

    d.hubs = read_string_list(afc, "hubs");
    d.buffers = read_string_list(afc, "buffers");
    d.extruders = read_string_list(afc, "extruders");
    d.next_lane = read_clearable<std::string>(afc, "next_lane");
    d.position_saved = ams::read_field<bool>(afc, "position_saved");
    // AFC publishes false, not null, when Spoolman is off.
    d.spoolman = ams::read_field<std::string>(afc, "spoolman");
    d.maps = read_string_list(afc, "maps");
    // Virtual-tools firmware (#832) publishes this whatever the opt-in's value;
    // presence alone is the signal.
    d.multiple_tool_mapping = afc.contains("multiple_tool_mapping");
    d.quiet_mode = ams::read_field<bool>(afc, "quiet_mode");
    d.led_state = ams::read_field<bool>(afc, "led_state");

    const auto system = afc.find("system");
    if (system != afc.end() && system->is_object()) {
        d.num_extruders = read_integer_field(*system, "num_extruders");
        const auto exts = system->find("extruders");
        if (exts != system->end() && exts->is_object()) {
            d.system_extruders = parse_system_extruders(*exts);
        }
    }

    d.error_state = ams::read_field<bool>(afc, "error_state");
    d.bypass_state = ams::read_field<bool>(afc, "bypass_state");
    return d;
}

HubDelta parse_hub(const nlohmann::json& data) {
    return HubDelta{ams::read_field<bool>(data, "state"),
                    ams::read_field<float>(data, "afc_bowden_length")};
}

BufferDelta parse_buffer(const nlohmann::json& data) {
    BufferDelta d;
    d.lanes = read_string_list(data, "lanes");
    d.fault_detection_enabled = ams::read_field<bool>(data, "fault_detection_enabled");
    d.distance_to_fault = ams::read_field<float>(data, "distance_to_fault");
    d.error_sensitivity = ams::read_field<float>(data, "error_sensitivity");
    d.state = ams::read_field<std::string>(data, "state");
    // null whenever the buffer is disabled or no lane is loaded (v1.2.0+).
    d.active_lane = read_clearable<std::string>(data, "active_lane");
    d.rotation_distance = read_clearable<float>(data, "rotation_distance");
    d.fault_timer = read_clearable<float>(data, "fault_timer");
    // AFC seeds _last_multiplier with an int 1, so any number is accepted.
    d.multiplier = ams::read_field<float>(data, "multiplier");
    d.multiplier_high = ams::read_field<float>(data, "multiplier_high");
    d.multiplier_low = ams::read_field<float>(data, "multiplier_low");
    // `type: FPS_PSF` only; a TurtleNeck buffer sends none of these.
    d.fps_value = ams::read_field<float>(data, "fps_value");
    d.smoothed_fps = ams::read_field<float>(data, "smoothed_fps");
    d.set_point = ams::read_field<float>(data, "set_point");
    return d;
}

BufferMotion buffer_motion(const std::string& state) {
    if (state == "Advancing") {
        return BufferMotion::Advancing;
    }
    if (state == "Trailing") {
        return BufferMotion::Trailing;
    }
    return BufferMotion::Unknown;
}

void apply_buffer_delta(BufferHealth& health, const BufferDelta& d) {
    if (d.fault_detection_enabled) {
        health.fault_detection_enabled = *d.fault_detection_enabled;
    }
    if (d.distance_to_fault) {
        health.distance_to_fault = *d.distance_to_fault;
    }
    if (d.error_sensitivity) {
        health.error_sensitivity = *d.error_sensitivity;
    }
    if (d.state) {
        health.state = *d.state;
        health.motion = buffer_motion(health.state);
    }
    if (d.active_lane) {
        health.active_lane = d.active_lane->value_or("");
    }
    if (d.rotation_distance) {
        health.rotation_distance = d.rotation_distance->value_or(-1.0f);
    }
    if (d.fault_timer) {
        health.fault_timer = d.fault_timer->value_or(-1.0f);
    }
    if (d.multiplier) {
        health.multiplier = *d.multiplier;
    }
    if (d.multiplier_high) {
        health.multiplier_high = *d.multiplier_high;
    }
    if (d.multiplier_low) {
        health.multiplier_low = *d.multiplier_low;
    }
    if (d.smoothed_fps) {
        health.smoothed_fps = *d.smoothed_fps;
        health.fps_reported = true;
    }
    if (d.fps_value) {
        health.fps_value = *d.fps_value;
    }
    if (d.set_point) {
        health.fps_set_point = *d.set_point;
    }
}

ExtruderObjectDelta parse_extruder_object(const nlohmann::json& data) {
    ExtruderObjectDelta d;
    d.tool_start = ams::read_field<bool>(data, "tool_start_status");
    d.tool_end = ams::read_field<bool>(data, "tool_end_status");
    // Absent on older AFC, which is not the same as reported false.
    d.on_shuttle = ams::read_field<bool>(data, "on_shuttle");
    // null is set_unloaded() saying nothing is seated here.
    d.lane_loaded = read_clearable<std::string>(data, "lane_loaded");
    // Toolchanger state (AFC v1.2.0 #768).
    d.status = ams::read_field<std::string>(data, "status");
    d.next_pickup = ams::read_field<bool>(data, "next_pickup");
    d.is_standalone = ams::read_field<bool>(data, "is_standalone");
    return d;
}

UnitObjectDelta parse_unit_object(const nlohmann::json& data) {
    return UnitObjectDelta{read_string_list(data, "lanes"), read_string_list(data, "extruders"),
                           read_string_list(data, "hubs"), read_string_list(data, "buffers")};
}

UnitTopology derive_unit_topology(const std::vector<std::string>& lanes, size_t extruder_count,
                                  bool has_hubs,
                                  const std::unordered_map<std::string, std::string>& routing) {
    UnitTopology out;
    bool has_direct = false;
    bool has_hub_routed = false;
    for (const auto& lane : lanes) {
        const auto it = routing.find(lane);
        if (it == routing.end()) {
            // Unit objects sort before AFC_lane ones, so a frame can carry a unit
            // whose lanes were never parsed. Unknown is not direct: counting it
            // so flips a pure-hub unit to MIXED (#1229).
            out.lane_is_hub_routed.push_back(false);
            continue;
        }
        // "direct" and "direct_load" go straight to an extruder; any other
        // value names the hub the lane routes through.
        const bool is_hub = it->second.rfind("direct", 0) != 0;
        out.lane_is_hub_routed.push_back(is_hub);
        (is_hub ? has_hub_routed : has_direct) = true;
    }

    if (has_direct && has_hub_routed) {
        out.topology = PathTopology::MIXED;
    } else if (has_hub_routed && extruder_count > 1 && extruder_count == lanes.size()) {
        // A hub AND an extruder per lane: independent paths with hub sensors
        // along them (an ACE Pro as lane loader through AFC).
        out.topology = PathTopology::PARALLEL;
    } else if (has_hub_routed) {
        out.topology = PathTopology::HUB;
    } else if (has_direct && extruder_count > 1) {
        out.topology = PathTopology::PARALLEL;
    } else if (has_hubs && extruder_count <= 1) {
        out.topology = PathTopology::HUB;
    } else if (extruder_count > 1) {
        out.topology = PathTopology::PARALLEL;
    } else {
        out.topology = PathTopology::HUB;
    }
    return out;
}

std::vector<OamsEnvironmentReading> parse_oams_environment(const nlohmann::json& params) {
    std::vector<OamsEnvironmentReading> out;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        // OpenAMS publishes its HDC1080 as "aht3x <name>" by default and as
        // "temperature_oams <name>" when the alias is switched off.
        const auto* chip = sensors::humidity_chip_for_object(it.key());
        if (!chip || (chip->type != sensors::HumiditySensorType::AHT3X &&
                      chip->type != sensors::HumiditySensorType::TEMPERATURE_OAMS)) {
            continue;
        }
        OamsEnvironmentReading r;
        r.unit = text_io::to_lower(it.key().substr(chip->klipper_prefix.size()));
        r.temperature_c = ams::read_field<float>(it.value(), "temperature");
        r.humidity_pct = ams::read_field<float>(it.value(), "humidity");
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace helix::afc
