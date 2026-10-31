// src/printer/snapmaker_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_status_parse.h"

#include "ams_status_json.h"
#include "json_utils.h"
#include "lane_translation.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string_view>

namespace helix {

void ExtruderToolState::apply(const ExtruderDelta& delta) {
    if (delta.state) {
        state = *delta.state;
    }
    if (delta.park_pin) {
        park_pin = *delta.park_pin;
    }
    if (delta.active_pin) {
        active_pin = *delta.active_pin;
    }
    if (delta.activating_move) {
        activating_move = *delta.activating_move;
    }
    for (size_t i = 0; i < extruder_offset.size(); ++i) {
        if (delta.extruder_offset[i]) {
            extruder_offset[i] = *delta.extruder_offset[i];
        }
    }
    if (delta.switch_count) {
        switch_count = *delta.switch_count;
    }
    if (delta.retry_count) {
        retry_count = *delta.retry_count;
    }
    if (delta.error_count) {
        error_count = *delta.error_count;
    }
    if (delta.nozzle_diameter) {
        nozzle_diameter = *delta.nozzle_diameter;
    }
    if (delta.nozzle_volume_type) {
        nozzle_volume_type = *delta.nozzle_volume_type;
    }
}

namespace snapmaker {

ExtruderDelta parse_extruder_delta(const nlohmann::json& json) {
    ExtruderDelta d;
    if (auto v = ams::read_field<std::string>(json, "state")) {
        d.state = std::move(*v);
    }
    if (auto v = ams::read_field<bool>(json, "park_pin")) {
        d.park_pin = *v;
    }
    if (auto v = ams::read_field<bool>(json, "active_pin")) {
        d.active_pin = *v;
    }
    if (auto v = ams::read_field<bool>(json, "activating_move")) {
        d.activating_move = *v;
    }
    if (json.contains("extruder_offset") && json["extruder_offset"].is_array()) {
        const auto& arr = json["extruder_offset"];
        for (size_t i = 0; i < std::min(arr.size(), d.extruder_offset.size()); i++) {
            if (arr[i].is_number()) {
                d.extruder_offset[i] = arr[i].get<float>();
            }
        }
    }
    if (auto v = ams::read_field<int>(json, "switch_count")) {
        d.switch_count = *v;
    }
    if (auto v = ams::read_field<int>(json, "retry_count")) {
        d.retry_count = *v;
    }
    if (auto v = ams::read_field<int>(json, "error_count")) {
        d.error_count = *v;
    }
    // A diameter that is not a positive number says nothing about the nozzle.
    if (auto v = ams::read_field<double>(json, "nozzle_diameter");
        v && std::isfinite(*v) && *v > 0.0) {
        d.nozzle_diameter = *v;
    }
    if (auto v = ams::read_field<std::string>(json, "nozzle_volume_type")) {
        d.nozzle_volume_type = std::move(*v);
    }
    return d;
}

SnapmakerRfidInfo parse_rfid_info(const nlohmann::json& json) {
    SnapmakerRfidInfo info;

    if (auto v = ams::read_field<std::string>(json, "MAIN_TYPE")) {
        info.main_type = std::move(*v);
    }
    if (auto v = ams::read_field<std::string>(json, "SUB_TYPE")) {
        info.sub_type = std::move(*v);
    }
    if (auto v = ams::read_field<std::string>(json, "MANUFACTURER")) {
        info.manufacturer = std::move(*v);
    }
    if (auto v = ams::read_field<std::string>(json, "VENDOR")) {
        info.vendor = std::move(*v);
    }
    if (json.contains("ARGB_COLOR") && json["ARGB_COLOR"].is_number()) {
        // ARGB -> RGB: mask off the alpha byte
        uint32_t argb = json["ARGB_COLOR"].get<uint32_t>();
        info.color_rgb = argb & 0x00FFFFFF;
    }
    if (auto v = ams::read_field<int>(json, "HOTEND_MIN_TEMP")) {
        info.hotend_min_temp = *v;
    }
    if (auto v = ams::read_field<int>(json, "HOTEND_MAX_TEMP")) {
        info.hotend_max_temp = *v;
    }
    if (auto v = ams::read_field<int>(json, "BED_TEMP")) {
        info.bed_temp = *v;
    }
    if (auto v = ams::read_field<int>(json, "WEIGHT")) {
        info.weight_g = *v;
    }
    // COLOR_NUMS counts the RGB_n fields the tag carries; one is the plain
    // colour ARGB_COLOR already states.
    const int color_nums = json_util::safe_int(json, "COLOR_NUMS", 1);
    if (color_nums >= 2) {
        std::string joined;
        for (int n = 1; n <= std::min(color_nums, 5); ++n) {
            const std::string key = "RGB_" + std::to_string(n);
            const nlohmann::json* rgb = json_util::find_member(json, key.c_str());
            if (!rgb || !rgb->is_number()) {
                joined.clear();
                break;
            }
            if (!joined.empty()) {
                joined += ',';
            }
            joined += fmt::format("#{:06X}",
                                  static_cast<uint32_t>(json_util::as_int64(*rgb)) & 0xFFFFFFu);
        }
        info.multi_color_hexes = std::move(joined);
    }
    // CARD_UID is a 4-byte array like [144, 32, 196, 2]. Canonicalize to a
    // comma-joined string so the override system's baseline comparison is a
    // simple string == string check. Empty / missing array stays as empty
    // string (treated as "no tag / unread" by check_hardware_event_clear).
    const nlohmann::json* card_uid = json_util::find_member(json, "CARD_UID");
    if (card_uid && card_uid->is_array()) {
        std::string uid;
        bool has_card = false;
        bool readable = true;
        for (const auto& byte : *card_uid) {
            if (!byte.is_number()) {
                readable = false;
                // If any byte isn't a number, bail out — partial UIDs aren't
                // safe to compare. Leave info.uid empty so the check is a
                // no-op for this parse.
                uid.clear();
                break;
            }
            const std::int64_t value = json_util::as_int64(byte);
            has_card = has_card || value != 0;
            if (!uid.empty())
                uid.push_back(',');
            uid += std::to_string(value);
        }
        if (readable) {
            info.has_card = has_card;
        }
        info.uid = std::move(uid);
    } else if (card_uid && card_uid->is_number()) {
        info.has_card = json_util::as_double(*card_uid) != 0.0;
    }

    return info;
}

FilamentDetectDelta parse_filament_detect(const nlohmann::json& detect) {
    FilamentDetectDelta d;
    d.info = ams::read_indexed<SnapmakerRfidInfo, kToolCount>(
        detect, "info", [](const nlohmann::json& entry) -> std::optional<SnapmakerRfidInfo> {
            if (!entry.is_object()) {
                return std::nullopt;
            }
            return parse_rfid_info(entry);
        });
    d.state = ams::read_indexed<int, kToolCount>(detect, "state");
    return d;
}

std::optional<uint32_t> rgb_from_rgba_hex(const std::string& hex) {
    // Only an Observed reading is a colour; an empty or malformed value leaves
    // the slot's colour as it was.
    const auto reading = ams::read_lane_color(hex);
    if (reading.kind != ams::ColorReadingKind::Observed) {
        return std::nullopt;
    }
    return reading.rgb;
}

std::vector<ToolheadSensorDelta> parse_toolhead_sensors(const nlohmann::json& status) {
    std::vector<ToolheadSensorDelta> sensors;
    // Match both prefixes (motion is the Snapmaker default; switch is the
    // generic fallback) and any "e{N}_filament" / "e{N}" sensor name suffix.
    constexpr std::string_view motion_prefix = "filament_motion_sensor ";
    constexpr std::string_view switch_prefix = "filament_switch_sensor ";
    for (auto it = status.begin(); it != status.end(); ++it) {
        const std::string& key = it.key();
        std::string_view sensor_name;
        if (key.compare(0, motion_prefix.size(), motion_prefix) == 0) {
            sensor_name = std::string_view(key).substr(motion_prefix.size());
        } else if (key.compare(0, switch_prefix.size(), switch_prefix) == 0) {
            sensor_name = std::string_view(key).substr(switch_prefix.size());
        } else {
            continue;
        }
        // Anything but "e{N}_filament" or "e{N}" (toolhead_sensor,
        // bypass_sensor, custom names) is unrelated to per-tool runout.
        if (sensor_name.size() < 2 || sensor_name[0] != 'e') {
            continue;
        }
        size_t digit_end = 1;
        while (digit_end < sensor_name.size() &&
               std::isdigit(static_cast<unsigned char>(sensor_name[digit_end]))) {
            ++digit_end;
        }
        if (digit_end == 1) {
            continue; // no digits
        }
        const auto tool = text_io::parse_leading<int>(sensor_name.substr(1, digit_end - 1));
        if (!tool || *tool < 0 || *tool >= kToolCount || !it.value().is_object()) {
            continue;
        }
        ToolheadSensorDelta d;
        d.tool = *tool;
        d.object = key;
        d.enabled = ams::read_field<bool>(it.value(), "enabled");
        d.filament_detected = ams::read_field<bool>(it.value(), "filament_detected");
        sensors.push_back(std::move(d));
    }
    return sensors;
}

PrintTaskConfigDelta parse_print_task_config(const nlohmann::json& status) {
    PrintTaskConfigDelta d;
    d.preferences = read_print_preferences(status);

    const auto ptc_it = status.find("print_task_config");
    if (ptc_it == status.end() || !ptc_it->is_object()) {
        return d;
    }
    const auto& ptc = *ptc_it;

    if (ptc.contains("extruder_map_table") && ptc["extruder_map_table"].is_array()) {
        std::vector<int> table;
        table.reserve(ptc["extruder_map_table"].size());
        for (const auto& entry : ptc["extruder_map_table"]) {
            if (!entry.is_number_integer()) {
                table.push_back(-1);
                continue;
            }
            const int head = entry.get<int>();
            table.push_back((head >= 0 && head < kToolCount) ? head : -1);
        }
        d.extruder_map_table = std::move(table);
    }

    if (ptc.contains("extruders_used") && ptc["extruders_used"].is_array()) {
        std::vector<bool> used;
        used.reserve(ptc["extruders_used"].size());
        for (const auto& entry : ptc["extruders_used"]) {
            used.push_back(entry.is_boolean() && entry.get<bool>());
        }
        d.extruders_used = std::move(used);
    }

    d.filament_exist = ams::read_indexed<bool, kToolCount>(ptc, "filament_exist");
    d.filament_type = ams::read_indexed<std::string, kToolCount>(ptc, "filament_type");
    d.filament_vendor = ams::read_indexed<std::string, kToolCount>(ptc, "filament_vendor");
    d.filament_sub_type = ams::read_indexed<std::string, kToolCount>(ptc, "filament_sub_type");
    d.filament_official = ams::read_indexed<bool, kToolCount>(ptc, "filament_official");
    d.filament_color_rgba = ams::read_indexed<std::string, kToolCount>(ptc, "filament_color_rgba");
    return d;
}

std::vector<FeedChannelDelta> parse_feed_channels(const nlohmann::json& status) {
    std::vector<FeedChannelDelta> channels;
    for (const char* feed_key : {"filament_feed left", "filament_feed right"}) {
        if (!status.contains(feed_key) || !status[feed_key].is_object()) {
            continue;
        }
        const auto& feed = status[feed_key];
        for (int lane = 0; lane < kToolCount; lane++) {
            const std::string ext_key = "extruder" + std::to_string(lane);
            if (!feed.contains(ext_key) || !feed[ext_key].is_object()) {
                continue;
            }
            const auto& ch = feed[ext_key];
            FeedChannelDelta d;
            d.lane = lane;
            // Klipper publishes null for these before a sensor's first
            // reading, which reads as absent rather than as false/empty: a
            // frame that said nothing about filament must not clear the port
            // sensor and drop the slot to EMPTY.
            d.filament_detected = ams::read_field<bool>(ch, "filament_detected");
            d.channel_state = ams::read_field<std::string>(ch, "channel_state");
            d.channel_action_state = ams::read_field<std::string>(ch, "channel_action_state");
            d.channel_error = ams::read_field<std::string>(ch, "channel_error");
            d.module_exist = ams::read_field<bool>(ch, "module_exist");
            d.disable_auto = ams::read_field<bool>(ch, "disable_auto");
            channels.push_back(std::move(d));
        }
    }
    return channels;
}

StatusDelta parse_status(const nlohmann::json& status, const std::string& batch_macro_object) {
    StatusDelta d;

    // Klipper names the first extruder "extruder" and the rest "extruder1"...
    static const char* const extruder_keys[kToolCount] = {"extruder", "extruder1", "extruder2",
                                                          "extruder3"};
    for (int i = 0; i < kToolCount; i++) {
        if (status.contains(extruder_keys[i]) && status[extruder_keys[i]].is_object()) {
            d.extruders[i] = parse_extruder_delta(status[extruder_keys[i]]);
        }
    }

    if (status.contains("toolhead") && status["toolhead"].is_object()) {
        const auto& th = status["toolhead"];
        if (auto v = ams::read_field<std::string>(th, "extruder")) {
            d.toolhead_extruder = std::move(*v);
        }
    }

    if (status.contains("filament_detect") && status["filament_detect"].is_object()) {
        d.filament_detect = parse_filament_detect(status["filament_detect"]);
    }

    d.feed_channels = parse_feed_channels(status);

    d.toolhead_sensors = parse_toolhead_sensors(status);

    if (status.contains("print_task_config") && status["print_task_config"].is_object()) {
        d.print_task_config = parse_print_task_config(status);
    }

    if (!batch_macro_object.empty()) {
        const auto macro = status.find(batch_macro_object);
        if (macro != status.end() && macro->is_object()) {
            d.batch_doing = ams::read_field<bool>(*macro, "doing");
        }
    }

    return d;
}

} // namespace snapmaker
} // namespace helix
