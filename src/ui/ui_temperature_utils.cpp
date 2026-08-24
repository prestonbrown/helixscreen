// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_temperature_utils.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "spdlog/spdlog.h"
#include "theme_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace helix {
namespace ui {
namespace temperature {

bool validate_and_clamp(int& temp, int min_temp, int max_temp, const char* context,
                        const char* temp_type) {
    if (temp < min_temp || temp > max_temp) {
        spdlog::warn("[{}] Invalid {} temperature {}°C (valid: {}-{}°C), clamping", context,
                     temp_type, temp, min_temp, max_temp);
        temp = (temp < min_temp) ? min_temp : max_temp;
        return false;
    }
    return true;
}

bool validate_and_clamp_pair(int& current, int& target, int min_temp, int max_temp,
                             const char* context) {
    bool current_valid = validate_and_clamp(current, min_temp, max_temp, context, "current");
    bool target_valid = validate_and_clamp(target, min_temp, max_temp, context, "target");
    return current_valid && target_valid;
}

bool is_extrusion_safe(int current_temp, int min_extrusion_temp) {
    return current_temp >= min_extrusion_temp;
}

const char* get_extrusion_safety_status(int current_temp, int min_extrusion_temp) {
    if (current_temp >= min_extrusion_temp) {
        return lv_tr("Ready");
    }

    // Calculate how far below minimum we are
    static char status_buf[64];
    int deficit = min_extrusion_temp - current_temp;
    snprintf(status_buf, sizeof(status_buf), lv_tr("Heating (%d°C below minimum)"), deficit);
    return status_buf;
}

// ============================================================================
// Formatting Functions
// ============================================================================

char* format_temperature(int temp, char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "%d°C", temp);
    return buffer;
}

char* format_temperature_pair(int current, int target, char* buffer, size_t buffer_size) {
    if (target == 0) {
        snprintf(buffer, buffer_size, "%d / —°C", current);
    } else {
        snprintf(buffer, buffer_size, "%d / %d°C", current, target);
    }
    return buffer;
}

// Format a temperature number with one decimal place, dropping the decimal when
// the integer portion is 3 digits or more (>= 100) to keep wide values compact.
char* format_temp_number(float temp, char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, temp >= 100.0f ? "%.0f" : "%.1f", temp);
    return buffer;
}

char* format_temperature_f(float temp, char* buffer, size_t buffer_size) {
    format_temp_number(temp, buffer, buffer_size);
    size_t len = strlen(buffer);
    snprintf(buffer + len, buffer_size - len, "°C");
    return buffer;
}

char* format_temperature_pair_f(float current, float target, char* buffer, size_t buffer_size) {
    char current_buf[16];
    format_temp_number(current, current_buf, sizeof(current_buf));
    if (target == 0.0f) {
        snprintf(buffer, buffer_size, "%s / —°C", current_buf);
    } else {
        char target_buf[16];
        format_temp_number(target, target_buf, sizeof(target_buf));
        snprintf(buffer, buffer_size, "%s / %s°C", current_buf, target_buf);
    }
    return buffer;
}

char* format_target_or_off(int target, char* buffer, size_t buffer_size) {
    if (target == 0) {
        snprintf(buffer, buffer_size, "— °C");
    } else {
        snprintf(buffer, buffer_size, "%d°C", target);
    }
    return buffer;
}

char* format_temperature_range(int min_temp, int max_temp, char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "%d-%d°C", min_temp, max_temp);
    return buffer;
}

// ============================================================================
// Display Color Functions
// ============================================================================

HeatState classify_heat_state(int current, int target, int tolerance) {
    if (target <= 0) {
        return HeatState::Off;
    } else if (current < target - tolerance) {
        return HeatState::Heating;
    } else if (current > target + tolerance) {
        return HeatState::Cooling;
    }
    return HeatState::AtTemp;
}

HeatState classify_heat_state_with_mode(int current, int target, helix::ChamberMode mode,
                                        int tolerance) {
    if (mode == helix::ChamberMode::Maintaining) {
        // Target is a cooling ceiling, not a heat goal — Heating never applies.
        if (current > target + tolerance) {
            return HeatState::Cooling;
        }
        return HeatState::Neutral;
    }
    // Off / Heating: target is a genuine heat goal, so the plain classifier applies.
    return classify_heat_state(current, target, tolerance);
}

lv_color_t get_heating_state_color(HeatState state) {
    switch (state) {
    case HeatState::Off:
        return theme_manager_get_color("text_muted");
    case HeatState::Heating:
        return theme_manager_get_color("danger");
    case HeatState::Cooling:
        return theme_manager_get_color("info");
    case HeatState::Neutral:
        return theme_manager_get_color("text");
    case HeatState::AtTemp:
        break;
    }
    return theme_manager_get_color("success");
}

