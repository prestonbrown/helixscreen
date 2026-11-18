// SPDX-License-Identifier: GPL-3.0-or-later

#include "fan_heaters.h"

#include "core/text_io.h"

#include <string_view>

namespace helix::fan_heaters {

std::vector<std::string> config_list(const nlohmann::json& value) {
    const auto blank = [](std::string_view s) {
        return s.find_first_not_of(" \t") == std::string_view::npos;
    };
    std::vector<std::string> out;
    if (value.is_string()) {
        if (!blank(value.get<std::string>()))
            out.push_back(value.get<std::string>());
    } else if (value.is_array()) {
        for (const auto& e : value)
            if (e.is_string() && !blank(e.get<std::string>()))
                out.push_back(e.get<std::string>());
    }
    return out;
}

HeaterFanMap map_from_config(const nlohmann::json& settings) {
    HeaterFanMap out;
    if (!settings.is_object())
        return out;
    constexpr std::string_view kPrefix = "heater_fan ";
    for (const auto& [section, vals] : settings.items()) {
        if (std::string_view(section).substr(0, kPrefix.size()) != kPrefix || !vals.is_object())
            continue;
        HeaterFan fan;
        fan.section = section;
        auto it = vals.find("heater");
        fan.heaters = it == vals.end() ? std::vector<std::string>{"extruder"} : config_list(*it);
        for (auto& h : fan.heaters)
            h = text_io::to_lower(h);
        out.push_back(std::move(fan));
    }
    return out;
}

} // namespace helix::fan_heaters
