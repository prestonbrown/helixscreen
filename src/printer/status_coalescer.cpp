// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "status_coalescer.h"

#include <string>
#include <utility>

namespace helix {

namespace {

struct TelemetryRow {
    std::string_view prefix;
    std::string_view attribute;
};

// Attributes that change on almost every report and drive no decision a sleeping screen
// has to make at once. Grows only by measurement; anything absent is applied at once.
constexpr TelemetryRow kTelemetry[] = {
    {"extruder", "temperature"},
    {"extruder", "power"},
    {"heater_bed", "temperature"},
    {"heater_bed", "power"},
    {"heater_generic", "temperature"},
    {"heater_generic", "power"},
    {"temperature_sensor", "temperature"},
    {"temperature_fan", "temperature"},
    {"temperature_fan", "speed"},
    {"fan", "speed"},
    {"fan", "rpm"},
    {"heater_fan", "speed"},
    {"heater_fan", "rpm"},
    {"controller_fan", "speed"},
    {"controller_fan", "rpm"},
    {"fan_generic", "speed"},
    {"fan_generic", "rpm"},
    {"motion_report", "live_position"},
    {"motion_report", "live_velocity"},
    {"motion_report", "live_extruder_velocity"},
    {"toolhead", "position"},
    {"toolhead", "print_time"},
    {"toolhead", "estimated_print_time"},
    {"gcode_move", "gcode_position"},
    {"gcode_move", "position"},
    {"gcode_move", "speed"},
    {"print_stats", "print_duration"},
    {"print_stats", "total_duration"},
    {"print_stats", "filament_used"},
    {"virtual_sdcard", "progress"},
    {"virtual_sdcard", "file_position"},
    {"display_status", "progress"},
    {"idle_timeout", "printing_time"},
    {"system_stats", "sysload"},
    {"system_stats", "cputime"},
    {"system_stats", "memavail"},
};

/// The Klipper object type a status key names: the part before any space, without the
/// index a second extruder carries ("extruder1" -> "extruder").
std::string_view object_prefix(std::string_view object_name) {
    std::string_view type = object_name.substr(0, object_name.find(' '));
    while (!type.empty() && type.back() >= '0' && type.back() <= '9') {
        type.remove_suffix(1);
    }
    return type;
}

bool is_telemetry(std::string_view prefix, std::string_view attribute) {
    for (const TelemetryRow& row : kTelemetry) {
        if (row.prefix == prefix && row.attribute == attribute) {
            return true;
        }
    }
    return false;
}

} // namespace

bool status_is_telemetry_only(const nlohmann::json& status) {
    if (!status.is_object() || status.empty()) {
        return false;
    }
    for (const auto& [object_name, fields] : status.items()) {
        if (!fields.is_object() || fields.empty()) {
            return false;
        }
        const std::string_view prefix = object_prefix(object_name);
        for (const auto& field : fields.items()) {
            if (!is_telemetry(prefix, field.key())) {
                return false;
            }
        }
    }
    return true;
}

const std::vector<std::string_view>& telemetry_attributes() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const TelemetryRow& row : kTelemetry) {
            out.push_back(std::string(row.prefix) + "." + std::string(row.attribute));
        }
        return out;
    }();
    static const std::vector<std::string_view> views(names.begin(), names.end());
    return views;
}

void StatusCoalescer::hold(const nlohmann::json& status, double eventtime, uint64_t klippy_epoch,
                           uint32_t now_ms) {
    if (!m_held) {
        m_held = Held{nlohmann::json::object(), eventtime, klippy_epoch};
        m_first_hold_ms = now_ms;
    }
    for (const auto& [object_name, fields] : status.items()) {
        nlohmann::json& held_fields = m_held->status[object_name];
        for (const auto& field : fields.items()) {
            held_fields[field.key()] = field.value();
        }
    }
    if (eventtime > m_held->eventtime) {
        m_held->eventtime = eventtime;
    }
    m_held->klippy_epoch = klippy_epoch;
}

bool StatusCoalescer::due(uint32_t now_ms, uint32_t interval_ms) const {
    return m_held && static_cast<uint32_t>(now_ms - m_first_hold_ms) >= interval_ms;
}

std::optional<StatusCoalescer::Held> StatusCoalescer::take() {
    std::optional<Held> out = std::move(m_held);
    m_held.reset();
    return out;
}

} // namespace helix
