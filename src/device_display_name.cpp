// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "device_display_name.h"

#include "core/text_io.h"
#include "hardware_object_table.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace helix {

namespace {

// Hash function for pair<string, DeviceType> to use in unordered_map
struct PairHash {
    size_t operator()(const std::pair<std::string, DeviceType>& p) const {
        return std::hash<std::string>()(p.first) ^ (static_cast<size_t>(p.second) << 8);
    }
};

// Direct mappings for well-known Klipper object names
// These take priority over all heuristics
const std::unordered_map<std::pair<std::string, DeviceType>, std::string, PairHash>
    kDirectMappings = {
        // Fans
        {{"fan", DeviceType::FAN}, "Part Cooling Fan"},

        // Heaters
        {{"extruder", DeviceType::HEATER}, "Hotend Heater"},
        {{"heater_bed", DeviceType::HEATER}, "Bed Heater"},

        // Temperature sensors (aliases for heaters when queried as sensors)
        {{"extruder", DeviceType::TEMP_SENSOR}, "Hotend Temperature"},
        {{"heater_bed", DeviceType::TEMP_SENSOR}, "Bed Temperature"},
};

// Type suffixes to add (if not already present in the name)
const std::unordered_map<DeviceType, std::string> kTypeSuffixes = {
    {DeviceType::FAN, "Fan"},
    {DeviceType::HEATER, "Heater"},
    {DeviceType::TEMP_SENSOR, "Temperature"},
    {DeviceType::LED, "LED"},
    {DeviceType::LOAD_CELL, "Load Cell"},
    {DeviceType::FILAMENT_SENSOR, "Sensor"},
};

// Words that indicate the type is already present (case-insensitive)
// If any of these appear in the prettified name, don't add suffix
const std::unordered_map<DeviceType, std::unordered_set<std::string>> kSuffixSkipWords = {
    {DeviceType::FAN, {"fan", "fans", "cooling", "exhaust", "blower"}},
    {DeviceType::HEATER, {"heater", "heat", "heating"}},
    {DeviceType::TEMP_SENSOR, {"temp", "temperature", "thermistor"}},
    {DeviceType::LED, {"led", "leds", "light", "lights", "strip", "neopixel"}},
    {DeviceType::LOAD_CELL, {"load cell", "weight"}},
    {DeviceType::FILAMENT_SENSOR, {"sensor", "detector"}},
};

// Special word replacements (case-insensitive input -> exact output)
const std::unordered_map<std::string, std::string> kSpecialWords = {
    {"led", "LED"},
    {"leds", "LEDs"},
    {"psu", "PSU"},
    {"usb", "USB"},
    {"ac", "AC"},
    {"dc", "DC"},
    {"io", "I/O"},
    {"gpio", "GPIO"},
    {"aux", "Auxiliary"},
    {"temp", "Temperature"},
    {"ctrl", "Control"},
    {"pwr", "Power"},
    {"enc", "Enclosure"},
    {"cam", "Camera"},
    {"sw", "Switch"},
    {"btn", "Button"},
    {"htr", "Heater"},
    {"relay", "Relay"},
    {"mcu", "MCU"},
    {"cpu", "CPU"},
    {"afc", "AFC"},
    {"ercf", "ERCF"},
    {"ams", "AMS"},
    {"mmu", "MMU"},
    {"btt", "BTT"},
    {"tmc", "TMC"},
    {"rgb", "RGB"},
    {"rgbw", "RGBW"},
    {"neopixel", "NeoPixel"},
};

// Check if the prettified name contains any of the skip words for this type
bool contains_type_word(const std::string& pretty_name, DeviceType type) {
    auto it = kSuffixSkipWords.find(type);
    if (it == kSuffixSkipWords.end()) {
        return false;
    }

    std::string lower_name = helix::text_io::to_lower(pretty_name);

    for (const auto& skip_word : it->second) {
        // Check if the skip word appears as a whole word in the name
        size_t pos = lower_name.find(skip_word);
        while (pos != std::string::npos) {
            // Check word boundaries
            bool at_start = (pos == 0);
            bool at_end = (pos + skip_word.length() == lower_name.length());
            bool preceded_by_space = (pos > 0 && lower_name[pos - 1] == ' ');
            bool followed_by_space = (pos + skip_word.length() < lower_name.length() &&
                                      lower_name[pos + skip_word.length()] == ' ');

            if ((at_start || preceded_by_space) && (at_end || followed_by_space)) {
                return true;
            }
            pos = lower_name.find(skip_word, pos + 1);
        }
    }
    return false;
}

} // namespace

