// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_ACE || HELIX_HAS_SNAPMAKER

#include "ace_slot_parse.h"

#include "ams_status_json.h"
#include "json_utils.h"
#include "lane_translation.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix::ace {

using json = nlohmann::json;

DryerInfo ace_pro_dryer_info() {
    DryerInfo dryer;
    dryer.supported = true;
    dryer.active = false;
    dryer.allows_during_print = false;
    dryer.min_temp_c = kDryerMinTempC;
    dryer.max_temp_c = kDryerMaxTempC;
    dryer.max_duration_min = kDryerMaxDurationMin;
    dryer.supports_fan_control = false;
    return dryer;
}

SlotStatus slot_status_from_string(const std::string& status_str) {
    // Native Anycubic GoKlipper uses empty/ready/preload/running/runout;
    // community ValgACE uses available/loaded/ready. "runout" means the slot
    // ran dry mid-print - present-but-empty; map to EMPTY since SlotStatus has
    // no dedicated RUNOUT value. The ACEPRO driver reports the ACE's slot state
    // machine, whose motion states (an ACE 2 slot under feed assist reads
    // "shifting" throughout) and tag read all have the spool in the slot.
    if (status_str == "empty" || status_str == "runout") {
        return SlotStatus::EMPTY;
    }
    if (status_str == "available" || status_str == "loaded" || status_str == "ready" ||
        status_str == "preload" || status_str == "running" || status_str == "feeding" ||
        status_str == "unwinding" || status_str == "shifting" || status_str == "identifying") {
        return SlotStatus::AVAILABLE;
    }
    return SlotStatus::UNKNOWN;
}

TagRead read_tag_state(const json& slot_json) {
    const auto it = slot_json.find("rfid");
    if (it == slot_json.end()) {
        return TagRead::Silent;
    }
    if (it->is_boolean()) {
        return it->get<bool>() ? TagRead::Identified : TagRead::Memory;
    }
    if (it->is_number_integer()) {
        switch (it->get<std::int64_t>()) {
        case 2:
            return TagRead::Identified;
        case 1:
            return TagRead::NoTag;
        default:
            return TagRead::InProgress;
        }
    }
    return TagRead::Silent;
}

std::optional<std::string> read_slot_material(const json& slot_json) {
    auto get_str = [&slot_json](const char* key) -> std::optional<std::string> {
        if (slot_json.is_object() && slot_json.contains(key) && slot_json[key].is_string()) {
            auto value = slot_json[key].get<std::string>();
            if (!value.empty()) {
                return value;
            }
        }
        return std::nullopt;
    };
    if (auto material = get_str("material")) {
        return material;
    }
    return get_str("type");
}

std::optional<int> read_slot_tool(const json& slot_json, int tool_limit) {
    const json* tool = helix::json_util::find_member(slot_json, "tool");
    if (tool == nullptr || !tool->is_number_integer()) {
        return std::nullopt;
    }
    const int value = helix::json_util::as_int(*tool, -1);
    if (value < 0 || value >= tool_limit) {
        return std::nullopt;
    }
    return value;
}

std::optional<uint32_t> parse_slot_color(const json& color_val) {
    // [r, g, b] array: the hub's own numeric spelling, not a colour string, so
    // the shared text grammar has nothing to say about it.
    if (color_val.is_array() && color_val.size() >= 3) {
        // nlohmann's get<int>() reads any number (and a boolean); anything
        // else in the triplet makes the whole colour unreadable.
        const auto& r_val = color_val[0];
        const auto& g_val = color_val[1];
        const auto& b_val = color_val[2];
        const auto readable = [](const json& v) { return v.is_number() || v.is_boolean(); };
        if (!readable(r_val) || !readable(g_val) || !readable(b_val)) {
            spdlog::debug("[ACE] Failed to parse color array");
            return std::nullopt;
        }
        const auto r = static_cast<uint8_t>(r_val.get<int>());
        const auto g = static_cast<uint8_t>(g_val.get<int>());
        const auto b = static_cast<uint8_t>(b_val.get<int>());
        return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(b);
    }

    // Hex string "#RRGGBB" or "0xRRGGBB", read by the same grammar as every
    // other lane-shaped producer. Both of the helper's non-colour answers are
    // "no reading" here: a producer states an absent colour by omitting the
    // key, so an empty string is a malformed value rather than a bay the hub is
    // clearing.
    if (color_val.is_string()) {
        const auto reading = helix::ams::read_lane_color(color_val.get<std::string>());
        if (reading.kind == helix::ams::ColorReadingKind::Observed) {
            return reading.rgb;
        }
        spdlog::debug("[ACE] Slot color '{}' carries no colour", color_val.get<std::string>());
    }

    return std::nullopt;
}

