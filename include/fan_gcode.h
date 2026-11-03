// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace helix {

/// Whether an output_pin fan's name (without "output_pin ") marks a hotend,
/// extruder or heatbreak fan. The firmware drives those with the heater; the
/// screen shows them read-only and never commands them.
inline bool output_pin_fan_is_firmware_driven(std::string_view short_name) {
    std::string lower(short_name);
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower.find("hotend") != std::string::npos ||
           lower.find("extruder") != std::string::npos ||
           lower.find("heatbreak") != std::string::npos;
}

/// Generate the gcode command to set a fan's speed.
/// @param fan Full Moonraker object name (e.g., "fan", "output_pin fan0", "fan_generic aux")
/// @param speed_percent 0.0-100.0
/// @return Gcode string ready for execute_gcode(); empty for a fan the screen
///         must not command (output_pin_fan_is_firmware_driven)
inline std::string fan_gcode(const std::string& fan, double speed_percent) {
    int s_value = static_cast<int>(std::round(speed_percent * 255.0 / 100.0));

    // Canonical Klipper [fan] section
    if (fan == "fan") {
        if (s_value == 0) {
            return "M107";
        }
        return "M106 S" + std::to_string(s_value);
    }

    // output_pin fan objects (Creality-style)
    if (fan.rfind("output_pin ", 0) == 0) {
        std::string short_name = fan.substr(11);

        // output_pin fanN -> M106 P<N> S<value>
        if (short_name.rfind("fan", 0) == 0 && short_name.size() > 3) {
            std::string index_str = short_name.substr(3);
            // Verify it's a number
            bool is_numeric = !index_str.empty();
            for (char c : index_str) {
                if (!std::isdigit(c)) {
                    is_numeric = false;
                    break;
                }
            }
            if (is_numeric) {
                if (s_value == 0) {
                    return "M107 P" + index_str;
                }
                return "M106 P" + index_str + " S" + std::to_string(s_value);
            }
        }

        if (output_pin_fan_is_firmware_driven(short_name)) {
            return {};
        }
        // Non-numeric output_pin fan or non-fan output_pin: SET_PIN
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << "SET_PIN PIN=" << short_name
           << " VALUE=" << (speed_percent / 100.0);
        return ss.str();
    }

    // All other fan types: SET_FAN_SPEED
    std::string fan_name = fan;
    size_t space_pos = fan_name.find(' ');
    if (space_pos != std::string::npos) {
        fan_name = fan_name.substr(space_pos + 1);
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << "SET_FAN_SPEED FAN=" << fan_name
       << " SPEED=" << (speed_percent / 100.0);
    return ss.str();
}

} // namespace helix