lv_color_t get_heating_state_color(int current_deg, int target_deg, int tolerance) {
    return get_heating_state_color(classify_heat_state(current_deg, target_deg, tolerance));
}

const char* get_heating_state_variant(int current_deg, int target_deg, int tolerance) {
    switch (classify_heat_state(current_deg, target_deg, tolerance)) {
    case HeatState::Off:
        return "muted";
    case HeatState::Heating:
        return "danger";
    case HeatState::Cooling:
        return "info";
    case HeatState::Neutral:
        // classify_heat_state() (mode-unaware) never returns Neutral — only
        // classify_heat_state_with_mode() does. Kept for switch exhaustiveness.
        return "text";
    case HeatState::AtTemp:
        break;
    }
    return "success";
}

// ============================================================================
// Heater Display
// ============================================================================

HeaterDisplayResult heater_display(int current_deci, int target_deci) {
    HeaterDisplayResult result;

    // The reading as the UI renders it. Everything below - the string, the
    // status word, the color - derives from this one value, so a card cannot
    // print "223 / 220" and call itself Ready at the same time.
    const int shown_deci = displayed_deci(current_deci);
    int current_deg = deci_to_degrees(shown_deci);
    int target_deg = deci_to_degrees(target_deci);

    // Format temperature string
    char buf[32];
    if (target_deci > 0) {
        std::snprintf(buf, sizeof(buf), "%d / %d°C", current_deg, target_deg);
    } else {
        std::snprintf(buf, sizeof(buf), "%d°C", current_deg);
    }
    result.temp = buf;

    // Calculate percentage (clamped to 0-100)
    if (target_deci <= 0) {
        result.pct = 0;
    } else {
        int pct = (current_deci * 100) / target_deci;
        result.pct = std::clamp(pct, 0, 100);
    }

    // Determine status using shared tolerance constant. Classified in
    // decidegrees against the displayed reading so the word matches both the
    // string above and the temp_display card showing the same heater.
    const HeatState state =
        classify_heat_state(shown_deci, target_deci, DEFAULT_AT_TEMP_TOLERANCE_DECI);
    switch (state) {
    case HeatState::Off:
        result.status = lv_tr("Off");
        break;
    case HeatState::Heating:
        result.status = lv_tr("Heating...");
        break;
    case HeatState::Cooling:
        result.status = lv_tr("Cooling");
        break;
    case HeatState::Neutral:
        // classify_heat_state() (mode-unaware) never returns Neutral.
    case HeatState::AtTemp:
        result.status = lv_tr("Ready");
        break;
    }

    // Get color from the same heating state logic
    result.color = get_heating_state_color(state);

    return result;
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
                       bool m141_available) {
    return m141_available && !heater_full_name.empty() && !chamber_heater_name.empty() &&
           heater_full_name == chamber_heater_name;
}

ChamberSetpoint chamber_effective_setpoint(int heater_target_deci, int fan_target_deci,
                                           int fan_resting_deci) {
    // Mirrors the live computation in PrinterTemperatureState::update_chamber_setpoint()
    // exactly: heater wins; fan wins only when it is above 0 and not at the
    // configured resting target (which M141 S0 parks the fan at on the K2).
    if (heater_target_deci > 0)
        return {heater_target_deci, helix::ChamberMode::Heating};
    if (fan_target_deci > 0 && fan_target_deci != fan_resting_deci)
        return {fan_target_deci, helix::ChamberMode::Maintaining};
    return {0, helix::ChamberMode::Off};
}

const char* chamber_mode_word(helix::ChamberMode mode) {
    switch (mode) {
    case helix::ChamberMode::Heating:
        return "Heating";
    case helix::ChamberMode::Maintaining:
        return "Maintaining";
    default:
        return "Off";
    }
}

std::string chamber_status_text(int current_deci, int target_deci, helix::ChamberMode mode) {
    // Resolve the mode word (untranslated key), then localise at the call site.
    std::string mode_str = lv_tr(chamber_mode_word(mode));

    // Append thermal progress ("Ready" / "Cooling") only when it adds information
    // beyond the mode word.  Suppress "Heating · Heating..." and the Off cases.
    auto result = heater_display(current_deci, target_deci);
    const std::string& progress = result.status; // already localised
    if (target_deci <= 0 || progress == std::string(lv_tr("Heating...")) ||
        progress == std::string(lv_tr("Off"))) {
        return mode_str;
    }
    return mode_str + " \xc2\xb7 " + progress; // " · " UTF-8 middle dot
}

} // namespace temperature
} // namespace ui
} // namespace helix
