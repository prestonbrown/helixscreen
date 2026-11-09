// SPDX-License-Identifier: GPL-3.0-or-later

#include "fan_tachometers.h"

#include "text_io.h"

#include <cmath>
#include <string_view>

namespace helix::fan_tach {
namespace {

// Creality's fan_feedback module (K1 and K2 series klippy/extras/fan_feedback.py)
// publishes fan0_speed..fan4_speed. Its fanN_pin options are tach inputs, so the
// fan each slot measures comes from Creality's own config, not from pin matching.
constexpr std::string_view kCrealityObject = "fan_feedback";

enum class Role { None, Part, Hotend, ChamberHeater };

// Lowercased, whitespace and pin-modifier prefixes stripped: "nozzle_mcu: PB4" and
// "^nozzle_mcu:pb4" both read "nozzle_mcu:pb4".
std::string normalize_pin(std::string_view raw) {
    std::string out;
    for (char c : raw) {
        if (c == ' ' || c == '\t' || ((c == '^' || c == '~' || c == '!') && out.empty()))
            continue;
        out.push_back(c);
    }
    return text_io::to_lower(out);
}

// configfile.settings holds a string for config.get() and an array for getlist().
std::vector<std::string> string_list(const nlohmann::json& v) {
    std::vector<std::string> out;
    if (v.is_string()) {
        out.push_back(v.get<std::string>());
    } else if (v.is_array()) {
        for (const auto& e : v)
            if (e.is_string())
                out.push_back(e.get<std::string>());
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

// The heater_fan section whose heater list names a heater accepted by `wanted`.
template <typename Pred> std::string heater_fan_for(const nlohmann::json& settings, Pred wanted) {
    for (const auto& [section, vals] : settings.items()) {
        if (!starts_with(section, "heater_fan ") || !vals.is_object())
            continue;
        auto it = vals.find("heater");
        // Klipper's heater_fan defaults to the extruder.
        std::vector<std::string> heaters =
            it == vals.end() ? std::vector<std::string>{"extruder"} : string_list(*it);
        for (const auto& h : heaters)
            if (wanted(text_io::to_lower(h)))
                return section;
    }
    return {};
}

std::string resolve_fan(const nlohmann::json& settings, Role role) {
    switch (role) {
    case Role::Part:
        // M106 drives output_pin fan0 on Creality's stock macros; [fan] elsewhere.
        if (settings.contains("output_pin fan0"))
            return "output_pin fan0";
        if (settings.contains("fan"))
            return "fan";
        return {};
    case Role::Hotend:
        return heater_fan_for(settings, [](const std::string& h) { return h == "extruder"; });
    case Role::ChamberHeater:
        return heater_fan_for(settings, [&](const std::string& h) {
            return settings.contains("heater_generic " + h);
        });
    case Role::None:
        break;
    }
    return {};
}

// K2 series: one oid per MCU, fixed slots. printer_params.cfg documents
// fan0_pin "PTC Fan" (the chamber heater's fan; a list when it has several),
// fan1_pin the throat (hotend heatsink) fan and fan2_pin the model (part) fan.
// fan3_speed and fan4_speed are never written.
TachMap map_k2_series(const nlohmann::json& settings) {
    const std::pair<const char*, Role> slots[] = {
        {"fan0_speed", Role::ChamberHeater},
        {"fan1_speed", Role::Hotend},
        {"fan2_speed", Role::Part},
    };
    TachMap out;
    for (const auto& [field, role] : slots) {
        std::string fan = resolve_fan(settings, role);
        if (!fan.empty())
            out.push_back({std::string(kCrealityObject), field, std::move(fan)});
    }
    return out;
}

// K1 series: one oid per configured fanN_pin, and only the first two configured
// pins are reported, as fan0_speed and fan1_speed. printer_params.cfg documents
// nozzle_mcu:PB4 as the throat fan and nozzle_mcu:PB3 as the model fan; PC6, the
// backplane fan, has no Klipper fan object and stays unmapped.
Role k1_role_for_pin(const std::string& pin) {
    if (pin == "nozzle_mcu:pb4")
        return Role::Hotend;
    if (pin == "nozzle_mcu:pb3")
        return Role::Part;
    return Role::None;
}

TachMap map_k1_series(const nlohmann::json& fb, const nlohmann::json& settings) {
    TachMap out;
    int slot = 0;
    for (int i = 0; i < 5 && slot < 2; ++i) {
        auto it = fb.find("fan" + std::to_string(i) + "_pin");
        if (it == fb.end())
            continue;
        const auto pins = string_list(*it);
        if (pins.empty())
            continue;
        const std::string field = "fan" + std::to_string(slot) + "_speed";
        ++slot;
        std::string fan = resolve_fan(settings, k1_role_for_pin(normalize_pin(pins.front())));
        if (!fan.empty())
            out.push_back({std::string(kCrealityObject), field, std::move(fan)});
    }
    return out;
}

} // namespace

TachMap map_from_config(const nlohmann::json& settings) {
    if (!settings.is_object())
        return {};
    auto it = settings.find(kCrealityObject);
    if (it == settings.end() || !it->is_object())
        return {};
    // The K2 module requires fan1_pin and fan2_pin; the K1 module reads only the
    // pins configured, and no K1 config sets fan2_pin.
    const auto fan2 = it->find("fan2_pin");
    if (fan2 != it->end() && !string_list(*fan2).empty())
        return map_k2_series(settings);
    return map_k1_series(*it, settings);
}

std::vector<std::pair<std::string, int>> readings(const TachMap& map,
                                                  const nlohmann::json& status) {
    std::vector<std::pair<std::string, int>> out;
    if (!status.is_object())
        return out;
    for (const auto& src : map) {
        auto obj = status.find(src.object);
        if (obj == status.end() || !obj->is_object())
            continue;
        auto v = obj->find(src.field);
        if (v == obj->end() || !v->is_number())
            continue;
        out.emplace_back(src.fan, static_cast<int>(std::lround(v->get<double>())));
    }
    return out;
}

} // namespace helix::fan_tach
