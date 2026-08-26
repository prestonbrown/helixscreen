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

/// Find a strip by its Klipper/Moonraker ID. Returns nullptr when absent.
/// Backends hold a handful of strips each, so the linear scan is the right
/// shape — this exists so call sites stop hand-rolling the loop.
inline const LedStripInfo* find_strip(const std::vector<LedStripInfo>& strips,
                                      const std::string& id) {
    for (const auto& s : strips) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

/// Mutable overload for the backends that patch capability flags in place.
inline LedStripInfo* find_strip(std::vector<LedStripInfo>& strips, const std::string& id) {
    for (auto& s : strips) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

/// Macro devices are addressed by a synthetic "macro:<display name>" strip ID
/// so they can share the selection list with real strips.
constexpr const char* MACRO_STRIP_PREFIX = "macro:";

/// True when a strip ID refers to a macro device.
inline bool is_macro_strip_id(const std::string& id) {
    return id.rfind(MACRO_STRIP_PREFIX, 0) == 0;
}

/// Strip the "macro:" prefix from a strip ID, yielding the raw macro display
/// name. IDs without the prefix are returned unchanged.
inline std::string strip_macro_name(const std::string& id) {
    constexpr size_t prefix_len = 6; // strlen("macro:")
    return is_macro_strip_id(id) ? id.substr(prefix_len) : id;
}

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

/// Find a configured macro device by its display name. Returns nullptr when
/// absent. Accepts either the raw display name or a "macro:"-prefixed strip ID.
inline const LedMacroInfo* find_macro(const std::vector<LedMacroInfo>& macros,
                                      const std::string& name_or_id) {
    const std::string name = strip_macro_name(name_or_id);
    // A bare "macro:" ID would otherwise match an unnamed draft device, handing
    // the caller an entry whose gcode fields are all empty.
    if (name.empty()) {
        return nullptr;
    }
    for (const auto& m : macros) {
        if (m.display_name == name) {
            return &m;
        }
    }
    return nullptr;
}

/// Index of the synthetic "Custom..." row appended to every macro dropdown in
/// the settings editor. It sits after the detected macros.
inline int macro_custom_index(const std::vector<std::string>& discovered) {
    return static_cast<int>(discovered.size());
}

/// Resolve one macro field of the settings editor into the gcode to store.
///
/// Typed text always wins. Keyword detection is deliberately fuzzy and will
/// never cover every naming scheme, so without a free-text path a macro it
/// misses is unreachable from the UI entirely.
inline std::string resolve_macro_field(const std::string& typed, int dropdown_index,
                                       const std::vector<std::string>& discovered) {
    const size_t first = typed.find_first_not_of(" \t\n\r");
    if (first != std::string::npos) {
        const size_t last = typed.find_last_not_of(" \t\n\r");
        return typed.substr(first, last - first + 1);
    }
    if (dropdown_index >= 0 && dropdown_index < static_cast<int>(discovered.size())) {
        return discovered[dropdown_index];
    }
    return "";
}

/// How an already-stored macro should be presented in the editor.
struct MacroFieldView {
    int dropdown_index; ///< row to preselect
    std::string typed;  ///< text to prefill (empty when the dropdown covers it)
};

/// Decide the editor state for a macro already saved on a device. A value the
/// detector no longer offers lands in the text box rather than snapping the
/// dropdown to row 0 -- doing that silently rewrote the field to an unrelated
/// macro the next time the user pressed Save.
inline MacroFieldView macro_field_view(const std::string& stored,
                                       const std::vector<std::string>& discovered) {
    if (stored.empty()) {
        return {0, ""};
    }
    for (size_t i = 0; i < discovered.size(); ++i) {
        if (discovered[i] == stored) {
            return {static_cast<int>(i), ""};
        }
    }
    return {macro_custom_index(discovered), stored};
}

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
