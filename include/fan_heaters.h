// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which heaters drive each [heater_fan], read from Klipper's configfile.settings.
//
// A heater_fan's status object reports only its speed, never the heater it follows,
// so the config is the one place that ties a hotend fan to its extruder. On a
// toolchanger every tool's hotend fan names its own extruder.

#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix::fan_heaters {

/// One [heater_fan] section, as Klipper keys it in configfile.settings (lowercased),
/// and the heaters that switch it on, lowercased.
struct HeaterFan {
    std::string section;
    std::vector<std::string> heaters;

    bool operator==(const HeaterFan& o) const {
        return section == o.section && heaters == o.heaters;
    }
};

using HeaterFanMap = std::vector<HeaterFan>;

/// configfile.settings holds a string for config.get() and an array for getlist().
/// Blank entries are dropped.
std::vector<std::string> config_list(const nlohmann::json& value);

/// Every [heater_fan] section in configfile.settings with its heaters. A section
/// without a `heater` option follows the extruder, as Klipper defaults it.
HeaterFanMap map_from_config(const nlohmann::json& settings);

/// The one section whose heaters include one @p wanted accepts; empty when none
/// does, or several do, since a fan picked from several would be a guess.
template <typename Pred> std::string only_fan_for(const HeaterFanMap& map, Pred wanted) {
    std::string found;
    for (const auto& fan : map) {
        bool match = false;
        for (const auto& heater : fan.heaters) {
            match = match || wanted(heater);
        }
        if (!match)
            continue;
        if (!found.empty())
            return {};
        found = fan.section;
    }
    return found;
}

} // namespace helix::fan_heaters