std::string extract_device_suffix(const std::string& object_name) {
    if (object_name.empty()) {
        return object_name;
    }

    // "<type> <instance>": the table knows every type prefix, so what follows the
    // space is the instance exactly when the table says so.
    const size_t space_pos = object_name.find(' ');
    if (space_pos != std::string::npos && space_pos + 1 < object_name.length()) {
        const std::string tail = object_name.substr(space_pos + 1);
        if (hw::classify_object(object_name).instance == tail) {
            return tail;
        }
    }
    return object_name;
}

std::string prettify_name(const std::string& snake_case_name) {
    if (snake_case_name.empty()) {
        return snake_case_name;
    }

    std::string result;
    std::string current_word;

    auto flush_word = [&]() {
        if (current_word.empty()) {
            return;
        }

        // Check for special word replacement
        std::string lower_word = helix::text_io::to_lower(current_word);
        auto it = kSpecialWords.find(lower_word);

        if (it != kSpecialWords.end()) {
            current_word = it->second;
        } else if (current_word.size() >= 2 &&
                   std::all_of(current_word.begin(), current_word.end(), [](unsigned char c) {
                       return std::isupper(c) || std::isdigit(c);
                   })) {
            // Preserve all-uppercase words as acronyms (EBB, NTC, BTT, SB2240, etc.)
        } else {
            // Capitalize first letter, lowercase rest
            current_word[0] =
                static_cast<char>(std::toupper(static_cast<unsigned char>(current_word[0])));
            for (size_t i = 1; i < current_word.size(); ++i) {
                current_word[i] =
                    static_cast<char>(std::tolower(static_cast<unsigned char>(current_word[i])));
            }
        }

        if (!result.empty()) {
            result += ' ';
        }
        result += current_word;
        current_word.clear();
    };

    // Skip leading underscore (common in macros)
    size_t start = (snake_case_name[0] == '_') ? 1 : 0;

    // Skip HELIX_ prefix for cleaner macro display
    if (snake_case_name.rfind("HELIX_", 0) == 0) {
        start = 6;
    }

    for (size_t i = start; i < snake_case_name.size(); ++i) {
        char c = snake_case_name[i];
        if (c == '_' || c == '-') {
            flush_word();
        } else {
            current_word += c;
        }
    }
    flush_word();

    return result.empty() ? snake_case_name : result;
}

std::string get_display_name(const std::string& technical_name, DeviceType type) {
    if (technical_name.empty()) {
        return technical_name;
    }

    // 1. Check direct mappings first (highest priority)
    auto direct_it = kDirectMappings.find({technical_name, type});
    if (direct_it != kDirectMappings.end()) {
        return direct_it->second;
    }

    // 2. Extract suffix (strip type prefix like "heater_fan " or "neopixel ")
    std::string suffix = extract_device_suffix(technical_name);

    // 3. Check direct mappings again with just the suffix
    direct_it = kDirectMappings.find({suffix, type});
    if (direct_it != kDirectMappings.end()) {
        return direct_it->second;
    }

    // 4. Prettify the name (snake_case -> Title Case with special words)
    std::string pretty = prettify_name(suffix);

    // 5. Add type suffix if appropriate
    auto suffix_it = kTypeSuffixes.find(type);
    if (suffix_it != kTypeSuffixes.end() && !contains_type_word(pretty, type)) {
        pretty += ' ';
        pretty += suffix_it->second;
    }

    return pretty;
}

} // namespace helix
