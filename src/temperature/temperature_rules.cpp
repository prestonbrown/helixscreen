// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_rules.h"

#include "core/temperature_units.h"

#include <cstdio>
#include <cstring>

namespace helix::temperature_rules {

// Format a temperature number with one decimal place, dropping the decimal when
// the integer portion is 3 digits or more (>= 100) to keep wide values compact.
// The rule applies to the number as shown, so 40°C reads "104" in °F.
char* format_number_compact(double value, char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, value >= 100.0 ? "%.0f" : "%.1f", value);
    return buffer;
}

char* format_temp_number(float temp, char* buffer, size_t buffer_size) {
    return format_number_compact(units::to_display_temp(temp), buffer, buffer_size);
}

char* format_temperature_f(float temp, char* buffer, size_t buffer_size) {
    format_temp_number(temp, buffer, buffer_size);
    size_t len = strlen(buffer);
    snprintf(buffer + len, buffer_size - len, "%s", units::display_temp_symbol());
    return buffer;
}

// Used by cooldown's multi-line gcode batch and IMoonrakerAPI::set_temperature().
const char* build_heater_gcode(const std::string& heater_full_name, int target_deci, char* buffer,
                               size_t buffer_size, bool use_m141) {
    if (heater_full_name.empty()) {
        return nullptr;
    }

    if (use_m141) {
        std::snprintf(buffer, buffer_size, "M141 S%d", deci_to_degrees(target_deci));
        return buffer;
    }

    if (heater_full_name.rfind("temperature_fan ", 0) == 0) {
        std::string fan_name = heater_full_name.substr(16);
        std::snprintf(buffer, buffer_size,
                      "SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=%s TARGET=%d", fan_name.c_str(),
                      deci_to_degrees(target_deci));
    } else if (heater_full_name.rfind("heater_generic ", 0) == 0) {
        std::string object_name = heater_full_name.substr(15);
        std::snprintf(buffer, buffer_size, "SET_HEATER_TEMPERATURE HEATER=%s TARGET=%d",
                      object_name.c_str(), deci_to_degrees(target_deci));
    } else {
        // Bare heater names (extruder, heater_bed, etc.)
        std::snprintf(buffer, buffer_size, "SET_HEATER_TEMPERATURE HEATER=%s TARGET=%d",
                      heater_full_name.c_str(), deci_to_degrees(target_deci));
    }

    return buffer;
}

bool chamber_uses_m141(const std::string& heater_full_name, const std::string& chamber_heater_name,
                       bool m141_available, std::optional<bool> m141_sets_heater) {
    return m141_available && !heater_full_name.empty() && !chamber_heater_name.empty() &&
           heater_full_name == chamber_heater_name &&
           m141_sets_heater != std::optional<bool>(false);
}

ChamberSetpoint chamber_effective_setpoint(int heater_target_deci, int fan_target_deci,
                                           int fan_resting_deci) {
    // The one rule for the effective setpoint, applied by
    // PrinterTemperatureState on every status frame:
    // heater wins; the fan target is always the effective setpoint when
    // it holds one, and is labeled Resting when it equals the configured resting
    // target — the state M141 S0 parks the fan at on the K2 and that a deliberate
    // set equal to resting is byte-identical to. The number is displayed either
    // way; Resting only changes which glyph sits beside it.
    if (heater_target_deci > 0)
        return {heater_target_deci, helix::ChamberMode::Heating};
    if (fan_target_deci > 0) {
        return {fan_target_deci, fan_target_deci == fan_resting_deci
                                     ? helix::ChamberMode::Resting
                                     : helix::ChamberMode::Maintaining};
    }
    return {0, helix::ChamberMode::Off};
}

} // namespace helix::temperature_rules