void DryerDelta::apply(DryerInfo& dryer) const {
    if (active) {
        dryer.active = *active;
    }
    if (current_temp_c) {
        dryer.current_temp_c = *current_temp_c;
    }
    if (target_temp_c) {
        dryer.target_temp_c = *target_temp_c;
    }
    if (duration_min) {
        dryer.duration_min = *duration_min;
    }
    if (remaining_min) {
        dryer.remaining_min = *remaining_min;
    }
}

DryerDelta read_dryer_state(const json& data) {
    DryerDelta d;
    if (!data.is_object()) {
        return d;
    }
    // ACE states its temperature top-level (`temp`, the hub's ambient reading
    // near the dryer) - the only reading the Klipper-object spellings carry -
    // so it applies even on a frame with no dryer object. A bridge that ALSO
    // states a current temp inside the dryer object is the more specific
    // reading and wins.
    if (auto v = ams::read_field<float>(data, "temp")) {
        d.current_temp_c = *v;
    }

    const json* dryer = nullptr;
    if (data.contains("dryer") && data["dryer"].is_object()) {
        dryer = &data["dryer"];
    } else if (data.contains("dryer_status") && data["dryer_status"].is_object()) {
        dryer = &data["dryer_status"];
    }
    if (dryer == nullptr) {
        return d;
    }

    // Klipper-object spelling: {status, target_temp, duration, remain_time}
    if (dryer->contains("status") && (*dryer)["status"].is_string()) {
        const std::string ds = (*dryer)["status"].get<std::string>();
        d.active = (ds != "stop" && ds != "idle" && !ds.empty());
    }
    if (dryer->contains("target_temp") && (*dryer)["target_temp"].is_number()) {
        d.target_temp_c = (*dryer)["target_temp"].get<float>();
    }
    if (dryer->contains("duration") && (*dryer)["duration"].is_number()) {
        d.duration_min = (*dryer)["duration"].get<int>();
    }
    if (dryer->contains("remain_time") && (*dryer)["remain_time"].is_number()) {
        d.remaining_min = (*dryer)["remain_time"].get<int>();
    }

    // REST-bridge spelling: {active, current_temp, remaining_minutes,
    // duration_minutes}
    if (dryer->contains("active") && (*dryer)["active"].is_boolean()) {
        d.active = (*dryer)["active"].get<bool>();
    }
    if (dryer->contains("current_temp") && (*dryer)["current_temp"].is_number()) {
        d.current_temp_c = (*dryer)["current_temp"].get<float>();
    }
    if (dryer->contains("remaining_minutes") && (*dryer)["remaining_minutes"].is_number_integer()) {
        d.remaining_min = (*dryer)["remaining_minutes"].get<int>();
    }
    if (dryer->contains("duration_minutes") && (*dryer)["duration_minutes"].is_number_integer()) {
        d.duration_min = (*dryer)["duration_minutes"].get<int>();
    }
    return d;
}

void apply_dryer_state(const json& data, DryerInfo& out) {
    read_dryer_state(data).apply(out);
}

std::optional<AmsError> check_dryer_request(const DryerInfo& dryer, float temp_c,
                                            int duration_min) {
    if (temp_c < dryer.min_temp_c || temp_c > dryer.max_temp_c) {
        return AmsError(
            AmsResult::COMMAND_FAILED, "Temperature out of range: " + std::to_string(temp_c),
            lv_tr("Invalid temperature"),
            fmt::format(lv_tr("Set temperature between {}°C and {}°C"),
                        static_cast<int>(dryer.min_temp_c), static_cast<int>(dryer.max_temp_c)));
    }
    if (duration_min <= 0 || duration_min > dryer.max_duration_min) {
        return AmsError(
            AmsResult::COMMAND_FAILED, "Duration out of range: " + std::to_string(duration_min),
            lv_tr("Invalid duration"),
            fmt::format(lv_tr("Set duration between 1 and {} minutes"), dryer.max_duration_min));
    }
    return std::nullopt;
}

std::pair<float, int> restart_request(const DryerInfo& dryer, float temp_c, int duration_min) {
    const float temp = temp_c < 0 ? dryer.target_temp_c : temp_c;
    // A temperature change restarts the cycle, so it must restart on what is
    // left. Reusing the session length would silently hand back time served.
    const int duration = duration_min >= 0
                             ? duration_min
                             : (dryer.remaining_min > 0 ? dryer.remaining_min : dryer.duration_min);
    return {temp, duration};
}

} // namespace helix::ace

#endif // HELIX_HAS_ACE || HELIX_HAS_SNAPMAKER
