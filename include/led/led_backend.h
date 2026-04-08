// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace helix::led {

enum class LedBackendType { NATIVE, LED_EFFECT, WLED, MACRO, OUTPUT_PIN };

struct LedStripInfo {
    std::string name; // Display name
    std::string id;   // Klipper/Moonraker ID (e.g., "neopixel chamber_light")
    LedBackendType backend;
    bool supports_color; // RGB/RGBW capable
    bool supports_white; // Has W channel (RGBW)
    bool is_pwm = false; // PWM capable (output_pin with pwm:true)

    // Per-channel capability (set from configfile for generic [led] sections).
    // When all false, capabilities are inferred from the LED type prefix.
    bool has_red_pin = false;
    bool has_green_pin = false;
    bool has_blue_pin = false;
    bool has_white_pin = false;
    bool pin_config_known = false; // True if configfile was parsed for this strip
};

struct LedEffectInfo {
    std::string name;         // Klipper config name (e.g., "led_effect breathing")
    std::string display_name; // Human-friendly (e.g., "Breathing")
    std::string icon_hint;    // Icon name for card (e.g., "air", "local_fire_department")
    std::vector<std::string>
        target_leds; // Strip IDs this effect targets (e.g., "neopixel chamber_light")
    bool enabled =
        false; // Whether this effect is currently active (tracked via Moonraker subscription)
};

enum class MacroLedType { ON_OFF, TOGGLE, PRESET };

struct LedMacroInfo {
    std::string display_name;                 // User-friendly label
    MacroLedType type = MacroLedType::TOGGLE; // Control style
    std::string on_macro;                     // ON_OFF type: gcode to turn on
    std::string off_macro;                    // ON_OFF type: gcode to turn off
    std::string toggle_macro;                 // TOGGLE type: single toggle macro
    std::vector<std::string> presets;         // PRESET type: Klipper macro names
};

/// WLED preset info fetched from device
struct WledPresetInfo {
    int id = -1;
    std::string name;
};

/// WLED strip runtime state (from Moonraker status polling)
struct WledStripState {
    bool is_on = false;
    int brightness = 255;   // 0-255
    int active_preset = -1; // -1 = no preset active
};

/// Pretty-print a Klipper macro name for display.
/// Strips common prefixes (LED_, LIGHT_, STATUS_LED_), replaces underscores
/// with spaces, and title-cases each word.
/// Example: "LED_PARTY_MODE" -> "Party Mode"
inline std::string pretty_print_macro(const std::string& macro_name) {
    std::string s = macro_name;

    // Strip common prefixes (longest first)
    static const std::string prefixes[] = {"STATUS_LED_", "LIGHT_", "LED_"};
    for (const auto& prefix : prefixes) {
        if (s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0) {
            s = s.substr(prefix.size());
            break;
        }
    }

    // Replace underscores with spaces and title-case
    bool capitalize_next = true;
    for (auto& ch : s) {
        if (ch == '_') {
            ch = ' ';
            capitalize_next = true;
        } else if (capitalize_next) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            capitalize_next = false;
        } else {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }

    return s;
}

} // namespace helix::led
