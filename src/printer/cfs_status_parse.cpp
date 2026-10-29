// src/printer/cfs_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cfs_status_parse.h"

#include "ams_status_json.h"
#include "json_utils.h"

namespace helix::cfs {

BoxFrameShape classify_box_frame(const nlohmann::json& box) {
    BoxFrameShape shape;
    shape.has_top_level = box.contains("filament") || box.contains("map") ||
                          box.contains("auto_refill") || box.contains("state");
    shape.has_unit_data =
        box.contains("T1") || box.contains("T2") || box.contains("T3") || box.contains("T4");
    return shape;
}

std::optional<FilamentSensorDelta> parse_filament_sensor(const nlohmann::json& params) {
    const auto it = params.find("filament_switch_sensor filament_sensor");
    if (it == params.end()) {
        return std::nullopt;
    }
    FilamentSensorDelta d;
    if (it->is_object()) {
        d.filament_detected = ams::read_field<bool>(*it, "filament_detected");
    }
    return d;
}

std::optional<ExtruderTempDelta> parse_extruder(const nlohmann::json& params) {
    const auto it = params.find("extruder");
    if (it == params.end()) {
        return std::nullopt;
    }
    ExtruderTempDelta d;
    if (it->is_object()) {
        d.target_c = ams::read_field<double>(*it, "target");
        d.temperature_c = ams::read_field<double>(*it, "temperature");
    }
    return d;
}

std::optional<MotorControlDelta> parse_motor_control(const nlohmann::json& params) {
    const auto it = params.find("motor_control");
    if (it == params.end()) {
        return std::nullopt;
    }
    MotorControlDelta d;
    if (it->is_object()) {
        d.motor_ready = ams::read_field<bool>(*it, "motor_ready");
    }
    return d;
}

bool has_registered_unit(const nlohmann::json& configfile_settings) {
    const nlohmann::json* section = json_util::find_member(configfile_settings, "auto_addr");
    const nlohmann::json* option =
        section ? json_util::find_member(*section, "mb_addr_table_uniids") : nullptr;
    if (!option) {
        return false;
    }
    const nlohmann::json table =
        option->is_string()
            ? nlohmann::json::parse(option->get_ref<const std::string&>(), nullptr, false)
            : *option;
    if (!table.is_array()) {
        return false;
    }
    for (const auto& entry : table) {
        if (!entry.is_array()) {
            continue;
        }
        for (const auto& byte : entry) {
            if (json_util::as_int64(byte) != 0) {
                return true;
            }
        }
    }
    return false;
}

} // namespace helix::cfs
