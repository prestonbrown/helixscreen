// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_temperature_utils.h"

#include "app_globals.h"
#include "config.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_types.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "spdlog/spdlog.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

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

bool active_nozzle_ready_for_extrusion(const SafetyLimits& limits) {
    // Users whose macros heat the nozzle themselves, or who are deliberately
    // cold-pulling, opt out of this gate everywhere at once.
    if (helix::SafetySettingsManager::instance().get_allow_cold_extrude()) {
        return true;
    }
    auto* subj = get_printer_state().temperature_state().get_active_extruder_temp_subject();
    const int current = subj ? deci_to_degrees(lv_subject_get_int(subj)) : 0;
    return is_extrusion_safe(current, extrusion_floor_c(limits));
}

int extrusion_floor_c(const SafetyLimits& limits, const std::string& extruder) {
    return static_cast<int>(std::ceil(limits.min_extrude_temp_for(extruder)));
}

int nozzle_max_temp_c(const SafetyLimits& limits, const std::string& extruder) {
    return static_cast<int>(std::floor(limits.max_temp_for(extruder)));
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

char* format_temperature_range(int min_temp, int max_temp, char* buffer, size_t buffer_size,
                               bool with_unit) {
    if (min_temp > max_temp && max_temp > 0) {
        std::swap(min_temp, max_temp);
    }
    if (max_temp <= 0) {
        max_temp = min_temp; // only the minimum is set
    }
    if (min_temp == max_temp || min_temp <= 0) {
        snprintf(buffer, buffer_size, with_unit ? "%d°C" : "%d", max_temp);
    } else {
        // Translated for the separator: Japanese writes a range with "〜".
        snprintf(buffer, buffer_size, with_unit ? lv_tr("%d-%d°C") : lv_tr("%d-%d"), min_temp,
                 max_temp);
    }
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

bool is_residual_hot(int current_deci) {
    return current_deci > RESIDUAL_HEAT_THRESHOLD_DECI;
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
    // decidegrees against the displayed reading so the glyph matches both the
    // string above and the temp_display card showing the same heater.
    const HeatState state =
        classify_heat_state(shown_deci, target_deci, DEFAULT_AT_TEMP_TOLERANCE_DECI);
    result.state = state;

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

// Map a thermal classification onto the status-area states. Chamber Neutral
// (maintaining at or below the cooling ceiling) is the "holding, satisfied"
// answer, so it reads as Ready rather than None.
static HeaterStatusState status_state_of(HeatState heat) {
    switch (heat) {
    case HeatState::Heating:
        return HeaterStatusState::Heating;
    case HeatState::AtTemp:
    case HeatState::Neutral:
        return HeaterStatusState::Ready;
    case HeatState::Cooling:
        return HeaterStatusState::Cooling;
    case HeatState::Off:
        break;
    }
    return HeaterStatusState::None;
}

HeaterStatus classify_heater_status(int current_deci, int target_deci, int power_pct,
                                    helix::ChamberMode mode) {
    HeaterStatus status;
    status.state = status_state_of(classify_heat_state_with_mode(
        displayed_deci(current_deci), target_deci, mode, DEFAULT_AT_TEMP_TOLERANCE_DECI));
    if (power_pct > 0) {
        status.duty = std::to_string(power_pct) + "%";
    }
    return status;
}

std::string heater_keypad_title(HeaterType type) {
    switch (type) {
    case HeaterType::Nozzle:
        return ToolState::instance().nozzle_label();
    case HeaterType::Bed:
        return lv_tr("Bed");
    case HeaterType::Chamber:
        return lv_tr("Chamber");
    }
    return {};
}

std::string build_cooldown_gcode(const std::string& macro_gcode,
                                 const std::vector<std::string>& extruder_names,
                                 const std::string& chamber_heater_name) {
    if (!helix::is_default_cooldown_gcode(macro_gcode)) {
        return macro_gcode;
    }

    // The default macro already names the primary extruder.
    std::vector<std::string> extra_extruders;
    for (const auto& name : extruder_names) {
        if (name != "extruder") {
            extra_extruders.push_back(name);
        }
    }
    // Shorter first, so extruder2 precedes extruder10.
    std::sort(extra_extruders.begin(), extra_extruders.end(),
              [](const std::string& a, const std::string& b) {
                  return a.size() != b.size() ? a.size() < b.size() : a < b;
              });
    extra_extruders.erase(std::unique(extra_extruders.begin(), extra_extruders.end()),
                          extra_extruders.end());

    std::string gcode = macro_gcode;
    char line[128];
    auto append_off = [&](const std::string& heater) {
        if (build_heater_off_gcode(heater, line, sizeof(line))) {
            gcode += "\n";
            gcode += line;
        }
    };
    for (const auto& name : extra_extruders) {
        append_off(name);
    }
    append_off(chamber_heater_name);
    return gcode;
}

std::string resolve_cooldown_gcode(const helix::PrinterTemperatureState& temps) {
    auto* cfg = helix::Config::get_instance();
    helix::MacroConfig default_cooldown{"Cool Down", helix::kDefaultCooldownGcode};
    const auto cooldown = cfg->get_macro("cooldown", default_cooldown);

    std::vector<std::string> extruder_names;
    extruder_names.reserve(temps.extruders().size());
    for (const auto& [name, info] : temps.extruders()) {
        extruder_names.push_back(name);
    }
    return build_cooldown_gcode(cooldown.gcode, extruder_names, temps.chamber_heater_name());
}

} // namespace temperature
} // namespace ui
} // namespace helix
