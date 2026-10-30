// src/printer/ace_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_ACE

#include "ace_status_parse.h"

#include "ams_status_json.h"
#include "json_utils.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <limits>

namespace helix::ace {

using json = nlohmann::json;

namespace {

/// Whether an ACEPRO unit's `connection_state` says it is connected; nullopt for
/// a state this reader does not know, which leaves the unit as it was.
std::optional<bool> ace_link_from_state(const std::string& state) {
    if (state == "connected") {
        return true;
    }
    if (state == "initializing" || state == "connecting" || state == "reconnecting" ||
        state == "disabled") {
        return false;
    }
    return std::nullopt;
}

ParkState read_park_state(const json& slot_json) {
    if (slot_json.contains("park_state") && slot_json["park_state"].is_string()) {
        const auto& text = slot_json["park_state"].get_ref<const std::string&>();
        if (text == "parked") {
            return ParkState::Parked;
        }
        if (text == "loaded") {
            return ParkState::Loaded;
        }
    }
    return ParkState::Unknown;
}

/// A number or a numeric string that fits an int.
std::optional<int> read_lenient_int(const json& obj, const char* key) {
    const json* v = json_util::find_member(obj, key);
    std::int64_t out = 0;
    if (v == nullptr || !json_util::detail::to_i64(*v, out) ||
        out < std::numeric_limits<int>::min() || out > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(out);
}

/// The local slot index a native GoKlipper `current_filament` names
/// ("<unitId>-<localIndex>"), when it names one that is not negative. An empty
/// string means nothing loaded, which the seat logic decides on its own.
std::optional<int> read_current_filament(const std::string& cf) {
    const auto dash = cf.find('-');
    if (cf.empty() || dash == std::string::npos || dash + 1 >= cf.size()) {
        return std::nullopt;
    }
    const auto local_index = text_io::parse_leading<int>(cf.substr(dash + 1));
    if (!local_index) {
        spdlog::debug("[ACE] Failed to parse current_filament '{}'", cf);
        return std::nullopt;
    }
    if (*local_index < 0) {
        return std::nullopt;
    }
    return local_index;
}

} // namespace

SlotDelta read_slot(const json& slot_json) {
    SlotDelta d;
    if (auto v = ams::read_field<std::string>(slot_json, "status")) {
        d.status = slot_status_from_string(*v);
        d.status_word = std::move(*v);
    }
    if (slot_json.contains("color")) {
        d.color = parse_slot_color(slot_json["color"]);
    }
    d.material = read_slot_material(slot_json);
    d.tool = read_slot_tool(slot_json, kMaxUnits * kSlotsPerUnit).value_or(-1);
    d.tag_read = read_tag_state(slot_json);
    d.park_state = read_park_state(slot_json);
    d.temp_min = read_lenient_int(slot_json, "temp_min");
    d.temp_max = read_lenient_int(slot_json, "temp_max");
    return d;
}

SlotArray read_slots(const json& data) {
    SlotArray out;
    if (!data.contains("slots") || !data["slots"].is_array()) {
        return out;
    }
    const auto& arr = data["slots"];
    if (arr.size() > kMaxSlotsPerObject) {
        out.refused_size = arr.size();
        return out;
    }
    std::vector<std::optional<SlotDelta>> entries;
    entries.reserve(arr.size());
    for (const auto& slot_json : arr) {
        if (slot_json.is_object()) {
            entries.emplace_back(read_slot(slot_json));
        } else {
            entries.emplace_back(std::nullopt);
        }
    }
    out.entries = std::move(entries);
    return out;
}

PathSensors read_path_sensors(const json& data) {
    PathSensors s;
    s.rdm = ams::read_field<bool>(data, "rdm_sensor");
    s.toolhead = ams::read_field<bool>(data, "toolhead_sensor");
    return s;
}

ObjectDelta parse_ace_object(const json& data) {
    ObjectDelta d;
    d.model = ams::read_field<std::string>(data, "model");
    d.protocol = ams::read_field<std::string>(data, "protocol");
    d.firmware = ams::read_field<std::string>(data, "firmware");
    if (auto v = ams::read_field<std::string>(data, "status")) {
        d.action = ams_action_from_string(*v);
    }

    d.slots = read_slots(data);
    if (data.contains("slots") && data["slots"].is_array()) {
        const auto& arr = data["slots"];
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const auto& sj = arr[i];
            if (sj.contains("status") && sj["status"].is_string() &&
                sj["status"].get_ref<const std::string&>() == "loaded") {
                d.first_loaded_entry = i;
                break;
            }
        }
    }

    if (auto v = ams::read_field<std::string>(data, "connection_state")) {
        d.linked = ace_link_from_state(*v);
    }
    d.dryer = read_dryer_state(data);
    d.humidity = ams::read_field<float>(data, "humidity");
    d.loaded_slot = ams::read_integer_field(data, "loaded_slot");
    if (auto v = ams::read_field<std::string>(data, "current_filament")) {
        d.current_filament_slot = read_current_filament(*v);
    }
    d.current_index = ams::read_integer_field(data, "current_index");
    d.sensors = read_path_sensors(data);
    d.target_index = ams::read_integer_field(data, "target_index");
    d.ace_pro_enabled = ams::read_field<bool>(data, "ace_pro_enabled");
    d.feed_assist_slot = ams::read_integer_field(data, "feed_assist_slot");
    d.endless_spool_enabled = ams::read_field<bool>(data, "endless_spool_enabled");
    d.endless_spool_match_mode = ams::read_field<std::string>(data, "endless_spool_match_mode");
    return d;
}

InfoResponse parse_info_response(const json& data) {
    InfoResponse r;
    r.names_model = ams::read_field<std::string>(data, "model").has_value();
    r.version = ams::read_field<std::string>(data, "version");
    if (auto v = ams::read_integer_field(data, "slot_count")) {
        if (*v < 0 || static_cast<std::size_t>(*v) > kMaxSlotsPerObject) {
            r.refused_slot_count = *v;
        } else {
            r.slot_count = *v;
        }
    }
    return r;
}

StatusResponse parse_status_response(const json& data) {
    StatusResponse r;
    r.model = ams::read_field<std::string>(data, "model");
    r.firmware = ams::read_field<std::string>(data, "firmware");
    r.loaded_slot = ams::read_integer_field(data, "loaded_slot");
    r.names_manager = data.contains("ace_manager");
    if (auto v = ams::read_integer_field(data, "instance_index"); v && *v >= 0) {
        r.instance_index = *v;
    }
    if (r.names_manager && data["ace_manager"].is_object()) {
        const auto& manager = data["ace_manager"];
        r.manager_object = true;
        r.manager_current_index = ams::read_integer_field(manager, "current_index");
        r.manager_sensors = read_path_sensors(manager);
        r.manager_target_index = ams::read_integer_field(manager, "target_index");
    }

    // The ValgACE bridge names the action field `action`; the ACEPRO driver names
    // it `status` ("ready"/etc.) with the same loading/unloading/error
    // vocabulary the object path maps (#1069). A stated `action`, even an empty
    // one, is the bridge's answer and `status` is not consulted.
    std::string action_str;
    if (auto v = ams::read_field<std::string>(data, "action")) {
        action_str = std::move(*v);
    } else if (auto v = ams::read_field<std::string>(data, "status")) {
        action_str = std::move(*v);
    }
    if (!action_str.empty()) {
        r.action = ams_action_from_string(action_str);
    }
    r.action_from_unit_status = !data.contains("action");
    r.dryer = read_dryer_state(data);
    return r;
}

} // namespace helix::ace

#endif // HELIX_HAS_ACE
